// Internal, object-independent mDNS address service.  All work runs on the caller's task thread.
#pragma once
#include <kotuku/config.h>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace net::mdns {
struct Address {
   std::array<uint8_t, 16> bytes{};
   uint32_t interface_index = 0;
   bool ipv6 = false;
   bool operator==(const Address &) const = default;
};

struct Question { std::string name; uint16_t type = 255; bool unicast = false; };

struct Record {
   std::string name;
   Address address;
   uint32_t ttl = 120;
   bool flush = true;
   uint8_t section = 0; // Answer, authority, additional.
};

struct Message {
   bool response = false;
   std::vector<Question> questions;
   std::vector<Record> records;
};

bool valid_name(std::string_view Name);
bool usable_address(const Address &Value);
bool decode(std::span<const uint8_t> Packet, Message &Result);
std::vector<uint8_t> encode(const Message &Value);

struct Packet { uint32_t interface_index; bool ipv6; std::vector<uint8_t> bytes; };
enum class Status { PROBING, READY, FAILED };
struct Registration { std::string name; Address address; Status status = Status::PROBING; };
struct Resolution { std::string name; std::vector<Address> addresses; };

// At most 32 records, 64 pending names, eight answers per name, and 256 scoped cache entries.
// Resolution takes at most three seconds.  No unicast DNS fallback or unsolicited cache insertion.

class Engine {
public:
   bool register_name(std::string_view Name, const Address &Value, uint64_t NowUs);
   bool resolve(std::string_view Name, std::span<const Address> Scopes, uint64_t NowUs);
   void receive(std::span<const uint8_t> Packet, uint32_t Interface, bool IPv6,
      uint16_t SourcePort, int HopLimit, uint64_t NowUs);
   void tick(uint64_t NowUs);
   void withdraw();
   void failed(uint32_t Interface, bool IPv6);
   bool pending() const;
   std::vector<Registration> registrations() const;
   std::vector<Resolution> take_results();
   std::vector<Packet> take_output();
private:
   struct Local {
      Registration value;
      unsigned probes = 0;
      uint64_t next = 0, defence = 0, last_reply = 0;
   };
   struct Query {
      std::string name;
      std::vector<Address> scopes, answers;
      uint64_t expires = 0, next = 0, settle = 0;
      unsigned attempts = 0;
      size_t next_scope = 0;
   };
   struct Cached { std::string name; Address address; uint64_t expires; uint64_t received; };
   bool emit(const Message &Value, uint32_t Interface, bool IPv6);
   void announce(Local &Value, uint32_t TTL);
   std::vector<Local> locals;
   std::vector<Query> queries;
   std::vector<Cached> cache;
   std::vector<Resolution> results;
   std::vector<Packet> output;
};

// Scoped multicast sockets, polled non-blockingly with a finite packet budget.  Separate instances share
// UDP 5353 with system responders, but own independent registrations and caches; destruction sends goodbyes.

class Service {
public:
   Service();
   ~Service();
   Service(const Service &) = delete;
   Service & operator=(const Service &) = delete;
   bool open(bool IPv6);
   bool register_address(std::string_view Name, const Address &Value, uint64_t NowUs);
   bool resolve(std::string_view Name, uint64_t NowUs);
   void tick(uint64_t NowUs);
   void withdraw();
   Engine engine;
   const std::vector<Address> & scopes() const { return interfaces; }
private:
   void flush();
   intptr_t socket4 = -1, socket6 = -1;
   std::vector<Address> interfaces;
   std::vector<Packet> pending_output;
};
}
