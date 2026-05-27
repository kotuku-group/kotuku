
static ERR parse_spline(APTR Path, int Index, int Command, double X, double Y, anim_base::SPLINE_POINTS &Meta)
{
   Meta.emplace_back(kt::POINT<float> { float(X), float(Y) }, 0);

   if (Meta.size() > 1) {
      Meta[Meta.size()-2].angle = std::atan2(Meta.back().point.y - Meta[Meta.size()-2].point.y, Meta.back().point.x - Meta[Meta.size()-2].point.x);
      Meta[Meta.size()-2].cos_angle = std::cos(Meta[Meta.size()-2].angle);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static double parse_begin(anim_base &Anim, std::string_view Value)
{
   if (Value.ends_with(".begin")) {
      auto ref_id = strhash(std::string(Value.substr(0, Value.size()-6)));
      Anim.svg->StartOnBegin[ref_id].push_back(&Anim);
      return std::numeric_limits<double>::max();
   }
   else if (Value.ends_with(".end")) {
      auto ref_id = strhash(std::string(Value.substr(0, Value.size()-4)));
      Anim.svg->StartOnEnd[ref_id].push_back(&Anim);
      return std::numeric_limits<double>::max();
   }
   else if ("access-key" IS Value) { // Start the animation when the user presses a key.
      Anim.begin_on_key = true;
      return std::numeric_limits<double>::max();
   }
   else return read_time(Value);
}

//********************************************************************************************************************

static void remove_animation_triggers(anim_base &Anim)
{
   auto remove_anim = [&Anim](auto &Map) {
      for (auto it = Map.begin(); it != Map.end(); ) {
         auto &list = it->second;
         list.erase(std::remove(list.begin(), list.end(), &Anim), list.end());
         if (list.empty()) it = Map.erase(it);
         else ++it;
      }
   };

   remove_anim(Anim.svg->StartOnBegin);
   remove_anim(Anim.svg->StartOnEnd);
}

//********************************************************************************************************************
// Set common animation properties

static ERR set_anim_property(anim_base &Anim, XTag &Tag, uint32_t Hash, const std::string_view Value)
{
   switch (Hash) {
      case SVF_id:
         Anim.id = Value;
         break;

      case SVF_href:
      case SVF_xlink_href: {
         OBJECTPTR ref_vector;
         auto ref = uri_name(std::string(Value));
         if (Anim.svg->Scene->findDef(ref.c_str(), &ref_vector) IS ERR::Okay) {
            Anim.target_vector = ref_vector->UID;
         }
         break;
      }

      case SVF_attributeName: // Name of the target attribute affected by the From and To values.
         Anim.target_attrib = Value;
         break;

      case SVF_attributeType: // Namespace of the target attribute: XML, CSS, auto
         if ("XML" IS Value) Anim.attrib_type = ATT::XML;
         else if ("CSS" IS Value) Anim.attrib_type = ATT::CSS;
         else if ("auto" IS Value) Anim.attrib_type = ATT::AUTO;
         break;

      case SVF_fill: // freeze, remove
         if ("freeze" IS Value) Anim.freeze = true; // Freeze the effect value at the last value of the duration (i.e. keep the last frame).
         else if ("remove" IS Value) Anim.freeze = true; // The default.  The effect is stopped when the duration is over.
         break;

      case SVF_additive: // replace, sum
         if ("replace" IS Value) Anim.additive = ADD::REPLACE; // The animation values replace the underlying values of the target vector's attributes.
         else if ("sum" IS Value) Anim.additive = ADD::SUM; // The animation adds to the underlying values of the target vector.
         break;

      case SVF_accumulate:
         if ("none" IS Value) Anim.accumulate = false; // Repeat iterations are not cumulative.  This is the default.
         else if ("sum" IS Value) Anim.accumulate = true; // Each repeated iteration builds on the last value of the previous iteration.
         break;

      case SVF_from: // The starting value of the animation.
         Anim.from = Value;
         break;

      // It is not legal to specify both 'by' and 'to' attributes - if both are specified, only the to attribute will
      // be used (the by will be ignored).

      case SVF_to: // Specifies the ending value of the animation.
         Anim.to = Value;
         break;

      case SVF_by: // Specifies a relative offset value for the animation.
         Anim.by = Value;
         break;

      case SVF_begin:
         // Defines when the element should become active.  Specified as a semi-colon list.
         //   offset: A clock-value that is offset from the moment the animation is activated.
         //   id.end/begin: Reference to another animation's begin or end to determine when the animation starts.
         //   event: An event reference like 'focusin' determines that the animation starts when the event is triggered.
         //   id.repeat(value): Reference to another animation, repeat when the given value is reached.
         //   access-key: The animation starts when a keyboard key is pressed.
         //   clock: A real-world clock time (not supported)

         if ("indefinite" IS Value) {
            Anim.begin_offset = std::numeric_limits<double>::max();
         }
         else if (Value.find(';') != std::string::npos) {
            // A series of numbers has likely been provided, but '.begin' and '.end' references are possible also.

            for (unsigned v=0; v < Value.size(); ) {
                while (v < std::ssize(Value) and (Value[v] <= 0x20)) v++;
                auto v_end = Value.find(';', v);
                if (v_end IS std::string::npos) v_end = Value.size();

                if (auto begin_offset = parse_begin(Anim, Value.substr(v, v_end - v)); begin_offset != std::numeric_limits<double>::max()) {
                   Anim.begin_series.push_back(begin_offset);
                }
                v = v_end + 1;
            }

            if (not Anim.begin_series.empty()) Anim.begin_offset = Anim.begin_series[0];
            else Anim.begin_offset = std::numeric_limits<double>::max();
         }
         else Anim.begin_offset = parse_begin(Anim, Value);

         break;

      case SVF_end:
         // The animation ends when one of the triggers is reached.  A semi-colon list of multiple values is permitted
         // and documented as the 'end-value-list'.  End is paired with 'begin' and should be parsed in the same way.
         break;

      case SVF_dur: // 4s, 02:33, 12:10:53, 45min, 4ms, 12.93, 1h, 'media', 'indefinite'
         if ("media" IS Value) Anim.duration = 0; // Does not apply to animation
         else if ("indefinite" IS Value) Anim.duration = -1;
         else Anim.duration = read_time(Value);
         break;

      case SVF_min: // Specifies the minimum value of the active duration.
         if ("media" IS Value) Anim.min_duration = 0; // Does not apply to animation
         else Anim.min_duration = read_time(Value);
         break;

      case SVF_max: // Specifies the maximum value of the active duration.
         if ("media" IS Value) Anim.max_duration = 0; // Does not apply to animation
         else Anim.max_duration = read_time(Value);
         break;

      case SVF_calcMode: // Specifies the interpolation mode for the animation.
         if ("discrete" IS Value)    Anim.calc_mode = CMODE::DISCRETE;
         else if ("linear" IS Value) Anim.calc_mode = CMODE::LINEAR;
         else if ("paced" IS Value)  Anim.calc_mode = CMODE::PACED;
         else if ("spline" IS Value) Anim.calc_mode = CMODE::SPLINE;
         break;

      case SVF_restart: // always, whenNotActive, never
         if ("always" IS Value) Anim.restart = RST::ALWAYS;
         else if ("whenNotActive" IS Value) Anim.restart = RST::WHEN_NOT_ACTIVE;
         else if ("never" IS Value) Anim.restart = RST::NEVER;
         break;

      case SVF_repeatDur: // Specifies the total duration for repeat.
         if ("indefinite" IS Value) Anim.repeat_duration = -1;
         else Anim.repeat_duration = read_time(Value);
         break;

      case SVF_repeatCount: // Specifies the number of iterations of the animation function.  Integer, 'indefinite'
         if ("indefinite" IS Value) Anim.repeat_count = -1;
         else Anim.repeat_count = read_time(Value);
         break;

      // Similar to 'from' and 'to', this is a series of values that are interpolated over the time line.
      // If a list of values is specified, any from, to and by attribute values are ignored.

      case SVF_values: {
         Anim.values.clear();
         int s, v = 0;
         while (v < std::ssize(Value)) {
            while ((v < std::ssize(Value)) and (Value[v] <= 0x20)) v++;
            for (s=v; (s < std::ssize(Value)) and (Value[s] != ';'); s++);
            Anim.values.push_back(std::string(Value.substr(v, s-v)));
            v = s;
            if ((v < std::ssize(Value)) and (Value[v] IS ';')) v++;
         }
         break;
      }

      // Takes a semicolon-separated list of floating point values between 0 and 1 and indicates how far along
      // the motion path the object shall move at the moment in time specified by corresponding 'keyTimes'
      // value. Distance calculations use the user agent's distance along the path algorithm. Each progress
      // value in the list corresponds to a value in the 'keyTimes' attribute list.

      case SVF_keyPoints: {
         Anim.key_points.clear();
         int s, v = 0;
         while (v < std::ssize(Value)) {
            while (v < std::ssize(Value) and (Value[v] <= 0x20)) v++;
            for (s=v; s < std::ssize(Value) and (Value[s] != ';'); s++);
            std::string_view val = Value.substr(v, s-v);
            double fv = 0;
            auto [ ptr, error ] = std::from_chars(val.data(), val.data() + val.size(), fv);
            if ((error IS std::errc()) and (ptr IS val.data() + val.size())) {
               fv = std::clamp(fv, 0.0, 1.0);
               Anim.key_points.push_back(fv);
            }
            v = s;
            if ((v < std::ssize(Value)) and (Value[v] IS ';')) v++;
         }
         break;
      }

      // A semicolon-separated list of time values used to control the pacing of the animation. Each time in the
      // list corresponds to a value in the 'values' attribute list, and defines when the value is used in the
      // animation function. Each time value in the 'keyTimes' list is specified as a floating point value between
      // 0 and 1 (inclusive), representing a proportional offset into the simple duration of the animation
      // element.
      //
      // For animations specified with a 'values' list, the 'keyTimes' attribute if specified must have exactly as
      // many values as there are in the 'values' attribute. For from/to/by animations, the 'keyTimes' attribute
      // if specified must have two values.
      //
      // Each successive time value must be greater than or equal to the preceding time value.

      case SVF_keyTimes: {
         Anim.timing.clear();
         int s, v = 0;
         double last_val = 0.0;
         while (v < std::ssize(Value)) {
            while (v < std::ssize(Value) and (Value[v] <= 0x20)) v++;
            for (s=v; s < std::ssize(Value) and (Value[s] != ';'); s++);
            std::string_view val = Value.substr(v, s-v);
            double fv = 0;
            auto [ ptr, error ] = std::from_chars(val.data(), val.data() + val.size(), fv);
            if ((error IS std::errc()) and (ptr IS val.data() + val.size())) {
               fv = std::clamp(fv, last_val, 1.0);
               Anim.timing.push_back(fv);
               last_val = fv;
            }
            v = s;
            if ((v < std::ssize(Value)) and (Value[v] IS ';')) v++;
         }
         break;
      }

      // A set of Bezier control points associated with the 'keyTimes' list, defining a cubic Bezier function
      // that controls interval pacing. The attribute value is a semicolon-separated list of control point
      // descriptions. Each control point description is a set of four values: x1 y1 x2 y2, describing the
      // Bezier control points for one time segment. Note: SMIL allows these values to be separated either by
      // commas with optional whitespace, or by whitespace alone. The 'keyTimes' values that define the
      // associated segment are the Bezier "anchor points", and the 'keySplines' values are the control points.
      // Thus, there must be one fewer sets of control points than there are 'keyTimes'.
      //
      // The values must all be in the range 0 to 1.
      // This attribute is ignored unless the 'calcMode' is set to 'spline'.
      // Parsing errors must be propagated.

      case SVF_keySplines: {
         Anim.splines.clear();
         int s, v = 0;
         while (v < std::ssize(Value)) {
            while (v < std::ssize(Value) and (Value[v] <= 0x20)) v++;
            for (s=v; s < std::ssize(Value) and (Value[s] != ';'); s++);
            auto quad = std::string_view(Value.substr(v, s-v));

            POINT<double> a, b;
            read_numseq(quad, { &a.x, &a.y, &b.x, &b.y });
            a.x = std::clamp(a.x, 0.0, 1.0);
            a.y = std::clamp(a.y, 0.0, 1.0);
            b.x = std::clamp(b.x, 0.0, 1.0);
            b.y = std::clamp(b.y, 0.0, 1.0);
            Anim.splines.push_back(std::make_pair(a, b));

            v = s;
            if ((v < std::ssize(Value)) and (Value[v] IS ';')) v++;
         }

         if (Anim.splines.size() < 2) Anim.splines.clear();
         else {
            // Convert the splines into bezier paths and generate a point-based path in advance.
            for (auto &sp : Anim.splines) {
               APTR path;
               if (vec::GeneratePath("", &path) IS ERR::Okay) {
                  anim_base::SPLINE_POINTS lookup;
                  vec::MoveTo(path, 0, 0);
                  vec::Curve4(path, sp.first.x, sp.first.y, sp.second.x, sp.second.y, 1.0, 1.0);
                  auto func = C_FUNCTION(parse_spline, &lookup);
                  vec::TracePath(path, &func, 512.0);
                  Anim.spline_paths.push_back(lookup);
                  FreeResource(path);
               }
            }
         }
         break;
      }

      case SVF_externalResourcesRequired:
         // Deprecated
         break;
   }

   return ERR::Okay;
}
