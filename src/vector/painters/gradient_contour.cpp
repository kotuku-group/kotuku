/*********************************************************************************************************************

-CLASS-
GradientContour: Contour-following colour gradient paint server.

GradientContour follows the contours of the target vector path.  `Floor` controls the colour ramp floor and
`Multiplier` controls the ramp scale.

This gradient operates exclusively in `BOUNDING_BOX` space.  If @Gradient.Units is set to `USERSPACE` it will be
reset to `BOUNDING_BOX` during initialisation.

-END-

*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_Init(extGradientContour *Self)
{
   kt::Log log;

   if (Self->Units IS VUNIT::USERSPACE) {
      log.warning("Contour gradients are not compatible with Units.USERSPACE.");
      Self->Units = VUNIT::BOUNDING_BOX;
   }

   if (Self->Floor >= Self->Multiplier) return log.warning(ERR::OutOfRange);

   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Floor: Colour ramp floor for contour gradients.

The Floor value is used as the floor for contour gradient colour values.  It has a valid range of `0 &lt;= Floor &lt;
Multiplier`; the constraint against #Multiplier is enforced at initialisation.

*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_SET_Floor(extGradientContour *Self, double Value)
{
   if (Value < 0) return ERR::OutOfRange;
   Self->Floor = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Multiplier: Colour ramp multiplier for contour gradients.

The Multiplier value acts as a multiplier for contour gradient colour values.  It has a valid range of `.01 <
Multiplier < 10`.

-END-
*********************************************************************************************************************/

static ERR GRADIENTCONTOUR_SET_Multiplier(extGradientContour *Self, double Value)
{
   if ((Value < 0.01) or (Value > 10)) return ERR::OutOfRange;
   Self->Multiplier = Value;
   if (Self->initialised()) Self->modified();
   return ERR::Okay;
}

static ERR GRADIENTCONTOUR_GET_XMLDef(extGradientContour *Self, std::string_view &Value)
{
   std::stringstream stream;
   stream << "contourGradient";
   gradient_xml_attr(stream, "floor", Self->Floor);
   gradient_xml_attr(stream, "multiplier", Self->Multiplier);
   return gradient_xml_result(stream.str(), Value);
}

//********************************************************************************************************************

#include "gradient_contour_def.cpp"

static const FieldArray clGradientContourFields[] = {
   { "Floor",      FDF_DOUBLE|FDF_RW, nullptr, GRADIENTCONTOUR_SET_Floor },
   { "Multiplier", FDF_DOUBLE|FDF_RW, nullptr, GRADIENTCONTOUR_SET_Multiplier },
   { "XMLDef",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R, GRADIENTCONTOUR_GET_XMLDef },
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
