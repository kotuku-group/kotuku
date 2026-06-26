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
   double wDecay = 1.0;
   double wPhase = 0;
   double wThickness = 0;
   WVC wClose = WVC::NIL;
   int wStyle = int(WVS::CURVED);

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

static double wave_angled(double Phase)
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
   switch ((WVS)Vector->wStyle) {
      case WVS::ANGLED:   return wave_angled(Phase);
      case WVS::SAWTOOTH: return wave_sawtooth(Phase);
      case WVS::CURVED:
      case WVS::NIL:
      default:            return sin(DEG2RAD * Phase);
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

   double decay;
   if (Vector->wDecay IS 0) decay = 0.00000001;
   else if (Vector->wDecay >= 0) decay = 360 * Vector->wDecay;
   else decay = 360 * -Vector->wDecay;

   const double amp = (height * 0.5) * Vector->wAmplitude;
   const double scale = 1.0 / Vector->Transform.scale(); // Essential for smooth curves when scale > 1.0

   double x = 0, y = wave_value(Vector, Vector->wPhase) * amp + (height * 0.5);
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

   double phase = Vector->wPhase;
   double xscale = width * (1.0 / 360.0);
   double freq = Vector->wFrequency * scale;
   double angle;
   double last_x = x, last_y = y;
   if (Vector->wDecay IS 1.0) {
      for (angle=scale; angle < 360; angle += scale, phase += freq) {
         double x = angle * xscale;
         double y = (wave_value(Vector, phase) * amp) + (height * 0.5);
         if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
         if ((std::abs(x - last_x) >= 0.5) or (std::abs(y - last_y) >= 0.5)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      phase -= freq;
      phase += freq * (360.0 - (angle - scale)) / scale;
      double x = width;
      double y = (wave_value(Vector, phase) * amp) + (height * 0.5);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
      Path.line_to(ox + x, oy + y);
   }
   else if (Vector->wDecay > 0) {
      for (angle=scale; angle < 360; angle += scale, phase += freq) {
         double x = angle * xscale;
         double y = (wave_value(Vector, phase) * amp) / exp((double)angle / decay) + (height * 0.5);
         if ((std::abs(x - last_x) >= 0.5) or (std::abs(y - last_y) >= 0.5)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      phase -= freq;
      phase += freq * (360.0 - (angle - scale)) / scale;
      double x = width;
      double y = (wave_value(Vector, phase) * amp) / exp(360.0 / decay) + (height * 0.5);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
      Path.line_to(ox + x, oy + y);
   }
   else if (Vector->wDecay < 0) {
      for (angle=scale; angle < 360; angle += scale, phase += freq) {
         double x = angle * xscale;
         double y = (wave_value(Vector, phase) * amp) / log((double)angle / decay) + (height * 0.5);
         if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
         if ((std::abs(x - last_x) >= 0.5) or (std::abs(y - last_y) >= 0.5)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      phase -= freq;
      phase += freq * (360.0 - (angle - scale)) / scale;
      double x = width;
      double y = (wave_value(Vector, phase) * amp) / log(360.0 / decay) + (height * 0.5);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
      Path.line_to(ox + x, oy + y);
   }

   if (Vector->wThickness > 0) {
      double x, y;
      int total = Path.total_vertices();
      Path.last_vertex(&x, &y);
      Path.line_to(x, y + Vector->wThickness);
      for (int i=total-1; i >= 0; i--) {
         Path.vertex(i, &x, &y);
         Path.line_to(x, y + Vector->wThickness);
      }
      Path.translate(0, -Vector->wThickness * 0.5); // Ensure that the wave is centered vertically.
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
filled.

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

The amplitude of a sine wave can be decayed between its start and end points by setting the Decay field.  Using a decay
gives the wave an appearance of being funnelled into a cone-like shape.  If the value is negative, the start and
end points for the decay will be reversed.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Decay(extVectorWave *Self, double Value)
{
   Self->wDecay = Value;
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
Style: Selects an alternative wave style.

By default, waves are generated in the style of a sine wave.  Alternative styles can be selected by setting this field.
The `CURVED` style generates a sine wave, `ANGLED` generates a triangular wave and `SAWTOOTH` generates a ramp with a
sharp return edge.

*********************************************************************************************************************/

static ERR VECTORWAVE_SET_Style(extVectorWave *Self, int Value)
{
   if ((Value IS int(WVS::NIL)) or (Value IS int(WVS::CURVED)) or (Value IS int(WVS::ANGLED)) or
      (Value IS int(WVS::SAWTOOTH))) {
      Self->wStyle = Value;
      reset_path(Self);
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Thickness: Expands the height of the wave to the specified value to produce a closed path.

Specifying a thickness value will create a wave that forms a filled shape, rather than the default of a stroked path.
The thickness (height) of the wave is determined by the provided value.

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
