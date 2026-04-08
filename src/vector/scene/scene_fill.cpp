
//********************************************************************************************************************

void SceneRenderer::render_fill(VectorState &State, extVector &Vector, agg::rasterizer_scanline_aa<> &Raster, extPainter &Painter)
{
   // Think of the vector's path as representing a mask for the fill algorithm.  Any transforms applied to
   // an image/gradient fill are independent of the path.

   if (Vector.FillRule IS VFR::NON_ZERO) Raster.filling_rule(agg::fill_non_zero);
   else if (Vector.FillRule IS VFR::EVEN_ODD) Raster.filling_rule(agg::fill_even_odd);

   // Solid colour.  Bitmap fonts will set DisableFill.Colour to ensure texture maps are used instead

   if ((Painter.Colour.Alpha > 0) and (!Vector.DisableFillColour)) {
      auto colour = agg::rgba(Painter.Colour, Painter.Colour.Alpha * Vector.FillOpacity * State.mOpacity);

      if ((Vector.PathQuality IS RQ::CRISP) or (Vector.PathQuality IS RQ::FAST)) {
         agg::renderer_scanline_bin_solid renderer(mRenderBase);
         renderer.color(colour);

         if (!State.mClipStack->empty()) {
            agg::alpha_mask_gray8 alpha_mask(State.mClipStack->top().m_renderer);
            agg::scanline_u8_am<agg::alpha_mask_gray8> mScanLineMasked(alpha_mask);
            agg::render_scanlines(Raster, mScanLineMasked, renderer);
         }
         else agg::render_scanlines(Raster, mScanLine, renderer);
      }
      else {
         agg::renderer_scanline_aa_solid renderer(mRenderBase);
         renderer.color(colour);

         if (!State.mClipStack->empty()) {
            agg::alpha_mask_gray8 alpha_mask(State.mClipStack->top().m_renderer);
            agg::scanline_u8_am<agg::alpha_mask_gray8> mScanLineMasked(alpha_mask);
            agg::render_scanlines(Raster, mScanLineMasked, renderer);
         }
         else agg::render_scanlines(Raster, mScanLine, renderer);
      }
   }

   if (Painter.Image) { // Bitmap image fill.  NB: The SVG class creates a standard VectorRectangle and associates an image with it in order to support <image> tags.
      fill_image(State, Vector.Bounds, Vector.BasePath, Vector.Scene->SampleMethod,
         build_fill_transform(Vector, Painter.Image->Units IS VUNIT::USERSPACE, State),
         mView->vpFixedWidth, mView->vpFixedHeight, *((extVectorImage *)Painter.Image), mRenderBase, Raster, Vector.FillOpacity * State.mOpacity);
   }

   if (Painter.Gradient) {
      if (auto table = get_fill_gradient_table(Painter, State.mOpacity * Vector.FillOpacity)) {
         fill_gradient(State, Vector.Bounds, &Vector.BasePath,
            build_fill_transform(Vector, Painter.Gradient->Units IS VUNIT::USERSPACE, State),
            mView->vpFixedWidth, mView->vpFixedHeight, *((extVectorGradient *)Painter.Gradient), table, mRenderBase, Raster);
      }
   }

   if (Painter.Pattern) {
      fill_pattern(State, Vector.Bounds, &Vector.BasePath, Vector.Scene->SampleMethod,
         build_fill_transform(Vector, Painter.Pattern->Units IS VUNIT::USERSPACE, State),
         mView->vpFixedWidth, mView->vpFixedHeight, *((extVectorPattern *)Painter.Pattern), mRenderBase, Raster);
   }
}

//********************************************************************************************************************
// Image extension
// Path: The original vector path without transforms.
// Transform: Transforms to be applied to the path and to align the image.

