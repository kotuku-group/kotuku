/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
DisplacementFX: Applies the displacement map filter effect.

This filter effect uses the pixel values from the image from Mix to spatially displace the image from Input. This
is the transformation to be performed:

<pre>
P'(x,y) <- P(x + Scale * (XC(x,y) - 0.5), y + Scale * (YC(x,y) - 0.5))
</pre>

where `P(x,y)` is the Input image, and `P'(x,y)` is the Target.  `XC(x,y)` and `YC(x,y)` are the component values
of the channel designated by the #XChannel and #YChannel.  For example, to use the red component of Mix to control
displacement in `X` and the green component to control displacement in `Y`, set #XChannel to `CMP::RED`
and #YChannel to `CMP::GREEN`.

The displacement map defines the inverse of the mapping performed.

The Input image is to remain premultiplied for this filter effect.  The calculations using the pixel values
from Mix are performed using non-premultiplied colour values.  If the image from Mix consists of premultiplied
colour values, those values are automatically converted into non-premultiplied colour values before performing this
operation.

A normal map supplied directly by @LightingFX with type `NORMAL` uses its stored RGB values in either filter colour
space.  Select red for X and green for Y.  A component of 128, or a pixel outside the normal map's subregion, gives
zero displacement along that axis.  To read a normal map from any other source, such as an image loaded by
@ImageFX, set #NormalConvention to the map's convention.  Refraction then reads the map in the same way as
@LightingFX.NormalMap.

-END-

This filter can have arbitrary non-localized effect on the input which might require substantial buffering in the
processing pipeline. However with this formulation, any intermediate buffering needs can be determined by Scale
which represents the maximum range of displacement in either x or y.

When applying this filter, the source pixel location will often lie between several source pixels. In this case it
is recommended that high quality viewers apply an interpolent on the surrounding pixels, for example bilinear or
bicubic, rather than simply selecting the nearest source pixel. Depending on the speed of the available
interpolents, this choice may be affected by the ‘image-rendering’ property setting.

The ‘color-interpolation-filters’ property only applies to the Mix image and does not apply to the Input
image.  The Input image must remain in its current color space.

*********************************************************************************************************************/

#include <bs_thread_pool.h>

class extDisplacementFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::DISPLACEMENTFX;
   static constexpr CSTRING CLASS_NAME = "DisplacementFX";
   using create = kt::Create<extDisplacementFX>;

   double Scale = 0; // SVG default requires this is 0, which makes the displacment algorithm ineffective.
   VSM ResampleMethod = VSM::BILINEAR; // Resample method.
   CMP XChannel = CMP::ALPHA, YChannel = CMP::ALPHA;
   NMC NormalConvention = NMC::AUTO; // AUTO reads the Mix as a normal map only if LightingFX wrote it

   extDisplacementFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) { }

   filter_sampling sampling() const {
      if (not std::isfinite(Scale)) return { filter_access::BOUNDED, Scale, Scale };
      // The displacement magnitude depends on the processing dimensions themselves.  Keep the full-input margin.
      return { filter_access::FULL_INPUT, filter_sampling::UNBOUNDED, filter_sampling::UNBOUNDED };
   }
};

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR DISPLACEMENTFX_Draw(extDisplacementFX *Self, struct acDraw *Args)
{
   kt::Log log;

   // SVG rules state that the Input texture is pre-multiplied.  The Mix displacement map is not.  In practice however,
   // this should not make any difference to Input because the pixels are copied verbatim (not-withstanding pixel
   // interpolation measures).

   // SVG also states that Filter->ColourSpace applies to the Mix and not Input.  The Input must remain in its current
   // colour space.  When the filter colour space is linear, RGB channels from Mix are linearised on sampling; alpha
   // remains unchanged.

   const objBitmap *in_bmp;
   if (auto error = get_source_bitmap(Self, &in_bmp, filter_input_role::INPUT); error != ERR::Okay) return error;
   filter_surface_view map;
   if (auto error = get_source_view(Self, map, filter_input_role::MIX); error != ERR::Okay) return error;
   const auto mix_bmp = map.Bitmap;
   if (not mix_bmp) return ERR::NoSupport;

   const uint8_t RGBA[4] = {
      uint8_t(Self->Target->ColourFormat->RedPos>>3),
      uint8_t(Self->Target->ColourFormat->GreenPos>>3),
      uint8_t(Self->Target->ColourFormat->BluePos>>3),
      uint8_t(Self->Target->ColourFormat->AlphaPos>>3)
   };

   const auto input = filter_sampler(in_bmp);
   const uint8_t *mix = mix_bmp->Data + (mix_bmp->Clip.Left * 4) + (mix_bmp->Clip.Top * mix_bmp->LineWidth);
   uint8_t *dest  = Self->Target->Data + (Self->Target->Clip.Left * Self->Target->BytesPerPixel) + (Self->Target->Clip.Top * Self->Target->LineWidth);

   const int width      = Self->Target->Clip.Right  - Self->Target->Clip.Left;
   const int height     = Self->Target->Clip.Bottom - Self->Target->Clip.Top;
   const int mix_width  = mix_bmp->Clip.Right  - mix_bmp->Clip.Left;
   const int mix_height = mix_bmp->Clip.Bottom - mix_bmp->Clip.Top;
   const int in_width   = in_bmp->Clip.Right   - in_bmp->Clip.Left;
   const int in_height  = in_bmp->Clip.Bottom  - in_bmp->Clip.Top;
   const int draw_width = (mix_width < width) ? mix_width : width;
   const int draw_height = (mix_height < height) ? mix_height : height;
   if ((draw_width < 1) or (draw_height < 1)) return ERR::Okay;

   if (not std::isfinite(Self->Scale)) return ERR::InvalidValue;
   const auto x_channel = Self->XChannel;
   const auto y_channel = Self->YChannel;
   if ((unsigned(x_channel) > unsigned(CMP::ALPHA)) or (unsigned(y_channel) > unsigned(CMP::ALPHA))) {
      return ERR::InvalidValue;
   }
   const bool normals = map.NormalMap or (Self->NormalConvention != NMC::AUTO);
   const bool normal_x = normals and (x_channel != CMP::ALPHA);
   const bool normal_y = normals and (y_channel != CMP::ALPHA);

   // The displacement is Scale * (C - 0.5) in the coordinate system of PrimitiveUnits, which for bounding box units
   // is a fraction of the bounding box on each axis.  The linear part of the client transform maps the user-space
   // vector to bitmap pixels, so that scaling and rotation of the client also apply to the displacement.  The
   // factors convert a sampled component (C - 0.5 in 8-bit steps, or a signed normal component) to that vector.

   const auto &render = *Self->Filter->Render;
   const bool bounding_box = render.PrimitiveUnits IS VUNIT::BOUNDING_BOX;
   const double unit_x = bounding_box ? render.ClientBounds.width() : 1.0;
   const double unit_y = bounding_box ? render.ClientBounds.height() : 1.0;
   const double ux = Self->Scale * unit_x * (normal_x ? (0.5 / NORMAL_MAP_RANGE) : (1.0 / 255.0));
   const double uy = Self->Scale * unit_y * (normal_y ? (0.5 / NORMAL_MAP_RANGE) : (1.0 / 255.0));
   const auto &t = render.Transform;
   const double xx = t.sx * ux, xy = t.shy * ux; // Bitmap displacement for a unit X component
   const double yx = t.shx * uy, yy = t.sy * uy; // Bitmap displacement for a unit Y component
   if ((not std::isfinite(xx)) or (not std::isfinite(xy)) or (not std::isfinite(yx)) or (not std::isfinite(yy))) {
      return ERR::InvalidValue;
   }

   const uint8_t x_type = RGBA[int(x_channel)];
   const uint8_t y_type = RGBA[int(y_channel)];
   const uint8_t alpha = RGBA[int(CMP::ALPHA)];
   const bool linear_mix = effect_colour_space(Self) IS VCS::LINEAR_RGB;
   const bool flip_green = Self->NormalConvention IS NMC::Y_UP; // Y_UP maps store Y negated in green

   auto sample_mix = [linear_mix, alpha, flip_green](const uint8_t *Pixel, CMP Channel, uint8_t Type,
      bool Normal) -> double {
      if (Normal) {
         if (not Pixel[alpha]) return 0;
         const int component = int(Pixel[Type]) - NORMAL_MAP_CENTRE;
         return (flip_green and (Channel IS CMP::GREEN)) ? -component : component;
      }
      const auto value = (linear_mix and (Channel != CMP::ALPHA)) ? glLinearRGB.convert(Pixel[Type]) : Pixel[Type];
      return double(value) - 127.5;
   };

   const bool nearest        = Self->ResampleMethod IS VSM::NEIGHBOUR;
   const int mix_stride      = mix_bmp->LineWidth, dest_stride = Self->Target->LineWidth;
   const int mix_pixel_size  = mix_bmp->BytesPerPixel, dest_pixel_size = Self->Target->BytesPerPixel;

   auto render_rows = [&](int StartY, int EndY) {
      for (int y=StartY; y < EndY; y++) {
         auto m = mix + std::ptrdiff_t(y) * mix_stride;
         auto d = dest + std::ptrdiff_t(y) * dest_stride;
         for (int x=0; x < draw_width; x++, m += mix_pixel_size, d += dest_pixel_size) {
            const double vx = sample_mix(m, x_channel, x_type, normal_x);
            const double vy = sample_mix(m, y_channel, y_type, normal_y);
            const double sample_x = double(x) + (xx * vx) + (yx * vy);
            const double sample_y = double(y) + (xy * vx) + (yy * vy);

            if (nearest) input.sample<filter_interpolation::NEAREST>(d,
               sample_x + in_bmp->Clip.Left, sample_y + in_bmp->Clip.Top);
            else if ((sample_x >= 0.0) and (sample_x < double(in_width)) and
                     (sample_y >= 0.0) and (sample_y < double(in_height))) {
               input.sample<filter_interpolation::BILINEAR>(d,
                  sample_x + in_bmp->Clip.Left, sample_y + in_bmp->Clip.Top);
            }
            else std::fill_n(d, 4, uint8_t(0));
         }
      }
   };

   // Keep small draws on the caller thread to avoid paying for a pool with little work per worker.

   constexpr size_t MIN_PIXELS_PER_WORKER = 32 * 1024;
   constexpr int MIN_ROWS_PER_WORKER = 4;
   const size_t thread_count = std::min({ size_t(std::thread::hardware_concurrency()),
      size_t(draw_height / MIN_ROWS_PER_WORKER), (size_t(draw_width) * draw_height) / MIN_PIXELS_PER_WORKER });

   if (thread_count < 2) render_rows(0, draw_height);
   else {
      // The sources are read-only and each block writes to distinct target rows.
      BS::thread_pool pool(thread_count);
      pool.detach_blocks(0, draw_height, render_rows);
      pool.wait();
   }

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
always read as colour.

In normal map mode a component of 127 displaces by half of #Scale, as a colour component of 255 does in colour mode.

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

Currently `NEIGHBOUR` and `BILINEAR` are supported.  Other resample methods fall back to bilinear filtering.

!VSM

-FIELD-
Scale: Displacement scale factor.

The amount is expressed in the coordinate system established by @VectorFilter.PrimitiveUnits on the parent
@VectorFilter.  In user space units, a map component of 255 displaces by half of Scale in user units, and a component
of 0 by half of Scale in the opposite direction.  In bounding box units, Scale is a fraction of the width of the
client's bounding box on the X axis and of its height on the Y axis.  The displacement is transformed with the client,
so it scales and rotates with it.  When the value of this field is 0, this operation has no effect on the source
image.

-FIELD-
XChannel: X axis channel selection.
Lookup: CMP

-FIELD-
YChannel: Y axis channel selection.
Lookup: CMP

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
   if (Effect.ResampleMethod IS VSM::NEIGHBOUR) Element.attribute("image-rendering", "optimizeSpeed");
   else if (Effect.ResampleMethod IS VSM::LANCZOS) Element.attribute("image-rendering", "optimizeQuality");
   else write_filter_xml_resampling(Element, Effect.ResampleMethod);
   if (Effect.NormalConvention IS NMC::Y_DOWN) Element.attribute("kotuku:normalConvention", "y-down");
   else if (Effect.NormalConvention IS NMC::Y_UP) Element.attribute("kotuku:normalConvention", "y-up");
   return ERR::Okay;
}

