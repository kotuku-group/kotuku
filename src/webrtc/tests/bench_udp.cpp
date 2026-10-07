/*********************************************************************************************************************

Phase 0 benchmark: NetSocket UDP receive throughput at WebRTC packet sizes.

A sender thread transmits 1200-byte datagrams over loopback to a receiver, cycling the first byte through the STUN,
DTLS and RTP ranges so that every received datagram is classified as the WebRTC demultiplexer will (RFC 7983).  The
receiver is measured in three configurations:

  raw     poll() and recvfrom() directly on a socket.  The baseline.
  drain   A NetServer (NSF::UDP) whose C++ Incoming callback calls RecvFrom() until the socket is empty.
  single  The same, but reading exactly one datagram per callback, as the existing Tiri tests do.

Each configuration runs once in flood mode, which reports the receiver's capacity, and once paced at a rate well
above what a busy WebRTC peer receives, which must show zero loss.  Heap allocations are counted by interposing
glibc's malloc() for the duration of the receive window, so per-packet allocation is measured rather than assumed.

Finally the benchmark checks how RecvFrom() reports a datagram larger than the caller's buffer.

Run from the install folder so that the Core can be located:

  cd build/agents-install && ../agents/src/webrtc/bench_webrtc_udp [packets=200000] [rate=20000]

*********************************************************************************************************************/

#include <kotuku/startup.h>
#include <kotuku/main.h>
#include <kotuku/modules/network.h>
#include <kotuku/modules/module.h>

#include "../dtls_transport.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

JUMPTABLE_NETWORK
static OBJECTPTR modNetwork;

CSTRING ProgName = "WebRTCUDPBench";

//********************************************************************************************************************
// Allocation counting.  Defining malloc() in the executable interposes it for every shared library in the process.

extern "C" void * __libc_malloc(size_t);
extern "C" void * __libc_calloc(size_t, size_t);
extern "C" void * __libc_realloc(void *, size_t);

static std::atomic<bool> glCountAllocations = false;
static std::atomic<uint64_t> glAllocations = 0;

extern "C" void * malloc(size_t Size)
{
   if (glCountAllocations.load(std::memory_order_relaxed)) glAllocations.fetch_add(1, std::memory_order_relaxed);
   return __libc_malloc(Size);
}

extern "C" void * calloc(size_t Count, size_t Size)
{
   if (glCountAllocations.load(std::memory_order_relaxed)) glAllocations.fetch_add(1, std::memory_order_relaxed);
   return __libc_calloc(Count, Size);
}

extern "C" void * realloc(void *Pointer, size_t Size)
{
   if (glCountAllocations.load(std::memory_order_relaxed)) glAllocations.fetch_add(1, std::memory_order_relaxed);
   return __libc_realloc(Pointer, Size);
}

//********************************************************************************************************************

namespace {

constexpr int PACKET_SIZE = 1200;
constexpr int BASE_PORT = 19960;
constexpr uint8_t FIRST_BYTES[] = { 0x00, 0x17, 0x80 }; // STUN Binding Request, DTLS application data, RTP v2

struct Stats {
   uint64_t received = 0;
   uint64_t bytes = 0;
   uint64_t callbacks = 0;
   uint64_t stun = 0, dtls = 0, rtp = 0, unknown = 0;
   std::chrono::steady_clock::time_point first, last;
   std::array<int8_t, 2048> buffer;
   IPAddress source;
   bool drain = true;

