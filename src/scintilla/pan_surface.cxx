//********************************************************************************************************************
// SurfacePan class

class SurfacePan : public Scintilla::Surface
{
   int penx;
   int peny;
   extScintilla *scintilla; // The Scintilla object that owns this surface; provides the font set
   objBitmap *bitmap;
   int8_t own_bitmap:1; /* True if this object owns the bitmap, and will free it */
   int pencol;
   Scintilla::PRectangle cliprect;

public:
   SurfacePan();
   virtual ~SurfacePan();

   void Init(Scintilla::WindowID wid);
   void Init(Scintilla::SurfaceID sid, Scintilla::WindowID wid);
   void InitPixMap(int width, int height, Scintilla::Surface *surface_, Scintilla::WindowID wid);

   void Release();
   bool Initialised();
   void PenColour(Scintilla::ColourAllocated fore);
   int LogPixelsY();
   int DeviceHeightFont(int points);
   void MoveTo(int x_, int y_);
   void LineTo(int x_, int y_);
   void Polygon(Scintilla::Point *pts, int npts, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back);
   void RectangleDraw(Scintilla::PRectangle rc, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back);
   void FillRectangle(Scintilla::PRectangle rc, Scintilla::ColourAllocated back);
   void FillRectangle(Scintilla::PRectangle rc, Scintilla::Surface &surfacePattern);
   void RoundedRectangle(Scintilla::PRectangle rc, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back);
   void Ellipse(Scintilla::PRectangle rc, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back);
   void Copy(Scintilla::PRectangle rc, Scintilla::Point from, Scintilla::Surface &surfaceSource);
   void AlphaRectangle(Scintilla::PRectangle rc, int cornerSize, Scintilla::ColourAllocated fill, int alphaFill, Scintilla::ColourAllocated outline, int alphaOutline, int flags);
	void DrawRGBAImage(Scintilla::PRectangle rc, int width, int height, const unsigned char *pixelsImage);

   void DrawTextBase(Scintilla::PRectangle rc, Scintilla::Font &font_, int ybase, const char *s, int len, Scintilla::ColourAllocated fore);
   void DrawTextNoClip(Scintilla::PRectangle rc, Scintilla::Font &font_, int ybase, const char *s, int len, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back);
   void DrawTextClipped(Scintilla::PRectangle rc, Scintilla::Font &font_, int ybase, const char *s, int len, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back);
   void DrawTextTransparent(Scintilla::PRectangle rc, Scintilla::Font &font_, int ybase, const char *s, int len, Scintilla::ColourAllocated fore);
   void MeasureWidths(Scintilla::Font &font_, const char *s, int len, int *positions);
   int WidthText(Scintilla::Font &font_, const char *s, int len);
   int WidthChar(Scintilla::Font &font_, char ch);
   int Ascent(Scintilla::Font &font_);
   int Descent(Scintilla::Font &font_);
   int InternalLeading(Scintilla::Font &font_);
   int ExternalLeading(Scintilla::Font &font_);
   int Height(Scintilla::Font &font_);
   int AverageCharWidth(Scintilla::Font &font_);

   int SetPalette(Scintilla::Palette *pal, bool inBackGround);
   void SetClip(Scintilla::PRectangle rc);
   void FlushCachedState();

   void SetUnicodeMode(bool unicodeMode_);
   void SetDBCSMode(int codePage);

   void BlendRectangle(Scintilla::PRectangle rc, Scintilla::ColourAllocated colour, int alpha);

private:
   APTR GetFont(Scintilla::Font &Font);
   int font_index(const Scintilla::Font &Font) const;
   void draw_path(APTR Path, const Scintilla::ColourAllocated *Fill, int FillAlpha,
      const Scintilla::ColourAllocated *Stroke, int StrokeAlpha);
};

SurfacePan::SurfacePan()
:  cliprect(0,0,0,0)
{
   scintilla = nullptr;
   bitmap = nullptr;
   own_bitmap = FALSE;
   penx   = 0;
   peny   = 0;
   pencol = 0;
}

