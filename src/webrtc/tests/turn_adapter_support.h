// Production TCP/TLS adapter exercised against a deterministic, task-local fake server.
#pragma once
#include "turn_test_support.h"
#include "../turn/turn_socket.h"
#include <chrono>
#include <openssl/ssl.h>
#include <openssl/err.h>
#ifndef _WIN32
#include <netinet/tcp.h>
#endif

namespace rtc::test {
inline bool nonblocking(Socket SocketValue) {
#ifdef _WIN32
   u_long enabled = 1;
   return ioctlsocket(SocketValue, FIONBIO, &enabled) IS 0;
#else
   return fcntl(SocketValue, F_SETFL, O_NONBLOCK) IS 0;
#endif
}
inline uint64_t adapter_now() {
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
inline bool adapter_failure(int Line) {
   std::fprintf(stderr, "TURN adapter test failed at line %d\n", Line); return false;
}
inline bool turn_adapter_stall(bool Stall = true, bool CancelRefresh = false,
   const char *Certificate = nullptr, const char *Key = nullptr, bool CancelStall = false, bool ResumeStall = false) {
   SSL_CTX *tls_context = Certificate ? SSL_CTX_new(TLS_server_method()) : nullptr;
   auto release_context = kt::Defer([&]() { if (tls_context) SSL_CTX_free(tls_context); });
   if (Certificate and (not tls_context or SSL_CTX_use_certificate_chain_file(tls_context, Certificate) != 1 or
       SSL_CTX_use_PrivateKey_file(tls_context, Key, SSL_FILETYPE_PEM) != 1)) return adapter_failure(__LINE__);
   SSL *tls = nullptr;
   auto release_tls = kt::Defer([&]() { if (tls) SSL_free(tls); });
   bool handshake = false;
   const char *transport = Certificate ? "TLS" : "TCP";
   auto listener = ::socket(AF_INET, SOCK_STREAM, 0);
   if (listener IS INVALID_SOCKET_VALUE) return adapter_failure(__LINE__);
   auto release_listener = kt::Defer([&]() { close_socket(listener); });
   sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (::bind(listener, (sockaddr *)&address, sizeof(address)) != 0 or ::listen(listener, 1) != 0 or
       not nonblocking(listener)) return adapter_failure(__LINE__);
   AddressSize address_size = sizeof(address);
   if (::getsockname(listener, (sockaddr *)&address, &address_size) != 0) return adapter_failure(__LINE__);
   rtc::IceServer options;
   options.host = Certificate ? "localhost" : "127.0.0.1"; options.port = ntohs(address.sin_port);
   options.username = "user"; options.credential = "pass";
   options.transport = Certificate ? rtc::ServerTransport::TLS : rtc::ServerTransport::TCP;
   ice::Endpoint endpoint; endpoint.address = {127, 0, 0, 1}; endpoint.port = options.port;
   rtc::TurnSocket adapter(options, {endpoint, 1, 1});
   if (adapter.open(adapter_now()) != ERR::Okay) return adapter_failure(__LINE__);
   auto incoming = INVALID_SOCKET_VALUE;
   auto release_incoming = kt::Defer([&]() { if (incoming != INVALID_SOCKET_VALUE) close_socket(incoming); });
   turn::StreamDecoder decoder;
   unsigned grants = 0, refreshes = 0, data_frames = 0, denials = 0;
   uint64_t wire_received = 0, wire_sent = 0;
   auto deadline = adapter_now() + 10000000;
   uint64_t pump_time = 0;
   auto pump = [&]() {
      ProcessMessages(PMF::NIL, 1);
      if (incoming IS INVALID_SOCKET_VALUE) {
         incoming = ::accept(listener, nullptr, nullptr);
         if (incoming != INVALID_SOCKET_VALUE) {
            if (not nonblocking(incoming)) return adapter_failure(__LINE__);
            int no_delay = 1;
            if (::setsockopt(incoming, IPPROTO_TCP, TCP_NODELAY, (const char *)&no_delay, sizeof(no_delay)) != 0) {
               return adapter_failure(__LINE__);
            }
         }
      }
      if (incoming != INVALID_SOCKET_VALUE) {
         std::array<uint8_t, 16384> bytes{};
         if (Certificate and not tls) {
            tls = SSL_new(tls_context);
            if (not tls or SSL_set_fd(tls, int(incoming)) != 1) return adapter_failure(__LINE__);
         }
         if (tls and not handshake) {
            auto result = SSL_accept(tls);
            if (result IS 1) handshake = true;
            else {
               auto error = SSL_get_error(tls, result);
               if (error != SSL_ERROR_WANT_READ and error != SSL_ERROR_WANT_WRITE) {
                  return adapter_failure(__LINE__);
               }
               adapter.tick(pump_time ? pump_time : adapter_now());
               return adapter.failure() IS ERR::Okay;
            }
         }
         // Drain ready TLS records in bounded batches; SSL_read can return one small record per call.
         // Sleeping after each record makes resumption depend on the host timer resolution.
         size_t drained = 0;
         for (unsigned reads = 0; reads < 32 and drained < 65536; ++reads) {
            if (tls) ERR_clear_error();
            auto capacity = std::min(bytes.size(), size_t(65536 - drained));
            auto count = tls ? SSL_read(tls, bytes.data(), int(capacity)) :
               receive(incoming, bytes.data(), capacity, 0);
            if (tls and count <= 0) {
               auto error = SSL_get_error(tls, count);
               if (error != SSL_ERROR_WANT_READ and error != SSL_ERROR_WANT_WRITE) {
                  std::fprintf(stderr, "TURN fixture TLS read error %d: %s\n", error,
                     ERR_error_string(ERR_get_error(), nullptr));
                  return adapter_failure(__LINE__);
               }
            }
            if (count <= 0) break;
            drained += size_t(count);
            wire_received += uint64_t(count);
            size_t consumed = 0; std::vector<std::vector<uint8_t>> records;
            if (not decoder.feed({bytes.data(), size_t(count)}, consumed, records) or consumed != size_t(count)) {
               return adapter_failure(__LINE__);
            }
            for (auto &wire : records) {
               if (not wire.empty() and (wire[0] & 0xc0) IS 0x40) { ++data_frames; continue; }
               turn::Packet request{{}, true, std::move(wire)};
               auto message = turn_test::parse(request);
               if (message.type IS turn::SEND_INDICATION) { ++data_frames; continue; }
               auto response = message.type IS turn::ALLOCATE and
                  not message.find(stun::ATTR_USERNAME) ?
                  turn_test::challenge(request) : turn_test::success(request);
               ice::Endpoint requested_peer;
               auto peer_address = message.find(turn::ATTR_XOR_PEER_ADDRESS);
               if (peer_address and turn::decode_xor_address(*peer_address, message, requested_peer) and
                   requested_peer IS turn_test::endpoint(9, 9000)) {
                  std::array<uint8_t, 4> code{0, 0, 4, 3};
                  stun::Attribute forbidden{stun::ATTR_ERROR_CODE, code};
                  if (not stun::encode(message.type | 0x0110, message.transaction_id, std::span(&forbidden, 1),
                      turn_test::key(), false, true, response)) return adapter_failure(__LINE__);
                  ++denials;
               }
               if (message.type IS turn::REFRESH) ++refreshes;
               if (message.type IS turn::CHANNEL_BIND or message.type IS turn::CREATE_PERMISSION) ++grants;
               auto sent = tls ? SSL_write(tls, response.data(), int(response.size())) :
                  send(incoming, response.data(), response.size(), 0);
               if (sent != int(response.size())) {
                  return adapter_failure(__LINE__);
               }
               wire_sent += response.size();
            }
         }
      }
      adapter.tick(pump_time ? pump_time : adapter_now()); return adapter.failure() IS ERR::Okay;
   };
   while (not adapter.active() and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   if (not adapter.active()) return adapter_failure(__LINE__);
   auto peer = turn_test::endpoint(8, 9000);
   adapter.permit(peer, adapter_now());
   while (grants < 2 and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   for (int i = 0; i < 10; ++i) { ProcessMessages(PMF::NIL, 1); adapter.tick(pump_time ? pump_time : adapter_now()); }
   if (grants < 2) return adapter_failure(__LINE__);

   std::array<uint8_t, 1> probe{};
   if (not adapter.send(peer, probe, adapter_now())) return adapter_failure(__LINE__);
   while (data_frames < 1 and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   if (data_frames < 1) return adapter_failure(__LINE__);
   auto now = adapter_now() + 240000000;
   adapter.tick(now);
   while (grants < 3 and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   for (int i = 0; i < 10; ++i) { ProcessMessages(PMF::NIL, 1); adapter.tick(now); }
   if (not adapter.send(peer, probe, now)) return adapter_failure(__LINE__);
   while (data_frames < 2 and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   if (data_frames < 2) return adapter_failure(__LINE__);
   now += 240000000;
   adapter.tick(now);
   while (not refreshes and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   for (int i = 0; i < 10; ++i) { ProcessMessages(PMF::NIL, 1); adapter.tick(now); }
   if (not refreshes or not adapter.active()) return adapter_failure(__LINE__);
   if (not adapter.send(peer, probe, now)) return adapter_failure(__LINE__);
   while (data_frames < 3 and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   if (data_frames < 3) return adapter_failure(__LINE__);
   for (int i = 0; i < 10; ++i) if (not pump()) return adapter_failure(__LINE__);
   // A server-denied candidate must release its queued packet without invalidating another usable peer.
   auto before_denial = adapter.available();
   auto denied_peer = turn_test::endpoint(9, 9000);
   if (not adapter.send(denied_peer, probe, now)) return adapter_failure(__LINE__);
   while (denials < 2 and adapter_now() < deadline) if (not pump()) return adapter_failure(__LINE__);
   for (int i = 0; i < 10; ++i) if (not pump()) return adapter_failure(__LINE__);
   if (denials != 2 or not adapter.active() or adapter.available() != before_denial) {
      std::fprintf(stderr, "Denied peer retained queue: %zu/%zu room, %s\n", adapter.available(),
         before_denial, adapter.failure_reason());
      return adapter_failure(__LINE__);
   }
   if (not adapter.send(denied_peer, probe, now) or adapter.available() != before_denial) {
      return adapter_failure(__LINE__);
   }
   if (adapter.counters.turn_sent_bytes != wire_received or adapter.counters.turn_received_bytes != wire_sent)
      return adapter_failure(__LINE__);
   if (CancelRefresh) {
      adapter.tick(now + 480000000);
      if (not adapter.active()) return adapter_failure(__LINE__);
      std::printf("TURN %s: close while authenticated allocation refresh remains outstanding\n", transport);
      return true;
   }
   if (not Stall) {
      close_socket(incoming); incoming = INVALID_SOCKET_VALUE;
      while (adapter.failure() IS ERR::Okay and adapter_now() < deadline) {
         ProcessMessages(PMF::NIL, 1); adapter.tick(now);
      }
      if (adapter.failure() != ERR::Disconnected) return adapter_failure(__LINE__);
      std::printf("TURN %s: allocation/permission/channel renewal, server disappearance, immediate cleanup\n",
         transport);
      return true;
   }

   // Stop reading.  Fill the kernel send buffer and then the adapter/Network queues until admission is refused.
   std::array<uint8_t, 1200> payload{};
   size_t accepted = 0;
   while (accepted < 100000 and adapter.send(peer, payload, now)) ++accepted;
   if (not accepted or accepted IS 100000 or adapter.failure() != ERR::Okay or adapter.available() >= 1328) {
      std::fprintf(stderr, "Admission: %zu accepted, %zu room, error %d\n",
         accepted, adapter.available(), int(adapter.failure()));
      return adapter_failure(__LINE__);
   }
   auto room = adapter.available();
   if (adapter.send(peer, payload, now) or adapter.available() != room) return adapter_failure(__LINE__);
   // A timer snapshot can precede a send callback's progress timestamp.  It must not wrap the stall age.
   for (int i = 0; i < 8; ++i) adapter.tick(now - 1);
   if (adapter.failure() != ERR::Okay) {
      std::fprintf(stderr, "Stale timer snapshot: %s\n", adapter.failure_reason());
      return adapter_failure(__LINE__);
   }
   if (ResumeStall) {
      pump_time = now;
      auto resume_start = adapter_now();
      deadline = resume_start + 10000000;
      while (data_frames < accepted + 3 and adapter_now() < deadline) {
         if (not pump()) return adapter_failure(__LINE__);
      }
      if (data_frames != accepted + 3 or not adapter.active()) {
         std::fprintf(stderr, "Resume: %u/%zu frames, room %zu, error %d\n",
            data_frames, accepted + 3, adapter.available(), int(adapter.failure()));
         std::fprintf(stderr, "Resume bytes: received=%llu accepted=%llu decoder=%zu TLS-pending=%d\n",
            (unsigned long long)wire_received, (unsigned long long)adapter.counters.turn_sent_bytes,
            decoder.buffered(), tls ? SSL_pending(tls) : 0);
         return adapter_failure(__LINE__);
      }
      // All accepted application frames must survive back-pressure; admission must reopen after draining.
      for (int i = 0; i < 10; ++i) if (not pump()) return adapter_failure(__LINE__);
      if (adapter.available() < 1328 or not adapter.send(peer, probe, now)) return adapter_failure(__LINE__);
      std::printf("TURN %s: resumed reads delivered all %zu admitted datagrams and reopened admission in %.3f ms\n",
         transport, accepted, double(adapter_now() - resume_start) / 1000.0);
      return true;
   }
   if (CancelStall) {
      std::printf("TURN %s: close with saturated stream queues, %zu admitted datagrams\n", transport, accepted);
      return true;
   }
   // Permission and allocation maintenance have reserved queue space even while application admission is closed.
   // With a stalled stream, a finite deadline wins over indefinite buffering.
   adapter.tick(now + 6000000);
   if (adapter.failure() != ERR::Timeout) return adapter_failure(__LINE__);
   std::printf("TURN %s stall: %zu datagrams admitted, bounded refusal, finite timeout and immediate cleanup\n",
      transport, accepted);
   return true;
}
}
