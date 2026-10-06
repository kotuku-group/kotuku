// Thin adapter over the Network object that carries a WebSocket connection.
//
// The WebSocket class reads, writes and watches for writability through this interface so that it holds one code path
// for both roles.  Closing is not part of the adapter because the safe way to release a socket depends on the role and
// on the callback that is running (see close_transport() in class_websocket.cpp).  Requires the Network module
// headers.

#pragma once

#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <cstdint>
#include <span>

namespace ws {

// Calls the Write action directly because the generated write() stub reports zero bytes for any error, including a
// BufferOverflow that has accepted part of the data.

inline ERR write_object(OBJECTPTR Object, std::span<const uint8_t> Data, int &Accepted) noexcept
{
   struct acWrite args = { std::span<const int8_t>((const int8_t *)Data.data(), Data.size()) };
   auto error = Action(AC::Write, Object, &args);
   Accepted = ((args.Result > 0) and (size_t(args.Result) <= Data.size())) ? args.Result : 0;
   return error;
}

class Transport {
public:
   virtual ~Transport() = default;

   // The Network object that carries the connection.

   [[nodiscard]] virtual OBJECTPTR object() const noexcept = 0;

   // Reads up to Buffer.size() bytes.  Length is zero if no data is available.  An error means that the connection
   // has been lost.

   virtual ERR read(std::span<uint8_t> Buffer, int &Length) noexcept = 0;

   // Writes Data, setting Accepted to the number of bytes that the transport sent or queued.  Bytes beyond Accepted
   // must be offered again.  An error other than BufferOverflow means that the connection has been lost.

   virtual ERR write(std::span<const uint8_t> Data, int &Accepted) noexcept = 0;

   // True while bytes accepted by write() are still held in the transport's own queue.

   [[nodiscard]] virtual bool busy() noexcept = 0;

   // Enables or disables the writability notification.  While enabled, the transport's outgoing callback is invoked
   // each time its own queue is empty and the connection can accept more data.

   virtual void watch_writes(bool Enable) noexcept = 0;

   // Removes every callback that refers to the WebSocket.

   virtual void detach() noexcept = 0;
};

//********************************************************************************************************************
// Client role: a NetSocket transferred from the HTTP object.  Writability is reported through NetSocket.Outgoing.
// The adapter does not own the socket.

class NetSocketTransport final : public Transport {
public:
   NetSocketTransport(objNetSocket *Socket, OBJECTPTR Owner, FUNCTION Incoming, FUNCTION Feedback,
      FUNCTION Outgoing) noexcept
      : socket(Socket), owner(Owner), outgoing(Outgoing)
   {
      kt::SwitchContext context(Owner); // Network invokes the callbacks within this context
      socket->setIncoming(Incoming);
      socket->setFeedback(Feedback);
      socket->ClientData = Owner;
   }

   ~NetSocketTransport() override { detach(); }

   [[nodiscard]] OBJECTPTR object() const noexcept override { return socket; }
   [[nodiscard]] objNetSocket * net_socket() const noexcept { return socket; }

   ERR read(std::span<uint8_t> Buffer, int &Length) noexcept override {
      Length = 0;
      return socket->read(std::span<int8_t>((int8_t *)Buffer.data(), Buffer.size()), &Length);
   }

   ERR write(std::span<const uint8_t> Data, int &Accepted) noexcept override {
      return write_object(socket, Data, Accepted);
   }

   [[nodiscard]] bool busy() noexcept override {
      int queued = 0;
      if (socket->getOutQueueSize(queued) != ERR::Okay) return false;
      return queued > 0;
   }

   void watch_writes(bool Enable) noexcept override {
      if (detached or (Enable IS watching)) return;
      watching = Enable;
      kt::SwitchContext context(owner);
      socket->setOutgoing(Enable ? outgoing : FUNCTION{});
   }

   void detach() noexcept override {
      if (detached) return;
      detached = true;
      socket->setIncoming(FUNCTION{});
      socket->setFeedback(FUNCTION{});
      if (watching) socket->setOutgoing(FUNCTION{});
      watching = false;
      socket->ClientData = nullptr;
   }

private:
   objNetSocket *socket;
   OBJECTPTR owner;
   FUNCTION outgoing;
   bool watching = false;
   bool detached = false;
};

//********************************************************************************************************************
// Server role: a ClientSocket accepted by the WebSocketServer's NetServer.  The NetServer's Incoming, Feedback and
// Outgoing callbacks belong to the WebSocketServer, which routes them to the WebSocket that owns the connection, so the
// adapter installs no callbacks of its own.  The adapter does not own the socket.
//
// watch_writes() has nothing to do.  The WebSocket only waits for writability while busy() is true, and Network
// registers a ClientSocket for writability whenever its queue holds data, calling the NetServer's Outgoing callback
// once the queue has been written.  The registration is removed again when that callback leaves the queue empty.

class ClientSocketTransport final : public Transport {
public:
   explicit ClientSocketTransport(objClientSocket *Socket) noexcept : socket(Socket) { }

   [[nodiscard]] OBJECTPTR object() const noexcept override { return socket; }
   [[nodiscard]] objClientSocket * client_socket() const noexcept { return socket; }

   ERR read(std::span<uint8_t> Buffer, int &Length) noexcept override {
      Length = 0;
      return socket->read(std::span<int8_t>((int8_t *)Buffer.data(), Buffer.size()), &Length);
   }

   ERR write(std::span<const uint8_t> Data, int &Accepted) noexcept override {
      return write_object(socket, Data, Accepted);
   }

   [[nodiscard]] bool busy() noexcept override {
      int queued = 0;
      if (socket->getOutQueueSize(queued) != ERR::Okay) return false;
      return queued > 0;
   }

   void watch_writes(bool Enable) noexcept override { }

   void detach() noexcept override { }

private:
   objClientSocket *socket;
};

} // namespace ws
