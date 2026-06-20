/*********************************************************************************************************************

-CLASS-
GradientDistal: Signed distance field colour gradient paint server.

GradientDistal shades inward and outward from the target vector path outline.  The path outline is pinned to the
centre of the colour ramp, `Radius` controls exterior padding, and `InnerRadius` controls interior fade-out.

The base @Gradient.SpreadMethod field selects how the exterior fade behaves beyond the first cycle, where one cycle spans the
`Radius` distance:

<list type="bullet">
<li>`PAD` (the default) fades once over the `Radius` distance and then stops, producing the classic glow or halo.</li>
<li>`REPEAT` tiles the fade outward as a sawtooth: at the end of each `Radius` cycle the alpha and colour restart at
full strength, repeating until the parent area is filled.</li>
<li>`REFLECT` tiles the fade outward as a triangle wave: each cycle alternates direction, ramping the alpha back up
from zero before fading again, repeating until the parent area is filled.</li>
</list>

Setting #OuterFall to `GFALL::OPAQUE` holds the exterior alpha at full strength rather than tapering it, so with
`REPEAT` or `REFLECT` the full colour ramp is painted solidly out to the parent edge with no fade between tiles.  Under
`PAD` the alpha still cuts to transparent at the padding edge as usual.

This gradient operates exclusively in `BOUNDING_BOX` space.  If @Gradient.Units is set to `USERSPACE` it will be
reset to `BOUNDING_BOX` during initialisation.

-END-

*********************************************************************************************************************/

static ERR GRADIENTDISTAL_Init(extGradientDistal *Self)
{
   kt::Log log;

   if (Self->Units IS VUNIT::USERSPACE) {
      log.warning("Distal gradients are not compatible with Units.USERSPACE.");
      Self->Units = VUNIT::BOUNDING_BOX;
   }

   if (Self->Floor >= Self->Multiplier) return log.warning(ERR::OutOfRange);

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
InnerRadius: The size of the inner radius for distal gradients.

The InnerRadius defines the extent of the interior fade-out for distal gradients.  If zero, the interior of the
target shape is fully rendered.

*********************************************************************************************************************/

static ERR GRADIENTDISTAL_SET_InnerRadius(extGradientDistal *Self, Unit &Value)
{
   if (Value >= 0) {
      Self->SDFHash = 0;
      Self->InnerRadius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************
-FIELD-
Radius: Exterior margin around the target path.

The Radius value controls how much exterior padding is added around the target path so that the exterior half of the
signed distance colour ramp remains visible.

*********************************************************************************************************************/

static ERR GRADIENTDISTAL_SET_Radius(extGradientDistal *Self, Unit &Value)
{
   if (Value >= 0) {
      Self->SDFHash = 0;
      Self->Radius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
Floor: Colour ramp floor for distal gradients.

The Floor value is used as the floor for distal gradient colour values.  It has a valid range of `0 &lt;= Floor &lt;
Multiplier`; the constraint against #Multiplier is enforced at initialisation.

*********************************************************************************************************************/

static ERR GRADIENTDISTAL_SET_Floor(extGradientDistal *Self, Unit &Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->Floor = Value;
   Self->SDFHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Multiplier: Colour ramp multiplier for distal gradients.

The Multiplier value acts as a multiplier for distal gradient colour values.  It has a valid range of `.01 &lt;
Multiplier &lt; 10`.

-END-
*********************************************************************************************************************/

static ERR GRADIENTDISTAL_SET_Multiplier(extGradientDistal *Self, Unit &Value)
{
   if ((Value < 0.01) or (Value > 10)) return ERR::OutOfRange;
   Self->Multiplier = Value;
   Self->SDFHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
InnerFall: Selects the alpha fall-off curve for the interior fade.

InnerFall controls the shape of the interior alpha fade that runs from the path outline inward to `InnerRadius`.  The
default is `GFALL::SMOOTHSTEP`.  The fade is only visible when `InnerRadius` is non-zero; with a zero InnerRadius the
interior remains fully opaque and the curve has no effect.

Setting InnerFall to `GFALL::CLEAR` disables the interior entirely, rendering it fully transparent irrespective of
`InnerRadius`.  This is useful for producing an exterior-only halo with no fill inside the path outline.

-FIELD-
OuterFall: Selects the alpha fall-off curve for the exterior fade.

OuterFall controls the shape of the exterior alpha fade that runs from the path outline outward to the padding edge
defined by `Radius`.  The default is `GFALL::SMOOTHSTEP`.  Faster curves such as `GFALL::QUADRATIC` and
`GFALL::CUBIC` produce a tighter halo, while `GFALL::SMOOTHSTEP` holds the inner half bright for a wider glow.

Setting OuterFall to `GFALL::CLEAR` disables the exterior entirely, rendering it fully transparent so that only the
interior half of the gradient is drawn.

-END-
*********************************************************************************************************************/

static ERR GRADIENTDISTAL_SET_InnerFall(extGradientDistal *Self, GFALL Value)
{
   Self->InnerFall = Value;
   Self->SDFHash = 0; // The curve is baked into the cached SDF alpha buffer, so force a rebuild
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

static ERR GRADIENTDISTAL_SET_OuterFall(extGradientDistal *Self, GFALL Value)
{
   Self->OuterFall = Value;
   Self->SDFHash = 0; // The curve is baked into the cached SDF alpha buffer, so force a rebuild
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_distal_def.cpp"

static const FieldArray clGradientDistalFields[] = {
   { "Floor",       FDF_UNIT|FDF_RW, nullptr, GRADIENTDISTAL_SET_Floor },
   { "Multiplier",  FDF_UNIT|FDF_RW, nullptr, GRADIENTDISTAL_SET_Multiplier },
   { "Radius",      FDF_UNIT|FDF_RW, nullptr, GRADIENTDISTAL_SET_Radius },
   { "InnerRadius", FDF_UNIT|FDF_RW, nullptr, GRADIENTDISTAL_SET_InnerRadius },
   { "InnerFall",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENTDISTAL_SET_InnerFall, &clGradientDistalGFALL },
   { "OuterFall",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, GRADIENTDISTAL_SET_OuterFall, &clGradientDistalGFALL },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_distal(void)
{
   clGradientDistal = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTDISTAL),
      fl::Name("GradientDistal"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientDistalActions),
      fl::Fields(clGradientDistalFields),
      fl::Size(sizeof(extGradientDistal)),
      fl::Path(MOD_PATH));

   return clGradientDistal ? ERR::Okay : ERR::AddClass;
}