SurfacePan::~SurfacePan()
{
   Release();
}

void SurfacePan::Release()
{
   if ((bitmap) and (own_bitmap)) { FreeResource(bitmap); bitmap = nullptr; }
}

bool SurfacePan::Initialised()
{
   return bitmap != nullptr;
}

void SurfacePan::Init(Scintilla::WindowID WinID)
{
   scintilla = (extScintilla *)WinID;
}

void SurfacePan::Init(Scintilla::SurfaceID sid, Scintilla::WindowID WinID)
{
   if (bitmap) return;

   scintilla = (extScintilla *)WinID;

   // Surface id will be a bitmap object
   bitmap = static_cast<objBitmap *>(sid);

   // Set the Clipping rect to the dimensions of the bitmap
   cliprect.left   = 0;
   cliprect.right  = bitmap->Width;
   cliprect.top    = 0;
   cliprect.bottom = bitmap->Height;
}

void SurfacePan::InitPixMap(int width, int height, Scintilla::Surface *surface_, Scintilla::WindowID WinID)
{
   if (bitmap) return;

   kt::Log log(__FUNCTION__);
   log.traceBranch("Size: %dx%d", width, height);

   scintilla = (extScintilla *)WinID;

   if (!(bitmap = objBitmap::create::local(fl::Width(width), fl::Height(height), fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL)))) {
      log.warning("Failed to create offscreen surface object.");
      return;
   }

   own_bitmap = TRUE; // This object owns the bitmap and will delete it on destruction.

   // Set the Clipping rect to the dimensions of the bitmap

   cliprect.left   = 0;
   cliprect.top    = 0;
   cliprect.right  = bitmap->Width;
   cliprect.bottom = bitmap->Height;
}

//********************************************************************************************************************

inline uint32_t to_pan_col(objBitmap *bitmap, const Scintilla::ColourAllocated& colour)
{
   uint32_t col32 = colour.AsLong();
   return bitmap->packPixel(SCIRED(col32), SCIGREEN(col32), SCIBLUE(col32), 255);
}

//********************************************************************************************************************

inline void set_vector_colour(objVectorColour *Colour, const Scintilla::ColourAllocated &Value, int Alpha)
{
   uint32_t col32 = Value.AsLong();
   Colour->Red   = SCIRED(col32) / 255.0;
   Colour->Green = SCIGREEN(col32) / 255.0;
   Colour->Blue  = SCIBLUE(col32) / 255.0;
   Colour->Alpha = std::clamp(Alpha, 0, 255) / 255.0;
}

//********************************************************************************************************************
// Shapes are drawn with vec::DrawPath(), which provides anti-aliasing and alpha blending.  The Path is a generated
// vector path that is always freed by this function.  Strokes are one pixel wide.

void SurfacePan::draw_path(APTR Path, const Scintilla::ColourAllocated *Fill, int FillAlpha,
   const Scintilla::ColourAllocated *Stroke, int StrokeAlpha)
{
   if ((bitmap) and (scintilla) and (scintilla->FillColour) and (scintilla->StrokeColour)) {
      BitmapClipper clipper(bitmap, cliprect);

      if ((bitmap->Clip.Right > bitmap->Clip.Left) and (bitmap->Clip.Bottom > bitmap->Clip.Top)) {
         if (Fill) set_vector_colour(scintilla->FillColour, *Fill, FillAlpha);
         if (Stroke) set_vector_colour(scintilla->StrokeColour, *Stroke, StrokeAlpha);

         vec::DrawPath(bitmap, Path, Stroke ? 1.0 : 0.0, Stroke ? scintilla->StrokeColour : nullptr,
            Fill ? scintilla->FillColour : nullptr);
      }
   }

   FreeResource(Path);
}

//********************************************************************************************************************

