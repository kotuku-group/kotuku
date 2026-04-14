// Parser tip system for Tiri.
// Copyright © 2025-2026 Paul Manias

#include "parser_tips.h"

#include <format>
#include <cstdio>

//********************************************************************************************************************

const char* category_name(TipCategory Cat)
{
   switch (Cat) {
      case TipCategory::TypeSafety:     return "type-safety";
      case TipCategory::Performance:    return "performance";
      case TipCategory::CodeQuality:    return "code-quality";
      case TipCategory::BestPractice:   return "best-practice";
      case TipCategory::Style:          return "style";
      case TipCategory::KotukuSpecific: return "kotuku";
   }
   return "unknown";
}

//********************************************************************************************************************
// Format tip message for output.

std::string ParserTip::to_string(std::string_view Filename) const
{
   SourceSpan span = token.span();
   if (Filename.starts_with("=")) Filename.remove_prefix(1);
   else if (Filename.starts_with("@")) Filename.remove_prefix(1);
   return std::format("[TIP] {}:{}:{}: {}: {}", Filename, span.line, span.column, category_name(category), message);
}

//********************************************************************************************************************
// Emit a tip message if it passes the priority filter.

void TipEmitter::emit(const ParserTip &Tip, std::string_view Filename)
{
   if (should_emit(Tip.priority)) {
      tip.push_back(Tip);
      printf("%s\n", Tip.to_string(Filename).c_str());
   }
}

//********************************************************************************************************************
// Convenience method for emitting tips with all fields specified.

void TipEmitter::emit(uint8_t Priority, TipCategory Category, std::string Message, const Token &Location, std::string_view Filename)
{
   ParserTip tip_item {
      .priority = Priority,
      .category = Category,
      .message = std::move(Message),
      .token = Location
   };
   emit(tip_item, Filename);
}
