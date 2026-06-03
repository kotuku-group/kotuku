/*********************************************************************************************************************

-CLASS-
MergeFX: Combines multiple effects in sequence.

Use MergeFX to composite multiple input sources so that they are rendered on top of each other in a predefined
sequence.

Many effects produce a number of intermediate layers in order to create the final output image.  This filter allows
us to collapse those into a single image.  Although this could be done by using `n-1` Composite-filters, it is more
convenient to have  this common operation available in this form, and offers the implementation some additional
flexibility.

-END-

The canonical implementation of feMerge is to render the entire effect into one RGBA layer, and then render the
resulting layer on the output device. In certain cases (in particular if the output device itself is a continuous
tone device), and since merging is associative, it might be a sufficient approximation to evaluate the effect one
layer at a time and render each layer individually onto the output device bottom to top.

If the topmost image input is SourceGraphic and this ‘feMerge’ is the last filter primitive in the filter, the
implementation is encouraged to render the layers up to that point, and then render the SourceGraphic directly from
its vector description on top.

*********************************************************************************************************************/

class extMergeFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::MERGEFX;
   static constexpr CSTRING CLASS_NAME = "MergeFX";
   using create = kt::Create<extMergeFX>;

   std::vector<MergeSource> List;
};

//********************************************************************************************************************

static void release_merge_sources(std::vector<MergeSource> &List)
{
   for (auto &source : List) {
      if ((source.SourceType IS VSF::REFERENCE) and (source.Effect)) {
         ((extFilterEffect *)source.Effect)->UsageCount--;
      }
   }

   List.clear();
}

//********************************************************************************************************************

static void clear_merge_sources(std::vector<MergeSource> &List)
{
   List.clear();
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR MERGEFX_Draw(extMergeFX *Self, struct acDraw *Args)
{
   BAF copy_flags = (Self->Filter->ColourSpace IS VCS::LINEAR_RGB) ? BAF::LINEAR : BAF::NIL;

   for (auto source : Self->List) {
      objBitmap *bmp = nullptr;
      auto error = get_source_bitmap(Self->Filter, &bmp, source.SourceType, (extFilterEffect *)source.Effect, false);
      if ((error != ERR::Okay) and (error != ERR::Continue)) continue;
      if (!bmp) continue;

      gfx::CopyArea(bmp, Self->Target, copy_flags, 0, 0, bmp->Width, bmp->Height, 0, 0);

      copy_flags |= BAF::BLEND|BAF::COPY; // Any subsequent copies are to be blended
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MERGEFX_Free(extMergeFX *Self)
{
   clear_merge_sources(Self->List);
   Self->~extMergeFX();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MERGEFX_NewPlacement(extMergeFX *Self)
{
   new (Self) extMergeFX;
   Self->SourceType = VSF::IGNORE;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SourceList: A list of source types to be processed in the merge.

The list of sources is built as a simple array of MergeSource structures.

Input sources are defined by the SourceType field value.  In the case of `REFERENCE`, it is necessary to provide a
direct pointer to the referenced effect in the Effect field, or an error will be returned.

*********************************************************************************************************************/

static ERR MERGEFX_SET_SourceList(extMergeFX *Self, MergeSource *Value, int Elements)
{
   if ((!Value) or (Elements <= 0)) {
      release_merge_sources(Self->List);
      return ERR::Okay;
   }

   std::vector<MergeSource> list;
   list.reserve(Elements);

   for (int i=0; i < Elements; i++) {
      if (Value[i].SourceType IS VSF::REFERENCE) {
         if (!Value[i].Effect) return ERR::InvalidData;
      }

      list.push_back(Value[i]);
   }

   release_merge_sources(Self->List);

   for (auto &source : list) {
      if ((source.SourceType IS VSF::REFERENCE) and (source.Effect)) {
         ((extFilterEffect *)source.Effect)->UsageCount++;
      }
   }

   Self->List = std::move(list);
   return ERR::Okay;
}

static ERR MERGEFX_GET_SourceList(extMergeFX *Self, MergeSource **Value, int *Elements)
{
   *Value    = Self->List.data();
   *Elements = int(Self->List.size());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR MERGEFX_GET_XMLDef(extMergeFX *Self, std::string_view &Value)
{
   auto cppstr = std::string("feMerge");
   if (auto str = strclone(cppstr)) {
      Value = std::string_view{str, cppstr.size()};
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

//********************************************************************************************************************

#include "filter_merge_def.c"

static const FieldArray clMergeFXFields[] = {
   { "SourceList", FDF_VIRTUAL|FDF_STRUCT|FDF_ARRAY|FDF_RW|FDF_PURE, MERGEFX_GET_SourceList, MERGEFX_SET_SourceList, "MergeSource" },
   { "XMLDef",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R, MERGEFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_mergefx(void)
{
   clMergeFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::MERGEFX),
      fl::Name("MergeFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clMergeFXActions),
      fl::Fields(clMergeFXFields),
      fl::Size(sizeof(extMergeFX)),
      fl::Path(MOD_PATH));

   return clMergeFX ? ERR::Okay : ERR::AddClass;
}
