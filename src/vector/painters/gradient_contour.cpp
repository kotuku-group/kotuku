/*********************************************************************************************************************

-CLASS-
GradientContour: Contour-following colour gradient paint server.

GradientContour follows the contours of the target vector path.  `Floor` controls the colour ramp floor and
`Multiplier` controls the ramp scale.

-END-

*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_Init(extGradient *Self)
{
   if (Self->Units IS VUNIT::USERSPACE) {
      kt::Log().warning("Contour gradients are not compatible with Units.USERSPACE.");
      Self->Units = VUNIT::BOUNDING_BOX;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Floor: Colour ramp floor for contour gradients.

The Floor value is used as the floor for contour gradient colour values.  It has a valid range of `0 < Floor <
Multiplier`.

*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_GET_Floor(extGradientContour *Self, Unit *Value)
{
   Value->set(Self->Floor);
   return ERR::Okay;
}

static ERR GRADIENTCONTOUR_SET_Floor(extGradientContour *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X1) & (~VGF::FIXED_X1);
   else Self->Flags = (Self->Flags | VGF::FIXED_X1) & (~VGF::SCALED_X1);
   Self->Floor = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Multiplier: Colour ramp multiplier for contour gradients.

The Multiplier value acts as a multiplier for contour gradient colour values.  It has a valid range of `.01 <
Multiplier < 10`.

-END-
*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_GET_Multiplier(extGradientContour *Self, Unit *Value)
{
   Value->set(Self->Multiplier);
   return ERR::Okay;
}

static ERR GRADIENTCONTOUR_SET_Multiplier(extGradientContour *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X2) & (~VGF::FIXED_X2);
   else Self->Flags = (Self->Flags | VGF::FIXED_X2) & (~VGF::SCALED_X2);
   Self->Multiplier = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_contour_def.cpp"

static const FieldArray clGradientContourFields[] = {
   { "Floor", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONTOUR_GET_Floor,
      GRADIENTCONTOUR_SET_Floor },
   { "Multiplier", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE,
      GRADIENTCONTOUR_GET_Multiplier, GRADIENTCONTOUR_SET_Multiplier },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_contour(void)
{
   clGradientContour = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTCONTOUR),
      fl::Name("GradientContour"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientContourActions),
      fl::Fields(clGradientContourFields),
      fl::Size(sizeof(extGradientContour)),
      fl::Path(MOD_PATH));

   return clGradientContour ? ERR::Okay : ERR::AddClass;
}
