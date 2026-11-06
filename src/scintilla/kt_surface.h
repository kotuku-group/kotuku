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
      std::string storage;
      DrawTextNoClipUTF8(Rect, Font, Baseline, normal_text(Text, storage), Fore, Back);
   }
   void DrawTextNoClipUTF8(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore, ColourRGBA Back) override;
   void DrawTextClipped(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore, ColourRGBA Back) override {
      std::string storage;
      DrawTextClippedUTF8(Rect, Font, Baseline, normal_text(Text, storage), Fore, Back);
   }
   void DrawTextClippedUTF8(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore, ColourRGBA Back) override;
   void DrawTextTransparent(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore) override {
      std::string storage;
      DrawTextTransparentUTF8(Rect, Font, Baseline, normal_text(Text, storage), Fore);
   }
   void DrawTextTransparentUTF8(PRectangle Rect, const Font *Font,
      XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore) override;
   void MeasureWidths(const Font *Font, std::string_view Text, XYPOSITION *Positions) override;
   XYPOSITION WidthText(const Font *Font, std::string_view Text) override {
      std::string storage;
      return WidthTextUTF8(Font, normal_text(Text, storage));
   }
   void MeasureWidthsUTF8(const Font *Font, std::string_view Text, XYPOSITION *Positions) override;
   XYPOSITION WidthTextUTF8(const Font *Font, std::string_view Text) override;
   XYPOSITION Ascent(const Font *Font) override;
   XYPOSITION Descent(const Font *Font) override;
   XYPOSITION InternalLeading(const Font *) override { return 0; }
   XYPOSITION Height(const Font *Font) override;
   XYPOSITION AverageCharWidth(const Font *Font) override;

private:
   std::string_view normal_text(std::string_view Text, std::string &Storage) const;
   void draw_text(PRectangle Rect, const Font *Font, XYPOSITION Baseline, std::string_view Text, ColourRGBA Fore);
   APTR GetFont(const Font &Font);
   void draw_path(APTR Path, const ColourRGBA *Fill, int FillAlpha,
      const ColourRGBA *Stroke, int StrokeAlpha, double StrokeWidth = 1.0);
};

