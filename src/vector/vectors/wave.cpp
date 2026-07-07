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
   double wFrequency    = 1.0;
   double wFrequencyEnd = 1.0;
   double wDecay        = 1.0;
   double wNoise        = 0.0;
   double wPhase        = 0;
   WVE wEnvelope = WVE::LINEAR;
   WVC wClose    = WVC::NIL;
   WVT wType     = WVT::SINE;
   bool wFrequencyEndSet = false;

   extVectorWave(objMetaClass *ClassPtr, OBJECTID ObjectID) : extVector(ClassPtr, ObjectID) {
      GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_wave;
      LineJoin  = VLJ::ROUND;  // Rounds the outer (convex) side of sharp peaks
      InnerJoin = VIJ::ROUND;  // Absorbs the inner (concave) side, preventing cross-over
      LineCap   = VLC::ROUND;
   }

   void apply_wave_thickness(agg::path_storage &Path, double Thickness, double ApproxScale, double Length,
      double Amplitude, double Frequency, double FrequencyEnd);
   double add_sawtooth_vertices(agg::path_storage &Path, double OriginX, double OriginY, double XScale,
      double Amplitude, double Frequency, double FrequencyEnd, double AngleStep, double NoisePhaseInterval,
      double NoiseAngleMargin);
   double add_square_vertices(agg::path_storage &Path, double OriginX, double OriginY, double XScale,
      double Amplitude, double Frequency, double FrequencyEnd, double AngleStep, double NoisePhaseInterval,
      double NoiseAngleMargin);
   double add_triangle_vertices(agg::path_storage &Path, double OriginX, double OriginY, double XScale,
      double Amplitude, double Frequency, double FrequencyEnd, double AngleStep, double NoisePhaseInterval,
      double NoiseAngleMargin);

   inline double normalise_phase(double Phase) {
      double phase = fmod(Phase, 360.0);
      if (phase < 0.0) phase += 360.0;
      return phase;
   }

   double wave_triangle(double Phase) {
      double phase = normalise_phase(Phase);

      if (phase < 90.0) return phase * (1.0 / 90.0);
      else if (phase < 270.0) return 1.0 - ((phase - 90.0) * (1.0 / 90.0));
      else return -1.0 + ((phase - 270.0) * (1.0 / 90.0));
   }

   double wave_sawtooth(double Phase) {
      return 1.0 - (normalise_phase(Phase) * (1.0 / 180.0));
   }

   double wave_square(double Phase) {
      return normalise_phase(Phase) < 180.0 ? 1.0 : -1.0;
   }

   inline double wave_value(extVectorWave *Vector, double Phase) {
      switch (wType) {
         case WVT::TRIANGLE: return wave_triangle(Phase);
         case WVT::SAWTOOTH: return wave_sawtooth(Phase);
         case WVT::SQUARE:   return wave_square(Phase);
         default:            return sin(DEG2RAD * Phase);
      }
   }

   double envelope_curve(double Progress) {
      if (Progress < 0.0) Progress = 0.0;
      else if (Progress > 1.0) Progress = 1.0;

      switch (wEnvelope) {
         case WVE::QUADRATIC:  return Progress * Progress;
         case WVE::SMOOTHSTEP: return Progress * Progress * (3.0 - (2.0 * Progress));
         case WVE::EXPONENTIAL:
            return (1.0 - exp(Progress * -4.0)) * (1.0 / (1.0 - exp(-4.0)));
         default:
            return Progress;
      }
   }

   double envelope_integral(double Progress) {
      if (Progress < 0.0) Progress = 0.0;
      else if (Progress > 1.0) Progress = 1.0;

      switch (wEnvelope) {
         case WVE::QUADRATIC:  return Progress * Progress * Progress * (1.0 / 3.0);
         case WVE::SMOOTHSTEP:
            return (Progress * Progress * Progress) - (0.5 * Progress * Progress * Progress * Progress);
         case WVE::EXPONENTIAL:
            return (Progress + ((exp(Progress * -4.0) - 1.0) * 0.25)) * (1.0 / (1.0 - exp(-4.0)));
         default:
            return Progress * Progress * 0.5;
      }
   }

   inline double wave_progress_at_angle(double Angle) {
      double progress = Angle * (1.0 / 360.0);
      if (progress < 0.0) progress = 0.0;
      else if (progress > 1.0) progress = 1.0;
      return progress;
   }

   inline bool frequency_sweeps(double Frequency, double FrequencyEnd) {
      return not (Frequency IS FrequencyEnd);
   }

   inline double wave_phase_at_angle(double Angle, double Frequency, double FrequencyEnd) {
      double progress = wave_progress_at_angle(Angle);
      return wPhase + (360.0 * ((Frequency * progress) + ((FrequencyEnd - Frequency) * envelope_integral(progress))));
   }

   double wave_angle_at_phase(double Phase, double Frequency, double FrequencyEnd) {
      if (not frequency_sweeps(Frequency, FrequencyEnd)) return (Phase - wPhase) / Frequency;

      const double end_phase = wave_phase_at_angle(360.0, Frequency, FrequencyEnd);
      if (Phase <= wPhase) return 0.0;
      if (Phase >= end_phase) return 360.0;

      double low = 0.0;
      double high = 360.0;
      for (int i=0; i < 32; i++) {
         double mid = (low + high) * 0.5;
         if (wave_phase_at_angle(mid, Frequency, FrequencyEnd) < Phase) low = mid;
         else high = mid;
      }

      return (low + high) * 0.5;
   }

   inline double fixed_wave_thickness() {
      if (wThickness.scaled()) return get_parent_diagonal(this) * INV_SQRT2 * wThickness;
      else return wThickness;
   }

   double capped_wave_frequency(double Frequency, double Length, double Thickness) {
      if ((Thickness <= 0.0) or (std::abs(Length) <= 0.0)) return Frequency;

      double max_frequency = std::abs(Length) * (1.0 / (Thickness * 2.0));
      if ((max_frequency > 0.0) and (Frequency > max_frequency)) return max_frequency;
      else return Frequency;
   }

   double sampled_angle_step(double Scale, double Frequency, double FrequencyEnd) {
      constexpr double MAX_PHASE_STEP = 12.0;
      const double max_frequency = std::max(Frequency, FrequencyEnd);
      double angle_step = Scale;
      double phase_limited_step = MAX_PHASE_STEP * (1.0 / max_frequency);

      if (phase_limited_step < angle_step) angle_step = phase_limited_step;
      if ((wNoise > 0.0) and (4.0 < angle_step)) angle_step = 4.0;
      if (angle_step <= 0.0) angle_step = 1.0;

      return angle_step;
   }

   double phase_progress_at_phase(double Phase, double EndPhase) {
      const double phase_range = EndPhase - wPhase;
      if (phase_range <= 0.0) return 0.0;

      double progress = (Phase - wPhase) / phase_range;
      if (progress < 0.0) progress = 0.0;
      else if (progress > 1.0) progress = 1.0;
      return progress;
   }

   double exponential_decay_multiplier(double Progress) {
      constexpr double MAX_DECAY = 100.0;
      constexpr double MIN_DECAY = 0.000001;
      double end_scale = std::abs(wDecay);
      if (end_scale > MAX_DECAY) end_scale = MAX_DECAY;

      if (wDecay < 0) Progress = 1.0 - Progress;
      if (end_scale <= 0.0) {
         if (Progress >= 1.0) return 0.0;
         end_scale = MIN_DECAY;
      }

      return exp(log(end_scale) * Progress);
   }

   double decay_multiplier(double Angle, double Phase, double EndPhase) {
      double progress = Angle * (1.0 / 360.0);
      if (progress < 0.0) progress = 0.0;
      else if (progress > 1.0) progress = 1.0;

      if (wEnvelope IS WVE::EXPONENTIAL) {
         return exponential_decay_multiplier(phase_progress_at_phase(Phase, EndPhase));
      }

      // For a negative decay the envelope grows rather than decays, so mirror the shaping
      // curve about the midpoint to keep the fast portion at the end of the wave.
      if (wDecay < 0) progress = 1.0 - progress;

      progress = envelope_curve(progress);

      constexpr double MAX_DECAY = 100.0;
      double end_scale = std::abs(wDecay);
      if (end_scale > MAX_DECAY) end_scale = MAX_DECAY;

      if (wDecay < 0) return end_scale + ((1.0 - end_scale) * (1.0 - progress));
      else return 1.0 - ((1.0 - end_scale) * progress);
   }

   double noise_phase_interval(double Length, double Thickness, double Frequency, double FrequencyEnd) {
      constexpr double DEFAULT_NOISE_PHASE_INTERVAL = 45.0;

      if ((Thickness <= 0.0) or (std::abs(Length) <= 0.0)) return DEFAULT_NOISE_PHASE_INTERVAL;

      double max_frequency = std::max(Frequency, FrequencyEnd);
      if (max_frequency <= 0.0) return DEFAULT_NOISE_PHASE_INTERVAL;

      double thickness_limited_interval = Thickness * 2.0 * 360.0 * max_frequency / std::abs(Length);
      if (thickness_limited_interval > DEFAULT_NOISE_PHASE_INTERVAL) return thickness_limited_interval;
      else return DEFAULT_NOISE_PHASE_INTERVAL;
   }

   double noise_angle_margin(double Length, double Thickness) {
      if ((Thickness <= 0.0) or (std::abs(Length) <= 0.0)) return 0.0;
      else return Thickness * 360.0 / std::abs(Length);
   }

   double noise_sample(int Index) {
      double raw = sin((double(Index) * 12.9898) + 78.233) * 43758.5453123;
      return ((raw - floor(raw)) * 2.0) - 1.0;
   }

   double noise_value(double Phase, double PhaseInterval) {
      double position = Phase * (1.0 / PhaseInterval);
      double base = floor(position);
      double fraction = position - base;
      double smooth = fraction * fraction * (3.0 - (2.0 * fraction));
      double start = noise_sample(int(base));
      double end = noise_sample(int(base) + 1);

      return start + ((end - start) * smooth);
   }

   inline double decayed_wave_value(double Angle, double Phase, double EndPhase, double Amplitude,
      double NoisePhaseInterval)
   {
      double multiplier = decay_multiplier(Angle, Phase, EndPhase);
      double value = wave_value(this, Phase) * Amplitude * multiplier;
      if (wNoise > 0.0) value += noise_value(Phase, NoisePhaseInterval) * std::abs(Amplitude) * wNoise * multiplier;
      return value;
   }

   inline void add_wave_vertex(agg::path_storage &Path, double OriginX, double OriginY,
      double XScale, double Amplitude, double Angle, double Phase, double EndPhase, double NoisePhaseInterval)
   {
      double x = Angle * XScale;
      double y = decayed_wave_value(Angle, Phase, EndPhase, Amplitude, NoisePhaseInterval);
      if (Transition) apply_transition_xy(Transition, Angle * (1.0 / 360.0), &x, &y);
      Path.line_to(OriginX + x, OriginY + y);
   }

   void add_sampled_wave_vertices(agg::path_storage &Path, double OriginX, double OriginY, double XScale,
      double Amplitude, double Frequency, double FrequencyEnd, double StartAngle, double EndAngle, double AngleStep,
      double EndPhase, double NoisePhaseInterval, double NoiseAngleMargin)
   {
      if ((wNoise <= 0.0) or (EndAngle <= StartAngle)) return;

      double start_angle = StartAngle + NoiseAngleMargin;
      double end_angle = EndAngle - NoiseAngleMargin;
      if (end_angle <= start_angle) return;

      double angle = StartAngle + AngleStep;
      if (angle < start_angle) {
         angle += ceil((start_angle - angle) * (1.0 / AngleStep)) * AngleStep;
      }

      for (; angle < end_angle; angle += AngleStep) {
         double phase = wave_phase_at_angle(angle, Frequency, FrequencyEnd);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, angle, phase, EndPhase, NoisePhaseInterval);
      }
   }

   bool wave_needs_resolve(double Length, double Amplitude, double Thickness, double Frequency,
      double FrequencyEnd)
   {
      if (wNoise > 0.0) return true;
      if (wType != WVT::SINE) return false;
      if ((Transition) or (not (wDecay IS 1.0)) or frequency_sweeps(Frequency, FrequencyEnd)) return true;

      const double xscale = std::abs(Length) * (1.0 / 360.0);
      const double half_thickness = Thickness * 0.5;
      const double max_frequency = std::max(Frequency, FrequencyEnd);
      if ((xscale <= 0.0) or (half_thickness <= 0.0) or (std::abs(Amplitude) <= 0.0) or (max_frequency <= 0.0)) {
         return false;
      }

      const double phase_scale = DEG2RAD * max_frequency;
      const double max_curvature = std::abs(Amplitude) * phase_scale * phase_scale / (xscale * xscale);
      constexpr double STROKE_RESOLVE_CURVATURE_MARGIN = 0.9;

      // The ideal sine offset reaches a cusp at 1.0, but AGG strokes the sampled polyline generated below.  Round joins
      // can materialise small loops just before the analytic limit, so resolve close cases as well.
      return (half_thickness * max_curvature) >= STROKE_RESOLVE_CURVATURE_MARGIN;
   }
};

