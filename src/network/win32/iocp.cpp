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
   OVERLAPPED Overlapped = {};
   IocpOperationType Type = IocpOperationType::CONNECT;
   WSW_SOCKET Socket = 0;
   uint64_t Generation = 0;
   int ObjectID = 0;
   uintptr_t Callback = 0;
   WSW_SOCKET AcceptedSocket = 0;
   std::unique_ptr<uint8_t[]> Buffer;
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> Address = {};
   int AddressSize = 0;
   size_t BufferSize = 0;
   size_t BytesTransferred = 0;
   ERR Result = ERR::Busy;
};

struct IocpCompletionTarget {
   int ObjectID = 0;
   uintptr_t Callback = 0;
};

struct IocpAcceptedSocket {
   WSW_SOCKET Socket = 0;
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> Address = {};
   int AddressSize = 0;
};

struct IocpDatagram {
   std::vector<uint8_t> Buffer;
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> Address = {};
   int AddressSize = 0;
   ERR Result = ERR::Okay;
};

struct IocpSocketRecord {
   IocpCompletionTarget Connect;
   IocpCompletionTarget Read;
   IocpCompletionTarget Write;
   IocpCompletionTarget Accept;
   std::vector<IocpAcceptedSocket> AcceptedSockets;
   std::vector<IocpDatagram> Datagrams;
   std::vector<uint8_t> ReadBuffer;
   size_t ReadOffset = 0;
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> ConnectAddress = {};
   int ConnectAddressSize = 0;
   ERR ConnectResult = ERR::NotInitialised;
   ERR ReadResult = ERR::Okay;
   uint64_t Generation = 0;
   bool IPv6 = false;
   bool UDP = false;
   bool Cancelled = false;
   bool AcceptPending = false;
   bool ReadPending = false;
   bool WritePending = false;
};

static HANDLE glCompletionPort = INVALID_HANDLE_VALUE;
static std::vector<std::jthread> glWorkers;
static std::mutex glIocpMutex;
static std::unordered_map<WSW_SOCKET, IocpSocketRecord> glSockets;
static std::atomic_bool glShutdownRequested = false;
static std::atomic_uint64_t glNextGeneration = 1;
static int glCompletionMsgID = 0;
static iocp_post_message glPostMessage = nullptr;
static bool glWinsockInitialised = false;

static constexpr int MAX_IOCP_THREADS = 4;
static constexpr size_t IOCP_READ_BUFFER_SIZE = 65536;
static constexpr size_t IOCP_UDP_BUFFER_SIZE = 65536;
static constexpr size_t IOCP_ACCEPT_ADDRESS_SIZE = sizeof(sockaddr_storage) + 16;
static constexpr size_t IOCP_ACCEPT_BUFFER_SIZE = IOCP_ACCEPT_ADDRESS_SIZE * 2;

//********************************************************************************************************************

static ERR post_accept(WSW_SOCKET Socket);

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

