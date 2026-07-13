
#include "agg_span_gradient_contour.h"
#include "agg_span_gradient_sdf.h"
#include "agg_span_gradient_worley.h"

class VectorState;

//********************************************************************************************************************

static bool render_matrix_matches(const VectorMatrix &Matrix, const agg::trans_affine &Transform)
{
   return (Matrix.ScaleX IS Transform.sx) and
      (Matrix.ScaleY IS Transform.sy) and
      (Matrix.ShearX IS Transform.shx) and
      (Matrix.ShearY IS Transform.shy) and
      (Matrix.TranslateX IS Transform.tx) and
      (Matrix.TranslateY IS Transform.ty);
}

static void set_render_matrix(VectorMatrix &Matrix, const agg::trans_affine &Transform)
{
   Matrix.ScaleX     = Transform.sx;
   Matrix.ScaleY     = Transform.sy;
   Matrix.ShearX     = Transform.shx;
   Matrix.ShearY     = Transform.shy;
   Matrix.TranslateX = Transform.tx;
   Matrix.TranslateY = Transform.ty;
}

static bool set_render_transform(objVectorViewport *Viewport, const agg::trans_affine &Transform)
{
   auto vp = (extVectorViewport *)Viewport;

   if (not vp->matrices()) {
      if (Viewport->newMatrix(nullptr, false) != ERR::Okay) return false;
   }

   if (not render_matrix_matches(*vp->matrices(), Transform)) {
      set_render_matrix(*vp->matrices(), Transform);
      mark_dirty(Viewport, RC::TRANSFORM);
   }

   return true;
}

static bool viewport_has_fixed_size(const objVectorViewport *Viewport, double Width, double Height)
{
   auto viewport = (const extVectorViewport *)Viewport;

   return viewport->vpTargetWidth.defined() and
      viewport->vpTargetHeight.defined() and
      (not viewport->vpTargetWidth.scaled()) and
      (not viewport->vpTargetHeight.scaled()) and
      (viewport->vpTargetWidth IS Width) and
      (viewport->vpTargetHeight IS Height);
}

static void set_viewport_fixed_size(objVectorViewport *Viewport, double Width, double Height)
{
   if (not viewport_has_fixed_size(Viewport, Width, Height)) {
      Viewport->setWidth(Width);
      Viewport->setHeight(Height);
   }
}

static bool viewport_has_fixed_bounds(const objVectorViewport *Viewport, double X, double Y, double Width,
   double Height)
{
   auto viewport = (const extVectorViewport *)Viewport;

   if (Width < 1) Width = 1;
   if (Height < 1) Height = 1;

   return viewport->vpTargetX.defined() and
      viewport->vpTargetY.defined() and
      viewport_has_fixed_size(Viewport, Width, Height) and
      (not viewport->vpTargetX.scaled()) and
      (not viewport->vpTargetY.scaled()) and
      (viewport->vpTargetX IS X) and
      (viewport->vpTargetY IS Y);
}

static void set_viewport_fixed_bounds(objVectorViewport *Viewport, double X, double Y, double Width, double Height)
{
   if (not viewport_has_fixed_bounds(Viewport, X, Y, Width, Height)) {
      acRedimension(Viewport, X, Y, 0, Width, Height, 0);
   }
}

static agg::trans_affine build_matrix_transform(const VectorMatrix *Matrices)
{
   agg::trans_affine transform;

   for (auto matrix=Matrices; matrix; matrix=matrix->Next) {
      transform.multiply(matrix->ScaleX, matrix->ShearY, matrix->ShearX, matrix->ScaleY,
         matrix->TranslateX, matrix->TranslateY);
   }

   return transform;
}

//********************************************************************************************************************

class SceneRenderer
{
private:
   agg::renderer_base<agg::pixfmt_psl> mRenderBase;
   agg::pixfmt_psl    mFormat;
   agg::scanline32_p8 mScanLine; // Use scanline32_p8 for large solid polygons/rectangles and scanline_u for complex shapes like text
   extVectorViewport  *mView;    // The current view
   objBitmap          *mBitmap;
   int mBitmapOriginX, mBitmapOriginY;
   std::vector<class InputBoundary> mInputBounds; // Records boundaries for input events and cursor changes.

public:
   // The ClipBuffer is used to hold any alpha-masks that are generated as the scene is rendered.

   class ClipBuffer {
      VectorState *m_state;
      std::vector<uint8_t> m_bitmap;
      int m_width, m_height;
      extVector *m_shape;

      public:
      agg::rendering_buffer m_renderer;
      extVectorClip *m_clip;

      public:

      ClipBuffer() : m_state(nullptr), m_width(0), m_height(0), m_shape(nullptr), m_clip(nullptr) { }

      ClipBuffer(VectorState &pState, extVectorClip *pClip, extVector *pShape) :
         m_state(&pState), m_width(0), m_height(0), m_shape(pShape), m_clip(pClip) { }

      void draw(SceneRenderer &);
      void draw_viewport(SceneRenderer &);
      void draw_clips(SceneRenderer &, extVector *, agg::rasterizer_scanline_aa<> &,
         agg::renderer_base<agg::pixfmt_gray8> &, const agg::trans_affine &);
      void draw_bounding_box(SceneRenderer &);
      void draw_userspace(SceneRenderer &);
      bool use_cached_mask(const agg::trans_affine &, double, double);
      void store_cached_mask(const agg::trans_affine &, double, double);
      void resize_bitmap(int, int);
   };

private:
   std::stack<ClipBuffer> mClipStack;

   double view_width() {
      if (mView->vpViewWidth > 0) return mView->vpViewWidth;
      else return viewport_coordinate_width(mView);
   }

   double view_height() {
      if (mView->vpViewHeight > 0) return mView->vpViewHeight;
      else return viewport_coordinate_height(mView);
   }

   void render_fill(VectorState &, extVector &, agg::rasterizer_scanline_aa<> &, extPainter &);
   void render_stroke(VectorState &, extVector &);
   void copy_filter_bitmap(objBitmap *, extVectorFilter *);
   bool shape_intersects_clip(extVector *);
   void draw_vectors(extVector *, VectorState &);
   static const agg::trans_affine build_fill_transform(extVector &, bool,  VectorState &);

public:
   extVectorScene *Scene; // The top-level VectorScene performing the draw.
   int mObjectCount;     // The number of objects drawn

   bool clip_stack_empty() const { return mClipStack.empty(); }
   ClipBuffer &clip_stack_top() { return mClipStack.top(); }

   // When rendering into a buffered viewport the target bitmap is offset so that render-space coordinates map onto a
   // buffer whose top-left is (mBitmapOriginX, mBitmapOriginY).  Auxiliary buffers (e.g. the Gouraud path mask) must
   // account for the same offset to stay aligned with the render base.

   int bitmap_origin_x() const { return mBitmapOriginX; }
   int bitmap_origin_y() const { return mBitmapOriginY; }

   SceneRenderer(extVectorScene *pScene) : mBitmapOriginX(0), mBitmapOriginY(0), Scene(pScene) { }
   void draw(objBitmap *, objVectorViewport *);
};

//********************************************************************************************************************
// This class holds the current state as the vector scene is parsed for drawing.  It is most useful for managing
// inheritable values that arise as part of the drawing process (transformation management being an obvious example).
//
// NOTE: This feature is not intended to manage inheritable features that cross-over with SVG.  For instance, fill
// values are not inheritable.  Wherever it is possible to do so, inheritance should be managed by the client, with
// the goal of building a scene graph that has static properties.

class VectorState {
public:
   TClipRectangle<double> mClip; // Current clip region as defined by the viewports
   VLJ mLineJoin;
   VLC mLineCap;
   VIJ mInnerJoin;
   double mOpacity;
   VIS    mVisible;
   VOF    mOverflowX, mOverflowY;
   bool   mLinearRGB;
   bool   mIsolated;
   bool   mDirty;

   VectorState() :
      mClip(0, 0, DBL_MAX, DBL_MAX),
      mLineJoin(VLJ::MITER),
      mLineCap(VLC::BUTT),
      mInnerJoin(VIJ::MITER),
      mOpacity(1.0),
      mVisible(VIS::VISIBLE),
      mOverflowX(VOF::VISIBLE), mOverflowY(VOF::VISIBLE),
      mLinearRGB(false),
      mIsolated(false),
      mDirty(false)
      { }
};

//********************************************************************************************************************

