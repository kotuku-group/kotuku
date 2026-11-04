#include "mdns.h"
#include <algorithm>
#include <cctype>
#include <utility>

namespace net::mdns {

namespace {
uint16_t word(std::span<const uint8_t> Packet, size_t Offset) {
   return uint16_t((uint16_t(Packet[Offset]) << 8) | Packet[Offset + 1]);
}

void put(std::vector<uint8_t> &Packet, uint16_t Value) {
   Packet.push_back(uint8_t(Value >> 8)); Packet.push_back(uint8_t(Value));
}

bool name(std::span<const uint8_t> Packet, size_t &Offset, std::string &Result) {
   size_t cursor = Offset, end = 0;
   std::array<size_t, 128> visited{};
   size_t steps = 0;
   Result.clear();

   while (cursor < Packet.size() and steps < visited.size()) {
      if (std::find(visited.begin(), visited.begin() + steps, cursor) != visited.begin() + steps) return false;
      visited[steps++] = cursor;
      auto length = Packet[cursor++];

      if ((length & 0xc0) IS 0xc0) {
         if (cursor >= Packet.size()) return false;
         auto target = size_t(((length & 0x3f) << 8) | Packet[cursor++]);
         if (not end) end = cursor;
         if (target < 12 or target >= Packet.size()) return false;
         cursor = target;
      }
      else if (length & 0xc0) return false;
      else if (not length) { Offset = end ? end : cursor; return Result.size() <= 253; }
      else {
         if (cursor + length > Packet.size() or Result.size() + length + 1 > 254) return false;
         if (not Result.empty()) Result += '.';
         for (size_t i = 0; i < length; ++i) {
            auto ch = Packet[cursor++];
            // The internal service supports ASCII host names; other DNS names are safely ignored.
            if (ch < 33 or ch > 126 or ch IS '.') return false;
            Result += char(std::tolower(ch));
         }
      }
   }

   return false;
}

bool put_name(std::vector<uint8_t> &Packet, std::string_view Name) {
   if (not valid_name(Name)) return false;
   auto dot = Name.find('.');
   Packet.push_back(uint8_t(dot));
   Packet.insert(Packet.end(), Name.begin(), Name.begin() + dot);
   Packet.push_back(5); Packet.insert(Packet.end(), {'l', 'o', 'c', 'a', 'l', 0});
   return true;
}

std::string lower(std::string_view Text) {
   std::string result(Text);
   for (auto &ch : result) ch = char(std::tolower((unsigned char)ch));
   return result;
}

bool scoped(const Address &Value, const Address &Scope) {
   return Value.interface_index IS Scope.interface_index and Value.ipv6 IS Scope.ipv6;
}

}

bool valid_name(std::string_view Name) {
   if (Name.size() < 7 or Name.size() > 69) return false;
   auto text = lower(Name);
   if (not text.ends_with(".local")) return false;
   auto label = std::string_view(text).substr(0, text.size() - 6);
   if (label.empty() or label.size() > 63 or label.front() IS '-' or label.back() IS '-') return false;
   for (char ch : label) if (not ((ch >= 'a' and ch <= 'z') or (ch >= '0' and ch <= '9') or ch IS '-')) {
      return false;
   }
   return true;
}

bool usable_address(const Address &Value) {
   if (not Value.interface_index) return false;

   if (Value.ipv6) {
      if (Value.bytes[0] IS 0xff or (Value.bytes[0] IS 0xfe and (Value.bytes[1] & 0xc0) IS 0x80)) return false;
      bool prefix_zero = std::all_of(Value.bytes.begin(), Value.bytes.begin() + 15,
         [](uint8_t Byte) { return Byte IS 0; });
      if (prefix_zero and Value.bytes[15] <= 1) return false; // Unspecified and loopback.
      bool mapped = std::all_of(Value.bytes.begin(), Value.bytes.begin() + 10,
         [](uint8_t Byte) { return Byte IS 0; }) and Value.bytes[10] IS 255 and Value.bytes[11] IS 255;
      return not mapped;
   }

   return Value.bytes[0] != 0 and Value.bytes[0] < 224 and Value.bytes[0] != 127;
}

bool decode(std::span<const uint8_t> Packet, Message &Result) {
   if (Packet.size() < 12 or Packet.size() > 9000 or word(Packet, 0) != 0) return false;
   auto flags = word(Packet, 2);
   if ((flags & 0x7bff) != 0) return false; // No opcode, truncation, recursion or error responses.
   auto questions = word(Packet, 4);
   auto answers = word(Packet, 6), authority = word(Packet, 8), additional = word(Packet, 10);
   if (size_t(questions) + answers + authority + additional > 128) return false;
   Message parsed;
   parsed.response = (flags & 0x8000) != 0;
   size_t offset = 12;
   for (unsigned i = 0; i < questions; ++i) {
      Question question;
      if (not name(Packet, offset, question.name) or offset + 4 > Packet.size()) return false;
      question.type = word(Packet, offset);
      auto cls = word(Packet, offset + 2); offset += 4;
      question.unicast = (cls & 0x8000) != 0;
      if ((cls & 0x7fff) IS 1 and valid_name(question.name)) parsed.questions.push_back(std::move(question));
   }

   for (unsigned section = 0; section < 3; ++section) {
      auto count = section IS 0 ? answers : (section IS 1 ? authority : additional);
      for (unsigned i = 0; i < count; ++i) {
         Record record;
         if (not name(Packet, offset, record.name) or offset + 10 > Packet.size()) return false;
         auto type = word(Packet, offset), cls = word(Packet, offset + 2);
         record.ttl = (uint32_t(word(Packet, offset + 4)) << 16) | word(Packet, offset + 6);
         auto length = word(Packet, offset + 8); offset += 10;
         if (offset + length > Packet.size()) return false;
         if ((cls & 0x7fff) IS 1 and valid_name(record.name) and
             ((type IS 1 and length IS 4) or (type IS 28 and length IS 16))) {
            record.address.ipv6 = type IS 28;
            record.flush = (cls & 0x8000) != 0;
            record.section = uint8_t(section);
            std::copy_n(Packet.begin() + offset, length, record.address.bytes.begin());
            parsed.records.push_back(std::move(record));
         } else if ((type IS 1 and length != 4) or (type IS 28 and length != 16)) return false;
         offset += length;
      }
   }

   if (offset != Packet.size()) return false;
   Result = std::move(parsed);
   return true;
}

std::vector<uint8_t> encode(const Message &Value) {
   if (Value.questions.size() + Value.records.size() > 128 or
       std::any_of(Value.records.begin(), Value.records.end(), [](const Record &Record) {
          return Record.section > 2;
       })) return {};

   std::vector<uint8_t> packet(12, 0);
   packet[2] = Value.response ? 0x84 : 0;
   packet[5] = uint8_t(Value.questions.size());
   for (unsigned section = 0; section < 3; ++section) {
      packet[7 + section * 2] = uint8_t(std::count_if(Value.records.begin(), Value.records.end(),
         [section](const Record &Record) { return Record.section IS section; }));
   }

   for (const auto &question : Value.questions) {
      if (not put_name(packet, question.name)) return {};
      put(packet, question.type); put(packet, question.unicast ? 0x8001 : 1);
   }

   for (unsigned section = 0; section < 3; ++section) for (const auto &record : Value.records) {
      if (record.section != section) continue;
      if (not put_name(packet, record.name)) return {};
      put(packet, record.address.ipv6 ? 28 : 1); put(packet, record.flush ? 0x8001 : 1);
      put(packet, uint16_t(record.ttl >> 16)); put(packet, uint16_t(record.ttl));
      auto size = record.address.ipv6 ? 16 : 4;
      put(packet, uint16_t(size));
      packet.insert(packet.end(), record.address.bytes.begin(), record.address.bytes.begin() + size);
   }

   return packet.size() > 9000 ? std::vector<uint8_t>{} : packet;
}

bool Engine::emit(const Message &Value, uint32_t Interface, bool IPv6) {
   if (output.size() >= 128) return false;
   auto packet = encode(Value);
   if (packet.empty()) return false;
   output.push_back({Interface, IPv6, std::move(packet)});
   return true;
}

void Engine::announce(Local &Value, uint32_t TTL) {
   Message message; message.response = true;
   message.records.push_back({Value.value.name, Value.value.address, TTL});
   emit(message, Value.value.address.interface_index, Value.value.address.ipv6);
}

bool Engine::register_name(std::string_view Name, const Address &Value, uint64_t NowUs) {
   if (not valid_name(Name) or not usable_address(Value) or locals.size() >= 32) return false;
   auto text = lower(Name);
   for (const auto &local : locals) if (local.value.name IS text) return false;
   // The caller supplies cryptographically random names; use their bytes for a 0–250 ms probe delay.
   uint32_t jitter = 0;
   for (char ch : text) jitter = jitter * 33 + uint8_t(ch);
   locals.push_back({{text, Value}, 0, NowUs + jitter % 250001});
   return true;
}

bool Engine::resolve(std::string_view Name, std::span<const Address> Scopes, uint64_t NowUs)
{
   if (not valid_name(Name) or Scopes.empty() or Scopes.size() > 32 or results.size() >= 64) return false;

   auto text = lower(Name);

   for (const auto &query : queries) if (query.name IS text) return true;

   if (queries.size() >= 64) return false;
   Query query; query.name = text; query.expires = NowUs + 3000000; query.next = NowUs;

   for (const auto &scope : Scopes) {
      if (std::none_of(query.scopes.begin(), query.scopes.end(), [&](const Address &Value) {
          return scoped(Value, scope);
      })) query.scopes.push_back(scope);
   }

   for (const auto &entry : cache) if (entry.name IS text and entry.expires > NowUs and
       std::any_of(query.scopes.begin(), query.scopes.end(), [&](const Address &Scope) {
          return scoped(entry.address, Scope);
       }) and query.answers.size() < 8) query.answers.push_back(entry.address);
   if (not query.answers.empty()) query.settle = NowUs + 100000;
   queries.push_back(std::move(query));
   return true;
}

void Engine::receive(std::span<const uint8_t> Packet, uint32_t Interface, bool IPv6,
   uint16_t SourcePort, int HopLimit, uint64_t NowUs)
{
   if (not Interface or SourcePort != 5353 or HopLimit != 255) return;
   Message message;
   if (not decode(Packet, message)) return;

   for (auto record : message.records) {
      record.address.interface_index = Interface;
      // IPv4 mDNS may carry AAAA (and vice versa).  Cache keys use the answer family and receiving interface.
      if (not usable_address(record.address)) continue;
      for (auto &local : locals) {
         if (local.value.status IS Status::FAILED or local.value.name != record.name or
             local.value.address.interface_index != Interface or local.value.address.ipv6 != record.address.ipv6 or
             local.value.address.bytes IS record.address.bytes or not record.ttl) continue;
         if (not message.response and record.section != 1) continue;
         if (local.value.status IS Status::PROBING or (local.defence and NowUs - local.defence < 1000000)) {
            if (local.value.status IS Status::READY) announce(local, 0);
            local.value.status = Status::FAILED;
         } else { local.defence = NowUs; announce(local, 120); }
      }

      if (not message.response) continue;

      for (auto &query : queries) {
         if (query.name != record.name or query.expires <= NowUs or
             std::none_of(query.scopes.begin(), query.scopes.end(), [&](const Address &Scope) {
                return scoped(record.address, Scope);
             })) continue;
         auto existing = std::find(query.answers.begin(), query.answers.end(), record.address);
         if (not record.ttl) {
            if (existing != query.answers.end()) query.answers.erase(existing);
         } else if (existing IS query.answers.end() and query.answers.size() < 8) {
            query.answers.push_back(record.address);
            if (not query.settle) query.settle = NowUs + 100000;
         }
      }

      bool solicited = std::any_of(queries.begin(), queries.end(), [&](const Query &Query) {
         return Query.name IS record.name and Query.expires > NowUs and
            std::any_of(Query.scopes.begin(), Query.scopes.end(), [&](const Address &Scope) {
               return scoped(record.address, Scope);
            });
      });

      bool retained = std::any_of(cache.begin(), cache.end(), [&](const Cached &Cached) {
         return Cached.name IS record.name and Cached.address IS record.address;
      });

      if (solicited or retained) {
         if (record.flush) for (auto &entry : cache) {
            if (entry.name IS record.name and scoped(entry.address, record.address) and
                entry.address != record.address and NowUs - entry.received > 1000000) {
               entry.expires = std::min(entry.expires, NowUs + 1000000);
            }
         }
         auto cached = std::find_if(cache.begin(), cache.end(), [&](const Cached &Value) {
            return Value.name IS record.name and Value.address IS record.address;
         });
         auto expiry = NowUs + uint64_t(std::min(record.ttl, 120u)) * 1000000;
         if (cached != cache.end()) {
            cached->expires = record.ttl ? expiry : NowUs + 1000000; cached->received = NowUs;
         } else if (record.ttl and cache.size() < 256) cache.push_back({record.name, record.address, expiry, NowUs});
      }
   }

   if (message.response) return;

   for (auto &local : locals) {
      if (local.value.status != Status::READY or local.value.address.interface_index != Interface or
          local.value.address.ipv6 != IPv6 or (local.last_reply and NowUs - local.last_reply < 1000000)) continue;

      for (const auto &question : message.questions) {
         if (question.name != local.value.name or (question.type != 255 and
             question.type != (local.value.address.ipv6 ? 28 : 1))) continue;
         auto known = std::any_of(message.records.begin(), message.records.end(), [&](const Record &Record) {
            return Record.section IS 0 and Record.name IS local.value.name and
               Record.address.bytes IS local.value.address.bytes and
               Record.address.ipv6 IS local.value.address.ipv6 and Record.ttl >= 60;
         });
         if (not known) { announce(local, 120); local.last_reply = NowUs; }
         break;
      }
   }
}

void Engine::tick(uint64_t NowUs) {
   std::erase_if(cache, [NowUs](const Cached &Value) { return Value.expires <= NowUs; });
   for (auto &local : locals) {
      if (local.value.status IS Status::FAILED or local.next > NowUs) continue;
      if (local.value.status IS Status::PROBING) {
         if (local.probes < 3) {
            Message probe; probe.questions.push_back({local.value.name, 255});
            probe.records.push_back({local.value.name, local.value.address, 120, false, 1});
            emit(probe, local.value.address.interface_index, local.value.address.ipv6);
            ++local.probes; local.next = NowUs + 250000;
         } else { local.value.status = Status::READY; announce(local, 120); local.next = NowUs + 1000000; }
      } else { announce(local, 120); local.next = NowUs + 60000000; }
   }

   for (auto it = queries.begin(); it != queries.end();) {
      if (NowUs >= it->expires or (it->settle and NowUs >= it->settle and not it->answers.empty())) {
         if (results.size() < 64) results.push_back({it->name, std::move(it->answers)});
         it = queries.erase(it); continue;
      }

      if (NowUs >= it->next) {
         Message query; query.questions.push_back({it->name, 255});
         // Resume an interrupted round at its first unsent scope without consuming a retry.
         while (it->next_scope < it->scopes.size()) {
            const auto &scope = it->scopes[it->next_scope];
            if (not emit(query, scope.interface_index, scope.ipv6)) break;
            ++it->next_scope;
         }
         if (it->next_scope IS it->scopes.size()) {
            it->next_scope = 0;
            it->next = NowUs + (it->attempts++ ? 1000000 : 500000);
         }
      }

      ++it;
   }
}

void Engine::withdraw() {
   for (auto &local : locals) if (local.value.status IS Status::READY) announce(local, 0);
   locals.clear(); queries.clear(); cache.clear(); results.clear();
}

void Engine::failed(uint32_t Interface, bool IPv6) {
   for (auto &local : locals) if (local.value.address.interface_index IS Interface and
       local.value.address.ipv6 IS IPv6) {
      local.value.status = Status::FAILED;
   }
}

bool Engine::pending() const {
   return not queries.empty() or std::any_of(locals.begin(), locals.end(), [](const Local &Value) {
      return Value.value.status IS Status::PROBING;
   });
}

std::vector<Registration> Engine::registrations() const {
   std::vector<Registration> result;
   for (const auto &local : locals) result.push_back(local.value);
   return result;
}

std::vector<Resolution> Engine::take_results() { auto result = std::move(results); results.clear(); return result; }
std::vector<Packet> Engine::take_output() { auto result = std::move(output); output.clear(); return result; }

}
