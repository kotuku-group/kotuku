//********************************************************************************************************************
// Rotation angles are pre-calculated once.

void anim_motion::precalc_angles()
{
   if (points.size() < 2) return;

   // Start by calculating all angles from point to point.

   std::vector<double> precalc(points.size());
   POINT prev = points[0];
   precalc[0] = get_angle(points[0], points[1]);
   for (int i=1; i < std::ssize(points)-1; i++) {
      precalc[i] = get_angle(prev, points[i]);
      prev = points[i];
   }
   precalc[points.size()-1] = precalc[points.size()-2];

   // Average out the angle for each point so that they have a smoother flow.

   angles.clear();
   angles.reserve(precalc.size());
   angles.push_back(precalc[0]);
   for (int i=1; i < std::ssize(precalc)-1; i++) {
      angles.push_back((precalc[i] + precalc[i-1] + precalc[i+1]) / 3);
   }
   angles.push_back(precalc.back());
}

//********************************************************************************************************************

static ERR motion_callback(objVector *Vector, int Index, int Cmd, double X, double Y, anim_motion &Motion)
{
   Motion.points.push_back(kt::POINT<float> { float(X), float(Y) });
   return ERR::Okay;
};

//********************************************************************************************************************

void anim_motion::perform()
{
   POINT<float> a, b;
   double angle = -1;
   double seek_to = seek;

   if ((end_time) and (!freeze)) return;

   kt::ScopedObjectLock<objVector> vector(target_vector, 1000);
   if (!vector.granted()) return;

   // Note that the order of processing here is important, and matches the priorities documented for SVG's
   // animateMotion property.

   if ((mpath) or (not path.empty())) {
      int new_timestamp;
      vector->getPathTimestamp(new_timestamp);

      if ((points.empty()) or (path_timestamp != new_timestamp)) {
         // Trace the path and store its points.  Transforms are completely ignored when pulling the path from
         // an external source.

         auto call = C_FUNCTION(motion_callback, this);

         points.clear();
         if (mpath) {
            double display_scale;
            vector->getDisplayScale(display_scale);
            if ((mpath->trace(call, display_scale, false) != ERR::Okay) or (points.size() < 2)) return;
         }
         else if ((path->trace(call, 1.0, false) != ERR::Okay) or (points.size() < 2)) return;

         vector->getPathTimestamp(path_timestamp);
         total_dist = 0;
         distances.clear();

         if ((auto_rotate IS ART::AUTO) or (auto_rotate IS ART::AUTO_REVERSE)) {
            precalc_angles();
         }
      }

      if (calc_mode IS CMODE::PACED) {
         const auto dist = get_total_dist();
         const auto dist_pos = seek * dist;

         // Use the distances array to determine the correct index.

         int i;
         for (i=0; (i < std::ssize(distances)-1) and (distances[i+1] < dist_pos); i++);

         a = points[i];
         b = points[i+1];

         seek_to = interval_seek(dist_pos, distances[i], distances[i+1]);

         if ((auto_rotate IS ART::AUTO) or (auto_rotate IS ART::AUTO_REVERSE)) {
            angle = (angles[i] * (1.0 - seek_to)) + (angles[i+1] * seek_to);
            if (auto_rotate IS ART::AUTO_REVERSE) angle += 180.0;
         }
      }
      else { // CMODE::LINEAR: Interpolate between the two values
         int i = int((std::ssize(points)-1) * seek);
         if (i >= std::ssize(points)-1) i = std::ssize(points) - 2;

         a = points[i];
         b = points[i+1];

         seek_to = mod_seek(seek, points.size());

         if ((auto_rotate IS ART::AUTO) or (auto_rotate IS ART::AUTO_REVERSE)) {
            angle = (angles[i] * (1.0 - seek_to)) + (angles[i+1] * seek_to);
            if (auto_rotate IS ART::AUTO_REVERSE) angle += 180.0;
         }
      }
   }
   else if (not values.empty()) {
      // Values are x,y coordinate pairs.

      int i;
      if (calc_mode IS CMODE::PACED) {
         const auto dist_pos = seek * get_total_dist();
         for (i=0; (i < std::ssize(distances)-1) and (distances[i+1] < dist_pos); i++);
         seek_to = interval_seek(dist_pos, distances[i], distances[i+1]);
      }
      else if (calc_mode IS CMODE::SPLINE) {
         if (spline_paths.empty()) return;

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

         seek_to = spline_seek(sp, x);
      }
      else { // CMODE::LINEAR: Interpolate between the two values
         i = int((std::ssize(values)-1) * seek);
         if (i >= std::ssize(values)-1) i = values.size() - 2;
         seek_to = mod_seek(seek, values.size());
      }

      read_numseq(values[i], { &a.x, &a.y });
      read_numseq(values[i+1], { &b.x, &b.y });
   }
   else if (not from.empty()) {
      if (not to.empty()) {
         read_numseq(from, { &a.x, &a.y });
         read_numseq(to, { &b.x, &b.y } );
      }
      else if (not by.empty()) {
         read_numseq(from, { &a.x, &a.y });
         read_numseq(by, { &b.x, &b.y } );
         b.x += a.x;
         b.y += a.y;
      }
      else return;
   }
   else return;

   // Note how the matrix is assigned to the end of the transform list so that it is executed last.  This is a
   // requirement of the SVG standard.  It is important that the matrix is managed independently and not
   // intermixed with other transforms.

   if (not matrix) {
      vector->newMatrix(&matrix, true);
      matrix->Tag = MTAG_ANIMATE_MOTION;
   }
   vec::ResetMatrix(matrix);

   if (angle != -1) vec::Rotate(matrix, angle, 0, 0);
   else if (auto_rotate IS ART::FIXED) vec::Rotate(matrix, rotate, 0, 0);

   if (calc_mode IS CMODE::DISCRETE) {
      if (seek_to < 0.5) vec::Translate(matrix, a.x, a.y);
      else vec::Translate(matrix, b.x, b.y);
   }
   else { // CMODE::LINEAR
      kt::POINT<double> final { a.x + ((b.x - a.x) * seek_to), a.y + ((b.y - a.y) * seek_to) };
      vec::Translate(matrix, final.x, final.y);
   }
}
