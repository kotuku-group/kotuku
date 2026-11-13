/*********************************************************************************************************************

Unit tests for the WebRTC DTLS transport (OpenSSL or Schannel backend).

Two DtlsTransport instances are connected through a simulated datagram network that can drop and reorder packets.
Every datagram is checked against the configured MTU and the RFC 7983 DTLS first-byte range.  The tests cover what
WebRTC needs from a socket-independent DTLS backend:

  * ECDSA P-256 self-signed certificates and SDP-style SHA-256 fingerprints
  * Fingerprint verification, both before and after the handshake completes
  * use_srtp negotiation and RFC 5764 keying material export
  * Handshake message fragmentation at a fixed MTU, and reassembly of a ClientHello fragmented by the peer
  * Retransmission on loss, tolerance of reordering
  * Application data exchange and close_notify

This executable has no object-system dependencies and is registered with ctest under the `webrtc` label.

*********************************************************************************************************************/

#include "../dtls/dtls_transport.h"

#include <array>

#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>

using namespace rtc;

namespace {

int glFailures = 0;
int glPassed = 0;

#define CHECK(Condition, Message) \
   if (not (Condition)) { std::fprintf(stderr, "  FAIL line %d: %s\n", __LINE__, Message); glFailures++; return; }

struct Endpoint {
   std::unique_ptr<DtlsTransport> transport;
   std::deque<std::vector<uint8_t>> outbox;
   std::vector<std::vector<uint8_t>> received;
   std::vector<DtlsState> states;
   size_t datagrams_sent = 0;
   size_t largest_datagram = 0;
   bool misclassified = false;
};

// A pair of endpoints joined by a simulated network.  `drop` is consulted for every datagram in transit and receives
// the sending side ('a' or 'b') and the zero-based index of the datagram in that direction.

struct Link {
   Endpoint a, b;
   int mtu = DtlsTransport::DEFAULT_MTU;
   std::function<bool(char, size_t)> drop;
   std::function<std::vector<std::vector<uint8_t>>(char, const std::vector<uint8_t> &)> rewrite;
   bool reverse_flights = false;
   size_t index_a = 0, index_b = 0;

   void attach(Endpoint &Side, std::shared_ptr<DtlsCertificate> Certificate, DtlsRole Role) {
      DtlsCallbacks callbacks;
      callbacks.send = [this, &Side](std::span<const uint8_t> Datagram) {
         Side.outbox.emplace_back(Datagram.begin(), Datagram.end());
         Side.datagrams_sent++;
         if (Datagram.size() > Side.largest_datagram) Side.largest_datagram = Datagram.size();
         if (Datagram.empty() or (classify_packet(Datagram[0]) != PacketClass::DTLS)) Side.misclassified = true;
      };
      callbacks.receive = [&Side](std::span<const uint8_t> Data) {
         Side.received.emplace_back(Data.begin(), Data.end());
      };
      callbacks.state = [&Side](DtlsState State) { Side.states.push_back(State); };
      Side.transport = std::make_unique<DtlsTransport>(std::move(Certificate), Role, std::move(callbacks));
      Side.transport->set_mtu(mtu);
      Side.transport->set_initial_timeout(20); // Keep loss tests fast
   }

   // Deliver one flight in each direction.  Returns true if any datagram was in transit.

   bool deliver() {
      auto from_a = std::move(a.outbox);
      auto from_b = std::move(b.outbox);
      a.outbox.clear();
      b.outbox.clear();

      auto transfer = [this](std::deque<std::vector<uint8_t>> &Flight, char From, size_t &Index, Endpoint &To) {
         if (reverse_flights) std::reverse(Flight.begin(), Flight.end());
         for (auto &sent : Flight) {
            auto index = Index++;
            if (drop and drop(From, index)) continue;
            auto datagrams = rewrite ? rewrite(From, sent) : std::vector<std::vector<uint8_t>>{ sent };
            for (auto &datagram : datagrams) {
               auto state = To.transport->state();
               if ((state IS DtlsState::CONNECTING) or (state IS DtlsState::CONNECTED)) {
                  (void)To.transport->feed(datagram);
               }
            }
         }
      };

      transfer(from_a, 'a', index_a, b);
      transfer(from_b, 'b', index_b, a);
      return (!from_a.empty()) or (!from_b.empty());
   }

   // Run the network until Done() is satisfied or the deadline passes.

