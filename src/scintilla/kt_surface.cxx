#include "kt_surface.h"

// Vector's scalable and bitmap font decoders differ for invalid encodings.  Supply a common replacement glyph for
// each malformed byte while retaining the document's original bytes and Scintilla's byte-position arrays.

static std::string_view vector_text(std::string_view Text, std::string &Storage)
{
   for (size_t i = 0; i < Text.size(); ) {
      int bytes;
      const auto unicode = UTF8ReadValue(Text.substr(i), &bytes);
      const bool invalid = (unicode IS 0) and (uint8_t(Text[i]) >= 0x80);
      if (invalid) {
         if (Storage.empty()) Storage.assign(Text.substr(0, i));
         Storage.append("\xef\xbf\xbd");
      }
      else if (not Storage.empty()) Storage.append(Text.substr(i, size_t(bytes)));
      i += size_t(bytes);
   }
   return Storage.empty() ? Text : std::string_view(Storage);
}

SurfacePan::SurfacePan()
:  cliprect(0,0,0,0)
{
   scintilla = nullptr;
   bitmap = nullptr;
   own_bitmap = FALSE;
}

SurfacePan::~SurfacePan()
{
   Release();
}

void SurfacePan::Release() noexcept
{
   if (bitmap and own_bitmap) FreeResource(bitmap);
   bitmap = nullptr;
   own_bitmap = false;
   clips.clear();
   cliprect = PRectangle();
   scintilla = nullptr;
}

bool SurfacePan::Initialised()
{
   return bitmap != nullptr;
}

void SurfacePan::Init(WindowID WinID)
{
   scintilla = (extScintilla *)WinID;
}

void SurfacePan::Init(SurfaceID SurfaceData, WindowID WinID)
{
   if (bitmap or not SurfaceData) return;

   scintilla = (extScintilla *)WinID;

   // Surface id will be a bitmap object
   bitmap = (objBitmap *)SurfaceData;

   // Set the Clipping rect to the dimensions of the bitmap
   cliprect.left   = 0;
   cliprect.right  = bitmap->Width;
   cliprect.top    = 0;
   cliprect.bottom = bitmap->Height;
}

