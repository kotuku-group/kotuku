/*********************************************************************************************************************

-CLASS-
GradientDistal: Signed distance field colour gradient paint server.

GradientDistal shades inward and outward from the target vector path outline.  The path outline is pinned to the
centre of the colour ramp, `Radius` controls exterior padding, and `InnerRadius` controls interior fade-out.

-END-

*********************************************************************************************************************/

static ERR GRADIENTDISTAL_Init(extGradientDistal *Self)
{
   if (Self->Units IS VUNIT::USERSPACE) {
      kt::Log().warning("Distal gradients are not compatible with Units.USERSPACE.");
      Self->Units = VUNIT::BOUNDING_BOX;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
InnerRadius: The size of the inner radius for distal gradients.

The InnerRadius defines the extent of the interior fade-out for distal gradients.  If zero, the interior of the
target shape is fully rendered.

*********************************************************************************************************************/

static ERR GRADIENTDISTAL_GET_InnerRadius(extGradientDistal *Self, Unit *Value)
{
   Value->set(Self->InnerRadius);
   return ERR::Okay;
}

static ERR GRADIENTDISTAL_SET_InnerRadius(extGradientDistal *Self, Unit &Value)
{
   if (Value >= 0) {
      if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_FOCAL_RADIUS) & (~VGF::FIXED_FOCAL_RADIUS);
      else Self->Flags = (Self->Flags | VGF::FIXED_FOCAL_RADIUS) & (~VGF::SCALED_FOCAL_RADIUS);
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

static ERR GRADIENTDISTAL_GET_Radius(extGradientDistal *Self, Unit *Value)
{
   Value->set(Self->Radius);
   return ERR::Okay;
}

static ERR GRADIENTDISTAL_SET_Radius(extGradientDistal *Self, Unit &Value)
{
   if (Value >= 0) {
      if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_RADIUS) & (~VGF::FIXED_RADIUS);
      else Self->Flags = (Self->Flags | VGF::FIXED_RADIUS) & (~VGF::SCALED_RADIUS);
      Self->SDFHash = 0;
      Self->Radius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************
-FIELD-
X1: Colour ramp floor for distal gradients.

The X1 value is used as the floor for distal gradient colour values.

*********************************************************************************************************************/

static ERR GRADIENTDISTAL_GET_X1(extGradientDistal *Self, Unit *Value)
{
   Value->set(Self->X1);
   return ERR::Okay;
}

static ERR GRADIENTDISTAL_SET_X1(extGradientDistal *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X1) & (~VGF::FIXED_X1);
   else Self->Flags = (Self->Flags | VGF::FIXED_X1) & (~VGF::SCALED_X1);
   Self->X1 = Value;
   Self->SDFHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X2: Colour ramp multiplier for distal gradients.

The X2 value acts as a multiplier for distal gradient colour values.

-END-
*********************************************************************************************************************/

static ERR GRADIENTDISTAL_GET_X2(extGradientDistal *Self, Unit *Value)
{
   Value->set(Self->X2);
   return ERR::Okay;
}

static ERR GRADIENTDISTAL_SET_X2(extGradientDistal *Self, Unit &Value)
{
   if (Value.scaled()) Self->Flags = (Self->Flags | VGF::SCALED_X2) & (~VGF::FIXED_X2);
   else Self->Flags = (Self->Flags | VGF::FIXED_X2) & (~VGF::SCALED_X2);
   Self->X2 = Value;
   Self->SDFHash = 0;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_distal_def.cpp"

static const FieldArray clGradientDistalFields[] = {
   { "X1",          FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDISTAL_GET_X1, GRADIENTDISTAL_SET_X1 },
   { "X2",          FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDISTAL_GET_X2, GRADIENTDISTAL_SET_X2 },
   { "Radius",      FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDISTAL_GET_Radius, GRADIENTDISTAL_SET_Radius },
   { "InnerRadius", FDF_VIRTUAL|FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW|FDF_PURE, GRADIENTDISTAL_GET_InnerRadius, GRADIENTDISTAL_SET_InnerRadius },
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
