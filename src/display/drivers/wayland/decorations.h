#pragma once

#include "xdg-shell-client-protocol.h"
#include <cstdint>
#include <string>

// Software decoration geometry uses logical coordinates.  Application buffers contain only the client area.

namespace display {

struct WaylandFrame {
   int Border = 0;
   int Top = 0;
   int Width;
   int Height;

   WaylandFrame(int ClientWidth, int ClientHeight, bool Decorated)
      : Border(Decorated ? 4 : 0), Top(Decorated ? 28 : 0),
        Width(ClientWidth + Border * 2), Height(ClientHeight + Top + Border) { }

   bool contains(double X, double Y) const {
      return (X >= 0) and (Y >= 0) and (X < Width - Border * 2) and (Y < Height - Top - Border);
   }

   uint32_t edges(double X, double Y) const {
      if (not Border) return 0;
      uint32_t result = 0;
      if (Y < Border - Top) result |= XDG_TOPLEVEL_RESIZE_EDGE_TOP;
      else if (Y >= Height - Top - Border) result |= XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
      if (X < 0) result |= XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
      else if (X >= Width - Border * 2) result |= XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
      return result;
   }

   // Controls occupy three 24-pixel cells at the right of the title bar: minimise, maximise and close.
   int control(double X, double Y) const {
      if ((not Border) or (Y < Border - Top) or (Y >= 0) or (X < 0) or (X >= Width - Border * 2)) return 0;
      const int distance = Width - Border * 2 - int(X);
      return distance <= 24 ? 1 : distance <= 48 ? 2 : distance <= 72 ? 3 : 0;
   }
};

// A compact fallback title font avoids a dependency from the display backend back to the font module.
// Lower-case Latin letters share the capital glyphs; unsupported codepoints use a replacement box.

inline uint32_t title_glyph(unsigned char Character)
{
   constexpr uint32_t letters[] = {
      0x118fe2e, 0x0f8be2f, 0x1e0843e, 0x0f8c62f, 0x1f0bc3f, 0x010bc3f,
      0x1e8f43e, 0x118fe31, 0x1f2109f, 0x064a11c, 0x1149d31, 0x1f08421,
      0x118d771, 0x11cd671, 0x0e8c62e, 0x010be2f, 0x164d62e, 0x114be2f,
      0x0f8383e, 0x042109f, 0x0e8c631, 0x0454631, 0x11dd631, 0x1151151,
      0x0421151, 0x1f1111f
   };
   constexpr uint32_t digits[] = {
      0x0e9d72e, 0x0e210c4, 0x1f2222e, 0x0f8320f, 0x0847d29, 0x0f83c3f,
      0x0e8bc2e, 0x021111f, 0x0e8ba2e, 0x0e87a2e
   };
   if ((Character >= 'a') and (Character <= 'z')) Character -= 'a' - 'A';
   if ((Character >= 'A') and (Character <= 'Z')) return letters[Character - 'A'];
   if ((Character >= '0') and (Character <= '9')) return digits[Character - '0'];
   if (Character IS ' ') return 0;
   if (Character IS '-') return 0x0007c00;
   if (Character IS '.') return 0x0400000;
   return 0x1f8c63f;
}

inline uint32_t frame_pixel(const WaylandFrame &Frame, int X, int Y, const std::string &Title)
{
   constexpr uint32_t background = 0xff303844;
   constexpr uint32_t foreground = 0xffedf0f4;
   if ((X < Frame.Border) or (X >= Frame.Width - Frame.Border) or (Y < Frame.Border) or (Y >= Frame.Top))
      return 0xff202630;
   const int control = Frame.control(X - Frame.Border, Y - Frame.Top);
   if (control) {
      const int x = X - (Frame.Width - Frame.Border - control * 24) - 7;
      const int y = Y - 10;
      if ((x >= 0) and (x < 10) and (y >= 0) and (y < 10)) {
         if ((control IS 1) and ((x IS y) or (x + y IS 9))) return foreground;
         if ((control IS 2) and ((x IS 0) or (x IS 9) or (y IS 0) or (y IS 9))) return foreground;
         if ((control IS 3) and (y IS 8)) return foreground;
      }
      return control IS 1 ? 0xff733842 : background;
   }
   const int text_x = X - 12;
   const int text_y = Y - 9;
   if ((text_x >= 0) and (text_y >= 0) and (text_y < 10) and
         (X < Frame.Width - Frame.Border - 76)) {
      const size_t index = size_t(text_x / 12);
      const int column = (text_x % 12) / 2;
      if ((index < Title.size()) and (column < 5) and
            (title_glyph(uint8_t(Title[index])) & (1u << (text_y / 2 * 5 + column)))) return foreground;
   }
   return background;
}

}
