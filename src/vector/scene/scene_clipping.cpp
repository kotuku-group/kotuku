// TODO: Currently mask bitmaps are created and torn down on each drawing cycle.  We may be able to cache the bitmaps
// with Vectors when they request a mask.  Bear in mind that caching has to be on a per-vector basis and not in the
// VectorClip itself due to the fact that a given VectorClip can be referenced by many vectors.

//********************************************************************************************************************
// This function recursively draws all child vectors to a bitmap mask in an additive way.
//
// TODO: Currently the paths are transformed dynamically, but we could store a transformed 'MaskPath' permanently with
// the vectors that use them.  When the vector path is dirty, we can clear the MaskPath to force recomputation when
// required.
//
// SVG stipulates that masks constructed from RGB colours use the luminance formula to convert them to a greyscale
// value: ".2126R + .7152G + .0722B".  The best way to apply this is to convert solid colour values to their
// luminesence value prior to drawing them.

static constexpr int CLIP_MASK_CACHE_LIMIT = 16 * 1024 * 1024;

static bool clip_transform_matches(const agg::trans_affine &A, const agg::trans_affine &B)
{
   return (A.sx IS B.sx) and
      (A.sy IS B.sy) and
      (A.shx IS B.shx) and
      (A.shy IS B.shy) and
      (A.tx IS B.tx) and
      (A.ty IS B.ty);
}

//********************************************************************************************************************

bool SceneRenderer::ClipBuffer::use_cached_mask(const agg::trans_affine &Transform, double ParentWidth,
   double ParentHeight)
{
   // A null m_clip is valid: viewport boundary masks (draw_viewport) have no VectorClip and are keyed on the
   // shape's path state alone.  Note that a viewport using both vpClip and a ClipMask shares the one cache slot,
   // distinguished by the Clip pointer, so the two masks evict each other on alternate draws.

   auto &cache = m_shape->ClipCache;

   if ((!cache.Valid) or (cache.Clip != m_clip) or (cache.Bitmap.empty())) return false;
   if (cache.PathTimestamp != m_shape->PathTimestamp) return false;
   if (m_clip) {
      if (cache.ContentVersion != m_clip->ContentVersion) return false;
      if (cache.Units != m_clip->Units) return false;
      if (cache.Flags != m_clip->Flags) return false;
   }
   if ((cache.ParentWidth != ParentWidth) or (cache.ParentHeight != ParentHeight)) return false;
   if (!clip_transform_matches(cache.Transform, Transform)) return false;

   m_width = cache.Width;
   m_height = cache.Height;
   if (m_clip) m_clip->Bounds = cache.Bounds;
   m_renderer.attach(cache.Bitmap.data(), m_width - 1, m_height - 1, m_width);
   return true;
}

//********************************************************************************************************************

void SceneRenderer::ClipBuffer::store_cached_mask(const agg::trans_affine &Transform, double ParentWidth,
   double ParentHeight)
{
   if ((m_width <= 0) or (m_height <= 0)) return;

   if (uint64_t(m_width) * uint64_t(m_height) > CLIP_MASK_CACHE_LIMIT) {
      m_shape->ClipCache.clear();
      return;
   }

   auto &cache = m_shape->ClipCache;
   cache.Bitmap = m_bitmap;
   cache.Bounds = m_clip ? m_clip->Bounds : TClipRectangle<double>();
   cache.Transform = Transform;
   cache.Clip = m_clip;
   cache.ContentVersion = m_clip ? m_clip->ContentVersion : 0;
   cache.PathTimestamp = m_shape->PathTimestamp;
   cache.Width = m_width;
   cache.Height = m_height;
   cache.ParentWidth = ParentWidth;
   cache.ParentHeight = ParentHeight;
   cache.Units = m_clip ? m_clip->Units : VUNIT::UNDEFINED;
   cache.Flags = m_clip ? m_clip->Flags : VCLF::NIL;
   cache.Valid = true;

   m_renderer.attach(cache.Bitmap.data(), m_width - 1, m_height - 1, m_width);
}

//********************************************************************************************************************