static void fill_image(VectorState &State, const TClipRectangle<double> &Bounds, agg::path_storage &Path, VSM SampleMethod,
   const agg::trans_affine &Transform, double ViewWidth, double ViewHeight,
   extVectorImage &Image, agg::renderer_base<agg::pixfmt_psl> &RenderBase,
   agg::rasterizer_scanline_aa<> &Raster, double Alpha)
{
   const double c_width  = (Image.Units IS VUNIT::USERSPACE) ? ViewWidth : Bounds.width();
   const double c_height = (Image.Units IS VUNIT::USERSPACE) ? ViewHeight : Bounds.height();
   const double dx = Bounds.left + (dmf::hasScaledX(Image.Dimensions) ? (c_width * Image.X) : Image.X);
   const double dy = Bounds.top + (dmf::hasScaledY(Image.Dimensions) ? (c_height * Image.Y) : Image.Y);

   auto t_scale = Transform.scale();
   Path.approximation_scale(t_scale);

   double x_scale, y_scale, x_offset, y_offset;
   calc_aspectratio("fill_image", Image.AspectRatio, c_width, c_height,
      Image.Bitmap->Width, Image.Bitmap->Height, x_offset, y_offset, x_scale, y_scale);

   agg::trans_affine transform;
   transform.scale(x_scale, y_scale);
   transform.translate(dx + x_offset, dy + y_offset);
   transform *= Transform;

   transform.invert();

   const double final_x_scale = t_scale * x_scale;
   const double final_y_scale = t_scale * y_scale;

   if (SampleMethod IS VSM::AUTO) {
      if ((final_x_scale <= 0.5) or (final_y_scale <= 0.5)) SampleMethod = VSM::BICUBIC;
      else if ((final_x_scale <= 1.0) or (final_y_scale <= 1.0)) SampleMethod = VSM::SINC;
      else SampleMethod = VSM::SPLINE16; // Spline works well for enlarging monotone vectors and avoids sharpening artifacts.
   }

   if (!State.mClipStack->empty()) {
      agg::alpha_mask_gray8 alpha_mask(State.mClipStack->top().m_renderer);
      agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
      drawBitmap(masked_scanline, SampleMethod, RenderBase, Raster, Image.Bitmap, Image.SpreadMethod, Alpha, &transform);
   }
   else {
      agg::scanline_u8 scanline;
      drawBitmap(scanline, SampleMethod, RenderBase, Raster, Image.Bitmap, Image.SpreadMethod, Alpha, &transform);
   }
}

//********************************************************************************************************************
// Gradient fills.  // The Raster must contain the shape's path.