void SurfacePan::BlendRectangle(Scintilla::PRectangle rc, Scintilla::ColourAllocated colour, int alpha)
{
   if ((not bitmap) or (rc.Width() <= 0) or (rc.Height() <= 0)) return;

   APTR path;
   if (vec::GenerateRectangle(rc.left, rc.top, rc.Width(), rc.Height(), &path) IS ERR::Okay) {
      draw_path(path, &colour, alpha, nullptr, 0);
   }
}

//********************************************************************************************************************

void SurfacePan::PenColour(Scintilla::ColourAllocated fore)
{
   if (bitmap) {
      this->pencol = to_pan_col(bitmap, fore);
   }
}

//********************************************************************************************************************

int SurfacePan::LogPixelsY()
{
   return 96; // Kotuku's vector units are fixed at 96 DPI
}

//********************************************************************************************************************

int SurfacePan::DeviceHeightFont(int pointsize)
{
   int dpi = LogPixelsY();

   //MSG("DeviceHeightFont(%d, DPI %d)", pointsize, dpi);

   return (pointsize * dpi + (dpi / 2)) / 72;
}

//********************************************************************************************************************

void SurfacePan::MoveTo(int x, int y)
{
   penx = x;
   peny = y;
}

//********************************************************************************************************************
// Draws a one pixel line from the pen position.  The end point is excluded, as Scintilla expects.

void SurfacePan::LineTo(int x, int y)
{
   if (bitmap) {
      BitmapClipper clipper(bitmap, cliprect);

      DBGDRAW("panLineTo:","%dx%d - %dx%d", penx, peny, x, y);

      if (y IS peny) {
         if (x > penx) gfx::DrawRectangle(bitmap, penx, y, x - penx, 1, pencol, BAF::FILL);
         else if (x < penx) gfx::DrawRectangle(bitmap, x + 1, y, penx - x, 1, pencol, BAF::FILL);
      }
      else if (x IS penx) {
         if (y > peny) gfx::DrawRectangle(bitmap, x, peny, 1, y - peny, pencol, BAF::FILL);
         else gfx::DrawRectangle(bitmap, x, y + 1, 1, peny - y, pencol, BAF::FILL);
      }
      else { // Bresenham
         const int dx = std::abs(x - penx), sx = (penx < x) ? 1 : -1;
         const int dy = -std::abs(y - peny), sy = (peny < y) ? 1 : -1;
         int err = dx + dy;
         int cx = penx, cy = peny;
         while ((cx != x) or (cy != y)) {
            gfx::DrawRectangle(bitmap, cx, cy, 1, 1, pencol, BAF::FILL);
            const int e2 = err * 2;
            if (e2 >= dy) { err += dy; cx += sx; }
            if (e2 <= dx) { err += dx; cy += sy; }
         }
      }
   }

   penx = x;
   peny = y;
}

//********************************************************************************************************************
// Draws an image of RGBA bytes, centred within rc and alpha blended with the background.

void SurfacePan::DrawRGBAImage(Scintilla::PRectangle rc, int width, int height, const unsigned char *pixelsImage)
{
   if ((not bitmap) or (not pixelsImage) or (width < 1) or (height < 1)) return;

   if (rc.Width() > width) rc.left += (rc.Width() - width) / 2;
   if (rc.Height() > height) rc.top += (rc.Height() - height) / 2;

   if (auto image = objBitmap::create::local(fl::Width(width), fl::Height(height), fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL))) {
      for (int y=0; y < height; y++) {
         auto dest = (uint32_t *)(image->Data + (image->LineWidth * y));
         auto src  = pixelsImage + (width * y * 4);
         for (int x=0; x < width; x++, src += 4) dest[x] = image->packPixel(src[0], src[1], src[2], src[3]);
      }

      BitmapClipper clipper(bitmap, cliprect);
      gfx::CopyArea(image, bitmap, BAF::BLEND, 0, 0, width, height, rc.left, rc.top);
      FreeResource(image);
   }
}

