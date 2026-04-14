/*********************************************************************************************************************

Please note that this is not an extension of the Vector class.  It is used for the purposes of filter definitions only.

-CLASS-
VectorFilter: Constructs filter pipelines that alter rendered vector graphics.

The VectorFilter class allows post-effect filters to be applied to vectors as they are being rendered.  Filter
support is closely modelled around the SVG standard, and effect results are intended to match that of the standard.
Once created, a filter can be utilised by vector objects through their @Vector.Filter field.  By way of example in
SVG:

<pre>
&lt;circle cx="160" cy="50" r="40" fill="#f00" filter="url(&#35;EffectPipeline)"/&gt;
</pre>

Filter pipelines are constructed from effects based on the @FilterEffect class, such as @CompositeFX and @FloodFX.
Construct a new pipeline by creating effect objects and placing them under the ownership of the VectorFilter that
will be supporting them.  The VectorFilter will detect the filter objects and they will be processed in the order
in which they are added.  It is most optimal to create each pipeline in advance, and a new VectorFilter object should
be created for each pipeline as necessary.

It is important to note that filter effects are CPU intensive tasks and real-time performance may be disappointing.
If this is an issue, consider pre-rendering the filter effects in advance and caching the results in memory or files.

It is a requirement that VectorFilter objects are owned by the @VectorScene they are targeting.

-END-

*********************************************************************************************************************/

//#define DEBUG_FILTER_BITMAP
//#define EXPORT_FILTER_BITMAP

struct target {
   double bound_x, bound_y, bound_width, bound_height;
   double x, y, width, height;
};

static ERR get_source_bitmap(extVectorFilter *, objBitmap **, VSF, objFilterEffect *, bool);

//********************************************************************************************************************
// Universal function for rendering a filter's Bitmap to the target region.
//
// No blending is performed because this is intended for use when there is no former input.  Copying is done
// with only the transforms applied (if any).  Linear RGB interpolation will wait until post processing.

template <class T> void render_to_filter(T *Self, objBitmap *Bitmap, ARF AspectRatio = ARF::NONE, VSM SampleMethod = VSM::AUTO)
{
   auto filter = Self->Filter;

   // The image's x,y,width,height default to (0,0,100%,100%) of the target region.

   double p_x = std::round(filter->TargetX), p_y = std::round(filter->TargetY);
   double p_width = std::round(filter->TargetWidth), p_height = std::round(filter->TargetHeight);

   if (filter->PrimitiveUnits IS VUNIT::BOUNDING_BOX) {
      // In this mode image dimensions typically remain at the default, i.e. (0,0,100%,100%) of the target.
      // If the user does set the XYWH of the image then 'fixed' coordinates act as multipliers, as if they were relative.

      // W3 spec on whether to use the bounds or the filter target region:
      // "Any length values within the filter definitions represent fractions or percentages of the bounding box
      // on the referencing element."

      if (dmf::hasAnyX(Self->Dimensions)) p_x = std::round(filter->TargetX + (Self->X * filter->BoundWidth));
      if (dmf::hasAnyY(Self->Dimensions)) p_y = std::round(filter->TargetY + (Self->Y * filter->BoundHeight));
      if (dmf::hasAnyWidth(Self->Dimensions)) p_width = std::round(Self->Width * filter->BoundWidth);
      if (dmf::hasAnyHeight(Self->Dimensions)) p_height = std::round(Self->Height * filter->BoundHeight);
   }
   else {
      if (dmf::hasScaledX(Self->Dimensions)) p_x = std::round(filter->TargetX + (Self->X * filter->TargetWidth));
      else if (dmf::hasX(Self->Dimensions))  p_x = std::round(Self->X);

      if (dmf::hasScaledY(Self->Dimensions)) p_y = std::round(filter->TargetY + (Self->Y * filter->TargetHeight));
      else if (dmf::hasY(Self->Dimensions))  p_y = std::round(Self->Y);

      if (dmf::hasScaledWidth(Self->Dimensions)) p_width = std::round(filter->TargetWidth * Self->Width);
      else if (dmf::hasWidth(Self->Dimensions))  p_width = std::round(Self->Width);

      if (dmf::hasScaledHeight(Self->Dimensions)) p_height = std::round(filter->TargetHeight * Self->Height);
      else if (dmf::hasHeight(Self->Dimensions))  p_height = std::round(Self->Height);
   }

   double x_scale = 1, y_scale = 1, align_x = 0, align_y = 0;
   calc_aspectratio("align_filter", AspectRatio, p_width, p_height, Bitmap->Width, Bitmap->Height, align_x, align_y, x_scale, y_scale);

   p_x += std::round(align_x);
   p_y += std::round(align_y);

   agg::trans_affine img_transform;
   img_transform.scale(x_scale, y_scale);
   img_transform.translate(p_x, p_y);
   img_transform *= filter->ClientVector->Transform;
   img_transform.invert();

   if (img_transform.is_complex()) {
      agg::rasterizer_scanline_aa<> raster;
      agg::renderer_base<agg::pixfmt_psl> renderBase;
      agg::pixfmt_psl pixDest(*Self->Target);
      agg::pixfmt_psl pixSource(*Bitmap);

      renderBase.attach(pixDest);
      renderBase.clip_box(Self->Target->Clip.Left, Self->Target->Clip.Top, Self->Target->Clip.Right-1, Self->Target->Clip.Bottom-1);

      agg::span_interpolator_linear<> interpolator(img_transform);

      agg::image_filter_lut ifilter;
      set_filter(ifilter, SampleMethod, img_transform);

      agg::span_once<agg::pixfmt_psl> source(pixSource, 0, 0);
      agg::span_image_filter_rgba<agg::span_once<agg::pixfmt_psl>, agg::span_interpolator_linear<>>
         spangen(source, interpolator, ifilter, false);

      set_raster_rect_path(raster, Self->Target->Clip.Left, Self->Target->Clip.Top,
         Self->Target->Clip.Right - Self->Target->Clip.Left,
         Self->Target->Clip.Bottom - Self->Target->Clip.Top);

      renderSolidBitmap(renderBase, raster, spangen); // Solid render without blending.
   }
   else gfx::CopyArea(Bitmap, Self->Target, BAF::NIL, 0, 0, Bitmap->Width, Bitmap->Height, -img_transform.tx, -img_transform.ty);
}

