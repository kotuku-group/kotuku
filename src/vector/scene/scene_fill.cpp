
//********************************************************************************************************************

static void fill_gouraud(VectorState &, const TClipRectangle<double> &, double, double, extGradientGouraud &, double,
   agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &, const agg::trans_affine &, SceneRenderer *);

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

         if (!mClipStack.empty()) {
            agg::alpha_mask_gray8 alpha_mask(mClipStack.top().m_renderer);
            agg::scanline_u8_am<agg::alpha_mask_gray8> mScanLineMasked(alpha_mask);
            agg::render_scanlines(Raster, mScanLineMasked, renderer);
         }
         else agg::render_scanlines(Raster, mScanLine, renderer);
      }
      else {
         agg::renderer_scanline_aa_solid renderer(mRenderBase);
         renderer.color(colour);

         if (!mClipStack.empty()) {
            agg::alpha_mask_gray8 alpha_mask(mClipStack.top().m_renderer);
            agg::scanline_u8_am<agg::alpha_mask_gray8> mScanLineMasked(alpha_mask);
            agg::render_scanlines(Raster, mScanLineMasked, renderer);
         }
         else agg::render_scanlines(Raster, mScanLine, renderer);
      }
   }

   if (Painter.Image) { // Bitmap image fill.  NB: The SVG class creates a standard VectorRectangle and associates an image with it in order to support <image> tags.
      fill_image(State, Vector.Bounds, Vector.BasePath, Vector.Scene->SampleMethod,
         build_fill_transform(Vector, Painter.Image->Units IS VUNIT::USERSPACE, State),
         mView->vpFixedWidth, mView->vpFixedHeight, *((extVectorImage *)Painter.Image), mRenderBase, Raster,
         Vector.FillOpacity * State.mOpacity, this);
   }

   if (Painter.Gradient) {
      auto &gradient = *((extGradient *)Painter.Gradient);
      if (gradient.classID() IS CLASSID::GRADIENTGOURAUD) {
         fill_gouraud(State, Vector.Bounds, mView->vpFixedWidth, mView->vpFixedHeight, (extGradientGouraud &)gradient,
            State.mOpacity * Vector.FillOpacity, mRenderBase, Raster,
            build_fill_transform(Vector, gradient.Units IS VUNIT::USERSPACE, State), this);
      }
      else if (auto table = get_fill_gradient_table(Painter, State.mOpacity * Vector.FillOpacity)) {
         fill_gradient(State, Vector.Bounds, &Vector.BasePath,
            build_fill_transform(Vector, Painter.Gradient->Units IS VUNIT::USERSPACE, State),
            mView->vpFixedWidth, mView->vpFixedHeight, gradient, table, mRenderBase,
            Raster, this);
      }
   }

   if (Painter.Pattern) {
      fill_pattern(State, Vector.Bounds, &Vector.BasePath, Vector.Scene->SampleMethod,
         build_fill_transform(Vector, Painter.Pattern->Units IS VUNIT::USERSPACE, State),
         mView->vpFixedWidth, mView->vpFixedHeight, *((extVectorPattern *)Painter.Pattern), mRenderBase, Raster,
         this);
   }
}

//********************************************************************************************************************
// Image extension
// Path: The original vector path without transforms.
// Transform: Transforms to be applied to the path and to align the image.

static void fill_image(VectorState &State, const TClipRectangle<double> &Bounds, agg::path_storage &Path,
   VSM SampleMethod, const agg::trans_affine &Transform, double ViewWidth, double ViewHeight, extVectorImage &Image,
   agg::renderer_base<agg::pixfmt_psl> &RenderBase, agg::rasterizer_scanline_aa<> &Raster, double Alpha,
   SceneRenderer *Render)
{
   const double c_width  = (Image.Units IS VUNIT::USERSPACE) ? ViewWidth : Bounds.width();
   const double c_height = (Image.Units IS VUNIT::USERSPACE) ? ViewHeight : Bounds.height();
   const double dx = Bounds.left + (Image.X.scaled() ? (c_width * double(Image.X)) : double(Image.X));
   const double dy = Bounds.top + (Image.Y.scaled() ? (c_height * double(Image.Y)) : double(Image.Y));

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
      else SampleMethod = VSM::SPLINE16; // Spline is a better bicubic, it works well for enlarging monotone vectors and avoids sharpening artifacts.
   }

   if ((Render) and (!Render->clip_stack_empty())) {
      agg::alpha_mask_gray8 alpha_mask(Render->clip_stack_top().m_renderer);
      agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
      drawBitmap(masked_scanline, SampleMethod, RenderBase, Raster, Image.Bitmap, Image.SpreadMethod, Alpha,
         &transform);
   }
   else {
      agg::scanline_u8 scanline;
      drawBitmap(scanline, SampleMethod, RenderBase, Raster, Image.Bitmap, Image.SpreadMethod, Alpha, &transform);
   }
}

//********************************************************************************************************************
// Gouraud gradient fill.  Unlike the field-based gradients, a Gouraud gradient has no 1D colour ramp; it is defined
// by a mesh of coloured vertices that are interpolated barycentrically across triangles by agg::span_gouraud_rgba.
// Each triangle is rasterised independently, with the supplied vertex colours converted to the chosen colour space
// and the vertex positions mapped through the gradient's units and transform.
//
// The Raster argument arrives carrying the target vector's fill path, but a Gouraud fill replaces the rasterised
// geometry per-triangle.  The target path still acts as a fill mask via the active clip stack (when present).

// FNV-1a fingerprint of a Gouraud mesh's vertex positions, colours and connectivity.  Comparing fingerprints lets
// the transformed-triangle cache detect when the source mesh has changed (including in-place mutation through the
// authoring fields) without re-running the per-vertex transform and colour conversion every frame.

