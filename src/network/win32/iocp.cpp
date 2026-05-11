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
   uintptr_t Data = 0;
   std::unique_ptr<uint8_t[]> Buffer;
   size_t BufferSize = 0;
   size_t BytesTransferred = 0;
   ERR Result = ERR::Busy;
};

struct IocpCompletionTarget {
   int ObjectID = 0;
   uintptr_t Callback = 0;
   uintptr_t Data = 0;
};

struct IocpSocketRecord {
   void *Reference = nullptr;
   IocpCompletionTarget Connect;
   IocpCompletionTarget Read;
   IocpCompletionTarget Write;
   IocpCompletionTarget Accept;
   std::vector<uint8_t> ReadBuffer;
   size_t ReadOffset = 0;
   std::array<uint8_t, IOCP_ENDPOINT_STORAGE_SIZE> ConnectAddress = {};
   int ConnectAddressSize = 0;
   ERR ConnectResult = ERR::NotInitialised;
   ERR ReadResult = ERR::Okay;
   uint64_t Generation = 0;
   bool Cancelled = false;
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

//********************************************************************************************************************

static IocpCompletionTarget completion_target(const IocpSocketRecord &Record, IocpOperationType Type)
{
   if (Type IS IocpOperationType::CONNECT) return Record.Connect;
   else if (Type IS IocpOperationType::READ) return Record.Read;
   else if (Type IS IocpOperationType::WRITE) return Record.Write;
   else if (Type IS IocpOperationType::ACCEPT) return Record.Accept;
   else return {};
}

//********************************************************************************************************************

static ERR convert_error(int Error = 0)
{
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

static void store_operation_result(IocpSocketRecord &Record, const IocpOperation &Operation, ERR Error)
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
}

//********************************************************************************************************************

static void queue_operation_completion(const IocpOperation &Operation, size_t BytesTransferred, ERR Error)
{
   iocp_completion_message message;

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Operation.Socket);
      if (record IS glSockets.end()) return;
      if ((record->second.Cancelled) or (record->second.Generation != Operation.Generation)) return;

      store_operation_result(record->second, Operation, Error);
      auto target = completion_target(record->second, Operation.Type);

      message.Type = Operation.Type;
      message.Socket = Operation.Socket;
      message.Generation = Operation.Generation;
      message.ObjectID = target.ObjectID;
      message.Callback = target.Callback;
      message.Data = target.Data;
      message.BytesTransferred = BytesTransferred;
      message.Error = Error;
   }

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
      message.Data = target.Data;
      message.Error = ERR::Okay;
   }

   if (glPostMessage) return glPostMessage(glCompletionMsgID, &message, sizeof(message));
   else return ERR::NotInitialised;
}

//********************************************************************************************************************

static ERR post_read(WSW_SOCKET Socket)
{
   IocpCompletionTarget target;
   uint64_t generation = 0;

   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      if (record->second.Cancelled) return ERR::Cancelled;
      if (record->second.ReadPending) return ERR::Okay;
      if (!record->second.ReadBuffer.empty()) return ERR::Okay;
      if (record->second.ReadResult != ERR::Okay) return ERR::Okay;

      target = record->second.Read;
      if ((!target.Callback) or (target.ObjectID <= 0)) return ERR::Okay;

      record->second.ReadPending = true;
      generation = record->second.Generation;
   }

   auto operation = new IocpOperation();
   operation->Type = IocpOperationType::READ;
   operation->Socket = Socket;
   operation->Generation = generation;
   operation->ObjectID = target.ObjectID;
   operation->Callback = target.Callback;
   operation->Data = target.Data;
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

