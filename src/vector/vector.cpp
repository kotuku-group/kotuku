/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

This code utilises the work of the FreeType Project under the FreeType License.  For more information please refer to
the FreeType home page at www.freetype.org.

*********************************************************************************************************************/

//#include "vector.h"
//#include "font.h"
#include "idl.h"

JUMPTABLE_DISPLAY
JUMPTABLE_CORE
JUMPTABLE_FONT

OBJECTPTR clVectorScene = nullptr, clVectorViewport = nullptr, clVectorGroup = nullptr, clVectorColour = nullptr;
OBJECTPTR clVectorEllipse = nullptr, clVectorRectangle = nullptr, clVectorPath = nullptr, clVectorWave = nullptr;
OBJECTPTR clVectorFilter = nullptr, clVectorPolygon = nullptr, clVectorText = nullptr, clVectorClip = nullptr;
OBJECTPTR clVectorGradient = nullptr, clVectorImage = nullptr, clVectorPattern = nullptr, clVector = nullptr;
OBJECTPTR clVectorSpiral = nullptr, clVectorShape = nullptr, clVectorTransition = nullptr, clImageFX = nullptr;
OBJECTPTR clBlurFX = nullptr, clColourFX = nullptr, clCompositeFX = nullptr, clConvolveFX = nullptr, clFilterEffect = nullptr;
OBJECTPTR clFloodFX = nullptr, clMergeFX = nullptr, clMorphologyFX = nullptr, clOffsetFX = nullptr, clTurbulenceFX = nullptr;
OBJECTPTR clSourceFX = nullptr, clRemapFX = nullptr, clLightingFX = nullptr, clDisplacementFX = nullptr, clWaveFunctionFX = nullptr;

static OBJECTPTR modDisplay = nullptr;
static OBJECTPTR modFont = nullptr;
OBJECTPTR glVectorModule = nullptr;
FT_Library glFTLibrary = nullptr;

std::recursive_mutex glVectorFocusLock;
std::vector<extVector *> glVectorFocusList; // The first reference is the most foreground object with the focus

std::recursive_mutex glFontMutex;
ankerl::unordered_dense::map<uint32_t, std::unique_ptr<bmp_font>> glBitmapFonts;
ankerl::unordered_dense::map<uint32_t, std::unique_ptr<freetype_font>> glFreetypeFonts;

static ERR init_clip(void);
static ERR init_ellipse(void);
static ERR init_group(void);
static ERR init_path(void);
static ERR init_polygon(void);
static ERR init_rectangle(void);
static ERR init_spiral(void);
static ERR init_supershape(void);
static ERR init_text(void);
static ERR init_vector(void);
static ERR init_viewport(void);
static ERR init_wave(void);

#include "utility.cpp"

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   pf::Log log;

   CoreBase = argCoreBase;

   argModule->get(FID_Root, glVectorModule);

   if (FT_Init_FreeType(&glFTLibrary)) {
      log.warning("Failed to initialise the FreeType font library.");
      return ERR::LoadModule;
   }

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("font", &modFont, &FontBase) != ERR::Okay) return ERR::InitModule;

   ERR error;
   if ((error = init_vectorscene()) != ERR::Okay) return error; // Base class
   if ((error = init_vector()) != ERR::Okay) return error;
   if ((error = init_colour()) != ERR::Okay) return error;
   if ((error = init_clip()) != ERR::Okay) return error;
   if ((error = init_filter()) != ERR::Okay) return error;
   if ((error = init_gradient()) != ERR::Okay) return error;
   if ((error = init_group()) != ERR::Okay) return error;
   if ((error = init_image()) != ERR::Okay) return error;
   // Shapes
   if ((error = init_path()) != ERR::Okay) return error;
   if ((error = init_ellipse()) != ERR::Okay) return error;
   if ((error = init_spiral()) != ERR::Okay) return error;
   if ((error = init_supershape()) != ERR::Okay) return error;
   if ((error = init_pattern()) != ERR::Okay) return error;
   if ((error = init_polygon()) != ERR::Okay) return error;
   if ((error = init_text()) != ERR::Okay) return error;
   if ((error = init_rectangle()) != ERR::Okay) return error;
   if ((error = init_transition()) != ERR::Okay) return error;
   if ((error = init_viewport()) != ERR::Okay) return error;
   if ((error = init_wave()) != ERR::Okay) return error;
   // Effects
   if ((error = init_filtereffect()) != ERR::Okay) return error;
   if ((error = init_imagefx()) != ERR::Okay) return error;
   if ((error = init_sourcefx()) != ERR::Okay) return error;
   if ((error = init_blurfx()) != ERR::Okay) return error;
   if ((error = init_colourfx()) != ERR::Okay) return error;
   if ((error = init_compositefx()) != ERR::Okay) return error;
   if ((error = init_convolvefx()) != ERR::Okay) return error;
   if ((error = init_floodfx()) != ERR::Okay) return error;
   if ((error = init_mergefx()) != ERR::Okay) return error;
   if ((error = init_morphfx()) != ERR::Okay) return error;
   if ((error = init_offsetfx()) != ERR::Okay) return error;
   if ((error = init_turbulencefx()) != ERR::Okay) return error;
   if ((error = init_remapfx()) != ERR::Okay) return error;
   if ((error = init_lightingfx()) != ERR::Okay) return error;
   if ((error = init_displacementfx()) != ERR::Okay) return error;
   if ((error = init_wavefunctionfx()) != ERR::Okay) return error;

   update_dpi();
   return error;
}

