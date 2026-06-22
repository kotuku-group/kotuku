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
   using create = kt::Create<extFloodFX>;

   FRGB   Colour;
   double Opacity = 1.0;
   RGB8   ColourRGB; // A cached conversion of the FRGB value

   extFloodFX() {
      SourceType = VSF::NONE;
   }
};

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

static ERR FLOODFX_SET_Colour(extFloodFX *Self, FRGB *Value)
{
   if (Value) {
      Self->Colour = *Value;
      Self->ColourRGB.Red   = std::clamp(Self->Colour.Red * 255.0, 0.0, 255.0);
      Self->ColourRGB.Green = std::clamp(Self->Colour.Green * 255.0, 0.0, 255.0);
      Self->ColourRGB.Blue  = std::clamp(Self->Colour.Blue * 255.0, 0.0, 255.0);
      Self->ColourRGB.Alpha = std::clamp(Self->Colour.Alpha * 255.0, 0.0, 255.0);
   }
   else Self->Colour.Alpha = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Opacity: Modifies the opacity of the flood colour.

*********************************************************************************************************************/

static ERR FLOODFX_SET_Opacity(extFloodFX *Self, double Value)
{
   kt::Log log;
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

static ERR FLOODFX_GET_XMLDef(extFloodFX *Self, std::string_view &Value)
{
   std::stringstream stream;

   stream << "feFlood opacity=\"" << Self->Opacity << "\"";

   auto cppstr = stream.str();
   if (auto str = strclone(stream.str())) {
      Value = std::string_view{str, cppstr.size()};
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

//********************************************************************************************************************

#include "filter_flood_def.c"

static const FieldArray clFloodFXFields[] = {
   { "Colour",  FDF_STRUCT|FDF_RW, nullptr, FLOODFX_SET_Colour, "FRGB" },
   { "Opacity", FDF_DOUBLE|FDF_RW, nullptr, FLOODFX_SET_Opacity },
   { "XMLDef",  FDF_VIRTUAL|FDF_CPPSTRING|FDF_ALLOC|FDF_R, FLOODFX_GET_XMLDef },
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