//********************************************************************************************************************
// Fills a polygon with the back colour and outlines it with the fore colour.  Points are offset to pixel centres so
// that the outline is sharp, as the GTK platform does.

void SurfacePan::Polygon(Scintilla::Point *pts, int npts, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back)
{
   if ((not bitmap) or (npts < 2)) return;

   DBGDRAW("panPolygon","%d points", npts);

   std::string sequence;
   for (int i=0; i < npts; i++) {
      sequence += std::format("{}{} {} ", i ? 'L' : 'M', pts[i].x + 0.5, pts[i].y + 0.5);
   }
   sequence += 'Z';

   APTR path;
   if (vec::GeneratePath(sequence, &path) IS ERR::Okay) draw_path(path, &back, 255, &fore, 255);
}

//********************************************************************************************************************

void SurfacePan::RectangleDraw(Scintilla::PRectangle rc, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back)
{
   if (bitmap) {
      BitmapClipper clipper(bitmap, cliprect);

      uint32_t bk32 = to_pan_col(bitmap, back);
      uint32_t fr32 = to_pan_col(bitmap, fore);

      DBGDRAW("panRectangleDraw()","#%.8x, #%.8x", bk32, fr32);

      gfx::DrawRectangle(bitmap, rc.left, rc.top, rc.Width(), rc.Height(), bk32, BAF::FILL);
      gfx::DrawRectangle(bitmap, rc.left, rc.top, rc.Width(), rc.Height(), fr32, BAF::NIL);
   }
}

//********************************************************************************************************************

void SurfacePan::FillRectangle(Scintilla::PRectangle rc, Scintilla::ColourAllocated back)
{
   if (bitmap) {
      uint32_t colour;

      BitmapClipper clipper(bitmap, cliprect);
      colour = to_pan_col(bitmap, back);

      DBGDRAW("panFillRectangle()","Bitmap: %p, Size: %dx%d,%dx%d, Colour: $%.8x, Clipping: %dx%d,%dx%d", bitmap, rc.left, rc.top, rc.Width(), rc.Height(), colour, bitmap->Clip.Left, bitmap->Clip.Right, bitmap->Clip.Top, bitmap->Clip.Bottom);

      gfx::DrawRectangle(bitmap, rc.left, rc.top, rc.Width(), rc.Height(), colour, BAF::FILL);
   }
}

//********************************************************************************************************************
// Tiles the bitmap of the pattern surface over rc, starting from the top-left corner of rc.

void SurfacePan::FillRectangle(Scintilla::PRectangle rc, Scintilla::Surface &surfacePattern)
{
   if (not bitmap) return;

   auto &pattern = static_cast<SurfacePan &>(surfacePattern);
   if ((not pattern.bitmap) or (pattern.bitmap->Width < 1) or (pattern.bitmap->Height < 1)) {
      FillRectangle(rc, Scintilla::ColourAllocated(0)); // Show the area as black, as the GTK platform does
      return;
   }

   DBGDRAW("panFillRectangle(2):","%dx%d,%dx%d", rc.left, rc.top, rc.Width(), rc.Height());

   BitmapClipper clipper(bitmap, cliprect);
   const int pw = pattern.bitmap->Width, ph = pattern.bitmap->Height;
   for (int y=rc.top; y < rc.bottom; y += ph) {
      for (int x=rc.left; x < rc.right; x += pw) {
         gfx::CopyArea(pattern.bitmap, bitmap, BAF::NIL, 0, 0, std::min(pw, rc.right - x),
            std::min(ph, rc.bottom - y), x, y);
      }
   }
}

//********************************************************************************************************************