void SceneRenderer::ClipBuffer::draw_clips(SceneRenderer &Render, extVector *Shape, agg::rasterizer_scanline_aa<> &Raster,
   agg::renderer_base<agg::pixfmt_gray8> &Base, const agg::trans_affine &Transform)
{
   agg::scanline32_p8 sl;
   for (auto node=Shape; node; node=(extVector *)node->Next) {
      if (node->Class->BaseClassID IS CLASSID::VECTOR) {
         if (node->Visibility != VIS::VISIBLE);
         else if (!node->BasePath.empty()) {
            auto t = node->Transform * Transform;

            if (node->ClipRule IS VFR::NON_ZERO) Raster.filling_rule(agg::fill_non_zero);
            else if (node->ClipRule IS VFR::EVEN_ODD) Raster.filling_rule(agg::fill_even_odd);

            agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> solid(Base);

            if ((m_clip->Flags & (VCLF::APPLY_STROKES|VCLF::APPLY_FILLS)) != VCLF::NIL) {
               if ((m_clip->Flags & VCLF::APPLY_FILLS) != VCLF::NIL) {
                  // When the APPLY_FILLS option is enabled, regular fill painting rules will be applied.

                  if ((node->Fill->Colour.Alpha > 0) and (!node->DisableFillColour)) {
                     double value = (node->Fill->Colour.Red * 0.2126) + (node->Fill->Colour.Green * 0.7152) + (node->Fill->Colour.Blue * 0.0722);
                     value *= node->FillOpacity;
                     solid.color(agg::gray8(value * 0xff, 0xff));

                     agg::conv_transform<agg::path_storage, agg::trans_affine> final_path(node->BasePath, t);
                     Raster.reset();
                     Raster.add_path(final_path);
                     agg::render_scanlines(Raster, sl, solid);
                  }

                  if ((node->Fill->Gradient) or (node->Fill->Image) or (node->Fill->Pattern)) {
                     // The fill routines are written for 32-bit colour rendering, so we have to use active conversion
                     // of RGB colours to grey-scale.  This isn't that much of a problem if the masks remain static and
                     // we cache the results.

                     VectorState state;
                     agg::pixfmt_psl pixf;
                     agg::renderer_base<agg::pixfmt_psl> rb(pixf);
                     ColourFormat cf; // Dummy, not required
                     pixf.rawBitmap(m_bitmap.data(), m_width, m_height, m_width, 8, cf, BLM::LINEAR);
                     rb.attach(pixf);

                     agg::conv_transform<agg::path_storage, agg::trans_affine> final_path(node->BasePath, t);
                     Raster.reset();
                     Raster.add_path(final_path);

                     if (node->Fill->Gradient) {
                        auto gradient = (extVectorGradient *)node->Fill->Gradient;
                        if (gradient->Type IS VGT::GOURAUD) {
                           fill_gouraud(state, node->Bounds, Render.view_width(), Render.view_height(), *gradient,
                              state.mOpacity * node->FillOpacity, rb, Raster, t);
                        }
                        else if (auto table = get_fill_gradient_table(node->Fill[0], state.mOpacity * node->FillOpacity)) {
                           fill_gradient(state, node->Bounds, &node->BasePath, t, Render.view_width(), Render.view_height(),
                              *gradient, table, rb, Raster);
                        }
                      }

                     if (node->Fill->Image) { // Bitmap image fill.  NB: The SVG class creates a standard VectorRectangle and associates an image with it in order to support <image> tags.
                        fill_image(state, node->Bounds, node->BasePath, node->Scene->SampleMethod, t,
                           Render.view_width(), Render.view_height(), *((extVectorImage *)node->Fill->Image), rb, Raster,
                           node->FillOpacity);
                     }

                     if (node->Fill->Pattern) {
                        fill_pattern(state, node->Bounds, &node->BasePath, node->Scene->SampleMethod, t,
                           Render.view_width(), Render.view_height(), *((extVectorPattern *)node->Fill->Pattern), rb, Raster);
                     }
                  }
               }

               if ((m_clip->Flags & VCLF::APPLY_STROKES) != VCLF::NIL) {
                  if (node->StrokeRaster) {
                     double value = (std::clamp<float>(node->Stroke.Colour.Red, 0, 1) * 0.2126) +
                        (std::clamp<float>(node->Stroke.Colour.Green, 0, 1) * 0.7152) +
                        (std::clamp<float>(node->Stroke.Colour.Blue, 0, 1) * 0.0722);
                     value *= node->StrokeOpacity;
                     solid.color(agg::gray8(value * 0xff, 0xff));

                     agg::conv_stroke<agg::path_storage> stroked_path(node->BasePath);
                     configure_stroke(*node, stroked_path);
                     agg::conv_transform<agg::conv_stroke<agg::path_storage>, agg::trans_affine> final_path(stroked_path, t);

                     Raster.reset();
                     Raster.add_path(final_path);
                     agg::render_scanlines(Raster, sl, solid);
                  }
               }
            }
            else {
               // Regular 'clipping path' rules enabled.  SVG states that all paths are filled and stroking is not
               // supported in this mode.

               solid.color(agg::gray8(0xff, 0xff));
               agg::conv_transform<agg::path_storage, agg::trans_affine> final_path(node->BasePath, t);
               Raster.reset();
               Raster.add_path(final_path);
               agg::render_scanlines(Raster, sl, solid);
            }
         }
      }

      if (node->Child) draw_clips(Render, (extVector *)node->Child, Raster, Base, Transform);
   }
}

