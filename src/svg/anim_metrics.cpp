
#include "../link/linear_rgb.h"

//********************************************************************************************************************

void anim_base::set_orig_value(svgState &State)
{
   if ((freeze and not from.empty()) or target_attrib.empty()) return;

   pf::ScopedObjectLock<objVector> obj(target_vector);
   if (obj.granted()) {
      auto fid = strihash(target_attrib);
      switch(fid) {
         case SVF_DISPLAY:
            if (obj->Visibility IS VIS::HIDDEN) target_attrib_orig = "none";
            else if (obj->Visibility IS VIS::INHERIT) target_attrib_orig = "inherit";
            else if (obj->Visibility IS VIS::VISIBLE) target_attrib_orig = "inline";
            break;

         case SVF_STROKE_WIDTH:
            target_attrib_orig.assign(std::to_string(obj->get<double>(FID_StrokeWidth)));
            break;

         case SVF_FILL: {
            CSTRING val;
            if ((obj->get(FID_Fill, val) IS ERR::Okay) and (val)) target_attrib_orig = val;
            else target_attrib_orig = State.m_fill;
            break;
         }

         case SVF_STROKE: {
            CSTRING val;
            if ((obj->get(FID_Stroke, val) IS ERR::Okay) and (val)) target_attrib_orig = val;
            else target_attrib_orig = State.m_stroke;
            break;
         }

         case SVF_FILL_OPACITY:
            if (obj->FillOpacity != 1.0) target_attrib_orig = obj->FillOpacity;
            else if (State.m_fill_opacity != -1) target_attrib_orig = State.m_fill_opacity;
            else target_attrib_orig = 1.0;
            break;

         case SVF_OPACITY:
            if (obj->Opacity != 1.0) target_attrib_orig = obj->Opacity;
            else if (State.m_opacity != -1) target_attrib_orig = State.m_opacity;
            else target_attrib_orig = 1.0;
            break;

         default: {
            std::string buffer;
            if (obj->get(fid, buffer) IS ERR::Okay) target_attrib_orig.assign(buffer);
         }
      }
   }
}

//********************************************************************************************************************

double anim_motion::get_total_dist()
{
   if (total_dist != 0) return total_dist;

   distances.clear();
   total_dist = 0;
   if (not points.empty()) {
      auto prev = points[0];
      for (auto &pt : points) {
         auto delta = prev - pt;
         total_dist += delta;
         distances.push_back(total_dist);
         prev = pt;
      }
   }
   else if (not values.empty()) {
      POINT<double> prev, pt;
      read_numseq(values[0], { &prev.x, &prev.y });
      distances.push_back(0);
      for (int i=1; i < std::ssize(values); i++) {
         read_numseq(values[i], { &pt.x, &pt.y });
         total_dist += prev - pt;
         distances.push_back(total_dist);
         prev = pt;
      }
   }
   else if (not from.empty()) {
      if (not to.empty()) {
         POINT<double> a, b;
         read_numseq(from, { &a.x, &a.y });
         read_numseq(to, { &b.x, &b.y } );
         total_dist = a - b;
      }
      else if (not by.empty()) {
         POINT<double> a = { 0, 0 }, b;
         read_numseq(by, { &b.x, &b.y } );
         total_dist = a - b;
      }
   }
   else return 0;

   return total_dist;
}

//********************************************************************************************************************
// The default for get_total_dist() is to use the first value in any series and not to include pairings.

double anim_base::get_total_dist()
{
   if (total_dist != 0) return total_dist;

   distances.clear();
   total_dist = 0;
   if (not values.empty()) {
      double prev, val;
      read_numseq(values[0], { &prev });
      distances.push_back(0);
      for (int i=1; i < std::ssize(values); i++) {
         read_numseq(values[i], { &val });
         total_dist += std::abs(val - prev);
         distances.push_back(total_dist);
         prev = val;
      }
   }
   else if (not from.empty()) {
      if (not to.empty()) {
         double a, b;
         read_numseq(from, { &a });
         read_numseq(to, { &b } );
         total_dist = std::abs(a - b);
      }
      else if (not by.empty()) {
         read_numseq(by, { &total_dist } );
         total_dist = std::abs(total_dist);
      }
   }
   else return 0;

   return total_dist;
}