namespace agg {
class span_reflect_y
{
private:
   span_reflect_y();
public:
   typedef typename agg::rgba8::value_type value_type;
   typedef agg::rgba8 color_type;

   span_reflect_y(agg::pixfmt_psl & pixf, int offset_x, int offset_y) :
       m_src(&pixf),
       m_wrap_x(pixf.mWidth),
       m_wrap_y(pixf.mHeight),
       m_offset_x(offset_x),
       m_offset_y(offset_y)
   {
      m_bk_buf[0] = m_bk_buf[1] = m_bk_buf[2] = m_bk_buf[3] = 0;
   }

   void prepare() {}

   void generate(agg::rgba8 *s, int x, int y, unsigned len) {
      x += m_offset_x;
      y += m_offset_y;
      const value_type* p = (const value_type*)span(x, y, len);
      do {
         s->r = p[m_src->mPixelOrder.Red];
         s->g = p[m_src->mPixelOrder.Green];
         s->b = p[m_src->mPixelOrder.Blue];
         s->a = p[m_src->mPixelOrder.Alpha];
         p = (const value_type*)next_x();
         ++s;
      } while(--len);
   }

   uint8_t * span(int x, int y, unsigned) {
       m_x = x;
       m_row_ptr = m_src->row_ptr(m_wrap_y(y));
       return m_row_ptr + m_wrap_x(x) * 4;
   }

   uint8_t * next_x() {
       int x = ++m_wrap_x;
       return m_row_ptr + x * 4;
   }

   uint8_t * next_y() {
       m_row_ptr = m_src->row_ptr(++m_wrap_y);
       return m_row_ptr + m_wrap_x(m_x) * 4;
   }

   agg::pixfmt_psl *m_src;

private:
   wrap_mode_repeat_auto_pow2 m_wrap_x;
   wrap_mode_reflect_auto_pow2 m_wrap_y;
   uint8_t *m_row_ptr;
   int m_offset_x;
   int m_offset_y;
   uint8_t m_bk_buf[4];
   int m_x;
};

//********************************************************************************************************************

class span_reflect_x
{
private:
   span_reflect_x();
public:
   typedef typename agg::rgba8::value_type value_type;
   typedef agg::rgba8 color_type;

   span_reflect_x(agg::pixfmt_psl & pixf, int offset_x, int offset_y) :
       m_src(&pixf), m_wrap_x(pixf.mWidth), m_wrap_y(pixf.mHeight),
       m_offset_x(offset_x), m_offset_y(offset_y)
   {
      m_bk_buf[0] = m_bk_buf[1] = m_bk_buf[2] = m_bk_buf[3] = 0;
   }

   void prepare() {}

   void generate(agg::rgba8 *s, int x, int y, unsigned len) {
      x += m_offset_x;
      y += m_offset_y;
      const value_type* p = (const value_type*)span(x, y, len);
      do {
         s->r = p[m_src->mPixelOrder.Red];
         s->g = p[m_src->mPixelOrder.Green];
         s->b = p[m_src->mPixelOrder.Blue];
         s->a = p[m_src->mPixelOrder.Alpha];
         p = (const value_type*)next_x();
         ++s;
      } while(--len);
   }

   uint8_t * span(int x, int y, unsigned) {
      m_x = x;
      m_row_ptr = m_src->row_ptr(m_wrap_y(y));
      return m_row_ptr + m_wrap_x(x) * 4;
   }

   uint8_t * next_x() {
      int x = ++m_wrap_x;
      return m_row_ptr + x * 4;
   }

   uint8_t * next_y() {
      m_row_ptr = m_src->row_ptr(++m_wrap_y);
      return m_row_ptr + m_wrap_x(m_x) * 4;
   }

   agg::pixfmt_psl *m_src;

private:
   wrap_mode_reflect_auto_pow2 m_wrap_x;
   wrap_mode_repeat_auto_pow2 m_wrap_y;
   uint8_t *m_row_ptr;
   int m_offset_x;
   int m_offset_y;
   uint8_t m_bk_buf[4];
   int m_x;
};

//********************************************************************************************************************

class span_repeat_pf
{
private:
   span_repeat_pf();
public:
   typedef typename agg::rgba8::value_type value_type;
   typedef agg::rgba8 color_type;

   span_repeat_pf(agg::pixfmt_psl & pixf, int offset_x, int offset_y) :
       m_src(&pixf), m_wrap_x(pixf.mWidth), m_wrap_y(pixf.mHeight),
       m_offset_x(offset_x), m_offset_y(offset_y) {
      m_bk_buf[0] = m_bk_buf[1] = m_bk_buf[2] = m_bk_buf[3] = 0;
   }

   void prepare() {}

   void generate(agg::rgba8 *s, int x, int y, unsigned len) {
      x += m_offset_x;
      y += m_offset_y;
      const value_type* p = (const value_type*)span(x, y, len);
      do {
         s->r = p[m_src->mPixelOrder.Red];
         s->g = p[m_src->mPixelOrder.Green];
         s->b = p[m_src->mPixelOrder.Blue];
         s->a = p[m_src->mPixelOrder.Alpha];
         p = (const value_type*)next_x();
         ++s;
      } while(--len);
   }

   uint8_t * span(int x, int y, unsigned) {
      m_x = x;
      m_row_ptr = m_src->row_ptr(m_wrap_y(y));
      return m_row_ptr + m_wrap_x(x) * 4;
   }

   uint8_t * next_x() {
      int x = ++m_wrap_x;
      return m_row_ptr + x * 4;
   }

   uint8_t * next_y() {
      m_row_ptr = m_src->row_ptr(++m_wrap_y);
      return m_row_ptr + m_wrap_x(m_x) * 4;
   }

   agg::pixfmt_psl *m_src;

private:
   wrap_mode_repeat_auto_pow2 m_wrap_x, m_wrap_y;
   uint8_t *m_row_ptr;
   int m_offset_x, m_offset_y;
   uint8_t m_bk_buf[4];
   int m_x;
};
} // namespace

// Return the correct transformation matrix for a fill operation.  Requires that the vector's path has been generated.

const agg::trans_affine SceneRenderer::build_fill_transform(extVector &Vector, bool Userspace,  VectorState &State)
{
   if (Vector.dirty()) { // Sanity check: If the path is dirty then this function has been called out-of-sequence.
      DEBUG_BREAK
   }

   if (Userspace) { // Userspace: The vector's (x,y) position is ignored, but its transforms and all parent transforms will apply.
      agg::trans_affine transform;
      apply_transforms(Vector, transform);
      apply_parent_transforms(get_parent(&Vector), transform);
      return transform;
   }
   else return Vector.Transform; // Default BoundingBox: The vector's position, transforms, and parent transforms apply.
}

//********************************************************************************************************************

const agg::image_filter_lut & get_filter(VSM Method, agg::trans_affine &Transform, double Kernel)
{
   double kernel = 0;
   if ((Method IS VSM::SINC) or (Method IS VSM::LANCZOS) or (Method IS VSM::BLACKMAN)) {
      // For auto kernel calculation, use larger kernel sizes when shrinking.  A base-level of 3.0 is used so that
      // the use of advanced filter algorithms is justified for the client.
      double k;
      if (Kernel > 0.0) k = Kernel;
      else k = 3.0 + (1.0 / svg_diag(Transform.sx, Transform.sy));
      // Quantised to 0.25 steps; visually indistinguishable and it bounds the cache size.
      kernel = std::round(std::clamp(k, 2.0, 8.0) * 4.0) * 0.25;
   }

   // LUT construction is deterministic for a given method and kernel, so tables are computed once and
   // cached.  Thread-local storage keeps the cache lock-free

   thread_local std::unordered_map<int, agg::image_filter_lut> cache;
   const int key = (int(Method) << 8) | int(kernel * 4.0);
   if (auto it = cache.find(key); it != cache.end()) return it->second;

   auto &filter = cache[key];
   switch(Method) {
      case VSM::AUTO:
      case VSM::BILINEAR:  filter.calculate(agg::image_filter_bilinear(), true); break;
      case VSM::BICUBIC:   filter.calculate(agg::image_filter_bicubic(), true); break;
      case VSM::SPLINE16:  filter.calculate(agg::image_filter_spline16(), true); break;
      case VSM::KAISER:    filter.calculate(agg::image_filter_kaiser(), true); break;
      case VSM::QUADRIC:   filter.calculate(agg::image_filter_quadric(), true); break;
      case VSM::GAUSSIAN:  filter.calculate(agg::image_filter_gaussian(), true); break;
      case VSM::BESSEL:    filter.calculate(agg::image_filter_bessel(), true); break;
      case VSM::MITCHELL:  filter.calculate(agg::image_filter_mitchell(), true); break;
      case VSM::SINC:      filter.calculate(agg::image_filter_sinc(kernel), true); break;
      case VSM::LANCZOS:   filter.calculate(agg::image_filter_lanczos(kernel), true); break;
      case VSM::BLACKMAN:  filter.calculate(agg::image_filter_blackman(kernel), true); break;
      default:             filter.calculate(agg::image_filter_bicubic(), true); break;
   }
   return filter;
}

