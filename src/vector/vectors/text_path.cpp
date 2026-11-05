// This code generates VectorText paths by concatenating Freetype glyphs.  It works by converting every unicode
// character in the source string to its freetype path.  The path is converted to its AGG equivalent, and will
// be cached for future cycles.
//
// * To produce the best quality output, Freetype can produce paths for the same font with hinted adjustments at
//   different point sizes.  Although it is possible to generate a single set of glyphs for a common point size and
//   scale that as necessary, this produces demonstrably worse results for the user.
//
// * The fastest possible drawing process is to render the glyphs as cached textures.  This can be done in most
//   situations if the VectorText transform is limited to scaling and we treat the glyphs as masks, which would allow
//   gradient and pattern fills to work as expected.

//********************************************************************************************************************
// Converts a Freetype glyph outline to an AGG path.  The size of the font must be preset in the FT_Outline object,
// with a call to FT_Set_Char_Size()
//
// REQUIREMENT: The caller must have acquired a lock on glFontMutex.

freetype_font::glyph & freetype_font::ft_point::get_glyph(uint32_t Unicode)
{
   double x1, y1, x2, y2, x3, y3;

   if (glyphs.contains(Unicode)) return glyphs[Unicode];

   auto &path = glyphs[Unicode];

   path.glyph_index = FT_Get_Char_Index(font->face, Unicode);

   if (font->face->size->metrics.height != ft_size->metrics.height) {
      FT_Set_Char_Size(font->face, 0, ft_size->metrics.height, 72, 72);
   }

   // Change the variable font metrics if necessary

   if ((font->active_size != this) and (!axis.empty())) {
      FT_Set_Var_Design_Coordinates(font->face, axis.size(), axis.data());
      font->active_size = this;
   }

   // WARNING: FT_Load_Glyph leaks memory if you call it repeatedly for the same glyph (hence the importance of caching).

   // Small sizes always receive grid fitting.  Fonts that would otherwise load without any (stub-hinted TrueType
   // fonts on the native loader) are switched to the light auto-hinter.  Layout is unaffected because the advances
   // are taken from the unhinted linear advance.

   auto load_flags = font->glyph_flags;
   if (small_text and (load_flags & FT_LOAD_NO_AUTOHINT)) load_flags = FT_LOAD_TARGET_LIGHT;

   if (FT_Load_Glyph(font->face, path.glyph_index, load_flags|FT_LOAD_LINEAR_DESIGN)) return path;

   // The hinted advance is rounded to whole pixels, which accumulates as visible spacing errors and disagrees with
   // the unrounded kerning.  The linear advance is the unhinted value in font units, including any variations, and
   // is scaled in the same way as the kerning.  FreeType's own scaling of the linear advance is avoided because it
   // passes through a rounded 16.16 scale factor.

   if (FT_IS_SCALABLE(font->face) and (em_scale > 0)) {
      path.adv_x = double(font->face->glyph->linearHoriAdvance) * em_scale;
   }
   else path.adv_x = int26p6_to_dbl(font->face->glyph->advance.x);
   path.adv_y = int26p6_to_dbl(font->face->glyph->advance.y);

   // Emboldening widens stems so that they cover at least one full pixel column.  The vertical strength is kept
   // lower so that hinted horizontal stems stay on the grid.  The advances are left unchanged.

   if ((embolden_x or embolden_y) and (font->face->glyph->format IS FT_GLYPH_FORMAT_OUTLINE)) {
      FT_Outline_EmboldenXY(&font->face->glyph->outline, embolden_x, embolden_y);
   }

   const FT_Outline &outline = font->face->glyph->outline;

   int first = 0; // index of first point in contour
   for (int n=0; n < outline.n_contours; n++) {
      int last = outline.contours[n];  // index of last point in contour
      FT_Vector *limit = outline.points + last;

      FT_Vector v_start = outline.points[first];
      FT_Vector v_last  = outline.points[last];
      FT_Vector v_control = v_start;
      FT_Vector *point = outline.points + first;
      auto tags = outline.tags + first;
      char tag   = FT_CURVE_TAG(tags[0]);

      if (tag IS FT_CURVE_TAG_CUBIC) return path; // A contour cannot start with a cubic control point!

      // check first point to determine origin
      if (tag IS FT_CURVE_TAG_CONIC) { // first point is conic control.  Yes, this happens.
         if (FT_CURVE_TAG(outline.tags[last]) IS FT_CURVE_TAG_ON) { // start at last point if it is on the curve
            v_start = v_last;
            limit--;
         }
         else { // if both first and last points are conic, start at their middle and record its position for closure
            v_start.x = (v_start.x + v_last.x) / 2;
            v_start.y = (v_start.y + v_last.y) / 2;
            v_last = v_start;
         }
         point--;
         tags--;
      }

      x1 = int26p6_to_dbl(v_start.x);
      y1 = -int26p6_to_dbl(v_start.y);
      path.path.move_to(x1, y1);

      while (point < limit) {
         point++;
         tags++;

         tag = FT_CURVE_TAG(tags[0]);
         switch(tag) {
            case FT_CURVE_TAG_ON: {  // emit a single line_to
               x1 = int26p6_to_dbl(point->x);
               y1 = -int26p6_to_dbl(point->y);
               path.path.line_to(x1, y1);
               continue;
            }

            case FT_CURVE_TAG_CONIC: { // consume conic arcs
               v_control.x = point->x;
               v_control.y = point->y;

               Do_Conic:
                  if (point < limit) {
                      FT_Vector vec, v_middle;

                      point++;
                      tags++;
                      tag = FT_CURVE_TAG(tags[0]);

                      vec.x = point->x;
                      vec.y = point->y;

                      if (tag IS FT_CURVE_TAG_ON) {
                          x1 = int26p6_to_dbl(v_control.x);
                          y1 = -int26p6_to_dbl(v_control.y);
                          x2 = int26p6_to_dbl(vec.x);
                          y2 = -int26p6_to_dbl(vec.y);
                          path.path.curve3(x1, y1, x2, y2);
                          continue;
                      }

                      if (tag != FT_CURVE_TAG_CONIC) return path;

                      v_middle.x = (v_control.x + vec.x) / 2;
                      v_middle.y = (v_control.y + vec.y) / 2;

                      x1 = int26p6_to_dbl(v_control.x);
                      y1 = -int26p6_to_dbl(v_control.y);
                      x2 = int26p6_to_dbl(v_middle.x);
                      y2 = -int26p6_to_dbl(v_middle.y);
                      path.path.curve3(x1, y1, x2, y2);
                      v_control = vec;
                      goto Do_Conic;
                  }

                  x1 = int26p6_to_dbl(v_control.x);
                  y1 = -int26p6_to_dbl(v_control.y);
                  x2 = int26p6_to_dbl(v_start.x);
                  y2 = -int26p6_to_dbl(v_start.y);
                  path.path.curve3(x1, y1, x2, y2);
                  goto _close;
            }

            default: { // FT_CURVE_TAG_CUBIC
               FT_Vector vec1, vec2;

               if (point + 1 > limit || FT_CURVE_TAG(tags[1]) != FT_CURVE_TAG_CUBIC) return path;

               vec1.x = point[0].x;
               vec1.y = point[0].y;
               vec2.x = point[1].x;
               vec2.y = point[1].y;

               point += 2;
               tags  += 2;

               if (point <= limit) {
                  FT_Vector vec;
                  vec.x = point->x;
                  vec.y = point->y;
                  x1 = int26p6_to_dbl(vec1.x);
                  y1 = -int26p6_to_dbl(vec1.y);
                  x2 = int26p6_to_dbl(vec2.x);
                  y2 = -int26p6_to_dbl(vec2.y);
                  x3 = int26p6_to_dbl(vec.x);
                  y3 = -int26p6_to_dbl(vec.y);
                  path.path.curve4(x1, y1, x2, y2, x3, y3);
                  continue;
               }

               x1 = int26p6_to_dbl(vec1.x);
               y1 = -int26p6_to_dbl(vec1.y);
               x2 = int26p6_to_dbl(vec2.x);
               y2 = -int26p6_to_dbl(vec2.y);
               x3 = int26p6_to_dbl(v_start.x);
               y3 = -int26p6_to_dbl(v_start.y);
               path.path.curve4(x1, y1, x2, y2, x3, y3);
               goto _close;
            }
         } // switch
      } // while

      path.path.close_polygon();

_close:
      first = last + 1;
   }

   return path;
}