//********************************************************************************************************************

double anim_base::get_paired_dist()
{
   if (total_dist != 0) return total_dist;

   distances.clear();
   if (not values.empty()) {
      POINT<double> a, b;
      read_numseq(values[0], { &b.x, &b.y });
      distances.push_back(0);
      for (int i=1; i < std::ssize(values); i++) {
         read_numseq(values[i], { &a.x, &a.y });
         total_dist += a - b;
         distances.push_back(total_dist);
         b = a;
      }
   }
   else if (not from.empty()) {
      POINT<double> a, b;
      if (not to.empty()) {
         read_numseq(from, { &a.x, &a.y });
         read_numseq(to, { &b.x, &b.y } );
         total_dist = a - b;
      }
      else if (not by.empty()) {
         read_numseq(by, { &a.x, &a.y } );
         b = { 0, 0 };
         total_dist = a - b;
      }
   }

   return total_dist;
}

//********************************************************************************************************************
// Return an interpolated value based on the values or from/to/by settings.

double anim_base::get_numeric_value(objVector &Vector, FIELD Field)
{
   double from_val, to_val;
   double seek_to = seek;

   if ((seek >= 1.0) and (!freeze)) {
      return strtod(target_attrib_orig.c_str(), nullptr);
   }

   if (not values.empty()) {
      int i;
      if (timing.size() IS values.size()) {
         seek *= timing.back(); // In discrete mode the last time doesn't have to be 1.0
         for (i=0; (i < std::ssize(timing)-1) and (timing[i+1] < seek); i++);
         const double delta = timing[i+1] - timing[i];
         seek_to = (seek - timing[i]) / delta;
      }
      else {
         i = std::clamp<int>(int((values.size()-1) * seek), 0, values.size() - 2);
         // Recompute the seek position to fit between the two values
         const double mod = 1.0 / double(values.size() - 1);
         seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
      }

      read_numseq(values[i], { &from_val });
      read_numseq(values[i+1], { &to_val } );
   }
   else if (not from.empty()) {
      if (not to.empty()) {
         read_numseq(from, { &from_val });
         read_numseq(to, { &to_val } );
      }
      else if (not by.empty()) {
         read_numseq(from, { &from_val });
         read_numseq(by, { &to_val } );
         to_val += from_val;
      }
      else return 0;
   }
   else if (not to.empty()) {
      from_val = Vector.get<double>(Field);
      from = std::to_string(from_val);
      read_numseq(to, { &to_val } );
   }
   else if (not by.empty()) {
      from_val = Vector.get<double>(Field);
      from = std::to_string(from_val);
      read_numseq(by, { &to_val } );
      to_val += from_val;
   }
   else return 0;

   const auto offset = to_val;

   if ((accumulate) and (repeat_count)) {
      // Cumulative animation is not permitted for:
      // * The 'to animation' where 'from' is undefined.

      from_val += offset * repeat_index;
      to_val += offset * repeat_index;
   }

   if (additive IS ADD::SUM) {
      from_val += offset;
      to_val   += offset;
   }

   if (calc_mode IS CMODE::DISCRETE) {
      if (seek_to < 0.5) return from_val;
      else return to_val;
   }
   else { // CMODE::LINEAR
      return from_val + ((to_val - from_val) * seek_to);
   }
}

//********************************************************************************************************************
// Suitable for <set> instructions only.  Very straight-forward as there is no interpolation.

std::string anim_base::get_string()
{
   if ((seek >= 1.0) and (!freeze)) return target_attrib_orig;

   if (not from.empty()) {
      if (seek < 0.5) return from;
      else if (not to.empty()) return to;
      else return target_attrib_orig;
   }
   else if (not to.empty()) return to;
   else return target_attrib_orig;
}