static bool store_operation_result(IocpSocketRecord &Record, const IocpOperation &Operation, ERR Error)
{
   if (Operation.Type IS IocpOperationType::CONNECT) Record.ConnectResult = Error;
   else if (Operation.Type IS IocpOperationType::READ) {
      Record.ReadPending = false;
      Record.ReadResult = ((Error IS ERR::Okay) and (!Operation.BytesTransferred)) ? ERR::Disconnected : Error;

      if ((Record.ReadResult IS ERR::Okay) and (Operation.BytesTransferred > 0) and (Operation.Buffer)) {
         auto offset = Record.ReadBuffer.size();
         Record.ReadBuffer.resize(offset + Operation.BytesTransferred);
         std::memcpy(Record.ReadBuffer.data() + offset, Operation.Buffer.get(), Operation.BytesTransferred);
      }
   }
   else if (Operation.Type IS IocpOperationType::WRITE) {
      Record.WritePending = false;
   }
   else if (Operation.Type IS IocpOperationType::UDP_RECEIVE) {
      Record.ReadPending = false;
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
      Record.AcceptPending = false;

      if (Error != ERR::Okay) {
         close_accepted_socket(Operation);
         return true;
      }

      auto server_socket = socket_from_handle(Operation.Socket);
      auto accepted_socket = socket_from_handle(Operation.AcceptedSocket);

      if (setsockopt(accepted_socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char *)&server_socket,
          sizeof(server_socket)) IS SOCKET_ERROR) {
         close_accepted_socket(Operation);
         return true;
      }

      auto handle = CreateIoCompletionPort((HANDLE)accepted_socket, glCompletionPort, ULONG_PTR(accepted_socket), 0);
      if (handle IS nullptr) {
         close_accepted_socket(Operation);
         return true;
      }

      sockaddr *local_address = nullptr;
      sockaddr *remote_address = nullptr;
      int local_size = 0;
      int remote_size = 0;
      auto get_addresses = get_accept_ex_sockaddrs(server_socket);
      if (!get_addresses) {
         close_accepted_socket(Operation);
         return true;
      }

      get_addresses(Operation.Buffer.get(), 0, IOCP_ACCEPT_ADDRESS_SIZE, IOCP_ACCEPT_ADDRESS_SIZE, &local_address,
         &local_size, &remote_address, &remote_size);

      if ((!remote_address) or (remote_size <= 0) or (remote_size > int(IOCP_ENDPOINT_STORAGE_SIZE))) {
         close_accepted_socket(Operation);
         return true;
      }

      IocpAcceptedSocket accepted;
      accepted.Socket = Operation.AcceptedSocket;
      accepted.AddressSize = remote_size;
      std::memcpy(accepted.Address.data(), remote_address, size_t(remote_size));
      Record.AcceptedSockets.push_back(accepted);

      glSockets[accepted.Socket] = {
         .Generation = glNextGeneration++,
         .IPv6 = Record.IPv6,
         .Cancelled = false
      };
   }

   return false;
}

//********************************************************************************************************************

static void queue_operation_completion(const IocpOperation &Operation, size_t BytesTransferred, ERR Error)
{
   iocp_completion_message message;
   bool rearm_accept = false;

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Operation.Socket);
      if (record IS glSockets.end()) {
         close_accepted_socket(Operation);
         return;
      }
      if ((record->second.Cancelled) or (record->second.Generation != Operation.Generation)) {
         close_accepted_socket(Operation);
         return;
      }

      auto target = completion_target(record->second, Operation.Type);
      rearm_accept = store_operation_result(record->second, Operation, Error) and
         (target.Callback) and (target.ObjectID > 0);

      message.Type = Operation.Type;
      message.Socket = Operation.Socket;
      message.Generation = Operation.Generation;
      message.ObjectID = target.ObjectID;
      message.Callback = target.Callback;
      message.BytesTransferred = BytesTransferred;
      message.Error = Error;
   }

   if (rearm_accept) post_accept(Operation.Socket);

   if ((message.ObjectID > 0) and (message.Callback) and (glPostMessage)) {
      glPostMessage(glCompletionMsgID, &message, sizeof(message));
   }
}

//********************************************************************************************************************

