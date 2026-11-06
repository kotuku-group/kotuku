// Exercise the adapter's actual platform implementation, including operations unavailable through the public TDL.
#include <kotuku/startup.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/vector.h>
#include <kotuku/strings.hpp>

#include <cstdarg>
#include <cstdio>
#include <format>

#include "../engine_headers.h"
#include "../scintillakotuku.h"

JUMPTABLE_VECTOR
JUMPTABLE_DISPLAY

#ifdef KOTUKU_STATIC
#include "../kt_classes.h"
#include "../kt_surface.h"
#else
#define DBGDRAW(...)
#include "../kt_classes.cxx"
#include "../kt_surface.cxx"
#endif

CSTRING ProgName = "ScintillaSurface";

static bool run_checks()
{
   int failures = 0;
   auto expect = [&failures](bool Condition, const char *Description) {
      if (not Condition) { std::fprintf(stderr, "%s\n", Description); failures++; }
   };
   auto scene = objVectorScene::create::global(fl::PageWidth(120), fl::PageHeight(100));
   if (not scene) return false;
   auto release_scene = kt::Defer([scene]() { FreeResource(scene); });
   auto viewport = objVectorViewport::create::global(fl::Owner(scene->UID), fl::Width(120), fl::Height(100));
   if (not viewport) return false;
   auto editor = objScintilla::create::global(fl::Owner(viewport->UID), fl::Viewport(viewport),
      fl::Face("Clean"), fl::FontSize(11));
   if (not editor) return false;
   auto bitmap = objBitmap::create::global(fl::Width(120), fl::Height(100), fl::BitsPerPixel(32));
   if (not bitmap) return false;
   auto release_bitmap = kt::Defer([bitmap]() { FreeResource(bitmap); });

   SurfacePan surface;
   surface.Init(bitmap, editor);
   const PRectangle bounds(0, 0, 120, 100);
   const ColourRGBA white(255, 255, 255), black(0, 0, 0), red(255, 0, 0), blue(0, 0, 255);
   auto pixel = [bitmap](int X, int Y) {
      RGB8 *colour;
      gfx::ReadRGBPixel(bitmap, X, Y, &colour);
      return ColourRGBA(colour->Red, colour->Green, colour->Blue);
   };
   auto clear = [&]() { surface.FillRectangle(bounds, white); };
   auto is_colour = [](ColourRGBA A, ColourRGBA B) { return A.AsInteger() IS B.AsInteger(); };
   auto ink = [&]() {
      int count = 0;
      for (int y = 0; y < 100; y++) {
         for (int x = 0; x < 120; x++) if (not is_colour(pixel(x, y), white)) count++;
      }
      return count;
   };
   clear();
   const auto saved = bitmap->Clip;
   surface.SetClip(PRectangle(10, 10, 70, 70));
   surface.SetClip(PRectangle(20, 20, 80, 80));
   surface.FillRectangle(bounds, red);
   expect(is_colour(pixel(25, 25), red) and is_colour(pixel(15, 15), white), "Nested clips intersect");
   surface.PopClip();
   surface.FillRectangle(PRectangle(0, 0, 20, 20), blue);
   expect(is_colour(pixel(15, 15), blue) and is_colour(pixel(5, 5), white), "PopClip restores its parent");
   surface.SetClip(PRectangle(90, 90, 100, 100));
   surface.FillRectangle(bounds, black);
   surface.PopClip();
   surface.PopClip();
   expect(bitmap->Clip.Left IS saved.Left and bitmap->Clip.Right IS saved.Right and
      bitmap->Clip.Top IS saved.Top and bitmap->Clip.Bottom IS saved.Bottom, "Drawing restores the bitmap clip");
   expect(is_colour(pixel(90, 90), white), "An empty clip draws nothing");

   clear();
   surface.LineDraw(Point(10, 10.5), Point(60, 10.5), Stroke(black));
   expect(is_colour(pixel(25, 10), black), "Half-pixel lines occupy one solid pixel row");
   expect(is_colour(pixel(25, 9), white) and is_colour(pixel(25, 11), white), "Lines have no additional offset");
   const auto thin_ink = ink();
   clear();
   surface.LineDraw(Point(10, 10.5), Point(60, 10.5), Stroke(black, 3.5));
   expect(ink() > thin_ink * 3, "Fractional stroke widths affect coverage");

   const FillStroke colours(red, blue, 3);
   const PRectangle shape(10, 10, 60, 40);
   clear();
   surface.RectangleDraw(shape, colours);
   expect(is_colour(pixel(30, 20), red) and is_colour(pixel(30, 11), blue),
      "Rectangles preserve fill and stroke width");
   clear();
   surface.RectangleFrame(shape, Stroke(blue, 3));
   expect(is_colour(pixel(30, 20), white) and is_colour(pixel(30, 11), blue), "Frames leave the interior untouched");
   clear();
   surface.RoundedRectangle(shape, colours);
   expect(is_colour(pixel(30, 20), red), "Rounded rectangles fill their interior");
   clear();
   surface.AlphaRectangle(shape, 3.5, colours);
   expect(is_colour(pixel(30, 20), red) and is_colour(pixel(30, 11), blue), "Alpha rectangles preserve stroke width");
   clear();
   surface.Ellipse(shape, colours);
   expect(is_colour(pixel(30, 25), red) and is_colour(pixel(10, 10), white), "Ellipses respect their bounds");
   for (auto ends : { Surface::Ends::semiCircles, Surface::Ends::leftFlat, Surface::Ends::rightFlat,
         Surface::Ends::leftAngle, Surface::Ends::rightAngle }) {
      clear();
      surface.Stadium(shape, colours, ends);
      expect(is_colour(pixel(30, 25), red), "Every stadium end combination draws its interior");
   }
   clear();
   const Point triangle[] = { Point(10, 10), Point(60, 10), Point(35, 40) };
   surface.Polygon(triangle, 3, colours);
   expect(is_colour(pixel(35, 20), red), "Polygons fill their interior");

   clear();
   surface.GradientRectangle(shape, { { 0, red }, { 1, blue } }, Surface::GradientOptions::leftToRight);
   expect(pixel(10, 25).GetRed() > 240 and pixel(59, 25).GetBlue() > 240, "Horizontal gradients interpolate stops");
   surface.GradientRectangle(shape, { { 0, red }, { 1, blue } }, Surface::GradientOptions::topToBottom);
   expect(pixel(30, 10).GetRed() > 240 and pixel(30, 39).GetBlue() > 240, "Vertical gradients interpolate stops");
   clear();
   surface.FillRectangle(bounds, Fill(ColourRGBA(0, 0, 255, 128)));
   const auto blended = pixel(30, 30);
   expect(blended.GetRed() > 0 and blended.GetRed() < 255 and blended.GetBlue() IS 255, "RGBA fills blend");
   clear();
   const unsigned char image[] = { 0, 0, 255, 128 };
   surface.DrawRGBAImage(PRectangle(10, 10, 11, 11), 1, 1, image);
   expect(std::abs(int(pixel(10, 10).GetRed()) - int(blended.GetRed())) <= 1, "RGBA images use fill blending");

   auto pattern = surface.AllocatePixMap(2, 2);
   expect(pattern->Initialised(), "Pixmaps have unique ownership and valid storage");
   pattern->FillRectangle(PRectangle(0, 0, 2, 2), Fill(red));
   pattern->FillRectangle(PRectangle(0, 0, 1, 1), Fill(blue));
   clear();
   surface.SetClip(PRectangle(10, 10, 30, 30));
   surface.FillRectangle(PRectangle(0, 0, 40, 40), *pattern);
   expect(is_colour(pixel(10, 10), blue) and is_colour(pixel(11, 10), red) and
      is_colour(pixel(9, 10), white), "Pattern fills tile within the clip");
   surface.FillRectangle(PRectangle(40, 40, 50, 50), *pattern);
   expect(is_colour(pixel(40, 40), white), "A pattern outside the clip performs no bitmap copy");
   surface.Copy(PRectangle(0, 0, 2, 2), Point(0, 0), *pattern);
   expect(is_colour(pixel(0, 0), white), "Bitmap copies respect the destination clip");
   surface.PopClip();
   surface.Copy(PRectangle(0, 0, 2, 2), Point(0, 0), *pattern);
   expect(is_colour(pixel(0, 0), blue), "Unclipped bitmap copies transfer pixels");

   using Scintilla::Internal::FontParameters;
   const FontParameters parameters("Noto Sans", 19, Scintilla::FontWeight::Normal, false,
      Scintilla::FontQuality::QualityDefault, Scintilla::Technology::Default, Scintilla::CharacterSet::Default,
      Scintilla::Internal::localeNameDefault, Scintilla::FontStretch::Normal);
   auto font = Font::Allocate(parameters);
   const auto handle = ((PanFont *)font.get())->handle;
   expect(handle != nullptr, "Font parameters resolve a vector handle");
   APTR regular = nullptr;
   expect((vec::GetFontHandle("Noto Sans", "Regular", 0, 19, &regular) IS ERR::Okay) and (handle IS regular),
      "Normal Scintilla weight resolves the regular vector face");
   auto shared = font;
   font.reset();
   font = shared;
   const std::string text = "AV é😀é";
   std::vector<XYPOSITION> positions(text.size(), -1);
   surface.MeasureWidthsUTF8(font.get(), text, positions.data());
   expect(std::abs(positions.back() - vec::StringWidth(handle, text, -1)) < 0.0001,
      "UTF-8 cumulative advances include kerning and match vector measurement");
   expect(positions[3] IS positions[4] and positions[5] IS positions[8],
      "Every UTF-8 byte has its character's end advance");
   expect(std::abs(surface.WidthTextUTF8(font.get(), text) - positions.back()) < 0.0001,
      "WidthText preserves fractional advances");
   const std::string malformed = "bad\xf0(\x8c(";
   positions.resize(malformed.size());
   surface.MeasureWidthsUTF8(font.get(), malformed, positions.data());
   expect(std::abs(positions.back() - surface.WidthTextUTF8(font.get(), malformed)) < 0.0001,
      "Malformed UTF-8 still has bounded byte advances");
   for (const std::string_view invalid : { "\xc0\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82" }) {
      positions.resize(invalid.size());
      surface.MeasureWidthsUTF8(font.get(), invalid, positions.data());
      std::string replacement;
      for (size_t i = 0; i < invalid.size(); i++) replacement += "\xef\xbf\xbd";
      expect(std::abs(positions.back() - vec::StringWidth(handle, replacement, -1)) < 0.0001 and
         std::abs(surface.WidthTextUTF8(font.get(), invalid) - positions.back()) < 0.0001,
         "Invalid scalar encodings use the same replacement glyphs for measuring and drawing");
   }
   surface.SetMode(SurfaceMode(0, false));
   const std::string latin1 = "caf\xe9";
   positions.resize(latin1.size());
   surface.MeasureWidths(font.get(), latin1, positions.data());
   expect(std::abs(positions.back() - surface.WidthText(font.get(), latin1)) < 0.0001 and
      std::abs(positions.back() - surface.WidthTextUTF8(font.get(), "café")) < 0.0001,
      "Single-byte text mode maps Latin-1 to vector UTF-8");
   surface.SetMode(SurfaceMode(SC_CP_UTF8, false));
   clear();
   surface.DrawTextTransparentUTF8(bounds, font.get(), 30.5, text, black);
   std::vector<uint8_t> drawn(bitmap->Data, bitmap->Data + bitmap->LineWidth * bitmap->Height);
   clear();
   set_vector_colour(((extScintilla *)editor)->FillColour, black, 255);
   vec::DrawText(bitmap, handle, 0, 30.5, text, ((extScintilla *)editor)->FillColour, nullptr);
   expect(std::equal(drawn.begin(), drawn.end(), bitmap->Data), "Drawing preserves fractional baselines");
   surface.SetClip(PRectangle(10, 10, 70, 70));
   surface.DrawTextClippedUTF8(PRectangle(90, 0, 110, 40), font.get(), 30, text, black, red);
   surface.FillRectangle(bounds, blue);
   surface.PopClip();
   expect(is_colour(pixel(20, 20), blue) and is_colour(pixel(95, 20), white), "Clipped text restores nested clips");
   expect(surface.SupportsFeature(Scintilla::Supports::FractionalStrokeWidth) IS 1 and
      surface.SupportsFeature(Scintilla::Supports::ThreadSafeMeasureWidths) IS 0 and
      surface.PixelDivisions() IS 1 and not surface.Layout(nullptr), "Optional layout and capabilities are accurate");
   surface.FlushCachedState();
   surface.FlushDrawing();
   surface.Release();
   surface.Release();
   expect(not surface.Initialised(), "Release is idempotent");
   surface.Init(bitmap, editor);
   clear();
   expect(is_colour(pixel(20, 20), white), "Reinitialisation clears the old clip stack");
   return failures IS 0;
}

int main(int Argc, CSTRING *Argv)
{
   if (auto error = init_kotuku(Argc, Argv)) { std::fprintf(stderr, "%s\n", error); return 1; }
   OBJECTPTR module;
   const bool passed = (objModule::load("vector", &module, &VectorBase) IS ERR::Okay) and
      (objModule::load("display", &module, &DisplayBase) IS ERR::Okay) and
      (objModule::load("scintilla", &module, nullptr) IS ERR::Okay) and run_checks();
   close_kotuku();
   return passed ? 0 : 1;
}