//********************************************************************************************************************
// A generic drawing function for VMImage and VMPattern, this is used to fill vectors with bitmap images.
// Optimium drawing speed is ensured by only using the chosen SampleMethod if the transform is complex.

template <class T> void drawBitmap(T &Scanline, VSM SampleMethod, agg::renderer_base<agg::pixfmt_psl> &RenderBase, agg::rasterizer_scanline_aa<> &Raster,
   objBitmap *SrcBitmap, VSPREAD SpreadMethod, double Opacity, agg::trans_affine *Transform = nullptr, double XOffset = 0, double YOffset = 0)
{
   agg::pixfmt_psl pixels(*SrcBitmap);
   const bool requires_interpolation = (std::floor(XOffset) != XOffset) or (std::floor(YOffset) != YOffset) or
      ((Transform) and ((Transform->is_complex()) or (std::floor(Transform->tx) != Transform->tx) or
         (std::floor(Transform->ty) != Transform->ty)));

   if (requires_interpolation) {
      agg::span_interpolator_linear interpolator(*Transform);

      auto render_source = [&](auto &source) {
         using source_t = std::remove_reference_t<decltype(source)>;
         if (SampleMethod IS VSM::NEIGHBOUR) {
            agg::span_image_filter_rgba_nn<source_t, agg::span_interpolator_linear<>> spangen(source, interpolator);
            drawBitmapRender(Scanline, RenderBase, Raster, spangen, Opacity);
         }
         else {
            const agg::image_filter_lut &filter = get_filter(SampleMethod, *Transform);
            agg::span_image_filter_rgba<source_t, agg::span_interpolator_linear<>> spangen(source, interpolator, filter, true);
            drawBitmapRender(Scanline, RenderBase, Raster, spangen, Opacity);
         }
      };

      if (SpreadMethod IS VSPREAD::REFLECT_X) {
         agg::span_reflect_x source(pixels, XOffset, YOffset);
         render_source(source);
      }
      else if (SpreadMethod IS VSPREAD::REFLECT_Y) {
         agg::span_reflect_y source(pixels, XOffset, YOffset);
         render_source(source);
      }
      else if (SpreadMethod IS VSPREAD::REPEAT) {
         agg::span_repeat_pf source(pixels, XOffset, YOffset);
         render_source(source);
      }
      else { // VSPREAD::PAD and VSPREAD::CLIP modes.
         agg::span_once<agg::pixfmt_psl> source(pixels, XOffset, YOffset);
         render_source(source);
      }
   }
   else {
      // 1:1 copy with no transforms that require interpolation

      if (Transform) {
         XOffset += Transform->tx;
         YOffset += Transform->ty;
      }

      const int x_offset = int(XOffset);
      const int y_offset = int(YOffset);

      if (SpreadMethod IS VSPREAD::REFLECT_X) {
         agg::span_reflect_x source(pixels, x_offset, y_offset);
         drawBitmapRender(Scanline, RenderBase, Raster, source, Opacity);
      }
      else if (SpreadMethod IS VSPREAD::REFLECT_Y) {
         agg::span_reflect_y source(pixels, x_offset, y_offset);
         drawBitmapRender(Scanline, RenderBase, Raster, source, Opacity);
      }
      else if (SpreadMethod IS VSPREAD::REPEAT) {
         agg::span_repeat_pf source(pixels, x_offset, y_offset);
         drawBitmapRender(Scanline, RenderBase, Raster, source, Opacity);
      }
      else { // VSPREAD::PAD and VSPREAD::CLIP modes.
         agg::span_once<agg::pixfmt_psl> source(pixels, x_offset, y_offset);
         drawBitmapRender(Scanline, RenderBase, Raster, source, Opacity);
      }
   }
}

//********************************************************************************************************************
// Use for drawing stroked paths with texture brushes.  Source images should have width of ^2 if maximum efficiency
// is desired.

class pattern_rgb {
   public:
      typedef agg::rgba8 color_type;

      pattern_rgb(objBitmap &Bitmap, double Height) : mBitmap(&Bitmap) {
         mScale = ((double)Bitmap.Height) / Height;
         mHeight = Height;

         if (Bitmap.BitsPerPixel IS 32) {
            if (Bitmap.ColourFormat->AlphaPos IS 24) {
               if (Bitmap.ColourFormat->BluePos IS 0) pixel = &pixel32BGRA;
               else pixel = &pixel32RGBA;
            }
            else if (Bitmap.ColourFormat->RedPos IS 24) pixel = &pixel32AGBR;
            else pixel = &pixel32ARGB;
         }
         else if (Bitmap.BitsPerPixel IS 24) {
            if (Bitmap.ColourFormat->BluePos IS 0) pixel = &pixel24BGR;
            else pixel = &pixel24RGB;
         }
         else kt::Log().warning("pattern_rgb: Unsupported bitmap format %dbpp", Bitmap.BitsPerPixel);

         if (Height != (double)mBitmap->Height) {
            ipixel = pixel;
            pixel = &pixelScaled;
         }
      }

      unsigned width()  const { return mBitmap->Width;  }
      unsigned height() const { return mHeight; }

      static agg::rgba8 pixel32BGRA(const pattern_rgb &Pattern, int x, int y) {
         auto p = PIXEL_DATA(Pattern.mBitmap->Data + (y * Pattern.mBitmap->LineWidth) + (x<<2), pxBGRA);
         return p.getRGB();
      }

      static agg::rgba8 pixel32RGBA(const pattern_rgb &Pattern, int x, int y) {
         auto p = PIXEL_DATA(Pattern.mBitmap->Data + (y * Pattern.mBitmap->LineWidth) + (x<<2), pxRGBA);
         return p.getRGB();
      }

      static agg::rgba8 pixel32AGBR(const pattern_rgb &Pattern, int x, int y) {
         auto p = PIXEL_DATA(Pattern.mBitmap->Data + (y * Pattern.mBitmap->LineWidth) + (x<<2), pxAGBR);
         return p.getRGB();
      }

      static agg::rgba8 pixel32ARGB(const pattern_rgb &Pattern, int x, int y) {
         auto p = PIXEL_DATA(Pattern.mBitmap->Data + (y * Pattern.mBitmap->LineWidth) + (x<<2), pxARGB);
         return p.getRGB();
      }

      static agg::rgba8 pixel24BGR(const pattern_rgb &Pattern, int x, int y) {
         auto p = PIXEL_DATA(Pattern.mBitmap->Data + (y * Pattern.mBitmap->LineWidth) + (x<<2), pxBGR);
         return p.getRGB();
      }

      static agg::rgba8 pixel24RGB(const pattern_rgb &Pattern, int x, int y) {
         auto p = PIXEL_DATA(Pattern.mBitmap->Data + (y * Pattern.mBitmap->LineWidth) + (x<<2), pxRGB);
         return p.getRGB();
      }

      static agg::rgba8 pixelScaled(const pattern_rgb &Pattern, int x, int y) {
         double src_y = (y + 0.5) * Pattern.mScale - 0.5;
         int h  = Pattern.mBitmap->Height - 1;
         int y1 = agg::ufloor(src_y);
         int y2 = y1 + 1;
         agg::rgba8 pix1 = (y1 < 0) ? agg::rgba8::no_color() : Pattern.ipixel(Pattern, x, y1);
         agg::rgba8 pix2 = (y2 > h) ? agg::rgba8::no_color() : Pattern.ipixel(Pattern, x, y2);
         return pix1.gradient(pix2, src_y - y1);
      }

      agg::rgba8 (*pixel)(const pattern_rgb &, int x, int y);

   private:
      agg::rgba8 (*ipixel)(const pattern_rgb &, int x, int y);
      objBitmap *mBitmap;
      double mScale;
      double mHeight;
};

//********************************************************************************************************************

