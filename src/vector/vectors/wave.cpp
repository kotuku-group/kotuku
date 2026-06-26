/*********************************************************************************************************************

-CLASS-
VectorWave: Extends the Vector class with support for sine wave based paths.

The VectorWave class provides functionality for generating paths based on sine waves.  This feature is not part of the
SVG standard and therefore should not be used in cases where SVG compliance is a strict requirement.

The sine wave will be generated within a rectangular region at (#X,#Y) with size (#Width,#Height).  The horizontal
center-line within the rectangle will dictate the orientation of the sine wave, and the path vertices are generated
on a left-to-right basis.

Waves can be used in Kotuku's SVG implementation by using the &lt;kotuku:wave/&gt; element.

-END-

TODO: Add support for BUTT, ROUND, SQUARE caps when Thickness > 0

*********************************************************************************************************************/

static void generate_wave(class extVectorWave *Vector, agg::path_storage &Path);

class extVectorWave : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORWAVE;
   static constexpr CSTRING CLASS_NAME = "VectorWave";
   using create = kt::Create<extVectorWave>;

   Unit wX = Unit(0), wY = Unit(0);
   Unit wWidth = Unit(0), wHeight = Unit(0);
   double wAmplitude = 1.0;
   double wFrequency = 1.0;
   double wDecay     = 1.0;
   double wPhase     = 0;
   double wThickness = 0;
   WVC wClose        = WVC::NIL;
   WVE wEnvelope     = WVE::LINEAR;
   WVS wStyle        = WVS::SMOOTH;

   extVectorWave() {
      GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_wave;
   }
};

//********************************************************************************************************************

static double normalise_phase(double Phase)
{
   double phase = fmod(Phase, 360.0);
   if (phase < 0.0) phase += 360.0;
   return phase;
}

static double wave_triangle(double Phase)
{
   double phase = normalise_phase(Phase);

   if (phase < 90.0) return phase * (1.0 / 90.0);
   else if (phase < 270.0) return 1.0 - ((phase - 90.0) * (1.0 / 90.0));
   else return -1.0 + ((phase - 270.0) * (1.0 / 90.0));
}

static double wave_sawtooth(double Phase)
{
   return 1.0 - (normalise_phase(Phase) * (1.0 / 180.0));
}

static double wave_value(extVectorWave *Vector, double Phase)
{
   switch (Vector->wStyle) {
      case WVS::TRIANGLE: return wave_triangle(Phase);
      case WVS::SAWTOOTH: return wave_sawtooth(Phase);
      case WVS::SMOOTH:
      case WVS::NIL:
      default:            return sin(DEG2RAD * Phase);
   }
}

static double wave_phase_at_angle(extVectorWave *Vector, double Angle)
{
   return Vector->wPhase + (Angle * Vector->wFrequency);
}

static double wave_angle_at_phase(extVectorWave *Vector, double Phase)
{
   return (Phase - Vector->wPhase) / Vector->wFrequency;
}

static double decay_multiplier(extVectorWave *Vector, double Angle)
{
   double progress = Angle * (1.0 / 360.0);
   if (progress < 0.0) progress = 0.0;
   else if (progress > 1.0) progress = 1.0;

   switch (Vector->wEnvelope) {
      case WVE::QUADRATIC:  progress *= progress; break;
      case WVE::SMOOTHSTEP: progress = progress * progress * (3.0 - (2.0 * progress)); break;
      case WVE::EXPONENTIAL:
         progress = (exp(progress * 4.0) - 1.0) * (1.0 / (exp(4.0) - 1.0));
         break;
      default:
         break;
   }

   constexpr double MAX_DECAY = 100.0;
   double end_scale = std::abs(Vector->wDecay);
   if (end_scale > MAX_DECAY) end_scale = MAX_DECAY;

   if (Vector->wDecay < 0) return end_scale + ((1.0 - end_scale) * progress);
   else return 1.0 - ((1.0 - end_scale) * progress);
}

static double decayed_wave_value(extVectorWave *Vector, double Angle, double Phase, double Amplitude, double Height)
{
   return (wave_value(Vector, Phase) * Amplitude * decay_multiplier(Vector, Angle)) + (Height * 0.5);
}

