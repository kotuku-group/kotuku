/*********************************************************************************************************************

-CLASS-
VectorWave: Extends the Vector class with support for sine wave based paths.

The VectorWave class provides functionality for generating paths based on sine waves.  This feature is not part of the
SVG standard and therefore should not be used in cases where SVG compliance is a strict requirement.

The sine wave will be generated from (#X,#Y) across #Length.  The parent view height provides the vertical region for
centring and path closure, while #Amplitude defines the wave height.  The path vertices are generated on a
left-to-right basis.

Waves can be used in Kotuku's SVG implementation by using the &lt;kotuku:wave/&gt; element.

-END-

TODO: Allow the line cap and join styles to be configured when Thickness > 0 (currently fixed to ROUND).

*********************************************************************************************************************/

static void generate_wave(class extVectorWave *Vector, agg::path_storage &Path);

class extVectorWave : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORWAVE;
   static constexpr CSTRING CLASS_NAME = "VectorWave";
   using create = kt::Create<extVectorWave>;

   Unit wX = Unit(0), wY = Unit(0);
   Unit wLength = Unit(0), wAmplitude = Unit(0);
   Unit wThickness = Unit(0);
   double wFrequency = 1.0;
   double wDecay     = 1.0;
   double wPhase     = 0;
   WVE wEnvelope     = WVE::LINEAR;
   WVC wClose        = WVC::NIL;
   WVT wType         = WVT::SMOOTH;

   extVectorWave(objMetaClass *ClassPtr, OBJECTID ObjectID) : extVector(ClassPtr, ObjectID) {
      GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_wave;
   }
};

//********************************************************************************************************************

inline double normalise_phase(double Phase)
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

inline double wave_value(extVectorWave *Vector, double Phase)
{
   switch (Vector->wType) {
      case WVT::TRIANGLE: return wave_triangle(Phase);
      case WVT::SAWTOOTH: return wave_sawtooth(Phase);
      default:            return sin(DEG2RAD * Phase);
   }
}

inline double wave_phase_at_angle(extVectorWave *Vector, double Angle)
{
   return Vector->wPhase + (Angle * Vector->wFrequency);
}

inline double wave_angle_at_phase(extVectorWave *Vector, double Phase)
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
      default: // Linear
         break;
   }

   constexpr double MAX_DECAY = 100.0;
   double end_scale = std::abs(Vector->wDecay);
   if (end_scale > MAX_DECAY) end_scale = MAX_DECAY;

   if (Vector->wDecay < 0) return end_scale + ((1.0 - end_scale) * progress);
   else return 1.0 - ((1.0 - end_scale) * progress);
}

inline double decayed_wave_value(extVectorWave *Vector, double Angle, double Phase, double Amplitude)
{
   return (wave_value(Vector, Phase) * Amplitude * decay_multiplier(Vector, Angle));
}

inline void add_wave_vertex(extVectorWave *Vector, agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude, double Angle, double Phase)
{
   double x = Angle * XScale;
   double y = decayed_wave_value(Vector, Angle, Phase, Amplitude);
   if (Vector->Transition) apply_transition_xy(Vector->Transition, Angle * (1.0 / 360.0), &x, &y);
   Path.line_to(OriginX + x, OriginY + y);
}

static void add_triangle_vertices(extVectorWave *Vector, agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude)
{
   const double start_phase = Vector->wPhase;
   const double end_phase = wave_phase_at_angle(Vector, 360.0);
   const double first_cycle = floor((start_phase - 270.0) * (1.0 / 360.0)) * 360.0;

   for (double cycle=first_cycle; cycle < end_phase + 360.0; cycle += 360.0) {
      double peak_phase = cycle + 90.0;
      double peak_angle = wave_angle_at_phase(Vector, peak_phase);
      if ((peak_angle > 0.0) and (peak_angle < 360.0)) {
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, peak_angle, peak_phase);
      }

      double trough_phase = cycle + 270.0;
      double trough_angle = wave_angle_at_phase(Vector, trough_phase);
      if ((trough_angle > 0.0) and (trough_angle < 360.0)) {
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, trough_angle, trough_phase);
      }
   }
}