//********************************************************************************************************************
// Return an interpolated value based on the values or from/to/by settings.

double anim_base::get_dimension(objVector &Vector, FIELD Field)
{
   double from_val, to_val;
   double seek_to = seek;

   if (not values.empty()) {
      int i;

      if (calc_mode IS CMODE::PACED) {
         const auto dist_pos = seek * get_total_dist();
         for (i=0; (i < std::ssize(distances)-1) and (distances[i+1] < dist_pos); i++);
         seek_to = (dist_pos - distances[i]) / (distances[i+1] - distances[i]);
         // keyTiming is not permitted in PACED mode.
      }
      else if (calc_mode IS CMODE::SPLINE) {
         if (spline_paths.empty()) return 0;

         i = 0;
         if (timing.size() IS spline_paths.size()) {
            for (i=0; (i < std::ssize(timing)-1) and (timing[i+1] < seek); i++);
            i = std::clamp<int>(i, 0, timing.size() - 1);
         }
         else {
            // When no timing is specified, the 'values' are distributed evenly.  This determines
            // what spline-path we are going to use.

            i = std::clamp<int>(int(seek * std::ssize(spline_paths)), 0, std::ssize(spline_paths) - 1);
         }

         auto &sp = spline_paths[i]; // sp = The spline we're going to use

         // Rather than use distance, we're going to use the 'x' position as a lookup on the horizontal axis.
         // The paired y value then gives us the 'real' seek_to value.
         // The spline points are already sorted by the x value to make this easier.

         const double x = (seek >= 1.0) ? 1.0 : fmod(seek, 1.0 / double(std::ssize(spline_paths))) * std::ssize(spline_paths);

         int si;
         for (si=0; (si < std::ssize(sp.points)-1) and (sp.points[si+1].point.x < x); si++);

         const double mod_x = x - sp.points[si].point.x;
         const double c = mod_x / sp.points[si].cos_angle;
         seek_to = std::clamp(sp.points[si].point.y + std::sqrt((c * c) - (mod_x * mod_x)), 0.0, 1.0);
      }
      else { // CMODE::LINEAR
         if (timing.size() IS values.size()) {
            seek *= timing.back(); // In discrete mode the last time doesn't have to be 1.0

            for (i=0; (i < std::ssize(timing)-1) and (timing[i+1] < seek); i++);
            i = std::clamp<int>(i, 0, timing.size() - 2);
            const double delta = timing[i+1] - timing[i];
            seek_to = (seek - timing[i]) / delta;
         }
         else {
            i = std::clamp<int>(int((values.size()-1) * seek), 0, values.size() - 2);
            const double mod = 1.0 / double(values.size() - 1);
            seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
         }
      }

      read_numseq(values[i], { &from_val });
      read_numseq(values[i+1], { &to_val });
   }
   else if (not from.empty()) {
      if (not to.empty()) {
         read_numseq(from, { &from_val });
         read_numseq(to, { &to_val } );
      }
      else if (not by.empty()) {
         read_numseq(from, { &from_val });
         read_numseq(by, { &to_val } );
         to_val += from_val;
      }
      else return 0;
   }
   else if (not to.empty()) {
      from_val = Vector.get<double>(Field);
      from = std::to_string(from_val);
      read_numseq(to, { &to_val } );
   }
   else if (not by.empty()) {
      from_val = Vector.get<double>(Field);
      from = std::to_string(from_val);
      read_numseq(by, { &to_val } );
      to_val += from_val;
   }
   else return 0;

   const auto offset = to_val;

   if ((accumulate) and (repeat_count)) {
      // Cumulative animation is not permitted for:
      // * The 'to animation' where 'from' is undefined.
      // * Animations that do not repeat

      from_val += offset * repeat_index;
      to_val += offset * repeat_index;
   }

   if (additive IS ADD::SUM) {
      from_val += offset;
      to_val   += offset;
   }

   if (calc_mode IS CMODE::DISCRETE) {
      if (seek_to < 0.5) return from_val;
      else return to_val;
   }
   else { // CMODE::LINEAR
      return from_val + ((to_val - from_val) * seek_to);
   }
}