static void stroke_brush(VectorState &State, const extVectorImage &Image, agg::renderer_base<agg::pixfmt_psl> &RenderBase,
   agg::conv_transform<agg::path_storage, agg::trans_affine> &Path, double StrokeWidth)
{
   typedef agg::pattern_filter_bilinear_rgba8 FILTER_TYPE;
   FILTER_TYPE filter;
   pattern_rgb src(*Image.Bitmap, StrokeWidth);

   double scale;
   if (StrokeWidth IS (double)Image.Bitmap->Height) scale = 1.0;
   else scale = (double)StrokeWidth / (double)Image.Bitmap->Height;

   if (isPow2((uint32_t)Image.Bitmap->Width)) { // If the image width is a power of 2, use this optimised version
      typedef agg::line_image_pattern_pow2<FILTER_TYPE> pattern_type;
      pattern_type pattern(filter);
      agg::renderer_outline_image<agg::renderer_base<agg::pixfmt_psl>, pattern_type> ren_img(RenderBase, pattern);
      agg::rasterizer_outline_aa<agg::renderer_outline_image<agg::renderer_base<agg::pixfmt_psl>, pattern_type>> ras_img(ren_img);

      //ren_img.start_x(scale); // Optional offset

      pattern.create(src); // Configures the line pattern
      if (scale != 1.0) ren_img.scale_x(scale);
      ras_img.add_path(Path);
   }
   else { // Slightly slower version for non-power of 2 textures.
      typedef agg::line_image_pattern<FILTER_TYPE> pattern_type;
      pattern_type pattern(filter);
      agg::renderer_outline_image<agg::renderer_base<agg::pixfmt_psl>, pattern_type> ren_img(RenderBase, pattern);
      agg::rasterizer_outline_aa<agg::renderer_outline_image<agg::renderer_base<agg::pixfmt_psl>, pattern_type>> ras_img(ren_img);

      //ren_img.start_x(scale);

      pattern.create(src);
      if (scale != 1.0) ren_img.scale_x(scale);
      ras_img.add_path(Path);
   }
}

//********************************************************************************************************************

void SceneRenderer::draw(objBitmap *Bitmap, objVectorViewport *Viewport)
{
   kt::Log log;

   mObjectCount = 0;
   while (not mClipStack.empty()) mClipStack.pop();

   log.traceBranch("Bitmap: %dx%d,%dx%d, Viewport: %d", Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right, Bitmap->Clip.Bottom, Scene->Viewport->UID);

   if ((Bitmap->Clip.Bottom > Bitmap->Height) or (Bitmap->Clip.Right > Bitmap->Width)) {
      // NB: Any code that triggers this warning needs to be fixed.
      log.warning("Invalid Bitmap clip region: %d %d %d %d; W/H: %dx%d", Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right, Bitmap->Clip.Bottom, Bitmap->Width, Bitmap->Height);
      return;
   }

   if (Viewport) {
      mBitmap = Bitmap;
      mFormat.setBitmap(*Bitmap);
      mRenderBase.attach(mFormat);

      mView = nullptr; // Current view
      mRenderBase.clip_box(Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right-1, Bitmap->Clip.Bottom-1);

      // Pre-size the input boundary buffer based on the previous frame's boundary count to avoid growth reallocations.
      mInputBounds.reserve(Scene->InputBoundaries.size());

      VectorState state;
      draw_vectors((extVector *)Viewport, state);

      Scene->InputBoundaries = std::move(mInputBounds);
      Scene->SubtreeDirty = false;
   }
}

//********************************************************************************************************************
// Points StrokeRaster at the scene's shared gamma table (the scene rebuilds the table only when its
// Gamma value changes).

static void apply_stroke_gamma(extVector &Vector)
{
   Vector.StrokeRaster->gamma(((extVectorScene *)Vector.Scene)->gamma_table());
}

//********************************************************************************************************************
// Refer to configure_stroke() for the configuration of StrokeRaster.

void SceneRenderer::render_stroke(VectorState &State, extVector &Vector)
{
   auto &raster = *Vector.StrokeRaster;

   apply_stroke_gamma(Vector);

   // SVG requires stroke outlines to be rasterised with the non-zero rule regardless of the vector's FillRule;
   // stroke geometry legitimately self-overlaps (joins, dashes, crossings) and even-odd would punch holes in it.
   raster.filling_rule(agg::fill_non_zero);

   // Regarding this validation check, SVG requires that stroked vectors have a size > 0 when a paint server is used
   // as a stroker.  If the size is zero, the paint server is ignored and the solid colour can be used as a stroker
   // if the client has specified one.  Ref: W3C SVG Coordinate chapter, last paragraph of 7.11

   if (Vector.Bounds.valid()) {
      if (Vector.Stroke.Gradient) {
         auto gradient = (extGradient *)Vector.Stroke.Gradient;
         if (gradient->classID() IS CLASSID::GRADIENTGOURAUD) {
            fill_gouraud(State, Vector.Bounds, view_width(), view_height(), *(extGradientGouraud *)gradient,
               State.mOpacity * Vector.StrokeOpacity, mRenderBase, raster,
               build_fill_transform(Vector, gradient->Units IS VUNIT::USERSPACE, State), this);
         }
         else if (gradient->classID() IS CLASSID::GRADIENTMESH) {
            fill_mesh(State, Vector.Bounds, view_width(), view_height(), *(extGradientMesh *)gradient,
               State.mOpacity * Vector.StrokeOpacity, mRenderBase, raster,
               build_fill_transform(Vector, gradient->Units IS VUNIT::USERSPACE, State), this);
         }
         else if (auto table = get_stroke_gradient_table(Vector)) {
            fill_gradient(State, Vector.Bounds, &Vector.BasePath,
               build_fill_transform(Vector, gradient->Units IS VUNIT::USERSPACE, State),
               view_width(), view_height(), *gradient, table, mRenderBase, raster, this);
         }
         return;
      }

      if (Vector.Stroke.Pattern) {
         fill_pattern(State, Vector.Bounds, &Vector.BasePath, Vector.Scene->SampleMethod,
            build_fill_transform(Vector, Vector.Stroke.Pattern->Units IS VUNIT::USERSPACE, State),
            view_width(), view_height(), *((extVectorPattern *)Vector.Stroke.Pattern), mRenderBase, raster, this);
         return;
      }

      if (Vector.Stroke.Image) {
         double stroke_width = Vector.fixed_stroke_width() * Vector.Transform.scale();
         if (stroke_width < 1) stroke_width = 1;

         auto transform = Vector.Transform;
         Vector.BasePath.approximation_scale(transform.scale());
         agg::conv_transform<agg::path_storage, agg::trans_affine> stroke_path(Vector.BasePath, transform);

         stroke_brush(State, *((extVectorImage *)Vector.Stroke.Image), mRenderBase, stroke_path, stroke_width);
         return;
      }
   }

   // Solid colour

   if ((Vector.PathQuality IS RQ::CRISP) or (Vector.PathQuality IS RQ::FAST)) {
      agg::renderer_scanline_bin_solid renderer(mRenderBase);
      renderer.color(agg::rgba(Vector.Stroke.Colour, Vector.Stroke.Colour.Alpha * Vector.StrokeOpacity * State.mOpacity));

      if (not mClipStack.empty()) {
         agg::alpha_mask_gray8 alpha_mask(mClipStack.top().m_renderer);
         agg::scanline_u8_am<agg::alpha_mask_gray8> mScanLineMasked(alpha_mask);
         agg::render_scanlines(raster, mScanLineMasked, renderer);
      }
      else agg::render_scanlines(raster, mScanLine, renderer);
   }
   else {
      agg::renderer_scanline_aa_solid renderer(mRenderBase);
      renderer.color(agg::rgba(Vector.Stroke.Colour, Vector.Stroke.Colour.Alpha * Vector.StrokeOpacity * State.mOpacity));

      if (not mClipStack.empty()) {
         agg::alpha_mask_gray8 alpha_mask(mClipStack.top().m_renderer);
         agg::scanline_u8_am<agg::alpha_mask_gray8> mScanLineMasked(alpha_mask);
         agg::render_scanlines(raster, mScanLineMasked, renderer);
      }
      else agg::render_scanlines(raster, mScanLine, renderer);
   }
}

