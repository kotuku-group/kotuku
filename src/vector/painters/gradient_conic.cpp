/*********************************************************************************************************************

-CLASS-
GradientConic: Conic colour gradient paint server.

GradientConic draws colour values around a centre point, producing a conic gradient.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
CenterX: The horizontal centre point of the gradient.

The `(CenterX, CenterY)` coordinates define the centre point around which the conic gradient rotates.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTCONIC_GET_CenterX(extGradientConic *Self, Unit *Value)
{
   Value->set(Self->CenterX);
   return ERR::Okay;
}

static ERR GRADIENTCONIC_SET_CenterX(extGradientConic *Self, Unit &Value)
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

The `(CenterX, CenterY)` coordinates define the centre point around which the conic gradient rotates.  By default,
the centre point is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTCONIC_GET_CenterY(extGradientConic *Self, Unit *Value)
{
   Value->set(Self->CenterY);
   return ERR::Okay;
}

static ERR GRADIENTCONIC_SET_CenterY(extGradientConic *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CY) & (~VGF::FIXED_CY);
   else Self->Flags = (Self->Flags | VGF::FIXED_CY) & (~VGF::SCALED_CY);
   Self->CenterY = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Flags: Dimension flags for conic gradient fields.

Dimension flags indicate whether conic coordinate fields are fixed or scaled.

*********************************************************************************************************************/

static ERR GRADIENTCONIC_GET_Flags(extGradientConic *Self, VGF *Value)
{
   *Value = Self->Flags;
   return ERR::Okay;
}

static ERR GRADIENTCONIC_SET_Flags(extGradientConic *Self, VGF Value)
{
   Self->Flags = Value;
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

static const struct FieldDef clGradientConicFlags[] = {
   { "ScaledCX", 0x00000010 },
   { "ScaledCY", 0x00000020 },
   { "ScaledRadius", 0x00000100 },
   { "FixedCX", 0x00004000 },
   { "FixedCY", 0x00008000 },
   { "FixedRadius", 0x00040000 },
   { nullptr, 0 }
};

static const FieldArray clGradientConicFields[] = {
   { "CenterX", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_CenterX, GRADIENTCONIC_SET_CenterX },
   { "CenterY", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_CenterY, GRADIENTCONIC_SET_CenterY },
   { "Flags",   FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, GRADIENTCONIC_GET_Flags, GRADIENTCONIC_SET_Flags, &clGradientConicFlags },
   { "Radius",  FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_Radius, GRADIENTCONIC_SET_Radius },
   { "CX",      FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_CenterX, GRADIENTCONIC_SET_CenterX },
   { "CY",      FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTCONIC_GET_CenterY, GRADIENTCONIC_SET_CenterY },
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