//********************************************************************************************************************

#include "filter_effect.cpp"
#include "filter_blur.cpp"
#include "filter_colourmatrix.cpp"
#include "filter_composite.cpp"
#include "filter_convolve.cpp"
#include "filter_displacement.cpp"
#include "filter_flood.cpp"
#include "filter_image.cpp"
#include "filter_lighting.cpp"
#include "filter_merge.cpp"
#include "filter_morphology.cpp"
#include "filter_offset.cpp"
#include "filter_remap.cpp"
#include "filter_source.cpp"
#include "filter_turbulence.cpp"
#include "filter_wavefunction.cpp"

//********************************************************************************************************************
// Compute the Target* and Bound* values, which are used by filter effect algorithms to determine placement.  They
// reflect positions *without* transforms.  The caller is expected to apply ClientVector->Transform after making
// normalised coordinate calculations.
//
// The Target* values tell the effects exactly where to render to.
//
// BoundsWidth/Height reflect the bounds of the client vector and its children.  These values are to
// be used by effects to compute their area when PrimitiveUnits = BOUNDING_BOX.

static void compute_target_area(extVectorFilter *Self)
{
   TClipRectangle<double> bounds = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0, 0 };
   //TClipRectangle<double> bounds = { Self->ClientViewport->vpFixedWidth, Self->ClientViewport->vpFixedHeight, 0, 0 };
   calc_full_boundary(Self->ClientVector, bounds, false, false);
   double boundX = std::round(bounds.left);
   double boundY = std::round(bounds.top);
   Self->BoundWidth  = std::round(bounds.width());
   Self->BoundHeight = std::round(bounds.height());

   if (Self->Units IS VUNIT::BOUNDING_BOX) {
      if (dmf::hasX(Self->Dimensions)) Self->TargetX = boundX;
      else if (dmf::hasScaledX(Self->Dimensions)) Self->TargetX = std::round(boundX + (Self->X * Self->BoundWidth));
      else Self->TargetX = boundX;

      if (dmf::hasY(Self->Dimensions)) Self->TargetY = boundY;
      else if (dmf::hasScaledY(Self->Dimensions)) Self->TargetY = std::round(boundY + (Self->Y * Self->BoundHeight));
      else Self->TargetY = boundY;

      if (dmf::hasWidth(Self->Dimensions)) Self->TargetWidth = std::round(Self->Width * Self->BoundWidth);
      else if (dmf::hasScaledWidth(Self->Dimensions)) Self->TargetWidth = std::round(Self->Width * Self->BoundWidth);
      else Self->TargetWidth = Self->BoundWidth;

      if (dmf::hasHeight(Self->Dimensions)) Self->TargetHeight = std::round(Self->Height * Self->BoundHeight);
      else if (dmf::hasScaledHeight(Self->Dimensions)) Self->TargetHeight = std::round(Self->Height * Self->BoundHeight);
      else Self->TargetHeight = Self->BoundHeight;
   }
   else { // USERSPACE: Scaled dimensions are measured against the client's viewport rather than the vector.
      if (dmf::hasX(Self->Dimensions)) Self->TargetX = std::round(Self->X);
      else if (dmf::hasScaledX(Self->Dimensions)) Self->TargetX = std::round(Self->X * Self->ClientViewport->vpFixedWidth);
      else Self->TargetX = boundX;

      if (dmf::hasY(Self->Dimensions)) Self->TargetY = std::round(Self->Y);
      else if (dmf::hasScaledY(Self->Dimensions)) Self->TargetY = std::round(Self->Y * Self->ClientViewport->vpFixedHeight);
      else Self->TargetY = boundY;

      if (dmf::hasWidth(Self->Dimensions)) Self->TargetWidth = Self->Width;
      else if (dmf::hasScaledWidth(Self->Dimensions)) Self->TargetWidth = std::round(Self->Width * Self->ClientViewport->vpFixedWidth);
      else Self->TargetWidth = Self->ClientViewport->vpFixedWidth;

      if (dmf::hasHeight(Self->Dimensions)) Self->TargetHeight = Self->Height;
      else if (dmf::hasScaledHeight(Self->Dimensions)) Self->TargetHeight = std::round(Self->Height * Self->ClientViewport->vpFixedHeight);
      else Self->TargetHeight = Self->ClientViewport->vpFixedHeight;
   }
}

//********************************************************************************************************************
// Return a bitmap from the bank.  In order to save memory, bitmap data is managed internally so that it always
// reflects the size of the clipping region.  The bitmap's size reflects the Filter's (X,Y), (Width,Height) values in
// accordance with the unit setting.