void SurfacePan::RoundedRectangle(Scintilla::PRectangle rc, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back)
{
   DBGDRAW("panRoundedRectangle","");

   if (((rc.right - rc.left) > 4) && ((rc.bottom - rc.top) > 4)) {
      // Approximate a round rect with some cut off corners
      Scintilla::Point pts[] = {
         Scintilla::Point(rc.left + 2, rc.top),
         Scintilla::Point(rc.right - 2, rc.top),
         Scintilla::Point(rc.right, rc.top + 2),
         Scintilla::Point(rc.right, rc.bottom - 2),
         Scintilla::Point(rc.right - 2, rc.bottom),
         Scintilla::Point(rc.left + 2, rc.bottom),
         Scintilla::Point(rc.left, rc.bottom - 2),
         Scintilla::Point(rc.left, rc.top + 2),
      };
      Polygon(pts, sizeof(pts) / sizeof(pts[0]), fore, back);
   }
   else RectangleDraw(rc, fore, back);
}

//********************************************************************************************************************
// Draws a circle that fits the smaller dimension of rc, filled with the back colour and outlined with the fore colour.

void SurfacePan::Ellipse(Scintilla::PRectangle rc, Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back)
{
   if ((not bitmap) or (rc.Width() <= 0) or (rc.Height() <= 0)) return;

   DBGDRAW("panEllipse:","%dx%d,%dx%d", rc.left, rc.top, rc.Width(), rc.Height());

   const double radius = std::min(rc.Width(), rc.Height()) / 2;
   APTR path;
   if (vec::GenerateEllipse(((rc.left + rc.right) / 2) + 0.5, ((rc.top + rc.bottom) / 2) + 0.5, radius, radius, 0,
         &path) IS ERR::Okay) {
      draw_path(path, &back, 255, &fore, 255);
   }
}

//********************************************************************************************************************

void SurfacePan::Copy(Scintilla::PRectangle rc, Scintilla::Point from, Scintilla::Surface &surfaceSource)
{
   kt::Log log(__FUNCTION__);
   SurfacePan &src_surface = static_cast<SurfacePan&>(surfaceSource);

   if ((bitmap) and (src_surface.bitmap)) {
//      BitmapClipper clipper(bitmap, cliprect);

      DBGDRAW("panCopy:","From: %dx%d To: %dx%d,%dx%d, Clip: %dx%d,%dx%d", from.x, from.y, rc.left, rc.top, rc.Width(), rc.Height(), bitmap->Clip.Left, bitmap->Clip.Top, bitmap->Clip.Right, bitmap->Clip.Bottom);
      DBGDRAW("panCopy:","Bitmap: %d, Offset: %dx%d", bitmap->Head.UID, bitmap->XOffset, bitmap->YOffset);
      gfx::CopyArea(src_surface.bitmap, bitmap, BAF::NIL,
         from.x, from.y, rc.Width(), rc.Height(), /* source */
         rc.left, rc.top); /* dest */

      //bmpDrawRectangle(bitmap, rc.left, rc.top, rc.Width(), rc.Height(), PackPixel(bitmap, 255, 0, 0, 255), BAF::NIL);
   }
   else log.warning(ERR::Args);
}

//********************************************************************************************************************

void SurfacePan::DrawTextBase(Scintilla::PRectangle Rect, Scintilla::Font &Font, int Baseline,
   const char *String, int Length, Scintilla::ColourAllocated Fore)
{
   if ((not bitmap) or (not scintilla) or (Length <= 0)) return;

   if (auto handle = GetFont(Font)) {
      BitmapClipper clipper(bitmap, cliprect);
      set_vector_colour(scintilla->FillColour, Fore, 255);
      if (auto error = vec::DrawText(bitmap, handle, Rect.left, Baseline,
            std::string_view(String, size_t(Length)), scintilla->FillColour, nullptr); error != ERR::Okay) {
         kt::Log(__FUNCTION__).warning(error);
      }
   }
}

//********************************************************************************************************************

