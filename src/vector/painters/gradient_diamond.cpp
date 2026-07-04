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

static ERR GRADIENTDIAMOND_SET_Radius(extGradientDiamond *Self, Unit &Value)
{
   if (Value >= 0) {
      Self->Radius = Value;
      if (Self->initialised()) Self->modified();
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************
-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-
*********************************************************************************************************************/

static ERR GRADIENTDIAMOND_GET_XMLDef(extGradientDiamond *Self, std::string_view &Value)
{
   std::stringstream stream;
   stream << "diamondGradient";
   gradient_xml_attr(stream, "cx", Self->CX);
   gradient_xml_attr(stream, "cy", Self->CY);
   gradient_xml_attr(stream, "r", Self->Radius);
   return gradient_xml_result(stream.str(), Value);
}

//********************************************************************************************************************

#include "gradient_diamond_def.cpp"

static const FieldArray clGradientDiamondFields[] = {
   { "CX",      FDF_UNIT|FDF_RW, nullptr, GRADIENTDIAMOND_SET_CX },
   { "CY",      FDF_UNIT|FDF_RW, nullptr, GRADIENTDIAMOND_SET_CY },
   { "Radius",  FDF_UNIT|FDF_RW, nullptr, GRADIENTDIAMOND_SET_Radius },
   { "XMLDef",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R, GRADIENTDIAMOND_GET_XMLDef },
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