static ERR get_banked_bitmap(extVectorFilter *Self, objBitmap **BitmapResult)
{
   pf::Log log(__FUNCTION__);

   auto bi = Self->BankIndex;
   if (bi >= 255) return log.warning(ERR::ArrayFull);

   if (bi >= Self->Bank.size()) Self->Bank.emplace_back(std::make_unique<filter_bitmap>());

   #ifdef DEBUG_FILTER_BITMAP
      auto bmp = Self->Bank[bi].get()->get_bitmap(Self->ClientViewport->Scene->PageWidth, Self->ClientViewport->Scene->PageHeight, Self->VectorClip, true);
   #else
      auto bmp = Self->Bank[bi].get()->get_bitmap(Self->ClientViewport->Scene->PageWidth, Self->ClientViewport->Scene->PageHeight, Self->VectorClip, false);
   #endif

   if (bmp) {
      bmp->ColourSpace = CS::SRGB;
      bmp->Flags &= ~BMF::PREMUL;
      *BitmapResult = bmp;
      Self->BankIndex++;
      return ERR::Okay;
   }
   else return log.warning(ERR::CreateObject);
}

//********************************************************************************************************************
// Returns a rendered bitmap that represents the source.  Where possible, if a bitmap is being referenced then that
// reference will be returned.  Otherwise a new bitmap is allocated and rendered with the effect.  The bitmap must
// not be freed as they are permanently maintained until the VectorFilter is destroyed.

