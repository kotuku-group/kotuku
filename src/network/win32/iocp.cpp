#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#define IS ==

#include "../socket_errors.h"
#include "iocp.h"

struct IocpOperation {
   OVERLAPPED Overlapped = {}; // Native overlapped state submitted to Winsock.
   IocpOperationType Type = IocpOperationType::CONNECT; // Operation kind used when processing completion.
   WSW_SOCKET Socket = 0;     // Socket that owns this operation.
   uint64_t Generation = 0;   // Socket generation expected when the completion returns.
   int ObjectID = 0;          // Fallback object UID captured when the operation was posted.
   uintptr_t Callback = 0;    // Fallback callback captured when the operation was posted.
   WSW_SOCKET AcceptedSocket = 0; // Socket returned by AcceptEx operations.
   std::unique_ptr<uint8_t[]> Buffer; // Backend-owned operation buffer.
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> Address = {}; // Endpoint storage for UDP or accept results.
   int AddressSize = 0;          // Size of the endpoint stored in Address.
   uint64_t Sequence = 0;        // Ordered sequence number for TCP read operations.
   size_t BufferSize = 0;        // Number of bytes available in Buffer.
   size_t SendAccountedSize = 0; // Original send size charged against the socket send limit.
   size_t BytesTransferred = 0;  // Number of bytes reported by the completion port.
   ERR Result = ERR::Busy;       // Result assigned before the completion is queued to the main thread.
};

struct IocpCompletionTarget {
   int ObjectID = 0;       // UID of the object that should receive the completion.
   uintptr_t Callback = 0; // Callback function to invoke for the completion.
};

struct IocpAcceptedSocket {
   WSW_SOCKET Socket = 0; // Accepted client socket handle.
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> Address = {}; // Remote endpoint captured from AcceptEx.
   int AddressSize = 0;   // Size of the accepted remote endpoint.
};

struct IocpDatagram {
   std::vector<uint8_t> Buffer; // Datagram payload waiting for the caller.
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> Address = {}; // Source endpoint for the datagram.
   int AddressSize = 0;        // Size of the stored source endpoint.
   ERR Result = ERR::Okay;     // Receive result associated with this datagram.
};

struct IocpSendRequest {
   std::unique_ptr<uint8_t[]> Buffer; // Backend-owned TCP payload waiting for WSASend.
   size_t BufferSize = 0;             // Number of payload bytes in Buffer.
};

struct IocpReadCompletion {
   std::vector<uint8_t> Buffer; // Completed TCP bytes waiting for ordered delivery.
   ERR Result = ERR::Okay;      // Read result for this sequence.
};

struct IocpStoreResult {
   bool Notify = true;       // True when a main-thread completion should be posted.
   bool RearmAccept = false; // True when the accept pool should be replenished.
   bool PostRead = false;    // True when additional TCP reads should be posted.
   bool PostSend = false;    // True when queued TCP sends should be posted.
};

struct IocpSocketRecord {
   mutable std::mutex Mutex;     // Protects all mutable per-socket state in this record.
   int ObjectID = 0;             // UID of the object currently associated with this socket.
   IocpCompletionTarget Connect; // Target to notify when a connect operation completes.
   IocpCompletionTarget Read;    // Target to notify when buffered read data is available.
   IocpCompletionTarget Write;   // Target to notify when write capacity becomes available.
   IocpCompletionTarget Accept;  // Target to notify when accepted sockets are ready.
   std::deque<IocpAcceptedSocket> AcceptedSockets; // FIFO queue of completed AcceptEx sockets.
   std::deque<IocpSendRequest> SendQueue; // Backend-owned TCP send buffers waiting to be posted.
   std::vector<IocpDatagram> Datagrams;   // Completed UDP datagrams waiting for acRead or mtRecvFrom.
   std::vector<uint8_t> ReadBuffer;       // Ordered TCP bytes buffered for acRead.
   std::map<uint64_t, IocpReadCompletion> ReadCompletions; // Out-of-order TCP read completions by sequence.
   size_t BufferedReadBytes = 0;   // Number of unread bytes currently held in ReadBuffer.
   size_t ReadOffset = 0;          // Offset of the next unread byte in ReadBuffer.
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> ConnectAddress = {}; // Remote endpoint for pending connect.
   int ConnectAddressSize = 0;     // Size of the stored connect endpoint.
   ERR ConnectResult = ERR::NotInitialised; // Result reported for the latest connect operation.
   ERR ReadResult = ERR::Okay;     // Terminal read state once the peer disconnects or an error occurs.
   uint64_t Generation = 0;        // Monotonic token used to reject stale completions.
   uint64_t NextReadSequence = 0;  // Sequence number assigned to the next posted TCP read.
   uint64_t NextReadCompletionSequence = 0; // Next TCP read sequence eligible for ordered delivery.
   int AcceptDepth = 16;           // Maximum number of AcceptEx operations kept in flight.
   size_t AcceptPendingCount = 0;  // Number of AcceptEx operations currently pending.
   size_t ReadPendingCount = 0;    // Number of TCP WSARecv operations currently pending.
   size_t SendPendingCount = 0;    // Number of TCP WSASend operations currently pending.
   size_t SendPendingBytes = 0;    // Accepted TCP send bytes still queued or in flight.
   bool IPv6 = false;              // True when the socket was created for IPv6 or dual-stack use.
   bool UDP = false;               // True when this record represents a UDP socket.
   bool Cancelled = false;         // Set once shutdown or deregistration has invalidated the record.
   bool UdpReadPending = false;    // True while a UDP receive operation is posted.
};

using IocpSocketRecordPtr = std::shared_ptr<IocpSocketRecord>;

static HANDLE glCompletionPort = INVALID_HANDLE_VALUE;
static std::vector<std::jthread> glWorkers;
static std::mutex glRegistryMutex;
static std::unordered_map<WSW_SOCKET, IocpSocketRecordPtr> glSockets;
static std::atomic_bool glShutdownRequested = false;
static std::atomic_size_t glPendingOperations = 0;
static std::atomic_uint64_t glNextGeneration = 1;
static int glCompletionMsgID = 0;
static iocp_post_message glPostMessage = nullptr;
static bool glWinsockInitialised = false;

static constexpr ULONG_PTR IOCP_SHUTDOWN_KEY = ULONG_PTR(~uintptr_t(0));
static constexpr size_t IOCP_READ_BUFFER_SIZE = 65536;
static constexpr size_t IOCP_TCP_READ_HIGH_WATER = 1024 * 1024;
static constexpr size_t IOCP_TCP_READ_DEPTH = 4;
static constexpr size_t IOCP_TCP_SEND_HIGH_WATER = 1024 * 1024;
static constexpr size_t IOCP_TCP_SEND_DEPTH = 8;
static constexpr size_t IOCP_UDP_BUFFER_SIZE = 65536;
static constexpr size_t IOCP_ACCEPT_ADDRESS_SIZE = sizeof(sockaddr_storage) + 16;
static constexpr size_t IOCP_ACCEPT_BUFFER_SIZE = IOCP_ACCEPT_ADDRESS_SIZE * 2;
static constexpr int IOCP_DEFAULT_ACCEPT_DEPTH = 16;
static constexpr int IOCP_MIN_ACCEPT_DEPTH = 4;
static constexpr int IOCP_MAX_ACCEPT_DEPTH = 128;

//********************************************************************************************************************

static ERR post_accept(WSW_SOCKET Socket);
static ERR post_accept_pool(WSW_SOCKET Socket);
static ERR post_read_pool(WSW_SOCKET Socket);
static ERR post_send_pool(WSW_SOCKET Socket);

//********************************************************************************************************************

static IocpOperation *create_operation()
{
   glPendingOperations.fetch_add(1);
   return new IocpOperation();
}

