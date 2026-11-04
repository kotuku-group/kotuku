#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include <kotuku/startup.h>
#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/module.h>

#include "../ice/ice_socket.h"
#include "../sdp/sdp.h"
#include "../sctp/sctp_association.h"
#include "../sctp/dcep.h"

#include <openssl/rand.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>

JUMPTABLE_NETWORK
static OBJECTPTR glNetworkModule;
CSTRING ProgName = "WebRTCNativeSctpProbe";

static uint64_t now_us() {
   return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}

static std::string random_ice_text(size_t Length) {
   static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
   std::string result(Length, '\0');
   for (size_t i = 0; i < Length; ++i) {
      uint8_t value = 0;
      if (RAND_bytes(&value, 1) != 1) return {};
      result[i] = ALPHABET[value & 63];
   }
   return result;
}

static int run_probe() {
   std::string offer_text(std::istreambuf_iterator<char>{ std::cin }, {});
   rtc::sdp::Description offer;
   if (not rtc::sdp::parse(offer_text, offer) or offer.data.setup != rtc::sdp::Setup::ACTPASS) return 2;
   std::shared_ptr<rtc::DtlsCertificate> certificate;
   if (rtc::DtlsCertificate::generate(certificate) != ERR::Okay) return 3;
   auto ufrag = random_ice_text(8);
   auto password = random_ice_text(32);
   if (ufrag.empty() or password.empty()) return 4;

   bool client_role = std::getenv("KOTUKU_SCTP_DTLS_CLIENT") != nullptr;
   rtc::SctpConfig config;
   config.local_port = 6000;
   config.remote_port = offer.data.sctp_port;
   rtc::SctpAssociation association(config);
   rtc::SctpChannels channels(association, client_role);
   ERR receive_error = ERR::Okay;
   rtc::IceDtlsCallbacks callbacks;
   callbacks.receive = [&](std::span<const uint8_t> Packet) {
      std::fprintf(stderr, "SCTP_RX ");
      for (auto byte : Packet) std::fprintf(stderr, "%02x", byte);
      std::fprintf(stderr, "\n");
      receive_error = association.feed(Packet, now_us());
   };
   rtc::FullIceSocket socket(certificate, ufrag, password, rtc::ice::Role::CONTROLLED,
      client_role ? rtc::DtlsRole::CLIENT : rtc::DtlsRole::SERVER, callbacks);
   if (socket.set_remote_fingerprint(offer.data.fingerprint) != ERR::Okay or
       socket.open("127.0.0.1", 0) != ERR::Okay or
       not socket.set_remote_credentials(offer.data.ice_ufrag, offer.data.ice_pwd, offer.ice_lite)) return 5;
   rtc::sdp::Description answer;
   answer.session_id = "1";
   answer.ice_lite = false;
   answer.trickle = false;
   answer.data.mid = offer.data.mid;
   answer.data.ice_ufrag = ufrag;
   answer.data.ice_pwd = password;
   answer.data.fingerprint = certificate->fingerprint();
   answer.data.setup = client_role ? rtc::sdp::Setup::ACTIVE : rtc::sdp::Setup::PASSIVE;
   answer.data.sctp_port = config.local_port;
   answer.data.candidates.push_back("candidate:1 1 udp 2130706431 127.0.0.1 " +
      std::to_string(socket.local_address().port) + " typ host");
   answer.data.end_of_candidates = true;
   auto answer_text = rtc::sdp::serialize(answer);
   if (answer_text.empty() or not rtc::sdp::validate_answer(offer, answer)) return 6;
   std::cout << answer_text.size() << '\n' << answer_text << std::flush;
   if (not socket.start_ice(now_us())) return 10;

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);

   bool connected = false;
   bool established = false;
   bool native_opened = false;
   unsigned channel_closes = 0;
   while (std::chrono::steady_clock::now() < deadline) {
      ProcessMessages(PMF::NIL, 10);
      if (socket.state() IS rtc::DtlsState::CONNECTED and not connected) {
         connected = true;
         std::cout << "CONNECTED\n" << std::flush;
         if (association.state() IS rtc::SctpState::CLOSED and association.start(now_us()) != ERR::Okay) return 11;
      }
      if (connected) {
         if (receive_error != ERR::Okay or association.tick(now_us()) != ERR::Okay) return 12;
         if (association.state() IS rtc::SctpState::ESTABLISHED) {
            if (not native_opened) {
               for (unsigned mode = 0; mode < 6; ++mode) {
                  rtc::SctpChannelOptions options;
                  options.label = "native-" + std::to_string(mode); options.protocol = "echo";
                  options.ordered = mode < 3;
                  if (mode % 3 IS 1) options.reliability.max_retransmits = 0;
                  if (mode % 3 IS 2) options.reliability.max_lifetime_ms = 1000;
                  uint16_t id = 0;
                  if (channels.open(options, now_us(), id) != ERR::Okay) return 15;
               }
               rtc::SctpChannelOptions options; options.negotiated = true; options.id = 100;
               uint16_t id = 0;
               if (channels.open(options, now_us(), id) != ERR::Okay) return 16;
               native_opened = true;
            }
            if (channels.pump(now_us()) != ERR::Okay) return 14;
            for (auto &event : channels.take_events()) {
               if (event.type IS rtc::SctpChannelEventType::ERROR) {
                  std::fprintf(stderr, "Channel %u failed: %d\n", event.stream, int(event.error));
                  return 17;
               }
               if (event.type IS rtc::SctpChannelEventType::MESSAGE) {
                  if (channels.send(event.stream, event.payload, event.text, now_us()) != ERR::Okay) return 18;
                  auto channel = channels.channel(event.stream);
                  if (event.payload.empty() and not event.text and channel and channel->local and
                      not channel->options.negotiated and
                      channels.close(event.stream, now_us()) != ERR::Okay) return 19;
               }
               if (event.type IS rtc::SctpChannelEventType::CLOSE) {
                  ++channel_closes; std::cout << "CHANNEL_CLOSED " << channel_closes << '\n' << std::flush;
               }
            }
         }
         for (auto &packet : association.take_output()) {
            std::fprintf(stderr, "SCTP_TX ");
            for (auto byte : packet) std::fprintf(stderr, "%02x", byte);
            std::fprintf(stderr, "\n");
            if (socket.send(packet) != ERR::Okay) return 13;
            association.output_accepted(packet);
         }
         if (association.state() IS rtc::SctpState::ESTABLISHED and not established) {
            established = true;
            std::cout << "SCTP_CONNECTED\n" << std::flush;
         }
      }
      if (established and channel_closes >= 13) return 0;
      if (socket.state() IS rtc::DtlsState::FAILED or socket.socket_error() != ERR::Okay) return 7;
      if (socket.handle_timeout(now_us()) != ERR::Okay) return 8;
   }
   return 9;
}

int main(int argc, CSTRING *argv) {
#ifdef _WIN32
   // SDP already contains CRLF; CRT text translation would alter both framing and the declared byte count.
   if (_setmode(_fileno(stdout), _O_BINARY) < 0) return 1;
#endif
   if (auto message = init_kotuku(argc, argv)) {
      std::fprintf(stderr, "%s\n", message);
      return 1;
   }
   if (objModule::load("network", &glNetworkModule, &NetworkBase) != ERR::Okay) {
      close_kotuku();
      return 1;
   }
   auto result = run_probe();
   if (result) std::fprintf(stderr, "Native WebRTC probe failed at step %d\n", result);
   close_kotuku();
   return result ? 1 : 0;
}
