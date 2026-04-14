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

class extVectorWave : public extVector {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::VECTORWAVE;
   static constexpr CSTRING CLASS_NAME = "VectorWave";
   using create = pf::Create<extVectorWave>;

   double wX, wY;
   double wWidth, wHeight;
   double wAmplitude;
   double wFrequency;
   double wDecay;
   double wDegree;
   double wThickness;
   DMF    wDimensions;
   WVC    wClose;
   uint8_t  wStyle;
};

//********************************************************************************************************************

static void generate_wave(extVectorWave *Vector, agg::path_storage &Path)
{
   double ox = Vector->wX, oy = Vector->wY;
   double width = Vector->wWidth, height = Vector->wHeight;

   if (dmf::hasScaledX(Vector->wDimensions)) ox *= get_parent_width(Vector);
   if (dmf::hasScaledY(Vector->wDimensions)) oy *= get_parent_height(Vector);
   if (dmf::hasScaledWidth(Vector->wDimensions)) width *= get_parent_width(Vector);
   if (dmf::hasScaledHeight(Vector->wDimensions)) height *= get_parent_height(Vector);

   double decay;
   if (Vector->wDecay IS 0) decay = 0.00000001;
   else if (Vector->wDecay >= 0) decay = 360 * Vector->wDecay;
   else decay = 360 * -Vector->wDecay;

   const double amp = (height * 0.5) * Vector->wAmplitude;
   const double scale = 1.0 / Vector->Transform.scale(); // Essential for smooth curves when scale > 1.0

   double x = 0, y = sin(DEG2RAD * Vector->wDegree) * amp + (height * 0.5);
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

   // Sine wave generator.  This applies scaling so that the correct number of vertices are generated.  Also, the
   // last vertex is interpolated to end exactly at 360, ensuring that the path terminates accurately.

   double degree = Vector->wDegree;
   double xscale = width * (1.0 / 360.0);
   double freq = Vector->wFrequency * scale;
   double angle;
   double last_x = x, last_y = y;
   if (Vector->wDecay IS 1.0) {
      for (angle=scale; angle < 360; angle += scale, degree += freq) {
         double x = angle * xscale;
         double y = (sin(DEG2RAD * degree) * amp) + (height * 0.5);
         if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
         if ((std::abs(x - last_x) >= 0.5) or (std::abs(y - last_y) >= 0.5)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      degree -= freq;
      degree += freq * (360.0 - (angle - scale)) / scale;
      double x = width;
      double y = (sin(DEG2RAD * degree) * amp) + (height * 0.5);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
      Path.line_to(ox + x, oy + y);
   }
   else if (Vector->wDecay > 0) {
      for (angle=scale; angle < 360; angle += scale, degree += freq) {
         double x = angle * xscale;
         double y = (sin(DEG2RAD * degree) * amp) / exp((double)angle / decay) + (height * 0.5);
         if ((std::abs(x - last_x) >= 0.5) or (std::abs(y - last_y) >= 0.5)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      degree -= freq;
      degree += freq * (360.0 - (angle - scale)) / scale;
      double x = width;
      double y = (sin(DEG2RAD * degree) * amp) / exp(360.0 / decay) + (height * 0.5);
      if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
      Path.line_to(ox + x, oy + y);
   }
   else if (Vector->wDecay < 0) {
      for (angle=scale; angle < 360; angle += scale, degree += freq) {
         double x = angle * xscale;
         double y = (sin(DEG2RAD * degree) * amp) / log((double)angle / decay) + (height * 0.5);
         if (Vector->Transition) apply_transition_xy(Vector->Transition, angle * (1.0 / 360.0), &x, &y);
         if ((std::abs(x - last_x) >= 0.5) or (std::abs(y - last_y) >= 0.5)) {
            Path.line_to(ox + x, oy + y);
            last_x = x;
            last_y = y;
         }
      }
      degree -= freq;
      degree += freq * (360.0 - (angle - scale)) / scale;
      double x = width;
      double y = (sin(DEG2RAD * degree) * amp) / log(360.0 / decay) + (height * 0.5);
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

static ERR WAVE_Move(extVectorWave *Self, struct acMove *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   Self->wX += Args->DeltaX;
   Self->wY += Args->DeltaY;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToPoint: Moves the vector to a new fixed position.
-END-
*********************************************************************************************************************/

static ERR WAVE_MoveToPoint(extVectorWave *Self, struct acMoveToPoint *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if ((Args->Flags & MTF::X) != MTF::NIL) Self->wX = Args->X;
   if ((Args->Flags & MTF::Y) != MTF::NIL) Self->wY = Args->Y;
   if ((Args->Flags & MTF::RELATIVE) != MTF::NIL) Self->wDimensions = (Self->wDimensions | DMF::SCALED_X | DMF::SCALED_Y) & ~(DMF::FIXED_X | DMF::FIXED_Y);
   else Self->wDimensions = (Self->wDimensions | DMF::FIXED_X | DMF::FIXED_Y) & ~(DMF::SCALED_X | DMF::SCALED_Y);
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR WAVE_NewObject(extVectorWave *Self)
{
   Self->GeneratePath = (void (*)(extVector *, agg::path_storage &))&generate_wave;
   Self->wFrequency = 1.0;
   Self->wAmplitude = 1.0;
   Self->wDecay = 1.0;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Changes the vector's area.
-END-
*********************************************************************************************************************/

static ERR WAVE_Resize(extVectorWave *Self, struct acResize *Args)
{
   if (!Args) return ERR::NullArgs;

   Self->wWidth = Args->Width;
   Self->wHeight = Args->Height;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Amplitude: Adjusts the generated wave amplitude.

The Amplitude is expressed as a multiplier that adjusts the wave amplitude (i.e. height).  A value of 1.0 is the
default.

*********************************************************************************************************************/

static ERR WAVE_GET_Amplitude(extVectorWave *Self, double *Value)
{
   *Value = Self->wAmplitude;
   return ERR::Okay;
}

static ERR WAVE_SET_Amplitude(extVectorWave *Self, double Value)
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

static ERR WAVE_GET_Close(extVectorWave *Self, WVC *Value)
{
   *Value = Self->wClose;
   return ERR::Okay;
}

static ERR WAVE_SET_Close(extVectorWave *Self, WVC Value)
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

static ERR WAVE_GET_Decay(extVectorWave *Self, double *Value)
{
   *Value = Self->wDecay;
   return ERR::Okay;
}

static ERR WAVE_SET_Decay(extVectorWave *Self, double Value)
{
   Self->wDecay = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Degree: Declares the initial angle (in degrees) to use when generating the wave.

The degree value defines the initial angle that is used when computing the sine wave.  The default is zero.

Visually, changing the degree will affect the 'offset' of the generated wave.  Gradually incrementing the value
will give the wave an appearance of moving from right to left.

*********************************************************************************************************************/

static ERR WAVE_GET_Degree(extVectorWave *Self, double *Value)
{
   *Value = Self->wDegree;
   return ERR::Okay;
}

static ERR WAVE_SET_Degree(extVectorWave *Self, double Value)
{
   Self->wDegree = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Dimensions: Dimension flags define whether individual dimension fields contain fixed or scaled values.

The following dimension flags are supported:

<types lookup="DMF">
<type name="FIXED_HEIGHT">The #Height value is a fixed coordinate.</>
<type name="FIXED_WIDTH">The #Width value is a fixed coordinate.</>
<type name="FIXED_X">The #X value is a fixed coordinate.</>
<type name="FIXED_Y">The #Y value is a fixed coordinate.</>
<type name="SCALED_HEIGHT">The #Height value is a scaled coordinate.</>
<type name="SCALED_WIDTH">The #Width value is a scaled coordinate.</>
<type name="SCALED_X">The #X value is a scaled coordinate.</>
<type name="SCALED_Y">The #Y value is a scaled coordinate.</>
</types>

*********************************************************************************************************************/

static ERR WAVE_GET_Dimensions(extVectorWave *Self, DMF *Value)
{
   *Value = Self->wDimensions;
   return ERR::Okay;
}

static ERR WAVE_SET_Dimensions(extVectorWave *Self, DMF Value)
{
   Self->wDimensions = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Frequency: Defines the wave frequency (the distance between each wave).

The frequency determines the distance between each individual wave that is generated.  The default
value for the frequency is 1.0.  Shortening the frequency to a value closer to 0 will bring the waves closer together.

*********************************************************************************************************************/

static ERR WAVE_GET_Frequency(extVectorWave *Self, double *Value)
{
   *Value = Self->wFrequency;
   return ERR::Okay;
}

static ERR WAVE_SET_Frequency(extVectorWave *Self, double Value)
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

static ERR WAVE_GET_Height(extVectorWave *Self, Unit *Value)
{
   Value->set(Self->wHeight);
   return ERR::Okay;
}

static ERR WAVE_SET_Height(extVectorWave *Self, Unit &Value)
{
   if (Value.scaled()) Self->wDimensions = (Self->wDimensions | DMF::SCALED_HEIGHT) & (~DMF::FIXED_HEIGHT);
   else Self->wDimensions = (Self->wDimensions | DMF::FIXED_HEIGHT) & (~DMF::SCALED_HEIGHT);

   Self->wHeight = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Style: Selects an alternative wave style.

NOT IMPLEMENTED

By default, waves are generated in the style of a sine wave.  Alternative styles can be selected by setting this field.

*********************************************************************************************************************/

static ERR WAVE_GET_Style(extVectorWave *Self, int *Value)
{
   *Value = Self->wStyle;
   return ERR::Okay;
}

static ERR WAVE_SET_Style(extVectorWave *Self, int Value)
{
   Self->wStyle = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Thickness: Expands the height of the wave to the specified value to produce a closed path.

Specifying a thickness value will create a wave that forms a filled shape, rather than the default of a stroked path.
The thickness (height) of the wave is determined by the provided value.

*********************************************************************************************************************/

static ERR WAVE_GET_Thickness(extVectorWave *Self, double *Value)
{
   *Value = Self->wThickness;
   return ERR::Okay;
}

static ERR WAVE_SET_Thickness(extVectorWave *Self, double Value)
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

static ERR WAVE_GET_Width(extVectorWave *Self, Unit *Value)
{
   Value->set(Self->wWidth);
   return ERR::Okay;
}

static ERR WAVE_SET_Width(extVectorWave *Self, Unit &Value)
{
   if (Value.scaled()) Self->wDimensions = (Self->wDimensions | DMF::SCALED_WIDTH) & (~DMF::FIXED_WIDTH);
   else Self->wDimensions = (Self->wDimensions | DMF::FIXED_WIDTH) & (~DMF::SCALED_WIDTH);
   Self->wWidth = Value;
   reset_path(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X: The x coordinate of the wave.  Can be expressed as a fixed or scaled coordinate.

The x coordinate of the wave is defined here as either a fixed or scaled value.

*********************************************************************************************************************/

static ERR WAVE_GET_X(extVectorWave *Self, Unit *Value)
{
   Value->set(Self->wX);
   return ERR::Okay;
}

static ERR WAVE_SET_X(extVectorWave *Self, Unit &Value)
{
   if (Value.scaled()) Self->wDimensions = (Self->wDimensions | DMF::SCALED_X) & (~DMF::FIXED_X);
   else Self->wDimensions = (Self->wDimensions | DMF::FIXED_X) & (~DMF::SCALED_X);
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

static ERR WAVE_GET_Y(extVectorWave *Self, Unit *Value)
{
   Value->set(Self->wY);
   return ERR::Okay;
}

static ERR WAVE_SET_Y(extVectorWave *Self, Unit &Value)
{
   if (Value.scaled()) Self->wDimensions = (Self->wDimensions | DMF::SCALED_Y) & (~DMF::FIXED_Y);
   else Self->wDimensions = (Self->wDimensions | DMF::FIXED_Y) & (~DMF::SCALED_Y);
   Self->wY = Value;
   reset_path(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldDef clWaveClose[] = {
   { "None",   WVC::NONE },
   { "Top",    WVC::TOP },
   { "Bottom", WVC::BOTTOM },
   { nullptr, 0 }
};

static const FieldDef clWaveStyle[] = {
   { "Curved",   WVS::CURVED },
   { "Angled",   WVS::ANGLED },
   { "Sawtooth", WVS::SAWTOOTH },
   { nullptr, 0 }
};

static const FieldDef clWaveDimensions[] = {
   { "FixedHeight",   DMF::FIXED_HEIGHT },
   { "FixedWidth",    DMF::FIXED_WIDTH },
   { "FixedX",        DMF::FIXED_X },
   { "FixedY",        DMF::FIXED_Y },
   { "ScaledHeight",  DMF::SCALED_HEIGHT },
   { "ScaledWidth",   DMF::SCALED_WIDTH },
   { "ScaledX",       DMF::SCALED_X },
   { "ScaledY",       DMF::SCALED_Y },
   { nullptr, 0 }
};

static const FieldArray clWaveFields[] = {
   { "Amplitude",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, WAVE_GET_Amplitude, WAVE_SET_Amplitude },
   { "Close",      FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, WAVE_GET_Close, WAVE_SET_Close, &clWaveClose },
   { "Decay",      FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, WAVE_GET_Decay, WAVE_SET_Decay },
   { "Degree",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, WAVE_GET_Degree, WAVE_SET_Degree },
   { "Dimensions", FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, WAVE_GET_Dimensions, WAVE_SET_Dimensions, &clWaveDimensions },
   { "Frequency",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, WAVE_GET_Frequency, WAVE_SET_Frequency },
   { "Height",     FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, WAVE_GET_Height, WAVE_SET_Height },
   { "Style",      FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, WAVE_GET_Style, WAVE_SET_Style, &clWaveStyle },
   { "Thickness",  FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, WAVE_GET_Thickness, WAVE_SET_Thickness },
   { "X",          FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, WAVE_GET_X, WAVE_SET_X },
   { "Y",          FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, WAVE_GET_Y, WAVE_SET_Y },
   { "Width",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, WAVE_GET_Width, WAVE_SET_Width },
   END_FIELD
};

static const ActionArray clWaveActions[] = {
   { AC::NewObject,     WAVE_NewObject },
   { AC::Move,          WAVE_Move },
   { AC::MoveToPoint,   WAVE_MoveToPoint },
   //{ AC::Redimension, WAVE_Redimension },
   { AC::Resize,      WAVE_Resize },
   { AC::NIL, nullptr }
};

static ERR init_wave(void)
{
   clVectorWave = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTOR),
      fl::ClassID(CLASSID::VECTORWAVE),
      fl::Name("VectorWave"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clWaveActions),
      fl::Fields(clWaveFields),
      fl::Size(sizeof(extVectorWave)),
      fl::Path(MOD_PATH));

   return clVectorWave ? ERR::Okay : ERR::AddClass;
}
