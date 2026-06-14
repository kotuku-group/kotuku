/*********************************************************************************************************************

-CLASS-
GradientContour: Contour-following colour gradient paint server.

GradientContour follows the contours of the target vector path.  `X1` controls the colour ramp floor and `X2`
controls the ramp multiplier.

-END-

*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_Init(extGradient *Self)
{
   kt::Log log;

   if (Self->Units IS VUNIT::USERSPACE) {
      log.warning("Contour gradients are not compatible with Units.USERSPACE.");
      Self->Units = VUNIT::BOUNDING_BOX;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X1: Colour ramp floor for contour gradients.

The X1 value is used as the floor for contour gradient colour values.  It has a valid range of `0 < X1 < X2`.

*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_GET_X1(extGradientContour *Self, Unit *Value)
{
   Value->set(Self->X1);
   return ERR::Okay;
}

static ERR GRADIENTCONTOUR_SET_X1(extGradientContour *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X1) & (~VGF::FIXED_X1);
   else Self->Flags = (Self->Flags | VGF::FIXED_X1) & (~VGF::SCALED_X1);
   Self->X1 = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X2: Colour ramp multiplier for contour gradients.

The X2 value acts as a multiplier for contour gradient colour values.  It has a valid range of `.01 < X2 < 10`.

-END-
*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_GET_X2(extGradientContour *Self, Unit *Value)
{
   Value->set(Self->X2);
   return ERR::Okay;
}

static ERR GRADIENTCONTOUR_SET_X2(extGradientContour *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X2) & (~VGF::FIXED_X2);
   else Self->Flags = (Self->Flags | VGF::FIXED_X2) & (~VGF::SCALED_X2);
   Self->X2 = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_contour_def.cpp"

static const FieldArray clGradientContourFields[] = {
   { "X1", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONTOUR_GET_X1, GRADIENTCONTOUR_SET_X1 },
   { "X2", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONTOUR_GET_X2, GRADIENTCONTOUR_SET_X2 },
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
