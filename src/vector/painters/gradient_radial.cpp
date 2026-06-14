/*********************************************************************************************************************

-CLASS-
GradientRadial: Radial colour gradient paint server.

GradientRadial draws a colour ramp from a centre point to a radius.  Optional focal coordinates and a focal radius
support displaced radial gradients.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
CenterX: The horizontal centre point of the gradient.

The `(CenterX, CenterY)` coordinates define the centre point of the radial gradient.  By default, the centre point
is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_CenterX(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->CenterX);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_CenterX(extGradientRadial *Self, Unit &Value)
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

The `(CenterX, CenterY)` coordinates define the centre point of the radial gradient.  By default, the centre point
is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_CenterY(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->CenterY);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_CenterY(extGradientRadial *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CY) & (~VGF::FIXED_CY);
   else Self->Flags = (Self->Flags | VGF::FIXED_CY) & (~VGF::SCALED_CY);
   Self->CenterY = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
ContainFocal: Confines the focal point to the base radius.

When enabled, the focal point is constrained so that it remains within the base radial gradient circle.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_ContainFocal(extGradientRadial *Self, int *Value)
{
   *Value = Self->ContainFocal;
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_ContainFocal(extGradientRadial *Self, int Value)
{
   Self->ContainFocal = Value ? 1 : 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Flags: Dimension flags for radial gradient fields.

Dimension flags indicate whether radial coordinate fields are fixed or scaled.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_Flags(extGradientRadial *Self, VGF *Value)
{
   *Value = Self->Flags;
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_Flags(extGradientRadial *Self, VGF Value)
{
   Self->Flags = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
FocalRadius: The size of the focal radius for radial gradients.

If a radial gradient has a defined focal point by setting #FocalX and #FocalY, the FocalRadius can adjust the size of
the focal area.  The default of zero ensures that the focal area matches that defined by #Radius.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_FocalRadius(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->FocalRadius);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_FocalRadius(extGradientRadial *Self, Unit &Value)
{
   if (Value >= 0) {
      if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FOCAL_RADIUS) & (~VGF::FIXED_FOCAL_RADIUS);
      else Self->Flags = (Self->Flags | VGF::FIXED_FOCAL_RADIUS) & (~VGF::SCALED_FOCAL_RADIUS);
      Self->FocalRadius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************
-FIELD-
FocalX: The horizontal focal point for radial gradients.

The `(FocalX, FocalY)` coordinates define the focal point for radial gradients.  If left undefined, the focal point
will match the centre of the gradient.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_FocalX(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->FocalX);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_FocalX(extGradientRadial *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FX) & (~VGF::FIXED_FX);
   else Self->Flags = (Self->Flags | VGF::FIXED_FX) & (~VGF::SCALED_FX);
   Self->FocalX = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
FocalY: The vertical focal point for radial gradients.

The `(FocalX, FocalY)` coordinates define the focal point for radial gradients.  If left undefined, the focal point
will match the centre of the gradient.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_FocalY(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->FocalY);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_FocalY(extGradientRadial *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FY) & (~VGF::FIXED_FY);
   else Self->Flags = (Self->Flags | VGF::FIXED_FY) & (~VGF::SCALED_FY);
   Self->FocalY = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Radius: The radius of the gradient.

The radius of the gradient can be defined as a fixed unit or scaled relative to its container.  A default radius of
50% applies if this field is not set.

-END-
*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_Radius(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->Radius);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_Radius(extGradientRadial *Self, Unit &Value)
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

#include "gradient_radial_def.cpp"

static const struct FieldDef clGradientRadialFlags[] = {
   { "ScaledCX", 0x00000010 },
   { "ScaledCY", 0x00000020 },
   { "ScaledFX", 0x00000040 },
   { "ScaledFY", 0x00000080 },
   { "ScaledRadius", 0x00000100 },
   { "ScaledFocalRadius", 0x00000200 },
   { "FixedCX", 0x00004000 },
   { "FixedCY", 0x00008000 },
   { "FixedFX", 0x00010000 },
   { "FixedFY", 0x00020000 },
   { "FixedRadius", 0x00040000 },
   { "FixedFocalRadius", 0x00080000 },
   { "ContainFocal", 0x00100000 },
   { nullptr, 0 }
};

static const FieldArray clGradientRadialFields[] = {
   { "CenterX",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_CenterX, GRADIENTRADIAL_SET_CenterX },
   { "CenterY",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_CenterY, GRADIENTRADIAL_SET_CenterY },
   { "Flags",        FDF_VIRTUAL|FDF_INTFLAGS|FDF_RW, GRADIENTRADIAL_GET_Flags, GRADIENTRADIAL_SET_Flags, &clGradientRadialFlags },
   { "FocalX",       FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FocalX, GRADIENTRADIAL_SET_FocalX },
   { "FocalY",       FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FocalY, GRADIENTRADIAL_SET_FocalY },
   { "Radius",       FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_Radius, GRADIENTRADIAL_SET_Radius },
   { "FocalRadius",  FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FocalRadius, GRADIENTRADIAL_SET_FocalRadius },
   { "ContainFocal", FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_ContainFocal, GRADIENTRADIAL_SET_ContainFocal },
   { "CX",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_CenterX, GRADIENTRADIAL_SET_CenterX },
   { "CY",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_CenterY, GRADIENTRADIAL_SET_CenterY },
   { "FX",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FocalX, GRADIENTRADIAL_SET_FocalX },
   { "FY",           FDF_VIRTUAL|FDF_SYNONYM|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FocalY, GRADIENTRADIAL_SET_FocalY },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_radial(void)
{
   clGradientRadial = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTRADIAL),
      fl::Name("GradientRadial"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientRadialActions),
      fl::Fields(clGradientRadialFields),
      fl::Size(sizeof(extGradientRadial)),
      fl::Path(MOD_PATH));

   return clGradientRadial ? ERR::Okay : ERR::AddClass;
}