//********************************************************************************************************************
// Returns separate placement and advance adjustments in pixels, at the scale of the glyph outlines.
// Values are not rounded.  GPOS pair positioning takes priority; faces without it use a legacy 'kern' table.
//
// REQUIREMENT: The caller must have acquired a lock on glFontMutex.

gpos_kerning::adjustment freetype_font::ft_point::positioning(int PrevGlyph, int Glyph)
{
   if ((not PrevGlyph) or (not Glyph) or (not kerning_enabled)) return {};

   auto &gpos = font->get_gpos();
   if (gpos.enabled()) {
      if (not kern_ready) {
         // The normalised coordinates of the style are read back from FreeType, which has already applied any
         // 'avar' mapping to them.

         std::vector<int> coords;
         if (not axis.empty()) {
            if (font->active_size != this) {
               FT_Set_Var_Design_Coordinates(font->face, axis.size(), axis.data());
               font->active_size = this;
            }

            std::vector<FT_Fixed> blend(axis.size());
            if (!FT_Get_Var_Blend_Coordinates(font->face, blend.size(), blend.data())) {
               for (auto v : blend) coords.push_back(int(std::lround(double(v) / 4.0))); // 16.16 to F2Dot14
            }
         }
         kern_instance.set_coords(coords);
         kern_ready = true;
      }
      auto value = gpos.pair_adjustment(PrevGlyph, Glyph, kern_instance);
      value.first_advance *= em_scale;
      value.second_advance *= em_scale;
      value.first_placement *= em_scale;
      value.second_placement *= em_scale;
      return value;
   }
   else if (FT_HAS_KERNING(font->face)) {
      FT_Vector delta;
      if (!FT_Get_Kerning(font->face, PrevGlyph, Glyph, FT_KERNING_UNSCALED, &delta)) {
         return { double(delta.x) * em_scale };
      }
   }
   return {};
}

