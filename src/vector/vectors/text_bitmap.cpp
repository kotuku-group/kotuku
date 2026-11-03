
//********************************************************************************************************************
// Rastered fonts are drawn as a rectangular block referencing a VectorImage texture that contains the rendered font.

static void raster_text_to_bitmap(extVectorText *Vector)
{
   kt::Log log(__FUNCTION__);

   if (!Vector->txBitmapFont) {
      reset_font(Vector);
      if (!Vector->txBitmapFont) return;
   }

   auto &lines = Vector->txLines;
   if (!lines.size()) return;

   int dx = 0, dy = Vector->txBitmapFont->Leading;
   int longest_line_width = 0;

   if ((not Vector->txInlineSize) and (not Vector->needs_caret_geometry())) { // No wrapping or selection
      for (auto &line : Vector->txLines) {
         line.chars.clear();
         line.wrapOffsets.clear();
         int line_width = fnt::StringWidth(Vector->txBitmapFont, line, -1);
         if (line_width > longest_line_width) longest_line_width = line_width;
         dy += Vector->txBitmapFont->LineSpacing;
      }
   }
   else {
      const auto wrap_mode = Vector->effective_wrap_mode();
      for (auto &line : Vector->txLines) {
         line.chars.clear();
         line.wrapOffsets.clear();
         bool in_word = false;
         bool char_break = false; // ANYWHERE mode: break between characters until segment_end is reached
         const char *segment_end = nullptr; // End of the current wrapping segment

         auto wrap_line = [&](const char *Position) {
            line.wrapOffsets.push_back(int(Position - line.c_str()));
            if (dx > longest_line_width) longest_line_width = dx;
            dx = 0;
            dy += Vector->txBitmapFont->LineSpacing;
            Vector->txWrappedLines++;
         };

         if (line.empty() and Vector->needs_caret_geometry()) {
            const auto top = dy - Vector->txBitmapFont->Leading;
            line.chars.emplace_back(0, top, 0, top + Vector->txBitmapFont->Height);
         }
         for (auto str=line.c_str(); *str; ) {
            int char_len;
            auto unicode = UTF8ReadValue(std::string_view(line).substr(size_t(str - line.data())), &char_len);

            auto char_width = fnt::CharWidth(Vector->txBitmapFont, unicode);

            if (!unicode) break;

            // A segment starts at the first character of a word, and at each break point within a word that
            // segment_extent() reports.  In VTW::WORD mode the segments are whole words.

            bool segment_start = false, word_start = false;
            if (unicode <= 0x20) { in_word = false; char_break = false; }
            else if (not in_word) { in_word = true; segment_start = true; word_start = true; }
            else if ((segment_end) and (str >= segment_end)) segment_start = true;

            // Determine if this is a new segment that would wrap if drawn.
            // TODO: Wrapping information should be cached for speeding up subsequent redraws.

            if (Vector->txInlineSize) {
               if (segment_start) {
                  auto extent = segment_extent(str, wrap_mode, word_start);
                  segment_end = str + extent.bytes;
                  int segment_width = fnt::StringWidth(Vector->txBitmapFont, str, extent.chars);

                  // A segment that is too wide for an empty line stays on that line instead of leaving it blank.
                  if ((dx > 0) and (dx + segment_width >= Vector->txInlineSize)) wrap_line(str);

                  // In ANYWHERE mode, a segment that still doesn't fit is broken between characters.
                  char_break = (wrap_mode IS VTW::ANYWHERE) and (dx + segment_width > Vector->txInlineSize);
               }
               else if ((char_break) and (dx > 0) and (dx + char_width > Vector->txInlineSize)) {
                  wrap_line(str);
               }
            }

            str += char_len;

            if (Vector->needs_caret_geometry()) {
               const auto top = dy - Vector->txBitmapFont->Leading;
               line.chars.emplace_back(dx, top, dx, top + Vector->txBitmapFont->Height);
               if (!*str) { // Last character reached, add a final cursor entry past the character position.
                  line.chars.emplace_back(dx + char_width, top, dx + char_width,
                     top + Vector->txBitmapFont->Height);
               }
            }

            dx += char_width;
         }

         if (dx > longest_line_width) longest_line_width = dx;
         dx = 0;
         dy += Vector->txBitmapFont->LineSpacing;
      }
   }

   if (dy < Vector->txBitmapFont->LineSpacing) dy = Vector->txBitmapFont->LineSpacing; // Enforce min. height

   // Standard rectangle to host the text image.

   Vector->BasePath.move_to(0, 0);
   Vector->BasePath.line_to(longest_line_width, 0);
   Vector->BasePath.line_to(longest_line_width, dy);
   Vector->BasePath.line_to(0, dy);
   Vector->BasePath.close_polygon();

   Vector->txWidth = longest_line_width;

   if (Vector->needs_caret_geometry()) {
      Vector->txCursor.reset_vector(Vector);
   }
   rebuild_selection(Vector);

   if (!Vector->txBitmapImage) {
      if (!(Vector->txAlphaBitmap = objBitmap::create::local(
            fl::Width(std::max(1, longest_line_width)),
            fl::Height(dy),
            fl::BitsPerPixel(32),
            fl::Flags(BMF::ALPHA_CHANNEL)))) return;

      if (!(Vector->txBitmapImage = extVectorImage::create::local(
            fl::Bitmap(Vector->txAlphaBitmap),
            fl::SpreadMethod(VSPREAD::CLIP),
            fl::Units(VUNIT::BOUNDING_BOX),
            fl::AspectRatio(ARF::X_MIN|ARF::Y_MIN)))) return;
   }
   else acResize(Vector->txAlphaBitmap, std::max(1, longest_line_width), dy, 0);

   Vector->Fill.Image = Vector->txBitmapImage;
   Vector->DisableFillColour = true;

   Vector->txBitmapFont->Bitmap = Vector->txAlphaBitmap;

   gfx::DrawRectangle(Vector->txAlphaBitmap, 0, 0, Vector->txAlphaBitmap->Width, Vector->txAlphaBitmap->Height, 0x00000000, BAF::FILL);

   // Draw each visual line as split by the layout above, so that the Font class cannot apply a different wrapping
   // policy.  Trailing whitespace before a wrap stays with the earlier line, matching the caret layout.

   Vector->txBitmapFont->WrapEdge = 0;
   Vector->txBitmapFont->Colour.Red   = int(Vector->Fill.Colour.Red * 255.0);
   Vector->txBitmapFont->Colour.Green = int(Vector->Fill.Colour.Green * 255.0);
   Vector->txBitmapFont->Colour.Blue  = int(Vector->Fill.Colour.Blue * 255.0);
   Vector->txBitmapFont->Colour.Alpha = int(Vector->Fill.Colour.Alpha * 255.0);

   int y = Vector->txBitmapFont->Leading;
   auto draw_line = [&](std::string_view Line) {
      if (not Line.empty()) {
         Vector->txBitmapFont->setString(Line);
         Vector->txBitmapFont->X = 0;
         Vector->txBitmapFont->Y = y;
         acDraw(Vector->txBitmapFont);
      }
      y += Vector->txBitmapFont->LineSpacing;
   };

   for (const auto &line : lines) {
      const std::string_view text(line);
      size_t start = 0;
      for (auto offset : line.wrapOffsets) {
         draw_line(text.substr(start, size_t(offset) - start));
         start = size_t(offset);
      }
      draw_line(text.substr(start));
   }

   // Text paths are always oriented around (0,0) and are transformed later

   Vector->Bounds = { 0, 0, Vector->txWidth, double(dy) };
}