void SurfacePan::DrawTextNoClip(Scintilla::PRectangle rc, Scintilla::Font &font_, int ybase, const char *s, int len,
                                 Scintilla::ColourAllocated fore, Scintilla::ColourAllocated back)
{
   DBGDRAW("panDrawTextNoClip()","");
   FillRectangle(rc, back);
   DrawTextBase(rc, font_, ybase, s, len, fore);
}

//********************************************************************************************************************
// On GTK+, exactly same as DrawTextNoClip

void SurfacePan::DrawTextClipped(Scintilla::PRectangle Rect, Scintilla::Font &Font, int Baseline,
   const char *String, int Length, Scintilla::ColourAllocated Fore, Scintilla::ColourAllocated Back)
{
   DBGDRAW("panDrawTextClipped()", "");
   if (not bitmap) return;
   BitmapClipper clipper(bitmap, Rect);
   FillRectangle(Rect, Back);
   DrawTextBase(Rect, Font, Baseline, String, Length, Fore);
}

//********************************************************************************************************************
// Avoid drawing spaces in transparent mode; i.e. only draw if there is at least one non-space char.

void SurfacePan::DrawTextTransparent(Scintilla::PRectangle rc, Scintilla::Font &font_, int ybase,
   const char *s, int len, Scintilla::ColourAllocated fore)
{
   int i;

   for (i=0; i < len; i++) {
      if (s[i] != ' ') {
         DrawTextBase(rc, font_, ybase, s, len, fore);
         return;
      }
   }
}

//********************************************************************************************************************

void SurfacePan::MeasureWidths(Scintilla::Font &Font, const char *String, int Length, int *Positions)
{
   auto handle = GetFont(Font);
   double position = 0;
   uint32_t previous = 0;

   for (int i = 0; i < Length; ) {
      int bytes = 1;
      uint32_t unicode = UTF8ReadValue(std::string_view(String + i, size_t(Length - i)), &bytes);
      bytes = std::clamp(bytes, 1, Length - i);
      double kerning = 0;
      const double advance = vec::CharWidth(handle, unicode, previous, &kerning);
      position += kerning;
      position += advance;
      previous = unicode;
      const int rounded = int(std::round(position));
      for (int end = i + bytes; i < end; i++) Positions[i] = rounded;
   }
}

//********************************************************************************************************************

int SurfacePan::WidthText(Scintilla::Font &Font, const char *String, int Length)
{
   if (Length <= 0) return 0;
   if (auto handle = GetFont(Font)) {
      return int(std::round(vec::StringWidth(handle, std::string_view(String, size_t(Length)), -1)));
   }
   return 5;
}

//********************************************************************************************************************

int SurfacePan::WidthChar(Scintilla::Font &Font, char Char)
{
   return int(std::round(vec::CharWidth(GetFont(Font), uint8_t(Char), 0, nullptr)));
}

//********************************************************************************************************************
// LineSpacing includes the room above the cap height needed by accented capitals.

int SurfacePan::Ascent(Scintilla::Font &Font)
{
   FontMetrics metrics;
   if (vec::GetFontMetrics(GetFont(Font), &metrics) IS ERR::Okay) {
      return std::max(0, metrics.LineSpacing - metrics.Descent);
   }
   return 10;
}

//********************************************************************************************************************

int SurfacePan::Descent(Scintilla::Font &Font)
{
   FontMetrics metrics;
   if (vec::GetFontMetrics(GetFont(Font), &metrics) IS ERR::Okay) return std::max(0, metrics.Descent);
   return 3;
}

//********************************************************************************************************************

int SurfacePan::InternalLeading(Scintilla::Font &Font)
{
   return 0;
}

//********************************************************************************************************************

int SurfacePan::ExternalLeading(Scintilla::Font &Font)
{
   return 0;
}

//********************************************************************************************************************

int SurfacePan::Height(Scintilla::Font &font_)
{
   return Ascent(font_) + Descent(font_);
}

//********************************************************************************************************************

int SurfacePan::AverageCharWidth(Scintilla::Font &font_)
{
   return WidthChar(font_, 'x');
}

