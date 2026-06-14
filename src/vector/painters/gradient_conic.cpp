/*********************************************************************************************************************

-CLASS-
GradientConic: Conic colour gradient paint server.

GradientConic draws colour values around a centre point, producing a conic gradient.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
CX: The horizontal centre point of the gradient.

The `(CX, CY)` coordinates define the centre point around which the conic gradient rotates.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTCONIC_GET_CX(extGradientConic *Self, Unit *Value)
{
   Value->set(Self->CX);
   return ERR::Okay;
}

static ERR GRADIENTCONIC_SET_CX(extGradientConic *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CX) & (~VGF::FIXED_CX);
   else Self->Flags = (Self->Flags | VGF::FIXED_CX) & (~VGF::SCALED_CX);
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

static ERR GRADIENTCONIC_GET_CY(extGradientConic *Self, Unit *Value)
{
   Value->set(Self->CY);
   return ERR::Okay;
}

static ERR GRADIENTCONIC_SET_CY(extGradientConic *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CY) & (~VGF::FIXED_CY);
   else Self->Flags = (Self->Flags | VGF::FIXED_CY) & (~VGF::SCALED_CY);
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

static ERR GRADIENTCONIC_GET_Radius(extGradientConic *Self, Unit *Value)
{
   Value->set(Self->Radius);
   return ERR::Okay;
}

static ERR GRADIENTCONIC_SET_Radius(extGradientConic *Self, Unit &Value)
{
   if (Value >= 0) {
      if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_RADIUS) & (~VGF::FIXED_RADIUS);
      else Self->Flags = (Self->Flags | VGF::FIXED_RADIUS) & (~VGF::SCALED_RADIUS);
      Self->Radius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

//********************************************************************************************************************

#include "gradient_conic_def.cpp"

static const FieldArray clGradientConicFields[] = {
   { "Radius",  FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_Radius, GRADIENTCONIC_SET_Radius },
   { "CX",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_CX, GRADIENTCONIC_SET_CX },
   { "CY",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_CY, GRADIENTCONIC_SET_CY },
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
