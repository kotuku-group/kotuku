// Parser tip system for Tiri.
// Copyright © 2025-2026 Paul Manias

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "token_types.h"

// Categories of tip messages.

enum class TipCategory : uint8_t {
   TypeSafety,
   Performance,
   CodeQuality,
   BestPractice,
   Style,
   KotukuSpecific
};

// Get the display name for a tip category.

[[nodiscard]] const char* category_name(TipCategory Cat);

//********************************************************************************************************************
// Individual tip message with location information.

struct ParserTip {
   uint8_t priority;           // 1 = critical, 2 = medium, 3 = low
   TipCategory category;
   std::string message;
   Token token;                // Location information

   [[nodiscard]] std::string to_string(std::string_view Filename) const;
};

//********************************************************************************************************************
// Collects and filters tip messages based on the configured level.

class TipEmitter {
public:
   explicit TipEmitter(uint8_t Level) : level(Level) {}

   // Returns true if tip at given priority should be emitted.
   [[nodiscard]] bool should_emit(uint8_t Priority) const {
      #ifdef INCLUDE_TIPS
         return level > 0 and Priority <= level;
      #else
         return false;
      #endif
   }

   // Emit a tip message if it passes the priority filter.
   void emit(const ParserTip &Tip, std::string_view Filename = {});

   // Convenience method for emitting tips with all fields specified.
   void emit(uint8_t Priority, TipCategory, std::string, const Token &, std::string_view = {});

   [[nodiscard]] const std::vector<ParserTip>& entries() const { return tip; }
   [[nodiscard]] bool has_tip() const { return not tip.empty(); }
   [[nodiscard]] size_t count() const { return tip.size(); }

private:
   uint8_t level;
   std::vector<ParserTip> tip;
};
