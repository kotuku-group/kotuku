/*********************************************************************************************************************

-CLASS-
FilterEffect: FilterEffect is a support class for managing effects hosted by the VectorFilter class.

The FilterEffect class provides base-class functionality for effect classes.  FilterEffect objects must not be
instantiated directly by the client.

The documented fields and actions here are integral to all effects that utilise this class.

-END-

*********************************************************************************************************************/

static ERR FILTEREFFECT_Init(extFilterEffect *Self)
{
   kt::Log log;

   if ((not Self->Filter) or (Self->Owner != Self->Filter)) return log.warning(ERR::UnsupportedOwner);

   // Resolve the implicit input once, using the current authored order.
   if (Self->SourceType IS VSF::PREVIOUS) {
      const auto &effects = Self->Filter->Effects;
      auto position = std::ranges::find(effects, Self);
      if (position IS effects.end()) return log.warning(ERR::InvalidState);
      if (position != effects.begin()) {
         Self->Input = *std::prev(position);
         Self->SourceType = VSF::REFERENCE;
      }
      else Self->SourceType = VSF::GRAPHIC;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
MoveToBack: Move an effect to the back of the VectorFilter's list order.
-END-
*********************************************************************************************************************/

static ERR FILTEREFFECT_MoveToBack(extFilterEffect *Self)
{
   if (not Self->Filter) return ERR::UnsupportedOwner;
   auto &effects = Self->Filter->Effects;
   auto position = std::ranges::find(effects, Self);
   if (position IS effects.end()) return ERR::InvalidState;

   std::rotate(effects.begin(), position, std::next(position));
   update_filter_links(Self->Filter);
   return ERR::Okay;
}


/*********************************************************************************************************************
-ACTION-
MoveToFront: Move an effect to the front of the VectorFilter's list order.
-END-
*********************************************************************************************************************/

static ERR FILTEREFFECT_MoveToFront(extFilterEffect *Self)
{
   if (not Self->Filter) return ERR::UnsupportedOwner;
   auto &effects = Self->Filter->Effects;
   auto position = std::ranges::find(effects, Self);
   if (position IS effects.end()) return ERR::InvalidState;

   std::rotate(position, std::next(position), effects.end());
   update_filter_links(Self->Filter);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR FILTEREFFECT_NewOwner(extFilterEffect *Self, struct acNewOwner *Args)
{
   if ((not Args) or (not Args->NewOwner)) return ERR::NullArgs;
   if (Self->Filter and (Args->NewOwner != Self->Filter)) {
      clear_filter_references(Self);
      unlink_filter_effect(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Input: Reference to another effect to be used as an input source.

If another effect should be used as a source input, it must be referenced here.  The #SourceType will be automatically
set to `REFERENCE` as a result.

This field is the SVG equivalent to `in`.  If the Input is not defined by the client then it will default to the
previous effect at initialisation if available, otherwise the source graphic is used.

Attach both effects to the same filter before setting this field.  The referenced effect must precede its consumer
when the pipeline is drawn.  If the producer is removed, this field is cleared and #SourceType retains
`REFERENCE`.  Set a new input or a supported standard source to repair the pipeline.

*********************************************************************************************************************/

static ERR FILTEREFFECT_SET_Input(extFilterEffect *Self, extFilterEffect *Value)
{
   if (Value) {
      if (auto error = check_filter_reference(Self, Value, filter_input_role::INPUT); error != ERR::Okay) {
         return error;
      }
   }

   Self->Input = Value;
   Self->SourceType = Value ? VSF::REFERENCE : VSF::NIL;
   if (Self->Filter) Self->Filter->GraphRevision++;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Height: Primitive height of the effect area.

The `(Width, Height)` field values define the dimensions of the effect within the target clipping area.

-FIELD-
Mix: Reference to another effect to be used as a mixer with Input.

If another effect should be used as a mixed source input, it must be referenced here.  The #MixType will be
automatically set to `REFERENCE` as a result.

This field is the SVG equivalent to `in2`.  Effects that support a mixed source require an earlier producer in the
same filter or a standard source selected through #MixType.  Removing a referenced producer clears this field and
leaves #MixType set to `REFERENCE` until the input is repaired.

*********************************************************************************************************************/

static ERR FILTEREFFECT_SET_Mix(extFilterEffect *Self, extFilterEffect *Value)
{
   if (Value) {
      if (auto error = check_filter_reference(Self, Value, filter_input_role::MIX); error != ERR::Okay) {
         return error;
      }
   }

   Self->Mix = Value;
   Self->MixType = Value ? VSF::REFERENCE : VSF::NIL;
   if (Self->Filter) Self->Filter->GraphRevision++;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
MixType: If a secondary mix input is required for the effect, specify it here.
Lookup: VSF

Some effects support a secondary mix input for compositing, such as the @CompositeFX class.  If the mixing source is
a reference to another effect, set the #Mix field instead and this field will be set to `REFERENCE` automatically.

-FIELD-
SourceType: Specifies an input source for the effect algorithm, if required.
Lookup: VSF

If an effect requires an input source for processing, it must be specified here.  If the source is a reference to
another effect, set the #Input field instead and this field will be set to `REFERENCE` automatically.

-FIELD-
Width: Primitive width of the effect area.

The (Width,Height) field values define the dimensions of the effect within the target clipping area.

If a primitive subregion value is undefined, it defaults to the union of the subregions of the effects referenced as
inputs.  If the effect has no referenced inputs, or uses a standard input such as `SourceGraphic`, the default is the
filter region.  Output that falls outside of the primitive subregion is cleared.

-FIELD-
X: Primitive X coordinate for the effect.

The (X,Y) field values define the offset of the effect within the target clipping area.  Undefined values default
as described for #Width.

-FIELD-
Y: Primitive Y coordinate for the effect.

The (X,Y) field values define the offset of the effect within the target clipping area.
-END-

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR FILTEREFFECT_GET_XMLDef(extFilterEffect *Self, std::string &Value)
{
   // Derived classes are expected to override this field
   return ERR::NoSupport;
}

#include "filter_effect_def.c"

static const FieldArray clFilterEffectFields[] = {
   { "Next",       FDF_OBJECT|FDF_RW, nullptr, nullptr, CLASSID::FILTEREFFECT },
   { "Prev",       FDF_OBJECT|FDF_RW, nullptr, nullptr, CLASSID::FILTEREFFECT },
   { "Target",     FDF_OBJECT|FDF_RW, nullptr, nullptr, CLASSID::BITMAP },
   { "Input",      FDF_OBJECT|FDF_RW, nullptr, FILTEREFFECT_SET_Input, CLASSID::FILTEREFFECT },
   { "Mix",        FDF_OBJECT|FDF_RW, nullptr, FILTEREFFECT_SET_Mix, CLASSID::FILTEREFFECT },
   { "X",          FDF_UNIT|FDF_RW },
   { "Y",          FDF_UNIT|FDF_RW },
   { "Width",      FDF_UNIT|FDF_RW },
   { "Height",     FDF_UNIT|FDF_RW },
   { "SourceType", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clFilterEffectSourceType },
   { "MixType",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clFilterEffectMixType },
   { "XMLDef",     FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_filtereffect(void)
{
   clFilterEffect = objMetaClass::create::global(
      fl::ClassVersion(VER_FILTEREFFECT),
      fl::Name("FilterEffect"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clFilterEffectActions),
      fl::Fields(clFilterEffectFields),
      fl::Size(sizeof(extFilterEffect)),
      fl::Path(MOD_PATH));

   return clFilterEffect ? ERR::Okay : ERR::AddClass;
}
