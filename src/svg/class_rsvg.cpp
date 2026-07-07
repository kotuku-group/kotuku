/*********************************************************************************************************************

-CLASS-
RSVG: Image-based SVG renderer providing bitmap integration for SVG documents.

The RSVG class extends the @Image class to provide seamless integration of SVG documents within bitmap-based image
workflows.  This renderer automatically handles SVG-to-bitmap conversion, enabling SVG content to be treated as
standard raster images within applications that primarily work with bitmap formats.

Key features include automatic format detection, scalable rendering with resolution adaptation, and transparent
handling of both standard (.svg) and compressed (.svgz) SVG files.

*********************************************************************************************************************/

#include "../image/image.h"

class extRSVG : public extImage {
public:
   // NB: Private variables aren't permitted for derived image classes

   // Storing the SVG object in DerivedPtr avoids overhead
   inline objSVG * svg() { return (objSVG *)DerivedPtr; }

   ~extRSVG() {
      // Manual termination required because Free's use of FreeResource treats DerivedPtr as a generic memory block.
      if (DerivedPtr) { FreeResource(((objSVG *)DerivedPtr)->UID); DerivedPtr = nullptr; }
   }

   extRSVG(objMetaClass *pClass, OBJECTID pUID) : extImage(pClass, pUID) { }
};

//********************************************************************************************************************