static uint64_t gouraud_mesh_fingerprint(const GouraudMesh &Mesh)
{
   uint64_t hash = 0xcbf29ce484222325ULL;
   auto mix = [&hash](uint64_t Value) {
      hash ^= Value;
      hash *= 0x100000001b3ULL;
   };

   mix(Mesh.Vertices.size());
   for (auto &v : Mesh.Vertices) {
      mix(std::bit_cast<uint64_t>(v.X));
      mix(std::bit_cast<uint64_t>(v.Y));
      mix(std::bit_cast<uint32_t>(v.Colour.Red));
      mix(std::bit_cast<uint32_t>(v.Colour.Green));
      mix(std::bit_cast<uint32_t>(v.Colour.Blue));
      mix(std::bit_cast<uint32_t>(v.Colour.Alpha));
   }

   mix(Mesh.Indices.size());
   for (auto idx : Mesh.Indices) mix(idx);

   return hash;
}

//********************************************************************************************************************
// Guard against stale or malformed indexed meshes.  The field setter validates incoming arrays, but rendering also
// checks the final mesh so direct C++ edits or legacy data cannot index outside the prepared vertex list.

static bool gouraud_indices_valid(const GouraudMesh &Mesh)
{
   if (Mesh.Indices.empty()) return true;

   const auto total_vertices = Mesh.Vertices.size();
   for (auto idx : Mesh.Indices) {
      if ((idx < 0) or (size_t(idx) >= total_vertices)) {
         kt::Log log;
         log.warning("Ignoring Gouraud gradient with index %d outside the vertex range 0 - %d.",
            idx, int(total_vertices) - 1);
         return false;
      }
   }

   return true;
}

//********************************************************************************************************************
// Render the target fill/stroke raster into a greyscale alpha mask.  Gouraud triangles are then rendered through this
// mask, so triangle geometry remains clipped to the vector path instead of painting outside it.

static bool build_gouraud_path_mask(agg::rasterizer_scanline_aa<> &Raster,
   agg::renderer_base<agg::pixfmt_psl> &RenderBase, SceneRenderer *Render, std::vector<uint8_t> &Bitmap,
   agg::rendering_buffer &Buffer)
{
   // The triangle raster and the path Raster both operate in render-space coordinates.  When rendering into a
   // buffered viewport the target bitmap is offset by (origin_x, origin_y), so render-space (origin_x, origin_y) maps
   // to buffer pixel (0,0) and the render base only spans the buffer dimensions.  The mask, however, is indexed by
   // the render-space coordinates of the triangle scanlines, so it must extend to cover the origin offset as well;
   // otherwise the bottom/right edge of the shape (which sits at render-space y >= buffer height) samples beyond the
   // mask and is silently clipped.  Mirror the clip-mask convention (scene_clipping.cpp) of sizing to the full
   // render-space extent from (0,0).

   const unsigned origin_x = Render ? unsigned(std::max(0, Render->bitmap_origin_x())) : 0;
   const unsigned origin_y = Render ? unsigned(std::max(0, Render->bitmap_origin_y())) : 0;
   const unsigned width = RenderBase.width() + origin_x;
   const unsigned height = RenderBase.height() + origin_y;
   if ((!width) or (!height)) return false;

   Bitmap.assign(size_t(width) * size_t(height), 0);
   Buffer.attach(Bitmap.data(), width, height, int(width));

   agg::pixfmt_gray8 pixf(Buffer);
   agg::renderer_base<agg::pixfmt_gray8> base(pixf);
   agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> solid(base);
   agg::scanline_u8 scanline;

   solid.color(agg::gray8(0xff, 0xff));
   agg::render_scanlines(Raster, scanline, solid);

   if ((Render) and (!Render->clip_stack_empty())) {
      auto &clip = Render->clip_stack_top().m_renderer;
      const unsigned clip_width = clip.width();
      const unsigned clip_height = clip.height();

      for (unsigned y=0; y < height; y++) {
         auto mask_row = Buffer.row_ptr(int(y));
         if (y >= clip_height) {
            std::fill(mask_row, mask_row + width, 0);
            continue;
         }

         auto clip_row = clip.row_ptr(int(y));
         for (unsigned x=0; x < width; x++) {
            if (x >= clip_width) mask_row[x] = 0;
            else mask_row[x] = uint8_t((int(mask_row[x]) * int(clip_row[x]) + 127) / 255);
         }
      }
   }

   return true;
}

//********************************************************************************************************************
// Rebuild the cached transformed/coloured triangle list for a Gouraud gradient.  Vertex positions are mapped through
// the final placement transform and the fill opacity is folded into vertex alpha.  Degenerate (zero-area) triangles
// are dropped here so the render loop never has to filter them.
//
// Colour encoding follows ColourSpace, mirroring how the field gradients treat VCS in GradientColours:
//   * VCS::SRGB (and INHERIT) -> colours are stored sRGB-encoded.  span_gouraud_rgba interpolates linearly in that
//     encoding and emits the bytes verbatim, which the pixel format reads as sRGB.  That is correct sRGB blending.
//   * VCS::LINEAR_RGB -> colours are decoded to linear here, so span_gouraud_rgba_linear interpolates in linear
//     space and re-encodes each output pixel back to sRGB for storage.