//********************************************************************************************************************

void SceneRenderer::ClipBuffer::resize_bitmap(int Width, int Height)
{
   if ((Width <= 0) or (Height <= 0)) Width = Height = 1;

   if (Width > 8192)  Width = 8192;
   if (Height > 8192) Height = 8192;

   m_bitmap.resize(Width * Height);
   std::fill(m_bitmap.begin(), m_bitmap.end(), 0);

   m_width  = Width;
   m_height = Height;
}

//********************************************************************************************************************
// Called by the scene graph renderer to generate a bitmap mask for a non-rectangular (transformed) viewport.

void SceneRenderer::ClipBuffer::draw_viewport(SceneRenderer &Render)
{
   auto vp = (extVectorViewport *)m_shape;
   if (vp->dirty()) gen_vector_path(vp);

   // The mask is derived entirely from the viewport's generated path, so PathTimestamp is the key invalidator.

   if (use_cached_mask(vp->Transform, 0, 0)) return;

   resize_bitmap(int(vp->vpBounds.right) + 2, int(vp->vpBounds.bottom) + 2);

   m_renderer.attach(m_bitmap.data(), m_width-1, m_height-1, m_width);
   agg::pixfmt_gray8 pixf(m_renderer);
   agg::renderer_base<agg::pixfmt_gray8> rb(pixf);
   agg::renderer_scanline_aa_solid<agg::renderer_base<agg::pixfmt_gray8>> solid(rb);
   agg::rasterizer_scanline_aa<> rasterizer;

   solid.color(agg::gray8(0xff, 0xff));

   if (!vp->BasePath.empty()) {
      agg::scanline32_p8 sl;
      agg::path_storage final_path(vp->BasePath);

      rasterizer.reset();
      rasterizer.add_path(final_path);
      agg::render_scanlines(rasterizer, sl, solid);
   }

   store_cached_mask(vp->Transform, 0, 0);
}

//********************************************************************************************************************

void SceneRenderer::ClipBuffer::draw(SceneRenderer &Scene)
{
   if (!m_clip->Viewport->Child) {
      kt::Log log;
      log.warning("Clipping viewport has no assigned children.");
      return;
   }

   if (m_clip->Units IS VUNIT::BOUNDING_BOX) draw_bounding_box(Scene);
   else draw_userspace(Scene);
}

//********************************************************************************************************************
// This clip drawing technique borrows the same strategy from Viewport pattern drawing.

