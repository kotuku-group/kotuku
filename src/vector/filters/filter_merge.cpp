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

   std::vector<MergeLayer> Layers;
   std::vector<MergeSource> SourceList;

   extMergeFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::IGNORE;
   }
};

// All filter bitmaps share the same RGBA layout. Keep the alpha weights unrounded until the pixel is written.

template<bool Linear>
static void merge_weighted_layer(const objBitmap &Source, objBitmap &Target, double Opacity)
{
   const int a = Target.ColourFormat->AlphaPos >> 3;
   const int r = Target.ColourFormat->RedPos >> 3;
   const int g = Target.ColourFormat->GreenPos >> 3;
   const int b = Target.ColourFormat->BluePos >> 3;
   const int left = std::max(Source.Clip.Left, Target.Clip.Left);
   const int top = std::max(Source.Clip.Top, Target.Clip.Top);
   const int right = std::min(Source.Clip.Right, Target.Clip.Right);
   const int bottom = std::min(Source.Clip.Bottom, Target.Clip.Bottom);
   if ((left >= right) or (top >= bottom)) return;

   for (int y = top; y < bottom; y++) {
      auto source = Source.Data + std::ptrdiff_t(y) * Source.LineWidth + left * 4;
      auto dest = Target.Data + std::ptrdiff_t(y) * Target.LineWidth + left * 4;
      for (int x = left; x < right; x++, source += 4, dest += 4) {
         const double source_alpha = source[a] * Opacity;
         if (source_alpha IS 0) continue;
         const double dest_alpha = dest[a] * (1.0 - source_alpha / 255.0);
         const double out_alpha = source_alpha + dest_alpha;
         const auto alpha = uint8_t(std::lround(out_alpha));
         if (not alpha) continue;

         auto blend = [&](int Channel) {
            const double source_colour = composite_decode<Linear>(source[Channel]) * source_alpha;
            const double dest_colour = composite_decode<Linear>(dest[Channel]) * dest_alpha;
            return composite_encode<Linear>(uint16_t(std::lround((source_colour + dest_colour) / out_alpha)));
         };
         dest[r] = blend(r);
         dest[g] = blend(g);
         dest[b] = blend(b);
         dest[a] = alpha;
      }
   }
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR MERGEFX_Draw(extMergeFX *Self, struct acDraw *Args)
{
   auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;

   const bool linear = effect_colour_space(Self) IS VCS::LINEAR_RGB;
   BAF copy_flags = linear ? BAF::LINEAR : BAF::NIL;

   for (size_t i = 0; i < step->Inputs.size(); i++) {
      const objBitmap *bmp;
      auto error = get_source_bitmap(Self, &bmp, filter_input_role::MERGE, i);
      if ((error != ERR::Okay) and (error != ERR::Continue)) return error;
      if (!bmp) continue;

      const auto opacity = Self->Layers[i].Opacity;
      if (opacity IS 1.0) {
         if (error = copy_filter_bitmap(bmp, Self->Target, copy_flags, 0, 0, bmp->Width, bmp->Height, 0, 0);
             error != ERR::Okay) return error;
      }
      else if (linear) merge_weighted_layer<true>(*bmp, *Self->Target, opacity);
      else merge_weighted_layer<false>(*bmp, *Self->Target, opacity);

      copy_flags |= BAF::BLEND|BAF::COPY; // Any subsequent copies are to be blended
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Layers: An ordered list of weighted merge sources, from bottom to top.

The list is an array of MergeLayer structures.  Each layer is composited over the result of the layers before it, so
the first entry is the bottom of the stack.  Assigning an array replaces the entire list.  The new list is validated
before it is applied, so the existing list is retained if an error is returned.

!MergeLayer

A `REFERENCE` layer requires the EffectID of an earlier effect in the same filter.  The EffectID is reset to zero for
all other source types.  A layer with a SourceType of `NONE` contributes nothing to the result.  The Opacity scales
the alpha channel of the source before it is composited, and must be in the range 0 - 1.  Enabled must be 1 to render
the layer or 0 to skip it.

Disabled layers and layers with an Opacity of 0 retain their configuration but are not rendered and do not read their
source.  This allows layers to be toggled without rebuilding the list.

If a referenced effect is removed from the filter, the EffectID of its layer is reset to zero and the SourceType
remains `REFERENCE`.  Assign a new list to repair the pipeline.  Reading #SourceList returns the same sources
without the Opacity and Enabled controls.

In SVG documents, each `feMergeNode` defines a layer.  The `kotuku:opacity` attribute sets the Opacity and
`kotuku:enabled="false"` disables the layer.

-ERRORS-
Okay:
InvalidValue: An Opacity or Enabled value is out of range, or a referenced effect does not belong to the same filter.
DoesNotExist: The EffectID of a `REFERENCE` layer does not refer to an existing object.
Recursion: A layer references the MergeFX itself.
-END-

*********************************************************************************************************************/

static ERR MERGEFX_GET_Layers(extMergeFX *Self, std::span<MergeLayer> &Value)
{
   Value = Self->Layers;
   return ERR::Okay;
}

static void replace_merge_layers(extMergeFX *Self, std::vector<MergeLayer> &&Layers,
   std::vector<MergeSource> &&Sources)
{
   Self->Layers = std::move(Layers);
   Self->SourceList = std::move(Sources);
   if (Self->Filter) Self->Filter->GraphRevision++;
   effect_modified(Self);
}

static ERR MERGEFX_SET_Layers(extMergeFX *Self, std::span<const MergeLayer> &Value)
{
   std::vector<MergeLayer> layers;
   std::vector<MergeSource> sources;
   layers.reserve(Value.size());
   sources.reserve(Value.size());

   for (size_t i = 0; i < Value.size(); i++) {
      auto layer = Value[i];

      if ((not std::isfinite(layer.Opacity)) or (layer.Opacity < 0) or (layer.Opacity > 1) or
          ((layer.Enabled != 0) and (layer.Enabled != 1))) return ERR::InvalidValue;

      objFilterEffect *effect = nullptr;

      if (layer.SourceType IS VSF::REFERENCE) {
         if (layer.EffectID) {
            effect = (objFilterEffect *)GetObjectPtr(layer.EffectID);
            if (not effect) return ERR::DoesNotExist;
            if (auto error = check_filter_reference(Self, effect, filter_input_role::MERGE, i);
                error != ERR::Okay) return error;
         }
         else if (layer.Enabled and (layer.Opacity > 0)) return ERR::DoesNotExist;
      }
      else layer.EffectID = 0;

      layers.push_back(layer);
      sources.emplace_back(layer.SourceType, effect);
   }

   replace_merge_layers(Self, std::move(layers), std::move(sources));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SourceList: A list of source types to be processed in the merge.

The list is an array of MergeSource structures. Reading it returns the bindings from #Layers in order.
Assigning it replaces #Layers with enabled entries at opacity one.

Input sources are defined by the SourceType field value.  A `REFERENCE` entry needs a pointer to an earlier
effect in the same filter before the pipeline can render.  If a referenced effect is removed, that entry retains
its `REFERENCE` type and its pointer is cleared.  Replace the list to repair the pipeline.

*********************************************************************************************************************/

static ERR MERGEFX_GET_SourceList(extMergeFX *Self, std::span<MergeSource> &Value)
{
   Value = Self->SourceList;
   return ERR::Okay;
}

static ERR MERGEFX_SET_SourceList(extMergeFX *Self, std::span<const MergeSource> &Value)
{
   std::vector<MergeLayer> layers;
   std::vector<MergeSource> sources;
   layers.reserve(Value.size());
   sources.reserve(Value.size());

   for (size_t i = 0; i < Value.size(); i++) {
      const auto &source = Value[i];
      OBJECTID id = 0;
      if ((source.SourceType IS VSF::REFERENCE) and source.Effect) {
         if (auto error = check_filter_reference(Self, source.Effect, filter_input_role::MERGE, i);
             error != ERR::Okay) return error;
         id = source.Effect->UID;
      }

      layers.push_back({ source.SourceType, id, 1.0, 1 });
      sources.emplace_back(source.SourceType, id ? source.Effect : nullptr);
   }

   replace_merge_layers(Self, std::move(layers), std::move(sources));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

#include "filter_merge_def.c"

static const FieldArray clMergeFXFields[] = {
   { "SourceList", FDF_VIRTUAL|FDF_STRUCT|FDF_ARRAY|FDF_RW|FDF_PURE, MERGEFX_GET_SourceList, MERGEFX_SET_SourceList, "MergeSource" },
   { "Layers",     FDF_VIRTUAL|FDF_STRUCT|FDF_ARRAY|FDF_RW|FDF_PURE, MERGEFX_GET_Layers, MERGEFX_SET_Layers, "MergeLayer" },
   { "XMLDef",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
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