static void add_sawtooth_vertices(extVectorWave *Vector, agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude)
{
   const double start_phase = Vector->wPhase;
   const double end_phase = wave_phase_at_angle(Vector, 360.0);
   const double first_cycle = floor(start_phase * (1.0 / 360.0)) * 360.0;

   for (double cycle=first_cycle; cycle < end_phase + 360.0; cycle += 360.0) {
      double angle = wave_angle_at_phase(Vector, cycle);
      if ((angle > 0.0) and (angle < 360.0)) {
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, angle, cycle - 0.000001);
         add_wave_vertex(Vector, Path, OriginX, OriginY, XScale, Amplitude, angle, cycle);
      }
   }
}

static void apply_wave_thickness(agg::path_storage &Path, double Thickness, double ApproxScale)
{
   // The centreline path is converted into a filled outline using AGG's stroke generator.

   if (Path.total_vertices() < 2) return;

   agg::conv_stroke<agg::path_storage> stroke(Path);
   stroke.width(Thickness);
   stroke.line_join(VLJ::ROUND);  // Rounds the outer (convex) side of sharp peaks
   stroke.inner_join(VIJ::ROUND); // Absorbs the inner (concave) side, preventing cross-over
   stroke.line_cap(VLC::ROUND);
   stroke.approximation_scale(ApproxScale > 0.0 ? ApproxScale : 1.0);

   agg::path_storage outline;
   outline.concat_path(stroke);

   Path.remove_all();
   Path.concat_path(outline);
}

static double fixed_wave_thickness(extVectorWave *Vector)
{
   if (Vector->wThickness.scaled()) return get_parent_diagonal(Vector) * INV_SQRT2 * Vector->wThickness;
   else return Vector->wThickness;
}

//********************************************************************************************************************