static void fill_gradient(VectorState &State, const TClipRectangle<double> &Bounds, agg::path_storage *Path,
   const agg::trans_affine &Transform, double ViewWidth, double ViewHeight, extVectorGradient &Gradient,
   GRADIENT_TABLE *Table, agg::renderer_base<agg::pixfmt_psl> &RenderBase, agg::rasterizer_scanline_aa<> &Raster)
{
   constexpr int MAX_SPAN = 256;
   typedef agg::span_interpolator_linear<> interpolator_type;
   typedef agg::span_allocator<agg::rgba8> span_allocator_type;
   typedef agg::pod_auto_array<agg::rgba8, MAX_SPAN> color_array_type;
   typedef agg::renderer_base<agg::pixfmt_psl>  RENDERER_BASE_TYPE;

   agg::trans_affine   transform;
   interpolator_type   span_interpolator(transform);
   span_allocator_type span_allocator;

   const double c_width = Gradient.Units IS VUNIT::USERSPACE ? ViewWidth : Bounds.width();
   const double c_height = Gradient.Units IS VUNIT::USERSPACE ? ViewHeight : Bounds.height();
   const double x_offset = Gradient.Units IS VUNIT::USERSPACE ? 0 : Bounds.left;
   const double y_offset = Gradient.Units IS VUNIT::USERSPACE ? 0 : Bounds.top;

   Path->approximation_scale(Transform.scale());

   auto render_gradient = [&]<typename Method>(Method SpreadMethod, double SpanA, double SpanB) {
      typedef agg::span_gradient<agg::rgba8, interpolator_type, Method, color_array_type> span_gradient_type;
      typedef agg::renderer_scanline_aa<RENDERER_BASE_TYPE, span_allocator_type, span_gradient_type> renderer_gradient_type;

      span_gradient_type  span_gradient(span_interpolator, SpreadMethod, *Table, SpanA, SpanB);
      renderer_gradient_type solidrender_gradient(RenderBase, span_allocator, span_gradient);

      if (State.mClipStack->empty()) {
         agg::scanline_u8 scanline;
         agg::render_scanlines(Raster, scanline, solidrender_gradient);
      }
      else { // Masked gradient
         agg::alpha_mask_gray8 alpha_mask(State.mClipStack->top().m_renderer);
         agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
         agg::render_scanlines(Raster, masked_scanline, solidrender_gradient);
      }
   };

   if (Gradient.Type IS VGT::LINEAR) {
      auto span = MAX_SPAN;
      if (Gradient.Units IS VUNIT::BOUNDING_BOX) {
         // NOTE: In this mode we are mapping a 1x1 gradient square into the target path, which means
         // the gradient is stretched into position as a square.  We don't map the (X,Y) points to the
         // bounding box and draw point-to-point.

         const double x = x_offset + (c_width * Gradient.X1);
         const double y = y_offset + (c_height * Gradient.Y1);

         if (Gradient.CalcAngle) {
            const double dx = Gradient.X2 - Gradient.X1;
            const double dy = Gradient.Y2 - Gradient.Y1;
            Gradient.Angle     = atan2(dy, dx);
            Gradient.Length    = sqrt((dx * dx) + (dy * dy));
            Gradient.CalcAngle = false;
         }

         transform.scale(Gradient.Length);
         transform.rotate(Gradient.Angle);
         transform.scale(c_width / span, c_height / span);
         transform.translate(x, y);
      }
      else {
         TClipRectangle<double> area;
         if ((Gradient.Flags & VGF::SCALED_X1) != VGF::NIL) area.left = x_offset + (c_width * Gradient.X1);
         else area.left = x_offset + Gradient.X1;

         if ((Gradient.Flags & VGF::SCALED_X2) != VGF::NIL) area.right = x_offset + (c_width * Gradient.X2);
         else area.right = x_offset + Gradient.X2;

         if ((Gradient.Flags & VGF::SCALED_Y1) != VGF::NIL) area.top = y_offset + (c_height * Gradient.Y1);
         else area.top = y_offset + Gradient.Y1;

         if ((Gradient.Flags & VGF::SCALED_Y2) != VGF::NIL) area.bottom = y_offset + (c_height * Gradient.Y2);
         else area.bottom = y_offset + Gradient.Y2;

         if (Gradient.CalcAngle) {
            const double dx = area.width();
            const double dy = area.height();
            Gradient.Angle     = atan2(dy, dx);
            Gradient.Length    = sqrt((dx * dx) + (dy * dy));
            Gradient.CalcAngle = false;
         }

         transform.scale(Gradient.Length / span);
         transform.rotate(Gradient.Angle);
         transform.translate(area.left, area.top);
      }

      apply_transforms(Gradient, transform);
      transform *= Transform;
      transform.invert();

      agg::gradient_x gradient_func;

      if (Gradient.SpreadMethod IS VSPREAD::REFLECT) {
         agg::gradient_reflect_adaptor<agg::gradient_x> spread_method(gradient_func);
         render_gradient(spread_method, 0, span);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::REPEAT) {
         agg::gradient_repeat_adaptor<agg::gradient_x> spread_method(gradient_func);
         render_gradient(spread_method, 0, span);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::CLIP) {
         agg::gradient_clip_adaptor<agg::gradient_x> spread_method(gradient_func);
         render_gradient(spread_method, 0, span);
      }
      else render_gradient(gradient_func, 0, span);
   }
   else if (Gradient.Type IS VGT::RADIAL) {
      agg::point_d c, f;

      double radial_col_span = Gradient.Radius;
      double focal_radius = Gradient.FocalRadius;
      if (focal_radius <= 0) focal_radius = Gradient.Radius;

      if (Gradient.Units IS VUNIT::BOUNDING_BOX) {
         // NOTE: In this mode we are stretching a 1x1 gradient square into the target path.

         c.x = Gradient.CenterX;
         c.y = Gradient.CenterY;
         if ((Gradient.Flags & (VGF::SCALED_FX|VGF::FIXED_FX)) != VGF::NIL) f.x = Gradient.FocalX;
         else f.x = c.x;

         if ((Gradient.Flags & (VGF::SCALED_FY|VGF::FIXED_FY)) != VGF::NIL) f.y = Gradient.FocalY;
         else f.y = c.y;

         transform.translate(c);
         transform.scale(c_width, c_height);
         apply_transforms(Gradient, transform);
         transform.translate(x_offset, y_offset);
         transform *= Transform;
         transform.invert();

         // Increase the gradient scale from 1.0 in order for AGG to draw a smooth gradient.

         radial_col_span *= MAX_SPAN;
         transform.scale(MAX_SPAN);
         focal_radius *= MAX_SPAN;

         c.x *= MAX_SPAN;
         c.y *= MAX_SPAN;
         f.x *= MAX_SPAN;
         f.y *= MAX_SPAN;
      }
      else {
         if ((Gradient.Flags & VGF::SCALED_CX) != VGF::NIL) c.x = x_offset + (c_width * Gradient.CenterX);
         else c.x = x_offset + Gradient.CenterX;

         if ((Gradient.Flags & VGF::SCALED_CY) != VGF::NIL) c.y = y_offset + (c_height * Gradient.CenterY);
         else c.y = y_offset + Gradient.CenterY;

         if ((Gradient.Flags & VGF::SCALED_FX) != VGF::NIL) f.x = x_offset + (c_width * Gradient.FocalX);
         else if ((Gradient.Flags & VGF::FIXED_FX) != VGF::NIL) f.x = x_offset + Gradient.FocalX;
         else f.x = c.x;

         if ((Gradient.Flags & VGF::SCALED_FY) != VGF::NIL) f.y = y_offset + (c_height * Gradient.FocalY);
         else if ((Gradient.Flags & VGF::FIXED_FY) != VGF::NIL) f.y = y_offset + Gradient.FocalY;
         else f.y = c.y;

         if ((Gradient.Flags & VGF::SCALED_RADIUS) != VGF::NIL) { // Gradient is a ratio of the viewport's dimensions
            radial_col_span = (ViewWidth + ViewHeight) * radial_col_span * 0.5;
            focal_radius = (ViewWidth + ViewHeight) * focal_radius * 0.5;
         }

         transform.translate(c);
         apply_transforms(Gradient, transform);
         transform *= Transform;
         transform.invert();
      }

      if ((c.x IS f.x) and (c.y IS f.y)) {
         // Standard radial gradient, where the focal point is the same as the gradient center

         agg::gradient_radial gradient_func;

         if (Gradient.SpreadMethod IS VSPREAD::REFLECT) {
            agg::gradient_reflect_adaptor<agg::gradient_radial> spread_method(gradient_func);
            render_gradient(spread_method, 0, radial_col_span);
         }
         else if (Gradient.SpreadMethod IS VSPREAD::REPEAT) {
            agg::gradient_repeat_adaptor<agg::gradient_radial> spread_method(gradient_func);
            render_gradient(spread_method, 0, radial_col_span);
         }
         else if (Gradient.SpreadMethod IS VSPREAD::CLIP) {
            agg::gradient_clip_adaptor<agg::gradient_radial> spread_method(gradient_func);
            render_gradient(spread_method, 0, radial_col_span);
         }
         else render_gradient(gradient_func, 0, radial_col_span);
      }
      else {
         // Radial gradient with a displaced focal point (uses agg::gradient_radial_focus).
         // The FocalRadius allows the client to alter the border region at which the focal
         // calculations are being made.
         //
         // SVG requires the focal point to be within the base radius, this can be enforced by setting CONTAIN_FOCAL.

         if ((Gradient.Flags & VGF::CONTAIN_FOCAL) != VGF::NIL) {
            agg::point_d d = { f.x - c.x, f.y - c.y };
            const double sqr_radius = radial_col_span * radial_col_span;
            const double outside = ((d.x * d.x) / sqr_radius) + ((d.y * d.y) / sqr_radius);

            if (outside > 1.0) {
               const double k = std::sqrt(1.0 / outside);
               f.x = c.x + (d.x * k);
               f.y = c.y + (d.y * k);
            }
         }

         agg::gradient_radial_focus gradient_func(focal_radius, f.x - c.x, f.y - c.y);

         if (Gradient.SpreadMethod IS VSPREAD::REFLECT) {
            agg::gradient_reflect_adaptor<agg::gradient_radial_focus> spread_method(gradient_func);
            render_gradient(spread_method, 0, radial_col_span);
         }
         else if (Gradient.SpreadMethod IS VSPREAD::REPEAT) {
            agg::gradient_repeat_adaptor<agg::gradient_radial_focus> spread_method(gradient_func);
            render_gradient(spread_method, 0, radial_col_span);
         }
         else if (Gradient.SpreadMethod IS VSPREAD::CLIP) {
            agg::gradient_clip_adaptor<agg::gradient_radial_focus> spread_method(gradient_func);
            render_gradient(spread_method, 0, radial_col_span);
         }
         else render_gradient(gradient_func, 0, radial_col_span);
      }
   }
   else if (Gradient.Type IS VGT::DIAMOND) {
      agg::point_d c;

      if ((Gradient.Flags & VGF::SCALED_CX) != VGF::NIL) c.x = x_offset + (c_width * Gradient.CenterX);
      else c.x = x_offset + Gradient.CenterX;

      if ((Gradient.Flags & VGF::SCALED_CY) != VGF::NIL) c.y = y_offset + (c_height * Gradient.CenterY);
      else c.y = y_offset + Gradient.CenterY;

      // Standard diamond gradient, where the focal point is the same as the gradient center

      double radial_col_span = Gradient.Radius;
      if (Gradient.Units IS VUNIT::USERSPACE) {
         if ((Gradient.Flags & VGF::SCALED_RADIUS) != VGF::NIL) {
            radial_col_span = (ViewWidth + ViewHeight) * Gradient.Radius * 0.5;
         }
         else transform *= agg::trans_affine_scaling(Gradient.Radius * 0.01);
      }
      else { // Align to vector's bounding box
         // Set radial_col_span to the wider of the width/height
         if (c_height > c_width) {
            radial_col_span = c_height * Gradient.Radius;
            transform.scaleX(c_width / c_height);
         }
         else {
            radial_col_span = c_width * Gradient.Radius;
            transform.scaleY(c_height / c_width);
         }
      }

      agg::gradient_diamond gradient_func;

      transform.translate(c);
      apply_transforms(Gradient, transform);
      transform *= Transform;
      transform.invert();

      if (Gradient.SpreadMethod IS VSPREAD::REFLECT) {
         agg::gradient_reflect_adaptor<agg::gradient_diamond> spread_method(gradient_func);
         render_gradient(spread_method, 0, radial_col_span);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::REPEAT) {
         agg::gradient_repeat_adaptor<agg::gradient_diamond> spread_method(gradient_func);
         render_gradient(spread_method, 0, radial_col_span);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::CLIP) {
         agg::gradient_clip_adaptor<agg::gradient_diamond> spread_method(gradient_func);
         render_gradient(spread_method, 0, radial_col_span);
      }
      else render_gradient(gradient_func, 0, radial_col_span);
   }
   else if (Gradient.Type IS VGT::CONIC) {
      agg::point_d c;

      if ((Gradient.Flags & VGF::SCALED_CX) != VGF::NIL) c.x = x_offset + (c_width * Gradient.CenterX);
      else c.x = x_offset + Gradient.CenterX;

      if ((Gradient.Flags & VGF::SCALED_CY) != VGF::NIL) c.y = y_offset + (c_height * Gradient.CenterY);
      else c.y = y_offset + Gradient.CenterY;

      // Standard conic gradient, where the focal point is the same as the gradient center

      double radial_col_span = Gradient.Radius;
      if (Gradient.Units IS VUNIT::USERSPACE) {
         if ((Gradient.Flags & VGF::SCALED_RADIUS) != VGF::NIL) {
            radial_col_span = (ViewWidth + ViewHeight) * Gradient.Radius * 0.5;
         }
         else transform *= agg::trans_affine_scaling(Gradient.Radius * 0.01);
      }
      else { // Bounding box
         // Set radial_col_span to the wider of the width/height
         if (c_height > c_width) {
            radial_col_span = c_height * Gradient.Radius;
            transform.scaleX(c_width / c_height);
         }
         else {
            radial_col_span = c_width * Gradient.Radius;
            transform.scaleY(c_height / c_width);
         }
      }

      agg::gradient_conic gradient_func;
      transform.translate(c);
      apply_transforms(Gradient, transform);
      transform *= Transform;
      transform.invert();

      render_gradient(gradient_func, 0, radial_col_span);
   }
   else if (Gradient.Type IS VGT::CONTOUR) {
      // TODO: The creation of the contour gradient is expensive, but it can be cached as long as the
      // path hasn't been modified.

      auto x2 = std::clamp(Gradient.X2, 0.01, 10.0);
      auto x1 = std::clamp(Gradient.X1, 0.0, x2);

      agg::gradient_contour gradient_func;
      gradient_func.d1(x1 * 256.0);  // d1 is added to the DT base values
      gradient_func.d2(x2);  // d2 is a multiplier of the base DT value
      gradient_func.contour_create(*Path);

      transform.translate(Bounds.left, Bounds.top);
      apply_transforms(Gradient, transform);
      transform *= Transform;
      transform.invert();

      // Regarding repeatable spread methods, bear in mind that the nature of the contour gradient
      // means that it is always masked by the target path.  To achieve repetition, the client can
      // use an x2 value > 1.0 to specify the number of colour cycles.

      if (Gradient.SpreadMethod IS VSPREAD::REFLECT) {
         agg::gradient_reflect_adaptor<agg::gradient_contour> spread_method(gradient_func);
         render_gradient(spread_method, 0, 256.0);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::REPEAT) {
         agg::gradient_repeat_adaptor<agg::gradient_contour> spread_method(gradient_func);
         render_gradient(spread_method, 0, 256.0);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::CLIP) {
         agg::gradient_clip_adaptor<agg::gradient_contour> spread_method(gradient_func);
         render_gradient(spread_method, 0, 256.0);
      }
      else render_gradient(gradient_func, 0, 256.0);
   }
}

