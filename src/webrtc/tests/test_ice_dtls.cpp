#include "../ice/ice_dtls.h"
#include "../stun/stun.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
#include <thread>
#include <vector>

using namespace rtc;

static int fail(const char *Message) {
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

static std::vector<uint8_t> check_request(bool Nominate) {
   std::array<uint8_t, 12> tx{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
   std::array<uint8_t, 4> priority{ 0x6e, 0, 1, 0xff };
   std::array<uint8_t, 8> controlling{ 1, 2, 3, 4, 5, 6, 7, 8 };
   std::string username = "local:remote";
   std::string password = "local-password-0123456789";
   std::array<stun::Attribute, 4> attributes{
      stun::Attribute{ stun::ATTR_USERNAME,
         std::span<const uint8_t>((const uint8_t *)username.data(), username.size()), 0 },
      stun::Attribute{ 0x0024, priority, 0 },
      stun::Attribute{ 0x802a, controlling, 0 },
      stun::Attribute{ 0x0025, std::span<const uint8_t>(), 0 }
   };
   auto key = std::span<const uint8_t>((const uint8_t *)password.data(), password.size());
   std::vector<uint8_t> result;
   if (not stun::encode(stun::BINDING_REQUEST, tx,
       std::span<const stun::Attribute>(attributes.data(), Nominate ? 4u : 3u),
       key, false, true, result)) return {};
   return result;
}

int main() {
   std::shared_ptr<DtlsCertificate> client_cert, server_cert;
   if (DtlsCertificate::generate(client_cert) != ERR::Okay or
       DtlsCertificate::generate(server_cert) != ERR::Okay) return fail("Certificate generation failed");
   ice::Endpoint source;
   source.address = { 192, 0, 2, 1 };
   source.port = 50000;
   auto stranger = source;
   stranger.port = 50001;

   std::deque<std::vector<uint8_t>> client_out, server_out;
   bool wrong_destination = false;
   bool received_application = false;
   IceDtlsCallbacks server_callbacks;
   server_callbacks.send = [&](std::span<const uint8_t> Data, const ice::Endpoint &Destination) {
      if (Destination != source) wrong_destination = true;
      server_out.emplace_back(Data.begin(), Data.end());
   };
   server_callbacks.receive = [&](std::span<const uint8_t>) { received_application = true; };
   IceDtlsTransport server(server_cert, "local", "local-password-0123456789", "remote", server_callbacks);
   if (server.set_remote_fingerprint(client_cert->fingerprint()) != ERR::Okay) {
      return fail("Server fingerprint setup failed");
   }
   DtlsCallbacks client_callbacks;
   client_callbacks.send = [&](std::span<const uint8_t> Data) {
      client_out.emplace_back(Data.begin(), Data.end());
   };
   DtlsTransport client(client_cert, DtlsRole::CLIENT, client_callbacks);
   if (client.set_remote_fingerprint("sha-256", server_cert->fingerprint()) != ERR::Okay) {
      return fail("Client fingerprint setup failed");
   }

   auto check = check_request(false);
   if (server.feed(check, source) != ERR::Okay or not server.validated(source) or server.nominated() or
       server_out.size() != 1) return fail("ICE check did not validate source before nomination");
   server_out.clear(); // Simulate a lost first response.
   if (server.feed(check, source) != ERR::Okay or server_out.size() != 1) return fail("ICE retransmit failed");
   server_out.clear();
   if (client.start() != ERR::Okay or client_out.empty()) return fail("Client DTLS start failed");

   // A ClientHello from an unvalidated source must not start the server transport.
   if (server.feed(client_out.front(), stranger) != ERR::Okay or server.state() != DtlsState::NEW) {
      return fail("Unvalidated DTLS source was accepted");
   }

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
   while (std::chrono::steady_clock::now() < deadline) {
      while (not client_out.empty()) {
         auto datagram = std::move(client_out.front());
         client_out.pop_front();
         if (server.feed(datagram, source) != ERR::Okay) return fail("Server DTLS feed failed");
      }
      while (not server_out.empty()) {
         auto datagram = std::move(server_out.front());
         server_out.pop_front();
         if (client.feed(datagram) != ERR::Okay) return fail("Client DTLS feed failed");
      }
      if (server.state() IS DtlsState::CONNECTED and client.state() IS DtlsState::CONNECTED) break;
      if (server.handle_timeout() != ERR::Okay or client.handle_timeout() != ERR::Okay) {
         return fail("DTLS retransmission failed");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   if (server.state() != DtlsState::CONNECTED or client.state() != DtlsState::CONNECTED or wrong_destination) {
      return fail("ICE-authenticated DTLS handshake did not complete");
   }
   if (server.feed(check_request(true), source) != ERR::Okay or not server.nominated() or
       *server.nominated() != source) return fail("Late nomination failed");
   server_out.clear();
   std::array<uint8_t, 3> payload{ 1, 2, 3 };
   if (client.send(payload) != ERR::Okay) return fail("Client application send failed");
   while (not client_out.empty()) {
      if (server.feed(client_out.front(), source) != ERR::Okay) return fail("Application receive failed");
      client_out.pop_front();
   }
   if (not received_application) return fail("Application record was not delivered");
   return 0;
}