static void add_wave_vertex(extVectorWave *Vector, agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude, double Height, double Angle, double Phase)
{
   double x = Angle * XScale;
   double y = decayed_wave_value(Vector, Angle, Phase, Amplitude, Height);
   if (Vector->Transition) apply_transition_xy(Vector->Transition, Angle * (1.0 / 360.0), &x, &y);
   Path.line_to(OriginX + x, OriginY + y);
}

static void add_triangle_vertices(extVectorWave *Vector, agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude, double Height)
{
   const double start_phase = Vector->wPhase;
   const double end_phase = wave_phase_at_angle(Vector, 360.0);
   const double first_cycle = floor((start_phase - 270.0) * (1.0 / 360.0)) * 360.0;

   for (double cycle=first_cycle; cycle < end_phase + 360.0; cycle += 360.0) {
      double peak_phase = cycle + 90.0;
      double peak_angle = wave_angle_at_phase(Vector, peak_phase);
      if ((peak_angle > 0.0) and (peak_angle < 360.0)) {
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, Height, peak_angle, peak_phase);
      }

      double trough_phase = cycle + 270.0;
      double trough_angle = wave_angle_at_phase(Vector, trough_phase);
      if ((trough_angle > 0.0) and (trough_angle < 360.0)) {
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, Height, trough_angle, trough_phase);
      }
   }
}

static void add_sawtooth_vertices(extVectorWave *Vector, agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude, double Height)
{
   const double start_phase = Vector->wPhase;
   const double end_phase = wave_phase_at_angle(Vector, 360.0);
   const double first_cycle = floor(start_phase * (1.0 / 360.0)) * 360.0;

   for (double cycle=first_cycle; cycle < end_phase + 360.0; cycle += 360.0) {
      double angle = wave_angle_at_phase(Vector, cycle);
      if ((angle > 0.0) and (angle < 360.0)) {
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, Height, angle, cycle - 0.000001);
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, Height, angle, cycle);
      }
   }
}

struct wave_point {
   double x;
   double y;
};

static bool get_unit_direction(const wave_point &Start, const wave_point &End, double &X, double &Y)
{
   double dx = End.x - Start.x;
   double dy = End.y - Start.y;
   double length = sqrt((dx * dx) + (dy * dy));
   if (length <= 0.00000001) return false;

   X = dx / length;
   Y = dy / length;
   return true;
}

static wave_point wave_normal(double Dx, double Dy)
{
   return { -Dy, Dx };
}

static wave_point wave_offset_at_vertex(const std::vector<wave_point> &Points, int Index, double HalfThickness)
{
   int total = int(Points.size());
   double prev_x = 0, prev_y = 0, next_x = 0, next_y = 0;
   bool have_prev = (Index > 0) and get_unit_direction(Points[Index - 1], Points[Index], prev_x, prev_y);
   bool have_next = (Index < total - 1) and get_unit_direction(Points[Index], Points[Index + 1], next_x, next_y);

   if (have_prev and have_next) {
      auto prev_normal = wave_normal(prev_x, prev_y);
      auto next_normal = wave_normal(next_x, next_y);
      double miter_x = prev_normal.x + next_normal.x;
      double miter_y = prev_normal.y + next_normal.y;
      double miter_length = sqrt((miter_x * miter_x) + (miter_y * miter_y));

      if (miter_length > 0.00000001) {
         miter_x /= miter_length;
         miter_y /= miter_length;

         double dot = (miter_x * next_normal.x) + (miter_y * next_normal.y);
         if (std::abs(dot) > 0.1) {
            double distance = HalfThickness / std::abs(dot);
            double miter_limit = HalfThickness * 4.0;
            if (distance > miter_limit) distance = miter_limit;
            return { miter_x * distance, miter_y * distance };
         }
      }

      return { next_normal.x * HalfThickness, next_normal.y * HalfThickness };
   }
   else if (have_next) {
      auto normal = wave_normal(next_x, next_y);
      return { normal.x * HalfThickness, normal.y * HalfThickness };
   }
   else if (have_prev) {
      auto normal = wave_normal(prev_x, prev_y);
      return { normal.x * HalfThickness, normal.y * HalfThickness };
   }
   else return { 0, 0 };
}