static double distal_fill_inflation(extVector *Shape, double ViewWidth, double ViewHeight)
{
   double inflation = 0;

   for (auto &fill : Shape->Fill) {
      if (not fill.Gradient) continue;

      auto gradient = (extGradient *)fill.Gradient;
      if ((gradient->classID() IS CLASSID::GRADIENTDISTAL) and (gradient->SpreadMethod != VSPREAD::CLIP)) {
         auto distal = (extGradientDistal *)gradient;
         const double max_bound = (Shape->Bounds.width() > Shape->Bounds.height()) ? Shape->Bounds.width() :
            Shape->Bounds.height();
         const double radius = distal->Radius.scaled() ? distal->Radius * max_bound : double(distal->Radius);

         // For REPEAT/REFLECT the exterior tiles outward to fill the parent area, so fill_gradient() expands the SDF
         // buffer to a fill extent that reaches the view edges from the path bounds.

         double extent = radius;
         if ((gradient->SpreadMethod IS VSPREAD::REPEAT) or (gradient->SpreadMethod IS VSPREAD::REFLECT)) {
            const double reach_x = std::max(Shape->Bounds.left, ViewWidth - Shape->Bounds.right);
            const double reach_y = std::max(Shape->Bounds.top, ViewHeight - Shape->Bounds.bottom);
            extent = std::max({ radius, reach_x, reach_y });
         }

         if (extent > inflation) inflation = extent;
      }
   }

   return inflation * Shape->Transform.scale();
}

//********************************************************************************************************************
// Test the transformed vector bounds against the current render clip.  The returned value only controls rasterisation
// of this vector's own path; child traversal and input-boundary registration are handled separately by draw_vectors().

bool SceneRenderer::shape_intersects_clip(extVector *Shape)
{
   if (Shape->BasePath.empty()) return false;

   TClipRectangle<double> bounds;

   if (Shape->Transform.is_normal()) bounds = Shape;
   else {
      auto path = Shape->Bounds.as_path(Shape->Transform);
      bounds = get_bounds(path);
   }

   double inflation = 1.0; // Preserve antialiasing on sub-pixel edges close to the clip boundary.

   if (Shape->StrokeRaster) {
      double stroke_inflation = Shape->fixed_stroke_width() * Shape->Transform.scale();
      if (Shape->MiterLimit > 1.0) stroke_inflation *= Shape->MiterLimit;
      if (stroke_inflation > inflation) inflation = stroke_inflation;
   }

   auto fill_inflation = distal_fill_inflation(Shape, mView->vpFixedWidth, mView->vpFixedHeight);
   if (fill_inflation > inflation) inflation = fill_inflation;

   bounds.left   -= inflation;
   bounds.top    -= inflation;
   bounds.right  += inflation;
   bounds.bottom += inflation;

   auto &clip = mRenderBase.clip_box();

   return (bounds.right >= double(clip.x1)) and (bounds.bottom >= double(clip.y1)) and
      (bounds.left <= double(clip.x2)) and (bounds.top <= double(clip.y2));
}

//********************************************************************************************************************
// Filter bitmaps use absolute clip coordinates.  Buffered viewport targets are local bitmaps with their Data pointer
// shifted so vector rasterisation can still address absolute scene coordinates.  CopyArea() clips against bitmap
// dimensions, so translate the final filter copy back to viewport-local coordinates when a buffered target is active.

void SceneRenderer::copy_filter_bitmap(objBitmap *Bitmap, extVectorFilter *Filter)
{
   Bitmap->Opacity = (Filter->Opacity < 1.0) ? (255.0 * Filter->Opacity) : 255;

   if (mBitmapOriginX or mBitmapOriginY) {
      const int src_x = Bitmap->Clip.Left;
      const int src_y = Bitmap->Clip.Top;
      const int width = Bitmap->Clip.Right - Bitmap->Clip.Left;
      const int height = Bitmap->Clip.Bottom - Bitmap->Clip.Top;

      if ((width > 0) and (height > 0)) {
         auto save_data = mBitmap->offset(mBitmapOriginX, mBitmapOriginY);

         gfx::CopyArea(Bitmap, mBitmap, BAF::BLEND|BAF::COPY, src_x, src_y, width, height,
            src_x - mBitmapOriginX, src_y - mBitmapOriginY);

         mBitmap->Data = save_data;
      }
   }
   else gfx::CopyArea(Bitmap, mBitmap, BAF::BLEND|BAF::COPY, 0, 0, Bitmap->Width, Bitmap->Height, 0, 0);
}

// This is the main routine for parsing the vector tree for drawing.

