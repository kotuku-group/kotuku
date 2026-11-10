// Colour string conversion for FRGB and RGB8 struct fields.  The Vector module provides the colour parser, so it is
// loaded on first use rather than at Tiri initialisation.

#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>
#ifndef DISABLE_VECTOR
#include <kotuku/modules/vector.h>
#endif
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <string_view>

#include "lauxlib.h"
#include "lj_obj.h"
#include "defs.h"

#ifndef DISABLE_VECTOR
JUMPTABLE_VECTOR
#endif

namespace tiri {
OBJECTPTR modVector = nullptr;
}

using namespace tiri;

#ifndef DISABLE_VECTOR
static ERR load_vector(void)
{
#ifndef KOTUKU_STATIC
   static std::mutex vector_load_lock;
   const std::lock_guard<std::mutex> lock(vector_load_lock);

   if (not modVector) {
      kt::SwitchContext ctx(glTiriContext);
      if (objModule::load("vector", &modVector, &VectorBase) != ERR::Okay) return ERR::InitModule;
   }
#endif
   return ERR::Okay;
}
#endif

//********************************************************************************************************************
// Parses a colour string such as '#ff0000', 'rgb(255,0,0)' or 'red' into an FRGB or RGB8 struct buffer.  Returns
// NoSupport if the struct is not a colour type or the string is a list of numbers, in which case the caller falls
// back to positional CSV parsing.

ERR read_colour_struct(std::string_view Value, std::string_view StructName, APTR Dest)
{
   const bool is_frgb = StructName IS "FRGB";
   if ((not is_frgb) and (StructName != "RGB8")) return ERR::NoSupport;

   while ((not Value.empty()) and std::isspace((unsigned char)Value.front())) Value.remove_prefix(1);
   if (Value.empty()) return ERR::NoSupport;

   const char first = Value.front();
   if (std::isdigit((unsigned char)first) or (first IS '-') or (first IS '+') or (first IS '.')) return ERR::NoSupport;

#ifdef DISABLE_VECTOR
   return ERR::NoSupport;
#else
   if (auto error = load_vector(); error != ERR::Okay) return error;

   VectorPainter painter;
   if (auto error = vec::ReadPainter(nullptr, Value, &painter, nullptr); error != ERR::Okay) return error;

   if (is_frgb) ((FRGB *)Dest)[0] = painter.Colour;
   else {
      auto to_byte = [](float Component) { return uint8_t(std::lround(std::clamp(Component, 0.0f, 1.0f) * 255.0f)); };
      auto rgb = (RGB8 *)Dest;
      rgb->Red   = to_byte(painter.Colour.Red);
      rgb->Green = to_byte(painter.Colour.Green);
      rgb->Blue  = to_byte(painter.Colour.Blue);
      rgb->Alpha = to_byte(painter.Colour.Alpha);
   }
   return ERR::Okay;
#endif
}