//********************************************************************************************************************

double extVectorWave::add_triangle_vertices(agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude, double Frequency, double FrequencyEnd, double AngleStep,
   double NoisePhaseInterval, double NoiseAngleMargin)
{
   const double start_phase = wPhase;
   const double end_phase   = wave_phase_at_angle(360.0, Frequency, FrequencyEnd);
   const double first_cycle = floor((start_phase - 270.0) * (1.0 / 360.0)) * 360.0;
   double last_angle = 0.0;

   for (double cycle=first_cycle; cycle < end_phase + 360.0; cycle += 360.0) {
      double peak_phase = cycle + 90.0;
      double peak_angle = wave_angle_at_phase(peak_phase, Frequency, FrequencyEnd);
      if ((peak_angle > 0.0) and (peak_angle < 360.0)) {
         add_sampled_wave_vertices(Path, OriginX, OriginY, XScale, Amplitude, Frequency, FrequencyEnd,
            last_angle, peak_angle, AngleStep, end_phase, NoisePhaseInterval, NoiseAngleMargin);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, peak_angle, peak_phase, end_phase,
            NoisePhaseInterval);
         last_angle = peak_angle;
      }

      double trough_phase = cycle + 270.0;
      double trough_angle = wave_angle_at_phase(trough_phase, Frequency, FrequencyEnd);
      if ((trough_angle > 0.0) and (trough_angle < 360.0)) {
         add_sampled_wave_vertices(Path, OriginX, OriginY, XScale, Amplitude, Frequency, FrequencyEnd,
            last_angle, trough_angle, AngleStep, end_phase, NoisePhaseInterval, NoiseAngleMargin);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, trough_angle, trough_phase, end_phase,
            NoisePhaseInterval);
         last_angle = trough_angle;
      }
   }

   return last_angle;
}

