#pragma once

#include <kotuku/main.h>
#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

// Immutable grey8 coverage, positioned relative to the glyph's integer baseline origin.
struct text_coverage {
   int left = 0;
   int top = 0;
   int width = 0;
   int height = 0;
   int source_left = 0;
   int source_top = 0;
   std::vector<uint8_t> pixels;
};

// Font identifies an ft_point, so sizes and variable-font styles have independent entries.  Callers hold
// glFontMutex for every cache operation.  Shared ownership pins masks while a run blends outside that lock.
class text_coverage_cache {
public:
   static constexpr size_t BYTE_LIMIT = 4 * 1024 * 1024;

   struct key {
      const void *font;
      int glyph;
      int phase;
      bool operator==(const key &) const = default;
   };

private:
   struct key_hash {
      size_t operator()(const key &Key) const {
         return std::hash<const void *>()(Key.font) ^ (size_t(Key.glyph) * 4 + Key.phase);
      }
   };

   struct entry {
      key identity;
      std::shared_ptr<const text_coverage> mask;
      size_t bytes;
   };

   using queue = std::list<entry>;
   using index = std::unordered_map<key, queue::iterator, key_hash>;
   queue lru;
   index entries;
   size_t used = 0;
   size_t limit;

public:
   explicit text_coverage_cache(size_t Limit = BYTE_LIMIT) : limit(Limit) { }
   text_coverage_cache(const text_coverage_cache &) = delete;
   text_coverage_cache & operator=(const text_coverage_cache &) = delete;

   std::shared_ptr<const text_coverage> find(const key &Key) {
      const auto found = entries.find(Key);
      if (found IS entries.end()) return {};
      lru.splice(lru.begin(), lru, found->second);
      return found->second->mask;
   }

   std::shared_ptr<const text_coverage> insert(const key &Key, text_coverage Mask) {
      if (auto existing = find(Key)) return existing;

      // Charge pixels and fixed entry metadata, including empty glyphs, to keep every cache entry bounded.
      const size_t bytes = Mask.pixels.size() + sizeof(text_coverage) + sizeof(entry) + sizeof(index::value_type);
      if (bytes > limit) return {};
      while (used > limit - bytes) {
         const auto &oldest = lru.back();
         used -= oldest.bytes;
         entries.erase(oldest.identity);
         lru.pop_back();
      }
      auto mask = std::make_shared<const text_coverage>(std::move(Mask));
      lru.push_front({ Key, mask, bytes });
      entries.emplace(Key, lru.begin());
      used += bytes;
      return mask;
   }

   void clear() {
      entries.clear();
      lru.clear();
      used = 0;
   }

   size_t bytes() const { return used; }
   size_t size() const { return entries.size(); }
};

extern text_coverage_cache glTextCoverageCache;
