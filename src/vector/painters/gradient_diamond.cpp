/*********************************************************************************************************************

-CLASS-
GradientDiamond: Diamond colour gradient paint server.

GradientDiamond draws a square-shaped colour ramp from a centre point.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
CenterX: The horizontal centre point of the gradient.

The `(CenterX, CenterY)` coordinates define the centre point from which the diamond gradient expands.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTDIAMOND_GET_CenterX(extGradientDiamond *Self, Unit *Value)
{
   Value->set(Self->CenterX);
   return ERR::Okay;
}

static ERR GRADIENTDIAMOND_SET_CenterX(extGradientDiamond *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CX) & (~VGF::FIXED_CX);
   else Self->Flags = (Self->Flags | VGF::FIXED_CX) & (~VGF::SCALED_CX);
   Self->CenterX = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
CenterY: The vertical centre point of the gradient.

The `(CenterX, CenterY)` coordinates define the centre point from which the diamond gradient expands.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTDIAMOND_GET_CenterY(extGradientDiamond *Self, Unit *Value)
{
   Value->set(Self->CenterY);
   return ERR::Okay;
}

static ERR GRADIENTDIAMOND_SET_CenterY(extGradientDiamond *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CY) & (~VGF::FIXED_CY);
   else Self->Flags = (Self->Flags | VGF::FIXED_CY) & (~VGF::SCALED_CY);
   Self->CenterY = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Flags: Dimension flags for diamond gradient fields.

Dimension flags indicate whether diamond coordinate fields are fixed or scaled.

*********************************************************************************************************************/

static ERR GRADIENTDIAMOND_GET_Flags(extGradientDiamond *Self, VGF *Value)
{
   *Value = Self->Flags;
   return ERR::Okay;
}

static ERR GRADIENTDIAMOND_SET_Flags(extGradientDiamond *Self, VGF Value)
{
   Self->Flags = Value;
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
   Value->set(Self->Radius);
   return ERR::Okay;
}

static ERR GRADIENTDIAMOND_SET_Radius(extGradientDiamond *Self, Unit &Value)
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

#include "gradient_diamond_def.cpp"

static const struct FieldDef clGradientDiamondFlags[] = {
   { "ScaledCX", 0x00000010 },
   { "ScaledCY", 0x00000020 },
   { "ScaledRadius", 0x00000100 },
   { "FixedCX", 0x00004000 },
   { "FixedCY", 0x00008000 },
   { "FixedRadius", 0x00040000 },
   { nullptr, 0 }
};

static const FieldArray clGradientDiamondFields[] = {
   { "CenterX", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_CenterX, GRADIENTDIAMOND_SET_CenterX },
   { "CenterY", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_CenterY, GRADIENTDIAMOND_SET_CenterY },
   { "Flags",   FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, GRADIENTDIAMOND_GET_Flags, GRADIENTDIAMOND_SET_Flags, &clGradientDiamondFlags },
   { "Radius",  FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_Radius, GRADIENTDIAMOND_SET_Radius },
   { "CX",      FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_CenterX, GRADIENTDIAMOND_SET_CenterX },
   { "CY",      FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDIAMOND_GET_CenterY, GRADIENTDIAMOND_SET_CenterY },
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