static ERR get_source_bitmap(extVectorFilter *Self, objBitmap **BitmapResult, VSF SourceType, objFilterEffect *Effect, bool Premultiply)
{
   pf::Log log(__FUNCTION__);

   if (!BitmapResult) return log.warning(ERR::NullArgs);

   pf::SwitchContext ctx(Self);

   log.branch("%s #%d <- ID: #%u, Type: %d", Self->ActiveEffect->Class->ClassName, Self->ActiveEffect->UID, Effect ? Effect->UID : 0, int(SourceType));

   objBitmap *bmp = nullptr;
   if (SourceType IS VSF::GRAPHIC) { // SourceGraphic: Render the source vector without transformations (transforms will be applied in the final steps).
      if (auto error = get_banked_bitmap(Self, &bmp); error != ERR::Okay) return log.warning(error);
      if (auto sg = get_source_graphic(Self)) {
         gfx::CopyArea(sg, bmp, BAF::NIL, sg->Clip.Left, sg->Clip.Top,
            sg->Clip.Right - sg->Clip.Left, sg->Clip.Bottom - sg->Clip.Top,
            bmp->Clip.Left, bmp->Clip.Top);
      }
   }
   else if (SourceType IS VSF::ALPHA) { // SourceAlpha
      if (auto error = get_banked_bitmap(Self, &bmp); error != ERR::Okay) return log.warning(error);
      if (auto sg = get_source_graphic(Self)) {
         int dy = bmp->Clip.Top;
         for (int sy=sg->Clip.Top; sy < sg->Clip.Bottom; sy++) {
            uint32_t *src = (uint32_t *)(sg->Data + (sy * sg->LineWidth));
            uint32_t *dest = (uint32_t *)(bmp->Data + (dy * bmp->LineWidth));
            int dx = bmp->Clip.Left;
            for (int sx=sg->Clip.Left; sx < sg->Clip.Right; sx++) {
               dest[dx++] = src[sx] & 0xff000000;
            }
            dy++;
         }
      }
   }
   else if (SourceType IS VSF::BKGD) {
      // "Represents an image snapshot of the canvas under the filter region at the time that the filter element is invoked."
      // NOTE: The client needs to specify 'enable-background' in the nearest container element in order to indicate where the background is coming from;
      // additionally it serves as a marker for graphics to be rendered to a separate bitmap (essential for coping with any transformations in the scene graph).
      //
      // Refer to enable-background support in scene_draw.cpp

      if (auto error = get_banked_bitmap(Self, &bmp); error != ERR::Okay) return log.warning(error);

      if ((Self->BkgdBitmap) and ((Self->BkgdBitmap->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
         gfx::CopyArea(Self->BkgdBitmap, bmp, BAF::NIL, Self->VectorClip.left, Self->VectorClip.top,
            Self->VectorClip.right - Self->VectorClip.left, Self->VectorClip.bottom - Self->VectorClip.top,
            bmp->Clip.Left, bmp->Clip.Top);
      }
   }
   else if (SourceType IS VSF::BKGD_ALPHA) {
      if (auto error = get_banked_bitmap(Self, &bmp); error != ERR::Okay) return log.warning(error);
      if ((Self->BkgdBitmap) and ((Self->BkgdBitmap->Flags & BMF::ALPHA_CHANNEL) != BMF::NIL)) {
         int dy = bmp->Clip.Top;
         for (int sy=Self->BkgdBitmap->Clip.Top; sy < Self->BkgdBitmap->Clip.Bottom; sy++) {
            auto src = (uint32_t *)(Self->BkgdBitmap->Data + (sy * Self->BkgdBitmap->LineWidth));
            auto dest = (uint32_t *)(bmp->Data + (dy * bmp->LineWidth));
            int dx = bmp->Clip.Left;
            for (int sx=Self->BkgdBitmap->Clip.Left; sx < Self->BkgdBitmap->Clip.Right; sx++) {
               dest[dx++] = src[sx] & 0xff000000;
            }
            dy++;
         }
      }
   }
   else if (SourceType IS VSF::REFERENCE) {
      if (auto e = Effect) {
         // Find first effect in the hierarchy that outputs a bitmap.

         while ((!bmp) and (e)) {
            bmp = e->Target;
            e = e->Input;
         }

         if (!bmp) {
            log.warning("%s has dependency on %s effect #%u and does not output a bitmap.", Self->ActiveEffect->Class->ClassName, Effect->Class->ClassName, Effect->UID);
            return ERR::NoData;
         }
      }
      else {
         log.warning("%s source reference has not provided an effect.", Self->ActiveEffect->Class->ClassName);
         return ERR::NoData;
      }
  }
   else if (SourceType IS VSF::NONE) {
      *BitmapResult = nullptr;
      return ERR::Continue;
   }
   else {
      log.warning("Effect source %d is not supported.", int(SourceType));
      return ERR::NoSupport;
   }

   #if defined(EXPORT_FILTER_BITMAP) && defined (DEBUG_FILTER_BITMAP)
      save_bitmap(bmp, std::to_string(Self->UID) + "_" + std::to_string(Self->ClientVector->UID) + "_source");
   #endif

   if (Premultiply) bmp->premultiply();

   *BitmapResult = bmp;
   return ERR::Okay;
}

//********************************************************************************************************************
// Render the vector client(s) to an internal bitmap that can be used for SourceGraphic and SourceAlpha input.
// If the referenced vector has no content then the result is a bitmap cleared to 0x00000000, as per SVG specs.
// Rendering will occur only once to SourceGraphic, so multiple calls to this function in a filter pipeline are OK.
//
// TODO: It would be efficient to hook into the dirty markers of the client vector so that re-rendering
// occurs only in the event that the client has been modified.

objBitmap * get_source_graphic(extVectorFilter *Self)
{
   pf::Log log(__FUNCTION__);

   if (!Self->ClientVector) {
      log.warning("%s No ClientVector defined.", Self->ActiveEffect->Class->ClassName);
      return nullptr;
   }

   if (Self->Rendered) return Self->SourceGraphic; // Source bitmap already exists and drawn at the correct size.

   pf::SwitchContext ctx(Self);

   if (!Self->SourceGraphic) {
      // The BlendMode is set to SRGB for the sake of SVG compatibility.  Otherwise the use of filters
      // like feColorMatrix can produce unexpected results.

      if (!(Self->SourceGraphic = objBitmap::create::local(fl::Name("source_graphic"),
         fl::Width(Self->ClientViewport->Scene->PageWidth),
         fl::Height(Self->ClientViewport->Scene->PageHeight),
         fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL),
         fl::BlendMode(BLM::SRGB),
         fl::ColourSpace(CS::SRGB)))) return nullptr;
   }
   else if ((Self->ClientViewport->Scene->PageWidth > Self->SourceGraphic->Width) or
            (Self->ClientViewport->Scene->PageHeight > Self->SourceGraphic->Height)) {
      Self->SourceGraphic->resize(Self->ClientViewport->Scene->PageWidth, Self->ClientViewport->Scene->PageHeight);
   }

   if (!Self->SourceScene) {
      if ((Self->SourceScene = extVectorScene::create::local(
            fl::PageWidth(Self->ClientViewport->Scene->PageWidth),
            fl::PageHeight(Self->ClientViewport->Scene->PageHeight)))) {

         if (!extVectorViewport::create::global(fl::Owner(Self->SourceScene->UID),
               fl::ColourSpace(Self->ColourSpace))) return nullptr;
      }
      else return nullptr;
   }
   else if ((Self->ClientViewport->Scene->PageWidth > Self->SourceGraphic->Width) or
            (Self->ClientViewport->Scene->PageHeight > Self->SourceGraphic->Height)) {
      acResize(Self->SourceScene, Self->ClientViewport->Scene->PageWidth, Self->ClientViewport->Scene->PageHeight, 0);
   }

   auto const save_child = Self->SourceScene->Viewport->Child;
   Self->SourceScene->Viewport->Child = Self->ClientVector;
   Self->SourceGraphic->Clip = { Self->VectorClip.left, Self->VectorClip.top, Self->VectorClip.right, Self->VectorClip.bottom };

   if (Self->SourceGraphic->Clip.Top < 0)  Self->SourceGraphic->Clip.Top = 0;
   if (Self->SourceGraphic->Clip.Left < 0) Self->SourceGraphic->Clip.Left = 0;
   if (Self->SourceGraphic->Clip.Bottom > Self->SourceGraphic->Height) Self->SourceGraphic->Clip.Bottom = Self->SourceGraphic->Height;
   if (Self->SourceGraphic->Clip.Right  > Self->SourceGraphic->Width)  Self->SourceGraphic->Clip.Right  = Self->SourceGraphic->Width;

   // These non-fatal clipping checks will trigger if vector bounds lie outside of the visible/drawable area.
   if (Self->SourceGraphic->Clip.Top >= Self->SourceGraphic->Clip.Bottom) return nullptr;
   if (Self->SourceGraphic->Clip.Left >= Self->SourceGraphic->Clip.Right) return nullptr;

   auto const save_vector = Self->ClientVector->Next; // Switch off the Next pointer to prevent processing of siblings.
   Self->ClientVector->Next = nullptr;
   Self->Disabled = true; // Turning off the filter is required to prevent infinite recursion.

   gfx::DrawRectangle(Self->SourceGraphic, 0, 0, Self->SourceGraphic->Width, Self->SourceGraphic->Height, 0x00000000, BAF::FILL);
   Self->SourceScene->Bitmap = Self->SourceGraphic;
   acDraw(Self->SourceScene);

   Self->Disabled = false;
   Self->ClientVector->Next = save_vector;
   Self->SourceScene->Viewport->Child = save_child;

   Self->Rendered = true;
   return Self->SourceGraphic;
}

//********************************************************************************************************************
// Defines the VectorClip values, which are utilised by the filter renderers.

static ERR set_clip_region(extVectorFilter *Self, extVectorViewport *Viewport, extVector *Vector)
{
   pf::Log log(__FUNCTION__);

   const double container_width  = Viewport->vpFixedWidth;
   const double container_height = Viewport->vpFixedHeight;

   if ((container_width < 1) or (container_height < 1)) {
      log.warning("Viewport #%d has no size.", Viewport->UID);
      return ERR::NothingDone;
   }

   if (Self->Units IS VUNIT::BOUNDING_BOX) {
      // All coordinates are relative to the client vector, or vectors if we are applied to a group.
      // The bounds are oriented to the client vector's transforms.

      TClipRectangle<double> bounds = { std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), 0, 0 };
      calc_full_boundary(Vector, bounds, false /* siblings */, true /* transforms */);

      if ((bounds.right <= bounds.left) or (bounds.bottom <= bounds.top)) {
         // No child vector defines a path for a SourceGraphic.  Default back to the viewport.
         bounds = Viewport->vpBounds;
      }
      auto const bound_width  = bounds.width();
      auto const bound_height = bounds.height();

      if (dmf::hasX(Self->Dimensions)) Self->VectorClip.left = std::round(bounds.left + Self->X);
      else if (dmf::hasScaledX(Self->Dimensions)) Self->VectorClip.left = std::round(bounds.left + (Self->X * bound_width));
      else Self->VectorClip.left = std::round(bounds.left);

      if (dmf::hasY(Self->Dimensions)) Self->VectorClip.top = std::round(bounds.top + Self->Y);
      else if (dmf::hasScaledY(Self->Dimensions)) Self->VectorClip.top = std::round(bounds.top + (Self->Y * bound_height));
      else Self->VectorClip.top = std::round(bounds.top);

      if (dmf::hasWidth(Self->Dimensions)) Self->VectorClip.right = Self->VectorClip.left + std::round(Self->Width * bound_width);
      else if (dmf::hasScaledWidth(Self->Dimensions)) Self->VectorClip.right = Self->VectorClip.left + std::round(Self->Width * bound_width);
      else Self->VectorClip.right = Self->VectorClip.left + std::round(bound_width);

      if (dmf::hasHeight(Self->Dimensions)) Self->VectorClip.bottom = Self->VectorClip.top + std::round(Self->Height * bound_height);
      else if (dmf::hasScaledHeight(Self->Dimensions)) Self->VectorClip.bottom = Self->VectorClip.top + std::round(Self->Height * bound_height);
      else Self->VectorClip.bottom = Self->VectorClip.top + std::round(bound_height);
   }
   else { // USERSPACE
      double x, y, w, h;
      if (dmf::hasX(Self->Dimensions)) x = std::round(Self->X);
      else if (dmf::hasScaledX(Self->Dimensions)) x = std::round(Self->X * container_width);
      else x = 0;

      if (dmf::hasY(Self->Dimensions)) y = std::round(Self->Y);
      else if (dmf::hasScaledY(Self->Dimensions)) y = std::round(Self->Y * container_height);
      else y = 0;

      if (dmf::hasWidth(Self->Dimensions)) w = std::round(Self->Width);
      else if (dmf::hasScaledWidth(Self->Dimensions)) w = std::round(Self->Width * container_width);
      else w = std::round(container_width);

      if (dmf::hasHeight(Self->Dimensions)) h = std::round(Self->Height);
      else if (dmf::hasScaledHeight(Self->Dimensions)) h = std::round(Self->Height * container_height);
      else h = std::round(container_height);

      agg::path_storage rect;
      rect.move_to(x, y);
      rect.line_to(x + w, y);
      rect.line_to(x + w, y + h);
      rect.line_to(x, y + h);
      rect.close_polygon();

      agg::conv_transform<agg::path_storage, agg::trans_affine> path(rect, Vector->Transform);
      Self->VectorClip = get_bounds<agg::conv_transform<agg::path_storage, agg::trans_affine>, int>(path);
   }

   if (Self->VectorClip.left < Viewport->vpBounds.left)     Self->VectorClip.left   = Viewport->vpBounds.left;
   if (Self->VectorClip.top  < Viewport->vpBounds.top)      Self->VectorClip.top    = Viewport->vpBounds.top;
   if (Self->VectorClip.right  > Viewport->vpBounds.right)  Self->VectorClip.right  = Viewport->vpBounds.right;
   if (Self->VectorClip.bottom > Viewport->vpBounds.bottom) Self->VectorClip.bottom = Viewport->vpBounds.bottom;

   if ((Self->VectorClip.bottom <= Self->VectorClip.top) or (Self->VectorClip.right <= Self->VectorClip.left)) {
      return log.warning(ERR::InvalidDimension);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Main rendering routine for filter effects.  Called by the scene graph renderer whenever a vector uses a filter.

ERR render_filter(extVectorFilter *Self, extVectorViewport *Viewport, extVector *Vector, objBitmap *BkgdBitmap, objBitmap **Output)
{
   pf::Log log(__FUNCTION__);

   if (!Vector) return log.warning(ERR::NullArgs);
   if (Self->Disabled) return ERR::NothingDone;
   if (!Self->Effects) return log.warning(ERR::UndefinedField);

   pf::SwitchContext context(Self);

   CSTRING filter_name = Self->Name;
   if ((!filter_name) or (!filter_name[0])) filter_name = "Unnamed";
   CSTRING vector_name = Vector->Name;
   if ((!vector_name) or (!vector_name[0])) vector_name = "Unnamed";
   log.branch("Rendering '%s' filter content for %s #%d '%s'.", filter_name, Vector->Class->ClassName, Vector->UID, vector_name);

   Self->ClientViewport = Viewport;
   Self->ClientVector   = Vector;
   Self->BkgdBitmap     = BkgdBitmap; // For VSF::BKGD and VSF::BKGD_ALPHA
   Self->Rendered       = false; // Set to true when SourceGraphic is rendered
   Self->BankIndex      = 0;

   if (auto error = set_clip_region(Self, Viewport, Vector); error != ERR::Okay) return error;

   // Calculate Self->Target* and Self->Bound* values

   compute_target_area(Self);

   // Render the effect pipeline in sequence.  Linked effects get their own bitmap, everything else goes to a shared
   // output bitmap.  After all effects are rendered, the shared output bitmap is returned for rendering to the scene graph.
   //
   // * Effects may request the SourceGraphic, in which case we render the client vector to a separate scene graph
   //   and without transforms.
   //
   // TODO: Effects that don't have dependencies could be threaded.  Big pipelines could benefit from effects
   // being rendered to independent bitmaps in threads, then composited at the last stage.

   objBitmap *out = nullptr;
   for (auto e = Self->Effects; e; e = (extFilterEffect *)e->Next) {
      log.detail("Effect: %s #%u, Pipelined: %c; Use Count: %d", e->Class->ClassName, e->UID, e->UsageCount > 0 ? 'Y' : 'N', e->UsageCount);

      Self->ActiveEffect = e;

      if (e->UsageCount > 0) { // This effect is an input to something else
         if (auto error = get_banked_bitmap(Self, &e->Target); error != ERR::Okay) return error;
         gfx::DrawRectangle(e->Target, 0, 0, e->Target->Width, e->Target->Height, 0x00000000, BAF::FILL);
      }
      else { // This effect can render directly to the shared output bitmap
         if (!out) {
            if (auto error = get_banked_bitmap(Self, &out); error != ERR::Okay) return error;
            gfx::DrawRectangle(out, 0, 0, out->Width, out->Height, 0x00000000, BAF::FILL);
         }
         e->Target = out;
      }

      acDraw(e);
   }
   Self->ActiveEffect = nullptr;

   if (!out) {
      log.warning("Effect pipeline did not produce an output bitmap.");
      if (auto error = get_banked_bitmap(Self, &out); error != ERR::Okay) return error;
      gfx::DrawRectangle(out, 0, 0, out->Width, out->Height, 0x00000000, BAF::FILL);
   }

   #if defined(EXPORT_FILTER_BITMAP) && defined (DEBUG_FILTER_BITMAP)
      save_bitmap(out, std::to_string(Self->UID) + "_" + std::to_string(Vector->UID) + "_output");
   #endif

   #ifdef DEBUG_FILTER_BITMAP
      gfx::DrawRectangle(out, out->Clip.Left, out->Clip.Top, out->Clip.Right-out->Clip.Left, out->Clip.Bottom-out->Clip.Top, 0xff0000ff, BAF::NIL);
   #endif

   *Output = out;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clear: Removes all filter effects.
-END-
*********************************************************************************************************************/

static ERR VECTORFILTER_Clear(extVectorFilter *Self)
{
   pf::Log log;

   log.branch();
   while (Self->Effects) FreeResource(Self->Effects);

   Self->Bank.clear();
   Self->BankIndex = 0;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORFILTER_Free(extVectorFilter *Self)
{
   acClear(Self);

   if (Self->SourceGraphic) { FreeResource(Self->SourceGraphic); Self->SourceGraphic = nullptr; }
   if (Self->SourceScene)   { FreeResource(Self->SourceScene);   Self->SourceScene = nullptr; }

   Self->~extVectorFilter();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORFILTER_Init(extVectorFilter *Self)
{
   pf::Log log(__FUNCTION__);

   if ((int(Self->Units) <= 0) or (int(Self->Units) >= int(VUNIT::END))) {
      log.traceWarning("Invalid Units value of %d", Self->Units);
      return log.warning(ERR::OutOfRange);
   }

   if (!Self->Scene) return log.warning(ERR::UnsupportedOwner);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORFILTER_NewChild(extVectorFilter *Self, struct acNewChild *Args)
{
   if (!Args) return ERR::NullArgs;

   if (Args->Object->Class->BaseClassID IS CLASSID::FILTEREFFECT) {
      auto effect = (extFilterEffect *)Args->Object;

      if (!Self->Effects) Self->Effects = effect;
      else if (Self->LastEffect) Self->LastEffect->Next = effect;

      effect->Prev = Self->LastEffect;
      effect->Next = nullptr;
      Self->LastEffect = effect;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORFILTER_NewPlacement(extVectorFilter *Self)
{
   new (Self) extVectorFilter;
   Self->Units          = VUNIT::BOUNDING_BOX;
   Self->PrimitiveUnits = VUNIT::UNDEFINED;
   Self->Opacity        = 1.0;
   Self->X              = -0.1; // -10% default as per SVG requirements
   Self->Y              = -0.1;
   Self->Width          = 1.2;  // +120% default as per SVG requirements
   Self->Height         = 1.2;
   Self->AspectRatio    = VFA::MEET; // Scale X/Y values independently
   Self->ColourSpace    = VCS::SRGB; // Our preferred colour-space is sRGB for speed.  Note that the SVG class will change this to linear by default.
   Self->Dimensions     = DMF::SCALED_X|DMF::SCALED_Y|DMF::SCALED_WIDTH|DMF::SCALED_HEIGHT;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORFILTER_NewOwner(extVectorFilter *Self, struct acNewOwner *Args)
{
   if (Args->NewOwner->classID() IS CLASSID::VECTORSCENE) {
      Self->Scene = (extVectorScene *)Args->NewOwner;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ColourSpace: The colour space of the filter graphics (SRGB or linear RGB).

By default, colour filters are processed in SRGB format.  This is the same colour space as used by the rest of the
graphics system, which means that no special conversion is necessary prior to and post filter processing.  However,
linear RGB is better suited for producing high quality results at a cost of speed.

Note that if SVG compatibility is required, linear RGB must be used as the default.

-FIELD-
Dimensions: Dimension flags define whether individual dimension fields contain fixed or scaled values.

The following dimension flags are supported:

<types lookup="DMF">
<type name="FIXED_X">The #X value is a fixed coordinate.</>
<type name="FIXED_Y">The #Y value is a fixed coordinate.</>
<type name="SCALED_X">The #X value is a scaled coordinate.</>
<type name="SCALED_Y">The #Y value is a scaled coordinate.</>
<type name="FIXED_WIDTH">The #Width value is a fixed coordinate.</>
<type name="FIXED_HEIGHT">The #Height value is a fixed coordinate.</>
<type name="SCALED_WIDTH">The #Width value is a scaled coordinate.</>
<type name="SCALED_HEIGHT">The #Height value is a scaled coordinate.</>
</types>

-FIELD-
EffectXML: Returns a SVG XML string that defines the filter's effects.

This field value will return a purpose-built string that defines the filter's effects in SVG compliant XML.  The string
is allocated and must be freed once no longer in use.

*********************************************************************************************************************/

static ERR VECTORFILTER_GET_EffectXML(extVectorFilter *Self, CSTRING *Value)
{
   std::stringstream ss;

   for (auto e = Self->Effects; e; e = (extFilterEffect *)e->Next) {
      ss << "<";
      CSTRING def;
      if (e->get(FID_XMLDef, def) IS ERR::Okay) {
         ss << def;
         FreeResource(def);
      }
      ss << "/>";
   }

   if ((*Value = strclone(ss.str()))) return ERR::Okay;
   else return ERR::AllocMemory;
}

/*********************************************************************************************************************

-FIELD-
Height: The height of the filter area.  Can be expressed as a fixed or scaled coordinate.

The height of the filter area is expressed here as a fixed or scaled coordinate.  The #Width and Height effectively
restrain the working space for the effect processing, making them an important consideration for efficiency.

The coordinate system for the width and height depends on the value for #Units.

The default values for #Width and Height is `120%`, as per the SVG standard.  This provides a buffer space for the
filter algorithms to work with, and is usually a sufficient default.

*********************************************************************************************************************/

static ERR VECTORFILTER_GET_Height(extVectorFilter *Self, Unit *Value)
{
   Value->set(Self->Height);
   return ERR::Okay;
}

static ERR VECTORFILTER_SET_Height(extVectorFilter *Self, Unit &Value)
{
   if (Value > 0) {
      if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_HEIGHT) & (~DMF::FIXED_HEIGHT);
      else Self->Dimensions = (Self->Dimensions | DMF::FIXED_HEIGHT) & (~DMF::SCALED_HEIGHT);
      Self->Height = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
Inherit: Inherit attributes from a VectorFilter referenced here.

Attributes can be inherited from another filter by referencing that gradient in this field.  This feature is provided
primarily for the purpose of simplifying SVG compatibility and its use may result in an unnecessary performance penalty.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_Inherit(extVectorFilter *Self, extVectorFilter *Value)
{
   if (Value) {
      if (Value->Class->BaseClassID IS CLASSID::VECTORFILTER) Self->Inherit = Value;
      else return ERR::InvalidValue;
   }
   else Self->Inherit = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Opacity: The opacity of the filter.

The opacity of the filter is defined as a value between 0.0 and 1.0, with 1.0 being fully opaque.  The default value
is 1.0.

*********************************************************************************************************************/

static ERR VECTORFILTER_SET_Opacity(extVectorFilter *Self, double Value)
{
   if (Value < 0.0) Value = 0;
   else if (Value > 1.0) Value = 1.0;
   Self->Opacity = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
PrimitiveUnits: Alters the behaviour of some effects that support alternative position calculations.

PrimitiveUnits alters the behaviour of some effects when their dimensions are calculated.  The default value is
`USERSPACE`.  When set to `BOUNDING_BOX`, the effect may calculate its dimensions strictly based on the client vector
using a scaled coordinate space of `(0, 0, 100%, 100%)`.

-FIELD-
ResX: Width of the intermediate images, measured in pixels.

The combination of ResX and #ResY define the available space for rendering of filter effects.  It is recommended that
the client does not set these values because the default `1:1` pixel ratio is appropriate in the majority of
circumstances.

-FIELD-
ResY: Height of the intermediate images, measured in pixels.

The combination of #ResX and ResY define the available space for rendering of filter effects.  It is recommended that
the client does not set these values because the default `1:1` pixel ratio is appropriate in the majority of
circumstances.

-FIELD-
Units: Defines the coordinate system for #X, #Y, #Width and #Height.

The default coordinate system is `BOUNDING_BOX`, which positions the filter within the client vector.
The alternative is `USERSPACE`, which positions the filter relative to the client vector's nearest viewport.

-FIELD-
Width: The width of the filter area.  Can be expressed as a fixed or scaled coordinate.

The width of the filter area is expressed here as a fixed or scaled coordinate.  The Width and #Height effectively
restrain the working space for the effect processing, making them an important consideration for efficiency.

The coordinate system for the width and height depends on the value for #Units.

The default values for #Width and Height is `120%`, as per the SVG standard.  This provides a buffer space for the
filter algorithms to work with, and is usually a sufficient default.

*********************************************************************************************************************/

static ERR VECTORFILTER_GET_Width(extVectorFilter *Self, Unit *Value)
{
   Value->set(Self->Width);
   return ERR::Okay;
}

static ERR VECTORFILTER_SET_Width(extVectorFilter *Self, Unit &Value)
{
   if (Value > 0) {
      if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_WIDTH) & (~DMF::FIXED_WIDTH);
      else Self->Dimensions = (Self->Dimensions | DMF::FIXED_WIDTH) & (~DMF::SCALED_WIDTH);

      Self->Width = Value;
      return ERR::Okay;
   }
   else return ERR::InvalidValue;
}

/*********************************************************************************************************************
-FIELD-
X: X coordinate for the filter.

The meaning of the (X, #Y) field values depend on the value for #Units.  In userspace mode, the filter position will be
relative to the client vector's parent viewport.  In bounding-box mode, the filter position is relative to the
vector's position.  It is important to note that coordinates are measured before any transforms are applied.

The default values for X and #Y is `10%`, as per the SVG standard.  This provides a buffer space for the filter
algorithms to work with, and is usually a sufficient default.

*********************************************************************************************************************/

static ERR VECTORFILTER_GET_X(extVectorFilter *Self, Unit *Value)
{
   Value->set(Self->X);
   return ERR::Okay;
}

static ERR VECTORFILTER_SET_X(extVectorFilter *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_X) & (~DMF::FIXED_X);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_X) & (~DMF::SCALED_X);

   Self->X = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Y: Y coordinate for the filter.

The meaning of the (#X, Y) field values depend on the value for #Units.  In userspace mode, the filter position will be
relative to the client vector's parent viewport.  In bounding-box mode, the filter position is relative to the
vector's position.  It is important to note that coordinates are measured before any transforms are applied.

The default values for #X and Y is `10%`, as per the SVG standard.  This provides a buffer space for the filter
algorithms to work with, and is usually a sufficient default.

-END-
*********************************************************************************************************************/

static ERR VECTORFILTER_GET_Y(extVectorFilter *Self, Unit *Value)
{
   Value->set(Self->Y);
   return ERR::Okay;
}

static ERR VECTORFILTER_SET_Y(extVectorFilter *Self, Unit &Value)
{
   if (Value.scaled()) Self->Dimensions = (Self->Dimensions | DMF::SCALED_Y) & (~DMF::FIXED_Y);
   else Self->Dimensions = (Self->Dimensions | DMF::FIXED_Y) & (~DMF::SCALED_Y);

   Self->Y = Value;
   return ERR::Okay;
}

//********************************************************************************************************************

static const FieldDef clFilterDimensions[] = {
   { "FixedX",       DMF::FIXED_X },
   { "FixedY",       DMF::FIXED_Y },
   { "ScaledX",      DMF::SCALED_X },
   { "ScaledY",      DMF::SCALED_Y },
   { "FixedWidth",   DMF::FIXED_WIDTH },
   { "FixedHeight",  DMF::FIXED_HEIGHT },
   { "ScaledWidth",  DMF::SCALED_WIDTH },
   { "ScaledHeight", DMF::SCALED_HEIGHT },
   { nullptr, 0 }
};

#include "filter_def.c"

static const FieldArray clFilterFields[] = {
   { "X",              FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORFILTER_GET_X, VECTORFILTER_SET_X },
   { "Y",              FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORFILTER_GET_Y, VECTORFILTER_SET_Y },
   { "Width",          FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORFILTER_GET_Width, VECTORFILTER_SET_Width },
   { "Height",         FDF_UNIT|FDF_DOUBLE|FDF_SCALED|FDF_RW, VECTORFILTER_GET_Height, VECTORFILTER_SET_Height },
   { "Opacity",        FDF_DOUBLE|FDF_RW, nullptr, VECTORFILTER_SET_Opacity },
   { "Inherit",        FDF_OBJECT|FDF_RW, nullptr, VECTORFILTER_SET_Inherit },
   { "ResX",           FDF_INT|FDF_RI },
   { "ResY",           FDF_INT|FDF_RI },
   { "Units",          FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterUnits },
   { "PrimitiveUnits", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterPrimitiveUnits },
   { "Dimensions",     FDF_INTFLAGS|FDF_R, nullptr, nullptr,        &clFilterDimensions },
   { "ColourSpace",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterColourSpace },
   { "AspectRatio",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorFilterAspectRatio },
   // Virtual fields
   { "EffectXML",      FDF_VIRTUAL|FDF_STRING|FDF_ALLOC|FDF_R, VECTORFILTER_GET_EffectXML },
   END_FIELD
};

//********************************************************************************************************************

ERR init_filter(void)
{
   clVectorFilter = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::VECTORFILTER),
      fl::Name("VectorFilter"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorFilterActions),
      fl::Fields(clFilterFields),
      fl::Size(sizeof(extVectorFilter)),
      fl::Path(MOD_PATH));

   return clVectorFilter ? ERR::Okay : ERR::AddClass;
}

