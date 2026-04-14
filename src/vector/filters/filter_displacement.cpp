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
from Mix are performed using non-premultiplied color values.  If the image from Mix consists of premultiplied
color values, those values are automatically converted into non-premultiplied color values before performing this
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

class extDisplacementFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::DISPLACEMENTFX;
   static constexpr CSTRING CLASS_NAME = "DisplacementFX";
   using create = pf::Create<extDisplacementFX>;

   double Scale;
   CMP XChannel, YChannel;
};

static double SQRT2DIV2 = sqrt(2.0) / 2.0;

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR DISPLACEMENTFX_Draw(extDisplacementFX *Self, struct acDraw *Args)
{
   pf::Log log;

   // SVG rules state that the Input texture is pre-multiplied.  The Mix displacement map is not.  In practice however,
   // this should not make any difference to Input because the pixels are copied verbatim (not-withstanding pixel
   // interpolation measures).

   // SVG also states that Filter->ColourSpace applies to the Mix and not Input.  The Input must remain in its current
   // colour space.  NOTE: If the displacement map is behaving in an unexpected way, check that colourspace is in the
   // expected format, most probably SRGB and not linear.

   objBitmap *inBmp, *mixBmp;
   if (get_source_bitmap(Self->Filter, &inBmp, Self->SourceType, Self->Input, false) != ERR::Okay) return log.warning(ERR::NoData);
   if (get_source_bitmap(Self->Filter, &mixBmp, Self->MixType, Self->Mix, false) != ERR::Okay) return log.warning(ERR::NoData);

   const uint8_t RGBA[4] = {
      uint8_t(Self->Target->ColourFormat->RedPos>>3),
      uint8_t(Self->Target->ColourFormat->GreenPos>>3),
      uint8_t(Self->Target->ColourFormat->BluePos>>3),
      uint8_t(Self->Target->ColourFormat->AlphaPos>>3)
   };

   uint8_t *input = inBmp->Data + (inBmp->Clip.Left * inBmp->BytesPerPixel) + (inBmp->Clip.Top * inBmp->LineWidth);
   uint8_t *mix   = mixBmp->Data + (mixBmp->Clip.Left * 4) + (mixBmp->Clip.Top * mixBmp->LineWidth);
   uint8_t *dest  = Self->Target->Data + (Self->Target->Clip.Left * Self->Target->BytesPerPixel) + (Self->Target->Clip.Top * Self->Target->LineWidth);

   const int width      = Self->Target->Clip.Right  - Self->Target->Clip.Left;
   const int height     = Self->Target->Clip.Bottom - Self->Target->Clip.Top;
   const int mix_width  = mixBmp->Clip.Right  - mixBmp->Clip.Left;
   const int mix_height = mixBmp->Clip.Bottom - mixBmp->Clip.Top;
   const int in_width   = inBmp->Clip.Right   - inBmp->Clip.Left;
   const int in_height  = inBmp->Clip.Bottom  - inBmp->Clip.Top;

   auto &client = Self->Filter->ClientVector;
   const double c_width  = client->Bounds.width();
   const double c_height = client->Bounds.height();

   double sx, sy;
   double scale_against;
   if (Self->Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      // Scale is relative to the bounding box dimensions
      scale_against = sqrt((c_width * c_width) + (c_height * c_height)) * SQRT2DIV2;
      double scale = Self->Scale / scale_against;
      sx = scale * double(mix_width)  * (1.0 / 255.0);
      sy = scale * double(mix_height) * (1.0 / 255.0);
   }
   else { // USERSPACE
      scale_against = sqrt((c_width * c_width) + (c_height * c_height)) * SQRT2DIV2;
      double scale = Self->Scale / scale_against;
      sx = scale * double(mix_width)  * (1.0 / 255.0);
      sy = scale * double(mix_height) * (1.0 / 255.0);
   }

   const uint8_t x_type = RGBA[int(Self->XChannel)];
   const uint8_t y_type = RGBA[int(Self->YChannel)];

   //log.warning("W/H: %dx%d; MW/H: %dx%d; IW/H: %dx%d; CW/H: %.2fx%.2f, BBox: %d", width, height, mix_width, mix_height, in_width, in_height, c_width, c_height, Self->Filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX);
   //log.warning("X Channel: %d, Y Channel: %d; Scale: %.2f / %.2f -> %.2f,%.2f; WH: %dx%d", Self->XChannel, Self->YChannel, Self->Scale, scale_against, sx, sy, width, height);

   static const double HALF8BIT = 255.0 * 0.5;
   for (int y=0; y < height; y++) {
      auto m = mix;
      auto d = (uint32_t *)dest;
      for (int x=0; x < width; x++, m += mixBmp->BytesPerPixel, d++) {
         auto dx = m[x_type];
         auto dy = m[y_type];
         // TODO: SVG recommends using interpolation between pixels rather than the dropping the fractional part
         // as done here.
         const int cx = x + F2I(sx * (double(dx) - HALF8BIT));
         const int cy = y + F2I(sy * (double(dy) - HALF8BIT));
         if ((cx < 0) or (cx >= in_width) or (cy < 0) or (cy >= in_height)) {
            // The source pixel is outside of retrievable bounds
            *d = 0;
         }
         else *d = ((uint32_t *)(input + (cx * 4) + (cy * inBmp->LineWidth)))[0];
      }
      mix  += mixBmp->LineWidth;
      dest += Self->Target->LineWidth;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DISPLACEMENTFX_NewObject(extDisplacementFX *Self)
{
   Self->Scale = 0; // SVG default requires this is 0, which makes the displacment algorithm ineffective.
   Self->XChannel = CMP::ALPHA;
   Self->YChannel = CMP::ALPHA;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Scale: Displacement scale factor.

The amount is expressed in the coordinate system established by @VectorFilter.PrimitiveUnits on the parent @VectorFilter.
When the value of this field is 0, this operation has no effect on the source image.

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_GET_Scale(extDisplacementFX *Self, double *Value)
{
   *Value = Self->Scale;
   return ERR::Okay;
}

static ERR DISPLACEMENTFX_SET_Scale(extDisplacementFX *Self, double Value)
{
   Self->Scale = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XChannel: X axis channel selection.
Lookup: CMP

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_GET_XChannel(extDisplacementFX *Self, CMP *Value)
{
   *Value = Self->XChannel;
   return ERR::Okay;
}

static ERR DISPLACEMENTFX_SET_XChannel(extDisplacementFX *Self, CMP Value)
{
   Self->XChannel = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
YChannel: Y axis channel selection.
Lookup: CMP

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_GET_YChannel(extDisplacementFX *Self, CMP *Value)
{
   *Value = Self->YChannel;
   return ERR::Okay;
}

static ERR DISPLACEMENTFX_SET_YChannel(extDisplacementFX *Self, CMP Value)
{
   Self->YChannel = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR DISPLACEMENTFX_GET_XMLDef(extDisplacementFX *Self, STRING *Value)
{
   std::stringstream stream;

   stream << "<feDisplacement/>";

   *Value = strclone(stream.str());
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_displacement_def.c"

static const FieldDef clChannel[] = {
   { "Red",   CMP::RED },
   { "Green", CMP::GREEN },
   { "Blue",  CMP::BLUE },
   { "Alpha", CMP::ALPHA },
   { nullptr, 0 }
};

static const FieldArray clDisplacementFXFields[] = {
   { "Scale",     FDF_VIRTUAL|FDF_DOUBLE|FDF_RW, DISPLACEMENTFX_GET_Scale, DISPLACEMENTFX_SET_Scale },
   { "XChannel",  FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, DISPLACEMENTFX_GET_XChannel, DISPLACEMENTFX_SET_XChannel, &clChannel },
   { "YChannel",  FDF_VIRTUAL|FDF_INT|FDF_LOOKUP|FDF_RW, DISPLACEMENTFX_GET_YChannel, DISPLACEMENTFX_SET_YChannel, &clChannel },
   { "XMLDef",    FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R, DISPLACEMENTFX_GET_XMLDef },
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
