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
   VSM ResampleMethod = VSM::NEIGHBOUR;

   extOffsetFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   // Computes the offset in bitmap pixels.

   std::pair<double, double> pixel_offset() const {
      const auto &render = *Filter->Render;
      const auto [unit_x, unit_y] = render.primitive_scales(true);
      double x = XOffset * unit_x, y = YOffset * unit_y;
      render.ToScene.transform_2x2(&x, &y);
      if (ResampleMethod != VSM::BILINEAR) return { std::round(x), std::round(y) };
      return { x, y };
   }

   filter_sampling sampling() const {
      const auto [x, y] = pixel_offset();
      return { filter_access::BOUNDED, std::ceil(std::abs(x)), std::ceil(std::abs(y)) };
   }
};

//********************************************************************************************************************

static ERR OFFSETFX_Draw(extOffsetFX *Self, struct acDraw *Args)
{
   filter_surface_view source;
   if (auto error = get_source_view(Self, source, filter_input_role::INPUT); error != ERR::Okay) return error;
   if (not source.Bitmap) return ERR::NoSupport;
   const auto [dx, dy] = Self->pixel_offset();
   if ((not std::isfinite(dx)) or (not std::isfinite(dy))) return ERR::InvalidValue;
   if ((std::abs(dx) >= source.Width) or (std::abs(dy) >= source.Height)) return ERR::Okay;

   const auto target = Self->Target;
   if ((dx IS std::trunc(dx)) and (dy IS std::trunc(dy))) {
      return copy_filter_bitmap(source.Bitmap, target, BAF::NIL, 0, 0, source.Width, source.Height, int(dx), int(dy));
   }

   // The input and target share the filter bitmap's origin.  Outside the input, taps are transparent.
   const filter_rgba_sampler input { source.Data, source.Stride, source.Valid.left, source.Valid.top,
      source.Valid.right, source.Valid.bottom };
   const auto alpha = source.Bitmap->ColourFormat->AlphaPos >> 3;
   const auto &clip = target->Clip;
   for (int y = clip.Top; y < clip.Bottom; y++) {
      auto dest = target->Data + std::ptrdiff_t(y) * target->LineWidth + std::ptrdiff_t(clip.Left) * 4;
      const double sample_y = y - dy;
      for (int x = clip.Left; x < clip.Right; x++, dest += 4) {
         input.sample<filter_interpolation::BILINEAR>(dest, x - dx, sample_y, FEM::TRANSPARENT, alpha);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ResampleMethod: Controls interpolation when the transformed offset falls between pixels.

`NEIGHBOUR` (the default) and `AUTO` round the transformed offset to the nearest whole pixel.
`BILINEAR` preserves fractional offsets using alpha-weighted interpolation with transparent edges.
Setting any other value returns `ERR::NoSupport`.

!VSM

*********************************************************************************************************************/

static ERR OFFSETFX_SET_ResampleMethod(extOffsetFX *Self, VSM Value)
{
   if ((Value != VSM::AUTO) and (Value != VSM::NEIGHBOUR) and (Value != VSM::BILINEAR)) return ERR::NoSupport;
   Self->ResampleMethod = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XOffset: The delta X coordinate for the input graphic.

The `(XOffset, YOffset)` field values define the offset of the input source within the target clipping area.  The
value is measured in the units of the filter's @VectorFilter.PrimitiveUnits.

*********************************************************************************************************************/

static ERR OFFSETFX_SET_XOffset(extOffsetFX *Self, double Value)
{
   Self->XOffset = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
YOffset: The delta Y coordinate for the input graphic.

The `(XOffset, YOffset)` field values define the offset of the input source within the target clipping area.  The
value is measured in the units of the filter's @VectorFilter.PrimitiveUnits.

*********************************************************************************************************************/

static ERR OFFSETFX_SET_YOffset(extOffsetFX *Self, double Value)
{
   Self->YOffset = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR offset_xml(const extOffsetFX &Effect, filter_xml_element &Element)
{
   Element.Name = "feOffset";
   Element.attribute("dx", Effect.XOffset);
   Element.attribute("dy", Effect.YOffset);
   if (Effect.ResampleMethod != VSM::NEIGHBOUR) Element.attribute("xvg:resampleMethod", int(Effect.ResampleMethod));
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_offset_def.c"

static const FieldArray clOffsetFXFields[] = {
   { "XOffset", FDF_DOUBLE|FDF_RW, nullptr, OFFSETFX_SET_XOffset },
   { "YOffset", FDF_DOUBLE|FDF_RW, nullptr, OFFSETFX_SET_YOffset },
   { "ResampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, OFFSETFX_SET_ResampleMethod, &clOffsetFXVSM },
   { "XMLDef",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
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
