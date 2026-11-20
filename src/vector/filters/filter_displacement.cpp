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

   const objBitmap *in_bmp, *mix_bmp;
   if (auto error = get_source_bitmap(Self, &in_bmp, filter_input_role::INPUT); error != ERR::Okay) return error;
   if (auto error = get_source_bitmap(Self, &mix_bmp, filter_input_role::MIX); error != ERR::Okay) return error;

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

   const auto &bounds = Self->Filter->Render->ClientPathBounds;
   const auto diagonal = filter_render_context::normalised_diagonal(bounds.width(), bounds.height());
   if ((not std::isfinite(diagonal)) or (not std::isfinite(Self->Scale))) return ERR::InvalidValue;
   if (diagonal <= 0) return ERR::Okay;
   const double scale = Self->Scale / diagonal;
   const double sx = scale * double(mix_width) * (1.0 / 255.0);
   const double sy = scale * double(mix_height) * (1.0 / 255.0);
   if ((not std::isfinite(sx)) or (not std::isfinite(sy))) return ERR::InvalidValue;

   const auto x_channel = Self->XChannel;
   const auto y_channel = Self->YChannel;
   const uint8_t x_type = RGBA[int(x_channel)];
   const uint8_t y_type = RGBA[int(y_channel)];
   const bool linear_mix = Self->Filter->ColourSpace IS VCS::LINEAR_RGB;

   auto sample_mix = [linear_mix](const uint8_t *Pixel, CMP Channel, uint8_t Type) -> uint8_t {
      if ((linear_mix) and (Channel != CMP::ALPHA)) return glLinearRGB.convert(Pixel[Type]);
      else return Pixel[Type];
   };

   constexpr double HALF8BIT = 255.0 * 0.5;
   const bool nearest        = Self->ResampleMethod IS VSM::NEIGHBOUR;
   const int mix_stride      = mix_bmp->LineWidth, dest_stride = Self->Target->LineWidth;
   const int mix_pixel_size  = mix_bmp->BytesPerPixel, dest_pixel_size = Self->Target->BytesPerPixel;

   auto render_rows = [&](int StartY, int EndY) {
      for (int y=StartY; y < EndY; y++) {
         auto m = mix + std::ptrdiff_t(y) * mix_stride;
         auto d = dest + std::ptrdiff_t(y) * dest_stride;
         for (int x=0; x < draw_width; x++, m += mix_pixel_size, d += dest_pixel_size) {
            auto dx = sample_mix(m, x_channel, x_type);
            auto dy = sample_mix(m, y_channel, y_type);
            const double sample_x = double(x) + (sx * (double(dx) - HALF8BIT));
            const double sample_y = double(y) + (sy * (double(dy) - HALF8BIT));

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
ResampleMethod: The resample algorithm to use for transforming the source image.

Currently `NEIGHBOUR` and `BILINEAR` are supported.  Other resample methods fall back to bilinear filtering.

!VSM

-FIELD-
Scale: Displacement scale factor.

The amount is expressed in the coordinate system established by @VectorFilter.PrimitiveUnits on the parent @VectorFilter.
When the value of this field is 0, this operation has no effect on the source image.

-FIELD-
XChannel: X axis channel selection.
Lookup: CMP

-FIELD-
YChannel: Y axis channel selection.
Lookup: CMP

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_GET_XMLDef(extDisplacementFX *Self, std::string &Value)
{
   std::stringstream stream;

   stream << "feDisplacement";

   Value = stream.str();
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_displacement_def.c"

static const FieldArray clDisplacementFXFields[] = {
   { "Scale",     FDF_DOUBLE|FDF_RW },
   { "ResampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clDisplacementFXVSM },
   { "XChannel",  FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clDisplacementFXCMP },
   { "YChannel",  FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clDisplacementFXCMP },
   { "XMLDef",    FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, DISPLACEMENTFX_GET_XMLDef },
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
