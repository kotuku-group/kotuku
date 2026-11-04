#include "turn_test_support.h"
#include <fstream>
#include <iterator>
using namespace turn_test;
namespace {
void codec_tests() {
   // Independent wire fixtures: hand-transcribed RFC 8656 ChannelData layout, and XOR address cookie arithmetic.
   const std::vector<uint8_t> wire{0x40, 0x01, 0x00, 0x03, 0x61, 0x62, 0x63};
   uint16_t channel = 0; std::span<const uint8_t> payload;
   require(decode_channel_data(wire, false, channel, payload) and channel IS 0x4001 and
      std::equal(payload.begin(), payload.end(), bytes("abc").begin()), "Independent ChannelData fixture");
   require(not decode_channel_data(wire, true, channel, payload), "Stream requires padding");
   auto padded = wire; padded.push_back(0xa5); // Padding contents are ignored, not required to be zero.
   require(decode_channel_data(padded, true, channel, payload), "Stream padding accepted");
   std::vector<uint8_t> encoded;
   require(encode_channel_data(0x4001, bytes("abc"), false, encoded) and encoded IS wire, "ChannelData round trip");
   require(not encode_channel_data(0x5000, {}, true, encoded), "Reserved channel number");
   std::vector<uint8_t> oversized(65533);
   require(not encode_channel_data(0x4000, oversized, true, encoded), "Frame bound");
   require(encode_channel_data(0x4000, {}, true, encoded) and encoded.size() IS 4, "Empty ChannelData");
   const std::vector<uint8_t> xor_fixture{0, 1, 0xe1, 0x13, 0xe1, 0x12, 0xa6, 0x43};
   TransactionId id{};
   require(encode_xor_address(endpoint(1, 49153), id, encoded) and encoded IS xor_fixture, "Independent XOR fixture");
   Message message; message.transaction_id = id;
   Attribute address{ATTR_XOR_PEER_ADDRESS, xor_fixture}; Endpoint peer;
   require(decode_xor_address(address, message, peer) and peer IS endpoint(1, 49153), "XOR fixture decode");
   require(encode_xor_address(endpoint(9, 5123, true), id, encoded), "IPv6 encode");
   address.value = encoded;
   require(decode_xor_address(address, message, peer) and peer IS endpoint(9, 5123, true), "IPv6 decode");
   // Binding must continue to reject TURN required attributes.
   message.attributes = {{ATTR_LIFETIME, xor_fixture}};
   require(not rtc::stun::unknown_required_attributes(message).empty() and valid_attributes(message),
      "Separate TURN attribute policy");
   message.attributes.push_back({0x1234, {}});
   require(not valid_attributes(message), "Unknown required TURN attribute");

   std::vector<uint8_t> stun_wire;
   require(rtc::stun::encode(REFRESH | 0x0100, id, {}, {}, false, true, stun_wire), "Stream STUN fixture");
   auto records_wire = padded; records_wire.insert(records_wire.end(), stun_wire.begin(), stun_wire.end());
   for (size_t split = 0; split <= records_wire.size(); ++split) {
      StreamDecoder decoder; size_t consumed = 0; std::vector<std::vector<uint8_t>> records;
      require(decoder.feed(std::span(records_wire).first(split), consumed, records) and consumed IS split,
         "Split header/payload first part");
      size_t count = records.size();
      require(decoder.feed(std::span(records_wire).subspan(split), consumed, records) and
         consumed IS records_wire.size() - split and count + records.size() IS 2 and decoder.buffered() IS 0,
         "Split/coalesced records");
   }
   StreamDecoder decoder; size_t consumed = 0; std::vector<std::vector<uint8_t>> records;
   std::vector<uint8_t> flood;
   for (unsigned i = 0; i < 100; ++i) flood.insert(flood.end(), padded.begin(), padded.end());
   require(decoder.feed(flood, consumed, records) and records.size() IS 32 and consumed IS 256,
      "Dispatch frame bound with unconsumed input");
   decoder.reset();
   std::vector<uint8_t> largest_payload(MAX_FRAME - 4), largest;
   require(encode_channel_data(0x4000, largest_payload, true, largest), "Largest allowed ChannelData frame");
   largest.insert(largest.end(), padded.begin(), padded.end());
   require(decoder.feed(largest, consumed, records) and consumed IS MAX_FRAME and records.size() IS 1,
      "Dispatch byte budget leaves following record for another call");
   require(decoder.feed(std::span(largest).subspan(consumed), consumed, records) and
      consumed IS padded.size() and records.size() IS 1 and decoder.buffered() IS 0, "Byte-budget rescheduling");
   decoder.reset();
   const std::array<uint8_t, 4> huge{0x40, 0x00, 0xff, 0xff};
   require(not decoder.feed(huge, consumed, records) and not decoder.feed({}, consumed, records),
      "Oversized stream record terminal failure");
   decoder.reset();
   auto bad = stun_wire; bad[4] ^= 1;
   require(not decoder.feed(bad, consumed, records), "Invalid stream STUN cookie");
}
void authentication_tests() {
   for (bool reliable : {false, true}) for (bool sha256 : {false, true}) {
      Client client(config(reliable)); establish(client, sha256);
      client.close(30); auto deletion = pop(client); auto parsed = parse(deletion);
      require(parsed.type IS REFRESH and parsed.find(ATTR_LIFETIME)->value[3] IS 0 and
         rtc::stun::verify_integrity(deletion.wire, parsed, key(sha256), sha256), "Authenticated deletion");
      require(client.state() IS State::CLOSED and client.pending() IS 0, "Immediate local cleanup");
      client.close(40); Packet packet; require(not client.pop_output(packet), "Close idempotency");
   }
   Client client(config()); require(client.start(0), "Auth test start"); auto initial = pop(client);
   auto wire = challenge(initial); Datagram data;
   auto wrong = initial.identity; ++wrong.allocation;
   require(client.receive(wrong, wire, 1, data) IS Result::IGNORED, "Wrong allocation rejected");
   wrong = initial.identity; ++wrong.generation;
   require(client.receive(wrong, wire, 1, data) IS Result::IGNORED, "Stale generation rejected");
   wrong = initial.identity; ++wrong.server.port;
   require(client.receive(wrong, wire, 1, data) IS Result::IGNORED, "Wrong server rejected");
   wire[8] ^= 1;
   require(client.receive(initial.identity, wire, 1, data) IS Result::IGNORED, "Wrong transaction rejected");
   require(client.receive(initial.identity, challenge(initial), 2, data) IS Result::ACCEPTED, "401 accepted");
   auto auth = pop(client);
   require(client.receive(auth.identity, success(auth, 600, false, false, "wrong"), 3, data) IS Result::IGNORED,
      "Wrong credentials rejected without mutation");
   require(client.receive(auth.identity, challenge(auth, 438, "next-nonce", false, false, "evil.org"), 3, data) IS
      Result::IGNORED, "Unauthenticated realm replacement rejected");
   require(client.receive(auth.identity, challenge(auth, 438, "next-nonce"), 4, data) IS Result::ACCEPTED,
      "Nonce-only 438 accepted");
   auto retry = pop(client);
   require(parse(retry).transaction_id != parse(auth).transaction_id, "Challenge uses fresh transaction");
   require(client.receive(auth.identity, success(auth), 5, data) IS Result::IGNORED, "Obsolete response rejected");
   require(client.receive(retry.identity, success(retry), 6, data) IS Result::ACCEPTED, "438 recovery success");
   require(client.receive(retry.identity, success(retry), 7, data) IS Result::IGNORED, "Duplicate reply rejected");

   Client userhash(config()); require(userhash.start(0), "Userhash start");
   auto hashed = pop(userhash);
   require(userhash.receive(hashed.identity, challenge(hashed, 401, "obMatJos2wAAAnonce", true), 1, data) IS
      Result::ACCEPTED, "Password algorithm and userhash challenge");
   hashed = pop(userhash); auto hashed_message = parse(hashed);
   require(hashed_message.find(rtc::stun::ATTR_USERHASH) and not hashed_message.find(rtc::stun::ATTR_USERNAME) and
      rtc::stun::verify_integrity(hashed.wire, hashed_message, key(true), true), "TURN shares userhash negotiation");
   require(userhash.receive(hashed.identity, challenge(hashed, 438, "next", false), 2, data) IS Result::IGNORED,
      "Unauthenticated algorithm downgrade rejected");
   require(userhash.receive(hashed.identity, success(hashed, 600, true), 3, data) IS Result::ACCEPTED,
      "SHA-256/userhash allocation");
   Client wrong_password(config()); require(wrong_password.start(0), "Wrong credentials start");
   auto credentials = pop(wrong_password);
   require(wrong_password.receive(credentials.identity, challenge(credentials), 1, data) IS Result::ACCEPTED,
      "Wrong credentials initial challenge");
   credentials = pop(wrong_password);
   require(wrong_password.receive(credentials.identity, challenge(credentials), 2, data) IS Result::FAILED and
      wrong_password.state() IS State::FAILED and wrong_password.error() IS 401, "Repeated 401 fails credentials");

   Client exhausted(config()); require(exhausted.start(0), "Challenge exhaustion start");
   auto request = pop(exhausted);
   require(exhausted.receive(request.identity, challenge(request), 1, data) IS Result::ACCEPTED, "First challenge");
   request = pop(exhausted);
   require(exhausted.receive(request.identity, challenge(request, 438, "second"), 2, data) IS Result::ACCEPTED,
      "Second challenge");
   request = pop(exhausted);
   require(exhausted.receive(request.identity, challenge(request, 438, "third"), 3, data) IS Result::FAILED and
      exhausted.state() IS State::FAILED and exhausted.queued_bytes() IS 0, "Bounded challenges and cleanup");
   Client invalid(config()); require(invalid.start(0), "Server error start"); request = pop(invalid);
   auto parsed = parse(request);
   std::array<uint8_t, 4> error{0, 0, 5, 8}; Attribute attribute{rtc::stun::ATTR_ERROR_CODE, error};
   require(rtc::stun::encode(ALLOCATE | 0x0110, parsed.transaction_id, std::span(&attribute, 1), {}, false,
      false, wire), "508 fixture");
   require(invalid.receive(request.identity, wire, 1, data) IS Result::FAILED and invalid.error() IS 508,
      "Server capacity error preserved");
   auto repeated = config(); repeated.random = [](TransactionId &Id) { Id.fill(0); return true; };
   Client entropy(repeated); require(entropy.start(0), "Entropy start"); request = pop(entropy);
   require(entropy.receive(request.identity, challenge(request), 1, data) IS Result::FAILED,
      "Transaction ID reuse fails closed");
}
void state_tests() {
   Client client(config()); establish(client); Endpoint peer = endpoint(8, 9000); grant(client, peer, 30);
   require(client.send(peer, bytes("hello"), 32), "Send indication before channel bind");
   auto sent = pop(client); require(parse(sent).type IS SEND_INDICATION, "Send indication method");
   auto indication = sent.wire; indication[1] = uint8_t(DATA_INDICATION);
   // No fingerprint after changing the indication method; regenerate a server indication explicitly.
   auto message = parse(sent); std::vector<Attribute> attrs;
   for (const auto &attribute : message.attributes) if (attribute.type != rtc::stun::ATTR_FINGERPRINT) {
      attrs.push_back(attribute);
   }
   require(rtc::stun::encode(DATA_INDICATION, message.transaction_id, attrs, {}, false, false, indication),
      "Data indication fixture");
   Datagram data;
   require(client.receive(sent.identity, indication, 33, data) IS Result::DATA and data.peer IS peer and
      data.payload.size() IS 5, "Data indication delivery");
   require(not client.send(endpoint(10, 9000), {}, 34), "Unpermitted egress blocked");
   require(client.bind(peer, 0x4000, 40), "Channel bind request"); auto binding = pop(client);
   require(parse(binding).type IS CHANNEL_BIND, "ChannelBind method");
   require(client.send(peer, {}, 41), "Pending channel falls back to Send");
   sent = pop(client); require(parse(sent).type IS SEND_INDICATION, "Pending binding not used for egress");
   require(client.receive(binding.identity, success(binding), 42, data) IS Result::ACCEPTED, "Binding success");
   require(client.send(peer, bytes("abc"), 43), "Bound ChannelData egress"); sent = pop(client);
   require(client.receive(sent.identity, sent.wire, 44, data) IS Result::DATA and data.payload.size() IS 3,
      "Bound ChannelData ingress");
   require(not client.bind(endpoint(9, 9000), 0x4000, 45) and not client.bind(peer, 0x4001, 45),
      "Channel and peer uniqueness");
   client.tick(240040); auto renewal = pop(client);
   require(parse(renewal).type IS CREATE_PERMISSION, "Independent permission renewal deadline");
   require(client.receive(renewal.identity, success(renewal), 240041, data) IS Result::ACCEPTED, "Permission renewal");
   client.tick(480040); auto refresh = pop(client); auto channel_refresh = pop(client);
   require(parse(refresh).type IS REFRESH and parse(channel_refresh).type IS CHANNEL_BIND,
      "Independent allocation and channel maintenance");
   require(client.receive(refresh.identity, success(refresh), 480041, data) IS Result::ACCEPTED, "Allocation refresh");
   require(client.receive(channel_refresh.identity, success(channel_refresh), 480041, data) IS Result::ACCEPTED,
      "Channel renewal also renews permission");
   client.tick(720040); renewal = pop(client);
   require(parse(renewal).type IS CREATE_PERMISSION, "Permission renewed by ChannelBind");
   // Lose permission refresh.  Reliable allocation remains active but permission expires independently.
   client.tick(759540); require(client.state() IS State::ACTIVE, "Permission timeout does not destroy allocation");
   client.tick(780040); require(not client.send(peer, {}, 780040), "Expired permission prevents traffic");

   Client expiry(config(true)); establish(expiry, false, 1); expiry.tick(1000);
   require(expiry.state() IS State::FAILED and expiry.pending() IS 0 and expiry.queued_bytes() IS 0,
      "Short lifetime expires with bounded cleanup");
   Client loss(config(true)); establish(loss); loss.tick(480000); refresh = pop(loss);
   loss.tick(480500); Packet packet; require(not loss.pop_output(packet), "Reliable transport does not retransmit");
   loss.tick(485000); require(loss.state() IS State::FAILED, "Refresh loss fails allocation at finite deadline");
   Client udp(config()); require(udp.start(0), "UDP timeout start"); (void)pop(udp);
   for (uint64_t time : {500u, 1500u, 3500u, 7500u, 15500u}) {
      udp.tick(time); require(pop(udp).wire.size() > 20, "UDP exponential retry");
   }
   udp.tick(30000); require(udp.state() IS State::FAILED and udp.pending() IS 0, "Gathering allocation deadline");

   Client ipv6(config(true, true)); establish(ipv6, true, 600, true);
   require(not ipv6.permission(peer, 30), "Peer family mismatch rejected");
   require(ipv6.bind(endpoint(4, 9000, true), 0x4fff, 30), "IPv6 ChannelBind");
}
void adversarial_tests() {
   auto fixture = [](const char *Name) {
      std::ifstream file(std::string(WEBRTC_FIXTURE_DIR) + "/" + Name, std::ios::binary);
      require(bool(file), "Independent fixture file available");
      return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), {});
   };
   Client client(config()); require(client.start(0), "Independent authentication start");
   auto initial = pop(client); Datagram data;
   require(client.receive(initial.identity, fixture("turn_allocate_401.turn"), 10, data) IS Result::ACCEPTED,
      "Independent 401 wire fixture");
   auto request = pop(client); auto valid = fixture("turn_allocate_success.turn");
   auto trailing = valid;
   const std::array<uint8_t, 8> tail{0x80, 0x22, 0, 4, 0, 0, 0, 1};
   trailing.insert(trailing.end(), tail.begin(), tail.end());
   trailing[2] = uint8_t((trailing.size() - 20) >> 8); trailing[3] = uint8_t(trailing.size() - 20);
   require(client.receive(request.identity, trailing, 11, data) IS Result::IGNORED and
      client.state() IS State::ALLOCATING, "Unauthenticated trailing attributes rejected");
   Message parsed; require(rtc::stun::parse(valid, parsed), "Independent success parse");
   std::vector<Attribute> attributes;
   for (const auto &attribute : parsed.attributes) if (attribute.type != rtc::stun::ATTR_MESSAGE_INTEGRITY) {
      attributes.push_back(attribute);
   }
   attributes.push_back({0x1234, {}});
   std::vector<uint8_t> malformed;
   require(rtc::stun::encode(parsed.type, parsed.transaction_id, attributes, key(), false, false, malformed),
      "Authenticated unknown required fixture");
   require(client.receive(request.identity, malformed, 12, data) IS Result::IGNORED,
      "Unknown required response rejected before mutation");
   attributes.pop_back(); attributes.push_back(attributes.front());
   require(rtc::stun::encode(parsed.type, parsed.transaction_id, attributes, key(), false, false, malformed),
      "Duplicate relayed address fixture");
   require(client.receive(request.identity, malformed, 13, data) IS Result::IGNORED,
      "Duplicate response fields rejected before mutation");
   require(client.receive(request.identity, valid, 20, data) IS Result::ACCEPTED and
      client.relayed() IS endpoint(2, 50000) and client.mapped() IS endpoint(3, 40000),
      "Independent authenticated allocation response");
   require(client.permission(endpoint(8, 9000), 30), "Authenticated error request"); request = pop(client);
   auto message = parse(request); std::array<uint8_t, 4> error{0, 0, 4, 3};
   Attribute forbidden{rtc::stun::ATTR_ERROR_CODE, error};
   require(rtc::stun::encode(CREATE_PERMISSION | 0x0110, message.transaction_id, std::span(&forbidden, 1),
      key(), false, false, malformed), "Signed 403");
   require(client.receive(request.identity, malformed, 31, data) IS Result::FAILED and
      client.state() IS State::ACTIVE and client.error() IS 403, "Peer denial preserves allocation");
   client.tick(32); require(client.permission_count() IS 0, "Failed permission placeholder removed");
   require(client.permission(endpoint(8, 9000), 33), "Allocation mismatch request"); request = pop(client);
   message = parse(request); error[3] = 37;
   require(rtc::stun::encode(CREATE_PERMISSION | 0x0110, message.transaction_id, std::span(&forbidden, 1),
      key(), false, false, malformed), "Signed 437");
   require(client.receive(request.identity, malformed, 34, data) IS Result::FAILED and
      client.state() IS State::FAILED, "Allocation mismatch invalidates live allocation");

   Client denial(config(true)); establish(denial, false, 3600); grant(denial, endpoint(8, 9000), 30);
   require(denial.bind(endpoint(8, 9000), 0x4000, 100), "Denied binding with existing permission");
   auto denied_binding = pop(denial); auto denied_message = parse(denied_binding); error[3] = 3;
   require(rtc::stun::encode(CHANNEL_BIND | 0x0110, denied_message.transaction_id, std::span(&forbidden, 1),
      key(), false, false, malformed), "Signed ChannelBind denial");
   require(denial.receive(denied_binding.identity, malformed, 101, data) IS Result::FAILED,
      "ChannelBind denial outcome");
   denial.tick(240030);
   require(parse(pop(denial)).type IS CREATE_PERMISSION, "Failed channel must preserve independent permission renewal");

   Client quarantine(config(true)); establish(quarantine, false, 3600);
   require(quarantine.bind(endpoint(8, 9000), 0x4000, 30), "Uncertain binding request"); (void)pop(quarantine);
   quarantine.tick(5030);
   require(not quarantine.bind(endpoint(9, 9000), 0x4000, 600030) and
      not quarantine.bind(endpoint(8, 9000), 0x4001, 600030), "Unconfirmed binding quarantine");
   require(not quarantine.bind(endpoint(9, 9000), 0x4000, 900030), "Lost response includes timeout in quarantine");
   require(quarantine.bind(endpoint(9, 9000), 0x4000, 905030), "Channel reuse after quarantine");
   auto binding = pop(quarantine);
   require(quarantine.receive(binding.identity, success(binding), 905031, data) IS Result::ACCEPTED,
      "Reused channel confirmed");
   std::vector<uint8_t> channel_wire;
   require(encode_channel_data(0x4000, bytes("new"), true, channel_wire), "Reused ChannelData");
   require(quarantine.receive(binding.identity, channel_wire, 905032, data) IS Result::DATA and
      data.peer IS endpoint(9, 9000), "Reused channel dispatches current peer");
   quarantine.close(905033);
   require(quarantine.receive(binding.identity, channel_wire, 905034, data) IS Result::IGNORED,
      "Closed allocation cannot deliver stale data");

   Client timer(config()); establish(timer, false, 3600);
   require(timer.permission(endpoint(9), 30), "Seven transmission transaction"); request = pop(timer);
   for (uint64_t time : {530u, 1530u, 3530u, 7530u, 15530u, 31530u}) {
      timer.tick(time); auto retransmit = pop(timer);
      require(retransmit.wire IS request.wire, "UDP retry keeps transaction and request bytes");
   }
   timer.tick(39530); require(timer.pending() IS 0 and timer.state() IS State::ACTIVE,
      "Seven UDP transmissions finish at finite deadline");
   Client short_close(config()); establish(short_close); short_close.close(30); short_close.tick(1030);
   Packet packet; require(not short_close.pop_output(packet), "Queued deletion has one second local deadline");
}
void limits_tests() {
   Client ingress(config(true)); establish(ingress); auto incoming_peer = endpoint(8, 9000);
   require(ingress.bind(incoming_peer, 0x4000, 30), "Ingress channel");
   auto bind_request = pop(ingress); Datagram received;
   require(ingress.receive(bind_request.identity, success(bind_request), 31, received) IS Result::ACCEPTED,
      "Ingress channel granted");
   std::vector<uint8_t> incoming(1280), wire;
   require(encode_channel_data(0x4000, incoming, true, wire) and
      ingress.receive(bind_request.identity, wire, 32, received) IS Result::DATA,
      "Remote datagram capacity is independent of local send MTU");
   incoming.resize(MAX_INNER_RECEIVE + 1);
   require(encode_channel_data(0x4000, incoming, true, wire) and
      ingress.receive(bind_request.identity, wire, 32, received) IS Result::IGNORED,
      "Ingress datagram capacity remains bounded");
   Client client(config(true)); establish(client); auto peer = endpoint(8, 9000); grant(client, peer, 30);
   std::vector<uint8_t> payload(1200);
   size_t accepted = 0;
   while (client.send(peer, payload, 40)) ++accepted;
   require(accepted > 100 and client.queued_bytes() <= MAX_QUEUED - MAINTENANCE_RESERVE, "Application queue bound");
   client.tick(240030); auto maintenance = pop(client);
   require(maintenance.maintenance and parse(maintenance).type IS CREATE_PERMISSION,
      "Maintenance capacity and queue priority reserved");
   Datagram data;
   require(client.receive(maintenance.identity, success(maintenance), 240031, data) IS Result::ACCEPTED,
      "Maintenance progress under application saturation");
   client.close(240032); auto deletion = pop(client);
   require(deletion.maintenance and parse(deletion).type IS REFRESH and client.queued_bytes() IS 0,
      "Teardown replaces saturated application queue");

   Client transactions(config(true)); establish(transactions);
   for (uint8_t i = 0; i < 14; ++i) {
      require(transactions.permission(endpoint(uint8_t(20 + i)), 30), "Fill transactions");
   }
   require(not transactions.permission(endpoint(40), 30) and transactions.pending() IS 14,
      "Reserve transaction slots");
   // Jumping past old request deadlines still permits allocation maintenance.
   transactions.tick(480000); require(transactions.pending() IS 1 and parse(pop(transactions)).type IS REFRESH,
      "Allocation maintenance survives transaction exhaustion");

   Client permissions(config(true)); establish(permissions);
   for (uint8_t i = 0; i < 64; ++i) grant(permissions, endpoint(uint8_t(20 + i)), 30);
   require(permissions.permission_count() IS 64 and not permissions.permission(endpoint(100), 32),
      "Permission limit");
   Client channels(config(true)); establish(channels);
   for (uint16_t i = 0; i < 64; ++i) {
      require(channels.bind(endpoint(8, uint16_t(9000 + i)), uint16_t(0x4000 + i), 30), "Fill channels");
      auto binding = pop(channels);
      require(channels.receive(binding.identity, success(binding), 31, data) IS Result::ACCEPTED, "Channel grant");
   }
   require(channels.channel_count() IS 64 and channels.permission_count() IS 1 and
      not channels.bind(endpoint(8, 10000), 0x4040, 32), "Channel cap and IP-only permissions");
   channels.close(32); require(channels.channel_count() IS 0 and channels.permission_count() IS 0,
      "Close releases all bounded resources");
}
}
int main() {
   codec_tests(); authentication_tests(); state_tests(); adversarial_tests(); limits_tests();
   std::puts("TURN codec, authentication, state, framing and resource tests passed"); return 0;
}
