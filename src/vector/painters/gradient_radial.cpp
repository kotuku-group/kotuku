/*********************************************************************************************************************

-CLASS-
GradientRadial: Radial colour gradient paint server.

GradientRadial draws a colour ramp from a centre point to a radius.  Optional focal coordinates and a focal radius
support displaced radial gradients.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
CX: The horizontal centre point of the gradient.

The `(CX, CY)` coordinates define the centre point of the radial gradient.  By default, the centre point
is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_CX(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->CX);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_CX(extGradientRadial *Self, Unit &Value)
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

The `(CX, CY)` coordinates define the centre point of the radial gradient.  By default, the centre point
is set to `50%`.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_CY(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->CY);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_CY(extGradientRadial *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_CY) & (~VGF::FIXED_CY);
   else Self->Flags = (Self->Flags | VGF::FIXED_CY) & (~VGF::SCALED_CY);
   Self->CY = Value;
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
FocalRadius: The size of the focal radius for radial gradients.

If a radial gradient has a defined focal point by setting #FX and #FY, the FocalRadius can adjust the size of
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
FX: The horizontal focal point for radial gradients.

The `(FX, FY)` coordinates define the focal point for radial gradients.  If left undefined, the focal point
will match the centre of the gradient.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_FX(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->FX);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_FX(extGradientRadial *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FX) & (~VGF::FIXED_FX);
   else Self->Flags = (Self->Flags | VGF::FIXED_FX) & (~VGF::SCALED_FX);
   Self->FX = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
FY: The vertical focal point for radial gradients.

The `(FX, FY)` coordinates define the focal point for radial gradients.  If left undefined, the focal point
will match the centre of the gradient.

*********************************************************************************************************************/

static ERR GRADIENTRADIAL_GET_FY(extGradientRadial *Self, Unit *Value)
{
   Value->set(Self->FY);
   return ERR::Okay;
}

static ERR GRADIENTRADIAL_SET_FY(extGradientRadial *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FY) & (~VGF::FIXED_FY);
   else Self->Flags = (Self->Flags | VGF::FIXED_FY) & (~VGF::SCALED_FY);
   Self->FY = Value;
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

static const FieldArray clGradientRadialFields[] = {
   { "CX",           FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_CX, GRADIENTRADIAL_SET_CX },
   { "CY",           FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_CY, GRADIENTRADIAL_SET_CY },
   { "FX",           FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FX, GRADIENTRADIAL_SET_FX },
   { "FY",           FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FY, GRADIENTRADIAL_SET_FY },
   { "Radius",       FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_Radius, GRADIENTRADIAL_SET_Radius },
   { "FocalRadius",  FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_FocalRadius, GRADIENTRADIAL_SET_FocalRadius },
   { "ContainFocal", FDF_VIRTUAL|FDF_INT|FDF_RW|FDF_PURE, GRADIENTRADIAL_GET_ContainFocal, GRADIENTRADIAL_SET_ContainFocal },
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