static ERR queue_record_completion(WSW_SOCKET Socket, IocpOperationType Type)
{
   iocp_completion_message message;

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;

      auto target = completion_target(record->second, Type);
      if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

      message.Type = Type;
      message.Socket = Socket;
      message.Generation = record->second.Generation;
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
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;
      if (record->second.ReadPending) return ERR::Okay;
      if (!record->second.Datagrams.empty()) return ERR::Okay;
      if (record->second.ReadResult != ERR::Okay) return ERR::Okay;

      target = record->second.Read;
      if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

      record->second.ReadPending = true;
      generation = record->second.Generation;
      ipv6 = record->second.IPv6;
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

   auto operation = new IocpOperation();
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
      delete operation;
      return ERR::Okay;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR post_read(WSW_SOCKET Socket)
{
   IocpCompletionTarget target;
   uint64_t generation = 0;
   bool udp = false;

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      udp = record->second.UDP;
      if (record->second.Cancelled) return ERR::Cancelled;
      if (!udp) {
         if (record->second.ReadPending) return ERR::Okay;
         if (!record->second.ReadBuffer.empty()) return ERR::Okay;
         if (record->second.ReadResult != ERR::Okay) return ERR::Okay;

         target = record->second.Read;
         if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

         record->second.ReadPending = true;
         generation = record->second.Generation;
      }
   }

   if (udp) return post_udp_receive(Socket);

   auto operation = new IocpOperation();
   operation->Type = IocpOperationType::READ;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;
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
      delete operation;
      return ERR::Okay;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR post_accept(WSW_SOCKET Socket)
{
   IocpCompletionTarget target;
   uint64_t generation = 0;
   bool server_ipv6 = false;

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;
      if (record->second.AcceptPending) return ERR::Okay;

      target = record->second.Accept;
      if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

      record->second.AcceptPending = true;
      generation = record->second.Generation;
      server_ipv6 = record->second.IPv6;
   }

   bool accepted_ipv6 = false;
   auto accepted_socket = create_tcp_socket(server_ipv6, accepted_ipv6);
   if (accepted_socket IS INVALID_SOCKET) {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record != glSockets.end()) record->second.AcceptPending = false;
      return convert_error();
   }

   auto operation = new IocpOperation();
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
         std::lock_guard<std::mutex> lock(glIocpMutex);
         auto record = glSockets.find(Socket);
         if (record != glSockets.end()) record->second.AcceptPending = false;
      }
      close_accepted_socket(*operation);
      delete operation;
      return convert_error();
   }

   auto result = accept_ex(socket_from_handle(Socket), accepted_socket, operation->Buffer.get(), 0,
      IOCP_ACCEPT_ADDRESS_SIZE, IOCP_ACCEPT_ADDRESS_SIZE, &bytes, &operation->Overlapped);

   if ((!result) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto error = convert_error();
      {
         std::lock_guard<std::mutex> lock(glIocpMutex);
         auto record = glSockets.find(Socket);
         if (record != glSockets.end()) record->second.AcceptPending = false;
      }
      close_accepted_socket(*operation);
      delete operation;
      return error;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void worker_thread()
{
   while (not glShutdownRequested) {
      DWORD bytes = 0;
      ULONG_PTR key = 0;
      OVERLAPPED *overlapped = nullptr;

      auto result = GetQueuedCompletionStatus(glCompletionPort, &bytes, &key, &overlapped, INFINITE);
      if (glShutdownRequested) break;
      if (!overlapped) continue;

      auto operation = CONTAINING_RECORD(overlapped, IocpOperation, Overlapped);
      ERR error = ERR::Okay;

      if (!result) error = convert_error(GetLastError());

      operation->BytesTransferred = size_t(bytes);
      operation->Result = error;
      queue_operation_completion(*operation, operation->BytesTransferred, operation->Result);

      delete operation;
   }
}

//********************************************************************************************************************

static ERR set_completion_target(WSW_SOCKET Socket, IocpOperationType Type, int ObjectID, uintptr_t Callback)
{
   std::lock_guard<std::mutex> lock(glIocpMutex);
   auto record = glSockets.find(Socket);
   if (record IS glSockets.end()) return ERR::Search;
   if (record->second.Cancelled) return ERR::Cancelled;

   auto target = IocpCompletionTarget { ObjectID, Callback };
   if (Type IS IocpOperationType::READ) record->second.Read = target;
   else if (Type IS IocpOperationType::WRITE) record->second.Write = target;
   else if (Type IS IocpOperationType::ACCEPT) record->second.Accept = target;
   else if (Type IS IocpOperationType::CONNECT) record->second.Connect = target;
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

   auto hardware_threads = std::thread::hardware_concurrency();
   auto worker_count = std::min<unsigned>(std::max<unsigned>(hardware_threads, 2), MAX_IOCP_THREADS);

   glShutdownRequested = false;
   glWorkers.reserve(worker_count);
   for (unsigned i = 0; i < worker_count; ++i) glWorkers.emplace_back(worker_thread);

   return ERR::Okay;
}

//********************************************************************************************************************

void iocp_expunge()
{
   glShutdownRequested = true;

   for (size_t i = 0; i < glWorkers.size(); ++i) {
      PostQueuedCompletionStatus(glCompletionPort, 0, 0, nullptr);
   }

   glWorkers.clear();

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
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

WSW_SOCKET iocp_create_socket(bool UDP, bool &IPv6)
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

   std::lock_guard<std::mutex> lock(glIocpMutex);
   glSockets[result] = {
      .Generation = glNextGeneration++,
      .IPv6 = IPv6,
      .UDP = UDP,
      .Cancelled = false
   };

   return result;
}

//********************************************************************************************************************

void iocp_close_socket(WSW_SOCKET Socket)
{
   iocp_deregister_socket(Socket);
   closesocket(socket_from_handle(Socket));
}

//********************************************************************************************************************

void iocp_deregister_socket(WSW_SOCKET Socket)
{
   std::lock_guard<std::mutex> lock(glIocpMutex);
   auto record = glSockets.find(Socket);
   if (record != glSockets.end()) {
      record->second.Cancelled = true;
      record->second.Generation++;
      glSockets.erase(record);
   }
}

//********************************************************************************************************************

int iocp_shutdown_socket(WSW_SOCKET Socket, int How)
{
   return shutdown(socket_from_handle(Socket), How);
}

//********************************************************************************************************************

ERR iocp_prepare_connect(WSW_SOCKET Socket, const void *Address, int AddressSize)
{
   if ((!Address) or (AddressSize <= 0) or (AddressSize > int(IOCP_ENDPOINT_STORAGE_SIZE))) return ERR::Args;

   std::lock_guard<std::mutex> lock(glIocpMutex);
   auto record = glSockets.find(Socket);
   if (record IS glSockets.end()) return ERR::Search;
   if (record->second.Cancelled) return ERR::Cancelled;

   std::memcpy(record->second.ConnectAddress.data(), Address, size_t(AddressSize));
   record->second.ConnectAddressSize = AddressSize;
   record->second.ConnectResult = ERR::Busy;
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
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;
      if (record->second.ConnectAddressSize <= 0) return ERR::NotInitialised;

      record->second.Connect = { ObjectID, Callback };
      record->second.ConnectResult = ERR::Busy;

      address = record->second.ConnectAddress;
      address_size = record->second.ConnectAddressSize;
      generation = record->second.Generation;
      target = record->second.Connect;
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

   auto operation = new IocpOperation();
   operation->Type = IocpOperationType::CONNECT;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;

   DWORD bytes = 0;
   auto result = connect_ex(socket, remote_address, address_size, nullptr, 0, &bytes, &operation->Overlapped);
   if ((!result) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto connect_error = convert_error();
      delete operation;
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
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      result = record->second.ConnectResult;
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
   std::lock_guard<std::mutex> lock(glIocpMutex);
   auto record = glSockets.find(Socket);
   if (record IS glSockets.end()) return false;
   if (record->second.Cancelled) return false;
   return record->second.Generation IS Generation;
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
   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_register_accept(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::ACCEPT, ObjectID, Callback);
       error != ERR::Okay) return error;

   return post_accept(Socket);
}

//********************************************************************************************************************

ERR iocp_accept(WSW_SOCKET Server, WSW_SOCKET &Client, void *Address, int *AddressSize)
{
   Client = WSW_SOCKET(INVALID_SOCKET);
   if ((!Address) or (!AddressSize)) return ERR::NullArgs;

   bool rearm_accept = false;

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Server);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;
      if (record->second.AcceptedSockets.empty()) return ERR::Search;

      auto accepted = record->second.AcceptedSockets.front();
      record->second.AcceptedSockets.erase(record->second.AcceptedSockets.begin());

      Client = accepted.Socket;
      auto copy_size = std::min(*AddressSize, accepted.AddressSize);
      std::memcpy(Address, accepted.Address.data(), size_t(copy_size));
      *AddressSize = copy_size;

      auto target = record->second.Accept;
      rearm_accept = (target.Callback) and (target.ObjectID > 0) and (!record->second.AcceptPending);
   }

   if (rearm_accept) post_accept(Server);
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
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      udp = record->second.UDP;
      buffered = udp ? !record->second.Datagrams.empty() : !record->second.ReadBuffer.empty();
      terminal = record->second.ReadResult != ERR::Okay;
   }

   if ((buffered) or (terminal)) {
      return queue_record_completion(Socket, udp ? IocpOperationType::UDP_RECEIVE : IocpOperationType::READ);
   }
   else return post_read(Socket);
}

