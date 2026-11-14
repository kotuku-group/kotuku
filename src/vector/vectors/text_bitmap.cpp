
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
   const int line_pitch = int(Vector->line_pitch(Vector->txBitmapFont->LineSpacing));
   const int paragraph_spacing = int(Vector->paragraph_spacing());
   const bool align_rows = needs_row_alignment(Vector);
   const bool decorate = (Vector->txFlags & (VTXF::UNDERLINE|VTXF::OVERLINE|VTXF::LINE_THROUGH)) != VTXF::NIL;
   const bool record_rows = align_rows or decorate;
   std::vector<TextRow> rows;

   if ((not Vector->txInlineSize) and (not Vector->needs_caret_geometry())) { // No wrapping or selection
      for (auto &line : Vector->txLines) {
         line.chars.clear();
         line.wrapOffsets.clear();
         int line_width = fnt::StringWidth(Vector->txBitmapFont, line, -1);
         if (line_width > longest_line_width) longest_line_width = line_width;
         if (record_rows) {
            int visible_width = 0;
            if (decorate) {
               // Measure only through the last visible character; retain the full width for layout and carets.
               size_t visible_end = 0;
               for (size_t offset=0; offset < line.size(); ) {
                  int length;
                  const auto unicode = UTF8ReadValue(std::string_view(line).substr(offset), &length);
                  offset += std::max(1, length);
                  if ((unicode > 0x20) and (text_character_class(unicode) != TextCharClass::SPACE)) {
                     visible_end = offset;
                  }
               }
               visible_width = fnt::StringWidth(Vector->txBitmapFont,
                  std::string_view(line).substr(0, visible_end), -1);
            }
            rows.push_back({ &line, 0, 0, 0, 0, double(line_width), false, {},
               double(dy + Vector->txBitmapFont->Height), double(visible_width) });
         }
         dy += line_pitch;
         if (&line != &Vector->txLines.back()) dy += paragraph_spacing;
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
         int visible_dx = 0; // Position after the last non-whitespace character of the visual line
         const char *row_start = line.c_str(), *visible_end = row_start;
         size_t row_first_char = 0;

         auto finish_row = [&](int Width) {
            if (Width > longest_line_width) longest_line_width = Width;
            if (record_rows) {
               const int visible_width = decorate ? fnt::StringWidth(Vector->txBitmapFont,
                  std::string_view(row_start, size_t(visible_end - row_start)), -1) : 0;
               rows.push_back({ &line, row_first_char, line.chars.size(), 0, 0, double(Width), false, {},
                  double(dy + Vector->txBitmapFont->Height), double(visible_width) });
            }
            row_first_char = line.chars.size();
         };

         auto wrap_line = [&](const char *Position) {
            line.wrapOffsets.push_back(int(Position - line.c_str()));
            finish_row(visible_dx); // Whitespace at a soft wrap hangs past the edge of the block
            dx = 0;
            visible_dx = 0;
            row_start = visible_end = Position;
            dy += line_pitch;
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
            if (unicode > 0x20) visible_dx = dx;
            if (decorate and (unicode > 0x20) and (text_character_class(unicode) != TextCharClass::SPACE)) {
               visible_end = str;
            }
         }

         finish_row(dx); // Whitespace before a hard break is retained so that the caret of an edited line is stable
         dx = 0;
         dy += line_pitch;
         if (&line != &Vector->txLines.back()) dy += paragraph_spacing;
      }
   }

   // The last row needs the natural height of the font, whatever the pitch between rows.

   dy += Vector->txBitmapFont->LineSpacing - line_pitch;

   if (dy < Vector->txBitmapFont->LineSpacing) dy = Vector->txBitmapFont->LineSpacing; // Enforce min. height

   // Font.Y receives the top of the row plus Leading, rather than its baseline.  In path coordinates the first
   // baseline is Height + Leading.  Keep that origin fixed when extending the texture for decoration lines.

   const int font_height = Vector->txBitmapFont->Height;
   const int natural_weight = std::max(1, int(std::round(font_height * 0.05)));
   const auto &weight = Vector->txDefinition.line_weight;
   const int line_weight = weight ? std::max(1, int(std::round(weight->pixels(Vector->txFontSize)))) : natural_weight;
   const std::array<VTXF, 3> line_flags = { VTXF::UNDERLINE, VTXF::OVERLINE, VTXF::LINE_THROUGH };
   const std::array<int, 3> line_tops = {
      std::max(1, Vector->txBitmapFont->Gutter / 2),
      -font_height - line_weight,
      -int(std::round(font_height * 0.3)) + int(std::round((natural_weight - line_weight) * 0.5))
   };
   int bitmap_top = 0, bitmap_bottom = dy;
   if (decorate) {
      for (const auto &row : rows) {
         if (row.visible_width <= 0) continue;
         for (size_t i=0; i < line_flags.size(); i++) {
            if ((Vector->txFlags & line_flags[i]) IS VTXF::NIL) continue;
            const int top = int(row.baseline) + line_tops[i];
            bitmap_top = std::min(bitmap_top, top);
            bitmap_bottom = std::max(bitmap_bottom, top + line_weight);
         }
      }
   }
   const int bitmap_height = bitmap_bottom - bitmap_top;

   // Standard rectangle to host the text image.

   Vector->BasePath.move_to(0, bitmap_top);
   Vector->BasePath.line_to(longest_line_width, bitmap_top);
   Vector->BasePath.line_to(longest_line_width, bitmap_bottom);
   Vector->BasePath.line_to(0, bitmap_bottom);
   Vector->BasePath.close_polygon();

   Vector->txWidth = longest_line_width;
   align_text_rows(Vector, rows, longest_line_width, nullptr);

   if (Vector->needs_caret_geometry()) {
      Vector->txCursor.reset_vector(Vector);
   }
   rebuild_selection(Vector);

   if (!Vector->txBitmapImage) {
      if (!(Vector->txAlphaBitmap = objBitmap::create::local(
            fl::Width(std::max(1, longest_line_width)),
            fl::Height(bitmap_height),
            fl::BitsPerPixel(32),
            fl::Flags(BMF::ALPHA_CHANNEL)))) return;

      if (!(Vector->txBitmapImage = extVectorImage::create::local(
            fl::Bitmap(Vector->txAlphaBitmap),
            fl::SpreadMethod(VSPREAD::CLIP),
            fl::Units(VUNIT::BOUNDING_BOX),
            fl::AspectRatio(ARF::X_MIN|ARF::Y_MIN)))) return;
   }
   else acResize(Vector->txAlphaBitmap, std::max(1, longest_line_width), bitmap_height, 0);

   Vector->Fill.Image = Vector->txBitmapImage;
   Vector->DisableFillColour = true;

   Vector->txBitmapFont->Bitmap = Vector->txAlphaBitmap;

   gfx::DrawRectangle(Vector->txAlphaBitmap, 0, 0, Vector->txAlphaBitmap->Width, Vector->txAlphaBitmap->Height,
      0x00000000, BAF::FILL);

   // Draw each visual line as split by the layout above, so that the Font class cannot apply a different wrapping
   // policy.  Trailing whitespace before a wrap stays with the earlier line, matching the caret layout.

   Vector->txBitmapFont->WrapEdge = 0;
   Vector->txBitmapFont->Colour.Red   = int(Vector->Fill.Colour.Red * 255.0);
   Vector->txBitmapFont->Colour.Green = int(Vector->Fill.Colour.Green * 255.0);
   Vector->txBitmapFont->Colour.Blue  = int(Vector->Fill.Colour.Blue * 255.0);
   Vector->txBitmapFont->Colour.Alpha = int(Vector->Fill.Colour.Alpha * 255.0);

   const auto colour = Vector->txAlphaBitmap->getColour(Vector->txBitmapFont->Colour);
   int y = Vector->txBitmapFont->Leading - bitmap_top;
   size_t row_index = 0;
   auto draw_line = [&](std::string_view Line) {
      const auto row = record_rows ? &rows[row_index++] : nullptr;
      const int x = align_rows ? int(row_alignment_shift(Vector, longest_line_width, row->width)) : 0;
      if (not Line.empty()) {
         Vector->txBitmapFont->setString(Line);
         Vector->txBitmapFont->X = x;
         Vector->txBitmapFont->Y = y;
         acDraw(Vector->txBitmapFont);
      }
      if (decorate and (row->visible_width > 0)) {
         for (size_t i=0; i < line_flags.size(); i++) {
            if ((Vector->txFlags & line_flags[i]) IS VTXF::NIL) continue;
            gfx::DrawRectangle(Vector->txAlphaBitmap, x, int(row->baseline) + line_tops[i] - bitmap_top,
               int(row->visible_width), line_weight, colour, BAF::FILL);
         }
      }
      y += line_pitch;
   };

   for (const auto &line : lines) {
      const std::string_view text(line);
      size_t start = 0;
      for (auto offset : line.wrapOffsets) {
         draw_line(text.substr(start, size_t(offset) - start));
         start = size_t(offset);
      }
      draw_line(text.substr(start));
      if (&line != &lines.back()) y += paragraph_spacing;
   }

   // Text paths are always oriented around (0,0) and are transformed later

   Vector->Bounds = { 0, double(bitmap_top), Vector->txWidth, double(bitmap_bottom) };
}
