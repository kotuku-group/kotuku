#include "stun.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace rtc::stun {

static uint16_t read16(const uint8_t *Bytes) {
   return (uint16_t(Bytes[0]) << 8) | uint16_t(Bytes[1]);
}

static uint32_t read32(const uint8_t *Bytes) {
   return (uint32_t(read16(Bytes)) << 16) | read16(Bytes + 2);
}

static void write16(uint8_t *Bytes, uint16_t Value) {
   Bytes[0] = uint8_t(Value >> 8);
   Bytes[1] = uint8_t(Value);
}

static void write32(uint8_t *Bytes, uint32_t Value) {
   write16(Bytes, uint16_t(Value >> 16));
   write16(Bytes + 2, uint16_t(Value));
}

static uint32_t crc32(std::span<const uint8_t> Data) {
   uint32_t crc = 0xffffffff;
   for (uint8_t byte : Data) {
      crc ^= byte;
      for (int bit = 0; bit < 8; bit++) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
   }
   return crc ^ 0xffffffff;
}

const Attribute * Message::find(uint16_t Type) const {
   for (const auto &attribute : attributes) if (attribute.type IS Type) return &attribute;
   return nullptr;
}

bool parse(std::span<const uint8_t> Datagram, Message &Result) {
   Result = {};
   if ((Datagram.size() < 20) or (Datagram.size() > 65555)) return false;
   if ((Datagram[0] & 0xc0) or (read32(Datagram.data() + 4) != MAGIC_COOKIE)) return false;
   auto payload_size = read16(Datagram.data() + 2);
   if ((payload_size & 3) or (size_t(payload_size) + 20 != Datagram.size())) return false;

   Message parsed;
   parsed.type = read16(Datagram.data());
   std::copy_n(Datagram.data() + 8, 12, parsed.transaction_id.begin());

   size_t offset = 20;
   while (offset < Datagram.size()) {
      if (Datagram.size() - offset < 4) return false;
      auto length = read16(Datagram.data() + offset + 2);
      auto padded = (size_t(length) + 3) & ~size_t(3);
      if (padded > Datagram.size() - offset - 4) return false;
      parsed.attributes.push_back({read16(Datagram.data() + offset), Datagram.subspan(offset + 4, length), offset});
      offset += 4 + padded;
   }

   Result = std::move(parsed);
   return true;
}

static bool hmac_at(std::span<const uint8_t> Datagram, size_t Offset, std::span<const uint8_t> Key,
   bool Sha256, std::span<uint8_t> Result) {
   if ((Offset < 20) or (Offset > Datagram.size()) or (Key.size() > size_t(std::numeric_limits<int>::max()))) {
      return false;
   }
   auto prefix = std::vector<uint8_t>(Datagram.begin(), Datagram.begin() + Offset);
   auto size = Sha256 ? 32 : 20;
   write16(prefix.data() + 2, uint16_t(Offset + 4 + size - 20));
   unsigned int written = 0;
   const auto *algorithm = Sha256 ? EVP_sha256() : EVP_sha1();
   return HMAC(algorithm, Key.data(), int(Key.size()), prefix.data(), prefix.size(), Result.data(), &written) and
      (written IS Result.size());
}

bool verify_integrity(std::span<const uint8_t> Datagram, const Message &Parsed,
   std::span<const uint8_t> Key, bool Sha256) {
   auto attribute = Parsed.find(Sha256 ? ATTR_MESSAGE_INTEGRITY_SHA256 : ATTR_MESSAGE_INTEGRITY);
   auto size = Sha256 ? 32 : 20;
   if ((!attribute) or (attribute->value.size() != size_t(size)) or
       (attribute->offset + 4 + size > Datagram.size())) return false;
   std::array<uint8_t, 32> expected{};
   if (not hmac_at(Datagram, attribute->offset, Key, Sha256, std::span<uint8_t>(expected.data(), size))) return false;
   return CRYPTO_memcmp(expected.data(), attribute->value.data(), size) IS 0;
}

bool verify_fingerprint(std::span<const uint8_t> Datagram, const Message &Parsed) {
   auto attribute = Parsed.find(ATTR_FINGERPRINT);
   if ((!attribute) or (attribute->value.size() != 4) or (attribute->offset + 8 != Datagram.size())) return false;
   return read32(attribute->value.data()) IS (crc32(Datagram.first(attribute->offset)) ^ 0x5354554e);
}