//********************************************************************************************************************

static void release_operation(IocpOperation *Operation)
{
   delete Operation;
   glPendingOperations.fetch_sub(1);
   glPendingOperations.notify_all();
}

//********************************************************************************************************************

static IocpSocketRecordPtr find_socket_record(WSW_SOCKET Socket)
{
   std::lock_guard<std::mutex> lock(glRegistryMutex);
   auto record = glSockets.find(Socket);
   if (record IS glSockets.end()) return nullptr;
   return record->second;
}

//********************************************************************************************************************

static IocpSocketRecordPtr create_socket_record()
{
   return std::make_shared<IocpSocketRecord>();
}

//********************************************************************************************************************

static IocpCompletionTarget completion_target(const IocpSocketRecord &Record, IocpOperationType Type)
{
   if (Type IS IocpOperationType::CONNECT) return Record.Connect;
   else if (Type IS IocpOperationType::READ) return Record.Read;
   else if (Type IS IocpOperationType::WRITE) return Record.Write;
   else if (Type IS IocpOperationType::ACCEPT) return Record.Accept;
   else if (Type IS IocpOperationType::UDP_RECEIVE) return Record.Read;
   else return {};
}

//********************************************************************************************************************

static ERR convert_error(int Error = 0)
{
   if (Error IS ERROR_NETNAME_DELETED) return ERR::Disconnected;
   if (Error IS ERROR_OPERATION_ABORTED) return ERR::Cancelled;
   if (Error IS ERROR_CONNECTION_ABORTED) return ERR::ConnectionAborted;
   if (Error IS ERROR_SEM_TIMEOUT) return ERR::TimeOut;

   return convert_socket_error(Error);
}

//********************************************************************************************************************

static ERR convert_send_to_error(int Error)
{
   switch (Error) {
      case WSAEWOULDBLOCK:
      case WSAEALREADY:
         return ERR::BufferOverflow;
      case WSAEINPROGRESS:
         return ERR::Busy;
      case WSAENETUNREACH:
         return ERR::NetworkUnreachable;
      case WSAEINVAL:
         return ERR::Args;
      default:
         return convert_error(Error);
   }
}

//********************************************************************************************************************

static int accept_depth_from_backlog(int Backlog)
{
   if (Backlog <= 0) return IOCP_DEFAULT_ACCEPT_DEPTH;
   return std::clamp(Backlog, IOCP_MIN_ACCEPT_DEPTH, IOCP_MAX_ACCEPT_DEPTH);
}

//********************************************************************************************************************

static SOCKET socket_from_handle(WSW_SOCKET Socket)
{
   return SOCKET(uintptr_t(Socket));
}

//********************************************************************************************************************

static WSW_SOCKET handle_from_socket(SOCKET Socket)
{
   return WSW_SOCKET(uintptr_t(Socket));
}

//********************************************************************************************************************

static LPFN_CONNECTEX get_connect_ex(SOCKET Socket)
{
   LPFN_CONNECTEX connect_ex = nullptr;
   GUID guid = WSAID_CONNECTEX;
   DWORD bytes = 0;

   auto result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &connect_ex,
      sizeof(connect_ex), &bytes, nullptr, nullptr);
   if (result IS SOCKET_ERROR) return nullptr;
   return connect_ex;
}

//********************************************************************************************************************

static LPFN_ACCEPTEX get_accept_ex(SOCKET Socket)
{
   LPFN_ACCEPTEX accept_ex = nullptr;
   GUID guid = WSAID_ACCEPTEX;
   DWORD bytes = 0;

   auto result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &accept_ex,
      sizeof(accept_ex), &bytes, nullptr, nullptr);
   if (result IS SOCKET_ERROR) return nullptr;
   return accept_ex;
}

//********************************************************************************************************************

static LPFN_GETACCEPTEXSOCKADDRS get_accept_ex_sockaddrs(SOCKET Socket)
{
   LPFN_GETACCEPTEXSOCKADDRS get_addresses = nullptr;
   GUID guid = WSAID_GETACCEPTEXSOCKADDRS;
   DWORD bytes = 0;

   auto result = WSAIoctl(Socket, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &get_addresses,
      sizeof(get_addresses), &bytes, nullptr, nullptr);
   if (result IS SOCKET_ERROR) return nullptr;
   return get_addresses;
}

//********************************************************************************************************************

static SOCKET create_tcp_socket(bool IPv6, bool &ActualIPv6)
{
   SOCKET socket = INVALID_SOCKET;

   if (IPv6) {
      socket = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
      ActualIPv6 = socket != INVALID_SOCKET;
      if (socket != INVALID_SOCKET) {
         DWORD v6only = 0;
         setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only, sizeof(v6only));
      }
   }

   if (socket IS INVALID_SOCKET) {
      socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
      ActualIPv6 = false;
   }

   if (socket != INVALID_SOCKET) {
      DWORD nodelay = 1;
      setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));
   }

   return socket;
}

//********************************************************************************************************************

static SOCKET create_udp_socket(bool IPv6, bool &ActualIPv6)
{
   SOCKET socket = INVALID_SOCKET;

   if (IPv6) {
      socket = WSASocket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
      ActualIPv6 = socket != INVALID_SOCKET;
      if (socket != INVALID_SOCKET) {
         DWORD v6only = 0;
         setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only, sizeof(v6only));
      }
   }

   if (socket IS INVALID_SOCKET) {
      socket = WSASocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_OVERLAPPED);
      ActualIPv6 = false;
   }

   return socket;
}

//********************************************************************************************************************