static void generate_wave(extVectorWave *Vector, agg::path_storage &Path)
{
   double ox = Vector->wX;
   double oy = Vector->wY;
   Unit length = Vector->wLength;
   Unit amplitude = Vector->wAmplitude;
   double thickness = fixed_wave_thickness(Vector);
   const double view_height = get_parent_height(Vector);

   if (Vector->wX.scaled() or Vector->wLength.scaled()) {
      auto view_width = get_parent_width(Vector);

      if (Vector->wX.scaled()) ox = Unit(ox * view_width);
      if (Vector->wLength.scaled()) length = Unit(length * view_width);
   }

   if (Vector->wY.scaled()) oy = Unit(oy * view_height);
   if (Vector->wAmplitude.scaled()) amplitude = Unit(amplitude * view_height);

   const double amp = amplitude * 0.5;
   const double scale = 1.0 / Vector->Transform.scale(); // Essential for smooth curves when scale > 1.0

   double x = 0, y = decayed_wave_value(Vector, 0, Vector->wPhase, amp);

   if (Vector->Transition) apply_transition_xy(Vector->Transition, 0, &x, &y);

   if ((Vector->wClose IS WVC::NIL) or (thickness > 0)) {
      Path.move_to(ox + x, oy + y);
   }
   else if (Vector->wClose IS WVC::TOP) {
      Path.move_to(ox + length, 0); // Top right
      Path.line_to(ox, 0); // Top left
      Path.line_to(ox + x, oy + y);
   }
   else if (Vector->wClose IS WVC::BOTTOM) {
      Path.move_to(ox + length, view_height); // Bottom right
      Path.line_to(ox, view_height); // Bottom left
      Path.line_to(ox + x, oy + y);
   }
   else return;

   // Wave generator.  This applies scaling so that the correct number of vertices are generated.  Also, the
   // last vertex is interpolated to end exactly at 360, ensuring that the path terminates accurately.

   double xscale = length * (1.0 / 360.0);
   double angle;
   double last_x = x, last_y = y;
   if ((Vector->wType IS WVT::TRIANGLE) or (Vector->wType IS WVT::SAWTOOTH)) {
      if (Vector->wType IS WVT::TRIANGLE) {
         add_triangle_vertices(Vector, Path, ox, oy, xscale, amp);
      }
      else if (Vector->wType IS WVT::SAWTOOTH) {
         add_sawtooth_vertices(Vector, Path, ox, oy, xscale, amp);
      }

      double end_phase = wave_phase_at_angle(Vector, 360.0);
      if ((Vector->wType IS WVT::SAWTOOTH) and (std::abs(normalise_phase(end_phase)) < 0.000001)) {
         end_phase -= 0.000001;
      }
      add_wave_vertex(Vector, Path, ox, oy, xscale, amp, 360.0, end_phase);
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
         double y = decayed_wave_value(Vector, angle, phase, amp);
         if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
         if ((std::abs(x - last_x) >= vertex_tolerance) or (std::abs(y - last_y) >= vertex_tolerance)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      double phase = wave_phase_at_angle(Vector, 360.0);
      double x = length;
      double y = decayed_wave_value(Vector, 360.0, phase, amp);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, 1.0, &x, &y);
      Path.line_to(ox + x, oy + y);
   }

   if (thickness > 0) {
      apply_wave_thickness(Path, thickness, Vector->Transform.scale());
   }

   if ((Vector->wClose != WVC::NIL) or (thickness > 0)) Path.close_polygon();

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
Resize: Changes the vector's length and amplitude.
-END-
*********************************************************************************************************************/

static ERR VECTORWAVE_Resize(extVectorWave *Self, struct acResize *Args)
{
   if (not Args) return ERR::NullArgs;

   Self->wLength = Unit(Args->Width);
   Self->wAmplitude = Unit(Args->Height);
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Amplitude: Defines the generated wave height.

The Amplitude is expressed as a fixed or scaled value.  Fixed values are used directly, while scaled values are
resolved against the parent view height.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Amplitude(extVectorWave *Self, Unit &Value)
{
   Self->wAmplitude = Value;
   reset_path(Self);
   return ERR::Okay;
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
Type: Selects an alternative wave style.

By default, waves are generated in the style of a sine wave.  Alternative styles can be selected by setting this field.
The `SMOOTH` style generates a sine wave, `TRIANGLE` generates a triangular wave and `SAWTOOTH` generates a ramp with a
sharp return edge.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Type(extVectorWave *Self, WVT Value)
{
   Self->wType = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Thickness: Expands the diameter of the wave to the specified value to produce a closed path.

Specifying a thickness value will create a wave that forms a filled shape, rather than the default of a stroked path.
The thickness (diameter) of the wave is determined by the provided value.  If defined as a percentage, the value is
resolved against the normalised diagonal of the parent viewport, matching the scaling rule used for #StrokeWidth.
Thickness is not compatible with the #Close option, and takes precedence over it.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Thickness(extVectorWave *Self, Unit &Value)
{
   Self->wThickness = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Length: The horizontal length of the generated wave.

The length of the generated wave is defined here as a fixed or scaled value.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Length(extVectorWave *Self, Unit &Value)
{
   Self->wLength = Value;
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
   { "Length",     FDF_UNIT|FDF_RW, nullptr, VECTORWAVE_SET_Length },
   { "Amplitude",  FDF_UNIT|FDF_RW, nullptr, VECTORWAVE_SET_Amplitude },
   { "Thickness",  FDF_UNIT|FDF_RW, nullptr, VECTORWAVE_SET_Thickness },
   { "Frequency",  FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Frequency },
   { "Decay",      FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Decay },
   { "Phase",      FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Phase },
   { "Envelope",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORWAVE_SET_Envelope, &clVectorWaveWVE },
   { "Close",      FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORWAVE_SET_Close, &clVectorWaveWVC },
   { "Type",       FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, VECTORWAVE_SET_Type, &clVectorWaveWVT },
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
