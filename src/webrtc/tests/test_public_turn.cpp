#include <kotuku/startup.h>
#include <kotuku/main.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/webrtc.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>
CSTRING ProgName = "WebRTCPublicTurnTest";
namespace {
struct Context {
   objWebChannel *remote = nullptr;
   std::vector<int8_t> expected;
   WCM type = WCM::TEXT;
   int messages = 0;
   bool failed = false;
};
ERR message(objWebChannel *, const void *Data, int Length, WCM Type, APTR Meta) {
   auto &context = *(Context *)Meta;
   if (Type != context.type or size_t(Length) != context.expected.size() or
       not std::equal(context.expected.begin(), context.expected.end(), (const int8_t *)Data)) context.failed = true;
   ++context.messages; return ERR::Okay;
}
void remote(objPeerConnection *, objWebChannel *Channel, APTR Meta) {
   auto &context = *(Context *)Meta; context.remote = Channel;
   if (Channel->setOnMessage(C_FUNCTION(message, &context)) != ERR::Okay) context.failed = true;
}
template<class Predicate> bool wait_for(Predicate Ready) {
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
   while (not Ready() and std::chrono::steady_clock::now() < deadline) ProcessMessages(PMF::NIL, 1);
   return Ready();
}
int failure(const std::string &Config, ERR Expected) {
   auto peer = objPeerConnection::create::local(kt::FieldValue(kt::fieldhash("IceServers"), Config),
      fl::Flags(RTCF::RELAY_ONLY | RTCF::DISABLE_IPV6));
   if (not peer) return 1;
   auto release = kt::Defer([&]() { FreeResource(peer); });
   if (peer->createOffer() != ERR::Okay or peer->setLocalDescription(peer->LocalDescription) != ERR::Okay or
       not wait_for([&]() { return peer->State IS RTCS::FAILED; })) return 2;
   if (peer->Error != Expected or peer->LocalDescription.find("candidate:") != std::string::npos) {
      std::fprintf(stderr, "Expected error %d, got %d\n", int(Expected), int(peer->Error)); return 3;
   }
   if (peer->close() != ERR::Okay or peer->close() != ERR::Okay) return 4;
   std::printf("PASS public relay failure %d, no direct fallback, idempotent close\n", int(Expected));
   return 0;
}
int delayed_dns_close(const std::string &Config, bool Destroy) {
   auto peer = objPeerConnection::create::local(kt::FieldValue(kt::fieldhash("IceServers"), Config),
      fl::Flags(RTCF::RELAY_ONLY | RTCF::DISABLE_IPV6));
   if (not peer) return 1;
   auto release = kt::Defer([&]() { if (peer) FreeResource(peer); });
   if (peer->createOffer() != ERR::Okay) return 2;
   // The external DNS fixture observes the actual query before releasing its delayed response.
   ProcessMessages(PMF::NIL, 250);
   std::puts("DNS_CLOSE_READY"); std::fflush(stdout);
   auto start = std::chrono::steady_clock::now();
   if (Destroy) { FreeResource(peer); peer = nullptr; }
   else if (peer->close() != ERR::Okay) return 3;
   auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
   if (elapsed >= 500) {
      std::fprintf(stderr, "DNS teardown exceeded 500 ms: %lld ms\n", (long long)elapsed);
      return 5;
   }
   if (peer) {
      if (peer->State != RTCS::CLOSED) return 4;
      FreeResource(peer); peer = nullptr;
   }
   // A queued resolver result must not call into the retired peer or revive its generation.
   ProcessMessages(PMF::NIL, 2500);
   std::printf("PASS delayed DNS %s and queued-result dispatch; blocked %lld ms\n",
      Destroy ? "free" : "close", (long long)elapsed);
   return 0;
}
ERR lookup_received(objNetLookup *, ERR, std::string_view, const std::vector<IPAddress> &, APTR Meta) {
   ++*(unsigned *)Meta;
   return ERR::Okay;
}
int dns_capacity(const std::string &Config) {
   auto start_host = Config.find("turn:");
   if (start_host IS std::string::npos) return 1;
   start_host += 5;
   auto end_host = Config.find(':', start_host);
   if (end_host IS std::string::npos) return 1;
   auto host = Config.substr(start_host, end_host - start_host);
   unsigned callbacks = 0;
   auto lookup = objNetLookup::create::local(fl::Flags(NLF::NO_CACHE));
   if (not lookup) return 2;
   auto release = kt::Defer([&]() { if (lookup) FreeResource(lookup); });
   if (lookup->setCallback(C_FUNCTION(lookup_received, &callbacks)) != ERR::Okay) return 2;
   for (unsigned i = 0; i < 32; ++i) if (lookup->resolveName(host) != ERR::Okay) return 3;
   if (lookup->resolveName(host) != ERR::BufferOverflow) return 4;
   ProcessMessages(PMF::NIL, 250);
   if (callbacks) return 5;
   std::puts("DNS_CLOSE_READY"); std::fflush(stdout);
   auto start = std::chrono::steady_clock::now();
   FreeResource(lookup); lookup = nullptr;
   auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
   if (elapsed >= 500) return 6;
   ProcessMessages(PMF::NIL, 2500);
   if (callbacks) return 7;
   std::printf("PASS delayed DNS capacity 32/33 and cancellation; blocked %lld ms\n", (long long)elapsed);
   return 0;
}
int cancel(const std::string &Config) {
   for (int stage = 0; stage < 3; ++stage) {
      auto peer = objPeerConnection::create::local(kt::FieldValue(kt::fieldhash("IceServers"), Config),
         fl::Flags(RTCF::RELAY_ONLY | RTCF::DISABLE_IPV6));
      if (not peer) return 1;
      if (peer->createOffer() != ERR::Okay) { FreeResource(peer); return 2; }
      if (stage) ProcessMessages(PMF::NIL, stage * 5);
      FreeResource(peer);
      ProcessMessages(PMF::NIL, 20);
   }
   std::printf("PASS public close/free during DNS/connect/allocation work\n"); return 0;
}
int exercise(const std::string &Config, bool FirstRelay, bool SecondRelay, bool Topology = false) {
   Context a, b;
   auto first = objPeerConnection::create::local(
      kt::FieldValue(kt::fieldhash("IceServers"), FirstRelay ? Config : "[]"),
      fl::Flags(FirstRelay ? RTCF::RELAY_ONLY | RTCF::DISABLE_IPV6 : RTCF::NO_MDNS));
   if (not first) return 1;
   auto release_a = kt::Defer([&]() { FreeResource(first); });
   auto second = objPeerConnection::create::local(
      kt::FieldValue(kt::fieldhash("IceServers"), SecondRelay ? Config : "[]"),
      fl::Flags(SecondRelay ? RTCF::RELAY_ONLY | RTCF::DISABLE_IPV6 : RTCF::NO_MDNS));
   if (not second) return 2;
   auto release_b = kt::Defer([&]() { FreeResource(second); });
   auto diagnose = [&](int Step) {
      int first_restart = 0, second_restart = 0;
      (void)first->getRestartRequired(first_restart); (void)second->getRestartRequired(second_restart);
      std::fprintf(stderr, "Public TURN step %d: states %d/%d, ICE %d/%d, errors %d/%d, restart %d/%d\n", Step,
         int(first->State), int(second->State), int(first->IceState), int(second->IceState),
         int(first->Error), int(second->Error), first_restart, second_restart);
      for (auto peer : {first, second}) {
         STRING snapshot = nullptr;
         if (peer->getStats(&snapshot) IS ERR::Okay and snapshot) {
            std::fprintf(stderr, "Peer %d: %s\n", peer->UID, snapshot);
            FreeResource(snapshot);
         }
      }
      return Step;
   };
   if (second->setOnWebChannel(C_FUNCTION(remote, &b)) != ERR::Okay) return 3;
   objWebChannel *channel = nullptr;
   if (first->createWebChannel("turn", "", true, false, -1, -1, -1, &channel) != ERR::Okay or
       channel->setOnMessage(C_FUNCTION(message, &a)) != ERR::Okay) return 4;
   if (first->createOffer() != ERR::Okay or first->setLocalDescription(first->LocalDescription) != ERR::Okay or
       not wait_for([&]() { return first->GatheringState IS RTCGATHER::COMPLETE; })) return 5;
   if (FirstRelay and (first->LocalDescription.find("typ relay") IS std::string::npos or
       first->LocalDescription.find("typ host") != std::string::npos or
       first->LocalDescription.find("raddr") != std::string::npos)) {
      std::fprintf(stderr, "Gather state/error %d/%d SDP:\n%s\n", int(first->State), int(first->Error),
         first->LocalDescription.c_str()); return 6;
   }
   if (second->setRemoteDescription(first->LocalDescription) != ERR::Okay or
       second->createAnswer() != ERR::Okay or second->setLocalDescription(second->LocalDescription) != ERR::Okay or
       not wait_for([&]() { return second->GatheringState IS RTCGATHER::COMPLETE; })) return 7;
   if (SecondRelay and (second->LocalDescription.find("typ relay") IS std::string::npos or
       second->LocalDescription.find("typ host") != std::string::npos)) return 8;
   if (first->setRemoteDescription(second->LocalDescription) != ERR::Okay or
       not wait_for([&]() { return channel->State IS WCS::OPEN and b.remote and b.remote->State IS WCS::OPEN; })) {
      std::fprintf(stderr, "Connection states %d/%d, errors %d/%d\n", int(first->State), int(second->State),
         int(first->Error), int(second->Error)); return 9;
   }
   for (auto type : {WCM::TEXT, WCM::BINARY}) for (size_t size : {size_t(0), size_t(2), size_t(262144)}) {
      a.type = b.type = type; a.messages = b.messages = 0;
      a.expected.assign(size, type IS WCM::TEXT ? 'x' : int8_t(0xff)); b.expected = a.expected;
      if (channel->send(a.expected, type) != ERR::Okay or
          not wait_for([&]() { return b.messages IS 1; }) or b.failed) return 10;
      if (b.remote->send(a.expected, type) != ERR::Okay or
          not wait_for([&]() { return a.messages IS 1; }) or a.failed) return 11;
   }
   if (Topology) {
      STRING snapshot = nullptr;
      if (first->getStats(&snapshot) != ERR::Okay or not snapshot) return 18;
      std::printf("TOPOLOGY_READY %s\n", snapshot);
      FreeResource(snapshot); std::fflush(stdout);
      if (not wait_for([&]() {
         int a = 0, b = 0;
         return first->getRestartRequired(a) IS ERR::Okay and second->getRestartRequired(b) IS ERR::Okay and a and b;
      })) return diagnose(19);
   }
   // Recreate every TURN allocation while retaining public channels and outstanding reliable data.
   for (auto initiator : {first, second}) {
      auto answerer = initiator IS first ? second : first;
      auto fingerprint = initiator->CertificateFingerprint;
      auto old_description = initiator->LocalDescription;
      a.type = b.type = WCM::BINARY; a.messages = b.messages = 0;
      a.expected.assign(262144, int8_t(0xa5)); b.expected = a.expected;
      if (channel->send(a.expected, WCM::BINARY) != ERR::Okay or initiator->restartIce() != ERR::Okay or
          not wait_for([&]() { return initiator->GatheringState IS RTCGATHER::COMPLETE; }) or
          initiator->setLocalDescription(initiator->LocalDescription) != ERR::Okay or
          answerer->setRemoteDescription(initiator->LocalDescription) != ERR::Okay or
          answerer->createAnswer() != ERR::Okay or
          not wait_for([&]() { return answerer->GatheringState IS RTCGATHER::COMPLETE; }) or
          answerer->setLocalDescription(answerer->LocalDescription) != ERR::Okay or
          initiator->setRemoteDescription(answerer->LocalDescription) != ERR::Okay or
          not wait_for([&]() { return b.messages IS 1; }) or b.failed) return diagnose(13);
      ProcessMessages(PMF::NIL, 50);
      if (b.messages != 1 or channel->State != WCS::OPEN or b.remote->State != WCS::OPEN or
          initiator->CertificateFingerprint != fingerprint or initiator->LocalDescription IS old_description) return 14;
      if (b.remote->send(a.expected, WCM::BINARY) != ERR::Okay or
          not wait_for([&]() { return a.messages IS 1; }) or a.failed) return diagnose(15);
   }
   for (auto peer : {first, second}) {
      STRING snapshot = nullptr;
      if (peer->getStats(&snapshot) != ERR::Okay or not snapshot) return 16;
      std::string stats(snapshot);
      FreeResource(snapshot);
      bool relay = peer IS first ? FirstRelay : SecondRelay;
      if (stats.find("\"selectedRoute\":null") != std::string::npos or
          (relay and (stats.find("\"localType\":\"relay\"") IS std::string::npos or
           stats.find("\"turnSentBytes\":\"0\"") != std::string::npos))) return 17;
      std::printf("Route evidence: %s\n", stats.c_str());
   }
   if (channel->close() != ERR::Okay or not wait_for([&]() { return channel->State IS WCS::CLOSED and
       b.remote->State IS WCS::CLOSED; })) return 12;
   std::printf("PASS public relay matrix %d/%d text/binary/empty/256KiB, queued reliable restart both initiators\n", FirstRelay, SecondRelay);
   return 0;
}
}
int main(int Argc, CSTRING *Argv) {
   if (Argc < 2) return 1;
   std::ifstream input(Argv[1]);
   std::string config{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
   if (config.empty() or config.size() > 16384) return 1;
   if (auto error = init_kotuku(Argc, Argv)) { std::fprintf(stderr, "%s\n", error); return 1; }
   OBJECTPTR module; int result = 0;
   if (objModule::load("webrtc", &module) != ERR::Okay) result = 1;
   else if (Argc >= 3 and std::string_view(Argv[2]) IS "security") result = failure(config, ERR::Security);
   else if (Argc >= 3 and std::string_view(Argv[2]) IS "timeout") result = failure(config, ERR::Timeout);
   else if (Argc >= 3 and std::string_view(Argv[2]) IS "dns-close") result = delayed_dns_close(config, false);
   else if (Argc >= 3 and std::string_view(Argv[2]) IS "dns-free") result = delayed_dns_close(config, true);
   else if (Argc >= 3 and std::string_view(Argv[2]) IS "dns-capacity") result = dns_capacity(config);
   else if (Argc >= 3 and std::string_view(Argv[2]) IS "cancel") result = cancel(config);
   else if (Argc >= 3 and std::string_view(Argv[2]) IS "topology") result = exercise(config, false, false, true);
   else for (auto mode : {1, 2, 3}) {
      result = exercise(config, mode & 1, mode & 2);
      if (result) { std::fprintf(stderr, "Relay mode %d failed at step %d\n", mode, result); break; }
   }
   if (result) std::fprintf(stderr, "Public TURN probe failed at step %d\n", result);
   close_kotuku(); return result ? 1 : 0;
}
