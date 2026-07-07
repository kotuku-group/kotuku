/*********************************************************************************************************************

-CLASS-
OffsetFX: A filter effect that offsets the position of an input source.

This filter offsets the input image relative to its current position in the image space by the specified vector
of `(XOffset,YOffset)`.

-END-

*********************************************************************************************************************/

class extOffsetFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::OFFSETFX;
   static constexpr CSTRING CLASS_NAME = "OffsetFX";
   using create = kt::Create<extOffsetFX>;

   int XOffset = 0, YOffset = 0;

   extOffsetFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }
};

//********************************************************************************************************************

static ERR OFFSETFX_Draw(extOffsetFX *Self, struct acDraw *Args)
{
   objBitmap *inBmp;
   int dx = int((double)Self->XOffset * Self->Filter->ClientVector->Transform.sx);
   int dy = int((double)Self->YOffset * Self->Filter->ClientVector->Transform.sy);
   if (!get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false)) {
      gfx::CopyArea(inBmp, Self->Target, BAF::NIL, 0, 0, inBmp->Width, inBmp->Height, dx, dy);
      return ERR::Okay;
   }
   else return ERR::NoData;
}

/*********************************************************************************************************************

-FIELD-
XOffset: The delta X coordinate for the input graphic.

The `(XOffset, YOffset)` field values define the offset of the input source within the target clipping area.

-FIELD-
YOffset: The delta Y coordinate for the input graphic.

The `(XOffset, YOffset)` field values define the offset of the input source within the target clipping area.

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
   { "XOffset", FDF_INT|FDF_RW },
   { "YOffset", FDF_INT|FDF_RW },
   { "XMLDef",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R|FDF_PURE, OFFSETFX_GET_XMLDef },
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
