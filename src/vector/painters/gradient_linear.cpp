/*********************************************************************************************************************

-CLASS-
GradientLinear: Linear colour gradient paint server.

GradientLinear draws a colour ramp along the line defined by `(X1, Y1)` and `(X2, Y2)`.  The object must be registered
with a @VectorScene via @VectorScene.AddDef() before it can be referenced by vector fill or stroke attributes.

-END-

*********************************************************************************************************************/

/*********************************************************************************************************************
-FIELD-
X1: Initial X coordinate for the gradient.

The `(X1, Y1)` field values define the starting coordinate for mapping linear gradients.  Coordinate values can be
expressed as units that are scaled to the target space.

*********************************************************************************************************************/

static ERR GRADIENTLINEAR_GET_X1(extGradientLinear *Self, Unit &Value)
{
   Value = Self->X1;
   return ERR::Okay;
}

static ERR GRADIENTLINEAR_SET_X1(extGradientLinear *Self, Unit &Value)
{
   Self->X1 = Value;
   Self->CalcAngle = true;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
X2: Final X coordinate for the gradient.

The `(X2, Y2)` field values define the end coordinate for mapping linear gradients.  Coordinate values can be
expressed as units that are scaled to the target space.

*********************************************************************************************************************/

static ERR GRADIENTLINEAR_GET_X2(extGradientLinear *Self, Unit &Value)
{
   Value = Self->X2;
   return ERR::Okay;
}

static ERR GRADIENTLINEAR_SET_X2(extGradientLinear *Self, Unit &Value)
{
   Self->X2 = Value;
   Self->CalcAngle = true;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y1: Initial Y coordinate for the gradient.

The `(X1, Y1)` field values define the starting coordinate for mapping linear gradients.  Coordinate values can be
expressed as units that are scaled to the target space.

*********************************************************************************************************************/

static ERR GRADIENTLINEAR_GET_Y1(extGradientLinear *Self, Unit &Value)
{
   Value = Self->Y1;
   return ERR::Okay;
}

static ERR GRADIENTLINEAR_SET_Y1(extGradientLinear *Self, Unit &Value)
{
   Self->Y1 = Value;
   Self->CalcAngle = true;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y2: Final Y coordinate for the gradient.

The `(X2, Y2)` field values define the end coordinate for mapping linear gradients.  Coordinate values can be
expressed as units that are scaled to the target space.

-END-
*********************************************************************************************************************/

static ERR GRADIENTLINEAR_GET_Y2(extGradientLinear *Self, Unit &Value)
{
   Value = Self->Y2;
   return ERR::Okay;
}

static ERR GRADIENTLINEAR_SET_Y2(extGradientLinear *Self, Unit &Value)
{
   Self->Y2 = Value;
   Self->CalcAngle = true;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "gradient_linear_def.cpp"

static const FieldArray clGradientLinearFields[] = {
   { "X1", FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, GRADIENTLINEAR_GET_X1, GRADIENTLINEAR_SET_X1 },
   { "Y1", FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, GRADIENTLINEAR_GET_Y1, GRADIENTLINEAR_SET_Y1 },
   { "X2", FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, GRADIENTLINEAR_GET_X2, GRADIENTLINEAR_SET_X2 },
   { "Y2", FDF_VIRTUAL|FDF_UNIT|FDF_RW|FDF_PURE, GRADIENTLINEAR_GET_Y2, GRADIENTLINEAR_SET_Y2 },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_gradient_linear(void)
{
   clGradientLinear = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::GRADIENT),
      fl::ClassID(CLASSID::GRADIENTLINEAR),
      fl::Name("GradientLinear"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clGradientLinearActions),
      fl::Fields(clGradientLinearFields),
      fl::Size(sizeof(extGradientLinear)),
      fl::Path(MOD_PATH));

   return clGradientLinear ? ERR::Okay : ERR::AddClass;
}
