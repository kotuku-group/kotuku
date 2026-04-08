
void anim_transform::perform()
{
   double seek_to = seek;

   if ((end_time) and (!freeze)) return;

   pf::ScopedObjectLock<objVector> vector(target_vector, 1000);
   if (vector.granted()) {
      vec::ResetMatrix(&matrix);
      switch(type) {
         case AT::TRANSLATE: {
            POINT<double> t_from = { 0, 0 }, t_to = { 0, 0 };

            if (not values.empty()) {
               int i;
               if (calc_mode IS CMODE::PACED) {
                  const auto dist_pos = seek * get_paired_dist();
                  for (i=0; (i < std::ssize(distances)-2) and (distances[i+1] < dist_pos); i++);
                  seek_to = std::clamp((dist_pos - distances[i]) / (distances[i+1] - distances[i]), 0.0, 1.0);
               }
               else {
                  i = int((values.size()-1) * seek);

                  const double mod = 1.0 / double(values.size() - 1);
                  seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
               }

               if (i >= std::ssize(values)-1) i = std::ssize(values) - 2;
               read_numseq(values[i], { &t_from.x, &t_from.y });
               read_numseq(values[i+1], { &t_to.x, &t_to.y } );
            }
            else if (not from.empty()) {
               read_numseq(from, { &t_from.x, &t_from.y });

               if (not to.empty()) {
                  read_numseq(to, { &t_to.x, &t_to.y } );
               }
               else if (not by.empty()) {
                  read_numseq(by, { &t_to.x, &t_to.y } );
                  t_to.x += t_from.x;
                  t_to.y += t_from.y;
               }
               else break;
            }
            else if (not to.empty()) break; // SVG prohibits the use of a single 'to' value for transforms.
            else if (not by.empty()) { // Placeholder; not correctly implemented
               read_numseq(by, { &t_to.x, &t_to.y } );
               t_from = t_to;
            }
            else break;

            const POINT<double> t_offset = t_to;

            if ((accumulate) and (repeat_count)) {
               const POINT<double> acc = t_offset * repeat_index;
               t_from += acc;
               t_to   += acc;
            }

            const double x = t_from.x + ((t_to.x - t_from.x) * seek_to);
            double y = t_from.y + ((t_to.y - t_from.y) * seek_to);
            matrix.TranslateX = x;
            matrix.TranslateY = y;
            break;
         }

         case AT::SCALE: {
            POINT<double> t_from = { 0, 0 }, t_to = { 0, 0 };

            if (not values.empty()) {
               int i;
               if (calc_mode IS CMODE::PACED) {
                  const auto dist_pos = seek * get_paired_dist();
                  for (i=0; (i < std::ssize(distances)-1) and (distances[i+1] < dist_pos); i++);
                  seek_to = (dist_pos - distances[i]) / (distances[i+1] - distances[i]);
               }
               else {
                  i = int((values.size()-1) * seek);
                  if (i >= std::ssize(values)-1) i = std::ssize(values) - 2;

                  const double mod = 1.0 / double(values.size() - 1);
                  seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
               }

               read_numseq(values[i], { &t_from.x, &t_from.y });
               read_numseq(values[i+1], { &t_to.x, &t_to.y } );

               if (!t_from.y) t_from.y = t_from.x;
            }
            else if (not from.empty()) {
               read_numseq(from, { &t_from.x, &t_from.y });
               if (!t_from.y) t_from.y = t_from.x;

               if (not to.empty()) {
                  read_numseq(to, { &t_to.x, &t_to.y } );
               }
               else if (not by.empty()) {
                  read_numseq(by, { &t_to.x, &t_to.y } );
                  t_to.x += t_from.x;
                  t_to.y += t_from.y;
               }
               else break;
            }
            else if (not to.empty()) break; // SVG prohibits the use of a single 'to' value for transforms.
            else if (not by.empty()) { // Placeholder; not correctly implemented
               read_numseq(by, { &t_to.x, &t_to.y } );
               t_from = t_to;
            }
            else break;

            if (!t_to.y) t_to.y = t_to.x;

            const POINT<double> t_offset = t_to;

            if ((accumulate) and (repeat_count)) {
               const POINT<double> acc = t_offset * repeat_index;
               t_from += acc;
               t_to   += acc;
            }

            const double x = t_from.x + ((t_to.x - t_from.x) * seek_to);
            double y = t_from.y + ((t_to.y - t_from.y) * seek_to);
            if (!y) y = x;
            matrix.ScaleX     *= x;
            matrix.ShearX     *= x;
            matrix.TranslateX *= x;
            matrix.ShearY     *= y;
            matrix.ScaleY     *= y;
            matrix.TranslateY *= y;
            break;
         }

         case AT::ROTATE: {
            ROTATE r_from, r_to;
            if (not values.empty()) {
               int i;
               if (calc_mode IS CMODE::PACED) {
                  // The get_total_dist() call will calculate the distance between angles as it operates on the first
                  // value of paired coordinates.
                  const auto dist_pos = seek * get_total_dist();
                  for (i=0; (i < std::ssize(distances)-1) and (distances[i+1] < dist_pos); i++);
                  seek_to = (dist_pos - distances[i]) / (distances[i+1] - distances[i]);
                  // keyTiming is not permitted in PACED mode.
               }
               else {
                  i = int((values.size()-1) * seek);
                  if (i >= std::ssize(values)-1) i = std::ssize(values) - 2;

                  const double mod = 1.0 / double(values.size() - 1);
                  seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
               }

               read_numseq(values[i], { &r_from.angle, &r_from.cx, &r_from.cy });
               read_numseq(values[i+1], { &r_to.angle, &r_to.cx, &r_to.cy } );
            }
            else if (not from.empty()) {
               read_numseq(from, { &r_from.angle, &r_from.cx, &r_from.cy });
               if (not to.empty()) {
                  read_numseq(to, { &r_to.angle, &r_to.cx, &r_to.cy } );
               }
               else if (not by.empty()) {
                  read_numseq(by, { &r_to.angle, &r_to.cx, &r_to.cy } );
                  r_to.angle += r_from.angle;
                  r_to.cx += r_from.cx;
                  r_to.cy += r_from.cy;
               }
               else break;
            }
            else if (not to.empty()) break; // SVG prohibits the use of a single 'to' value for transforms.
            else if (not by.empty()) { // Placeholder; not correctly implemented
               read_numseq(by, { &r_to.angle, &r_to.cx, &r_to.cy } );
               r_from = r_to;
            }
            else break;

            const auto r_offset = r_to;

            if ((accumulate) and (repeat_count)) {
               r_from += r_offset * repeat_index;
               r_to   += r_offset * repeat_index;
            }

            const ROTATE r_new = {
               r_from.angle + ((r_to.angle - r_from.angle) * seek_to),
               r_from.cx + ((r_to.cx - r_from.cx) * seek_to),
               r_from.cy + ((r_to.cy - r_from.cy) * seek_to)
            };

            vec::Rotate(&matrix, r_new.angle, r_new.cx, r_new.cy);
            break;
         }

         case AT::SKEW_X: {
            double t_from = 0, t_to = 0;

            if (not values.empty()) {
               int vi = int((values.size()-1) * seek);
               if (vi >= std::ssize(values)-1) vi = std::ssize(values) - 2;

               read_numseq(values[vi], { &t_from });
               read_numseq(values[vi+1], { &t_to } );

               const double mod = 1.0 / double(values.size() - 1);
               seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
            }
            else if (not from.empty()) {
               read_numseq(from, { &t_from });

               if (not to.empty()) {
                  read_numseq(to, { &t_to } );
               }
               else if (not by.empty()) {
                  read_numseq(by, { &t_to } );
                  t_to += t_from;
               }
               else break;
            }
            else if (not to.empty()) break; // SVG prohibits the use of a single 'to' value for transforms.
            else if (not by.empty()) { // Placeholder; not correctly implemented
               read_numseq(by, { &t_to } );
               t_from = t_to;
            }
            else break;

            const double t_offset = t_to;

            if ((accumulate) and (repeat_count)) {
               const double acc = t_offset * repeat_index;
               t_from += acc;
               t_to   += acc;
            }

            const double x = t_from + ((t_to - t_from) * seek_to);
            matrix.ShearX = tan(x * DEG2RAD);
            break;
         }

         case AT::SKEW_Y: {
            double t_from = 0, t_to = 0;

            if (not values.empty()) {
               int vi = int((values.size()-1) * seek);
               if (vi >= std::ssize(values)-1) vi = std::ssize(values) - 2;

               read_numseq(values[vi], { &t_from });
               read_numseq(values[vi+1], { &t_to } );

               const double mod = 1.0 / double(values.size() - 1);
               seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
            }
            else if (not from.empty()) {
               read_numseq(from, { &t_from });

               if (not to.empty()) {
                  read_numseq(to, { &t_to } );
               }
               else if (not by.empty()) {
                  read_numseq(by, { &t_to } );
                  t_to += t_from;
               }
               else break;
            }
            else if (not to.empty()) break; // SVG prohibits the use of a single 'to' value for transforms.
            else if (not by.empty()) { // Placeholder; not correctly implemented
               read_numseq(by, { &t_to } );
               t_from = t_to;
            }
            else break;

            const double t_offset = t_to;

            if ((accumulate) and (repeat_count)) {
               const double acc = t_offset * repeat_index;
               t_from += acc;
               t_to   += acc;
            }

            const double y = t_from + ((t_to - t_from) * seek_to);
            matrix.ShearY = tan(y * DEG2RAD);
            break;
         }

         default: return;
      } // switch

      svg->Animatrix[target_vector].transforms.push_back(this);
   }
}

