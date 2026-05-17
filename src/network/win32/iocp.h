#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <kotuku/system/errors.h>

typedef uintptr_t WSW_SOCKET;
typedef ERR (*iocp_post_message)(int MsgID, const void *Message, int Size);

static constexpr size_t IOCP_ENDPOINT_STORAGE_SIZE = 128;

enum class IocpOperationType : uint8_t {
   CONNECT,
   READ,
   WRITE,
   ACCEPT,
   UDP_RECEIVE,
   UDP_SEND
};

struct iocp_completion_message {
   IocpOperationType Type = IocpOperationType::CONNECT;
   WSW_SOCKET Socket = 0;
   uint64_t Generation = 0;
   int ObjectID = 0;
   uintptr_t Callback = 0;
   size_t BytesTransferred = 0;
   ERR Error = ERR::NIL;
};

ERR iocp_initialise(int MsgID, iocp_post_message PostMessage);
void iocp_expunge();

WSW_SOCKET iocp_create_socket(int ObjectID, bool UDP, bool &IPv6);
void iocp_close_socket(WSW_SOCKET Socket);
void iocp_deregister_socket(WSW_SOCKET Socket);
int iocp_shutdown_socket(WSW_SOCKET Socket, int How);
void iocp_set_socket_object(WSW_SOCKET Socket, int ObjectID);

ERR iocp_prepare_connect(WSW_SOCKET Socket, const void *Address, int AddressSize);
ERR iocp_begin_connect_wait(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback);
ERR iocp_complete_connect(WSW_SOCKET Socket);
bool iocp_validate_completion(WSW_SOCKET Socket, uint64_t Generation);

ERR iocp_bind(WSW_SOCKET Socket, const void *Address, int AddressSize);
ERR iocp_listen(WSW_SOCKET Socket, int Backlog);
ERR iocp_register_accept(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback);
ERR iocp_accept(WSW_SOCKET Server, WSW_SOCKET &Client, void *Address, int *AddressSize);

ERR iocp_register_read(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback);
ERR iocp_register_write(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback);
ERR iocp_remove_read(WSW_SOCKET Socket);
ERR iocp_remove_write(WSW_SOCKET Socket);
ERR iocp_recall_read(WSW_SOCKET Socket, int ObjectID, uintptr_t Callback);
bool iocp_has_pending_write(WSW_SOCKET Socket);

ERR iocp_receive(WSW_SOCKET Socket, void *Buffer, size_t Length, size_t &Received);
ERR iocp_append_receive(WSW_SOCKET Socket, std::vector<uint8_t> &Buffer, size_t Length, size_t &Received);
ERR iocp_send(WSW_SOCKET Socket, const void *Buffer, size_t &Length);
ERR iocp_send_to(WSW_SOCKET Socket, const void *Buffer, size_t &Length, const void *Address, int AddressSize);
ERR iocp_receive_from(WSW_SOCKET Socket, void *Buffer, size_t BufferSize, size_t &BytesRead, void *Address,
   int *AddressSize);

ERR iocp_get_local_ip(WSW_SOCKET Socket, void *Address, int *AddressSize);
ERR iocp_enable_keep_alive(WSW_SOCKET Socket);
ERR iocp_enable_broadcast(WSW_SOCKET Socket);
ERR iocp_set_multicast_ttl(WSW_SOCKET Socket, int TTL, bool IPv6);
ERR iocp_parse_multicast_group(const char *Group, bool &IPv6);
ERR iocp_join_multicast_group(WSW_SOCKET Socket, const char *Group, bool IPv6);
ERR iocp_leave_multicast_group(WSW_SOCKET Socket, const char *Group, bool IPv6);

uint32_t iocp_htonl(uint32_t Value);
uint32_t iocp_ntohl(uint32_t Value);
uint16_t iocp_htons(uint16_t Value);
uint16_t iocp_ntohs(uint16_t Value);