double extVectorWave::add_sawtooth_vertices(agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude, double Frequency, double FrequencyEnd, double AngleStep,
   double NoisePhaseInterval, double NoiseAngleMargin)
{
   const double start_phase = wPhase;
   const double end_phase   = wave_phase_at_angle(360.0, Frequency, FrequencyEnd);
   const double first_cycle = floor(start_phase * (1.0 / 360.0)) * 360.0;
   double last_angle = 0.0;

   for (double cycle=first_cycle; cycle < end_phase + 360.0; cycle += 360.0) {
      double angle = wave_angle_at_phase(cycle, Frequency, FrequencyEnd);
      if ((angle > 0.0) and (angle < 360.0)) {
         add_sampled_wave_vertices(Path, OriginX, OriginY, XScale, Amplitude, Frequency, FrequencyEnd,
            last_angle, angle, AngleStep, end_phase, NoisePhaseInterval, NoiseAngleMargin);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, angle, cycle - 0.000001, end_phase,
            NoisePhaseInterval);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, angle, cycle, end_phase, NoisePhaseInterval);
         last_angle = angle;
      }
   }

   return last_angle;
}

double extVectorWave::add_square_vertices(agg::path_storage &Path, double OriginX, double OriginY,
   double XScale, double Amplitude, double Frequency, double FrequencyEnd, double AngleStep,
   double NoisePhaseInterval, double NoiseAngleMargin)
{
   const double start_phase = wPhase;
   const double end_phase   = wave_phase_at_angle(360.0, Frequency, FrequencyEnd);
   const double first_cycle = floor(start_phase * (1.0 / 360.0)) * 360.0;
   double last_angle = 0.0;

   for (double cycle=first_cycle; cycle < end_phase + 360.0; cycle += 360.0) {
      double rising_angle = wave_angle_at_phase(cycle, Frequency, FrequencyEnd);
      if ((rising_angle > 0.0) and (rising_angle < 360.0)) {
         add_sampled_wave_vertices(Path, OriginX, OriginY, XScale, Amplitude, Frequency, FrequencyEnd,
            last_angle, rising_angle, AngleStep, end_phase, NoisePhaseInterval, NoiseAngleMargin);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, rising_angle, cycle - 0.000001, end_phase,
            NoisePhaseInterval);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, rising_angle, cycle, end_phase,
            NoisePhaseInterval);
         last_angle = rising_angle;
      }

      double falling_phase = cycle + 180.0;
      double falling_angle = wave_angle_at_phase(falling_phase, Frequency, FrequencyEnd);
      if ((falling_angle > 0.0) and (falling_angle < 360.0)) {
         add_sampled_wave_vertices(Path, OriginX, OriginY, XScale, Amplitude, Frequency, FrequencyEnd,
            last_angle, falling_angle, AngleStep, end_phase, NoisePhaseInterval, NoiseAngleMargin);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, falling_angle, falling_phase - 0.000001,
            end_phase, NoisePhaseInterval);
         add_wave_vertex(Path, OriginX, OriginY, XScale, Amplitude, falling_angle, falling_phase, end_phase,
            NoisePhaseInterval);
         last_angle = falling_angle;
      }
   }

   return last_angle;
}