   void record(const uint8_t *Data, size_t Length) {
      auto now = std::chrono::steady_clock::now();
      if (received IS 0) first = now;
      last = now;
      received++;
      bytes += Length;
      switch (rtc::classify_packet(Data[0])) {
         case rtc::PacketClass::STUN: stun++; break;
         case rtc::PacketClass::DTLS: dtls++; break;
         case rtc::PacketClass::RTP:  rtp++; break;
         default: unknown++; break;
      }
   }
};

// Sends Count datagrams to 127.0.0.1:Port.  A Rate of zero floods; otherwise datagrams are paced in 1 ms bursts.

void send_datagrams(int Port, uint64_t Count, int Rate, std::atomic<bool> &Finished)
{
   auto fd = socket(AF_INET, SOCK_DGRAM, 0);
   sockaddr_in dest = {};
   dest.sin_family = AF_INET;
   dest.sin_port = htons(uint16_t(Port));
   dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

   std::array<uint8_t, PACKET_SIZE> packet;
   for (size_t i = 0; i < packet.size(); i++) packet[i] = uint8_t(i);

   auto burst = (Rate > 0) ? std::max(1, Rate / 1000) : 0;
   auto next = std::chrono::steady_clock::now();
   for (uint64_t i = 0; i < Count; i++) {
      packet[0] = FIRST_BYTES[i % std::size(FIRST_BYTES)];
      while (sendto(fd, packet.data(), packet.size(), 0, (sockaddr *)&dest, sizeof(dest)) < 0) {
         if ((errno != ENOBUFS) and (errno != EAGAIN)) break;
         std::this_thread::yield();
      }
      if ((burst > 0) and (((i + 1) % uint64_t(burst)) IS 0)) {
         next += std::chrono::milliseconds(1);
         std::this_thread::sleep_until(next);
      }
   }
   ::close(fd);
   Finished = true;
}

struct Result {
   const char *name;
   uint64_t sent;
   Stats stats;
   uint64_t allocations;
};

void print_result(const Result &Result)
{
   auto &stats = Result.stats;
   double seconds = std::chrono::duration<double>(stats.last - stats.first).count();
   double pps = (seconds > 0) ? double(stats.received) / seconds : 0;
   double loss = Result.sent ? 100.0 * double(Result.sent - stats.received) / double(Result.sent) : 0;
   double per_callback = stats.callbacks ? double(stats.received) / double(stats.callbacks) : 0;
   double allocs = stats.received ? double(Result.allocations) / double(stats.received) : 0;
   std::printf("  %-14s %9llu rx %10.0f pkt/s %8.1f Mbit/s  loss %5.1f%%  %6.2f pkt/callback  %6.3f allocs/pkt"
      "  [stun %llu dtls %llu rtp %llu other %llu]\n",
      Result.name, (unsigned long long)stats.received, pps, pps * PACKET_SIZE * 8 / 1e6, loss, per_callback, allocs,
      (unsigned long long)stats.stun, (unsigned long long)stats.dtls, (unsigned long long)stats.rtp,
      (unsigned long long)stats.unknown);
}

// Wait for the sender to finish, then for the receive path to fall idle.

template <class Pump> void run_until_idle(std::atomic<bool> &Finished, Stats &Stats, Pump &&PumpOnce)
{
   uint64_t last_count = ~0ull;
   auto idle_since = std::chrono::steady_clock::now();
   while (true) {
      PumpOnce();
      auto now = std::chrono::steady_clock::now();
      if (Stats.received != last_count) {
         last_count = Stats.received;
         idle_since = now;
      }
      else if (Finished and (now - idle_since > std::chrono::milliseconds(250))) break;
   }
}

//********************************************************************************************************************
// Baseline: poll() and recvfrom() on the main thread.

Result run_raw(int Port, uint64_t Count, int Rate, const char *Name)
{
   Result result = { Name, Count, {}, 0 };
   auto fd = socket(AF_INET, SOCK_DGRAM, 0);
   sockaddr_in addr = {};
   addr.sin_family = AF_INET;
   addr.sin_port = htons(uint16_t(Port));
   addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0) {
      std::fprintf(stderr, "bind() failed on port %d\n", Port);
      ::close(fd);
      return result;
   }

   std::atomic<bool> finished = false;
   glAllocations = 0;
   glCountAllocations = true;
   std::thread sender(send_datagrams, Port, Count, Rate, std::ref(finished));

   auto &stats = result.stats;
   run_until_idle(finished, stats, [&]() {
      pollfd pfd = { fd, POLLIN, 0 };
      if (poll(&pfd, 1, 10) <= 0) return;
      stats.callbacks++;
      while (true) {
         auto n = recv(fd, stats.buffer.data(), stats.buffer.size(), MSG_DONTWAIT);
         if (n <= 0) break;
         stats.record((const uint8_t *)stats.buffer.data(), size_t(n));
      }
   });

   glCountAllocations = false;
   result.allocations = glAllocations;
   sender.join();
   ::close(fd);
   return result;
}

//********************************************************************************************************************
// NetServer with a native Incoming callback.

ERR netsocket_incoming(objNetServer *Socket, APTR Meta)
{
   auto &stats = *(Stats *)Meta;
   stats.callbacks++;
   do {
      int bytes_read = 0;
      if (Socket->recvFrom(&stats.source, stats.buffer, &bytes_read) != ERR::Okay) break;
      if (bytes_read <= 0) break;
      stats.record((const uint8_t *)stats.buffer.data(), size_t(bytes_read));
   } while (stats.drain);
   return ERR::Okay;
}

Result run_netsocket(int Port, uint64_t Count, int Rate, bool Drain, const char *Name)
{
   Result result = { Name, Count, {}, 0 };
   result.stats.drain = Drain;

   auto server = objNetServer::create::local(
      kt::FieldValue(kt::fieldhash("Address"), std::string_view("127.0.0.1")),
      fl::Port(Port),
      fl::Flags(NSF::UDP),
      fl::Incoming(C_FUNCTION(netsocket_incoming, &result.stats)));
   if (!server) {
      std::fprintf(stderr, "NetServer creation failed on port %d\n", Port);
      return result;
   }

   std::atomic<bool> finished = false;
   glAllocations = 0;
   glCountAllocations = true;
   std::thread sender(send_datagrams, Port, Count, Rate, std::ref(finished));

   run_until_idle(finished, result.stats, []() { ProcessMessages(PMF::NIL, 10); });

   glCountAllocations = false;
   result.allocations = glAllocations;
   sender.join();
   FreeResource(server);
   return result;
}