void SurfacePan::InitPixMap(int Width, int Height, WindowID WinID)
{
   if (bitmap or (Width <= 0) or (Height <= 0)) return;

   kt::Log log(__FUNCTION__);
   log.traceBranch("Size: %dx%d", Width, Height);

   scintilla = (extScintilla *)WinID;

   if (!(bitmap = objBitmap::create::local(fl::Width(Width), fl::Height(Height), fl::BitsPerPixel(32),
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


//********************************************************************************************************************
// Shapes are drawn with vec::DrawPath(), which provides anti-aliasing and alpha blending.  The Path is a generated
// vector path that is always freed by this function.  Stroke widths are in device pixels.

void SurfacePan::draw_path(APTR Path, const ColourRGBA *Fill, int FillAlpha,
   const ColourRGBA *Stroke, int StrokeAlpha, double StrokeWidth)
{
   if ((bitmap) and (scintilla) and (scintilla->FillColour) and (scintilla->StrokeColour)) {
      BitmapClipper clipper(bitmap, cliprect);

      if ((bitmap->Clip.Right > bitmap->Clip.Left) and (bitmap->Clip.Bottom > bitmap->Clip.Top)) {
         if (Fill) set_vector_colour(scintilla->FillColour, *Fill, FillAlpha);
         if (Stroke) set_vector_colour(scintilla->StrokeColour, *Stroke, StrokeAlpha);

         vec::DrawPath(bitmap, Path, Stroke ? StrokeWidth : 0.0, Stroke ? scintilla->StrokeColour : nullptr,
            Fill ? scintilla->FillColour : nullptr);
      }
   }

   FreeResource(Path);
}

//********************************************************************************************************************

void SurfacePan::BlendRectangle(PRectangle Rect, ColourRGBA Colour, int Alpha)
{
   if ((not bitmap) or (Rect.Width() <= 0) or (Rect.Height() <= 0)) return;

   APTR path;
   if (vec::GenerateRectangle(Rect.left, Rect.top, Rect.Width(), Rect.Height(), &path) IS ERR::Okay) {
      draw_path(path, &Colour, Alpha, nullptr, 0);
   }
}

//********************************************************************************************************************

int SurfacePan::LogPixelsY()
{
   return 96; // Kotuku's vector units are fixed at 96 DPI
}

//********************************************************************************************************************

int SurfacePan::DeviceHeightFont(int PointSize)
{
   int dpi = LogPixelsY();

   //MSG("DeviceHeightFont(%d, DPI %d)", PointSize, dpi);

   return (PointSize * dpi + (dpi / 2)) / 72;
}

//********************************************************************************************************************

// Draws an image of RGBA bytes, centred within rc and alpha blended with the background.

void SurfacePan::DrawRGBAImage(PRectangle Rect, int Width, int Height, const unsigned char *Pixels)
{
   if ((not bitmap) or (not Pixels) or (Width < 1) or (Height < 1)) return;

   if (Rect.Width() > Width) Rect.left += (Rect.Width() - Width) / 2;
   if (Rect.Height() > Height) Rect.top += (Rect.Height() - Height) / 2;

   if (auto image = objBitmap::create::local(fl::Width(Width), fl::Height(Height), fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL))) {
      for (int y=0; y < Height; y++) {
         auto dest = (uint32_t *)(image->Data + (image->LineWidth * y));
         auto src  = Pixels + (size_t(Width) * y * 4);
         for (int x=0; x < Width; x++, src += 4) dest[x] = image->packPixel(src[0], src[1], src[2], src[3]);
      }

      BitmapClipper clipper(bitmap, cliprect);
      gfx::CopyArea(image, bitmap, BAF::BLEND|BAF::LINEAR, 0, 0, Width, Height, Rect.left, Rect.top);
      FreeResource(image);
   }
}

//********************************************************************************************************************

void SurfacePan::FillRectangle(PRectangle Rect, ColourRGBA Back)
{
   BlendRectangle(Rect, Back, Back.GetAlpha());
}

//********************************************************************************************************************
// Tiles the bitmap of the pattern surface over rc, starting from the top-left corner of rc.

void SurfacePan::FillRectangle(PRectangle Rect, Surface &Pattern)
{
   if (not bitmap or Rect.Empty()) return;

   auto &pattern = (SurfacePan &)Pattern;
   if ((not pattern.bitmap) or (pattern.bitmap->Width < 1) or (pattern.bitmap->Height < 1)) {
      FillRectangle(Rect, ColourRGBA(0, 0, 0)); // Show the area as black, as the GTK platform does
      return;
   }

   DBGDRAW("panFillRectangle(2):","%dx%d,%dx%d", Rect.left, Rect.top, Rect.Width(), Rect.Height());

   BitmapClipper clipper(bitmap, cliprect);
   BitmapClipper rectangle_clipper(bitmap, Rect);
   if ((bitmap->Clip.Left >= bitmap->Clip.Right) or (bitmap->Clip.Top >= bitmap->Clip.Bottom)) return;
   const int pw = pattern.bitmap->Width, ph = pattern.bitmap->Height;
   for (int y=Rect.top; y < Rect.bottom; y += ph) {
      for (int x=Rect.left; x < Rect.right; x += pw) {
         gfx::CopyArea(pattern.bitmap, bitmap, BAF::BLEND|BAF::LINEAR, 0, 0, std::min(pw, int(Rect.right) - x),
            std::min(ph, int(Rect.bottom) - y), x, y);
      }
   }
}

//********************************************************************************************************************

void SurfacePan::Copy(PRectangle Rect, Point From, Surface &Source)
{
   kt::Log log(__FUNCTION__);
   SurfacePan &src_surface = (SurfacePan &)Source;

   if ((bitmap) and (src_surface.bitmap)) {
      BitmapClipper clipper(bitmap, cliprect);

      DBGDRAW("panCopy:","Bitmap: %d, Offset: %dx%d", bitmap->Head.UID, bitmap->XOffset, bitmap->YOffset);
      gfx::CopyArea(src_surface.bitmap, bitmap, BAF::NIL,
         From.x, From.y, Rect.Width(), Rect.Height(), /* source */
         Rect.left, Rect.top); /* dest */

   }
   else log.warning(ERR::Args);
}

//********************************************************************************************************************

void SurfacePan::draw_text(PRectangle Rect, const Font *Font, XYPOSITION Baseline,
   std::string_view Text, ColourRGBA Fore)
{
   if ((not bitmap) or (not scintilla) or Text.empty()) return;

   if (auto handle = GetFont(*Font)) {
      BitmapClipper clipper(bitmap, cliprect);
      set_vector_colour(scintilla->FillColour, Fore, Fore.GetAlpha());
      std::string storage;
      const auto text = vector_text(Text, storage);
      if (auto error = vec::DrawText(bitmap, handle, Rect.left, Baseline,
            text, scintilla->FillColour, nullptr); error != ERR::Okay) {
         kt::Log(__FUNCTION__).warning(error);
      }
   }
}

//********************************************************************************************************************

void SurfacePan::DrawTextNoClipUTF8(PRectangle Rect, const Font *Font, XYPOSITION Baseline,
   std::string_view Text, ColourRGBA Fore, ColourRGBA Back)
{
   DBGDRAW("panDrawTextNoClip()","");
   FillRectangle(Rect, Back);
   draw_text(Rect, Font, Baseline, Text, Fore);
}

//********************************************************************************************************************
// Intersect the text rectangle with the current surface and bitmap clips for the duration of the run.

void SurfacePan::DrawTextClippedUTF8(PRectangle Rect, const Font *Font, XYPOSITION Baseline,
   std::string_view Text, ColourRGBA Fore, ColourRGBA Back)
{
   DBGDRAW("panDrawTextClipped()", "");
   if (not bitmap) return;
   BitmapClipper clipper(bitmap, Rect);
   FillRectangle(Rect, Back);
   draw_text(Rect, Font, Baseline, Text, Fore);
}

//********************************************************************************************************************
// Avoid drawing spaces in transparent mode; i.e. only draw if there is at least one non-space char.

void SurfacePan::DrawTextTransparentUTF8(PRectangle Rect, const Font *Font, XYPOSITION Baseline,
   std::string_view Text, ColourRGBA Fore)
{
   if (Text.find_first_not_of(' ') != std::string_view::npos) draw_text(Rect, Font, Baseline, Text, Fore);
}

//********************************************************************************************************************

XYPOSITION SurfacePan::WidthTextUTF8(const Font *Font, std::string_view Text)
{
   if (Text.empty()) return 0;
   if (auto handle = GetFont(*Font)) {
      std::string storage;
      return vec::StringWidth(handle, vector_text(Text, storage), -1);
   }
   return 0;
}

//********************************************************************************************************************
// LineSpacing includes the room above the cap height needed by accented capitals.

XYPOSITION SurfacePan::Ascent(const Font *Font)
{
   FontMetrics metrics;
   if (vec::GetFontMetrics(GetFont(*Font), &metrics) IS ERR::Okay) {
      return std::max(0, metrics.LineSpacing - metrics.Descent);
   }
   return 10;
}

//********************************************************************************************************************

XYPOSITION SurfacePan::Descent(const Font *Font)
{
   FontMetrics metrics;
   if (vec::GetFontMetrics(GetFont(*Font), &metrics) IS ERR::Okay) return std::max(0, metrics.Descent);
   return 3;
}

//********************************************************************************************************************

XYPOSITION SurfacePan::Height(const Font *Font)
{
   return Ascent(Font) + Descent(Font);
}

//********************************************************************************************************************

XYPOSITION SurfacePan::AverageCharWidth(const Font *Font)
{
   return vec::CharWidth(GetFont(*Font), 'x', 0, nullptr);
}

//********************************************************************************************************************

void SurfacePan::SetClip(PRectangle Rect)
{
   clips.push_back(cliprect);
   cliprect = PRectangle(std::max(cliprect.left, Rect.left),
      std::max(cliprect.top, Rect.top), std::min(cliprect.right, Rect.right), std::min(cliprect.bottom, Rect.bottom));
}

//********************************************************************************************************************

void SurfacePan::FlushCachedState()
{
   // Bitmap/vector operations execute immediately.
}

//********************************************************************************************************************
// Vector owns its permanent cached handles.  A default descriptor is used only by completion lists.

APTR SurfacePan::GetFont(const Font &Font)
{
   const auto &font = (const PanFont &)Font;
   if (font.handle) return font.handle;
   return scintilla ? scintilla->FontHandles[0] : nullptr;
}

//********************************************************************************************************************
// Build a path with fractional geometry and a radius limited to the available dimensions.

static std::string rounded_path(PRectangle Rect, double Radius)
{
   const double x = Rect.left, y = Rect.top, width = Rect.Width(), height = Rect.Height();
   const double radius = std::clamp(Radius, 0.0, std::max(0.0, std::min(width, height) / 2));
   if (radius <= 0) {
      return std::format("M{} {} L{} {} L{} {} L{} {} Z", x, y, x+width, y, x+width, y+height, x, y+height);
   }
   return std::format("M{0} {1} L{2} {1} A{4} {4} 0 0 1 {3} {5} L{3} {6} A{4} {4} 0 0 1 {2} {7} L{0} {7} "
      "A{4} {4} 0 0 1 {8} {6} L{8} {5} A{4} {4} 0 0 1 {0} {1} Z",
      x + radius, y, x + width - radius, x + width, radius, y + radius, y + height - radius, y + height, x);
}

void SurfacePan::AlphaRectangle(PRectangle Rect, XYPOSITION Corner, FillStroke Colours)
{
   if ((not bitmap) or Rect.Empty()) return;
   const auto width = Colours.stroke.width;
   APTR path;
   const auto inner = Rect.Inset(width);
   if (not inner.Empty()) {
      if (vec::GeneratePath(rounded_path(inner, Corner), &path) IS ERR::Okay) {
         draw_path(path, &Colours.fill.colour, Colours.fill.colour.GetAlpha(), nullptr, 0);
      }
   }
   const auto outer = Rect.Inset(width / 2);
   if ((width > 0) and not outer.Empty()) {
      if (vec::GeneratePath(rounded_path(outer, Corner), &path) IS ERR::Okay) {
         draw_path(path, nullptr, 0, &Colours.stroke.colour, Colours.stroke.colour.GetAlpha(), width);
      }
   }
}

//********************************************************************************************************************

std::unique_ptr<Surface> Scintilla::Internal::Surface::Allocate(Scintilla::Technology)
{
   return std::make_unique<SurfacePan>();
}

std::unique_ptr<Surface> SurfacePan::AllocatePixMap(int Width, int Height)
{
   auto surface = std::make_unique<SurfacePan>();
   surface->SetMode(mode);
   surface->InitPixMap(Width, Height, scintilla);
   return surface;
}

int SurfacePan::SupportsFeature(Scintilla::Supports Feature) noexcept
{
   switch (Feature) {
      case Scintilla::Supports::LineDrawsFinal:
      case Scintilla::Supports::FractionalStrokeWidth:
      case Scintilla::Supports::TranslucentStroke:
      case Scintilla::Supports::PixelModification: return 1;
      default: return 0;
   }
}

void SurfacePan::LineDraw(Point Start, Point End,
   Stroke Stroke)
{
   const Point points[] = { Start, End };
   PolyLine(points, 2, Stroke);
}

void SurfacePan::PolyLine(const Point *Points, size_t Count,
   Stroke Stroke)
{
   if ((not bitmap) or (not Points) or (Count < 2)) return;
   std::string sequence;
   for (size_t i = 0; i < Count; i++) {
      sequence += std::format("{}{} {} ", i ? 'L' : 'M', Points[i].x, Points[i].y);
   }
   APTR path;
   if (vec::GeneratePath(sequence, &path) IS ERR::Okay) {
      draw_path(path, nullptr, 0, &Stroke.colour, Stroke.colour.GetAlpha(), Stroke.width);
   }
}

void SurfacePan::Polygon(const Point *Points, size_t Count,
   FillStroke Colours)
{
   if ((not bitmap) or (not Points) or (Count < 2)) return;
   std::string sequence;
   for (size_t i = 0; i < Count; i++) {
      sequence += std::format("{}{} {} ", i ? 'L' : 'M', Points[i].x, Points[i].y);
   }
   sequence += 'Z';
   APTR path;
   if (vec::GeneratePath(sequence, &path) IS ERR::Okay) {
      draw_path(path, &Colours.fill.colour, Colours.fill.colour.GetAlpha(),
         &Colours.stroke.colour, Colours.stroke.colour.GetAlpha(), Colours.stroke.width);
   }
}

void SurfacePan::RectangleDraw(PRectangle Rect, FillStroke Colours)
{
   if (Rect.Empty()) return;
   APTR path;
   const auto rect = Rect.Inset(Colours.stroke.width / 2);
   if (rect.Empty()) return;
   if (vec::GenerateRectangle(rect.left, rect.top, rect.Width(), rect.Height(), &path) IS ERR::Okay) {
      draw_path(path, &Colours.fill.colour, Colours.fill.colour.GetAlpha(),
         &Colours.stroke.colour, Colours.stroke.colour.GetAlpha(), Colours.stroke.width);
   }
}

void SurfacePan::RectangleFrame(PRectangle Rect, Stroke Stroke)
{
   const auto rect = Rect.Inset(Stroke.width / 2);
   if ((Stroke.width <= 0) or rect.Empty()) return;
   APTR path;
   if (vec::GenerateRectangle(rect.left, rect.top, rect.Width(), rect.Height(), &path) IS ERR::Okay) {
      draw_path(path, nullptr, 0, &Stroke.colour, Stroke.colour.GetAlpha(), Stroke.width);
   }
}

void SurfacePan::RoundedRectangle(PRectangle Rect, FillStroke Colours)
{
   const auto rect = Rect.Inset(Colours.stroke.width / 2);
   if (rect.Empty()) return;
   APTR path;
   if (vec::GeneratePath(rounded_path(rect, 2), &path) IS ERR::Okay) {
      draw_path(path, &Colours.fill.colour, Colours.fill.colour.GetAlpha(),
         &Colours.stroke.colour, Colours.stroke.colour.GetAlpha(), Colours.stroke.width);
   }
}

void SurfacePan::Ellipse(PRectangle Rect, FillStroke Colours)
{
   Rect = Rect.Inset(Colours.stroke.width / 2);
   if (Rect.Empty()) return;
   APTR path;
   if (vec::GenerateEllipse((Rect.left + Rect.right) / 2, (Rect.top + Rect.bottom) / 2,
         Rect.Width() / 2, Rect.Height() / 2, 0, &path) IS ERR::Okay) {
      draw_path(path, &Colours.fill.colour, Colours.fill.colour.GetAlpha(),
         &Colours.stroke.colour, Colours.stroke.colour.GetAlpha(), Colours.stroke.width);
   }
}

void SurfacePan::Stadium(PRectangle Rect, FillStroke Colours, Ends EndFlags)
{
   Rect = Rect.Inset(Colours.stroke.width / 2);
   if (Rect.Empty()) return;
   const double radius = std::min(Rect.Width(), Rect.Height()) / 2;
   const bool left_flat = (int(EndFlags) & int(Ends::leftFlat)) != 0;
   const bool left_angle = (int(EndFlags) & int(Ends::leftAngle)) != 0;
   const bool right_flat = (int(EndFlags) & int(Ends::rightFlat)) != 0;
   const bool right_angle = (int(EndFlags) & int(Ends::rightAngle)) != 0;
   const double left = Rect.left, right = Rect.right, top = Rect.top, bottom = Rect.bottom;
   std::string sequence = std::format("M{} {} ", left + radius, top);
   sequence += std::format("L{} {} ", right - radius, top);
   if (right_flat) sequence += std::format("L{} {} L{} {} ", right, top, right, bottom);
   else if (right_angle) sequence += std::format("L{} {} L{} {} ", right, (top + bottom) / 2, right - radius, bottom);
   else sequence += std::format("A{0} {0} 0 0 1 {1} {2} A{0} {0} 0 0 1 {3} {4} ",
      radius, right, top + radius, right - radius, bottom);
   sequence += std::format("L{} {} ", left + radius, bottom);
   if (left_flat) sequence += std::format("L{} {} L{} {} ", left, bottom, left, top);
   else if (left_angle) sequence += std::format("L{} {} ", left, (top + bottom) / 2);
   else sequence += std::format("A{0} {0} 0 0 1 {1} {2} A{0} {0} 0 0 1 {3} {4} ",
      radius, left, bottom - radius, left + radius, top);
   sequence += 'Z';
   APTR path;
   if (vec::GeneratePath(sequence, &path) IS ERR::Okay) {
      draw_path(path, &Colours.fill.colour, Colours.fill.colour.GetAlpha(),
         &Colours.stroke.colour, Colours.stroke.colour.GetAlpha(), Colours.stroke.width);
   }
}

void SurfacePan::GradientRectangle(PRectangle Rect,
   const std::vector<ColourStop> &Stops, GradientOptions Options)
{
   if (not bitmap or Rect.Empty() or Stops.empty()) return;
   if (Stops.size() IS 1) { FillRectangle(Rect, Fill(Stops.front().colour)); return; }

   std::vector<GradientStop> stops;
   stops.reserve(Stops.size());
   for (const auto &stop : Stops) {
      stops.push_back({ stop.position, { stop.colour.GetRedComponent(), stop.colour.GetGreenComponent(),
         stop.colour.GetBlueComponent(), stop.colour.GetAlphaComponent() } });
   }
   const bool horizontal = Options IS GradientOptions::leftToRight;
   auto gradient = objGradientLinear::create::local(fl::Units(VUNIT::USERSPACE),
      fl::X1(Rect.left), fl::Y1(Rect.top), fl::X2(horizontal ? Rect.right : Rect.left),
      fl::Y2(horizontal ? Rect.top : Rect.bottom));
   if (not gradient) return;
   auto release_gradient = kt::Defer([gradient]() { FreeResource(gradient); });
   if (gradient->setStops(stops) != ERR::Okay) return;
   APTR path;
   if (vec::GenerateRectangle(Rect.left, Rect.top, Rect.Width(), Rect.Height(), &path) IS ERR::Okay) {
      auto release_path = kt::Defer([path]() { FreeResource(path); });
      BitmapClipper clipper(bitmap, cliprect);
      if ((bitmap->Clip.Right > bitmap->Clip.Left) and (bitmap->Clip.Bottom > bitmap->Clip.Top)) {
         vec::DrawPath(bitmap, path, 0, nullptr, gradient);
      }
   }
}

void SurfacePan::MeasureWidthsUTF8(const Font *Font, std::string_view Text,
   XYPOSITION *Positions)
{
   auto handle = GetFont(*Font);
   double position = 0;
   uint32_t previous = 0;
   for (size_t i = 0; i < Text.size(); ) {
      int bytes = 1;
      uint32_t unicode = UTF8ReadValue(Text.substr(i), &bytes);
      if ((unicode IS 0) and (uint8_t(Text[i]) >= 0x80)) unicode = 0xfffd;
      const size_t length = std::clamp(size_t(std::max(1, bytes)), size_t(1), Text.size() - i);
      double kerning = 0;
      const double advance = vec::CharWidth(handle, unicode, previous, &kerning);
      position += kerning + advance;
      previous = unicode;
      for (size_t end = i + length; i < end; i++) Positions[i] = position;
   }
}

std::string_view SurfacePan::normal_text(std::string_view Text, std::string &Storage) const
{
   if (mode.codePage != 0) return Text;
   Storage.reserve(Text.size());
   for (uint8_t byte : Text) {
      if (byte < 0x80) Storage += char(byte);
      else { Storage += char(0xc0 | (byte >> 6)); Storage += char(0x80 | (byte & 0x3f)); }
   }
   return Storage;
}

void SurfacePan::MeasureWidths(const Font *Font, std::string_view Text, XYPOSITION *Positions)
{
   if (mode.codePage != 0) { MeasureWidthsUTF8(Font, Text, Positions); return; }
   auto handle = GetFont(*Font);
   double position = 0;
   uint32_t previous = 0;
   for (size_t i = 0; i < Text.size(); i++) {
      const auto unicode = uint8_t(Text[i]);
      double kerning = 0;
      const auto advance = vec::CharWidth(handle, unicode, previous, &kerning);
      position += advance + kerning;
      previous = unicode;
      Positions[i] = position;
   }
}
