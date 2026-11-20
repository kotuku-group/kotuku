/*********************************************************************************************************************

-CLASS-
OffsetFX: A filter effect that offsets the position of an input source.

This filter offsets the input image relative to its current position in the image space by the specified vector
of `(XOffset,YOffset)`.

If the @VectorFilter.PrimitiveUnits of the filter is `BOUNDING_BOX`, the offsets are fractions of the bounding box.
By default the X offset is scaled by the bounding box width and the Y offset by its height, as per the SVG standard.
If the @VectorFilter.AspectRatio is `NONE`, both offsets are scaled by the normalised diagonal of the bounding box,
which is the same formula used by a scaled stroke width.  The direction of the offset is then the same for any shape,
which suits drop shadows.

-END-

*********************************************************************************************************************/

class extOffsetFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::OFFSETFX;
   static constexpr CSTRING CLASS_NAME = "OffsetFX";
   using create = kt::Create<extOffsetFX>;

   double XOffset = 0, YOffset = 0;

   extOffsetFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   // Computes the offset in bitmap pixels.

   std::pair<double, double> pixel_offset() const {
      const auto &render = *Filter->Render;
      const auto [unit_x, unit_y] = render.primitive_scales(true);
      return { std::round((XOffset * unit_x) * render.ToScene.sx),
         std::round((YOffset * unit_y) * render.ToScene.sy) };
   }

   filter_sampling sampling() const {
      const auto [x, y] = pixel_offset();
      return { filter_access::BOUNDED, std::abs(x), std::abs(y) };
   }
};

//********************************************************************************************************************

static ERR OFFSETFX_Draw(extOffsetFX *Self, struct acDraw *Args)
{
   const objBitmap *input;
   if (auto error = get_source_bitmap(Self, &input, filter_input_role::INPUT); error != ERR::Okay) return error;
   const auto [dx, dy] = Self->pixel_offset();
   if ((not std::isfinite(dx)) or (not std::isfinite(dy))) return ERR::InvalidValue;
   if ((std::abs(dx) >= input->Width) or (std::abs(dy) >= input->Height)) return ERR::Okay;
   return copy_filter_bitmap(input, Self->Target, BAF::NIL, 0, 0, input->Width, input->Height, int(dx), int(dy));
}

/*********************************************************************************************************************

-FIELD-
XOffset: The delta X coordinate for the input graphic.

The `(XOffset, YOffset)` field values define the offset of the input source within the target clipping area.  The
value is measured in the units of the filter's @VectorFilter.PrimitiveUnits.

-FIELD-
YOffset: The delta Y coordinate for the input graphic.

The `(XOffset, YOffset)` field values define the offset of the input source within the target clipping area.  The
value is measured in the units of the filter's @VectorFilter.PrimitiveUnits.

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR OFFSETFX_GET_XMLDef(extOffsetFX *Self, std::string &Value)
{
   std::stringstream stream;
   stream << "feOffset dx=\"" << Self->XOffset << "\" dy=\"" << Self->YOffset << "\"";

   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_offset_def.c"

static const FieldArray clOffsetFXFields[] = {
   { "XOffset", FDF_DOUBLE|FDF_RW },
   { "YOffset", FDF_DOUBLE|FDF_RW },
   { "XMLDef",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, OFFSETFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_offsetfx(void)
{
   clOffsetFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::OFFSETFX),
      fl::Name("OffsetFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clOffsetFXActions),
      fl::Fields(clOffsetFXFields),
      fl::Size(sizeof(extOffsetFX)),
      fl::Path(MOD_PATH));

   return clOffsetFX ? ERR::Okay : ERR::AddClass;
}
