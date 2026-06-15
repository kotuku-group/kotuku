/*********************************************************************************************************************

-CLASS-
GradientDiamond: Diamond colour gradient paint server.

GradientDiamond draws a square-shaped colour ramp from a centre point.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
CX: The horizontal centre point of the gradient.

The `(CX, CY)` coordinates define the centre point from which the diamond gradient expands.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTDIAMOND_GET_CX(extGradientDiamond *Self, Unit *Value)
{
   *Value = Self->CX;
   return ERR::Okay;
}

static ERR GRADIENTDIAMOND_SET_CX(extGradientDiamond *Self, Unit &Value)
{
   Self->CX = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CY: The vertical centre point of the gradient.

The `(CX, CY)` coordinates define the centre point from which the diamond gradient expands.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTDIAMOND_GET_CY(extGradientDiamond *Self, Unit *Value)
{
   *Value = Self->CY;
   return ERR::Okay;
}

static ERR GRADIENTDIAMOND_SET_CY(extGradientDiamond *Self, Unit &Value)
{
   Self->CY = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Radius: The radius of the gradient.

The radius of the diamond gradient can be defined as a fixed unit or scaled relative to its container.  A default
radius of 50% applies if this field is not set.

-END-
*********************************************************************************************************************/

static ERR GRADIENTDIAMOND_GET_Radius(extGradientDiamond *Self, Unit *Value)
{
   *Value = Self->Radius;
   return ERR::Okay;
}

static ERR GRADIENTDIAMOND_SET_Radius(extGradientDiamond *Self, Unit &Value)
{
   if (Value >= 0) {
      Self->Radius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

//********************************************************************************************************************

#include "gradient_diamond_def.cpp"

static const FieldArray clGradientDiamondFields[] = {
   { "CX",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_CX, GRADIENTDIAMOND_SET_CX },
   { "CY",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_CY, GRADIENTDIAMOND_SET_CY },
   { "Radius",  FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_Radius, GRADIENTDIAMOND_SET_Radius },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_diamond(void)
{
   clGradientDiamond = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTDIAMOND),
      fl::Name("GradientDiamond"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientDiamondActions),
      fl::Fields(clGradientDiamondFields),
      fl::Size(sizeof(extGradientDiamond)),
      fl::Path(MOD_PATH));

   return clGradientDiamond ? ERR::Okay : ERR::AddClass;
}