   bool pump(const std::function<bool()> &Done, int TimeoutMs = 5000) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TimeoutMs);
      while (std::chrono::steady_clock::now() < deadline) {
         if (Done()) return true;
         if (deliver()) continue;
         (void)a.transport->handle_timeout();
         (void)b.transport->handle_timeout();
         if ((a.outbox.empty()) and (b.outbox.empty())) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return Done();
   }

   bool both_connected() {
      return (a.transport->state() IS DtlsState::CONNECTED) and (b.transport->state() IS DtlsState::CONNECTED);
   }
};

struct Certificates {
   std::shared_ptr<DtlsCertificate> a, b;
};

Certificates glCerts;

// Build a link where `a` is the DTLS client and `b` the server.  Fingerprints are exchanged up front unless
// KnowFingerprints is false.

std::unique_ptr<Link> make_link(int Mtu = DtlsTransport::DEFAULT_MTU, bool KnowFingerprints = true)
{
   auto link = std::make_unique<Link>();
   link->mtu = Mtu;
   link->attach(link->a, glCerts.a, DtlsRole::CLIENT);
   link->attach(link->b, glCerts.b, DtlsRole::SERVER);
   if (KnowFingerprints) {
      (void)link->a.transport->set_remote_fingerprint("sha-256", glCerts.b->fingerprint());
      (void)link->b.transport->set_remote_fingerprint("sha-256", glCerts.a->fingerprint());
   }
   return link;
}

bool start(Link &Link)
{
   return (Link.b.transport->start() IS ERR::Okay) and (Link.a.transport->start() IS ERR::Okay);
}

void report(std::string_view Name)
{
   std::printf("  PASS %.*s\n", int(Name.size()), Name.data());
   glPassed++;
}

//********************************************************************************************************************

void test_certificate()
{
   CHECK(glCerts.a->fingerprint().size() IS 95, "Fingerprint must be 32 colon-separated octets");
   CHECK(glCerts.a->fingerprint() != glCerts.b->fingerprint(), "Independent certificates must differ");

   auto link = make_link();
   CHECK(link->a.transport->set_remote_fingerprint("sha-1", glCerts.b->fingerprint()) IS ERR::NoSupport,
      "Only sha-256 fingerprints are accepted");
   CHECK(link->a.transport->set_remote_fingerprint("sha-256", "AB:CD") IS ERR::Args, "Malformed fingerprint accepted");

   std::string lower = glCerts.b->fingerprint();
   for (auto &ch : lower) ch = char(std::tolower((unsigned char)ch));
   CHECK(link->a.transport->set_remote_fingerprint("SHA-256", lower) IS ERR::Okay,
      "Fingerprint comparison must ignore case");
   report("certificate generation and fingerprint parsing");
}

void test_handshake_and_srtp_keys()
{
   auto link = make_link();
   CHECK(link->a.transport->set_role(DtlsRole::SERVER) IS ERR::Okay and
      link->a.transport->set_role(DtlsRole::CLIENT) IS ERR::Okay,
      "DTLS role could not be changed before the handshake");
   auto started = std::chrono::steady_clock::now();
   CHECK(start(*link), "start() failed");
   CHECK(link->a.transport->set_role(DtlsRole::SERVER) IS ERR::InvalidState,
      "DTLS role changed after the handshake began");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not complete");
   auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();

   CHECK(!link->a.misclassified and !link->b.misclassified, "A DTLS datagram fell outside the RFC 7983 range");
   CHECK(link->a.largest_datagram <= 1200 and link->b.largest_datagram <= 1200, "Datagram exceeded the MTU");

   SrtpKeyMaterial ka, kb;
   CHECK(link->a.transport->srtp_keys(ka) IS ERR::Okay, "Client key export failed");
   CHECK(link->b.transport->srtp_keys(kb) IS ERR::Okay, "Server key export failed");
   CHECK(ka.profile IS SrtpProfile::AEAD_AES_128_GCM, "AES-GCM should be preferred when both sides offer it");
   CHECK(ka.local_key.size() IS 16 and ka.local_salt.size() IS 12, "Unexpected AES-128-GCM key or salt length");
   CHECK(ka.local_key IS kb.remote_key and ka.local_salt IS kb.remote_salt, "Client write keys disagree");
   CHECK(ka.remote_key IS kb.local_key and ka.remote_salt IS kb.local_salt, "Server write keys disagree");
   CHECK(ka.local_key != ka.remote_key, "Directional keys must differ");

   std::printf("    handshake %.1f ms, client sent %zu datagrams (largest %zu), server sent %zu (largest %zu)\n",
      elapsed, link->a.datagrams_sent, link->a.largest_datagram, link->b.datagrams_sent, link->b.largest_datagram);
   report("handshake with AES-GCM SRTP key export");
}

