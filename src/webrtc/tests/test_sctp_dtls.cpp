// Real OpenSSL records with two SCTP associations, without sockets or object-system dependencies.
#include "../dtls/dtls_transport.h"
#include "../sctp/sctp_association.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <utility>

using namespace rtc;
namespace {
uint64_t now_us() {
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Endpoint {
   std::unique_ptr<DtlsTransport> dtls;
   SctpAssociation association;
   std::vector<std::vector<uint8_t>> packets;
   ERR error = ERR::Okay;
   bool started = false;
   bool inside_receive = false;
   bool reentered = false;
   Endpoint(uint16_t Local, uint16_t Remote) : association([&] {
      SctpConfig config;
      config.local_port = Local;
      config.remote_port = Remote;
      return config;
   }()) { }
   bool attach(DtlsRole Role, const std::shared_ptr<DtlsCertificate> &Certificate, std::string_view Fingerprint) {
      DtlsCallbacks callbacks;
      callbacks.send = [this](std::span<const uint8_t> Packet) { packets.emplace_back(Packet.begin(), Packet.end()); };
      callbacks.receive = [this](std::span<const uint8_t> Packet) {
         inside_receive = true;
         error = association.feed(Packet, now_us());
         inside_receive = false;
      };
      dtls = std::make_unique<DtlsTransport>(Certificate, Role, callbacks);
      dtls->set_initial_timeout(20);
      return dtls->set_remote_fingerprint("sha-256", Fingerprint) IS ERR::Okay and dtls->start() IS ERR::Okay;
   }
   bool pump() {
      if (dtls->state() IS DtlsState::CONNECTED) {
         if (not started) {
            started = true;
            if (association.state() IS SctpState::CLOSED) error = association.start(now_us());
         }
         if (error != ERR::Okay or association.tick(now_us()) != ERR::Okay) return false;
         for (auto &packet : association.take_output()) {
            reentered |= inside_receive;
            if (packet.size() > size_t(dtls->max_payload()) or dtls->send(packet) != ERR::Okay) return false;
         }
      }
      if (dtls->timeout_us() IS 0 and dtls->handle_timeout() != ERR::Okay) return false;
      return true;
   }
};
}
int main() {
   std::shared_ptr<DtlsCertificate> first, second;
   if (DtlsCertificate::generate(first) != ERR::Okay or DtlsCertificate::generate(second) != ERR::Okay) return 1;
   Endpoint client(5000, 6000), server(6000, 5000);
   if (not client.attach(DtlsRole::CLIENT, first, second->fingerprint()) or
       not server.attach(DtlsRole::SERVER, second, first->fingerprint())) return 2;
   auto limit = now_us() + 5000000;
   bool sent = false;
   std::vector<uint8_t> expected(65536);
   for (size_t i = 0; i < expected.size(); ++i) expected[i] = uint8_t(i * 17);
   bool client_received = false, server_received = false;
   while (now_us() < limit) {
      for (auto &packet : std::exchange(client.packets, {})) if (server.dtls->feed(packet) != ERR::Okay) return 3;
      for (auto &packet : std::exchange(server.packets, {})) if (client.dtls->feed(packet) != ERR::Okay) return 4;
      if (not client.pump() or not server.pump()) return 5;
      if (client.association.state() IS SctpState::ESTABLISHED and
          server.association.state() IS SctpState::ESTABLISHED) {
         if (not sent) {
            sent = true;
            if (client.association.enqueue(0, 53, expected, false, now_us()) != ERR::Okay or
                server.association.enqueue(1, 53, expected, true, now_us()) != ERR::Okay) return 8;
         }
         for (auto &message : client.association.take_messages()) {
            if (client_received or message.payload != expected or not message.unordered) return 9;
            client_received = true;
         }
         for (auto &message : server.association.take_messages()) {
            if (server_received or message.payload != expected or message.unordered) return 10;
            server_received = true;
         }
         if (client_received and server_received) {
            if (client.reentered or server.reentered) return 6;
            std::puts("Reliable SCTP messages exchanged over authenticated OpenSSL DTLS");
            return 0;
         }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
   }
   return 7;
}