static void apply_wave_thickness(agg::path_storage &Path, double Thickness)
{
   std::vector<wave_point> points;
   int total = Path.total_vertices();
   points.reserve(total);

   for (int i=0; i < total; i++) {
      double x, y;
      unsigned cmd = Path.vertex(i, &x, &y);
      if (agg::is_vertex(cmd)) {
         if (points.empty()) points.push_back({ x, y });
         else {
            auto &last = points.back();
            double dx = x - last.x;
            double dy = y - last.y;
            if (((dx * dx) + (dy * dy)) > 0.000000000001) points.push_back({ x, y });
         }
      }
   }

   total = int(points.size());
   if (total < 2) return;

   std::vector<wave_point> offsets;
   offsets.reserve(total);

   double half_thickness = Thickness * 0.5;
   for (int i=0; i < total; i++) {
      offsets.push_back(wave_offset_at_vertex(points, i, half_thickness));
   }

   Path.remove_all();
   Path.move_to(points[0].x + offsets[0].x, points[0].y + offsets[0].y);
   for (int i=1; i < total; i++) {
      Path.line_to(points[i].x + offsets[i].x, points[i].y + offsets[i].y);
   }

   for (int i=total - 1; i >= 0; i--) {
      Path.line_to(points[i].x - offsets[i].x, points[i].y - offsets[i].y);
   }
}

//********************************************************************************************************************