//********************************************************************************************************************
// For truetype fonts, this path generator creates text as a single path by concatenating the paths of all individual
// characters in the string.

static void generate_text(extVectorText *Vector, agg::path_storage &Path)
{
   kt::Log log(__FUNCTION__);

   if (!Vector->txHandle) {
      if (reset_font(Vector) != ERR::Okay) return;
   }

   Vector->txWrappedLines = 0;

   auto &lines = Vector->txLines;
   if (lines.empty()) return;

   if (Vector->txBitmapFont) {
      raster_text_to_bitmap(Vector);
      return;
   }

   auto guide = Vector->GuidePath;
   int wrapped_lines = 0; // Extra lines created by InlineSize word-wrapping
   double start_x, start_y, end_vx, end_vy;
   double advance_scale = 1.0;
   if (guide) {
      if ((Vector->GuideFlags & VMF::STRETCH) != VMF::NIL) {
         // In stretch mode, the standard morphing algorithm is used (see gen_vector_path())
         guide = nullptr;
      }
      else {
         if (guide->dirty()) gen_vector_path(guide); // Regenerate the target path if necessary

         if (!guide->BasePath.total_vertices()) guide = nullptr;
         else {
            guide->BasePath.rewind(0);
            guide->BasePath.vertex(0, &start_x, &start_y);
            end_vx = start_x;
            end_vy = start_y;
            if (guide->PathLength > 0) {
               const double measured_length = agg::path_length(guide->BasePath);
               if (measured_length > 0) advance_scale = measured_length / guide->PathLength;
               guide->BasePath.rewind(0);
            }
         }
      }
   }

   // Compute the string length in characters if a transition is being applied

   int total_chars = 0;
   if (Vector->Transition) {
      for (auto const &line : Vector->txLines) {
         for (auto lstr=line.c_str(); *lstr; ) {
            int char_len;
            uint32_t unicode = UTF8ReadValue(std::string_view(line).substr(size_t(lstr - line.data())), &char_len);
            lstr += char_len;
            if (unicode <= 0x20) continue;
            else if (!unicode) break;
            total_chars++;
         }
      }
   }

   const std::lock_guard lock(glFontMutex);

   auto &pt = *(freetype_font::ft_point *)Vector->txHandle;

   // Freetype seems to be happier when the DPI is maintained at its native 72, and we get font results
   // that match Chrome's output.  It also means that 1:1 metrics don't need to be converted to 96 DPI
   // point sizes.

   const bool kern = (Vector->txFlags & VTXF::NO_KERNING) IS VTXF::NIL;

   FT_Activate_Size(pt.ft_size);
   if (pt.font->face->size->metrics.height != dbl_to_int26p6(Vector->txFontSize)) {
      FT_Set_Char_Size(pt.font->face, 0, dbl_to_int26p6(Vector->txFontSize), 72, 72);
   }

   if (guide) {
      int char_index = 0;
      int cmd = -1;
      int prev_glyph_index = 0;
      double dist = 0; // Distance to next vertex
      double angle = 0;
      for (auto &line : Vector->txLines) {
         line.chars.clear();
         line.wrapOffsets.clear(); // Guide paths do not wrap
         prev_glyph_index = 0; // Kerning does not apply across lines

         int char_len;
         for (auto str=line.c_str(); *str; ) {
            uint32_t unicode = UTF8ReadValue(std::string_view(line).substr(size_t(str - line.data())), &char_len);
            str += char_len;

            if (unicode > 0x20) char_index++; // Index for transitions only increases if a glyph is being drawn

            agg::trans_affine transform;

            if (Vector->Transition) { // Apply any special transitions to transform early.
               apply_transition(Vector->Transition, double(char_index) / double(total_chars), transform);
            }

            auto &glyph = pt.get_glyph(unicode);

            auto adjustment = kern ? pt.positioning(prev_glyph_index, glyph.glyph_index) :
               gpos_kerning::adjustment{};
            start_x += adjustment.first_advance * advance_scale;
            const double placement = kern ? adjustment.second_placement +
               pt.first_placement(glyph.glyph_index, str) : 0;

            double char_width = (glyph.adv_x + adjustment.second_advance) * std::abs(transform.sx) * advance_scale;

            // Compute end_vx,end_vy (the last vertex to use for angle computation) and store the distance from start_x,start_y to end_vx,end_vy in dist.
            if (char_width > dist) {
               while (cmd != agg::path_cmd_stop) {
                  double current_x, current_y;
                  cmd = guide->BasePath.vertex(&current_x, &current_y);
                  if (agg::is_vertex(cmd)) {
                     const double x = (current_x - end_vx), y = (current_y - end_vy);
                     const double vertex_dist = sqrt((x * x) + (y * y));
                     dist += vertex_dist;

                     //log.trace("%c char_width: %.2f, VXY: %.2f %.2f, next: %.2f %.2f, dist: %.2f", current_char, char_width, start_x, start_y, end_vx, end_vy, dist);

                     end_vx = current_x;
                     end_vy = current_y;

                     if (char_width <= dist) break; // Stop processing vertices if dist exceeds the character width.
                  }
               }
            }

            // At this stage we can say that start_x,start_y is the bottom left corner of the character and end_vx,end_vy is
            // the bottom right corner.

            double tx = start_x, ty = start_y;

            if (cmd != agg::path_cmd_stop) { // Advance (start_x,start_y) to the next point on the guide path.
               angle = std::atan2(end_vy - start_y, end_vx - start_x);

               double x = end_vx - start_x, y = end_vy - start_y;
               double d = std::sqrt(x * x + y * y);
               start_x += x / d * char_width;
               start_y += y / d * char_width;

               dist -= char_width; // The distance to the next vertex is reduced by the width of the char.
            }
            else { // No more path to use - advance start_x,start_y by the last known angle.
               start_x += char_width * cos(angle);
               start_y += char_width * sin(angle);
            }

            //log.trace("Char '%c' is at (%.2f %.2f) to (%.2f %.2f), angle: %.2f, remaining dist: %.2f", unicode, tx, ty, start_x, start_y, angle / DEG2RAD, dist);

            tx += placement * advance_scale * cos(angle);
            ty += placement * advance_scale * sin(angle);
            transform.rotate(angle); // Rotate the character in accordance with its position on the path angle.
            transform.translate(tx, ty); // Move the character to its correct position on the path.
            if (unicode > 0x20) {
               agg::conv_transform<agg::path_storage, agg::trans_affine> trans_char(glyph.path, transform);
               Path.concat_path(trans_char);
            }

            if (Vector->needs_caret_geometry()) {
               transform.tx -= placement * advance_scale * cos(angle);
               transform.ty -= placement * advance_scale * sin(angle);
               calc_caret_position(line, transform, pt.ascent, pt.descent);
               if (not *str) {
                  transform.tx = start_x;
                  transform.ty = start_y;
                  calc_caret_position(line, transform, pt.ascent, pt.descent);
               }
            }

            prev_glyph_index = glyph.glyph_index;
         }
      }
      Vector->txWidth = start_x;
   }
   else {
      double dx = 0, dy = 0; // Text coordinate tracking from (0,0), not transformed
      double longest_line_width = 0;
      int prev_glyph_index = 0;
      int char_index = 0;
      const auto wrap_mode = Vector->effective_wrap_mode();

      for (auto &line : Vector->txLines) {
         line.chars.clear();
         line.wrapOffsets.clear();
         bool in_word = false;
         bool char_break = false; // ANYWHERE mode: break between characters until segment_end is reached
         prev_glyph_index = 0; // Kerning does not apply across lines
         const char *segment_end = nullptr; // End of the current wrapping segment
         unsigned previous_start = Path.total_vertices(), previous_end = previous_start;
         double previous_sx = 1, previous_shy = 0;

         auto wrap_line = [&](const char *Position) {
            if (dx > longest_line_width) longest_line_width = dx;
            dx = 0;
            dy += pt.line_spacing;
            wrapped_lines++;
            prev_glyph_index = 0; // Kerning does not apply across a line break
            line.wrapOffsets.push_back(int(Position - line.c_str()));
         };

         if ((line.empty()) and (Vector->needs_caret_geometry())) {
            agg::trans_affine transform;
            transform.translate(dx, dy);
            calc_caret_position(line, transform, pt.ascent, pt.descent);
         }
         else for (auto str=line.c_str(); *str; ) {
            int char_len;
            int unicode = UTF8ReadValue(std::string_view(line).substr(size_t(str - line.data())), &char_len);

            if (!unicode) break;

            // A segment starts at the first character of a word, and at each break point within a word that
            // segment_extent() reports.  In VTW::WORD mode the segments are whole words.

            bool segment_start = false, word_start = false;
            if (unicode <= 0x20) { in_word = false; char_break = false; }
            else if (not in_word) { in_word = true; segment_start = true; word_start = true; }
            else if ((segment_end) and (str >= segment_end)) segment_start = true;

            if (unicode > 0x20) char_index++; // Character index only increases if a glyph is being drawn

            agg::trans_affine transform;

            if (Vector->Transition) { // Apply any special transitions to the transform.
               apply_transition(Vector->Transition, double(char_index) / double(total_chars), transform);
            }

            // Determine if this is a new segment that would wrap if drawn.
            // TODO: Wrapping information should be cached for speeding up subsequent redraws.

            if (Vector->txInlineSize) {
               const double scale = std::abs(transform.sx);
               if (segment_start) {
                  auto extent = segment_extent(str, wrap_mode, word_start);
                  segment_end = str + extent.bytes;
                  auto segment_width = string_width(Vector, std::string_view(str, extent.bytes));

                  // A segment that is too wide for an empty line stays on that line instead of leaving it blank.
                  if ((dx > 0) and ((dx + segment_width) * scale >= Vector->txInlineSize)) wrap_line(str);

                  // In ANYWHERE mode, a segment that still doesn't fit is broken between characters.
                  char_break = (wrap_mode IS VTW::ANYWHERE) and
                     ((dx + segment_width) * scale > Vector->txInlineSize);
               }
            }

            // The glyph reference is retrieved after string_width(), which can modify the glyph cache.

            auto &glyph = pt.get_glyph(unicode);

            auto adjustment = kern ? pt.positioning(prev_glyph_index, glyph.glyph_index) :
               gpos_kerning::adjustment{};
            double kx = adjustment.first_advance;
            if (Vector->txInlineSize and not segment_start and char_break and (dx > 0) and
                ((dx + kx + glyph.adv_x) * std::abs(transform.sx) > Vector->txInlineSize)) {
               wrap_line(str);
               kx = 0; // Kerning does not apply across a line break
               adjustment = {};
            }
            dx += kx;

            str += char_len;

            double char_width = (glyph.adv_x + adjustment.second_advance) * std::abs(transform.sx);

            // Apply first-glyph placement only after wrapping has decided whether this pair stays together.
            if (adjustment.first_placement) {
               for (auto vertex=previous_start; vertex < previous_end; vertex++) {
                  double x, y;
                  if (agg::is_vertex(Path.vertex(vertex, &x, &y))) {
                     Path.modify_vertex(vertex, x + adjustment.first_placement * previous_sx,
                        y + adjustment.first_placement * previous_shy);
                  }
               }
            }
            previous_start = Path.total_vertices();
            previous_sx = transform.sx;
            previous_shy = transform.shy;
            const double placement = adjustment.second_placement;
            transform.translate(dx + placement, dy);
            agg::conv_transform<agg::path_storage, agg::trans_affine> trans_char(glyph.path, transform);
            Path.concat_path(trans_char);
            previous_end = Path.total_vertices();

            if (Vector->needs_caret_geometry()) {
               transform.tx -= placement;
               calc_caret_position(line, transform, pt.ascent, pt.descent);

               if (!*str) { // Last character reached, add a final cursor entry past the character position.
                  transform.translate(char_width, 0);
                  calc_caret_position(line, transform, pt.ascent, pt.descent);
               }
            }

            dx += char_width;
            dy += glyph.adv_y;

            prev_glyph_index = glyph.glyph_index;
         }

         if (dx > longest_line_width) longest_line_width = dx;
         dx = 0;
         dy += pt.line_spacing;
      }

      Vector->txWidth = longest_line_width;
   }

   if (Vector->needs_caret_geometry()) Vector->txCursor.reset_vector(Vector);
   rebuild_selection(Vector);

   // Text paths are always oriented around (0,0) and are transformed later

   Vector->txWrappedLines = wrapped_lines;
   Vector->Bounds = { 0.0, double(-pt.ascent), Vector->txWidth, 1.0 };
   auto total_lines = std::ssize(Vector->txLines) + wrapped_lines;
   if (total_lines > 1) Vector->Bounds.bottom += (total_lines - 1) * pt.line_spacing;

   // If debugging the above boundary calculation, use this for verification of the true values (bear in
   // mind it will provide tighter numbers, which is normal).
   //bounding_rect_single(Path, 0, &Vector->BX1, &Vector->BY1, &Vector->BX2, &Vector->BY2);
}
