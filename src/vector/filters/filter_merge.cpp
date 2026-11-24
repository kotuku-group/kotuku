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

   extMergeFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::IGNORE;
   }
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR MERGEFX_Draw(extMergeFX *Self, struct acDraw *Args)
{
   auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;

   BAF copy_flags = (effect_colour_space(Self) IS VCS::LINEAR_RGB) ? BAF::LINEAR : BAF::NIL;

   for (size_t i = 0; i < step->Inputs.size(); i++) {
      const objBitmap *bmp;
      auto error = get_source_bitmap(Self, &bmp, filter_input_role::MERGE, i);
      if ((error != ERR::Okay) and (error != ERR::Continue)) return error;
      if (!bmp) continue;

      if (error = copy_filter_bitmap(bmp, Self->Target, copy_flags, 0, 0, bmp->Width, bmp->Height, 0, 0);
          error != ERR::Okay) return error;

      copy_flags |= BAF::BLEND|BAF::COPY; // Any subsequent copies are to be blended
   }

   return ERR::Okay;
}

//********************************************************************************************************************

/*********************************************************************************************************************

-FIELD-
SourceList: A list of source types to be processed in the merge.

The list of sources is built as a simple array of MergeSource structures.

Input sources are defined by the SourceType field value.  A `REFERENCE` entry needs a pointer to an earlier
effect in the same filter before the pipeline can render.  If a referenced effect is removed, that entry retains
its `REFERENCE` type and its pointer is cleared.  Replace the list to repair the pipeline.

*********************************************************************************************************************/

static ERR MERGEFX_SET_SourceList(extMergeFX *Self, std::span<const MergeSource> &Value)
{
   std::vector<MergeSource> list;
   list.reserve(Value.size());

   for (size_t i = 0; i < Value.size(); i++) {
      auto source = Value[i];
      if ((source.SourceType IS VSF::REFERENCE) and source.Effect) {
         if (auto error = check_filter_reference(Self, source.Effect, filter_input_role::MERGE, i);
             error != ERR::Okay) return error;
      }
      else source.Effect = nullptr;

      list.push_back(source);
   }

   Self->List = std::move(list);
   if (Self->Filter) Self->Filter->GraphRevision++;
   return ERR::Okay;
}

static ERR MERGEFX_GET_SourceList(extMergeFX *Self, std::span<MergeSource> &Value)
{
   Value = std::span<MergeSource>(Self->List.data(), Self->List.size());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the filter.
-END-

*********************************************************************************************************************/

static ERR MERGEFX_GET_XMLDef(extMergeFX *Self, std::string &Value)
{
   Value = std::string("feMerge");
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_merge_def.c"

static const FieldArray clMergeFXFields[] = {
   { "SourceList", FDF_VIRTUAL|FDF_STRUCT|FDF_ARRAY|FDF_RW|FDF_PURE, MERGEFX_GET_SourceList, MERGEFX_SET_SourceList, "MergeSource" },
   { "XMLDef",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, MERGEFX_GET_XMLDef },
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