//********************************************************************************************************************

FRGB anim_base::get_colour_value(objVector &Vector, FIELD Field)
{
   VectorPainter from_col, to_col;
   double seek_to = seek;

   if (not values.empty()) {
      if (values.size() >= 2) {
         int vi = int((values.size()-1) * seek);
         if (vi >= int(values.size())-1) vi = values.size() - 2;
         vec::ReadPainter(nullptr, values[vi].c_str(), &from_col, nullptr);
         vec::ReadPainter(nullptr, values[vi+1].c_str(), &to_col, nullptr);

         const double mod = 1.0 / double(values.size() - 1);
         seek_to = (seek >= 1.0) ? 1.0 : fmod(seek, mod) / mod;
      }
      else if (values.size() IS 1) { // Equivalent to a single 'to'
         vec::ReadPainter(nullptr, target_attrib_orig.c_str(), &from_col, nullptr);
         vec::ReadPainter(nullptr, values[0].c_str(), &to_col, nullptr);
      }
      else return { 0, 0, 0, 0 };
   }
   else if (not from.empty()) {
      if (not to.empty()) {
         vec::ReadPainter(nullptr, from.c_str(), &from_col, nullptr);
         vec::ReadPainter(nullptr, to.c_str(), &to_col, nullptr);
      }
      else if (not by.empty()) {
         return { 0, 0, 0, 0 };
      }
   }
   else if (not to.empty()) {
      // The original value will be the 'from' in this situation
      vec::ReadPainter(nullptr, target_attrib_orig.c_str(), &from_col, nullptr);
      vec::ReadPainter(nullptr, to.c_str(), &to_col, nullptr);
   }
   else if (not by.empty()) {
      float *colour;
      int elements;
      if ((Vector.get(Field, colour, elements) IS ERR::Okay) and (elements IS 4)) {
         from_col.Colour = { colour[0], colour[1], colour[2], colour[3] };
         vec::ReadPainter(nullptr, to.c_str(), &to_col, nullptr);
         to_col.Colour.Red   = std::clamp<float>(to_col.Colour.Red   + colour[0], 0.0, 1.0);
         to_col.Colour.Green = std::clamp<float>(to_col.Colour.Green + colour[1], 0.0, 1.0);
         to_col.Colour.Blue  = std::clamp<float>(to_col.Colour.Blue  + colour[2], 0.0, 1.0);
         to_col.Colour.Alpha = std::clamp<float>(to_col.Colour.Alpha + colour[3], 0.0, 1.0);
      }
      else return { 0, 0, 0, 0 };
   }
   else return { 0, 0, 0, 0 };

   if ((seek_to >= 1.0) and (!freeze)) {
      VectorPainter painter;
      vec::ReadPainter(nullptr, target_attrib_orig.c_str(), &painter, nullptr);
      return painter.Colour;
   }

   if (calc_mode IS CMODE::DISCRETE) {
      if (seek_to < 0.5) return from_col.Colour;
      else return to_col.Colour;
   }
   else { // CMODE::LINEAR
      // Linear RGB interpolation is superior to operating on raw RGB values.

      glLinearRGB.convert(from_col.Colour);
      glLinearRGB.convert(to_col.Colour);

      auto result = FRGB {
         float(from_col.Colour.Red   + ((to_col.Colour.Red   - from_col.Colour.Red) * seek_to)),
         float(from_col.Colour.Green + ((to_col.Colour.Green - from_col.Colour.Green) * seek_to)),
         float(from_col.Colour.Blue  + ((to_col.Colour.Blue  - from_col.Colour.Blue) * seek_to)),
         float(from_col.Colour.Alpha + ((to_col.Colour.Alpha - from_col.Colour.Alpha) * seek_to))
      };

      glLinearRGB.invert(result);
      return result;
   }
}