static ERR set_completion_target(WSW_SOCKET Socket, IocpOperationType Type, int ObjectID, uintptr_t Callback,
   uintptr_t Data)
{
   std::lock_guard<std::mutex> lock(glIocpMutex);
   auto record = glSockets.find(Socket);
   if (record IS glSockets.end()) return ERR::Search;
   if (record->second.Cancelled) return ERR::Cancelled;

   auto target = IocpCompletionTarget { ObjectID, Callback, Data };
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

WSW_SOCKET iocp_create_socket(void *Reference, bool UDP, bool &IPv6)
{
   if ((UDP) or (glCompletionPort IS INVALID_HANDLE_VALUE)) {
      IPv6 = false;
      return WSW_SOCKET(INVALID_SOCKET);
   }

   SOCKET socket = WSASocket(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
   IPv6 = true;

   if (socket != INVALID_SOCKET) {
      DWORD v6only = 0;
      setsockopt(socket, IPPROTO_IPV6, IPV6_V6ONLY, (char *)&v6only, sizeof(v6only));
   }
   else {
      socket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
      IPv6 = false;
   }

   if (socket IS INVALID_SOCKET) return WSW_SOCKET(INVALID_SOCKET);

   DWORD nodelay = 1;
   setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (char *)&nodelay, sizeof(nodelay));

   auto handle = CreateIoCompletionPort((HANDLE)socket, glCompletionPort, ULONG_PTR(socket), 0);
   if (handle IS nullptr) {
      closesocket(socket);
      return WSW_SOCKET(INVALID_SOCKET);
   }

   WSW_SOCKET result = handle_from_socket(socket);

   std::lock_guard<std::mutex> lock(glIocpMutex);
   glSockets[result] = {
      .Reference = Reference,
      .Generation = glNextGeneration++,
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

ERR iocp_begin_connect_wait(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback, uintptr_t Data)
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

      record->second.Connect = { ObjectID, Callback, Data };
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
   failed_operation.Data = target.Data;

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
   operation->Data = target.Data;

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

ERR iocp_register_read(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback, uintptr_t Data)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::READ, ObjectID, Callback, Data);
       error != ERR::Okay) return error;

   bool buffered = false;
   bool terminal = false;
   {
      std::lock_guard<std::mutex> lock(glIocpMutex);
      auto record = glSockets.find(Socket);
      if (record IS glSockets.end()) return ERR::Search;
      buffered = !record->second.ReadBuffer.empty();
      terminal = record->second.ReadResult != ERR::Okay;
   }

   if ((buffered) or (terminal)) return queue_record_completion(Socket, IocpOperationType::READ);
   else return post_read(Socket);
}

//********************************************************************************************************************

ERR iocp_register_write(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback, uintptr_t Data)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::WRITE, ObjectID, Callback, Data);
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
   return set_completion_target(Socket, IocpOperationType::READ, 0, 0, 0);
}

//********************************************************************************************************************

ERR iocp_remove_write(WSW_SOCKET Socket)
{
   return set_completion_target(Socket, IocpOperationType::WRITE, 0, 0, 0);
}

//********************************************************************************************************************

ERR iocp_recall_read(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback, uintptr_t Data)
{
   if (auto error = set_completion_target(Socket, IocpOperationType::READ, ObjectID, Callback, Data);
       error != ERR::Okay) return error;

   return queue_record_completion(Socket, IocpOperationType::READ);
}

//********************************************************************************************************************

ERR iocp_receive(WSW_SOCKET Socket, void *Buffer, size_t Length, size_t &Received)
{
   Received = 0;
   if ((!Buffer) and (Length > 0)) return ERR::NullArgs;
   if (!Length) return ERR::Okay;

   ERR result = ERR::Okay;
   bool rearm_read = false;

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
      }
      else {
         result = record->second.ReadResult;
         if ((result IS ERR::Okay) and (!record->second.ReadPending)) rearm_read = true;
      }
   }

   if (rearm_read) {
      if (auto error = post_read(Socket); error != ERR::Okay) return error;
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
   operation->Data = target.Data;
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

ERR iocp_get_local_ip(WSW_SOCKET Socket, void *Address, int *AddressSize)
{
   if ((!Address) or (!AddressSize)) return ERR::NullArgs;
   return getsockname(socket_from_handle(Socket), (sockaddr *)Address, AddressSize) ? convert_error() : ERR::Okay;
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