static ERR MODExpunge(void)
{
   glBitmapFonts.clear();
   glFreetypeFonts.clear();

   if (glFTLibrary) { FT_Done_FreeType(glFTLibrary); glFTLibrary = nullptr; }

   if (modDisplay) { FreeResource(modDisplay); modDisplay = nullptr; }
   if (modFont)    { FreeResource(modFont); modFont = nullptr; }

   // Sub-classes

   if (clVectorShape)      { FreeResource(clVectorShape);      clVectorShape = nullptr; }
   if (clVectorSpiral)     { FreeResource(clVectorSpiral);     clVectorSpiral = nullptr; }
   if (clVectorScene)      { FreeResource(clVectorScene);      clVectorScene = nullptr; }
   if (clVectorClip)       { FreeResource(clVectorClip);       clVectorClip = nullptr; }
   if (clVectorColour)     { FreeResource(clVectorColour);     clVectorColour = nullptr; }
   if (clVectorRectangle)  { FreeResource(clVectorRectangle);  clVectorRectangle = nullptr; }
   if (clVectorEllipse)    { FreeResource(clVectorEllipse);    clVectorEllipse = nullptr; }
   if (clVectorPath)       { FreeResource(clVectorPath);       clVectorPath = nullptr; }
   if (clVectorPolygon)    { FreeResource(clVectorPolygon);    clVectorPolygon = nullptr; }
   if (clVectorText)       { FreeResource(clVectorText);       clVectorText = nullptr; }
   if (clVectorGradient)   { FreeResource(clVectorGradient);   clVectorGradient = nullptr; }
   if (clVectorGroup)      { FreeResource(clVectorGroup);      clVectorGroup = nullptr; }
   if (clVectorViewport)   { FreeResource(clVectorViewport);   clVectorViewport = nullptr; }
   if (clVectorPattern)    { FreeResource(clVectorPattern);    clVectorPattern = nullptr; }
   if (clVectorFilter)     { FreeResource(clVectorFilter);     clVectorFilter = nullptr; }
   if (clVectorImage)      { FreeResource(clVectorImage);      clVectorImage = nullptr; }
   if (clVectorWave)       { FreeResource(clVectorWave);       clVectorWave = nullptr; }
   if (clVectorTransition) { FreeResource(clVectorTransition); clVectorTransition = nullptr; }

   if (clImageFX)          { FreeResource(clImageFX);          clImageFX = nullptr; }
   if (clSourceFX)         { FreeResource(clSourceFX);         clSourceFX = nullptr; }
   if (clBlurFX)           { FreeResource(clBlurFX);           clBlurFX = nullptr; }
   if (clColourFX)         { FreeResource(clColourFX);         clColourFX = nullptr; }
   if (clCompositeFX)      { FreeResource(clCompositeFX);      clCompositeFX = nullptr; }
   if (clConvolveFX)       { FreeResource(clConvolveFX);       clConvolveFX = nullptr; }
   if (clFloodFX)          { FreeResource(clFloodFX);          clFloodFX = nullptr; }
   if (clMergeFX)          { FreeResource(clMergeFX);          clMergeFX = nullptr; }
   if (clMorphologyFX)     { FreeResource(clMorphologyFX);     clMorphologyFX = nullptr; }
   if (clOffsetFX)         { FreeResource(clOffsetFX);         clOffsetFX = nullptr; }
   if (clTurbulenceFX)     { FreeResource(clTurbulenceFX);     clTurbulenceFX = nullptr; }
   if (clRemapFX)          { FreeResource(clRemapFX);          clRemapFX = nullptr; }
   if (clLightingFX)       { FreeResource(clLightingFX);       clLightingFX = nullptr; }
   if (clDisplacementFX)   { FreeResource(clDisplacementFX);   clDisplacementFX = nullptr; }
   if (clWaveFunctionFX)   { FreeResource(clWaveFunctionFX);   clWaveFunctionFX = nullptr; }

   // Base-classes

   if (clFilterEffect)     { FreeResource(clFilterEffect);     clFilterEffect = nullptr; }
   if (clVector)           { FreeResource(clVector);           clVector = nullptr; }

   return ERR::Okay;
}

//********************************************************************************************************************

#include "paths.cpp"

#include "vectors/vector.cpp"
#include "vectors/viewport.cpp"
#include "vectors/clip.cpp"
#include "vectors/path.cpp"
#include "vectors/text.cpp"
#include "vectors/group.cpp"
#include "vectors/ellipse.cpp"
#include "vectors/polygon.cpp"
#include "vectors/rectangle.cpp"
#include "vectors/spiral.cpp"
#include "vectors/supershape.cpp"
#include "vectors/wave.cpp"

//********************************************************************************************************************

extern ERR MODOpen(OBJECTPTR Module);

static STRUCTS glStructures = {
   { "GradientStop", sizeof(GradientStop) },
   { "MergeSource",  sizeof(MergeSource) },
   { "PathCommand",  sizeof(PathCommand) },
   { "Transition",   sizeof(Transition) },
   { "VectorMatrix", sizeof(VectorMatrix) },
   { "VectorPoint",  sizeof(VectorPoint) }
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, nullptr, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_vector_module() { return &ModHeader; }