void SceneRenderer::draw_vectors(extVector *CurrentVector, VectorState &ParentState) {
   for (auto shape=CurrentVector; shape; shape=(extVector *)shape->Next) {
      kt::Log log(__FUNCTION__);
      VectorState state = VectorState(ParentState);

      if (shape->baseClassID() != CLASSID::VECTOR) {
         log.trace("Non-Vector discovered in the vector tree.");
         continue;
      }
      else if (not shape->Scene) continue;

      if (shape->dirty()) gen_vector_path(shape);
      else log.trace("%s: #%d, Dirty: NO, ParentView: #%d", shape->Class->ClassName.c_str(), shape->UID, shape->ParentView ? shape->ParentView->UID : 0);

      if (shape->RequiresRedraw) {
         state.mDirty = true; // Carry-forward dirty marker for children
         shape->RequiresRedraw = false;
      }

      // Visibility management.  NB: Under SVG rules VectorGroup objects are always visible as they are not
      // classed as a graphics element.

      {
         bool visible = true;
         if (shape->Visibility IS VIS::INHERIT) {
            if (ParentState.mVisible != VIS::VISIBLE) visible = false;
         }
         else if (shape->Visibility != VIS::VISIBLE) visible = false;

         if (((not visible) or (not shape->ValidState)) and (shape->classID() != CLASSID::VECTORGROUP)) {
            log.trace("%s: #%d, Not Visible", get_name(shape), shape->UID);
            continue;
         }
      }

      mObjectCount++;

      auto filter = (extVectorFilter *)shape->Filter;
      if ((filter) and (not filter->Disabled)) {
         #ifdef DBG_DRAW
            log.traceBranch("Rendering filter for %s.", get_name(shape));
         #endif

         objBitmap *bmp;
         if (not render_filter(filter, mView, shape, mBitmap, &bmp)) {
            copy_filter_bitmap(bmp, filter);
         }
         continue;
      }

      #ifdef DBG_DRAW
         log.traceBranch("%s: #%d, Matrices: %p", get_name(shape), shape->UID, shape->matrices());
      #endif

      if (mBitmap->ColourSpace IS CS::LINEAR_RGB) state.mLinearRGB = true; // The target bitmap's colour space has priority if linear.
      else if (shape->ColourSpace IS VCS::LINEAR_RGB) state.mLinearRGB = true; // Use the parent value unless a specific CS is required by the client
      else if (shape->ColourSpace IS VCS::SRGB) state.mLinearRGB = false;

      if (shape->LineJoin  != VLJ::INHERIT) state.mLineJoin  = shape->LineJoin;
      if (shape->InnerJoin != VIJ::INHERIT) state.mInnerJoin = shape->InnerJoin;
      if (shape->LineCap   != VLC::INHERIT) state.mLineCap   = shape->LineCap;
      state.mOpacity = shape->Opacity * state.mOpacity;

      // Support for isolated vectors.  A vector will be isolated if it has children using a filter that uses BackgroundImage
      // or BackgroundAlpha as an input.  This feature requires the bitmap to have an alpha channel so that
      // blending will work correctly, and the bitmap will be cleared to accept fresh content.  It acts as
      // a placeholder over the existing target bitmap, and the new content will be rendered to the target
      // after processing the current branch.  The background is then discarded.

      objBitmap *bmpBkgd = nullptr;
      objBitmap *bmpSave = nullptr;
      int save_origin_x = 0;
      int save_origin_y = 0;
      if ((shape->Flags & VF::ISOLATED) != VF::NIL) {
         if (not shape->IsolatedBuffer) shape->IsolatedBuffer.reset(new (std::nothrow) filter_bitmap);
         if (shape->IsolatedBuffer) {
            auto &clip_box = mRenderBase.clip_box();
            TClipRectangle<int> isolated_clip(clip_box.x1, clip_box.y1, clip_box.x2 + 1, clip_box.y2 + 1);

            bmpBkgd = shape->IsolatedBuffer->get_bitmap(mBitmap->Width, mBitmap->Height, isolated_clip, false,
               shape->UID, "isolated_bkgd");
         }

         if (bmpBkgd) {
            bmpBkgd->ColourSpace = mBitmap->ColourSpace;
            bmpBkgd->BlendMode   = mBitmap->BlendMode;
            bmpBkgd->Flags       = (bmpBkgd->Flags & ~BMF::PREMUL) | BMF::ALPHA_CHANNEL | BMF::NO_DATA;
            bmpSave = mBitmap;
            mBitmap = bmpBkgd;
            save_origin_x = mBitmapOriginX;
            save_origin_y = mBitmapOriginY;
            mBitmapOriginX = 0;
            mBitmapOriginY = 0;
            mFormat.setBitmap(*bmpBkgd);
            clearmem(bmpBkgd->CreatorMeta, bmpBkgd->LineWidth * (bmpBkgd->Clip.Bottom - bmpBkgd->Clip.Top));
            state.mIsolated = true;
         }
      }

      if (shape->classID() IS CLASSID::VECTORVIEWPORT) {
         if ((shape->Child) or (has_input_boundary(shape)) or (shape->Fill[0].Pattern)) {
            auto view = (extVectorViewport *)shape;

            if (view->vpOverflowX != VOF::INHERIT) state.mOverflowX = view->vpOverflowX;
            if (view->vpOverflowY != VOF::INHERIT) state.mOverflowY = view->vpOverflowY;

            auto save_clip = state.mClip;
            auto clip = state.mClip;

            if ((state.mOverflowX IS VOF::HIDDEN) or (state.mOverflowX IS VOF::SCROLL) or ((view->vpAspectRatio & ARF::SLICE) != ARF::NIL)) {
               if (view->vpBounds.left > state.mClip.left) state.mClip.left = view->vpBounds.left;
               if (view->vpBounds.right < state.mClip.right) state.mClip.right = view->vpBounds.right;
            }

            if ((state.mOverflowY IS VOF::HIDDEN) or (state.mOverflowY IS VOF::SCROLL) or ((view->vpAspectRatio & ARF::SLICE) != ARF::NIL)) {
               if (view->vpBounds.top > state.mClip.top) state.mClip.top = view->vpBounds.top;
               if (view->vpBounds.bottom < state.mClip.bottom) state.mClip.bottom = view->vpBounds.bottom;
            }

            #ifdef DBG_DRAW
               log.traceBranch("Viewport #%d clip region (%.2f %.2f %.2f %.2f)", shape->UID, state.mClip.left, state.mClip.top, state.mClip.right, state.mClip.bottom);
            #endif

            if ((state.mClip.right > state.mClip.left) and (state.mClip.bottom > state.mClip.top)) { // Continue only if the clipping region is visible
               if (view->vpClip) {
                  mClipStack.emplace(state, (extVectorClip *)nullptr, view);
                  mClipStack.top().draw_viewport(*this);
               }

               validate_clip_mask(view); // Drop the mask link lazily if its target has been terminated.
               if (view->ClipMask) {
                  mClipStack.emplace(state, view->ClipMask, view);
                  mClipStack.top().draw(*this);
               }

               auto save_rb_clip = mRenderBase.clip_box();
               if (state.mClip.left   > save_rb_clip.x1) mRenderBase.m_clip_box.x1 = state.mClip.left;
               if (state.mClip.top    > save_rb_clip.y1) mRenderBase.m_clip_box.y1 = state.mClip.top;
               if (state.mClip.right  < save_rb_clip.x2) mRenderBase.m_clip_box.x2 = state.mClip.right;
               if (state.mClip.bottom < save_rb_clip.y2) mRenderBase.m_clip_box.y2 = state.mClip.bottom;

               log.trace("ViewBox (%g %g %g %g) Scale (%g %g) Fix (%g %g %g %g)",
                  view->vpViewX, view->vpViewY, view->vpViewWidth, view->vpViewHeight,
                  view->vpXScale, view->vpYScale,
                  view->FinalX, view->FinalY, view->vpFixedWidth, view->vpFixedHeight);

               auto saved_viewport = mView;  // Save current viewport state and switch to the new viewport state
               mView = view;

               // For viewports that read user input, we record the collision box for the cursor.

               if (has_input_boundary(shape)) {
                  clip.shrinking(view);
                  mInputBounds.emplace_back(shape->UID, view->Cursor, clip, view->vpBounds.left, view->vpBounds.top);
               }

               if ((Scene->Flags & VPF::OUTLINE_VIEWPORTS) != VPF::NIL) { // Debug option: Draw the viewport's path with a green outline
                  agg::renderer_scanline_bin_solid renderer(mRenderBase);
                  renderer.color(agg::rgba(0, 1, 0));
                  agg::rasterizer_scanline_aa stroke_raster;
                  agg::conv_stroke<agg::path_storage> stroked_path(view->BasePath);
                  stroked_path.width(2);
                  stroke_raster.add_path(stroked_path);
                  agg::render_scanlines(stroke_raster, mScanLine, renderer);
               }

               if (view->Fill[0].Pattern) {
                  // Viewports can use FillPattern objects to render a different scene graph internally.
                  // This is useful for creating common graphics that can be re-used multiple times without
                  // them being pre-rendered to a cache as they would be for filled vector paths.
                  //
                  // The client can expect a result that is equivalent to the pattern's viewport being a child of
                  // the current viewport.  NB: There is a performance penalty in that transforms will be
                  // applied in realtime.

                  // Use transforms for the purpose of placing the pattern correctly

                  auto &t = view->Transform;
                  if (set_render_transform(view->Fill[0].Pattern->Scene->Viewport, t)) {

                     if (view->Fill[0].Pattern->Units IS VUNIT::BOUNDING_BOX) {
                        view->Fill[0].Pattern->Scene->setPageWidth(view->Scene->PageWidth);
                        view->Fill[0].Pattern->Scene->setPageHeight(view->Scene->PageHeight);
                        set_viewport_fixed_size(view->Fill[0].Pattern->Scene->Viewport, view->vpFixedWidth,
                           view->vpFixedHeight);
                     }

                     draw_vectors((extVectorViewport *)((extVectorPattern *)view->Fill[0].Pattern)->Viewport, state);
                  }

                  if ((view->FGFill) and (view->Fill[1].Pattern)) {
                     // Support for foreground fill patterns
                     if (set_render_transform(view->Fill[1].Pattern->Scene->Viewport, t)) {

                        if (view->Fill[1].Pattern->Units IS VUNIT::BOUNDING_BOX) {
                           view->Fill[1].Pattern->Scene->setPageWidth(view->Scene->PageWidth);
                           view->Fill[1].Pattern->Scene->setPageHeight(view->Scene->PageHeight);
                           set_viewport_fixed_size(view->Fill[1].Pattern->Scene->Viewport, view->vpFixedWidth,
                              view->vpFixedHeight);
                        }

                        draw_vectors((extVectorViewport *)((extVectorPattern *)view->Fill[1].Pattern)->Viewport, state);
                     }
                  }
               }

               if (view->Child) {
                  constexpr int MAX_AREA = 4096 * 4096; // Maximum allowable area for enabling a viewport buffer

                  if ((view->vpBuffered) and (view->vpFixedWidth * view->vpFixedHeight < MAX_AREA)) {
                     // In buffered mode, children will be drawn to an independent bitmap that is permanently
                     // cached.

                     bool redraw = view->vpRefreshBuffer or state.mDirty;
                     view->vpRefreshBuffer = false;

                     if ((not redraw) and (view->vpSeenShareVersion != Scene->ShareVersion)) redraw = true;

                     if (view->vpBuffer) {
                        if ((view->vpBuffer->Width != view->vpFixedWidth) or (view->vpBuffer->Height != view->vpFixedHeight)) {
                           view->vpBuffer->resize(view->vpFixedWidth, view->vpFixedHeight);
                           redraw = true;
                        }
                     }
                     else {
                        view->vpBuffer = objBitmap::create::local(fl::Name("vp_buffer"),
                           fl::Owner(view->UID),
                           fl::Width(view->vpFixedWidth),
                           fl::Height(view->vpFixedHeight),
                           fl::BitsPerPixel(32),
                           fl::Flags(BMF::ALPHA_CHANNEL));
                           //fl::ColourSpace(view->vpColourSpace));
                        redraw = true;
                     }

                     if (redraw) {
                        view->vpBuffer->BkgdIndex = 0;
                        view->vpBuffer->clear();

                        // A new state is allocated for drawing the children.  Clipping to the bitmap is enforced with
                        // the overflow values.

                        VectorState child_state;
                        child_state.mOverflowX = VOF::HIDDEN;
                        child_state.mOverflowY = VOF::HIDDEN;

                        auto save_bmp    = mBitmap;
                        auto save_format = mFormat;
                        auto save_rb     = mRenderBase;
                        auto save_origin_x = mBitmapOriginX;
                        auto save_origin_y = mBitmapOriginY;

                        // The vector paths will target the coordinate space of their parents, so we
                        // make adjustments to the bitmap to orient to (0,0).

                        mBitmap = view->vpBuffer;
                        mBitmapOriginX = int(view->vpBounds.left);
                        mBitmapOriginY = int(view->vpBounds.top);
                        auto save_data = mBitmap->offset(-mBitmapOriginX, -mBitmapOriginY);
                        mFormat.setBitmap(*view->vpBuffer);

                        auto ib_size = mInputBounds.size();

                        std::stack<ClipBuffer> save_clip_stack;
                        save_clip_stack.swap(mClipStack);

                        draw_vectors((extVector *)view->Child, child_state);

                        mClipStack.swap(save_clip_stack);

                        // Cache the freshly-generated boundaries on the viewport so that they can be re-used on
                        // subsequent frames that skip re-rendering this buffered viewport.

                        if (mInputBounds.size() > ib_size) {
                           if (not view->vpInputBounds) view->vpInputBounds = std::make_unique<std::vector<InputBoundary>>();
                           view->vpInputBounds->assign(mInputBounds.begin() + ib_size, mInputBounds.end());
                        }
                        else if (view->vpInputBounds) view->vpInputBounds->clear();

                        mBitmap->Data = save_data;

                        mRenderBase = save_rb;
                        mBitmap     = save_bmp;
                        mFormat     = save_format;
                        mBitmapOriginX = save_origin_x;
                        mBitmapOriginY = save_origin_y;

                        view->vpSeenShareVersion = Scene->ShareVersion;

                        //save_bitmap(view->vpBuffer, std::string("viewport"));
                     }
                     else if (view->vpInputBounds) {
                        // Input boundaries need to be redeclared on every drawing cycle, so replay the cached
                        // boundaries from the previous render of this buffered viewport.
                        mInputBounds.insert(mInputBounds.end(), view->vpInputBounds->begin(), view->vpInputBounds->end());
                     }

                     // Draw the cached bitmap buffer

                     view->BasePath.approximation_scale(view->Transform.scale());

                     auto transform = view->Transform;
                     transform.invert();

                     agg::rasterizer_scanline_aa<> raster;
                     raster.add_path(view->BasePath);

                     if (not mClipStack.empty()) {
                        agg::alpha_mask_gray8 alpha_mask(mClipStack.top().m_renderer);
                        agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
                        drawBitmap(masked_scanline, VSM::AUTO, mRenderBase, raster, view->vpBuffer, VSPREAD::REPEAT, view->Opacity, &transform);
                     }
                     else {
                        agg::scanline_u8 scanline;
                        drawBitmap(scanline, VSM::AUTO, mRenderBase, raster, view->vpBuffer, VSPREAD::REPEAT, view->Opacity, &transform);
                     }
                  }
                  else draw_vectors((extVector *)view->Child, state);
               }

               if (view->ClipMask) mClipStack.pop();

               if (view->vpClip) mClipStack.pop();

               mView = saved_viewport;

               mRenderBase.clip_box_naked(save_rb_clip);
            }
            else log.trace("Clipping boundary results in invisible viewport.");

            state.mClip = save_clip;
         }
      }
      else {
         validate_clip_mask(shape); // Drop the mask link lazily if its target has been terminated.
         if (shape->ClipMask) {
            mClipStack.emplace(state, shape->ClipMask, shape);
            mClipStack.top().draw(*this);
         }

         if (shape->GeneratePath) { // A vector that generates a path is one that can be drawn
            #ifdef DBG_DRAW
               log.traceBranch("%s: #%d, Mask: %p", get_name(shape), shape->UID, shape->ClipMask);
            #endif

            if (not mView) {
               // Vector shapes not inside a viewport cannot be drawn (they may exist as definitions for other objects,
               // e.g. as guide paths).
               return;
            }

            const bool render_path = shape_intersects_clip(shape);

            if (render_path) {
               if (shape->FillRaster) {
                  render_fill(state, *shape, *shape->FillRaster, shape->Fill[0]);
                  if (shape->FGFill) render_fill(state, *shape, *shape->FillRaster, shape->Fill[1]);
               }

               if (shape->StrokeRaster) {
                  render_stroke(state, *shape);
               }
            }

            if (has_input_boundary(shape)) {
               // If the vector receives user input events then we record the collision box for the mouse cursor.

               TClipRectangle b;

               if (not shape->BasePath.empty()) {
                  if (shape->Transform.is_normal()) b = shape;
                  else {
                     auto path = shape->Bounds.as_path(shape->Transform);
                     b = get_bounds(path);
                  }

                  // Clipping masks can reduce the boundary further.

                  if (not mClipStack.empty()) {
                     auto &top = mClipStack.top();
                     if ((top.m_clip) and (top.m_clip->Bounds.valid())) {
                        // NB: This hasn't had much testing and doesn't consider nested clips.
                        // The Clip bounds should be post-transform
                        b.shrinking(top.m_clip->Bounds);
                     }
                  }
               }
               else b = { -1, -1, -1, -1 };

               const double abs_x = b.left;
               const double abs_y = b.top;

               TClipRectangle<double> rb_bounds = { double(mRenderBase.xmin()), double(mRenderBase.ymin()), double(mRenderBase.xmax()), double(mRenderBase.ymax()) };
               b.shrinking(rb_bounds);

               mInputBounds.emplace_back(shape->UID, shape->Cursor, b, abs_x, abs_y, has_input_subscriptions(shape) ? false : true);
            }
         } // if: shape->GeneratePath

         if (shape->Child) draw_vectors((extVector *)shape->Child, state);

         if (shape->ClipMask) mClipStack.pop();
      }

      if (bmpBkgd) {
         agg::rasterizer_scanline_aa raster;

         basic_path(raster, bmpBkgd->Clip.Left, bmpBkgd->Clip.Top, bmpBkgd->Clip.Right, bmpBkgd->Clip.Bottom);

         mBitmap = bmpSave;
         mBitmapOriginX = save_origin_x;
         mBitmapOriginY = save_origin_y;
         mFormat.setBitmap(*mBitmap);
         if (not mClipStack.empty()) {
            agg::alpha_mask_gray8 alpha_mask(mClipStack.top().m_renderer);
            agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
            drawBitmap(masked_scanline, shape->Scene->SampleMethod, mRenderBase, raster, bmpBkgd, VSPREAD::CLIP, 1.0);
         }
         else {
            agg::scanline_u8 scanline;
            drawBitmap(scanline, shape->Scene->SampleMethod, mRenderBase, raster, bmpBkgd, VSPREAD::CLIP, 1.0);
         }
      }
   } // for loop
}