void test_srtp_profile_fallback()
{
   auto link = make_link();
   static constexpr std::array<SrtpProfile, 1> ONLY_CM = { SrtpProfile::AES128_CM_SHA1_80 };
   link->b.transport->set_srtp_profiles(ONLY_CM);
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not complete");

   SrtpKeyMaterial keys;
   CHECK(link->a.transport->srtp_keys(keys) IS ERR::Okay, "Key export failed");
   CHECK(keys.profile IS SrtpProfile::AES128_CM_SHA1_80, "Expected fallback to AES128_CM_SHA1_80");
   CHECK(keys.local_key.size() IS 16 and keys.local_salt.size() IS 14, "Unexpected AES-CM key or salt length");
   report("SRTP profile fallback to AES128_CM_SHA1_80");
}

void test_no_srtp()
{
   auto link = make_link();
   link->a.transport->set_srtp_profiles({});
   link->b.transport->set_srtp_profiles({});
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not complete");

   SrtpKeyMaterial keys;
   CHECK(link->a.transport->srtp_keys(keys) IS ERR::NoSupport, "Key export must fail without use_srtp");
   report("data-channel-only handshake without use_srtp");
}

void test_application_data()
{
   auto link = make_link();
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not complete");

   auto max_payload = link->a.transport->max_payload();
   CHECK(max_payload > 1100 and max_payload < 1200, "Unexpected payload ceiling at a 1200-byte MTU");

   std::vector<uint8_t> small = { 'h', 'e', 'l', 'l', 'o' };
   std::vector<uint8_t> large(size_t(max_payload), 0x5a);
   std::vector<uint8_t> oversized(size_t(max_payload) + 1, 0xa5);

   CHECK(link->a.transport->send(small) IS ERR::Okay, "Small send failed");
   auto replay = link->a.outbox.back();
   CHECK(link->b.transport->send(large) IS ERR::Okay, "Maximum-size send failed");
   CHECK(link->a.transport->send(oversized) IS ERR::DataSize, "Oversized send must be refused");
   CHECK(link->pump([&]() { return link->a.received.size() IS 1 and link->b.received.size() IS 1; }),
      "Application data was not delivered");

   CHECK(link->b.received[0] IS small, "Small record corrupted");
   CHECK(link->a.received[0] IS large, "Large record corrupted");
   CHECK(link->a.largest_datagram <= 1200 and link->b.largest_datagram <= 1200, "Record exceeded the MTU");

   CHECK(link->b.transport->feed(replay) IS ERR::Okay, "Duplicate DTLS record failed transport");
   CHECK(link->b.transport->feed(replay) IS ERR::Okay, "Repeated DTLS record failed transport");
   CHECK(link->b.received.size() IS 1, "Replayed DTLS record delivered application data twice");
   CHECK(link->a.transport->send(small) IS ERR::Okay, "Fresh send after replay failed");
   auto corrupt = link->a.outbox.back();
   corrupt.back() ^= 1;
   CHECK(link->b.transport->feed(corrupt) IS ERR::Okay, "Bad-MAC record should be discarded");
   CHECK(link->b.received.size() IS 1, "Bad-MAC record delivered application data");
   CHECK(link->pump([&]() { return link->b.received.size() IS 2; }), "Fresh record was lost after bad-MAC/replay");

   // Two application records coalesced into a single datagram.

   CHECK(link->a.transport->send(small) IS ERR::Okay, "First coalesced send failed");
   CHECK(link->a.transport->send(small) IS ERR::Okay, "Second coalesced send failed");

   auto combined = std::move(link->a.outbox.front());
   link->a.outbox.pop_front();
   combined.insert(combined.end(), link->a.outbox.front().begin(), link->a.outbox.front().end());
   link->a.outbox.pop_front();

   CHECK(link->b.transport->feed(combined) IS ERR::Okay, "Coalesced records failed");
   CHECK(link->b.received.size() IS 4 and link->b.received[2] IS small and link->b.received[3] IS small,
      "Every application record in a coalesced datagram must be delivered");

   std::printf("    max application payload at 1200-byte MTU: %d bytes\n", max_payload);
   report("application data within the MTU");
}