//********************************************************************************************************************
// Functionality not required.

int SurfacePan::SetPalette(Scintilla::Palette *, bool)
{
   return 0;
}

//********************************************************************************************************************

void SurfacePan::SetClip(Scintilla::PRectangle rc)
{
   DBGDRAW("panSetClip","%dx%d,%dx%d", rc.left, rc.top, rc.Width(), rc.Height());
   cliprect = rc;

   /*if (bitmap) {
//        NOTE: just blindly set the clipping rect here, or check it?

      bitmap->ClipLeft = MAX(bitmap->ClipLeftrc.left;
      bitmap->ClipRight = rc.right;
      bitmap->ClipTop = rc.top;
      bitmap->ClipBottom = rc.bottom;
   }*/
}

//********************************************************************************************************************

void SurfacePan::FlushCachedState()
{
   kt::Log log(__FUNCTION__);
   log.trace("UNSUPPORTED");
}

//********************************************************************************************************************

void SurfacePan::SetUnicodeMode(bool unicodeMode_)
{
   kt::Log log(__FUNCTION__);
//   log.trace("%d [UNSUPPORTED]", unicodeMode_);
}

//********************************************************************************************************************

void SurfacePan::SetDBCSMode(int codePage)
{
   kt::Log log(__FUNCTION__);
//   log.trace("CodePage: %d [UNSUPPORTED]", codePage);
}

//********************************************************************************************************************
// Styles select the owning editor's shared handles; font handles are never freed by Scintilla.

int SurfacePan::font_index(const Scintilla::Font &Font) const
{
   return (Font.bold ? 1 : 0) + (Font.italic ? 2 : 0);
}

APTR SurfacePan::GetFont(Scintilla::Font &Font)
{
   return scintilla ? scintilla->FontHandles[font_index(Font)] : nullptr;
}

//********************************************************************************************************************

void SurfacePan::AlphaRectangle(Scintilla::PRectangle rc, int cornerSize, Scintilla::ColourAllocated fill,
   int alphaFill, Scintilla::ColourAllocated outline, int alphaOutline, int flags)
{
   if ((not bitmap) or (rc.Width() <= 0) or (rc.Height() <= 0)) return;

   // Following the GTK platform, the fill is inset by one pixel and the outline is drawn on the outer pixels.

   auto rounded = [](double X, double Y, double Width, double Height, double Radius) {
      Radius = std::min({ Radius, Width * 0.5, Height * 0.5 });
      if (Radius <= 0) {
         return std::format("M{} {} L{} {} L{} {} L{} {} Z", X, Y, X+Width, Y, X+Width, Y+Height, X, Y+Height);
      }
      return std::format("M{0} {1} L{2} {1} A{4} {4} 0 0 1 {3} {5} L{3} {6} A{4} {4} 0 0 1 {2} {7} L{0} {7} "
         "A{4} {4} 0 0 1 {8} {6} L{8} {5} A{4} {4} 0 0 1 {0} {1} Z",
         X + Radius, Y, X + Width - Radius, X + Width, Radius, Y + Radius, Y + Height - Radius, Y + Height, X);
   };

   APTR path;
   if ((rc.Width() > 2) and (rc.Height() > 2)) {
      auto inner = rounded(rc.left + 1, rc.top + 1, rc.Width() - 2, rc.Height() - 2, cornerSize);
      if (vec::GeneratePath(inner, &path) IS ERR::Okay) draw_path(path, &fill, alphaFill, nullptr, 0);
   }

   auto outer = rounded(rc.left + 0.5, rc.top + 0.5, rc.Width() - 1, rc.Height() - 1, cornerSize);
   if (vec::GeneratePath(outer, &path) IS ERR::Okay) draw_path(path, nullptr, 0, &outline, alphaOutline);
}

//********************************************************************************************************************

Scintilla::Surface * Scintilla::Surface::Allocate()
{
   return new SurfacePan();
}