void SceneRenderer::ClipBuffer::draw_userspace(SceneRenderer &Scene)
{
   auto transform = m_shape->Transform;

   if (m_shape->classID() IS CLASSID::VECTORTEXT) {
      // This feels a bit hacky and might not necessarily be right... but it does get around the
      // issue of VectorText's positioning around the baseline and having path bounds in negative space.
      transform.tx = 0;
      transform.ty = 0;
   }

   auto parent_width  = get_parent_width(m_shape);
   auto parent_height = get_parent_height(m_shape);

   if (use_cached_mask(transform, parent_width, parent_height)) return;

   auto clip_viewport = (extVectorViewport *)m_clip->Viewport;
   clip_viewport->vpClipConfiguring = true;
   if (!set_render_transform(m_clip->Viewport, transform)) {
      clip_viewport->vpClipConfiguring = false;
      return;
   }

   // Defining the viewport's dimensions is important for clip paths that use scaled coordinates
   //m_clip->Viewport->setFields(fl::X(m_shape->ParentView->vpTargetX), fl::Y(m_shape->ParentView->vpTargetY));
   set_viewport_fixed_size(m_clip->Viewport, parent_width, parent_height);
   clip_viewport->vpClipConfiguring = false;

   m_clip->Bounds = TCR_EXPANDING;
   calc_full_boundary((extVector *)m_clip->Viewport, m_clip->Bounds, false, true, true);

   if (m_clip->Bounds.left > m_clip->Bounds.right) return; // Return if no paths were defined.

   agg::path_storage clip_bound_path = m_clip->Bounds.as_path();
   auto clip_bound_final = get_bounds(clip_bound_path);

   resize_bitmap(int(clip_bound_final.right) + 2, int(clip_bound_final.bottom) + 2);

   // Every child vector of the VectorClip that exports a path will be rendered to the mask.

   m_renderer.attach(m_bitmap.data(), m_width-1, m_height-1, m_width);
   agg::pixfmt_gray8 pixf(m_renderer);
   agg::renderer_base<agg::pixfmt_gray8> rb(pixf);
   agg::rasterizer_scanline_aa<> rasterizer;

   draw_clips(Scene, (extVector *)m_clip->Viewport->Child, rasterizer, rb, agg::trans_affine());
   store_cached_mask(transform, parent_width, parent_height);
}

//********************************************************************************************************************

void SceneRenderer::ClipBuffer::draw_bounding_box(SceneRenderer &Render)
{
   const auto transform = m_shape->Transform;

   if (use_cached_mask(transform, 0, 0)) return;

   TClipRectangle<double> shape_bounds = TCR_EXPANDING; // Bounds *without transforms*
   calc_full_boundary(m_shape, shape_bounds, false, false, false);

   // Set the target area to mock the shape.  The viewbox will remain at (0 0 1 1), or whatever the
   // client has defined if the default is overridden.

   auto clip_viewport = (extVectorViewport *)m_clip->Viewport;
   clip_viewport->vpClipConfiguring = true;
   set_viewport_fixed_bounds(m_clip->Viewport, shape_bounds.left, shape_bounds.top,
      shape_bounds.width(), shape_bounds.height());

   if (!set_render_transform(m_clip->Viewport, build_matrix_transform(m_shape->Matrices))) {
      clip_viewport->vpClipConfiguring = false;
      return;
   }
   clip_viewport->vpClipConfiguring = false;

   m_clip->Bounds = TCR_EXPANDING;
   calc_full_boundary((extVector *)m_clip->Viewport, m_clip->Bounds, false, true, true);

   if (m_clip->Bounds.left > m_clip->Bounds.right) return; // Return if no paths were defined.

   auto clip_bound_path = m_clip->Bounds.as_path(m_shape->Transform);
   auto clip_bound_final = get_bounds(clip_bound_path);

   resize_bitmap(int(clip_bound_final.right) + 2, int(clip_bound_final.bottom) + 2);

   // Every child vector of the VectorClip that exports a path will be rendered to the mask.

   m_renderer.attach(m_bitmap.data(), m_width-1, m_height-1, m_width);
   agg::pixfmt_gray8 pixf(m_renderer);
   agg::renderer_base<agg::pixfmt_gray8> rb(pixf);
   agg::rasterizer_scanline_aa<> rasterizer;

   draw_clips(Render, (extVector *)m_clip->Viewport->Child, rasterizer, rb, transform);
   store_cached_mask(transform, 0, 0);
}
