// Relevant SVG materials:
// https://www.w3.org/TR/SVG11/animate.html#ToAttribute
// https://www.w3.org/TR/2001/REC-smil-animation-20010904

#include <cmath>
#include <string>

static const double DEG2RAD = 0.01745329251994329576923690768489;  // Multiple any angle by this value to convert to radians
static const double RAD2DEG = 57.295779513082320876798154814105;
static const int MAX_VALUES = 8;

//********************************************************************************************************************

enum class AT : char { // Transform Type
   NIL = 0, TRANSLATE = 1, SCALE, ROTATE, SKEW_X, SKEW_Y
};

enum class ADD : char { // Additive
   SUM = 0, REPLACE
};

enum class ATT : char { // Attribute Type
   AUTO = 0, CSS, XML
};

enum class ART: char {
   NIL = 0,
   AUTO,
   AUTO_REVERSE,
   FIXED
};

enum class CMODE : char { // Specifies the interpolation mode for the animation.
   LINEAR = 0, // Simple linear interpolation between values is used to calculate the animation function.
   DISCRETE,   // The animation function will jump from one value to the next without any interpolation.
   PACED,      // Defines interpolation to produce an even pace of change across the animation. This is only supported for values that define a linear numeric range, and for which some notion of "distance" between points can be calculated (e.g. position, width, height, etc.).  Any keyTimes or keySplines will be ignored.
   SPLINE      // Interpolates from one value in the values list to the next according to a time function defined by a cubic Bezier spline. The points of the spline are defined in the keyTimes attribute, and the control points for each interval are defined in the keySplines attribute.
};

enum class RST : char { // Restart
   ALWAYS = 0,
   WHEN_NOT_ACTIVE,
   NEVER
};

//********************************************************************************************************************

struct ROTATE {
   double angle = 0, cx = 0, cy = 0;

   constexpr ROTATE & operator += (const ROTATE &Other) {
      angle += Other.angle;
      cx += Other.cx;
      cy += Other.cy;
      return *this;
   }

   constexpr ROTATE & operator += (const double &Angle) {
      angle += Angle;
      return *this;
   }

   constexpr ROTATE & operator *= (const double &Angle) {
      angle *= Angle;
      return *this;
   }
};

constexpr ROTATE operator * (const ROTATE &lhs, const double &Num) {
   return ROTATE { lhs.angle * Num, lhs.cx, lhs.cy };
}

//********************************************************************************************************************

template <class T = double> double dist(const pf::POINT<T> &A, const pf::POINT<T> &B)
{
   if (A == B) return 0;
   double a = std::abs(B.x - A.x);
   double b = std::abs(B.y - A.y);
   if (a > b) std::swap(a, b);
   return b + 0.428 * a * a / b; // Error level of ~1.04%
   //return std::sqrt((a * a) + (b * b)); // Full accuracy
}

//********************************************************************************************************************

class anim_base {
private:
   uint32_t _hash_id = 0;

public:
   struct spline_point {
      pf::POINT<float> point;
      float angle;
      float cos_angle;
      spline_point(pf::POINT<float> pPoint, float pAngle) : point(pPoint), angle(pAngle) { }
   };

   using DISTANCES = std::vector<float>;
   using SPLINE_POINTS = std::vector<spline_point>;

   class spline_path {
   public:
      SPLINE_POINTS points;
      spline_path(SPLINE_POINTS pPoints) : points(pPoints) { }
   };

   // Variables