bool decode_xor_address(const Attribute &AttributeValue, const Message &Parsed,
   std::array<uint8_t, 16> &Address, uint16_t &Port, bool &IPv6) {
   const auto data = AttributeValue.value;
   if ((AttributeValue.type != ATTR_XOR_MAPPED_ADDRESS) or (data.size() < 4) or data[0]) return false;
   IPv6 = data[1] IS 2;
   if ((data[1] != 1) and (!IPv6)) return false;
   auto length = IPv6 ? 16 : 4;
   if (data.size() != size_t(4 + length)) return false;
   Port = read16(data.data() + 2) ^ uint16_t(MAGIC_COOKIE >> 16);
   std::array<uint8_t, 16> mask{};
   write32(mask.data(), MAGIC_COOKIE);
   std::copy(Parsed.transaction_id.begin(), Parsed.transaction_id.end(), mask.begin() + 4);
   Address.fill(0);
   for (int i = 0; i < length; i++) Address[i] = data[4 + i] ^ mask[i];
   return true;
}

bool long_term_key(std::string_view Username, std::string_view Realm, std::string_view Password,
   std::vector<uint8_t> &Key, bool Sha256) {
   auto text = std::string(Username) + ":" + std::string(Realm) + ":" + std::string(Password);
   Key.resize(Sha256 ? 32 : 16);
   unsigned int written = 0;
   if (EVP_Digest(text.data(), text.size(), Key.data(), &written,
       Sha256 ? EVP_sha256() : EVP_md5(), nullptr) != 1) {
      Key.clear();
      return false;
   }
   return written IS Key.size();
}

static bool append_attribute(std::vector<uint8_t> &Result, uint16_t Type, std::span<const uint8_t> Value) {
   if (Value.size() > 65535) return false;
   auto padded = (Value.size() + 3) & ~size_t(3);
   if (Result.size() + 4 + padded > 65555) return false;
   auto offset = Result.size();
   Result.resize(offset + 4 + padded, 0);
   write16(Result.data() + offset, Type);
   write16(Result.data() + offset + 2, uint16_t(Value.size()));
   std::copy(Value.begin(), Value.end(), Result.begin() + offset + 4);
   write16(Result.data() + 2, uint16_t(Result.size() - 20));
   return true;
}

bool encode(uint16_t Type, std::span<const uint8_t, 12> TransactionId, std::span<const Attribute> Attributes,
   std::span<const uint8_t> Key, bool Sha256, bool Fingerprint, std::vector<uint8_t> &Result) {
   Result.assign(20, 0);
   write16(Result.data(), Type);
   write32(Result.data() + 4, MAGIC_COOKIE);
   std::copy(TransactionId.begin(), TransactionId.end(), Result.begin() + 8);

   for (const auto &attribute : Attributes) {
      if ((attribute.type IS ATTR_MESSAGE_INTEGRITY) or (attribute.type IS ATTR_MESSAGE_INTEGRITY_SHA256) or
          (attribute.type IS ATTR_FINGERPRINT) or (!append_attribute(Result, attribute.type, attribute.value))) {
         Result.clear();
         return false;
      }
   }

   if (not Key.empty()) {
      std::array<uint8_t, 32> digest{};
      auto size = Sha256 ? 32 : 20;
      if ((Result.size() + 4 + size + (Fingerprint ? 8 : 0) > 65555) or
          (not hmac_at(Result, Result.size(), Key, Sha256, std::span<uint8_t>(digest.data(), size))) or
          (not append_attribute(Result, Sha256 ? ATTR_MESSAGE_INTEGRITY_SHA256 : ATTR_MESSAGE_INTEGRITY,
             std::span<const uint8_t>(digest.data(), size)))) {
         Result.clear();
         return false;
      }
   }

   if (Fingerprint) {
      if (Result.size() + 8 > 65555) { Result.clear(); return false; }
      write16(Result.data() + 2, uint16_t(Result.size() + 8 - 20));
      std::array<uint8_t, 4> fingerprint{};
      write32(fingerprint.data(), crc32(Result) ^ 0x5354554e);
      if (not append_attribute(Result, ATTR_FINGERPRINT, fingerprint)) { Result.clear(); return false; }
   }
   return true;
}

}
