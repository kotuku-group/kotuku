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

   void pixel_offset(int &DX, int &DY) const {
      double dx = XOffset, dy = YOffset;
      if (Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
         if (Filter->AspectRatio IS VFA::NONE) {
            double diag = dist(0, 0, Filter->BoundWidth, Filter->BoundHeight) * INV_SQRT2;
            dx *= diag;
            dy *= diag;
         }
         else {
            dx *= Filter->BoundWidth;
            dy *= Filter->BoundHeight;
         }
      }

      DX = int(std::lround(dx * Filter->ClientVector->Transform.sx));
      DY = int(std::lround(dy * Filter->ClientVector->Transform.sy));
   }
};

//********************************************************************************************************************

static ERR OFFSETFX_Draw(extOffsetFX *Self, struct acDraw *Args)
{
   objBitmap *inBmp;
   if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
      int dx, dy;
      Self->pixel_offset(dx, dy);
      gfx::CopyArea(inBmp, Self->Target, BAF::NIL, 0, 0, inBmp->Width, inBmp->Height, dx, dy);
      return ERR::Okay;
   }
   else return ERR::NoData;
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
