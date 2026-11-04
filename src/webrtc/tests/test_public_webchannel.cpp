#include <kotuku/startup.h>
#include <kotuku/main.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/webrtc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

CSTRING ProgName = "WebChannelPublicTest";
namespace {
struct Context {
   objWebChannel *remote = nullptr;
   std::vector<int8_t> expected;
   unsigned messages = 0, opens = 0, closes = 0, lows = 0;
   bool failed = false;
};
void opened(objWebChannel *Channel, APTR Meta) {
   auto &context = *(Context *)Meta;
   ++context.opens;
   if (Channel->State != WCS::OPEN) context.failed = true;
}
void closed(objWebChannel *Channel, APTR Meta) {
   auto &context = *(Context *)Meta;
   ++context.closes;
   if (Channel->State != WCS::CLOSED) context.failed = true;
}
void low(objWebChannel *, APTR Meta) { ++((Context *)Meta)->lows; }
ERR message(objWebChannel *, const void *Data, int Length, WCM Type, APTR Meta) {
   auto &context = *(Context *)Meta;
   if (Type != WCM::BINARY or size_t(Length) != context.expected.size() or
       not std::equal(context.expected.begin(), context.expected.end(), (const int8_t *)Data)) context.failed = true;
   ++context.messages;
   return ERR::Okay;
}
ERR callbacks(objWebChannel *Channel, Context &Data) {
   auto error = Channel->setOnOpen(C_FUNCTION(opened, &Data));
   if (error IS ERR::Okay) error = Channel->setOnClose(C_FUNCTION(closed, &Data));
   if (error IS ERR::Okay) error = Channel->setOnMessage(C_FUNCTION(message, &Data));
   if (error IS ERR::Okay) error = Channel->setOnBufferedAmountLow(C_FUNCTION(low, &Data));
   return error;
}
void remote_channel(objPeerConnection *, objWebChannel *Channel, APTR Meta) {
   auto &context = *(Context *)Meta;
   context.remote = Channel;
   if (callbacks(Channel, context) != ERR::Okay) context.failed = true;
}
template<class Predicate> bool wait_for(Predicate Ready, int Seconds = 10) {
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(Seconds);
   while (not Ready() and std::chrono::steady_clock::now() < deadline) ProcessMessages(PMF::NIL, 1);
   return Ready();
}
int exercise(int Mode, size_t Size) {
   Context a, b;
   auto first = objPeerConnection::create::local(fl::Flags(RTCF::NO_MDNS),
      kt::FieldValue(kt::fieldhash("ReceiveLimit"), 16777216));
   if (not first) return 1;
   auto release_first = kt::Defer([&]() { FreeResource(first); });
   auto second = objPeerConnection::create::local(fl::Flags(RTCF::NO_MDNS),
      kt::FieldValue(kt::fieldhash("ReceiveLimit"), 16777216));
   if (not second) return 2;
   auto release_second = kt::Defer([&]() { FreeResource(second); });
   if (second->setOnWebChannel(C_FUNCTION(remote_channel, &b)) != ERR::Okay) return 3;
   objWebChannel *channel = nullptr;
   bool negotiated = Mode IS 6;
   if (first->createWebChannel("native", "test", Mode != 1 and Mode != 4 and Mode != 5,
       negotiated, negotiated ? 42 : -1,
       Mode IS 2 or Mode IS 4 ? 0 : -1, Mode IS 3 or Mode IS 5 ? 5000 : -1, &channel) != ERR::Okay) return 4;
   if (callbacks(channel, a) != ERR::Okay) return 5;
   int stream_id = -2;
   if (channel->getStream(stream_id) != ERR::Okay or stream_id != (negotiated ? 42 : -1)) return 26;
   if (negotiated) {
      if (second->createWebChannel("native", "test", true, true, 42, -1, -1, &b.remote) != ERR::Okay or
          callbacks(b.remote, b) != ERR::Okay) return 6;
   }
   if (first->createOffer() != ERR::Okay or first->setLocalDescription(first->LocalDescription) != ERR::Okay or
       second->setRemoteDescription(first->LocalDescription) != ERR::Okay or second->createAnswer() != ERR::Okay or
       second->setLocalDescription(second->LocalDescription) != ERR::Okay or
       first->setRemoteDescription(second->LocalDescription) != ERR::Okay) return 7;
   if (not wait_for([&]() { return a.opens IS 1 and b.opens IS 1; })) return 8;
   if (channel->getStream(stream_id) != ERR::Okay or
       (negotiated ? stream_id != 42 : (stream_id < 0 or stream_id % 2 != 0))) return 27;
   int64_t limit = 0;
   if (channel->getMaxMessageSize(limit) != ERR::Okay or limit != 16777216) return 9;
   a.expected.resize(Size); b.expected.resize(Size);
   for (size_t i = 0; i < Size; ++i) a.expected[i] = b.expected[i] = int8_t(i * 13 + i / 113);
   if (channel->send(a.expected, WCM::BINARY) != ERR::Okay) return 10;
   int64_t amount = 0;
   if (channel->getBufferedAmount(amount) != ERR::Okay or size_t(amount) != Size or a.lows) return 11;
   if (not wait_for([&]() { return b.messages IS 1 and a.lows IS 1; }, 90)) return 12;
   auto reverse_error = b.remote->send(a.expected, WCM::BINARY);
   if (reverse_error != ERR::Okay or not wait_for([&]() { return a.messages IS 1; }, 90)) {
      std::fprintf(stderr, "Reverse transfer: send=%d received=%u/%u states=%d/%d ICE=%d/%d\n",
         int(reverse_error), a.messages, b.messages, int(first->State), int(second->State),
         int(first->IceState), int(second->IceState));
      for (auto peer : {first, second}) {
         STRING snapshot = nullptr;
         if (peer->getStats(&snapshot) IS ERR::Okay and snapshot) {
            std::fprintf(stderr, "Reverse transfer stats: %s\n", snapshot);
            FreeResource(snapshot);
         }
      }
      return 13;
   }
   if (channel->close() != ERR::Okay or channel->close() != ERR::Okay or
       not wait_for([&]() { return a.closes IS 1 and b.closes IS 1; })) return 14;
   if (a.failed or b.failed) return 15;
   std::printf("Public C++ channel mode %d: %zu exact bytes in each direction\n", Mode, Size);
   return 0;
}
}
int main(int Argc, CSTRING *Argv) {
   if (auto error = init_kotuku(Argc, Argv)) { std::fprintf(stderr, "%s\n", error); return 1; }
   OBJECTPTR module;
   int result = 0;
   if (objModule::load("webrtc", &module) != ERR::Okay) { close_kotuku(); return 1; }
   // C++ allocated snapshots must outlive the action context and parent object.
   auto peer = objPeerConnection::create::local();
   STRING retained = nullptr, next = nullptr;
   if (not peer or peer->getStats(&retained) != ERR::Okay or not retained) result = 1;
   std::string copy = retained ? retained : "";
   if (peer and (peer->getStats(&next) != ERR::Okay or not next or retained IS next)) result = 1;
   if (next) FreeResource(next);
   if (peer) FreeResource(peer);
   if (retained) {
      if (std::string_view(retained) != copy) result = 1;
      FreeResource(retained);
   }
   if (result) { close_kotuku(); return result; }
   if (objModule::load("webrtc", &module) != ERR::Okay) result = 1;
   else for (int mode = 0; mode < 7; ++mode) {
      result = exercise(mode, mode IS 6 ? 16777216 : 1024);
      if (result) { std::fprintf(stderr, "Mode %d failed at step %d\n", mode, result); break; }
   }
   close_kotuku();
   return result ? 1 : 0;
}
