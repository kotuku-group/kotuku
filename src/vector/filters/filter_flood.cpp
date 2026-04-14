/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
FloodFX: Applies the flood filter effect.

The FloodFX class is an output-only effect that fills its target area with a single colour value.

-END-

*********************************************************************************************************************/

class extFloodFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::FLOODFX;
   static constexpr CSTRING CLASS_NAME = "FloodFX";
   using create = pf::Create<extFloodFX>;

   FRGB   Colour;
   RGB8   ColourRGB; // A cached conversion of the FRGB value
   double Opacity;
};

//********************************************************************************************************************

static ERR FLOODFX_NewObject(extFloodFX *Self)
{
   Self->Opacity = 1.0;
   Self->SourceType = VSF::NONE;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR FLOODFX_Draw(extFloodFX *Self, struct acDraw *Args)
{
   auto &filter = Self->Filter;

   // Draw to destination.  No anti-aliasing is applied and the alpha channel remains constant.
   // Note: There seems to be a quirk in the SVG standards in that flooding does not honour the
   // linear RGB space when blending.  This is indicated in the formal test results, but
   // W3C documentation has no mention of it.

   const auto col = agg::rgba8(Self->ColourRGB, int(Self->Colour.Alpha * Self->Opacity * 255.0));

   agg::rasterizer_scanline_aa<> raster;
   agg::renderer_base<agg::pixfmt_psl> renderBase;
   agg::scanline32_p8 scanline;
   agg::pixfmt_psl format(*Self->Target);
   renderBase.attach(format);

   agg::path_storage path;
   path.move_to(filter->TargetX, filter->TargetY);
   path.line_to(filter->TargetX + filter->TargetWidth, filter->TargetY);
   path.line_to(filter->TargetX + filter->TargetWidth, filter->TargetY + filter->TargetHeight);
   path.line_to(filter->TargetX, filter->TargetY + filter->TargetHeight);
   path.close_polygon();

   agg::renderer_scanline_bin_solid< agg::renderer_base<agg::pixfmt_psl> > solid_render(renderBase);
   agg::conv_transform<agg::path_storage, agg::trans_affine> final_path(path, filter->ClientVector->Transform);
   raster.add_path(final_path);
   renderBase.clip_box(Self->Target->Clip.Left, Self->Target->Clip.Top, Self->Target->Clip.Right - 1, Self->Target->Clip.Bottom - 1);
   solid_render.color(col);
   agg::render_scanlines(raster, scanline, solid_render);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Colour: The colour of the fill in RGB float format.

This field defines the colour of the flood fill in floating-point RGBA format, in a range of 0 - 1.0 per component.

The colour is complemented by the #Opacity field.

*********************************************************************************************************************/

static ERR FLOODFX_GET_Colour(extFloodFX *Self, float **Value, int *Elements)
{
   *Value = (float *)&Self->Colour;
   *Elements = 4;
   return ERR::Okay;
}

static ERR FLOODFX_SET_Colour(extFloodFX *Self, float *Value, int Elements)
{
   pf::Log log;
   if (Value) {
      if (Elements >= 3) {
         Self->Colour.Red   = Value[0];
         Self->Colour.Green = Value[1];
         Self->Colour.Blue  = Value[2];
         Self->Colour.Alpha = (Elements >= 4) ? Value[3] : 1.0;

         Self->ColourRGB.Red   = int(Self->Colour.Red * 255.0);
         Self->ColourRGB.Green = int(Self->Colour.Green * 255.0);
         Self->ColourRGB.Blue  = int(Self->Colour.Blue * 255.0);
         Self->ColourRGB.Alpha = int(Self->Colour.Alpha * 255.0);
      }
      else return log.warning(ERR::InvalidValue);
   }
   else Self->Colour.Alpha = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Opacity: Modifies the opacity of the flood colour.

*********************************************************************************************************************/

static ERR FLOODFX_GET_Opacity(extFloodFX *Self, double *Value)
{
   *Value = Self->Opacity;
   return ERR::Okay;
}

static ERR FLOODFX_SET_Opacity(extFloodFX *Self, double Value)
{
   pf::Log log;
   if ((Value >= 0.0) and (Value <= 1.0)) {
      Self->Opacity = Value;
      return ERR::Okay;
   }
   else return log.warning(ERR::OutOfRange);
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Returns an SVG compliant XML string that describes the effect.
-END-

*********************************************************************************************************************/

static ERR FLOODFX_GET_XMLDef(extFloodFX *Self, STRING *Value)
{
   std::stringstream stream;

   stream << "<feFlood opacity=\"" << Self->Opacity << "\"/>";

   *Value = strclone(stream.str());
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_flood_def.c"

static const FieldArray clFloodFXFields[] = {
   { "Colour",  FDF_VIRTUAL|FD_FLOAT|FDF_ARRAY|FD_RW,   FLOODFX_GET_Colour, FLOODFX_SET_Colour },
   { "Opacity", FDF_VIRTUAL|FDF_DOUBLE|FDF_RW,          FLOODFX_GET_Opacity, FLOODFX_SET_Opacity },
   { "XMLDef",  FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R, FLOODFX_GET_XMLDef },
   END_FIELD
};

//********************************************************************************************************************

ERR init_floodfx(void)
{
   clFloodFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::FLOODFX),
      fl::Name("FloodFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clFloodFXActions),
      fl::Fields(clFloodFXFields),
      fl::Size(sizeof(extFloodFX)),
      fl::Path(MOD_PATH));

   return clFloodFX ? ERR::Okay : ERR::AddClass;
}