   extSVG *svg;
   std::vector<std::string> values; // Set of discrete values that override 'from', 'to', 'by'
   std::vector<double> timing;      // Key times.  Ignored if duration < 0
   std::vector<double> key_points;  // Key points
   DISTANCES distances;             // Maps directly to 'points' or 'values' for paced calculations
   std::string from;                // Start from this value. Ignored if 'values' is defined.
   std::string to, by;              // Note that 'to' and 'by' are mutually exclusive, with 'to' as the preference.
   std::string target_attrib;       // Name of the target attribute affected by the From and To values.
   std::string target_attrib_orig;  // Original value of the target attribute (if not freezing)
   std::string id;                  // Identifier for the animation
   std::vector< std::pair<pf::POINT<double>, pf::POINT<double> > > splines; // Key splines
   std::vector<spline_path> spline_paths;
   std::vector<double> begin_series; // List of valid start times for the animation
   double begin_offset = 0;    // Start animating after this much time (in seconds) has elapsed.
   double repeat_duration = 0; // The animation will be allowed to repeat for up to the number of seconds indicated.  The time includes the initial loop.
   double min_duration = 0;    // The minimum value of the active duration.  If zero, the active duration is not constrained.
   double max_duration = 0;    // The maximum value of the active duration.
   double duration   = 0;      // Measured in seconds, anything < 0 means infinite.
   double start_time = 0;      // This is time-stamped once the animation has started (the first begin event is hit)
   double end_time   = 0;      // This is time-stamped once the animation has finished all of its cycles (including repetitions)
   double end  = 0;
   double seek = 0;            // Current seek position, between 0 - 1.0
   double total_dist = 0;      // Total distance between all value nodes
   OBJECTID target_vector = 0;
   int   repeat_count = 0; // Repetition count.  Anything < 0 means infinite.
   int   repeat_index = 0; // Current index within the repeat cycle.
   CMODE  calc_mode    = CMODE::LINEAR;
   RST    restart      = RST::ALWAYS;
   ATT    attrib_type  = ATT::AUTO;
   ADD    additive     = ADD::REPLACE;
   bool   freeze       = false; // True if the animation freezes on the last frame
   bool   accumulate   = false;
   bool   begin_on_key = false; // Animation starts if the user hits a key
   bool   begin_on_click = false; // Animation starts if the user clicks anywhere in the scene graph

   // Functionality

   anim_base(extSVG *pSVG, OBJECTID pTarget) : svg(pSVG), target_vector(pTarget) { }

   double get_paired_dist();
   double get_total_dist();
   double get_dimension(objVector &, FIELD);
   double get_numeric_value(objVector &, FIELD);
   std::string get_string();
   FRGB get_colour_value(objVector &, FIELD);
   bool started(double);
   bool next_frame(double);
   void set_orig_value(svgState &);
   void activate(bool);
   void stop(double);

   uint32_t hash_id() {
      _hash_id = strihash(id);
      return _hash_id;
   }

   virtual void perform() = 0;

   virtual bool is_valid() {
      if (!values.empty()) return true;
      if ((!to.empty()) or (!by.empty())) return true;
      return false;
   }
};

//********************************************************************************************************************

class anim_transform : public anim_base {
public:
   VectorMatrix matrix = { .Vector = nullptr }; // Exclusive transform matrix for animation.
   AT type = AT::NIL;

   anim_transform(extSVG *pSVG, OBJECTID pTarget) : anim_base(pSVG, pTarget) { }

   void perform();

   std::string type_name() {
      switch (type) {
         case AT::TRANSLATE: return "translate";
         case AT::SCALE:     return "scale";
         case AT::ROTATE:    return "rotate";
         case AT::SKEW_X:    return "skewX";
         case AT::SKEW_Y:    return "skewY";
         default: return "?";
      }
   }
};

//********************************************************************************************************************

class anim_motion : public anim_base {
public:
   ART auto_rotate = ART::NIL; // Inline rotation along the path
   double rotate = 0; // Fixed angle rotation
   objVector *mpath = nullptr; // External vector path (untracked)
   VectorMatrix *matrix = nullptr;
   pf::GuardedObject<objVector> path; // Client provided path sequence
   std::vector<pf::POINT<float>> points;
   std::vector<float> angles; // Precalc'd angles for rotation along paths
   int path_timestamp = 0;

   anim_motion(extSVG *pSVG, OBJECTID pTarget) : anim_base(pSVG, pTarget) {
      calc_mode = CMODE::PACED;
   }

   void perform();
   void precalc_angles();
   double get_total_dist();

   bool is_valid() {
      if (!values.empty()) return true;
      if (path.id) return true;
      if (mpath) return true;
      if ((!to.empty()) or (!by.empty())) return true;
      return false;
   }
};

//********************************************************************************************************************

class anim_value : public anim_base {
public:
   XTag *tag = nullptr;

   anim_value(extSVG *pSVG, OBJECTID pTarget, XTag *pTag) : anim_base(pSVG, pTarget), tag(pTag) { }
   void perform();
   void set_value(objVector &Vector);
};

//********************************************************************************************************************

template <class T = float> double get_angle(pf::POINT<T> &A, pf::POINT<T> &B) {
   return std::atan2(B.y - A.y, B.x - A.x) * RAD2DEG;
}
