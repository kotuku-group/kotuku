/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
DisplacementFX: Applies the displacement map filter effect.

Each output pixel samples @FilterEffect.Input at its own position plus a displacement taken from
@FilterEffect.Mix or #VectorMap.
For a colour map, the X and Y components are the selected channels in the range 0 to 1, minus #NeutralX
and #NeutralY respectively.  Multiply the components by #ScaleX and #ScaleY, transform the resulting
vector with the client, and add it to the output position to obtain the input sample position.

The filter colour space applies to the RGB components of @FilterEffect.Mix.  Alpha is read unchanged and colour
channels are read in straight-alpha form.  The source image is sampled in its own colour space with
alpha-weighted bilinear interpolation by default.

A normal map supplied directly by @LightingFX with type `NORMAL` is read as signed data.  Select red for
X and green for Y.  A component of 128, or a transparent normal-map pixel, gives zero displacement along
that axis.  Set #NormalConvention to read normal maps from other sources, such as @ImageFX.

-END-

*********************************************************************************************************************/

#include <bs_thread_pool.h>

class extDisplacementFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::DISPLACEMENTFX;
   static constexpr CSTRING CLASS_NAME = "DisplacementFX";
   using create = kt::Create<extDisplacementFX>;

   double Scale = 0;
   double ScaleX = std::numeric_limits<double>::quiet_NaN();
   double ScaleY = std::numeric_limits<double>::quiet_NaN();
   double NeutralX = 0.5, NeutralY = 0.5;
   VSM ResampleMethod = VSM::BILINEAR;
   CMP XChannel = CMP::ALPHA, YChannel = CMP::ALPHA;
   FEM EdgeMode = FEM::TRANSPARENT;
   NMC NormalConvention = NMC::AUTO; // AUTO reads the Mix as a normal map only if LightingFX wrote it

   filter_named_input VectorMap { "VectorMap", { .Kind = filter_input_kind::VECTOR_MAP, .Optional = true,
      .NativeOnly = true } };
   const filter_effect_extension Extension { std::span(&VectorMap, 1) };

   extDisplacementFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      FilterExtension = &Extension;
   }

   filter_sampling sampling() const {
      return { filter_access::FULL_INPUT, filter_sampling::UNBOUNDED, filter_sampling::UNBOUNDED,
         EdgeMode IS FEM::REPEAT or EdgeMode IS FEM::MIRROR };
   }
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR DISPLACEMENTFX_Draw(extDisplacementFX *Self, struct acDraw *Args)
{
   filter_surface_view source;
   if (auto error = get_source_view(Self, source, filter_input_role::INPUT); error != ERR::Okay) return error;
   const auto step = active_filter_step(Self);
   if (not step) return ERR::InvalidState;
   const auto &render = *Self->Filter->Render;

   const bool vector_map = step->Operation.Inputs IS filter_inputs::SINGLE;
   filter_surface_view map;
   filter_map_view field;
   if (vector_map) {
      if (auto error = get_filter_map(Self, Self->VectorMap.Name, field); error != ERR::Okay) return error;
   }
   else if (auto error = get_source_view(Self, map, filter_input_role::MIX); error != ERR::Okay) return error;

   const auto target = Self->Target;
   const auto &clip = target->Clip;
   const int width = clip.Right - clip.Left, height = clip.Bottom - clip.Top;
   if ((width <= 0) or (height <= 0)) return ERR::Okay;

   const auto edge = Self->EdgeMode;
   const auto region = filter_input_region(Self, source, step->Inputs.front().Producer, edge);
   const filter_rgba_sampler input { source.Data, source.Stride, region.left, region.top, region.right, region.bottom };

   const auto x_channel = Self->XChannel, y_channel = Self->YChannel;
   const bool normals = map.NormalMap or (Self->NormalConvention != NMC::AUTO);
   const bool normal_x = normals and (x_channel != CMP::ALPHA);
   const bool normal_y = normals and (y_channel != CMP::ALPHA);
   const double neutral_x = Self->NeutralX * 255.0, neutral_y = Self->NeutralY * 255.0;
   const double scale_x = std::isnan(Self->ScaleX) ? Self->Scale : Self->ScaleX;
   const double scale_y = std::isnan(Self->ScaleY) ? Self->Scale : Self->ScaleY;

   // Convert the map's component units to primitive units, then apply the client's linear transform.
   const bool bounding_box = render.PrimitiveUnits IS VUNIT::BOUNDING_BOX;
   const double unit_x = bounding_box ? render.ClientBounds.width() : 1.0;
   const double unit_y = bounding_box ? render.ClientBounds.height() : 1.0;
   const auto component_scale = [vector_map](bool Normal) {
      return vector_map ? 1.0 : (Normal ? (0.5 / NORMAL_MAP_RANGE) : (1.0 / 255.0));
   };
   const double ux = scale_x * unit_x * component_scale(normal_x);
   const double uy = scale_y * unit_y * component_scale(normal_y);
   const auto &t = render.Transform;
   const double xx = t.sx * ux, xy = t.shy * ux;
   const double yx = t.shx * uy, yy = t.sy * uy;
   if ((not std::isfinite(xx)) or (not std::isfinite(xy)) or (not std::isfinite(yx)) or (not std::isfinite(yy))) {
      return ERR::InvalidValue;
   }

   const auto alpha = source.Bitmap->ColourFormat->AlphaPos >> 3;
   const bool straight = source.Alpha IS filter_alpha::STRAIGHT;
   const filter_rgba_sampler mix { map.Data, map.Stride, map.Valid.left, map.Valid.top,
      map.Valid.right, map.Valid.bottom };
   const auto &format = *(map.Bitmap ? map.Bitmap : source.Bitmap)->ColourFormat;
   const int rgba[] = { format.RedPos >> 3, format.GreenPos >> 3, format.BluePos >> 3, format.AlphaPos >> 3 };
   const bool linear_mix = effect_colour_space(Self) IS VCS::LINEAR_RGB;
   const bool flip_green = Self->NormalConvention IS NMC::Y_UP;

   auto sample_mix = [&](const uint8_t *Pixel, CMP Channel, bool Normal, double Neutral) -> double {
      const int coverage = Pixel ? Pixel[rgba[int(CMP::ALPHA)]] : 0;
      const int stored = Pixel ? Pixel[rgba[int(Channel)]] : 0;
      if (Normal) {
         if (not coverage) return 0;
         const int component = stored - NORMAL_MAP_CENTRE;
         return (flip_green and (Channel IS CMP::GREEN)) ? -component : component;
      }
      if (Channel IS CMP::ALPHA) return double(stored) - Neutral;
      auto value = uint8_t(stored);
      if (map.Alpha IS filter_alpha::PREMULTIPLIED) {
         value = coverage ? uint8_t(std::min(255, (stored * 255 + coverage / 2) / coverage)) : 0;
      }
      if (linear_mix and (map.Encoding IS CS::SRGB)) value = glLinearRGB.convert(value);
      else if ((not linear_mix) and (map.Encoding IS CS::LINEAR_RGB)) value = glLinearRGB.invert(value);
      return double(value) - Neutral;
   };

   const int input_x = render.VectorClip.left - source.OriginX, input_y = render.VectorClip.top - source.OriginY;
   const int map_x = render.VectorClip.left - map.OriginX, map_y = render.VectorClip.top - map.OriginY;
   const bool nearest = Self->ResampleMethod IS VSM::NEIGHBOUR;
   auto render_rows = [&](int StartY, int EndY) {
      for (int y = StartY; y < EndY; y++) {
         auto dest = target->Data + std::ptrdiff_t(y) * target->LineWidth + std::ptrdiff_t(clip.Left) * 4;
         for (int x = clip.Left; x < clip.Right; x++, dest += 4) {
            double vx, vy;
            if (vector_map) {
               const auto value = field.sample(filter_pixel_centre(x + render.VectorClip.left),
                  filter_pixel_centre(y + render.VectorClip.top));
               vx = value[0];
               vy = value[1];
            }
            else {
               const auto pixel = mix.pixel(x + map_x, y + map_y);
               vx = sample_mix(pixel, x_channel, normal_x, neutral_x);
               vy = sample_mix(pixel, y_channel, normal_y, neutral_y);
            }

            const double sample_x = double(x + input_x) + (xx * vx) + (yx * vy);
            const double sample_y = double(y + input_y) + (xy * vx) + (yy * vy);
            if (nearest) input.sample<filter_interpolation::NEAREST>(dest, sample_x, sample_y, edge);
            else input.sample<filter_interpolation::BILINEAR>(dest, sample_x, sample_y, edge, straight ? alpha : -1);
         }
      }
   };

   // Keep small draws on the caller thread to avoid paying for a pool with little work per worker.

   constexpr size_t MIN_PIXELS_PER_WORKER = 32 * 1024;
   constexpr int MIN_ROWS_PER_WORKER = 4;
   const size_t thread_count = std::min({ size_t(std::thread::hardware_concurrency()),
      size_t(height / MIN_ROWS_PER_WORKER), (size_t(width) * height) / MIN_PIXELS_PER_WORKER });

   if (thread_count < 2) render_rows(clip.Top, clip.Bottom);
   else {
      // The sources are read-only and each block writes to distinct target rows.
      BS::thread_pool pool(thread_count);
      pool.detach_blocks(clip.Top, clip.Bottom, render_rows);
      pool.wait();
   }

   target->ColourSpace = source.Encoding;
   if (not straight) {
      target->Flags |= BMF::PREMUL;
      if (auto error = target->demultiply(); error != ERR::Okay) return error;
   }
   return source.Encoding IS CS::LINEAR_RGB ? target->convertToRGB() : ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EdgeMode: Controls how @FilterEffect.Input is sampled outside its image region.

`TRANSPARENT` is the default.  The SVG extension `kotuku:edgeMode` accepts `transparent`, `clamp`,
`repeat` and `mirror`.

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_SET_EdgeMode(extDisplacementFX *Self, FEM Value)
{
   if ((Value < FEM::TRANSPARENT) or (Value > FEM::MIRROR)) return ERR::OutOfRange;
   Self->EdgeMode = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
NeutralX: The X channel value that gives zero displacement in a colour map.

The range is 0 to 1, with a default of 0.5.  The value is in the filter colour space for RGB and in unit
coverage for alpha.  The SVG attribute is `kotuku:neutralX`.

-FIELD-
NeutralY: The Y channel value that gives zero displacement in a colour map.

The range is 0 to 1, with a default of 0.5.  The value is in the filter colour space for RGB and in unit
coverage for alpha.  The SVG attribute is `kotuku:neutralY`.

*********************************************************************************************************************/

template<double extDisplacementFX::*Field>
static ERR DISPLACEMENTFX_SET_NeutralXY(extDisplacementFX *Self, double Value)
{
   if (not ((Value >= 0.0) and (Value <= 1.0))) return ERR::OutOfRange;
   Self->*Field = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
NormalConvention: Reads the Mix input as a normal map in the given axis convention.

The default, `AUTO`, reads the Mix input as a normal map only if it is the result of @LightingFX with type `NORMAL`,
and as colour otherwise.  `Y_DOWN` and `Y_UP` read any Mix input as a normal map: the red, green and blue channels
are signed components centred on 128, read as data in either filter colour space, and transparent pixels give zero
displacement.  `Y_UP` negates the green component, for maps whose Y increases upwards, as in OpenGL.  Use the same
value as @LightingFX.NormalConvention so that refraction and shading agree.  The alpha channel, if selected, is
always read as colour.  RGB normal components ignore the neutral fields.

In normal map mode a signed component of +127 displaces by half the effective scale for its axis.

The SVG attribute is `kotuku:normalConvention`, with the values `auto`, `y-down` and `y-up`.

-END-

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_GET_NormalConvention(extDisplacementFX *Self, NMC &Value)
{
   Value = Self->NormalConvention;
   return ERR::Okay;
}

static ERR DISPLACEMENTFX_SET_NormalConvention(extDisplacementFX *Self, NMC Value)
{
   if ((Value != NMC::AUTO) and (Value != NMC::Y_DOWN) and (Value != NMC::Y_UP)) return ERR::OutOfRange;
   Self->NormalConvention = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ResampleMethod: The resample algorithm to use for transforming the source image.

Use `NEIGHBOUR` for nearest-neighbour sampling or `BILINEAR` for alpha-weighted bilinear sampling.
The default is `BILINEAR`, also selected by `AUTO`.  Other values return `ERR::NoSupport`.

!VSM

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_SET_ResampleMethod(extDisplacementFX *Self, VSM Value)
{
   if ((Value != VSM::AUTO) and (Value != VSM::NEIGHBOUR) and (Value != VSM::BILINEAR)) return ERR::NoSupport;
   Self->ResampleMethod = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Scale: Displacement scale factor.

This is the fallback for both axis overrides.  The scale is expressed in @VectorFilter.PrimitiveUnits.
In bounding box units, it is a fraction of the client's bounding box width for X or height for Y.
The client's linear transform applies to the displacement, including rotation and shear.

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_SET_Scale(extDisplacementFX *Self, double Value)
{
   if (not std::isfinite(Value)) return ERR::InvalidValue;
   Self->Scale = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ScaleX: Overrides #Scale on the X axis.

Accepts any finite signed value.  A zero override disables horizontal displacement in primitive coordinates.
Read or write NaN for inheritance from #Scale.  An explicit override serialises as `kotuku:scaleX`.

-FIELD-
ScaleY: Overrides #Scale on the Y axis.

Accepts any finite signed value.  A zero override disables vertical displacement in primitive coordinates.
Read or write NaN for inheritance from #Scale.  An explicit override serialises as `kotuku:scaleY`.

*********************************************************************************************************************/

template<double extDisplacementFX::*Field>
static ERR DISPLACEMENTFX_SET_ScaleXY(extDisplacementFX *Self, double Value)
{
   if (std::isinf(Value)) return ERR::InvalidValue;
   Self->*Field = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
VectorMap: Optional earlier filter result containing two signed floating-point components per pixel.

While bound, this input takes precedence over @FilterEffect.Mix and contributes to the default primitive subregion.
The first and second components multiply the effective X and Y scales directly.  A component of +1 thus
displaces by one effective scale along its axis in primitive units.  Invalid or missing samples give zero
displacement.  Channel selectors, colour transfer, normal conventions and colour neutral values apply only
to @FilterEffect.Mix.  Clearing this field reactivates the colour map on the live effect.  This field requires a
native vector producer.  @VectorFilter.EffectXML returns `ERR::NoSupport` while this field is bound.

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_GET_VectorMap(extDisplacementFX *Self, objFilterEffect **Value)
{
   *Value = Self->VectorMap.Reference;
   return ERR::Okay;
}

static ERR DISPLACEMENTFX_SET_VectorMap(extDisplacementFX *Self, objFilterEffect *Value)
{
   return set_named_filter_input(Self, Self->VectorMap.Name, Value ? VSF::REFERENCE : VSF::NONE, Value);
}

/*********************************************************************************************************************

-FIELD-
XChannel: X axis channel selection.
Lookup: CMP

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_SET_XChannel(extDisplacementFX *Self, CMP Value)
{
   if (unsigned(Value) > unsigned(CMP::ALPHA)) return ERR::OutOfRange;
   Self->XChannel = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR displacement_xml(const extDisplacementFX &Effect, filter_xml_element &Element)
{
   constexpr std::string_view channels = "RGBA";
   if ((size_t(Effect.XChannel) >= channels.size()) or (size_t(Effect.YChannel) >= channels.size())) {
      return ERR::InvalidValue;
   }
   Element.Name = "feDisplacementMap";
   Element.attribute("scale", Effect.Scale);
   Element.attribute("xChannelSelector", channels.substr(int(Effect.XChannel), 1));
   Element.attribute("yChannelSelector", channels.substr(int(Effect.YChannel), 1));
   if (std::isfinite(Effect.ScaleX)) Element.attribute("kotuku:scaleX", Effect.ScaleX);
   if (std::isfinite(Effect.ScaleY)) Element.attribute("kotuku:scaleY", Effect.ScaleY);
   if (Effect.NeutralX != 0.5) Element.attribute("kotuku:neutralX", Effect.NeutralX);
   if (Effect.NeutralY != 0.5) Element.attribute("kotuku:neutralY", Effect.NeutralY);
   switch (Effect.EdgeMode) {
      case FEM::CLAMP: Element.attribute("kotuku:edgeMode", "clamp"); break;
      case FEM::REPEAT: Element.attribute("kotuku:edgeMode", "repeat"); break;
      case FEM::MIRROR: Element.attribute("kotuku:edgeMode", "mirror"); break;
      case FEM::TRANSPARENT: break;
   }
   if (Effect.ResampleMethod IS VSM::NEIGHBOUR) Element.attribute("image-rendering", "optimizeSpeed");
   else write_filter_xml_resampling(Element, Effect.ResampleMethod);
   if (Effect.NormalConvention IS NMC::Y_DOWN) Element.attribute("kotuku:normalConvention", "y-down");
   else if (Effect.NormalConvention IS NMC::Y_UP) Element.attribute("kotuku:normalConvention", "y-up");
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
YChannel: Y axis channel selection.
Lookup: CMP

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_SET_YChannel(extDisplacementFX *Self, CMP Value)
{
   if (unsigned(Value) > unsigned(CMP::ALPHA)) return ERR::OutOfRange;
   Self->YChannel = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_displacement_def.c"

static const FieldArray clDisplacementFXFields[] = {
   { "Scale",            FDF_DOUBLE|FDF_RW, nullptr, DISPLACEMENTFX_SET_Scale },
   { "ScaleX",           FDF_DOUBLE|FDF_RW, nullptr, DISPLACEMENTFX_SET_ScaleXY<&extDisplacementFX::ScaleX> },
   { "ScaleY",           FDF_DOUBLE|FDF_RW, nullptr, DISPLACEMENTFX_SET_ScaleXY<&extDisplacementFX::ScaleY> },
   { "NeutralX",         FDF_DOUBLE|FDF_RW, nullptr, DISPLACEMENTFX_SET_NeutralXY<&extDisplacementFX::NeutralX> },
   { "NeutralY",         FDF_DOUBLE|FDF_RW, nullptr, DISPLACEMENTFX_SET_NeutralXY<&extDisplacementFX::NeutralY> },
   { "ResampleMethod",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, DISPLACEMENTFX_SET_ResampleMethod, &clDisplacementFXVSM },
   { "XChannel",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, DISPLACEMENTFX_SET_XChannel, &clDisplacementFXCMP },
   { "YChannel",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, DISPLACEMENTFX_SET_YChannel, &clDisplacementFXCMP },
   { "NormalConvention", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, DISPLACEMENTFX_GET_NormalConvention, DISPLACEMENTFX_SET_NormalConvention, &clDisplacementFXNMC },
   { "EdgeMode",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, DISPLACEMENTFX_SET_EdgeMode, &clDisplacementFXFEM },
   { "VectorMap",        FDF_VIRTUAL|FDF_OBJECT|FDF_RW|FDF_PURE, DISPLACEMENTFX_GET_VectorMap, DISPLACEMENTFX_SET_VectorMap, CLASSID::FILTEREFFECT },
   { "XMLDef",           FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_displacementfx(void)
{
   clDisplacementFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::DISPLACEMENTFX),
      fl::Name("DisplacementFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clDisplacementFXActions),
      fl::Fields(clDisplacementFXFields),
      fl::Size(sizeof(extDisplacementFX)),
      fl::Path(MOD_PATH));

   return clDisplacementFX ? ERR::Okay : ERR::AddClass;
}
