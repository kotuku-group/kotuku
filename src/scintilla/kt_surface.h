#pragma once

using Scintilla::Internal::ColourRGBA;
using Scintilla::Internal::PRectangle;
using Scintilla::Internal::Point;
using Scintilla::Internal::Font;
using Scintilla::Internal::Surface;
using Scintilla::Internal::SurfaceID;
using Scintilla::Internal::WindowID;
using Scintilla::Internal::XYPOSITION;
using Scintilla::Internal::Stroke;
using Scintilla::Internal::Fill;
using Scintilla::Internal::FillStroke;
using Scintilla::Internal::IScreenLineLayout;
using Scintilla::Internal::IScreenLine;
using Scintilla::Internal::SurfaceMode;
using Scintilla::Internal::ColourStop;

inline void set_vector_colour(objVectorColour *Colour, const ColourRGBA &Value, int Alpha)
{
   Colour->Red   = Value.GetRed() / 255.0;
   Colour->Green = Value.GetGreen() / 255.0;
   Colour->Blue  = Value.GetBlue() / 255.0;
   Colour->Alpha = std::clamp(Alpha, 0, 255) / 255.0;
}

//********************************************************************************************************************
// SurfacePan class

class SurfacePan : public Surface {
   extScintilla *scintilla; // The Scintilla object that owns this surface; provides the font set
   objBitmap *bitmap;
   bool own_bitmap = false; /* True if this object owns the bitmap, and will free it */
   PRectangle cliprect;
   std::vector<PRectangle> clips;
   SurfaceMode mode { SC_CP_UTF8, false };

public:
   SurfacePan();
   ~SurfacePan() override;

   void Init(WindowID Window) override;
   void Init(SurfaceID SurfaceData, WindowID Window) override;
   void InitPixMap(int Width, int Height, WindowID Window);

   void Release() noexcept override;
   bool Initialised() override;
   int LogPixelsY() override;
   int DeviceHeightFont(int Points) override;
   void FillRectangle(PRectangle Rect, ColourRGBA Back);
   void FillRectangle(PRectangle Rect, Surface &Pattern) override;
   void Copy(PRectangle Rect, Point From, Surface &Source) override;
   void DrawRGBAImage(PRectangle Rect, int Width, int Height, const unsigned char *Pixels) override;

   void DrawTextBase(PRectangle Rect, const Font &Font, XYPOSITION Baseline,
      const char *String, int Length, ColourRGBA Fore);
   void DrawTextNoClip(PRectangle Rect, const Font &Font, XYPOSITION Baseline, const char *String, int Length,
      ColourRGBA Fore, ColourRGBA Back);
   void DrawTextClipped(PRectangle Rect, const Font &Font, XYPOSITION Baseline, const char *String, int Length,
      ColourRGBA Fore, ColourRGBA Back);
   void DrawTextTransparent(PRectangle Rect, const Font &Font, XYPOSITION Baseline,
      const char *String, int Length, ColourRGBA Fore);
   XYPOSITION WidthText(const Font &Font, const char *String, int Length);
   XYPOSITION WidthChar(const Font &Font, char Character);
   int Ascent(const Font &Font);
   int Descent(const Font &Font);
   int InternalLeading(const Font &Font);
   int Height(const Font &Font);
   XYPOSITION AverageCharWidth(const Font &Font);

   void SetClip(PRectangle Rect) override;
   void FlushCachedState() override;

   void BlendRectangle(PRectangle Rect, ColourRGBA Colour, int Alpha);

