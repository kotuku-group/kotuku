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

   if (FT_Load_Glyph(font->face, path.glyph_index, font->glyph_flags)) return path;
   path.adv_x = int26p6_to_dbl(font->face->glyph->advance.x);
   path.adv_y = int26p6_to_dbl(font->face->glyph->advance.y);

   const FT_Outline &outline = font->face->glyph->outline;

   int first = 0; // index of first point in contour
   for (int n=0; n < outline.n_contours; n++) {
      int last = outline.contours[n];  // index of last point in contour
      FT_Vector *limit = outline.points + last;

      FT_Vector v_start = outline.points[first];
      FT_Vector v_last  = outline.points[last];
      FT_Vector v_control = v_start;
      FT_Vector *point = outline.points + first;
      char *tags = outline.tags  + first;
      char tag   = FT_CURVE_TAG(tags[0]);

      if (tag == FT_CURVE_TAG_CUBIC) return path; // A contour cannot start with a cubic control point!

      // check first point to determine origin
      if (tag == FT_CURVE_TAG_CONIC) { // first point is conic control.  Yes, this happens.
         if (FT_CURVE_TAG(outline.tags[last]) == FT_CURVE_TAG_ON) { // start at last point if it is on the curve
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

                      if (tag == FT_CURVE_TAG_ON) {
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
// For truetype fonts, this path generator creates text as a single path by concatenating the paths of all individual
// characters in the string.

static void generate_text(extVectorText *Vector, agg::path_storage &Path)
{
   kt::Log log(__FUNCTION__);

   if (!Vector->txHandle) {
      if (reset_font(Vector) != ERR::Okay) return;
   }

   auto &lines = Vector->txLines;
   if (lines.empty()) return;

   if (Vector->txBitmapFont) {
      raster_text_to_bitmap(Vector);
      return;
   }

   auto guide = Vector->GuidePath;
   double start_x, start_y, end_vx, end_vy;
   double path_scale = 1.0;
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
               path_scale = guide->PathLength / agg::path_length(guide->BasePath);
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
            uint32_t unicode = UTF8ReadValue(lstr, &char_len);
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

   FT_Activate_Size(pt.ft_size);
   if (pt.font->face->size->metrics.height != dbl_to_int26p6(Vector->txFontSize)) {
      FT_Set_Char_Size(pt.font->face, 0, dbl_to_int26p6(Vector->txFontSize), 72, 72);
   }

   if (guide) {
      // The scale_char transform is applied to each character to ensure that it is scaled to the path correctly.

      agg::trans_affine scale_char;

      if (path_scale != 1.0) {
         scale_char.translate(0, Vector->txFontSize);
         scale_char.scale(path_scale);
         scale_char.translate(0, -Vector->txFontSize * path_scale);
      }

      int char_index = 0;
      int cmd = -1;
      int prev_glyph_index = 0;
      double dist = 0; // Distance to next vertex
      double angle = 0;
      for (auto &line : Vector->txLines) {
         line.chars.clear();
         auto wrap_state = WS_NO_WORD;

         int char_len;
         for (auto str=line.c_str(); *str; ) {
            uint32_t unicode = UTF8ReadValue(str, &char_len);
            str += char_len;

            if (unicode <= 0x20) wrap_state = WS_NO_WORD;
            else if (wrap_state IS WS_NEW_WORD) wrap_state = WS_IN_WORD;
            else wrap_state = WS_NEW_WORD;

            if (unicode > 0x20) char_index++; // Index for transitions only increases if a glyph is being drawn

            agg::trans_affine transform(scale_char); // The initial transform scales the char to the path.

            if (Vector->Transition) { // Apply any special transitions to transform early.
               apply_transition(Vector->Transition, double(char_index) / double(total_chars), transform);
            }

            auto &glyph = pt.get_glyph(unicode);

            double kx, ky;
            get_kerning_xy(pt.font->face, glyph.glyph_index, prev_glyph_index, kx, ky);
            start_x += kx;

            double char_width = glyph.adv_x * std::abs(transform.sx); //transform.scale();

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

            if (unicode > 0x20) {
               transform.rotate(angle); // Rotate the character in accordance with its position on the path angle.
               transform.translate(tx, ty); // Move the character to its correct position on the path.
               agg::conv_transform<agg::path_storage, agg::trans_affine> trans_char(glyph.path, transform);
               Path.concat_path(trans_char);
            }

            //dx += char_width;
            //dy += glyph.adv_y + ky;

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

      for (auto &line : Vector->txLines) {
         line.chars.clear();
         auto wrap_state = WS_NO_WORD;
         if ((line.empty()) and (Vector->txCursor.vector)) {
            calc_caret_position(line, Vector->txFontSize);
         }
         else for (auto str=line.c_str(); *str; ) {
            int char_len;
            int unicode = UTF8ReadValue(str, &char_len);

            if (unicode <= 0x20) { wrap_state = WS_NO_WORD; prev_glyph_index = 0; }
            else if (wrap_state IS WS_NEW_WORD) wrap_state = WS_IN_WORD;
            else if (wrap_state IS WS_NO_WORD)  wrap_state = WS_NEW_WORD;

            if (!unicode) break;

            if (unicode > 0x20) char_index++; // Character index only increases if a glyph is being drawn

            agg::trans_affine transform;

            if (Vector->Transition) { // Apply any special transitions to the transform.
               apply_transition(Vector->Transition, double(char_index) / double(total_chars), transform);
            }

            // Determine if this is a new word that would wrap if drawn.
            // TODO: Wrapping information should be cached for speeding up subsequent redraws.

            if ((wrap_state IS WS_NEW_WORD) and (Vector->txInlineSize)) {
               int word_length = 0;
               for (int c=0; (str[c]) and (str[c] > 0x20); ) {
                  for (++c; ((str[c] & 0xc0) IS 0x80); c++);
                  word_length++;
               }

               auto word_width = string_width(Vector, std::string_view(str, word_length));

               if ((dx + word_width) * std::abs(transform.sx) >= Vector->txInlineSize) {
                  dx = 0;
                  dy += pt.line_spacing;
               }
            }

            str += char_len;

            auto &glyph = pt.get_glyph(unicode);

            double kx, ky;
            get_kerning_xy(pt.font->face, glyph.glyph_index, prev_glyph_index, kx, ky);
            dx += kx;

            double char_width = glyph.adv_x * std::abs(transform.sx); // transform.scale();

            transform.translate(dx, dy);
            agg::conv_transform<agg::path_storage, agg::trans_affine> trans_char(glyph.path, transform);
            Path.concat_path(trans_char);

            if (Vector->txCursor.vector) {
               calc_caret_position(line, transform, pt.ascent);

               if (!*str) { // Last character reached, add a final cursor entry past the character position.
                  transform.translate(char_width, 0);
                  calc_caret_position(line, transform, pt.ascent);
               }
            }

            dx += char_width;
            dy += glyph.adv_y + ky;

            prev_glyph_index = glyph.glyph_index;
         }

         if (dx > longest_line_width) longest_line_width = dx;
         dx = 0;
         dy += pt.line_spacing;
      }

      Vector->txWidth = longest_line_width;
   }

   if (Vector->txCursor.vector) Vector->txCursor.reset_vector(Vector);

   // Text paths are always oriented around (0,0) and are transformed later

   Vector->Bounds = { 0.0, double(-pt.ascent), Vector->txWidth, 1.0 };
   if (Vector->txLines.size() > 1) Vector->Bounds.bottom += (Vector->txLines.size() - 1) * pt.line_spacing;

   // If debugging the above boundary calculation, use this for verification of the true values (bear in
   // mind it will provide tighter numbers, which is normal).
   //bounding_rect_single(Path, 0, &Vector->BX1, &Vector->BY1, &Vector->BX2, &Vector->BY2);
}