void test_small_mtu_fragmentation()
{
   auto link = make_link(300);
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not complete at a 300-byte MTU");
   CHECK(link->a.largest_datagram <= 300 and link->b.largest_datagram <= 300, "Fragmentation did not respect the MTU");
   std::printf("    300-byte MTU: client sent %zu datagrams, server sent %zu\n", link->a.datagrams_sent,
      link->b.datagrams_sent);
   report("handshake fragmentation at a 300-byte MTU");
}

void test_loss_recovery()
{
   auto link = make_link();
   // Lose the first ClientHello and the first datagram of the server's first flight.
   link->drop = [](char From, size_t Index) { return Index IS 0; };
   auto started = std::chrono::steady_clock::now();
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not recover from loss");
   auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
   // Backends can round timer expiry differently; report the observed recovery time without assuming exact expiry.
   std::printf("    recovered in %.1f ms with a 20 ms initial retransmission timer\n", elapsed);

   // The server has already completed when its Finished is lost.  A backend must still retransmit that flight
   // without reopening or destroying the connected context.

   auto final_loss = make_link();
   bool dropped = false;
   final_loss->drop = [&](char From, size_t) {
      if (From IS 'b' and not dropped and final_loss->b.transport->state() IS DtlsState::CONNECTED) {
         dropped = true;
         return true;
      }
      return false;
   };

   CHECK(start(*final_loss), "Final-flight loss start failed");
   CHECK(final_loss->pump([&]() { return final_loss->both_connected(); }), "Lost server Finished did not recover");
   CHECK(dropped, "Final-flight loss fixture did not drop the server Finished");
   CHECK(final_loss->a.transport->timeout_us() < 0 and final_loss->b.transport->timeout_us() < 0,
      "Retransmission timers must stop after completion");

   report("retransmission after loss");
}

void test_heavy_loss_recovery()
{
   auto link = make_link();
   // Drop every third datagram in both directions.
   link->drop = [](char From, size_t Index) { return (Index % 3) IS 1; };
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }, 10000), "Handshake did not survive 33% loss");
   report("handshake under 33% loss");
}

void test_reordering()
{
   auto link = make_link(300); // A small MTU produces multi-datagram flights to reorder
   link->reverse_flights = true;
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not tolerate reordering");
   report("reordered handshake flights");
}

// Splits each client ClientHello in two (both, when the server sends a HelloVerifyRequest), as browsers do once a
// post-quantum key share pushes it past the MTU.  The fragments travel as separate datagrams, or together in reverse
// order when Together is set.  Epoch-0 client records are renumbered so the extra record keeps sequence numbers unique
// and increasing.

std::vector<std::vector<uint8_t>> split_client_hello(const std::vector<uint8_t> &Datagram, bool Together)
{
   auto renumber = [](std::vector<uint8_t> &Record, uint64_t Sequence) {
      for (int i = 0; i < 6; i++) Record[5 + i] = uint8_t(Sequence >> ((5 - i) * 8));
   };

   std::vector<std::vector<uint8_t>> records;
   size_t position = 0;
   while (position + 13 <= Datagram.size()) {
      size_t length = (size_t(Datagram[position + 11]) << 8) | Datagram[position + 12];
      if (position + 13 + length > Datagram.size()) break;
      std::vector<uint8_t> record(Datagram.begin() + position, Datagram.begin() + position + 13 + length);
      position += 13 + length;

      bool epoch_zero = not record[3] and not record[4];
      if (not epoch_zero) {
         records.push_back(std::move(record));
         continue;
      }

      uint64_t sequence = 0;
      for (int i = 0; i < 6; i++) sequence = (sequence << 8) | record[5 + i];

      bool whole_hello = record[0] IS 22 and length > 12 and record[13] IS 1 and
         record[19] IS 0 and record[20] IS 0 and record[21] IS 0 and
         (size_t(record[22]) << 16 | size_t(record[23]) << 8 | record[24]) + 12 IS length;
      if (not whole_hello) {
         renumber(record, sequence * 2);
         records.push_back(std::move(record));
         continue;
      }

      size_t body = length - 12, first = body / 2;
      for (size_t part = 0; part < 2; part++) {
         size_t offset = part ? first : 0, size = part ? body - first : first;
         std::vector<uint8_t> fragment(record.begin(), record.begin() + 13 + 6);
         fragment[11] = uint8_t((size + 12) >> 8);
         fragment[12] = uint8_t(size + 12);
         const uint8_t span[6] = { uint8_t(offset >> 16), uint8_t(offset >> 8), uint8_t(offset),
            uint8_t(size >> 16), uint8_t(size >> 8), uint8_t(size) };
         fragment.insert(fragment.end(), span, span + 6);
         fragment.insert(fragment.end(), record.begin() + 25 + offset, record.begin() + 25 + offset + size);
         renumber(fragment, sequence * 2 + part);
         records.push_back(std::move(fragment));
      }
      if (Together) std::swap(records[records.size() - 2], records.back());
   }

   if (not Together) return records;

   std::vector<uint8_t> combined;
   for (auto &record : records) combined.insert(combined.end(), record.begin(), record.end());
   return { combined };
}