   std::unique_ptr<Surface> AllocatePixMap(int Width, int Height) override;
   void SetMode(SurfaceMode Mode) override {
      // The editor admits UTF-8 only.  Code page zero is used for single-byte platform text; DBCS and bidi are absent.
      mode = SurfaceMode(Mode.codePage IS 0 ? 0 : SC_CP_UTF8, false);
   }
   int SupportsFeature(Scintilla::Supports Feature) noexcept override;
   int PixelDivisions() override { return 1; }
   std::unique_ptr<IScreenLineLayout> Layout(const IScreenLine *) override {
      return {}; // Optional bidi layout is not advertised.
   }
   void LineDraw(Point Start, Point End,
      Stroke Stroke) override;
   void PolyLine(const Point *Points, size_t Count, Stroke Stroke) override;
   void Polygon(const Point *Points, size_t Count,
      FillStroke Colours) override;
   void RectangleDraw(PRectangle Rect, FillStroke Colours) override;
   void RectangleFrame(PRectangle Rect, Stroke Stroke) override;
   void FillRectangle(PRectangle Rect, Fill Fill) override {
      BlendRectangle(Rect, Fill.colour, Fill.colour.GetAlpha());
   }
   void FillRectangleAligned(PRectangle Rect, Fill Fill) override {
      FillRectangle(Scintilla::Internal::PixelAlign(Rect, PixelDivisions()), Fill);
   }
   void RoundedRectangle(PRectangle Rect, FillStroke Colours) override;
   void AlphaRectangle(PRectangle Rect, XYPOSITION Corner, FillStroke Colours) override;
   void GradientRectangle(PRectangle Rect,
      const std::vector<ColourStop> &Stops, GradientOptions Options) override;
   void Ellipse(PRectangle Rect, FillStroke Colours) override;
   void Stadium(PRectangle Rect, FillStroke Colours, Ends Ends) override;
   void PopClip() override {
      if (not clips.empty()) { cliprect = clips.back(); clips.pop_back(); }
   }
   void FlushDrawing() override { } // Bitmap/vector operations are synchronous.
   void DrawTextNoClip(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore, ColourRGBA Back) override {
      const auto text = normal_text(Text);
      DrawTextNoClip(Rect, *Font, Baseline, text.data(), int(text.size()), Fore, Back);
   }
   void DrawTextNoClipUTF8(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore, ColourRGBA Back) override {
      DrawTextNoClip(Rect, *Font, Baseline, Text.data(), int(Text.size()), Fore, Back);
   }
   void DrawTextClipped(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore, ColourRGBA Back) override {
      const auto text = normal_text(Text);
      DrawTextClipped(Rect, *Font, Baseline, text.data(), int(text.size()), Fore, Back);
   }
   void DrawTextClippedUTF8(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore, ColourRGBA Back) override {
      DrawTextClipped(Rect, *Font, Baseline, Text.data(), int(Text.size()), Fore, Back);
   }
   void DrawTextTransparent(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore) override {
      const auto text = normal_text(Text);
      DrawTextTransparent(Rect, *Font, Baseline, text.data(), int(text.size()), Fore);
   }
   void DrawTextTransparentUTF8(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore) override {
      DrawTextTransparent(Rect, *Font, Baseline, Text.data(), int(Text.size()), Fore);
   }
   void MeasureWidths(const Font *Font, std::string_view Text,
      XYPOSITION *Positions) override;
   XYPOSITION WidthText(const Font *Font,
      std::string_view Text) override {
      const auto text = normal_text(Text);
      return WidthText(*Font, text.data(), int(text.size()));
   }
   void MeasureWidthsUTF8(const Font *Font, std::string_view Text,
      XYPOSITION *Positions) override;
   XYPOSITION WidthTextUTF8(const Font *Font,
      std::string_view Text) override { return WidthText(*Font, Text.data(), int(Text.size())); }
   XYPOSITION Ascent(const Font *Font) override {
      return Ascent(*Font);
   }
   XYPOSITION Descent(const Font *Font) override {
      return Descent(*Font);
   }
   XYPOSITION InternalLeading(const Font *Font) override {
      return InternalLeading(*Font);
   }
   XYPOSITION Height(const Font *Font) override {
      return Height(*Font);
   }
   XYPOSITION AverageCharWidth(const Font *Font) override {
      return AverageCharWidth(*Font);
   }

private:
   std::string normal_text(std::string_view Text) const;
   APTR GetFont(const Font &Font);
   void draw_path(APTR Path, const ColourRGBA *Fill, int FillAlpha,
      const ColourRGBA *Stroke, int StrokeAlpha, double StrokeWidth = 1.0);
};