//********************************************************************************************************************
// For direct vector drawing via the API, no transforms.

void SimpleVector::DrawPath(objBitmap *Bitmap, double StrokeWidth, OBJECTPTR StrokeStyle, OBJECTPTR FillStyle)
{
   kt::Log log("draw_path");

   agg::scanline_u8  scanline;
   agg::pixfmt_psl   format;
   agg::trans_affine transform; // Dummy transform

   format.setBitmap(*Bitmap);
   mRenderer.attach(format);
   mRenderer.clip_box(Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right-1, Bitmap->Clip.Bottom-1);
   //if (Gamma != 1.0) mRaster.gamma(agg::gamma_power(Gamma));

   log.traceBranch("Bitmap: %p, Stroke: %p (%s), Fill: %p (%s)", Bitmap, StrokeStyle, get_name(StrokeStyle), FillStyle, get_name(FillStyle));

   auto bounds = get_bounds(mPath);
   VectorState state;
   if (FillStyle) {
      mRaster.reset();
      mRaster.add_path(mPath);

      if (FillStyle->classID() IS CLASSID::VECTORCOLOUR) {
         auto colour = (objVectorColour *)FillStyle;
         agg::renderer_scanline_aa_solid solid(mRenderer);
         solid.color(agg::rgba(colour->Red, colour->Green, colour->Blue, colour->Alpha));
         agg::render_scanlines(mRaster, scanline, solid);
      }
      else if (FillStyle->classID() IS CLASSID::VECTORIMAGE) {
         extVectorImage &image = (extVectorImage &)*FillStyle;
         fill_image(state, bounds, mPath, VSM::AUTO, transform, Bitmap->Width, Bitmap->Height, image, mRenderer, mRaster);
      }
      else if (FillStyle->baseClassID() IS CLASSID::GRADIENT) {
         extGradient &gradient = (extGradient &)*FillStyle;
         if (gradient.classID() IS CLASSID::GRADIENTGOURAUD) {
            fill_gouraud(state, bounds, Bitmap->Width, Bitmap->Height, (extGradientGouraud &)gradient, 1.0, mRenderer, mRaster, transform);
         }
         else if (gradient.classID() IS CLASSID::GRADIENTMESH) {
            fill_mesh(state, bounds, Bitmap->Width, Bitmap->Height, (extGradientMesh &)gradient, 1.0, mRenderer, mRaster, transform);
         }
         else if (gradient.Colours) {
            fill_gradient(state, bounds, &mPath, transform, Bitmap->Width, Bitmap->Height, gradient,
               &gradient.Colours->table, mRenderer, mRaster);
         }
      }
      else if (FillStyle->classID() IS CLASSID::VECTORPATTERN) {
         fill_pattern(state, bounds, &mPath, VSM::AUTO, transform, Bitmap->Width, Bitmap->Height, (extVectorPattern &)*FillStyle, mRenderer, mRaster);
      }
      else log.warning("The FillStyle is not supported.");
   }

   if ((StrokeWidth > 0) and (StrokeStyle)) {
      if (StrokeStyle->baseClassID() IS CLASSID::GRADIENT) {
         agg::conv_stroke<agg::path_storage> stroke_path(mPath);
         mRaster.reset();
         mRaster.add_path(stroke_path);

         extGradient &gradient = (extGradient &)*StrokeStyle;
         if (gradient.classID() IS CLASSID::GRADIENTGOURAUD) {
            fill_gouraud(state, bounds, Bitmap->Width, Bitmap->Height, (extGradientGouraud &)gradient, 1.0, mRenderer, mRaster, transform);
         }
         else if (gradient.classID() IS CLASSID::GRADIENTMESH) {
            fill_mesh(state, bounds, Bitmap->Width, Bitmap->Height, (extGradientMesh &)gradient, 1.0, mRenderer, mRaster, transform);
         }
         else if (gradient.Colours) {
            fill_gradient(state, bounds, &mPath, transform, Bitmap->Width, Bitmap->Height, gradient,
               &gradient.Colours->table, mRenderer, mRaster);
         }
      }
      else if (StrokeStyle->classID() IS CLASSID::VECTORPATTERN) {
         agg::conv_stroke<agg::path_storage> stroke_path(mPath);
         mRaster.reset();
         mRaster.add_path(stroke_path);
         fill_pattern(state, bounds, &mPath, VSM::AUTO, transform, Bitmap->Width, Bitmap->Height, (extVectorPattern &)*StrokeStyle, mRenderer, mRaster);
      }
      else if (StrokeStyle->classID() IS CLASSID::VECTORIMAGE) {
         extVectorImage &image = (extVectorImage &)*StrokeStyle;
         agg::conv_transform<agg::path_storage, agg::trans_affine> path(mPath, transform);
         stroke_brush(state, image, mRenderer, path, StrokeWidth);
      }
      else if (StrokeStyle->classID() IS CLASSID::VECTORCOLOUR) {
         agg::renderer_scanline_aa_solid solid(mRenderer);
         agg::conv_stroke<agg::path_storage> stroke_path(mPath);
         mRaster.reset();
         mRaster.add_path(stroke_path);
         objVectorColour *colour = (objVectorColour *)FillStyle;
         solid.color(agg::rgba(colour->Red, colour->Green, colour->Blue, colour->Alpha));
         agg::render_scanlines(mRaster, scanline, solid);
      }
      else log.warning("The StrokeStyle is not supported.");
   }
}