static ERR bind_udp_ephemeral(SOCKET Socket, bool IPv6)
{
   if (IPv6) {
      sockaddr_in6 local_address = {};
      local_address.sin6_family = AF_INET6;
      local_address.sin6_addr = in6addr_any;
      local_address.sin6_port = 0;
      if (bind(Socket, (sockaddr *)&local_address, sizeof(local_address)) IS SOCKET_ERROR) {
         auto error = WSAGetLastError();
         if (error != WSAEINVAL) return convert_error(error);
      }
   }
   else {
      sockaddr_in local_address = {};
      local_address.sin_family = AF_INET;
      local_address.sin_addr.s_addr = INADDR_ANY;
      local_address.sin_port = 0;
      if (bind(Socket, (sockaddr *)&local_address, sizeof(local_address)) IS SOCKET_ERROR) {
         auto error = WSAGetLastError();
         if (error != WSAEINVAL) return convert_error(error);
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void close_accepted_socket(const IocpOperation &Operation)
{
   if ((Operation.Type IS IocpOperationType::ACCEPT) and (Operation.AcceptedSocket != WSW_SOCKET(INVALID_SOCKET))) {
      closesocket(socket_from_handle(Operation.AcceptedSocket));
   }
}

//********************************************************************************************************************

static bool tcp_read_post_needed(const IocpSocketRecord &Record)
{
   if (Record.UDP) return false;
   if (Record.Cancelled) return false;
   if (Record.ReadResult != ERR::Okay) return false;
   if (!Record.ReadCompletions.empty()) return false;
   if (Record.BufferedReadBytes >= IOCP_TCP_READ_HIGH_WATER) return false;
   if (Record.ReadPendingCount >= IOCP_TCP_READ_DEPTH) return false;
   if ((!Record.Read.Callback) or (Record.Read.ObjectID <= 0)) return false;
   return true;
}

//********************************************************************************************************************

static bool tcp_send_post_needed(const IocpSocketRecord &Record)
{
   if (Record.UDP) return false;
   if (Record.Cancelled) return false;
   if (Record.SendQueue.empty()) return false;
   if (Record.SendPendingCount >= IOCP_TCP_SEND_DEPTH) return false;
   return true;
}

//********************************************************************************************************************

static bool tcp_send_capacity_available(const IocpSocketRecord &Record)
{
   if (Record.UDP) return false;
   if (Record.Cancelled) return false;
   return Record.SendPendingBytes < IOCP_TCP_SEND_HIGH_WATER;
}

//********************************************************************************************************************

static size_t append_ordered_tcp_reads(IocpSocketRecord &Record)
{
   size_t appended = 0;

   while (Record.ReadResult IS ERR::Okay) {
      auto completion = Record.ReadCompletions.find(Record.NextReadCompletionSequence);
      if (completion IS Record.ReadCompletions.end()) break;

      auto read = std::move(completion->second);
      Record.ReadCompletions.erase(completion);
      Record.NextReadCompletionSequence++;

      if (read.Result != ERR::Okay) {
         Record.ReadResult = read.Result;
         Record.ReadCompletions.clear();
         break;
      }

      if (read.Buffer.empty()) {
         Record.ReadResult = ERR::Disconnected;
         Record.ReadCompletions.clear();
         break;
      }

      auto offset = Record.ReadBuffer.size();
      Record.ReadBuffer.resize(offset + read.Buffer.size());
      std::memcpy(Record.ReadBuffer.data() + offset, read.Buffer.data(), read.Buffer.size());
      Record.BufferedReadBytes += read.Buffer.size();
      appended += read.Buffer.size();
   }

   return appended;
}

//********************************************************************************************************************

static ERR bind_ephemeral(SOCKET Socket, const sockaddr *RemoteAddress)
{
   if (!RemoteAddress) return ERR::NullArgs;

   if (RemoteAddress->sa_family IS AF_INET6) {
      sockaddr_in6 local_address = {};
      local_address.sin6_family = AF_INET6;
      local_address.sin6_addr = in6addr_any;
      local_address.sin6_port = 0;
      if (bind(Socket, (sockaddr *)&local_address, sizeof(local_address)) IS SOCKET_ERROR) return convert_error();
   }
   else if (RemoteAddress->sa_family IS AF_INET) {
      sockaddr_in local_address = {};
      local_address.sin_family = AF_INET;
      local_address.sin_addr.s_addr = INADDR_ANY;
      local_address.sin_port = 0;
      if (bind(Socket, (sockaddr *)&local_address, sizeof(local_address)) IS SOCKET_ERROR) return convert_error();
   }
   else return ERR::InvalidData;

   return ERR::Okay;
}

//********************************************************************************************************************

static IocpStoreResult store_operation_result(IocpSocketRecord &Record, const IocpOperation &Operation, ERR Error)
{
   IocpStoreResult result;

   if (Operation.Type IS IocpOperationType::CONNECT) Record.ConnectResult = Error;
   else if (Operation.Type IS IocpOperationType::READ) {
      result.Notify = false;
      if (Record.ReadPendingCount > 0) Record.ReadPendingCount--;

      if ((Record.ReadResult IS ERR::Okay) and (Operation.Sequence >= Record.NextReadCompletionSequence)) {
         IocpReadCompletion completion;
         completion.Result = Error;

         if ((Error IS ERR::Okay) and (Operation.BytesTransferred > 0)) {
            if (Operation.Buffer) {
               completion.Buffer.resize(Operation.BytesTransferred);
               std::memcpy(completion.Buffer.data(), Operation.Buffer.get(), Operation.BytesTransferred);
            }
            else completion.Result = ERR::Failed;
         }

         if (Record.ReadCompletions.emplace(Operation.Sequence, std::move(completion)).second) {
            auto prior_result = Record.ReadResult;
            auto appended = append_ordered_tcp_reads(Record);
            result.Notify = (appended > 0) or ((prior_result IS ERR::Okay) and (Record.ReadResult != ERR::Okay));
         }
      }

      result.PostRead = tcp_read_post_needed(Record);
   }
   else if (Operation.Type IS IocpOperationType::WRITE) {
      if (Record.SendPendingCount > 0) Record.SendPendingCount--;

      auto accounted_size = Operation.SendAccountedSize ? Operation.SendAccountedSize : Operation.BufferSize;
      if (Record.SendPendingBytes >= accounted_size) Record.SendPendingBytes -= accounted_size;
      else Record.SendPendingBytes = 0;

      result.PostSend = tcp_send_post_needed(Record);
      result.Notify = tcp_send_capacity_available(Record);
   }
   else if (Operation.Type IS IocpOperationType::UDP_SEND) {
      // The completion only confirms that backend-owned datagram storage can be released.
   }
   else if (Operation.Type IS IocpOperationType::UDP_RECEIVE) {
      Record.UdpReadPending = false;
      Record.ReadResult = Error;

      if ((Error IS ERR::Okay) and (Operation.Buffer) and
          (Operation.AddressSize > 0) and (Operation.AddressSize <= int(IOCP_ENDPOINT_STORAGE_SIZE))) {
         IocpDatagram datagram;
         datagram.Buffer.resize(Operation.BytesTransferred);
         if (Operation.BytesTransferred > 0) {
            std::memcpy(datagram.Buffer.data(), Operation.Buffer.get(), Operation.BytesTransferred);
         }
         datagram.AddressSize = Operation.AddressSize;
         datagram.Result = ERR::Okay;
         std::memcpy(datagram.Address.data(), Operation.Address.data(), size_t(Operation.AddressSize));
         Record.Datagrams.push_back(std::move(datagram));
      }
   }
   else if (Operation.Type IS IocpOperationType::ACCEPT) {
      if (Record.AcceptPendingCount > 0) Record.AcceptPendingCount--;

      if (Error != ERR::Okay) {
         close_accepted_socket(Operation);
         result.RearmAccept = true;
         return result;
      }

      auto server_socket = socket_from_handle(Operation.Socket);
      auto accepted_socket = socket_from_handle(Operation.AcceptedSocket);

      if (setsockopt(accepted_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&server_socket,
          sizeof(server_socket)) IS SOCKET_ERROR) {
         close_accepted_socket(Operation);
         result.RearmAccept = true;
         return result;
      }

      auto handle = CreateIoCompletionPort((HANDLE)accepted_socket, glCompletionPort, ULONG_PTR(accepted_socket), 0);
      if (handle IS nullptr) {
         close_accepted_socket(Operation);
         result.RearmAccept = true;
         return result;
      }

      sockaddr *local_address = nullptr;
      sockaddr *remote_address = nullptr;
      int local_size = 0;
      int remote_size = 0;
      auto get_addresses = get_accept_ex_sockaddrs(server_socket);
      if (!get_addresses) {
         close_accepted_socket(Operation);
         result.RearmAccept = true;
         return result;
      }

      get_addresses(Operation.Buffer.get(), 0, IOCP_ACCEPT_ADDRESS_SIZE, IOCP_ACCEPT_ADDRESS_SIZE, &local_address,
         &local_size, &remote_address, &remote_size);

      if ((!remote_address) or (remote_size <= 0) or (remote_size > int(IOCP_ENDPOINT_STORAGE_SIZE))) {
         close_accepted_socket(Operation);
         result.RearmAccept = true;
         return result;
      }

      IocpAcceptedSocket accepted;
      accepted.Socket = Operation.AcceptedSocket;
      accepted.AddressSize = remote_size;
      std::memcpy(accepted.Address.data(), remote_address, size_t(remote_size));
      Record.AcceptedSockets.push_back(accepted);

      auto accepted_record = create_socket_record();
      {
         std::lock_guard<std::mutex> accepted_lock(accepted_record->Mutex);
         accepted_record->Generation = glNextGeneration++;
         accepted_record->IPv6 = Record.IPv6;
         accepted_record->Cancelled = false;
      }

      {
         std::lock_guard<std::mutex> registry_lock(glRegistryMutex);
         glSockets[Operation.AcceptedSocket] = accepted_record;
      }

      return result;
   }

   return result;
}

//********************************************************************************************************************

static ERR post_write_tail(const IocpOperation &Operation, size_t Offset)
{
   if ((!Operation.Buffer) or (Offset >= Operation.BufferSize)) return ERR::Okay;

   auto remaining = Operation.BufferSize - Offset;
   auto operation = create_operation();
   operation->Type              = IocpOperationType::WRITE;
   operation->Socket            = Operation.Socket;
   operation->Generation        = Operation.Generation;
   operation->ObjectID          = Operation.ObjectID;
   operation->Callback          = Operation.Callback;
   operation->BufferSize        = remaining;
   operation->SendAccountedSize = Operation.SendAccountedSize ? Operation.SendAccountedSize : Operation.BufferSize;
   operation->Buffer            = std::make_unique<uint8_t[]>(operation->BufferSize);
   std::memcpy(operation->Buffer.get(), Operation.Buffer.get() + Offset, remaining);

   WSABUF wsabuf;
   wsabuf.buf = (CHAR *)operation->Buffer.get();
   wsabuf.len = ULONG(operation->BufferSize);

   DWORD bytes = 0;
   auto result = WSASend(socket_from_handle(Operation.Socket), &wsabuf, 1, &bytes, 0, &operation->Overlapped, nullptr);
   if ((result IS SOCKET_ERROR) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto error = convert_error();
      release_operation(operation);
      return error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void queue_operation_completion(const IocpOperation &Operation, size_t BytesTransferred, ERR Error)
{
   iocp_completion_message message;
   bool rearm_accept = false;
   bool post_reads = false;
   bool post_sends = false;
   bool notify_completion = true;

   {
      auto record = find_socket_record(Operation.Socket);
      if (!record) {
         close_accepted_socket(Operation);
         return;
      }

      std::lock_guard<std::mutex> lock(record->Mutex);
      if ((record->Cancelled) or (record->Generation != Operation.Generation)) {
         close_accepted_socket(Operation);
         return;
      }

      auto target = completion_target(*record, Operation.Type);

      if ((Operation.Type IS IocpOperationType::WRITE) and (Error IS ERR::Okay) and
          (BytesTransferred > 0) and (BytesTransferred < Operation.BufferSize)) {
         if (auto tail_error = post_write_tail(Operation, BytesTransferred); tail_error IS ERR::Okay) return;
         else Error = tail_error;
      }

      auto store_result = store_operation_result(*record, Operation, Error);
      rearm_accept      = store_result.RearmAccept and (target.Callback) and (target.ObjectID > 0);
      post_reads        = store_result.PostRead;
      post_sends        = store_result.PostSend;
      notify_completion = store_result.Notify;

      message.Type       = Operation.Type;
      message.Socket     = Operation.Socket;
      message.Generation = Operation.Generation;
      message.ObjectID   = target.ObjectID ? target.ObjectID : Operation.ObjectID;
      message.Callback   = target.Callback;
      message.BytesTransferred = BytesTransferred;
      message.Error = Error;
   }

   if (post_reads) post_read_pool(Operation.Socket);
   if (post_sends) post_send_pool(Operation.Socket);
   if (rearm_accept) post_accept(Operation.Socket);

   if ((notify_completion) and (message.ObjectID > 0) and (message.Callback) and (glPostMessage)) {
      glPostMessage(glCompletionMsgID, &message, sizeof(message));
   }
}

//********************************************************************************************************************

static ERR queue_record_completion(WSW_SOCKET Socket, IocpOperationType Type)
{
   iocp_completion_message message;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;

      auto target = completion_target(*record, Type);
      if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

      message.Type = Type;
      message.Socket = Socket;
      message.Generation = record->Generation;
      message.ObjectID = target.ObjectID;
      message.Callback = target.Callback;
      message.Error = ERR::Okay;
   }

   if (glPostMessage) return glPostMessage(glCompletionMsgID, &message, sizeof(message));
   else return ERR::NotInitialised;
}

//********************************************************************************************************************

static ERR post_udp_receive(WSW_SOCKET Socket)
{
   IocpCompletionTarget target;
   uint64_t generation = 0;
   bool ipv6 = false;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;
      if (record->UdpReadPending) return ERR::Okay;
      if (!record->Datagrams.empty()) return ERR::Okay;
      if (record->ReadResult != ERR::Okay) return ERR::Okay;

      target = record->Read;
      if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

      record->UdpReadPending = true;
      generation = record->Generation;
      ipv6 = record->IPv6;
   }

   if (auto error = bind_udp_ephemeral(socket_from_handle(Socket), ipv6); error != ERR::Okay) {
      IocpOperation failed_operation;
      failed_operation.Type = IocpOperationType::UDP_RECEIVE;
      failed_operation.Socket = Socket;
      failed_operation.Generation = generation;
      failed_operation.ObjectID = target.ObjectID;
      failed_operation.Callback = target.Callback;
      failed_operation.Result = error;
      queue_operation_completion(failed_operation, 0, failed_operation.Result);
      return ERR::Okay;
   }

   auto operation = create_operation();
   operation->Type = IocpOperationType::UDP_RECEIVE;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;
   operation->BufferSize = IOCP_UDP_BUFFER_SIZE;
   operation->Buffer = std::make_unique<uint8_t[]>(operation->BufferSize);
   operation->AddressSize = int(operation->Address.size());

   WSABUF buffer;
   buffer.buf = (CHAR *)operation->Buffer.get();
   buffer.len = ULONG(operation->BufferSize);

   DWORD bytes = 0;
   DWORD flags = 0;
   auto result = WSARecvFrom(socket_from_handle(Socket), &buffer, 1, &bytes, &flags,
      (sockaddr *)operation->Address.data(), &operation->AddressSize, &operation->Overlapped, nullptr);
   if ((result IS SOCKET_ERROR) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto error = convert_error();
      operation->Result = error;
      operation->BytesTransferred = 0;
      queue_operation_completion(*operation, 0, operation->Result);
      release_operation(operation);
      return ERR::Okay;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR post_read(WSW_SOCKET Socket)
{
   IocpCompletionTarget target;
   uint64_t generation = 0;
   uint64_t sequence = 0;
   bool udp = false;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      udp = record->UDP;
      if (record->Cancelled) return ERR::Cancelled;
      if (!udp) {
         if (!tcp_read_post_needed(*record)) return ERR::Okay;

         record->ReadPendingCount++;
         generation = record->Generation;
         sequence = record->NextReadSequence++;
         target = record->Read;
      }
   }

   if (udp) return post_udp_receive(Socket);

   auto operation = create_operation();
   operation->Type = IocpOperationType::READ;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;
   operation->Sequence = sequence;
   operation->BufferSize = IOCP_READ_BUFFER_SIZE;
   operation->Buffer = std::make_unique<uint8_t[]>(operation->BufferSize);

   WSABUF buffer;
   buffer.buf = (CHAR *)operation->Buffer.get();
   buffer.len = ULONG(operation->BufferSize);

   DWORD bytes = 0;
   DWORD flags = 0;
   auto result = WSARecv(socket_from_handle(Socket), &buffer, 1, &bytes, &flags, &operation->Overlapped, nullptr);
   if ((result IS SOCKET_ERROR) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto error = convert_error();
      operation->Result = error;
      operation->BytesTransferred = 0;
      queue_operation_completion(*operation, 0, operation->Result);
      release_operation(operation);
      return ERR::Okay;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static bool read_post_needed(WSW_SOCKET Socket, ERR &Error)
{
   auto record = find_socket_record(Socket);
   if (!record) {
      Error = ERR::Search;
      return false;
   }

   std::lock_guard<std::mutex> lock(record->Mutex);
   if (record->Cancelled) {
      Error = ERR::Cancelled;
      return false;
   }

   Error = ERR::Okay;
   return tcp_read_post_needed(*record);
}

//********************************************************************************************************************

static ERR post_read_pool(WSW_SOCKET Socket)
{
   ERR error = ERR::Okay;
   size_t posted = 0;

   while (read_post_needed(Socket, error)) {
      if (auto post_error = post_read(Socket); post_error != ERR::Okay) return posted ? ERR::Okay : post_error;
      posted++;
   }

   return error;
}

//********************************************************************************************************************

static ERR post_send(WSW_SOCKET Socket)
{
   IocpCompletionTarget target;
   IocpSendRequest request;
   uint64_t generation = 0;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;
      if (!tcp_send_post_needed(*record)) return ERR::Okay;

      target = record->Write;
      generation = record->Generation;
      request = std::move(record->SendQueue.front());
      record->SendQueue.pop_front();
      record->SendPendingCount++;
   }

   if ((!request.Buffer) or (!request.BufferSize)) return ERR::Okay;

   auto operation = create_operation();
   operation->Type = IocpOperationType::WRITE;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;
   operation->BufferSize = request.BufferSize;
   operation->SendAccountedSize = request.BufferSize;
   operation->Buffer = std::move(request.Buffer);

   WSABUF wsabuf;
   wsabuf.buf = (CHAR *)operation->Buffer.get();
   wsabuf.len = ULONG(operation->BufferSize);

   DWORD bytes = 0;
   auto result = WSASend(socket_from_handle(Socket), &wsabuf, 1, &bytes, 0, &operation->Overlapped, nullptr);
   if ((result IS SOCKET_ERROR) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto error = convert_error();
      operation->Result = error;
      operation->BytesTransferred = 0;
      queue_operation_completion(*operation, 0, operation->Result);
      release_operation(operation);
      return ERR::Okay;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static bool send_post_needed(WSW_SOCKET Socket, ERR &Error)
{
   auto record = find_socket_record(Socket);
   if (!record) {
      Error = ERR::Search;
      return false;
   }

   std::lock_guard<std::mutex> lock(record->Mutex);
   if (record->Cancelled) {
      Error = ERR::Cancelled;
      return false;
   }

   Error = ERR::Okay;
   return tcp_send_post_needed(*record);
}

//********************************************************************************************************************

static ERR post_send_pool(WSW_SOCKET Socket)
{
   ERR error = ERR::Okay;
   size_t posted = 0;

   while (send_post_needed(Socket, error)) {
      if (auto post_error = post_send(Socket); post_error != ERR::Okay) return posted ? ERR::Okay : post_error;
      posted++;
   }

   return error;
}

//********************************************************************************************************************

static ERR post_accept(WSW_SOCKET Socket)
{
   IocpCompletionTarget target;
   uint64_t generation = 0;
   bool server_ipv6 = false;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;
      if (record->AcceptPendingCount >= size_t(record->AcceptDepth)) return ERR::Okay;

      target = record->Accept;
      if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

      record->AcceptPendingCount++;
      generation = record->Generation;
      server_ipv6 = record->IPv6;
   }

   bool accepted_ipv6 = false;
   auto accepted_socket = create_tcp_socket(server_ipv6, accepted_ipv6);
   if (accepted_socket IS INVALID_SOCKET) {
      if (auto record = find_socket_record(Socket); record) {
         std::lock_guard<std::mutex> lock(record->Mutex);
         if (record->AcceptPendingCount > 0) record->AcceptPendingCount--;
      }
      return convert_error();
   }

   auto operation = create_operation();
   operation->Type = IocpOperationType::ACCEPT;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;
   operation->AcceptedSocket = handle_from_socket(accepted_socket);
   operation->BufferSize = IOCP_ACCEPT_BUFFER_SIZE;
   operation->Buffer = std::make_unique<uint8_t[]>(operation->BufferSize);

   DWORD bytes = 0;
   auto accept_ex = get_accept_ex(socket_from_handle(Socket));
   if (!accept_ex) {
      {
         if (auto record = find_socket_record(Socket); record) {
            std::lock_guard<std::mutex> lock(record->Mutex);
            if (record->AcceptPendingCount > 0) record->AcceptPendingCount--;
         }
      }
      close_accepted_socket(*operation);
      release_operation(operation);
      return convert_error();
   }

   auto result = accept_ex(socket_from_handle(Socket), accepted_socket, operation->Buffer.get(), 0,
      IOCP_ACCEPT_ADDRESS_SIZE, IOCP_ACCEPT_ADDRESS_SIZE, &bytes, &operation->Overlapped);

   if ((!result) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto error = convert_error();
      {
         if (auto record = find_socket_record(Socket); record) {
            std::lock_guard<std::mutex> lock(record->Mutex);
            if (record->AcceptPendingCount > 0) record->AcceptPendingCount--;
         }
      }
      close_accepted_socket(*operation);
      release_operation(operation);
      return error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static bool accept_post_needed(WSW_SOCKET Socket, ERR &Error)
{
   auto record = find_socket_record(Socket);
   if (!record) {
      Error = ERR::Search;
      return false;
   }

   std::lock_guard<std::mutex> lock(record->Mutex);
   if (record->Cancelled) {
      Error = ERR::Cancelled;
      return false;
   }

   auto target = record->Accept;
   if ((!target.Callback) or (target.ObjectID <= 0)) {
      Error = ERR::Okay;
      return false;
   }

   Error = ERR::Okay;
   return record->AcceptPendingCount < size_t(record->AcceptDepth);
}

//********************************************************************************************************************

static ERR post_accept_pool(WSW_SOCKET Socket)
{
   ERR error = ERR::Okay;
   size_t posted = 0;

   while (accept_post_needed(Socket, error)) {
      if (auto post_error = post_accept(Socket); post_error != ERR::Okay) return posted ? ERR::Okay : post_error;
      posted++;
   }

   return error;
}

//********************************************************************************************************************

static void worker_thread()
{
   while (true) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED *overlapped = nullptr;

      auto result = GetQueuedCompletionStatus(glCompletionPort, &bytes, &key, &overlapped, INFINITE);
      if ((!overlapped) and (key IS IOCP_SHUTDOWN_KEY)) break;
      if (!overlapped) continue;

      auto operation = CONTAINING_RECORD(overlapped, IocpOperation, Overlapped);
      ERR error = ERR::Okay;

      if (!result) error = convert_error(GetLastError());

      operation->BytesTransferred = size_t(bytes);
      operation->Result = error;
      queue_operation_completion(*operation, operation->BytesTransferred, operation->Result);

      release_operation(operation);
   }
}

//********************************************************************************************************************

static ERR set_completion_target(WSW_SOCKET Socket, IocpOperationType Type, int ObjectID, uintptr_t Callback)
{
   auto record = find_socket_record(Socket);
   if (!record) return ERR::Search;

   std::lock_guard<std::mutex> lock(record->Mutex);
   if (record->Cancelled) return ERR::Cancelled;

   auto target = IocpCompletionTarget { ObjectID, Callback };
   if (Type IS IocpOperationType::READ) record->Read = target;
   else if (Type IS IocpOperationType::WRITE) record->Write = target;
   else if (Type IS IocpOperationType::ACCEPT) record->Accept = target;
   else if (Type IS IocpOperationType::CONNECT) record->Connect = target;
   else return ERR::NoSupport;

   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_initialise(int MsgID, iocp_post_message PostMessage)
{
   glCompletionMsgID = MsgID;
   glPostMessage = PostMessage;

   if (!glWinsockInitialised) {
      WSADATA data;
      if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return ERR::SystemCall;
      glWinsockInitialised = true;
   }

   if (glCompletionPort != INVALID_HANDLE_VALUE) return ERR::Okay;

   glCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 0);
   if (glCompletionPort IS nullptr) {
      glCompletionPort = INVALID_HANDLE_VALUE;
      return ERR::SystemCall;
   }

   auto worker_count = std::clamp<unsigned>(std::thread::hardware_concurrency(), 2, 64);

   glShutdownRequested = false;
   glWorkers.reserve(worker_count);
   for (unsigned i = 0; i < worker_count; ++i) glWorkers.emplace_back(worker_thread);

   return ERR::Okay;
}

//********************************************************************************************************************

void iocp_expunge()
{
   glShutdownRequested = true;

   if (glCompletionPort != INVALID_HANDLE_VALUE) {
      std::vector<WSW_SOCKET> sockets;
      std::vector<IocpSocketRecordPtr> records;

      {
         std::lock_guard<std::mutex> lock(glRegistryMutex);
         sockets.reserve(glSockets.size());
         for (auto &entry : glSockets) {
            sockets.push_back(entry.first);
            records.push_back(entry.second);
         }
      }

      for (auto &record : records) {
         std::lock_guard<std::mutex> record_lock(record->Mutex);
         record->Cancelled = true;
         record->Generation++;

         for (auto &accepted : record->AcceptedSockets) {
            sockets.push_back(accepted.Socket);
         }
      }

      std::sort(sockets.begin(), sockets.end());
      sockets.erase(std::unique(sockets.begin(), sockets.end()), sockets.end());

      for (auto socket : sockets) {
         auto native_socket = socket_from_handle(socket);
         if (native_socket IS INVALID_SOCKET) continue;

         CancelIoEx((HANDLE)native_socket, nullptr);
         shutdown(native_socket, SD_BOTH);
         closesocket(native_socket);
      }

      while (true) {
         auto pending = glPendingOperations.load();
         if (!pending) break;
         glPendingOperations.wait(pending);
      }

      for (size_t i = 0; i < glWorkers.size(); ++i) {
         PostQueuedCompletionStatus(glCompletionPort, 0, IOCP_SHUTDOWN_KEY, nullptr);
      }
   }

   glWorkers.clear();

   {
      std::lock_guard<std::mutex> lock(glRegistryMutex);
      glSockets.clear();
   }

   if (glCompletionPort != INVALID_HANDLE_VALUE) {
      CloseHandle(glCompletionPort);
      glCompletionPort = INVALID_HANDLE_VALUE;
   }

   if (glWinsockInitialised) {
      WSACleanup();
      glWinsockInitialised = false;
   }
}

//********************************************************************************************************************

WSW_SOCKET iocp_create_socket(int ObjectID, bool UDP, bool &IPv6)
{
   if (glCompletionPort IS INVALID_HANDLE_VALUE) {
      IPv6 = false;
      return WSW_SOCKET(INVALID_SOCKET);
   }

   SOCKET socket = UDP ? create_udp_socket(true, IPv6) : create_tcp_socket(true, IPv6);

   if (socket IS INVALID_SOCKET) return WSW_SOCKET(INVALID_SOCKET);

   auto handle = CreateIoCompletionPort((HANDLE)socket, glCompletionPort, ULONG_PTR(socket), 0);
   if (handle IS nullptr) {
      closesocket(socket);
      return WSW_SOCKET(INVALID_SOCKET);
   }

   WSW_SOCKET result = handle_from_socket(socket);

   auto record = create_socket_record();
   {
      std::lock_guard<std::mutex> record_lock(record->Mutex);
      record->ObjectID = ObjectID;
      record->Generation = glNextGeneration++;
      record->IPv6 = IPv6;
      record->UDP = UDP;
      record->Cancelled = false;
   }

   std::lock_guard<std::mutex> lock(glRegistryMutex);
   glSockets[result] = record;

   return result;
}

//********************************************************************************************************************

void iocp_close_socket(WSW_SOCKET Socket)
{
   iocp_deregister_socket(Socket);

   auto native_socket = socket_from_handle(Socket);
   if (native_socket IS INVALID_SOCKET) return;

   shutdown(native_socket, SD_SEND);
   closesocket(native_socket);
}

//********************************************************************************************************************

void iocp_deregister_socket(WSW_SOCKET Socket)
{
   if (auto record = find_socket_record(Socket); record) {
      std::lock_guard<std::mutex> record_lock(record->Mutex);
      record->Cancelled = true;
      record->Generation++;
   }

   {
      std::lock_guard<std::mutex> lock(glRegistryMutex);
      auto record = glSockets.find(Socket);
      if (record != glSockets.end()) {
         glSockets.erase(record);
      }
   }
}

//********************************************************************************************************************

int iocp_shutdown_socket(WSW_SOCKET Socket, int How)
{
   return shutdown(socket_from_handle(Socket), How);
}

//********************************************************************************************************************

void iocp_set_socket_object(WSW_SOCKET Socket, int ObjectID)
{
   auto record = find_socket_record(Socket);
   if (record) {
      std::lock_guard<std::mutex> lock(record->Mutex);
      record->ObjectID = ObjectID;
   }
}

//********************************************************************************************************************

ERR iocp_prepare_connect(WSW_SOCKET Socket, const void *Address, int AddressSize)
{
   if ((!Address) or (AddressSize <= 0) or (AddressSize > int(IOCP_ENDPOINT_STORAGE_SIZE))) return ERR::Args;

   auto record = find_socket_record(Socket);
   if (!record) return ERR::Search;

   std::lock_guard<std::mutex> lock(record->Mutex);
   if (record->Cancelled) return ERR::Cancelled;

   std::memcpy(record->ConnectAddress.data(), Address, size_t(AddressSize));
   record->ConnectAddressSize = AddressSize;
   record->ConnectResult = ERR::Busy;
   return ERR::Busy;
}

//********************************************************************************************************************

ERR iocp_begin_connect_wait(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> address;
   int address_size = 0;
   uint64_t generation = 0;
   IocpCompletionTarget target;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;
      if (record->ConnectAddressSize <= 0) return ERR::NotInitialised;

      record->Connect = { ObjectID, Callback };
      record->ConnectResult = ERR::Busy;

      address = record->ConnectAddress;
      address_size = record->ConnectAddressSize;
      generation = record->Generation;
      target = record->Connect;
   }

   IocpOperation failed_operation;
   failed_operation.Type = IocpOperationType::CONNECT;
   failed_operation.Socket = Socket;
   failed_operation.Generation = generation;
   failed_operation.ObjectID = target.ObjectID;
   failed_operation.Callback = target.Callback;

   auto socket = socket_from_handle(Socket);
   auto remote_address = (const sockaddr *)address.data();

   auto error = bind_ephemeral(socket, remote_address);
   if (error != ERR::Okay) {
      failed_operation.Result = error;
      queue_operation_completion(failed_operation, 0, failed_operation.Result);
      return ERR::Okay;
   }

   auto connect_ex = get_connect_ex(socket);
   if (!connect_ex) {
      failed_operation.Result = convert_error();
      queue_operation_completion(failed_operation, 0, failed_operation.Result);
      return ERR::Okay;
   }

   auto operation = create_operation();
   operation->Type = IocpOperationType::CONNECT;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;

   DWORD bytes = 0;
   auto result = connect_ex(socket, remote_address, address_size, nullptr, 0, &bytes, &operation->Overlapped);
   if ((!result) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto connect_error = convert_error();
      release_operation(operation);
      failed_operation.Result = connect_error;
      queue_operation_completion(failed_operation, 0, failed_operation.Result);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_complete_connect(WSW_SOCKET Socket)
{
   ERR result = ERR::Okay;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      result = record->ConnectResult;
   }

   if (result IS ERR::Okay) {
      auto socket = socket_from_handle(Socket);
      if (setsockopt(socket, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, nullptr, 0) IS SOCKET_ERROR) {
         return convert_error();
      }
   }

   return result;
}

//********************************************************************************************************************

bool iocp_validate_completion(WSW_SOCKET Socket, uint64_t Generation)
{
   auto record = find_socket_record(Socket);
   if (!record) return false;

   std::lock_guard<std::mutex> lock(record->Mutex);
   if (record->Cancelled) return false;
   return record->Generation IS Generation;
}

//********************************************************************************************************************

ERR iocp_bind(WSW_SOCKET Socket, const void *Address, int AddressSize)
{
   if ((!Address) or (AddressSize <= 0)) return ERR::Args;
   if (bind(socket_from_handle(Socket), (const sockaddr *)Address, AddressSize) IS SOCKET_ERROR) return convert_error();
   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_listen(WSW_SOCKET Socket, int Backlog)
{
   if (listen(socket_from_handle(Socket), Backlog) IS SOCKET_ERROR) return convert_error();

   if (auto record = find_socket_record(Socket); record) {
      std::lock_guard<std::mutex> lock(record->Mutex);
      record->AcceptDepth = accept_depth_from_backlog(Backlog);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_register_accept(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::ACCEPT, ObjectID, Callback);
       error != ERR::Okay) return error;

   return post_accept_pool(Socket);
}

//********************************************************************************************************************

ERR iocp_accept(WSW_SOCKET Server, WSW_SOCKET &Client, void *Address, int *AddressSize)
{
   Client = WSW_SOCKET(INVALID_SOCKET);
   if ((!Address) or (!AddressSize)) return ERR::NullArgs;

   {
      auto record = find_socket_record(Server);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;
      if (record->AcceptedSockets.empty()) return ERR::Search;

      auto accepted = record->AcceptedSockets.front();
      record->AcceptedSockets.pop_front();

      Client = accepted.Socket;
      auto copy_size = std::min(*AddressSize, accepted.AddressSize);
      std::memcpy(Address, accepted.Address.data(), size_t(copy_size));
      *AddressSize = copy_size;
   }

   post_accept(Server);

   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_register_read(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::READ, ObjectID, Callback);
       error != ERR::Okay) return error;

   bool buffered = false;
   bool terminal = false;
   bool udp = false;
   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      udp = record->UDP;
      buffered = udp ? !record->Datagrams.empty() : record->BufferedReadBytes > 0;
      terminal = record->ReadResult != ERR::Okay;
   }

   if ((buffered) or (terminal)) {
      return queue_record_completion(Socket, udp ? IocpOperationType::UDP_RECEIVE : IocpOperationType::READ);
   }
   else return udp ? post_read(Socket) : post_read_pool(Socket);
}

//********************************************************************************************************************

ERR iocp_register_write(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::WRITE, ObjectID, Callback);
       error != ERR::Okay) return error;

   bool capacity_available = false;
   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      capacity_available = tcp_send_capacity_available(*record);
   }

   return capacity_available ? queue_record_completion(Socket, IocpOperationType::WRITE) : ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_remove_read(WSW_SOCKET Socket)
{
   return set_completion_target(Socket, IocpOperationType::READ, 0, 0);
}

//********************************************************************************************************************

ERR iocp_remove_write(WSW_SOCKET Socket)
{
   return set_completion_target(Socket, IocpOperationType::WRITE, 0, 0);
}

//********************************************************************************************************************

bool iocp_has_pending_write(WSW_SOCKET Socket)
{
   auto record = find_socket_record(Socket);
   if (!record) return false;

   std::lock_guard<std::mutex> lock(record->Mutex);
   return (not record->Cancelled) and (record->SendPendingBytes > 0);
}

//********************************************************************************************************************

ERR iocp_recall_read(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::READ, ObjectID, Callback);
       error != ERR::Okay) return error;

   bool udp = false;
   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      udp = record->UDP;
   }

   return queue_record_completion(Socket, udp ? IocpOperationType::UDP_RECEIVE : IocpOperationType::READ);
}

//********************************************************************************************************************

ERR iocp_receive(WSW_SOCKET Socket, void *Buffer, size_t Length, size_t &Received)
{
   Received = 0;
   if ((!Buffer) and (Length > 0)) return ERR::NullArgs;
   if (!Length) return ERR::Okay;

   ERR result = ERR::Okay;
   bool rearm_read = false;
   bool recall_read = false;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;

      auto available = record->BufferedReadBytes;
      if (available > 0) {
         Received = std::min(Length, available);
         std::memcpy(Buffer, record->ReadBuffer.data() + record->ReadOffset, Received);
         record->ReadOffset += Received;
         record->BufferedReadBytes -= Received;

         if (record->BufferedReadBytes <= 0) {
            record->ReadBuffer.clear();
            record->ReadOffset = 0;
            if (record->ReadResult IS ERR::Okay) rearm_read = true;
            else recall_read = true;
         }
         else recall_read = true;

         if ((record->ReadResult IS ERR::Okay) and (tcp_read_post_needed(*record))) {
            rearm_read = true;
         }
      }
      else {
         result = record->ReadResult;
         if (result IS ERR::Okay) rearm_read = true;
      }
   }

   if (rearm_read) {
      if (auto error = post_read_pool(Socket); error != ERR::Okay) return error;
   }
   if (recall_read) {
      if (auto error = queue_record_completion(Socket, IocpOperationType::READ); error != ERR::Okay) return error;
   }

   return result;
}

