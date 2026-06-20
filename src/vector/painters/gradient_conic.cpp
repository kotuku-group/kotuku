/*********************************************************************************************************************

-CLASS-
GradientConic: Conic colour gradient paint server.

GradientConic draws colour values around a centre point, producing a conic gradient.

The #Span field is honoured by `REPEAT`, `REFLECT` and `CLIP` spread modes.  A `REFLECT` spread sweeps the colour
ramp forwards and then back again within each Span cycle, forming a triangular sweep.  With the default span of
`1.0` a single triangle spans the full turn: the ramp runs from start to end across the first half-turn and back to
the start across the second.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
CX: The horizontal centre point of the gradient.

The `(CX, CY)` coordinates define the centre point around which the conic gradient rotates.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTCONIC_SET_CX(extGradientConic *Self, Unit &Value)
{
   Self->CX = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CY: The vertical centre point of the gradient.

The `(CX, CY)` coordinates define the centre point around which the conic gradient rotates.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTCONIC_SET_CY(extGradientConic *Self, Unit &Value)
{
   Self->CY = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Radius: The radius of the gradient.

The radius of the conic gradient can be defined as a fixed unit or scaled relative to its container.  A default
radius of 50% applies if this field is not set.

-END-
*********************************************************************************************************************/

static ERR GRADIENTCONIC_SET_Radius(extGradientConic *Self, Unit &Value)
{
   if (Value >= 0) {
      Self->Radius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************
-FIELD-
Span: Defines the angular size of one conic gradient cycle.

The span is normalised so that `1.0` equals 360 degrees.  Values smaller than `1.0` define a partial angular cycle
for @Gradient.SpreadMethod modes that can repeat, reflect or clip the gradient.  The default value is `1.0`.

When @Gradient.SpreadMethod is `PAD`, Span is ignored and the conic gradient renders as a full-turn gradient.

-END-
*********************************************************************************************************************/

static ERR GRADIENTCONIC_SET_Span(extGradientConic *Self, double Value)
{
   if ((Value <= 0) or (Value > 1.0)) return ERR::OutOfRange;

   Self->Span = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_conic_def.cpp"

static const FieldArray clGradientConicFields[] = {
   { "CX",      FDF_UNIT|FDF_RW, nullptr, GRADIENTCONIC_SET_CX },
   { "CY",      FDF_UNIT|FDF_RW, nullptr, GRADIENTCONIC_SET_CY },
   { "Radius",  FDF_UNIT|FDF_RW, nullptr, GRADIENTCONIC_SET_Radius },
   { "Span",    FDF_DOUBLE|FDF_RW, nullptr, GRADIENTCONIC_SET_Span },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_conic(void)
{
   clGradientConic = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTCONIC),
      fl::Name("GradientConic"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientConicActions),
      fl::Fields(clGradientConicFields),
      fl::Size(sizeof(extGradientConic)),
      fl::Path(MOD_PATH));

   return clGradientConic ? ERR::Okay : ERR::AddClass;
}