static void rebuild_gouraud_cache(extGradientGouraud &Gradient, const GouraudMesh &Mesh, const agg::trans_affine &Transform,
   double Opacity, VCS ColourSpace, uint64_t MeshHash)
{
   const auto &verts = Mesh.Vertices;
   const bool linear = (ColourSpace IS VCS::LINEAR_RGB);

   struct PreparedVertex { double x, y; agg::rgba8 colour; };
   std::vector<PreparedVertex> pv;
   pv.reserve(verts.size());
   for (auto &v : verts) {
      double x = v.X, y = v.Y;
      Transform.transform(&x, &y);
      agg::rgba8 colour(v.Colour, v.Colour.Alpha * Opacity);
      if (linear) { // Decode RGB to linear space; alpha is unaffected by the transfer function.
         colour.r = glLinearRGB.convert(colour.r);
         colour.g = glLinearRGB.convert(colour.g);
         colour.b = glLinearRGB.convert(colour.b);
      }
      pv.push_back({ x, y, colour });
   }

   // Triangle connectivity: indexed list when provided, otherwise a flat triangle soup (every 3 vertices).

   const size_t tri_count = Mesh.Indices.empty() ? (verts.size() / 3) : (Mesh.Indices.size() / 3);

   auto &cache = Gradient.GouraudTriangles;
   cache.Triangles.clear();
   cache.Triangles.reserve(tri_count);

   auto fetch = [&](size_t Tri, int Corner) -> const PreparedVertex & {
      const int idx = Mesh.Indices.empty() ? int(Tri * 3 + Corner) : Mesh.Indices[Tri * 3 + Corner];
      return pv[size_t(idx)];
   };

   for (size_t t=0; t < tri_count; t++) {
      const auto &a = fetch(t, 0);
      const auto &b = fetch(t, 1);
      const auto &c = fetch(t, 2);

      // Drop degenerate (near zero-area) triangles; the base span_gouraud dilates needle triangles, but a true
      // zero-area triangle contributes nothing and would only waste a scanline pass.
      const double area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
      if (std::abs(area) < 1e-6) continue;

      GouraudTriangle tri;
      tri.x[0] = a.x; tri.y[0] = a.y; tri.colour[0] = a.colour;
      tri.x[1] = b.x; tri.y[1] = b.y; tri.colour[1] = b.colour;
      tri.x[2] = c.x; tri.y[2] = c.y; tri.colour[2] = c.colour;
      cache.Triangles.push_back(tri);
   }

   cache.Transform   = Transform;
   cache.MeshHash    = MeshHash;
   cache.Opacity     = Opacity;
   cache.ColourSpace = ColourSpace;
   cache.Valid       = true;
}

static void fill_gouraud(VectorState &State, const TClipRectangle<double> &Bounds, double ViewWidth, double ViewHeight,
   extGradientGouraud &Gradient, double Opacity, agg::renderer_base<agg::pixfmt_psl> &RenderBase,
   agg::rasterizer_scanline_aa<> &Raster, const agg::trans_affine &Transform, SceneRenderer *Render)
{
   if ((!Gradient.Gouraud) or (Gradient.Gouraud->Vertices.empty())) return;

   const auto &mesh = *Gradient.Gouraud;
   if (!gouraud_indices_valid(mesh)) return;

   std::vector<uint8_t> path_mask;
   agg::rendering_buffer path_mask_buffer;
   if (!build_gouraud_path_mask(Raster, RenderBase, Render, path_mask, path_mask_buffer)) return;

   // Map the mesh coordinate space into the target.  BOUNDING_BOX stretches a normalised (0..1) mesh into the
   // path's bounds; USERSPACE positions the mesh directly in the viewport's coordinate system.

   const double c_width  = (Gradient.Units IS VUNIT::USERSPACE) ? ViewWidth  : Bounds.width();
   const double c_height = (Gradient.Units IS VUNIT::USERSPACE) ? ViewHeight : Bounds.height();
   const double x_offset = (Gradient.Units IS VUNIT::USERSPACE) ? 0 : Bounds.left;
   const double y_offset = (Gradient.Units IS VUNIT::USERSPACE) ? 0 : Bounds.top;

   agg::trans_affine transform;
   if (Gradient.Units IS VUNIT::BOUNDING_BOX) {
      transform.scale(c_width, c_height);
      transform.translate(x_offset, y_offset);
   }
   apply_transforms(Gradient, transform);
   transform *= Transform;

   // The transformed triangle list depends only on the mesh data, the final placement transform, the opacity and
   // the colour space.  Reuse the cached result while those inputs are unchanged; otherwise rebuild it.

   const VCS colour_space = Gradient.ColourSpace;
   const uint64_t mesh_hash = gouraud_mesh_fingerprint(mesh);
   if (!Gradient.GouraudTriangles.matches(mesh_hash, transform, Opacity, colour_space)) {
      rebuild_gouraud_cache(Gradient, mesh, transform, Opacity, colour_space, mesh_hash);
   }

   typedef agg::span_allocator<agg::rgba8> span_allocator_type;
   span_allocator_type span_allocator;
   agg::rasterizer_scanline_aa<> triangle_raster;
   agg::alpha_mask_gray8 alpha_mask(path_mask_buffer);
   agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);

   // The Resolution field reduces the gradient's colour resolution, mirroring the field gradients.  It maps to a
   // count of 1/(1-Resolution) distinct colours; for a Gouraud fill that becomes the number of discrete levels each
   // output channel is snapped to.  The 8-bit output span cannot represent more than 256 channel values, so counts
   // at or above 256 disable banding and use the plain span path.

   const double resolution = std::clamp(Gradient.Resolution, 0.0, 1.0);
   const double level_count = (resolution < 1.0) ? std::round(1.0 / (1.0 - resolution)) : 256.0;
   const int levels = int(std::clamp(level_count, 2.0, 256.0));
   const bool quantise = (levels < 256);

   // Render every cached triangle through the chosen Gouraud span generator.  The cache stores colours in the
   // encoding the generator expects (sRGB-encoded for the plain generator, linear-decoded for the linear one).
   //
   // Each triangle is rendered as a separate anti-aliased scanline pass, so the partial edge coverage of adjacent
   // triangles does not sum to full opacity along a shared edge.  That leaves a faint translucent seam where the
   // background bleeds through.  span_gouraud dilates the triangle outward by 'd' into a 6-vertex polygon (colours
   // are still interpolated from the original vertices via miter joins), so feeding the rasteriser that dilated
   // outline makes adjacent triangles overlap by ~'d' pixels.  Because shared edges carry identical colours on both
   // sides, the overlap blends invisibly and seals the seam.

   auto render_triangles = [&]<typename SpanGen>() {
      constexpr double DILATION = 0.5; // px; ~half a pixel of overlap is enough to cover the AA gap
      for (auto &tri : Gradient.GouraudTriangles.Triangles) {
         // The quantising wrapper takes a trailing 'levels' argument that the plain/linear generators do not.
         auto make_span = [&] {
            if constexpr (requires { SpanGen(tri.colour[0], tri.colour[1], tri.colour[2],
               tri.x[0], tri.y[0], tri.x[1], tri.y[1], tri.x[2], tri.y[2], DILATION, levels); }) {
               return SpanGen(tri.colour[0], tri.colour[1], tri.colour[2],
                  tri.x[0], tri.y[0], tri.x[1], tri.y[1], tri.x[2], tri.y[2], DILATION, levels);
            }
            else {
               return SpanGen(tri.colour[0], tri.colour[1], tri.colour[2],
                  tri.x[0], tri.y[0], tri.x[1], tri.y[1], tri.x[2], tri.y[2], DILATION);
            }
         };
         auto span_gen = make_span();

         triangle_raster.reset();
         triangle_raster.add_path(span_gen);
         agg::render_scanlines_aa(triangle_raster, masked_scanline, RenderBase, span_allocator, span_gen);
      }
   };

   if (colour_space IS VCS::LINEAR_RGB) {
      if (quantise) render_triangles.template operator()<agg::span_gouraud_rgba_quantise<agg::span_gouraud_rgba_linear<agg::rgba8>>>();
      else render_triangles.template operator()<agg::span_gouraud_rgba_linear<agg::rgba8>>();
   }
   else {
      if (quantise) render_triangles.template operator()<agg::span_gouraud_rgba_quantise<agg::span_gouraud_rgba<agg::rgba8>>>();
      else render_triangles.template operator()<agg::span_gouraud_rgba<agg::rgba8>>();
   }
}