//********************************************************************************************************************

ERR iocp_register_write(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::WRITE, ObjectID, Callback);
       error != ERR::Okay) return error;

   bool pending = false;
   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      pending = record->second.WritePending;
   }

   return pending ? ERR::Okay : queue_record_completion(Socket, IocpOperationType::WRITE);
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

ERR iocp_recall_read(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::READ, ObjectID, Callback);
       error != ERR::Okay) return error;

   bool udp = false;
   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      udp = record->second.UDP;
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
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;

      auto available = record->second.ReadBuffer.size() - record->second.ReadOffset;
      if (available > 0) {
         Received = std::min(Length, available);
         std::memcpy(Buffer, record->second.ReadBuffer.data() + record->second.ReadOffset, Received);
         record->second.ReadOffset += Received;

         if (record->second.ReadOffset >= record->second.ReadBuffer.size()) {
            record->second.ReadBuffer.clear();
            record->second.ReadOffset = 0;
            if (record->second.ReadResult IS ERR::Okay) rearm_read = true;
            else result = record->second.ReadResult;
         }
         else recall_read = true;
      }
      else {
         result = record->second.ReadResult;
         if ((result IS ERR::Okay) and (!record->second.ReadPending)) rearm_read = true;
      }
   }

   if (rearm_read) {
      if (auto error = post_read(Socket); error != ERR::Okay) return error;
   }
   else if (recall_read) {
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

   IocpCompletionTarget target;
   uint64_t generation = 0;
   size_t requested = std::min<size_t>(Length, 0x7fffffff);

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;
      if (record->second.WritePending) {
         Length = 0;
         return ERR::BufferOverflow;
      }

      record->second.WritePending = true;
      target = record->second.Write;
      generation = record->second.Generation;
   }

   auto operation = new IocpOperation();
   operation->Type = IocpOperationType::WRITE;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;
   operation->BufferSize = requested;
   operation->Buffer = std::make_unique<uint8_t[]>(operation->BufferSize);
   std::memcpy(operation->Buffer.get(), Buffer, requested);

   WSABUF wsabuf;
   wsabuf.buf = (CHAR *)operation->Buffer.get();
   wsabuf.len = ULONG(operation->BufferSize);

   DWORD bytes = 0;
   auto result = WSASend(socket_from_handle(Socket), &wsabuf, 1, &bytes, 0, &operation->Overlapped, nullptr);
   if ((result IS SOCKET_ERROR) and (WSAGetLastError() != WSA_IO_PENDING)) {
      auto error = convert_error();
      {
         std::lock_guard<std::mutex> lock(glIocpMutex);
         auto record = glSockets.find(Socket);
         if (record != glSockets.end()) record->second.WritePending = false;
      }
      delete operation;
      Length = 0;
      return error;
   }

   Length = requested;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR iocp_send_to(WSW_SOCKET Socket, const void *Buffer, size_t &Length, const void *Address, int AddressSize)
{
   if ((!Buffer) and (Length > 0)) return ERR::NullArgs;
   if ((!Address) or (AddressSize <= 0)) return ERR::Args;
   if (!Length) return ERR::Okay;

   auto result = sendto(socket_from_handle(Socket), (const char *)Buffer, int(Length), 0, (const sockaddr *)Address,
      AddressSize);
   if (result >= 0) {
      Length = size_t(result);
      return ERR::Okay;
   }

   Length = 0;
   switch (WSAGetLastError()) {
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
         return convert_error();
   }
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
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;

      if (!record->second.Datagrams.empty()) {
         datagram = std::move(record->second.Datagrams.front());
         record->second.Datagrams.erase(record->second.Datagrams.begin());
         has_datagram = true;
         recall_read = !record->second.Datagrams.empty();
         if ((!recall_read) and (record->second.ReadResult IS ERR::Okay)) rearm_read = true;
      }
      else {
         result = record->second.ReadResult;
         if ((result IS ERR::Okay) and (!record->second.ReadPending)) rearm_read = true;
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