//********************************************************************************************************************

ERR iocp_append_receive(WSW_SOCKET Socket, std::vector<uint8_t> &Buffer, size_t Length, size_t &Received)
{
   Received = 0;
   if (!Length) return ERR::Okay;

   std::vector<uint8_t> temp(Length);
   auto error = iocp_receive(Socket, temp.data(), temp.size(), Received);
   if ((error IS ERR::Okay) and (Received > 0)) {
      auto offset = Buffer.size();
      Buffer.resize(offset + Received);
      std::memcpy(Buffer.data() + offset, temp.data(), Received);
   }
   return error;
}

//********************************************************************************************************************

ERR iocp_send(WSW_SOCKET Socket, const void *Buffer, size_t &Length)
{
   if ((!Buffer) and (Length > 0)) return ERR::NullArgs;
   if (!Length) return ERR::Okay;

   size_t requested = std::min<size_t>(Length, 0x7fffffff);
   size_t accepted = 0;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;

      auto capacity = IOCP_TCP_SEND_HIGH_WATER - std::min(record->SendPendingBytes, IOCP_TCP_SEND_HIGH_WATER);
      if (!capacity) {
         Length = 0;
         return ERR::BufferOverflow;
      }

      accepted = std::min(requested, capacity);

      IocpSendRequest request;
      request.BufferSize = accepted;
      request.Buffer = std::make_unique<uint8_t[]>(request.BufferSize);
      std::memcpy(request.Buffer.get(), Buffer, request.BufferSize);

      record->SendQueue.push_back(std::move(request));
      record->SendPendingBytes += accepted;
   }

   Length = accepted;
   if (auto error = post_send_pool(Socket); error != ERR::Okay) {
      Length = 0;
      return error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_send_to(WSW_SOCKET Socket, const void *Buffer, size_t &Length, const void *Address, int AddressSize)
{
   if ((!Buffer) and (Length > 0)) return ERR::NullArgs;
   if ((!Address) or (AddressSize <= 0) or (AddressSize > int(IOCP_ENDPOINT_STORAGE_SIZE))) return ERR::Args;
   if (!Length) return ERR::Okay;

   uint64_t generation = 0;
   int object_id = 0;
   size_t requested = std::min<size_t>(Length, 0x7fffffff);

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;

      generation = record->Generation;
      object_id = record->ObjectID;
   }

   auto operation = create_operation();
   operation->Type        = IocpOperationType::UDP_SEND;
   operation->Socket      = Socket;
   operation->Generation  = generation;
   operation->ObjectID    = object_id;
   operation->BufferSize  = requested;
   operation->Buffer      = std::make_unique<uint8_t[]>(operation->BufferSize);
   operation->AddressSize = AddressSize;
   std::memcpy(operation->Buffer.get(), Buffer, requested);
   std::memcpy(operation->Address.data(), Address, size_t(AddressSize));

   WSABUF wsabuf;
   wsabuf.buf = (CHAR *)operation->Buffer.get();
   wsabuf.len = ULONG(operation->BufferSize);

   DWORD bytes = 0;
   auto result = WSASendTo(socket_from_handle(Socket), &wsabuf, 1, &bytes, 0,
      (const sockaddr *)operation->Address.data(), operation->AddressSize, &operation->Overlapped, nullptr);
   auto socket_error = (result IS SOCKET_ERROR) ? WSAGetLastError() : 0;
   if ((result IS SOCKET_ERROR) and (socket_error != WSA_IO_PENDING)) {
      auto error = convert_send_to_error(socket_error);
      release_operation(operation);
      Length = 0;
      return error;
   }

   Length = requested;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_receive_from(WSW_SOCKET Socket, void *Buffer, size_t BufferSize, size_t &BytesRead, void *Address,
   int *AddressSize)
{
   BytesRead = 0;
   if ((!Buffer) or (!Address) or (!AddressSize)) return ERR::NullArgs;
   if (!BufferSize) return ERR::Okay;

   ERR result = ERR::Okay;
   bool rearm_read = false;
   bool recall_read = false;
   IocpDatagram datagram;
   bool has_datagram = false;

   {
      auto record = find_socket_record(Socket);
      if (!record) return ERR::Search;

      std::lock_guard<std::mutex> lock(record->Mutex);
      if (record->Cancelled) return ERR::Cancelled;

      if (!record->Datagrams.empty()) {
         datagram = std::move(record->Datagrams.front());
         record->Datagrams.erase(record->Datagrams.begin());
         has_datagram = true;
         recall_read = !record->Datagrams.empty();
         if ((!recall_read) and (record->ReadResult IS ERR::Okay)) rearm_read = true;
      }
      else {
         result = record->ReadResult;
         if ((result IS ERR::Okay) and (!record->UdpReadPending)) rearm_read = true;
      }
   }

   if (has_datagram) {
      auto copy_size = std::min(BufferSize, datagram.Buffer.size());
      if (copy_size > 0) std::memcpy(Buffer, datagram.Buffer.data(), copy_size);
      BytesRead = copy_size;

      auto address_size = std::min(*AddressSize, datagram.AddressSize);
      if (address_size > 0) std::memcpy(Address, datagram.Address.data(), size_t(address_size));
      *AddressSize = address_size;

      if (BufferSize < datagram.Buffer.size()) result = ERR::BufferOverflow;
      else result = datagram.Result;
   }

   if (recall_read) {
      if (auto error = queue_record_completion(Socket, IocpOperationType::UDP_RECEIVE); error != ERR::Okay) {
         return error;
      }
   }
   else if (rearm_read) {
      if (auto error = post_udp_receive(Socket); error != ERR::Okay) return error;
   }

   return result;
}

//********************************************************************************************************************

ERR iocp_get_local_ip(WSW_SOCKET Socket, void *Address, int *AddressSize)
{
   if ((!Address) or (!AddressSize)) return ERR::NullArgs;
   return getsockname(socket_from_handle(Socket), (sockaddr *)Address, AddressSize) ? convert_error() : ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_enable_broadcast(WSW_SOCKET Socket)
{
   BOOL broadcast = TRUE;
   if (setsockopt(socket_from_handle(Socket), SOL_SOCKET, SO_BROADCAST, (char *)&broadcast,
       sizeof(broadcast)) IS SOCKET_ERROR) {
      return convert_error();
   }
   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_set_multicast_ttl(WSW_SOCKET Socket, int TTL, bool IPv6)
{
   DWORD ttl = DWORD(TTL);
   if (IPv6) {
      if (setsockopt(socket_from_handle(Socket), IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char *)&ttl,
          sizeof(ttl)) IS SOCKET_ERROR) {
         return convert_error();
      }
   }
   else if (setsockopt(socket_from_handle(Socket), IPPROTO_IP, IP_MULTICAST_TTL, (char *)&ttl,
       sizeof(ttl)) IS SOCKET_ERROR) {
      return convert_error();
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_parse_multicast_group(const char *Group, bool &IPv6)
{
   if ((!Group) or (!Group[0])) return ERR::Args;

   in6_addr addr6 = {};
   in_addr addr4 = {};

   if (inet_pton(AF_INET6, Group, &addr6) IS 1) {
      if (addr6.s6_addr[0] != 0xff) return ERR::Args;
      IPv6 = true;
      return ERR::Okay;
   }
   else if (inet_pton(AF_INET, Group, &addr4) IS 1) {
      auto host_address = ntohl(addr4.s_addr);
      if ((host_address < 0xe0000000) or (host_address > 0xefffffff)) return ERR::Args;
      IPv6 = false;
      return ERR::Okay;
   }
   else return ERR::Args;
}

//********************************************************************************************************************

ERR iocp_join_multicast_group(WSW_SOCKET Socket, const char *Group, bool IPv6)
{
   if (IPv6) {
      ipv6_mreq request = {};
      if (inet_pton(AF_INET6, Group, &request.ipv6mr_multiaddr) != 1) return ERR::Args;
      request.ipv6mr_interface = 0;
      if (setsockopt(socket_from_handle(Socket), IPPROTO_IPV6, IPV6_JOIN_GROUP, (char *)&request,
          sizeof(request)) IS SOCKET_ERROR) {
         return convert_error();
      }
   }
   else {
      ip_mreq request = {};
      if (inet_pton(AF_INET, Group, &request.imr_multiaddr) != 1) return ERR::Args;
      request.imr_interface.s_addr = INADDR_ANY;
      if (setsockopt(socket_from_handle(Socket), IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&request,
          sizeof(request)) IS SOCKET_ERROR) {
         return convert_error();
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_leave_multicast_group(WSW_SOCKET Socket, const char *Group, bool IPv6)
{
   if (IPv6) {
      ipv6_mreq request = {};
      if (inet_pton(AF_INET6, Group, &request.ipv6mr_multiaddr) != 1) return ERR::Args;
      request.ipv6mr_interface = 0;
      if (setsockopt(socket_from_handle(Socket), IPPROTO_IPV6, IPV6_LEAVE_GROUP, (char *)&request,
          sizeof(request)) IS SOCKET_ERROR) {
         return convert_error();
      }
   }
   else {
      ip_mreq request = {};
      if (inet_pton(AF_INET, Group, &request.imr_multiaddr) != 1) return ERR::Args;
      request.imr_interface.s_addr = INADDR_ANY;
      if (setsockopt(socket_from_handle(Socket), IPPROTO_IP, IP_DROP_MEMBERSHIP, (char *)&request,
          sizeof(request)) IS SOCKET_ERROR) {
         return convert_error();
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

uint32_t iocp_htonl(uint32_t Value)
{
   return htonl(Value);
}

uint32_t iocp_ntohl(uint32_t Value)
{
   return ntohl(Value);
}

uint16_t iocp_htons(uint16_t Value)
{
   return htons(Value);
}

uint16_t iocp_ntohs(uint16_t Value)
{
   return ntohs(Value);
}

#endif