//********************************************************************************************************************

void agg::pixfmt_psl::setBitmap(objBitmap &Bitmap, BLM BlendMode) noexcept
{
   if (BlendMode IS BLM::AUTO) {
      if (Bitmap.ColourSpace IS CS::LINEAR_RGB) BlendMode = BLM::LINEAR;
      else BlendMode = Bitmap.BlendMode;
   }

   rawBitmap(Bitmap.Data, Bitmap.Clip.Right, Bitmap.Clip.Bottom, Bitmap.LineWidth, Bitmap.BitsPerPixel, *Bitmap.ColourFormat, BlendMode);
}

void agg::pixfmt_psl::rawBitmap(uint8_t *Data, int Width, int Height, int Stride, int BitsPerPixel, ColourFormat &ColourFormat, BLM BlendMode) noexcept
{
   mData   = Data;
   mWidth  = Width;
   mHeight = Height;
   mStride = Stride;
   mBytesPerPixel = BitsPerPixel/8;

   if (BitsPerPixel IS 32) {
      // Each branch binds the per-pixel and span operations for a <pixel order, blend mode> pair.
      // The span operations are template instantiations with the per-pixel work inlined, so the
      // function pointer indirection is paid once per span rather than once per pixel.

      if (BlendMode IS BLM::LINEAR) {
         if (ColourFormat.AlphaPos IS 24) {
            if (ColourFormat.BluePos IS 0) set_ops32<2,1,0,3, &linear32BGRA, &linearCopy32BGRA, &linearCover32BGRA>(pxBGRA);
            else set_ops32<0,1,2,3, &linear32RGBA, &linearCopy32RGBA, &linearCover32RGBA>(pxRGBA);
         }
         else if (ColourFormat.RedPos IS 24) set_ops32<3,1,2,0, &linear32AGBR, &linearCopy32AGBR, &linearCover32AGBR>(pxAGBR);
         else set_ops32<1,2,3,0, &linear32ARGB, &linearCopy32ARGB, &linearCover32ARGB>(pxARGB);
      }
      else if (BlendMode IS BLM::SRGB) {
         // The trailing 'true' selects the SSE2 solid-span kernels where available.
         if (ColourFormat.AlphaPos IS 24) {
            if (ColourFormat.BluePos IS 0) set_ops32<2,1,0,3, &srgb32BGRA, &srgbCopy32BGRA, &srgbCover32BGRA, true>(pxBGRA);
            else set_ops32<0,1,2,3, &srgb32RGBA, &srgbCopy32RGBA, &srgbCover32RGBA, true>(pxRGBA);
         }
         else if (ColourFormat.RedPos IS 24) set_ops32<3,1,2,0, &srgb32AGBR, &srgbCopy32AGBR, &srgbCover32AGBR, true>(pxAGBR);
         else set_ops32<1,2,3,0, &srgb32ARGB, &srgbCopy32ARGB, &srgbCover32ARGB, true>(pxARGB);
      }
      else { // BLM::GAMMA
         if (ColourFormat.AlphaPos IS 24) {
            if (ColourFormat.BluePos IS 0) set_ops32<2,1,0,3, &gamma32BGRA, &gammaCopy32BGRA, &gammaCover32BGRA>(pxBGRA);
            else set_ops32<0,1,2,3, &gamma32RGBA, &gammaCopy32RGBA, &gammaCover32RGBA>(pxRGBA);
         }
         else if (ColourFormat.RedPos IS 24) set_ops32<3,1,2,0, &gamma32AGBR, &gammaCopy32AGBR, &gammaCover32AGBR>(pxAGBR);
         else set_ops32<1,2,3,0, &gamma32ARGB, &gammaCopy32ARGB, &gammaCover32ARGB>(pxARGB);
      }
   }
   else if (BitsPerPixel IS 24) {
      kt::Log log;
      log.warning("Support for 24-bit bitmaps is deprecated.");
   }
   else if (BitsPerPixel IS 16) {
      // Deprecated.  16-bit client code should use 32-bit and downscale instead.
      kt::Log log;
      log.warning("Support for 16-bit bitmaps is deprecated.");
   }
   else if (BitsPerPixel IS 8) {
      // For generating grey-scale alpha masks
      fBlendHLine      = &blendHLine8;
      fBlendSolidHSpan = &blendSolidHSpan8;
      fBlendColorHSpan = &blendColorHSpan8;
      fCopyColorHSpan  = &copyColorHSpan8;
      fBlendPix        = &blend8;
      fCopyPix         = &copy8;
      fCoverPix        = &cover8;
   }
}