static ERR RSVG_Activate(extRSVG *Self)
{
   kt::Log log;
   log.branch();

   if (auto error = acQuery(Self); error != ERR::Okay) return error;

   auto bmp = Self->Bitmap;
   if (not bmp->initialised()) {
      if (InitObject(bmp) != ERR::Okay) return ERR::Init;
   }

   gfx::DrawRectangle(bmp, 0, 0, bmp->Width, bmp->Height, 0, BAF::FILL); // Black background
   Self->svg()->render(bmp, 0, 0, bmp->Width, bmp->Height);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR RSVG_Free(extRSVG *Self)
{
   Self->~extRSVG();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR RSVG_Init(extRSVG *Self)
{
   kt::Log log;

   std::string_view path;
   Self->getPath(path);

   if (path.empty() or ((Self->Flags & PCF::NEW) != PCF::NIL)) {
      return ERR::NoSupport; // Creating new SVG's is not supported in this module.
   }

   std::span<int8_t> header;

   if (wildcmp("*.svg|*.svgz", path));
   else if (!Self->getHeader(header)) {
      if (strisearch("<svg", std::string_view((CSTRING)header.data(), header.size())) >= 0) {
      }
      else return ERR::NoSupport;
   }
   else return ERR::NoSupport;

   log.trace("File \"%s\" is in SVG format.", path);

   Self->Flags |= PCF::SCALABLE;

   if ((Self->Flags & PCF::LAZY) != PCF::NIL) return ERR::Okay;
   return acActivate(Self);
}

//********************************************************************************************************************

static ERR RSVG_New(extRSVG *Self)
{
   new (Self) extRSVG(Self->Class, Self->UID);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR RSVG_Query(extRSVG *Self)
{
   kt::Log log;
   objBitmap *bmp;

   if (not (bmp = Self->Bitmap)) return log.warning(ERR::ObjectCorrupt);

   if (Self->Queried) return ERR::Okay;
   Self->Queried = TRUE;

   if (not Self->svg()) {
      std::string_view path;
      if (!Self->getPath(path)) {
         if ((Self->DerivedPtr = objSVG::create::local(fl::Path(path)))) {
         }
         else return log.warning(ERR::CreateObject);
      }
      else return log.warning(ERR::GetField);
   }

   objVectorScene *scene;
   ERR error;
   if ((!(error = Self->svg()->getScene(scene))) and (scene)) {
      if ((Self->Flags & PCF::FORCE_ALPHA_32) != PCF::NIL) {
         bmp->Flags |= BMF::ALPHA_CHANNEL;
         bmp->BitsPerPixel  = 32;
         bmp->BytesPerPixel = 4;
      }

      // Look for the viewport, represented by the <svg/> tag.

      objVector *view = scene->Viewport;
      while ((view) and (view->classID() != CLASSID::VECTORVIEWPORT)) view = view->Next;
      if (not view) {
         log.warning("SVG source file does not define a valid <svg/> tag.");
         return ERR::Failed;
      }

      // Check for fixed dimensions specified by the SVG.

      Unit view_width, view_height;
      ((objVectorViewport *)view)->getWidth(view_width);
      ((objVectorViewport *)view)->getHeight(view_height);

      // If the SVG source doesn't specify fixed dimensions, automatically force rescaling to the display width and height.

      if (not view_width)  ((objVectorViewport *)view)->setWidth(Unit(1.0, FD_SCALED));
      if (not view_height) ((objVectorViewport *)view)->setHeight(Unit(1.0, FD_SCALED));

      if ((Self->DisplayWidth > 0) and (Self->DisplayHeight > 0)) { // Client specified the display size?
         // Give the vector scene a target width and height.
         if (not view_width) scene->setPageWidth(Self->DisplayWidth);
         else scene->setPageWidth(view_width);

         if (not view_height) scene->setPageHeight(Self->DisplayHeight);
         else scene->setPageHeight(view_height);
      }

      if (not bmp->Width) {
         if (view_width) bmp->Width = view_width;
         else if (Self->DisplayWidth) bmp->Width = Self->DisplayWidth;
         if (not bmp->Width) bmp->Width = 1024;
      }

      if (not bmp->Height) {
         if (view_height) bmp->Height = view_height;
         else if (Self->DisplayHeight) bmp->Height = Self->DisplayHeight;
         if (not bmp->Height) bmp->Height = bmp->Width; // Equivalent to width in order to maintain a 1:1 scale
      }

      if (not Self->DisplayWidth)  Self->DisplayWidth  = bmp->Width;
      if (not Self->DisplayHeight) Self->DisplayHeight = bmp->Height;
      if (bmp->BitsPerPixel < 15) bmp->BitsPerPixel = 32;

      error = acQuery(bmp);
      return error;
   }
   else {
      log.trace("Failed to retrieve Vector from SVG.");
      return log.warning(error);
   }
}

//********************************************************************************************************************

static ERR RSVG_Resize(extRSVG *Self, struct acResize *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Self->svg()) {
      if (not Self->Bitmap->initialised()) {
         if (InitObject(Self->Bitmap) != ERR::Okay) return ERR::Init;
      }

      if (!Action(AC::Resize, Self->Bitmap, Args)) {
         objVectorScene *scene;
         if ((!Self->svg()->getScene(scene)) and (scene)) {
            scene->setPageWidth(Self->Bitmap->Width);
            scene->setPageHeight(Self->Bitmap->Height);

            gfx::DrawRectangle(Self->Bitmap, 0, 0, Self->Bitmap->Width, Self->Bitmap->Height, 0, BAF::FILL);
            acDraw(Self->svg());
         }
         else return ERR::GetField;

         return ERR::Okay;
      }
      else return ERR::Failed;
   }
   else return ERR::NotInitialised;
}

//********************************************************************************************************************

static const ActionArray clActions[] = {
   { AC::Activate,      RSVG_Activate },
   { AC::Free, RSVG_Free },
   { AC::Init,          RSVG_Init },
   { AC::New,  RSVG_New },
   { AC::Query,         RSVG_Query },
   { AC::Resize,        RSVG_Resize },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

static ERR init_rsvg(void)
{
   clRSVG = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::IMAGE),
      fl::ClassID(CLASSID::RSVG),
      fl::Name("RSVG"),
      fl::Category(CCF::GRAPHICS),
      fl::FileExtension("svg|svgz"),
      fl::FileDescription("SVG image"),
      fl::Actions(clActions),
      fl::Size(sizeof(extRSVG)),
      fl::Path(MOD_PATH));

   return clRSVG ? ERR::Okay : ERR::AddClass;
}