//********************************************************************************************************************
// Fixed-size patterns can be rendered internally as a separate bitmap for tiling.  That bitmap is copied to the
// target bitmap with the necessary transforms applied.  USERSPACE patterns are suitable for this method.  If the
// client needs the pattern to maintain a fixed alignment with the associated vector, they must set the X,Y field
// values manually when that vector changes position.
//
// Patterns rendered with BOUNDING_BOX require real-time calculation as they have a dependency on the target
// vector's dimensions.

static void fill_pattern(VectorState &State, const TClipRectangle<double> &Bounds, agg::path_storage *Path,
   VSM SampleMethod, const agg::trans_affine &Transform, double ViewWidth, double ViewHeight,
   extVectorPattern &Pattern, agg::renderer_base<agg::pixfmt_psl> &RenderBase,
   agg::rasterizer_scanline_aa<> &Raster)
{
   constexpr bool SCALE_BITMAP = true; // Should always be true for fidelity, but switch to false if debugging coordinate issues.
   const double elem_width  = (Pattern.Units IS VUNIT::USERSPACE) ? ViewWidth : Bounds.width();
   const double elem_height = (Pattern.Units IS VUNIT::USERSPACE) ? ViewHeight : Bounds.height();
   const double x_offset = (Pattern.Units IS VUNIT::USERSPACE) ? 0 : Bounds.left;
   const double y_offset = (Pattern.Units IS VUNIT::USERSPACE) ? 0 : Bounds.top;
   double dx, dy;

   auto t_scale = Transform.scale();
   Path->approximation_scale(t_scale);

   if (Pattern.Units IS VUNIT::USERSPACE) { // Use fixed coords in the pattern; equiv. to 'userSpaceOnUse' in SVG
      double target_width, target_height;
      if (dmf::hasScaledWidth(Pattern.Dimensions)) target_width = elem_width * Pattern.Width;
      else if (dmf::hasWidth(Pattern.Dimensions)) target_width = Pattern.Width;
      else target_width = 1;

      if (dmf::hasScaledHeight(Pattern.Dimensions)) target_height = elem_height * Pattern.Height;
      else if (dmf::hasHeight(Pattern.Dimensions)) target_height = Pattern.Height;
      else target_height = 1;

      if (dmf::hasScaledX(Pattern.Dimensions)) dx = x_offset + (elem_width * Pattern.X);
      else if (dmf::hasX(Pattern.Dimensions)) dx = x_offset + Pattern.X;
      else dx = x_offset;

      if (dmf::hasScaledY(Pattern.Dimensions)) dy = y_offset + (elem_height * Pattern.Y);
      else if (dmf::hasY(Pattern.Dimensions)) dy = y_offset + Pattern.Y;
      else dy = y_offset;

      int page_width = int(target_width);
      int page_height = int(target_height);

      if ((page_width != Pattern.Scene->PageWidth) or (page_height != Pattern.Scene->PageHeight)) {
         Pattern.Scene->PageWidth = page_width;
         Pattern.Scene->PageHeight = page_height;
         mark_dirty(Pattern.Scene->Viewport, RC::DIRTY);
      }
   }
   else {
      // BOUNDING_BOX.  The tile size is 1.0x1.0 and member coordinates should range from 0.0 - 1.0.
      // The tile will be stretched to fit the target Bounds area.  The Pattern Viewport must have
      // its ViewX/Y/W/H values set to 0/0/1.0/1.0.

      double target_width, target_height;

      ((extVectorViewport *)Pattern.Viewport)->vpAspectRatio = ARF::X_MAX|ARF::Y_MAX; // Stretch the 1x1 viewport to match the PageW/H

      if (Pattern.ContentUnits IS VUNIT::BOUNDING_BOX) {
         Pattern.Viewport->setFields(fl::ViewWidth(Pattern.Width), fl::ViewHeight(Pattern.Height));
      }

      if (dmf::hasScaledWidth(Pattern.Dimensions)) target_width = Pattern.Width * elem_width;
      else target_width = Pattern.Width;

      if (dmf::hasScaledHeight(Pattern.Dimensions)) target_height = Pattern.Height * elem_height;
      else target_height = Pattern.Height;

      dx = x_offset + ((elem_width * Pattern.X) * (SCALE_BITMAP ? t_scale : 1.0));
      dy = y_offset + ((elem_height * Pattern.Y) * (SCALE_BITMAP ? t_scale : 1.0));

      // Scale the bitmap so that it matches the final scale on the display.  This requires a matching inverse
      // adjustment when computing the final transform.

      int page_width = int(target_width * (SCALE_BITMAP ? t_scale : 1.0));
      int page_height = int(target_height * (SCALE_BITMAP ? t_scale : 1.0));

      // Mark the bitmap for recomputation if needed.

      if ((page_width != Pattern.Scene->PageWidth) or (page_height != Pattern.Scene->PageHeight)) {
         if ((page_width < 1) or (page_height < 1) or (page_width > 8192) or (page_height > 8192)) {
            // Dimensions in excess of reasonable values can occur if the user is confused over the application
            // of bounding-box values that are being scaled.
            pf::Log log(__FUNCTION__);
            log.warning("Invalid pattern dimensions of %dx%d detected.", page_width, page_height);
            page_width  = 1;
            page_height = 1;
         }
         Pattern.Scene->PageWidth  = page_width;
         Pattern.Scene->PageHeight = page_height;
         mark_dirty(Pattern.Scene->Viewport, RC::DIRTY);
      }
   }

   // Redraw the pattern source if any part of the definition is marked as dirty.
   if ((check_dirty((extVector *)Pattern.Scene->Viewport)) or (!Pattern.Bitmap)) {
      if (acDraw(&Pattern) != ERR::Okay) return;
   }

   agg::trans_affine transform;

   if (Pattern.Matrices) { // Client used the 'patternTransform' SVG attribute
      auto &m = *Pattern.Matrices;
      transform.load_all(m.ScaleX, m.ShearY, m.ShearX, m.ScaleY, m.TranslateX + dx, m.TranslateY + dy);
   }
   else transform.translate(dx, dy);

   if ((SCALE_BITMAP) and (Pattern.Units != VUNIT::USERSPACE)) {
      // Invert any prior bitmap scaling
      transform.scale(1.0 / t_scale, 1.0 / t_scale);
   }

   // NB: If the Transform multiplication isn't performed, the pattern tile effectively becomes detached
   // from the target vector and is drawn as a static background.  Would it a be a useful feature for this
   // to be available to the client as a toggle?

   transform *= Transform;
   transform.invert();

   if (SampleMethod IS VSM::AUTO) {
      // Using anything more sophisticated than bicubic sampling for tiling is a CPU killer.
      // If the client requires a different method, they will need to set it explicitly.
      SampleMethod = VSM::BILINEAR;
   }

   if (!State.mClipStack->empty()) {
      agg::alpha_mask_gray8 alpha_mask(State.mClipStack->top().m_renderer);
      agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
      drawBitmap(masked_scanline, SampleMethod, RenderBase, Raster, Pattern.Bitmap, Pattern.SpreadMethod, Pattern.Opacity, &transform);
   }
   else {
      agg::scanline_u8 scanline;
      drawBitmap(scanline, SampleMethod, RenderBase, Raster, Pattern.Bitmap, Pattern.SpreadMethod, Pattern.Opacity, &transform);
   }
}
