#pragma once

// Scintilla shares these descriptors.  Vector owns the cached handles for the process lifetime.
class PanFont final : public Scintilla::Internal::Font {
public:
   APTR handle = nullptr;

   // Completion lists use the owning editor's regular handle until a style font is supplied.
   PanFont() = default;
   explicit PanFont(const Scintilla::Internal::FontParameters &Parameters) {
      constexpr const char *widths[] = { "UltraCondensed", "ExtraCondensed", "Condensed", "SemiCondensed",
         "Normal", "SemiExpanded", "Expanded", "ExtraExpanded", "UltraExpanded" };
      const auto width = widths[std::clamp(int(Parameters.stretch), 1, 9) - 1];
      const auto style = std::format("weight={};italic={};width={}",
         int(Parameters.weight), Parameters.italic ? "yes" : "no", width);
      const auto face = Parameters.faceName ? Parameters.faceName : "Clean";
      const int size = std::max(1, int(std::lround(Parameters.size)));
      if (auto error = vec::GetFontHandle(face, style, int(Parameters.weight), size, &handle); error != ERR::Okay) {
         kt::Log("ScintillaFont").warning(error);
      }
   }
};

// Restore the bitmap clip even if a nested drawing operation exits early.
class BitmapClipper {
   objBitmap *bitmap;
   ClipRectangle saved;
public:
   BitmapClipper(objBitmap *Bitmap, const Scintilla::Internal::PRectangle &Clip) :
      bitmap(Bitmap), saved(Bitmap->Clip) {
      bitmap->Clip.Left = std::max(saved.Left, int(std::floor(Clip.left)));
      bitmap->Clip.Top = std::max(saved.Top, int(std::floor(Clip.top)));
      bitmap->Clip.Right = std::min(saved.Right, int(std::ceil(Clip.right)));
      bitmap->Clip.Bottom = std::min(saved.Bottom, int(std::ceil(Clip.bottom)));
   }

   ~BitmapClipper() { bitmap->Clip = saved; }
};