//********************************************************************************************************************
// Writable fields that would otherwise be written directly report their changes, so that buffered viewports redraw
// the filter's clients.

static ERR DISPLACEMENTFX_SET_Scale(extDisplacementFX *Self, double Value)
{
   Self->Scale = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DISPLACEMENTFX_SET_ResampleMethod(extDisplacementFX *Self, VSM Value)
{
   Self->ResampleMethod = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DISPLACEMENTFX_SET_XChannel(extDisplacementFX *Self, CMP Value)
{
   Self->XChannel = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DISPLACEMENTFX_SET_YChannel(extDisplacementFX *Self, CMP Value)
{
   Self->YChannel = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_displacement_def.c"

static const FieldArray clDisplacementFXFields[] = {
   { "Scale",            FDF_DOUBLE|FDF_RW, nullptr, DISPLACEMENTFX_SET_Scale },
   { "ResampleMethod",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, DISPLACEMENTFX_SET_ResampleMethod, &clDisplacementFXVSM },
   { "XChannel",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, DISPLACEMENTFX_SET_XChannel, &clDisplacementFXCMP },
   { "YChannel",         FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, DISPLACEMENTFX_SET_YChannel, &clDisplacementFXCMP },
   { "NormalConvention", FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, DISPLACEMENTFX_GET_NormalConvention, DISPLACEMENTFX_SET_NormalConvention, &clDisplacementFXNMC },
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