//********************************************************************************************************************
// A datagram larger than the receive buffer.  WebRTC peers must never deliver a truncated packet to DTLS or SRTP.

struct TruncationProbe {
   std::array<int8_t, 1500> buffer;
   IPAddress source;
   int bytes_read = -1;
   ERR error = ERR::NothingDone;
};

ERR truncation_incoming(objNetServer *Socket, APTR Meta)
{
   auto &probe = *(TruncationProbe *)Meta;
   probe.error = Socket->recvFrom(&probe.source, probe.buffer, &probe.bytes_read);
   return ERR::Okay;
}

void probe_truncation(int Port)
{
   TruncationProbe probe;
   auto server = objNetServer::create::local(
      kt::FieldValue(kt::fieldhash("Address"), std::string_view("127.0.0.1")),
      fl::Port(Port), fl::Flags(NSF::UDP), fl::Incoming(C_FUNCTION(truncation_incoming, &probe)));
   if (!server) return;

   auto fd = socket(AF_INET, SOCK_DGRAM, 0);
   sockaddr_in dest = {};
   dest.sin_family = AF_INET;
   dest.sin_port = htons(uint16_t(Port));
   dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   std::array<uint8_t, 2000> oversized = {};
   sendto(fd, oversized.data(), oversized.size(), 0, (sockaddr *)&dest, sizeof(dest));
   ::close(fd);

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
   while ((probe.bytes_read < 0) and (std::chrono::steady_clock::now() < deadline)) ProcessMessages(PMF::NIL, 10);

   std::printf("  2000-byte datagram into a 1500-byte buffer: RecvFrom() returned %s with BytesRead %d%s\n",
      GetErrorMsg(probe.error), probe.bytes_read,
      ((probe.error IS ERR::Okay) and (probe.bytes_read IS 1500)) ? " (silently truncated)" : "");
   FreeResource(server);
}

} // namespace

//********************************************************************************************************************

int main(int argc, CSTRING *argv)
{
   if (auto msg = init_kotuku(argc, argv)) {
      std::fprintf(stderr, "%s\n", msg);
      return 1;
   }

   if (objModule::load("network", &modNetwork, &NetworkBase) != ERR::Okay) {
      std::fprintf(stderr, "Failed to load the network module.\n");
      close_kotuku();
      return 1;
   }

   uint64_t packets = 200000;
   int rate = 20000;
   for (int i = 1; i < argc; i++) {
      std::string_view arg(argv[i]);
      if (arg.starts_with("packets=")) packets = std::strtoull(argv[i] + 8, nullptr, 10);
      else if (arg.starts_with("rate=")) rate = std::atoi(argv[i] + 5);
   }
   auto paced_packets = std::min<uint64_t>(packets, uint64_t(rate) * 2); // Two seconds per paced run

   std::printf("WebRTC UDP receive benchmark: %d-byte datagrams over loopback\n", PACKET_SIZE);
   std::printf("Flood (%llu packets, sender unpaced):\n", (unsigned long long)packets);
   print_result(run_raw(BASE_PORT, packets, 0, "raw"));
   print_result(run_netsocket(BASE_PORT + 1, packets, 0, true, "drain"));
   print_result(run_netsocket(BASE_PORT + 2, packets, 0, false, "single"));

   std::printf("Paced (%llu packets at %d pkt/s):\n", (unsigned long long)paced_packets, rate);
   auto paced_raw    = run_raw(BASE_PORT + 3, paced_packets, rate, "raw");
   auto paced_drain  = run_netsocket(BASE_PORT + 4, paced_packets, rate, true, "drain");
   auto paced_single = run_netsocket(BASE_PORT + 5, paced_packets, rate, false, "single");
   print_result(paced_raw);
   print_result(paced_drain);
   print_result(paced_single);

   std::printf("Edge cases:\n");
   probe_truncation(BASE_PORT + 6);

   // Pass criteria.  Loss is judged against the raw baseline from the same run, because scheduler stalls on a busy
   // host drop packets from any receiver once the default socket buffer (about 90 datagrams of this size) fills.
   // Allocation is judged per packet; a small constant number of allocations from the message loop is expected.

   auto lost = [](const Result &R) { return R.sent - R.stats.received; };
   auto tolerance = paced_packets / 100;
   bool ok = true;
   if (lost(paced_drain) > lost(paced_raw) + tolerance) {
      std::printf("FAIL: NetSocket drain lost %llu packets against a raw baseline of %llu\n",
         (unsigned long long)lost(paced_drain), (unsigned long long)lost(paced_raw));
      ok = false;
   }
   if (paced_drain.stats.unknown or paced_single.stats.unknown) {
      std::printf("FAIL: datagrams were misclassified\n");
      ok = false;
   }
   if (paced_drain.allocations > paced_drain.stats.received / 20) {
      std::printf("FAIL: the receive path allocates per packet\n");
      ok = false;
   }

   close_kotuku();
   return ok ? 0 : 1;
}
