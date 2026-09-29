#include <kotuku/main.h>
#include "../drivers/wayland/decorations.h"
#include <cstdio>

static int check(bool Condition, const char *Message)
{
   if (Condition) return 0;
   std::fprintf(stderr, "%s\n", Message);
   return 1;
}

int main()
{
   using namespace display;
   int failures = 0;
   WaylandFrame frame(400, 260, true);
   failures += check((frame.Width IS 408) and (frame.Height IS 292), "Frame must preserve the client area");
   failures += check(frame.contains(0, 0) and frame.contains(399.5, 259.5), "Client bounds must include edges");
   failures += check(not frame.contains(400, 0) and not frame.contains(0, -1), "Decorations are outside the client");
   failures += check(frame.edges(-1, -27) IS XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT, "Top left resize corner");
   failures += check(frame.edges(400, 260) IS XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT, "Bottom right resize corner");
   failures += check(frame.edges(20, -12) IS 0, "Title bar must move rather than resize");
   failures += check(frame.control(388, -12) IS 1, "Close button hit test");
   failures += check(frame.control(364, -12) IS 2, "Maximise button hit test");
   failures += check(frame.control(340, -12) IS 3, "Minimise button hit test");
   failures += check(frame.control(388, 0) IS 0, "Content must not activate window controls");
   failures += check(frame.control(388, -27) IS 0, "Top resize edge must not activate window controls");
   failures += check(frame.control(400, -12) IS 0, "Right resize edge must not activate window controls");
   WaylandFrame plain(400, 260, false);
   failures += check((plain.Width IS 400) and (plain.Height IS 260), "Undecorated geometry must equal content");
   failures += check((plain.control(388, -12) IS 0) and (plain.edges(-1, -1) IS 0), "No invisible controls");
   for (const auto &title : { std::string(), std::string("A"), std::string(1024, 'W') }) {
      WaylandFrame narrow(4, 4, true);
      for (int y = 0; y < narrow.Height; y++) for (int x = 0; x < narrow.Width; x++)
         failures += check((frame_pixel(narrow, x, y, title) >> 24) IS 255, "Frame pixels must be opaque");
   }
   failures += check(frame_pixel(frame, 14, 9, "A") != frame_pixel(frame, 14, 9, " "),
      "Changing the title must change its rendered pixels");
   return failures;
}