//********************************************************************************************************************
// FNV-1a fingerprint of a path's vertex data and curve flattening scale.  Used to detect path modification for
// contour gradient caching; hashing is orders of magnitude cheaper than rebuilding the contour's distance transform.

static uint64_t path_fingerprint(const agg::path_storage &Path)
{
   uint64_t hash = 0xcbf29ce484222325ULL;
   auto mix = [&hash](uint64_t Value) {
      hash ^= Value;
      hash *= 0x100000001b3ULL;
   };

   const auto total = Path.total_vertices();
   mix(std::bit_cast<uint64_t>(Path.approximation_scale()));
   mix(total);

   double x, y;
   for (unsigned i=0; i < total; i++) {
      mix(Path.vertex(i, &x, &y));
      mix(std::bit_cast<uint64_t>(x));
      mix(std::bit_cast<uint64_t>(y));
   }
   return hash;
}

static uint64_t mix_fingerprint(uint64_t Hash, uint64_t Value)
{
   Hash ^= Value;
   Hash *= 0x100000001b3ULL;
   return Hash;
}

//********************************************************************************************************************
// Gradient fills.

static void fill_gradient(VectorState &State, const TClipRectangle<double> &Bounds, agg::path_storage *Path,
   const agg::trans_affine &Transform, double ViewWidth, double ViewHeight, extGradient &Gradient,
   GRADIENT_TABLE *Table, agg::renderer_base<agg::pixfmt_psl> &RenderBase,
   agg::rasterizer_scanline_aa<> &Raster, SceneRenderer *Render)
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

   // Most gradients are masked by the target vector's own path, which is carried by Raster.  An SDF gradient is the
   // exception: it describes a signed field that extends beyond the path, so its branch redirects active_raster at a
   // padded coverage region to expose the exterior half of the ramp.

   agg::rasterizer_scanline_aa<> *active_raster = &Raster;

   auto render_gradient = [&]<typename Method>(Method SpreadMethod, double SpanA, double SpanB) {
      typedef agg::span_gradient<agg::rgba8, interpolator_type, Method, color_array_type> span_gradient_type;
      typedef agg::renderer_scanline_aa<RENDERER_BASE_TYPE, span_allocator_type, span_gradient_type> renderer_gradient_type;

      span_gradient_type  span_gradient(span_interpolator, SpreadMethod, *Table, SpanA, SpanB);
      renderer_gradient_type solidrender_gradient(RenderBase, span_allocator, span_gradient);

      if ((!Render) or (Render->clip_stack_empty())) {
         agg::scanline_u8 scanline;
         agg::render_scanlines(*active_raster, scanline, solidrender_gradient);
      }
      else { // Masked gradient
         agg::alpha_mask_gray8 alpha_mask(Render->clip_stack_top().m_renderer);
         agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
         agg::render_scanlines(*active_raster, masked_scanline, solidrender_gradient);
      }
   };

   auto resolve_unit = [](Unit &Value, double Offset, double Span) {
      return Value.scaled() ? Offset + (Span * Value) : Offset + Value;
   };

   auto resolve_span = [](Unit &Value, double Span) {
      return Value.scaled() ? Span * Value : double(Value);
   };

   if (Gradient.classID() IS CLASSID::GRADIENTLINEAR) {
      auto &linear = (extGradientLinear &)Gradient;

      // An undefined coordinate (NaN) leaves the gradient geometry incomplete, so there is nothing to render.
      if (std::isnan(linear.X1) or std::isnan(linear.Y1) or std::isnan(linear.X2) or std::isnan(linear.Y2)) return;

      auto span = MAX_SPAN;
      if (Gradient.Units IS VUNIT::BOUNDING_BOX) {
         // NOTE: In this mode we are mapping a 1x1 gradient square into the target path, which means
         // the gradient is stretched into position as a square.  We don't map the (X,Y) points to the
         // bounding box and draw point-to-point.

         const double x = x_offset + (c_width * linear.X1);
         const double y = y_offset + (c_height * linear.Y1);

         if (linear.CalcAngle) {
            const double dx = linear.X2 - linear.X1;
            const double dy = linear.Y2 - linear.Y1;
            linear.Angle     = atan2(dy, dx);
            linear.Length    = sqrt((dx * dx) + (dy * dy));
            linear.CalcAngle = false;
         }

         transform.scale(linear.Length);
         transform.rotate(linear.Angle);
         transform.scale(c_width / span, c_height / span);
         transform.translate(x, y);
      }
      else {
         TClipRectangle<double> area;
         area.left = resolve_unit(linear.X1, x_offset, c_width);
         area.right = resolve_unit(linear.X2, x_offset, c_width);
         area.top = resolve_unit(linear.Y1, y_offset, c_height);
         area.bottom = resolve_unit(linear.Y2, y_offset, c_height);

         if (linear.CalcAngle) {
            const double dx = area.width();
            const double dy = area.height();
            linear.Angle     = atan2(dy, dx);
            linear.Length    = sqrt((dx * dx) + (dy * dy));
            linear.CalcAngle = false;
         }

         transform.scale(linear.Length / span);
         transform.rotate(linear.Angle);
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
   else if (Gradient.classID() IS CLASSID::GRADIENTRADIAL) {
      auto &radial = (extGradientRadial &)Gradient;

      if (std::isnan(radial.CX) or std::isnan(radial.CY) or std::isnan(radial.Radius)) return;

      agg::point_d c, f;

      double radial_col_span = radial.Radius;
      double focal_radius = radial.FocalRadius;
      if (focal_radius <= 0) focal_radius = radial_col_span;

      if (Gradient.Units IS VUNIT::BOUNDING_BOX) {
         // NOTE: In this mode we are stretching a 1x1 gradient square into the target path.

         c.x = radial.CX;
         c.y = radial.CY;
         f.x = radial.FX.defined() ? double(radial.FX) : c.x;
         f.y = radial.FY.defined() ? double(radial.FY) : c.y;

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
         c.x = resolve_unit(radial.CX, x_offset, c_width);
         c.y = resolve_unit(radial.CY, y_offset, c_height);
         f.x = radial.FX.defined() ? resolve_unit(radial.FX, x_offset, c_width) : c.x;
         f.y = radial.FY.defined() ? resolve_unit(radial.FY, y_offset, c_height) : c.y;

         const double average_view_size = (ViewWidth + ViewHeight) * 0.5;
         radial_col_span = resolve_span(radial.Radius, average_view_size);
         if (radial.FocalRadius > 0) focal_radius = resolve_span(radial.FocalRadius, average_view_size);
         else focal_radius = radial_col_span;

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

         if (radial.ContainFocal) {
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
   else if (Gradient.classID() IS CLASSID::GRADIENTDIAMOND) {
      auto &diamond = (extGradientDiamond &)Gradient;

      if (std::isnan(diamond.CX) or std::isnan(diamond.CY) or std::isnan(diamond.Radius)) return;

      agg::point_d c;

      c.x = resolve_unit(diamond.CX, x_offset, c_width);
      c.y = resolve_unit(diamond.CY, y_offset, c_height);

      // Standard diamond gradient, where the focal point is the same as the gradient center

      double radial_col_span = diamond.Radius;
      if (Gradient.Units IS VUNIT::USERSPACE) {
         if (diamond.Radius.scaled()) radial_col_span = (ViewWidth + ViewHeight) * diamond.Radius * 0.5;
         else transform *= agg::trans_affine_scaling(diamond.Radius * 0.01);
      }
      else { // Align to vector's bounding box
         // Set radial_col_span to the wider of the width/height
         if (c_height > c_width) {
            radial_col_span = c_height * diamond.Radius;
            transform.scaleX(c_width / c_height);
         }
         else {
            radial_col_span = c_width * diamond.Radius;
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
   else if (Gradient.classID() IS CLASSID::GRADIENTCONIC) {
      auto &conic = (extGradientConic &)Gradient;

      if (std::isnan(conic.CX) or std::isnan(conic.CY) or std::isnan(conic.Radius)) return;

      agg::point_d c;

      c.x = resolve_unit(conic.CX, x_offset, c_width);
      c.y = resolve_unit(conic.CY, y_offset, c_height);

      // Standard conic gradient, where the focal point is the same as the gradient center

      double radial_col_span = conic.Radius;
      if (Gradient.Units IS VUNIT::USERSPACE) {
         if (conic.Radius.scaled()) radial_col_span = (ViewWidth + ViewHeight) * conic.Radius * 0.5;
         else transform *= agg::trans_affine_scaling(conic.Radius * 0.01);
      }
      else { // Bounding box
         // Set radial_col_span to the wider of the width/height
         if (c_height > c_width) {
            radial_col_span = c_height * conic.Radius;
            transform.scaleX(c_width / c_height);
         }
         else {
            radial_col_span = c_width * conic.Radius;
            transform.scaleY(c_height / c_width);
         }
      }

      agg::gradient_conic gradient_func;
      transform.translate(c);
      apply_transforms(Gradient, transform);
      transform *= Transform;
      transform.invert();

      if (Gradient.SpreadMethod IS VSPREAD::REFLECT) {
         if (conic.Span < 1.0) {
            agg::gradient_conic_reflect_span span_func(conic.Span);
            agg::gradient_reflect_adaptor<agg::gradient_conic_reflect_span> spread_method(span_func);
            render_gradient(spread_method, 0, radial_col_span);
         }
         else render_gradient(gradient_func, 0, radial_col_span);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::REPEAT) {
         agg::gradient_conic_span span_func(conic.Span);
         agg::gradient_repeat_adaptor<agg::gradient_conic_span> spread_method(span_func);
         render_gradient(spread_method, 0, radial_col_span);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::CLIP) {
         agg::gradient_conic_span span_func(conic.Span);
         agg::gradient_clip_adaptor<agg::gradient_conic_span> spread_method(span_func);
         render_gradient(spread_method, 0, radial_col_span);
      }
      else render_gradient(gradient_func, 0, radial_col_span);
   }
   else if (Gradient.classID() IS CLASSID::GRADIENTCONTOUR) {
      auto &contour = (extGradientContour &)Gradient;
      // The contour's distance transform buffer depends only on the path content, so it is cached with the
      // gradient and reused until the path's fingerprint changes.  The d1/d2 values only affect a small
      // lookup table and can be updated freely on the cached object.

      auto multiplier = std::clamp<double>(contour.Multiplier, 0.01, 10.0);
      auto floor = std::clamp<double>(contour.Floor, 0.0, multiplier);

      bool rebuild = false;
      if (!contour.ContourCache) {
         contour.ContourCache = new agg::gradient_contour();
         rebuild = true;
      }

      auto &gradient_func = *contour.ContourCache;
      gradient_func.d1(floor * 256.0);  // d1 is added to the DT base values
      gradient_func.d2(multiplier);  // d2 is a multiplier of the base DT value

      const auto hash = path_fingerprint(*Path);
      if ((rebuild) or (hash != contour.ContourHash)) {
         gradient_func.contour_create(*Path);
         contour.ContourHash = hash;
      }

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
   else if (Gradient.classID() IS CLASSID::GRADIENTVORONOI) {
      auto &voronoi = (extGradientVoronoi &)Gradient;
      // The Worley field buffer depends on the path and all generation parameters.  The d1/d2 values only affect
      // the small lookup table, so they can be updated without rebuilding the feature-point field.

      auto multiplier = std::clamp<double>(voronoi.Multiplier, 0.01, 10.0);
      auto floor = std::clamp<double>(voronoi.Floor, 0.0, multiplier);

      bool rebuild = false;
      if (!voronoi.WorleyCache) {
         voronoi.WorleyCache = new agg::gradient_worley();
         rebuild = true;
      }

      auto &gradient_func = *voronoi.WorleyCache;
      gradient_func.d1(floor * 256.0);
      gradient_func.d2(multiplier);

      const auto path_hash = path_fingerprint(*Path);
      const auto resolved_seed = voronoi.Seed ? uint64_t(voronoi.Seed) : path_hash;
      const bool use_points = not voronoi.Points.empty();

      uint64_t hash = path_hash;
      hash = mix_fingerprint(hash, uint64_t(int(voronoi.WorleyMode)));
      hash = mix_fingerprint(hash, uint64_t(int(voronoi.WorleyMetric)));
      if (use_points) {
         hash = mix_fingerprint(hash, uint64_t(voronoi.Points.size()));
         for (auto &point : voronoi.Points) {
            hash = mix_fingerprint(hash, std::bit_cast<uint64_t>(point.X));
            hash = mix_fingerprint(hash, std::bit_cast<uint64_t>(point.Y));
            hash = mix_fingerprint(hash, std::bit_cast<uint64_t>(point.Height));
         }
      }
      else {
         hash = mix_fingerprint(hash, resolved_seed);
         hash = mix_fingerprint(hash, uint64_t(voronoi.PointCount));
         hash = mix_fingerprint(hash, std::bit_cast<uint64_t>(voronoi.HeightMin));
         hash = mix_fingerprint(hash, std::bit_cast<uint64_t>(voronoi.HeightMax));
         hash = mix_fingerprint(hash, std::bit_cast<uint64_t>(voronoi.Jitter));
      }

      if ((rebuild) or (hash != voronoi.WorleyHash)) {
         std::vector<agg::worley_feature> points;
         if (use_points) {
            points.reserve(voronoi.Points.size());
            for (auto &point : voronoi.Points) points.push_back({ point.X, point.Y, point.Height });
         }

         gradient_func.worley_create(*Path, resolved_seed, voronoi.PointCount, voronoi.WorleyMode,
            voronoi.WorleyMetric, voronoi.HeightMin, voronoi.HeightMax, voronoi.Jitter,
            use_points ? &points : nullptr);
         voronoi.WorleyHash = hash;
      }

      transform.translate(Bounds.left, Bounds.top);
      apply_transforms(Gradient, transform);
      transform *= Transform;
      transform.invert();

      if (Gradient.SpreadMethod IS VSPREAD::REFLECT) {
         agg::gradient_reflect_adaptor<agg::gradient_worley> spread_method(gradient_func);
         render_gradient(spread_method, 0, 256.0);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::REPEAT) {
         agg::gradient_repeat_adaptor<agg::gradient_worley> spread_method(gradient_func);
         render_gradient(spread_method, 0, 256.0);
      }
      else if (Gradient.SpreadMethod IS VSPREAD::CLIP) {
         agg::gradient_clip_adaptor<agg::gradient_worley> spread_method(gradient_func);
         render_gradient(spread_method, 0, 256.0);
      }
      else render_gradient(gradient_func, 0, 256.0);
   }
   else if (Gradient.classID() IS CLASSID::GRADIENTDISTAL) {
      auto &distal = (extGradientDistal &)Gradient;

      if (std::isnan(distal.Radius) or std::isnan(distal.InnerRadius)) return;

      // The SDF gradient's signed distance field buffer depends on the path content and the configured padding,
      // so it is cached with the gradient and reused until the path's fingerprint changes.  Unlike the contour
      // buffer, it is padded outward so that the exterior half of the colour ramp has room to be rendered.

      auto multiplier = std::clamp<double>(distal.Multiplier, 0.01, 10.0);
      auto floor = std::clamp<double>(distal.Floor, 0.0, multiplier);

      bool rebuild = false;
      if (!distal.SDFCache) {
         distal.SDFCache = new agg::gradient_sdf();
         rebuild = true;
      }

      auto &gradient_func = *distal.SDFCache;
      const double max_bound = (Bounds.width() > Bounds.height()) ? Bounds.width() : Bounds.height();
      const double radius = resolve_span(distal.Radius, max_bound);
      gradient_func.padding(radius);
      gradient_func.inner_radius(resolve_span(distal.InnerRadius, max_bound));
      gradient_func.d1(floor * 256.0);  // d1 is added to the signed DT base values
      gradient_func.d2(multiplier);  // d2 is a multiplier of the base DT value

      // The InnerFall / OuterFall curves are baked into the SDF alpha buffer, so a curve change invalidates the cache
      // exactly like a Radius change (see the field setters, which reset SDFHash).  The GFALL enum values map 1:1 to
      // agg::sdf_falloff_curve; NIL falls back to smoothstep.

      auto fall_curve = [](GFALL Value) {
         if (Value IS GFALL::NIL) return agg::sdf_falloff_curve::smoothstep;
         return agg::sdf_falloff_curve(int(Value));
      };
      gradient_func.inner_fall(fall_curve(distal.InnerFall));
      gradient_func.outer_fall(fall_curve(distal.OuterFall));

      // The spread method selects how the exterior fade behaves beyond the first Radius cycle.  PAD fades once and
      // stops; REPEAT and REFLECT tile the cycle outward until the parent area is covered.  The unsupported VSPREAD
      // variants (PAD-equivalents, REFLECT_X/Y, plus the CLIP handling below) collapse to a single padded fade.

      agg::sdf_spread spread = agg::sdf_spread::pad;
      if (Gradient.SpreadMethod IS VSPREAD::REPEAT) spread = agg::sdf_spread::repeat;
      else if (Gradient.SpreadMethod IS VSPREAD::REFLECT) spread = agg::sdf_spread::reflect;
      gradient_func.spread(spread);

      // For REPEAT/REFLECT the exterior must extend far enough to fill the parent area.  The cycle length stays at
      // Radius; the fill extent is the furthest the field has to reach from the path bounds to cover the view, so a
      // tiled fade always paints out to (and a little past) the parent edges regardless of where the shape sits.

      double fill_extent = radius;
      if (spread != agg::sdf_spread::pad) {
         const double reach_x = std::max(Bounds.left, ViewWidth - Bounds.right);
         const double reach_y = std::max(Bounds.top, ViewHeight - Bounds.bottom);
         fill_extent = std::max({ radius, reach_x, reach_y });
      }
      gradient_func.fill_extent(fill_extent);

      // Resolution stepping is baked into the SDF alpha buffer too.  The base Resolution setter does not touch the
      // SDF cache, so a change is detected here and forces a rebuild alongside the path fingerprint check.

      const double resolution = std::clamp(Gradient.Resolution, 0.0, 1.0);
      gradient_func.resolution(resolution);

      const auto hash = path_fingerprint(*Path);
      if ((rebuild) or (hash != distal.SDFHash) or (resolution != distal.SDFResolution) or
          (int(spread) != distal.SDFSpread) or (fill_extent != distal.SDFExtent)) {
         gradient_func.sdf_create(*Path);
         distal.SDFHash = hash;
         distal.SDFResolution = resolution;
         distal.SDFSpread = int(spread);
         distal.SDFExtent = fill_extent;
      }

      // The path is rendered into the SDF buffer at an inset of (origin_x, origin_y) to leave a margin for the
      // exterior field.  Offset the gradient sampling transform by the same amount so the outline stays aligned
      // with the path edge.

      const double margin_x = gradient_func.sdf_origin_x();
      const double margin_y = gradient_func.sdf_origin_y();
      const double region_x = Bounds.left - margin_x;
      const double region_y = Bounds.top - margin_y;

      transform.translate(region_x, region_y);
      apply_transforms(Gradient, transform);
      transform *= Transform;
      transform.invert();

      agg::rasterizer_scanline_aa<> sdf_raster;

      if (Gradient.SpreadMethod != VSPREAD::CLIP) {
         // Unlike the path-masked gradients, the SDF describes a signed field that extends beyond the target path.
         // To expose the exterior half of the ramp the coverage is widened to the full padded field extent rather
         // than the path itself.  Any active clip stack still constrains the result inside render_gradient.

         agg::path_storage region;
         region.move_to(region_x, region_y);
         region.line_to(region_x + gradient_func.sdf_width(), region_y);
         region.line_to(region_x + gradient_func.sdf_width(), region_y + gradient_func.sdf_height());
         region.line_to(region_x, region_y + gradient_func.sdf_height());
         region.close_polygon();
         agg::conv_transform<agg::path_storage> region_trans(region, Transform);
         sdf_raster.add_path(region_trans);
         active_raster = &sdf_raster;
      }

      // The SDF glow uses a dedicated span generator that modulates per-pixel alpha by a smoothstep falloff so the
      // exterior fades to transparent over the padding distance and stops contributing once alpha reaches zero.
      // If SpreadMethod is CLIP, the original target path raster remains active and clips the field to the fill.

      typedef agg::span_gradient_sdf<agg::rgba8, interpolator_type, color_array_type> sdf_span_type;
      typedef agg::renderer_scanline_aa<RENDERER_BASE_TYPE, span_allocator_type, sdf_span_type> sdf_renderer_type;

      sdf_span_type sdf_span(span_interpolator, gradient_func, *Table, 0, 256.0);
      sdf_renderer_type sdf_render(RenderBase, span_allocator, sdf_span);

      if ((!Render) or (Render->clip_stack_empty())) {
         agg::scanline_u8 scanline;
         agg::render_scanlines(*active_raster, scanline, sdf_render);
      }
      else {
         agg::alpha_mask_gray8 alpha_mask(Render->clip_stack_top().m_renderer);
         agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
         agg::render_scanlines(*active_raster, masked_scanline, sdf_render);
      }
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
   agg::rasterizer_scanline_aa<> &Raster, SceneRenderer *Render)
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
      if (Pattern.Width.scaled()) target_width = elem_width * Pattern.Width;
      else if (Pattern.Width.defined()) target_width = Pattern.Width;
      else target_width = 1;

      if (Pattern.Height.scaled()) target_height = elem_height * Pattern.Height;
      else if (Pattern.Height.defined()) target_height = Pattern.Height;
      else target_height = 1;

      if (Pattern.X.scaled()) dx = x_offset + (elem_width * Pattern.X);
      else if (Pattern.X.defined()) dx = x_offset + Pattern.X;
      else dx = x_offset;

      if (Pattern.Y.scaled()) dy = y_offset + (elem_height * Pattern.Y);
      else if (Pattern.Y.defined()) dy = y_offset + Pattern.Y;
      else dy = y_offset;

      auto page_width = int(target_width);
      auto page_height = int(target_height);

      if ((page_width != Pattern.Scene->PageWidth) or (page_height != Pattern.Scene->PageHeight)) {
         Pattern.Scene->PageWidth  = page_width;
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
         Pattern.Viewport->setViewWidth(double(Pattern.Width));
         Pattern.Viewport->setViewHeight(double(Pattern.Height));
      }

      if (Pattern.Width.scaled()) target_width = Pattern.Width * elem_width;
      else target_width = Pattern.Width;

      if (Pattern.Height.scaled()) target_height = Pattern.Height * elem_height;
      else target_height = Pattern.Height;

      dx = x_offset + ((elem_width * Pattern.X) * (SCALE_BITMAP ? t_scale : 1.0));
      dy = y_offset + ((elem_height * Pattern.Y) * (SCALE_BITMAP ? t_scale : 1.0));

      // Scale the bitmap so that it matches the final scale on the display.  This requires a matching inverse
      // adjustment when computing the final transform.

      auto page_width = int(target_width * (SCALE_BITMAP ? t_scale : 1.0));
      auto page_height = int(target_height * (SCALE_BITMAP ? t_scale : 1.0));

      // Mark the bitmap for recomputation if needed.

      if ((page_width != Pattern.Scene->PageWidth) or (page_height != Pattern.Scene->PageHeight)) {
         if ((page_width < 1) or (page_height < 1) or (page_width > 8192) or (page_height > 8192)) {
            // Dimensions in excess of reasonable values can occur if the user is confused over the application
            // of bounding-box values that are being scaled.
            kt::Log log(__FUNCTION__);
            log.warning("Invalid pattern dimensions of %dx%d detected.", page_width, page_height);
            page_width  = 1;
            page_height = 1;
         }
         Pattern.Scene->PageWidth  = page_width;
         Pattern.Scene->PageHeight = page_height;
         mark_dirty(Pattern.Scene->Viewport, RC::DIRTY);
      }
   }

   // The pattern's scene may carry a lingering transform if it was also consumed as a viewport fill
   // (see draw_vectors()), so restore the identity transform before checking for a redraw.
   set_render_transform(Pattern.Scene->Viewport, agg::trans_affine());

   // Redraw the pattern source if any part of the definition has been marked dirty.
   if ((((extVectorScene *)Pattern.Scene)->SubtreeDirty) or (!Pattern.Bitmap)) {
      if (acDraw(&Pattern) != ERR::Okay) return;
   }

   agg::trans_affine transform;

   if (not Pattern.Matrices.empty()) { // Client used the 'patternTransform' SVG attribute
      auto &m = Pattern.Matrices[0];
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

   if ((Render) and (!Render->clip_stack_empty())) {
      agg::alpha_mask_gray8 alpha_mask(Render->clip_stack_top().m_renderer);
      agg::scanline_u8_am<agg::alpha_mask_gray8> masked_scanline(alpha_mask);
      drawBitmap(masked_scanline, SampleMethod, RenderBase, Raster, Pattern.Bitmap, Pattern.SpreadMethod,
         Pattern.Opacity, &transform);
   }
   else {
      agg::scanline_u8 scanline;
      drawBitmap(scanline, SampleMethod, RenderBase, Raster, Pattern.Bitmap, Pattern.SpreadMethod, Pattern.Opacity,
         &transform);
   }
}