void extVectorWave::apply_wave_thickness(agg::path_storage &Path, double Thickness, double ApproxScale, double Length,
   double Amplitude, double Frequency, double FrequencyEnd)
{
   // The centreline path is converted into a filled outline using AGG's stroke generator.

   if (Path.total_vertices() < 2) return;

   agg::path_storage centreline;
   centreline.concat_path(Path);

   agg::conv_stroke<agg::path_storage> stroke(centreline);
   stroke.width(Thickness);
   stroke.line_join(this->LineJoin);  // Rounds the outer (convex) side of sharp peaks
   stroke.inner_join(this->InnerJoin); // Absorbs the inner (concave) side, preventing cross-over
   stroke.line_cap(this->LineCap);
   stroke.approximation_scale(ApproxScale > 0.0 ? ApproxScale : 1.0);

   if (wave_needs_resolve(Length, Amplitude, Thickness, Frequency, FrequencyEnd)) {
      agg::resolve_stroke_self_intersections(Path, stroke);
   }
   else {
      Path.remove_all();
      Path.concat_path(stroke);
   }
}

//********************************************************************************************************************

static void generate_wave(extVectorWave *Vector, agg::path_storage &Path)
{
   double ox = Vector->wX;
   double oy = Vector->wY;
   Unit length = Vector->wLength;
   Unit amplitude = Vector->wAmplitude;
   double thickness = Vector->fixed_wave_thickness();
   const double view_height = get_parent_height(Vector);

   if (Vector->wX.scaled() or Vector->wLength.scaled()) {
      auto view_width = get_parent_width(Vector);

      if (Vector->wX.scaled()) ox = Unit(ox * view_width);
      if (Vector->wLength.scaled()) length = Unit(length * view_width);
   }

   if (Vector->wY.scaled()) oy = Unit(oy * view_height);
   if (Vector->wAmplitude.scaled()) amplitude = Unit(amplitude * view_height);

   const double amp = amplitude * 0.5;
   const double scale = 1.0 / Vector->Transform.scale(); // Essential for sine curves when scale > 1.0
   const double frequency = Vector->capped_wave_frequency(Vector->wFrequency, length, thickness);
   const double frequency_end = Vector->capped_wave_frequency(Vector->wFrequencyEnd, length, thickness);
   double end_phase = Vector->wave_phase_at_angle(360.0, frequency, frequency_end);
   double noise_phase_interval = Vector->noise_phase_interval(length, thickness, frequency, frequency_end);
   double noise_angle_margin = Vector->noise_angle_margin(length, thickness);

   double x = 0, y = Vector->decayed_wave_value(0, Vector->wPhase, end_phase, amp, noise_phase_interval);

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
   double angle_step = Vector->sampled_angle_step(scale, frequency, frequency_end);
   if ((Vector->wType IS WVT::TRIANGLE) or (Vector->wType IS WVT::SAWTOOTH) or (Vector->wType IS WVT::SQUARE)) {
      double last_angle = 0.0;
      if (Vector->wType IS WVT::TRIANGLE) {
         last_angle = Vector->add_triangle_vertices(Path, ox, oy, xscale, amp, frequency, frequency_end, angle_step,
            noise_phase_interval, noise_angle_margin);
      }
      else if (Vector->wType IS WVT::SAWTOOTH) {
         last_angle = Vector->add_sawtooth_vertices(Path, ox, oy, xscale, amp, frequency, frequency_end, angle_step,
            noise_phase_interval, noise_angle_margin);
      }
      else if (Vector->wType IS WVT::SQUARE) {
         last_angle = Vector->add_square_vertices(Path, ox, oy, xscale, amp, frequency, frequency_end, angle_step,
            noise_phase_interval, noise_angle_margin);
      }

      if ((Vector->wType IS WVT::SAWTOOTH) and (std::abs(Vector->normalise_phase(end_phase)) < 0.000001)) {
         end_phase -= 0.000001;
      }
      else if (Vector->wType IS WVT::SQUARE) {
         double phase = Vector->normalise_phase(end_phase);
         if ((std::abs(phase) < 0.000001) or (std::abs(phase - 180.0) < 0.000001)) end_phase -= 0.000001;
      }
      Vector->add_sampled_wave_vertices(Path, ox, oy, xscale, amp, frequency, frequency_end, last_angle, 360.0,
         angle_step, end_phase, noise_phase_interval, noise_angle_margin);
      Vector->add_wave_vertex(Path, ox, oy, xscale, amp, 360.0, end_phase, end_phase, noise_phase_interval);
   }
   else {
      double last_x = x, last_y = y;
      double vertex_tolerance = 0.5 * scale;

      for (angle=angle_step; angle < 360; angle += angle_step) {
         double phase = Vector->wave_phase_at_angle(angle, frequency, frequency_end);
         double x = angle * xscale;
         double y = Vector->decayed_wave_value(angle, phase, end_phase, amp, noise_phase_interval);
         if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
         if ((std::abs(x - last_x) >= vertex_tolerance) or (std::abs(y - last_y) >= vertex_tolerance)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      double phase = Vector->wave_phase_at_angle(360.0, frequency, frequency_end);
      double x = length;
      double y = Vector->decayed_wave_value(360.0, phase, end_phase, amp, noise_phase_interval);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, 1.0, &x, &y);
      Path.line_to(ox + x, oy + y);
   }

   if (thickness > 0) {
      Vector->apply_wave_thickness(Path, thickness, Vector->Transform.scale(), length, amp, frequency, frequency_end);
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
Envelope: Selects the interpolation envelope applied to the wave.

The Envelope field controls how the #Decay value, and any #FrequencyEnd sweep, is interpolated across the wave.  The
default `LINEAR` envelope uses an even taper, while `QUADRATIC`, `SMOOTHSTEP` and `EXPONENTIAL` apply progressively
stronger curves.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Envelope(extVectorWave *Self, WVE Value)
{
   Self->wEnvelope = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Frequency: Defines the wave frequency at the start of the wave.

The frequency determines the distance between each individual wave that is generated.  The default
value for the frequency is 1.0.  Shortening the frequency to a value closer to 0 will bring the waves closer together.
If #FrequencyEnd is set to a different value, Frequency is used as the start frequency for the sweep.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Frequency(extVectorWave *Self, double Value)
{
   if (Value > 0.0) {
      Self->wFrequency = Value;
      if (not Self->wFrequencyEndSet) Self->wFrequencyEnd = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
FrequencyEnd: Defines the wave frequency at the end of the wave.

When FrequencyEnd differs from #Frequency, the wave performs a frequency sweep between the start and end points.  The
frequency is interpolated across the wave using the selected #Envelope, and the phase is integrated from that
interpolated frequency so the result behaves as a chirp.  If FrequencyEnd is not assigned, it follows #Frequency and
the wave uses a constant frequency.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_FrequencyEnd(extVectorWave *Self, double Value)
{
   if (Value > 0.0) {
      Self->wFrequencyEnd = Value;
      Self->wFrequencyEndSet = true;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Noise: Perturbs the generated wave path with deterministic noise.

The Noise value ranges from `0.0` to `1.0`.  A value of `0.0` applies no noise and preserves the unperturbed wave,
while `1.0` applies the strongest perturbation.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Noise(extVectorWave *Self, double Value)
{
   if ((Value >= 0.0) and (Value <= 1.0)) {
      Self->wNoise = Value;
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
The `SINE` style generates a sine wave, `TRIANGLE` generates a triangular wave, `SAWTOOTH` generates a ramp with a
sharp return edge and `SQUARE` generates a stepped wave with vertical transitions.

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
resolved against the normalised diagonal of the parent viewport, matching the scaling rule used for @Vector.StrokeWidth.
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
   { "FrequencyEnd", FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_FrequencyEnd },
   { "Decay",      FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Decay },
   { "Noise",      FDF_DOUBLE|FDF_RW, nullptr, VECTORWAVE_SET_Noise },
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