static void generate_wave(extVectorWave *Vector, agg::path_storage &Path)
{
   Unit ox = Vector->wX;
   Unit oy = Vector->wY;
   Unit width = Vector->wWidth;
   Unit height = Vector->wHeight;

   if (Vector->wX.scaled() or Vector->wY.scaled() or Vector->wWidth.scaled() or Vector->wHeight.scaled()) {
      auto view_width = get_parent_width(Vector);
      auto view_height = get_parent_height(Vector);

      if (Vector->wX.scaled()) ox = Unit(ox * view_width);
      if (Vector->wY.scaled()) oy = Unit(oy * view_height);
      if (Vector->wWidth.scaled()) width = Unit(width * view_width);
      if (Vector->wHeight.scaled()) height = Unit(height * view_height);
   }

   const double amp = (height * 0.5) * Vector->wAmplitude;
   const double scale = 1.0 / Vector->Transform.scale(); // Essential for smooth curves when scale > 1.0

   double x = 0, y = decayed_wave_value(Vector, 0, Vector->wPhase, amp, height);
   if (Vector->Transition) apply_transition_xy(Vector->Transition, 0, &x, &y);

   if ((Vector->wClose IS WVC::NIL) or (Vector->wThickness > 0)) {
      Path.move_to(ox + x, oy + y);
   }
   else if (Vector->wClose IS WVC::TOP) {
      Path.move_to(ox + width, oy); // Top right
      Path.line_to(ox, oy); // Top left
      Path.line_to(ox + x, oy + y);
   }
   else if (Vector->wClose IS WVC::BOTTOM) {
      Path.move_to(ox + width, oy + height); // Bottom right
      Path.line_to(ox, oy + height); // Bottom left
      Path.line_to(ox + x, oy + y);
   }
   else return;

   // Wave generator.  This applies scaling so that the correct number of vertices are generated.  Also, the
   // last vertex is interpolated to end exactly at 360, ensuring that the path terminates accurately.

   double xscale = width * (1.0 / 360.0);
   double angle;
   double last_x = x, last_y = y;
   if ((Vector->wStyle IS WVS::TRIANGLE) or (Vector->wStyle IS WVS::SAWTOOTH)) {
      if (Vector->wStyle IS WVS::TRIANGLE) {
         add_triangle_vertices(Vector, Path, ox, oy, xscale, amp, height);
      }
      else if (Vector->wStyle IS WVS::SAWTOOTH) {
         add_sawtooth_vertices(Vector, Path, ox, oy, xscale, amp, height);
      }

      double end_phase = wave_phase_at_angle(Vector, 360.0);
      if ((Vector->wStyle IS WVS::SAWTOOTH) and (std::abs(normalise_phase(end_phase)) < 0.000001)) {
         end_phase -= 0.000001;
      }
      add_wave_vertex(Vector, Path, ox, oy, xscale, amp, height, 360.0, end_phase);
   }
   else {
      constexpr double MAX_PHASE_STEP = 12.0;
      double angle_step = scale;
      double phase_limited_step = MAX_PHASE_STEP * (1.0 / Vector->wFrequency);
      double vertex_tolerance = 0.5 * scale;
      if (phase_limited_step < angle_step) angle_step = phase_limited_step;
      if (angle_step <= 0.0) angle_step = 1.0;

      for (angle=angle_step; angle < 360; angle += angle_step) {
         double phase = wave_phase_at_angle(Vector, angle);
         double x = angle * xscale;
         double y = decayed_wave_value(Vector, angle, phase, amp, height);
         if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
         if ((std::abs(x - last_x) >= vertex_tolerance) or (std::abs(y - last_y) >= vertex_tolerance)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      double phase = wave_phase_at_angle(Vector, 360.0);
      double x = width;
      double y = decayed_wave_value(Vector, 360.0, phase, amp, height);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, 1.0, &x, &y);
      Path.line_to(ox + x, oy + y);
   }

   if (Vector->wThickness > 0) {
      apply_wave_thickness(Path, Vector->wThickness);
   }

   if ((Vector->wClose != WVC::NIL) or (Vector->wThickness > 0)) Path.close_polygon();

   Vector->Bounds = get_bounds(Path);
}

/*********************************************************************************************************************
-ACTION-
Move: Moves the vector to a new position.
-END-
*********************************************************************************************************************/

static ERR VECTORWAVE_Move(extVectorWave *Self, struct acMove *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Self->wX.scaled()) Self->wX = Unit((Self->wX * get_parent_width(Self)) + Args->DeltaX);
   else Self->wX = Unit(Self->wX + Args->DeltaX);

   if (Self->wY.scaled()) Self->wY = Unit((Self->wY * get_parent_height(Self)) + Args->DeltaY);
   else Self->wY = Unit(Self->wY + Args->DeltaY);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves the vector to a new fixed position.
-END-
*********************************************************************************************************************/

static ERR VECTORWAVE_MoveToPoint(extVectorWave *Self, struct acMoveToPoint *Args)
{
   if (not Args) return ERR::NullArgs;

   if ((Args->Flags & MTF::RELATIVE) != MTF::NIL) {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->wX = Unit(Args->X, FD_SCALED);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->wY = Unit(Args->Y, FD_SCALED);
   }
   else {
      if ((Args->Flags & MTF::X) != MTF::NIL) Self->wX = Unit(Args->X);
      if ((Args->Flags & MTF::Y) != MTF::NIL) Self->wY = Unit(Args->Y);
   }
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Changes the vector's area.
-END-
*********************************************************************************************************************/

static ERR VECTORWAVE_Resize(extVectorWave *Self, struct acResize *Args)
{
   if (not Args) return ERR::NullArgs;

   Self->wWidth = Unit(Args->Width);
   Self->wHeight = Unit(Args->Height);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Amplitude: Adjusts the generated wave amplitude.

The Amplitude is expressed as a multiplier that adjusts the wave amplitude (i.e. height).  A value of 1.0 is the
default.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Amplitude(extVectorWave *Self, double Value)
{
   if (Value > 0.0) {
      Self->wAmplitude = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Close: Closes the generated wave path at either the top or bottom.

Setting the Close field to `TOP` or `BOTTOM` will close the generated wave's path so that it is suitable for being
filled.  This setting is not compatible with the #Thickness option.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Close(extVectorWave *Self, WVC Value)
{
   Self->wClose = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Decay: Declares a rate of decay to apply to the wave amplitude.

The amplitude of a wave can be decayed between its start and end points by setting the Decay field.  A value of `1.0`
is the default and applies no decay.  Values between `0.0` and `1.0` set the amplitude multiplier at the end of the
wave, so `0.5` tapers the wave to half amplitude and `0.999` remains visually close to no decay.  If the value is
negative, the start and end points for the decay will be reversed.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Decay(extVectorWave *Self, double Value)
{
   Self->wDecay = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Envelope: Selects the decay envelope applied to the wave amplitude.

The Envelope field controls how the #Decay value is interpolated across the wave.  The default `LINEAR` envelope uses
an even taper, while `QUADRATIC`, `SMOOTHSTEP` and `EXPONENTIAL` apply progressively stronger curves to the decay.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Envelope(extVectorWave *Self, WVE Value)
{
   Self->wEnvelope = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Frequency: Defines the wave frequency (the distance between each wave).

The frequency determines the distance between each individual wave that is generated.  The default
value for the frequency is 1.0.  Shortening the frequency to a value closer to 0 will bring the waves closer together.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Frequency(extVectorWave *Self, double Value)
{
   if (Value > 0.0) {
      Self->wFrequency = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Height: The height of the area containing the wave.

The height of the area containing the wave is defined here as a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Height(extVectorWave *Self, Unit &Value)
{
   Self->wHeight = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Phase: Declares the initial phase, in degrees, to use when generating the wave.

The phase value defines the initial position that is used when computing the wave.  The default is zero.

Visually, changing the phase will affect the 'offset' of the generated wave.  Gradually incrementing the value
will give the wave an appearance of moving from right to left.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Phase(extVectorWave *Self, double Value)
{
   Self->wPhase = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Style: Selects an alternative wave style.

By default, waves are generated in the style of a sine wave.  Alternative styles can be selected by setting this field.
The `SMOOTH` style generates a sine wave, `TRIANGLE` generates a triangular wave and `SAWTOOTH` generates a ramp with a
sharp return edge.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Style(extVectorWave *Self, WVS Value)
{
   Self->wStyle = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Thickness: Expands the diameter of the wave to the specified value to produce a closed path.

Specifying a thickness value will create a wave that forms a filled shape, rather than the default of a stroked path.
The thickness (diameter) of the wave is determined by the provided value.  Thickness is not compatible with the #Close
option, and takes precedence over it.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Thickness(extVectorWave *Self, double Value)
{
   Self->wThickness = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Width: The width of the area containing the wave.

The width of the area containing the wave is defined here as a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Width(extVectorWave *Self, Unit &Value)
{
   Self->wWidth = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X: The x coordinate of the wave.  Can be expressed as a fixed or scaled coordinate.

The x coordinate of the wave is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_X(extVectorWave *Self, Unit &Value)
{
   Self->wX = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y: The y coordinate of the wave.  Can be expressed as a fixed or scaled coordinate.

The y coordinate of the wave is defined here as either a fixed or scaled value.
-END-
*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Y(extVectorWave *Self, Unit &Value)
{
   Self->wY = Value;
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "wave_def.cpp"

static const FieldArray clVectorWaveFields[] = {
   { "X",          FDF_UNIT|FDF_RW, nullptr, VECTORWAVE_SET_X },
   { "Y",          FDF_UNIT|FDF_RW, nullptr, VECTORWAVE_SET_Y },
   { "Width",      FDF_UNIT|FDF_RW, nullptr, VECTORWAVE_SET_Width },
   { "Height",     FDF_UNIT|FDF_RW, nullptr, VECTORWAVE_SET_Height },
   { "Amplitude",  FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Amplitude },
   { "Frequency",  FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Frequency },
   { "Decay",      FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Decay },
   { "Envelope",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORWAVE_SET_Envelope, &clVectorWaveWVE },
   { "Phase",      FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Phase },
   { "Thickness",  FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Thickness },
   { "Close",      FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORWAVE_SET_Close, &clVectorWaveWVC },
   { "Style",      FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORWAVE_SET_Style, &clVectorWaveWVS },
   END_FIELD
};

static ERR init_wave(void)
{
   clVectorWave = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORWAVE),
      fl::Name("VectorWave"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorWaveActions),
      fl::Fields(clVectorWaveFields),
      fl::Size(sizeof(extVectorWave)),
      fl::Path(MOD_PATH));

   return clVectorWave ? ERR::Okay : ERR::AddClass;
}