void test_fragmented_client_hello()
{
   for (bool together : { false, true }) {
      auto link = make_link();
      size_t splits = 0;
      link->rewrite = [&](char From, const std::vector<uint8_t> &Datagram) {
         if (From != 'a') return std::vector<std::vector<uint8_t>>{ Datagram };
         auto result = split_client_hello(Datagram, together);
         size_t size = 0;
         for (auto &datagram : result) size += datagram.size();
         if (size > Datagram.size()) splits++;
         return result;
      };

      CHECK(start(*link), "start() failed");
      CHECK(link->pump([&]() { return link->both_connected(); }),
         together ? "Reversed ClientHello fragments in one datagram were not reassembled" :
            "ClientHello fragments in separate datagrams were not reassembled");
      CHECK(splits >= 1, "Fixture did not fragment the ClientHello");

      SrtpKeyMaterial ka, kb;
      CHECK(link->a.transport->srtp_keys(ka) IS ERR::Okay and link->b.transport->srtp_keys(kb) IS ERR::Okay,
         "Key export failed after a fragmented ClientHello");
      CHECK(ka.local_key IS kb.remote_key, "Keys disagree after a fragmented ClientHello");
   }
   report("fragmented ClientHello reassembly");
}

void test_fingerprint_mismatch()
{
   auto link = make_link();
   // The server expects the wrong certificate for the client.
   CHECK(link->b.transport->set_remote_fingerprint("sha-256", glCerts.b->fingerprint()) IS ERR::Okay,
      "Fingerprint rejected");
   CHECK(start(*link), "start() failed");
   link->pump([&]() { return link->b.transport->state() IS DtlsState::FAILED; }, 2000);
   CHECK(link->b.transport->state() IS DtlsState::FAILED, "Server accepted a mismatched certificate");
   CHECK(link->a.transport->state() != DtlsState::CONNECTED, "Client reached CONNECTED against a rejecting peer");

   SrtpKeyMaterial keys;
   CHECK(link->b.transport->srtp_keys(keys) IS ERR::InvalidState, "Keys must not be exported after a failure");
   std::printf("    server error: %s\n", link->b.transport->error_message().c_str());
   report("fingerprint mismatch is rejected");
}

void test_deferred_fingerprint()
{
   // The client's remote description arrives after the handshake.  This happens when the offerer is the DTLS server
   // and the answerer's ClientHello races the SDP answer.
   auto link = make_link(DtlsTransport::DEFAULT_MTU, false);
   (void)link->b.transport->set_remote_fingerprint("sha-256", glCerts.a->fingerprint());
   CHECK(start(*link), "start() failed");
   link->pump([&]() { return link->b.transport->state() IS DtlsState::CONNECTED; });

   CHECK(link->b.transport->state() IS DtlsState::CONNECTED, "Server did not connect");
   CHECK(link->a.transport->state() IS DtlsState::CONNECTING, "Client must wait for the remote fingerprint");

   std::vector<uint8_t> early = { 'e', 'a', 'r', 'l', 'y' };
   CHECK(link->b.transport->send(early) IS ERR::Okay, "Server send failed");
   link->pump([&]() { return false; }, 50);
   CHECK(link->a.received.empty(), "Data from an unverified peer must not be delivered");

   CHECK(link->a.transport->set_remote_fingerprint("sha-256", glCerts.b->fingerprint()) IS ERR::Okay,
      "Deferred verification failed");
   CHECK(link->a.transport->state() IS DtlsState::CONNECTED, "Client did not connect after verification");

   // A mismatched fingerprint that arrives after the client's handshake has completed.  The server reaches CONNECTED
   // one flight before the client, so the network is run a little longer to deliver the server's Finished.

   auto rejected = make_link(DtlsTransport::DEFAULT_MTU, false);
   (void)rejected->b.transport->set_remote_fingerprint("sha-256", glCerts.a->fingerprint());
   CHECK(start(*rejected), "start() failed");
   rejected->pump([&]() { return rejected->b.transport->state() IS DtlsState::CONNECTED; });
   rejected->pump([&]() { return false; }, 50);
   CHECK(rejected->a.transport->state() IS DtlsState::CONNECTING, "Client must still await its fingerprint");
   CHECK(rejected->a.transport->set_remote_fingerprint("sha-256", glCerts.a->fingerprint()) IS ERR::Failed,
      "Deferred mismatch must fail");
   CHECK(rejected->a.transport->state() IS DtlsState::FAILED, "Client must fail on a deferred mismatch");

   // A mismatched fingerprint that arrives mid-handshake, after the peer certificate was seen but before the
   // handshake completed.  The comparison happens at completion.

   auto racing = make_link(DtlsTransport::DEFAULT_MTU, false);
   (void)racing->b.transport->set_remote_fingerprint("sha-256", glCerts.a->fingerprint());
   CHECK(start(*racing), "start() failed");
   racing->pump([&]() { return racing->b.transport->state() IS DtlsState::CONNECTED; });
   CHECK(racing->a.transport->set_remote_fingerprint("sha-256", glCerts.a->fingerprint()) IS ERR::Okay,
      "A fingerprint supplied mid-handshake is stored for later comparison");
   racing->pump([&]() { return racing->a.transport->state() IS DtlsState::FAILED; }, 1000);
   CHECK(racing->a.transport->state() IS DtlsState::FAILED, "Mismatch must fail when the handshake completes");
   report("deferred fingerprint verification");
}

void test_close()
{
   auto link = make_link();
   CHECK(start(*link), "start() failed");
   CHECK(link->pump([&]() { return link->both_connected(); }), "Handshake did not complete");
   CHECK(link->a.transport->close() IS ERR::Okay, "close() failed");
   CHECK(link->pump([&]() { return link->b.transport->state() IS DtlsState::CLOSED; }),
      "Peer did not observe close_notify");
   CHECK(link->b.transport->send(std::vector<uint8_t>{ 1 }) IS ERR::InvalidState, "Send after close must fail");

   // The server initiates the close.

   auto server_close = make_link();
   CHECK(start(*server_close), "Server-close start failed");
   CHECK(server_close->pump([&]() { return server_close->both_connected(); }), "Server-close handshake failed");
   CHECK(server_close->b.transport->close() IS ERR::Okay, "Server close failed");
   CHECK(server_close->pump([&]() { return server_close->a.transport->state() IS DtlsState::CLOSED; }),
      "Client did not observe the server close_notify");

   report("close_notify");
}

void test_classifier()
{
   CHECK(classify_packet(0x00) IS PacketClass::STUN and classify_packet(0x01) IS PacketClass::STUN, "STUN range");
   CHECK(classify_packet(22) IS PacketClass::DTLS and classify_packet(63) IS PacketClass::DTLS, "DTLS range");
   CHECK(classify_packet(64) IS PacketClass::TURN_CHANNEL, "TURN ChannelData range");
   CHECK(classify_packet(0x80) IS PacketClass::RTP and classify_packet(191) IS PacketClass::RTP, "RTP range");
   CHECK(classify_packet(4) IS PacketClass::UNKNOWN and classify_packet(200) IS PacketClass::UNKNOWN, "Gaps");
   report("RFC 7983 classification");
}

} // namespace

//********************************************************************************************************************

int main()
{
#ifdef CRYPTO_PROTOCOL_CNG
   std::printf("WebRTC DTLS transport (Schannel)\n");
#else
   std::printf("WebRTC DTLS transport (OpenSSL)\n");
#endif

   if ((DtlsCertificate::generate(glCerts.a, "kotuku-a") != ERR::Okay) or
       (DtlsCertificate::generate(glCerts.b, "kotuku-b") != ERR::Okay)) {
      std::fprintf(stderr, "Certificate generation failed\n");
      return 1;
   }

   test_classifier();
   test_certificate();
   test_handshake_and_srtp_keys();
   test_srtp_profile_fallback();
   test_no_srtp();
   test_application_data();
   test_small_mtu_fragmentation();
   test_loss_recovery();
   test_heavy_loss_recovery();
   test_reordering();
   test_fragmented_client_hello();
   test_fingerprint_mismatch();
   test_deferred_fingerprint();
   test_close();

   std::printf("%d passed, %d failed\n", glPassed, glFailures);
   return glFailures ? 1 : 0;
}
