// Included by filter.cpp to exercise the private planner, renderer and surface pool through real filter objects.

struct filter_test_scene {
   extVectorScene::create Scene;
   extVectorViewport *Viewport = nullptr;
   extVectorRectangle *Client = nullptr;
   extVectorFilter *Filter = nullptr;

   filter_test_scene(int Width = 8, int Height = 4)
      : Scene({ fl::PageWidth(Width), fl::PageHeight(Height) }) {
      if (not Scene.ok()) return;
      Viewport = extVectorViewport::create::local(fl::Owner(Scene->UID), fl::Width(Width), fl::Height(Height));
      if (not Viewport) return;
      Client = extVectorRectangle::create::local(fl::Owner(Viewport->UID), fl::Width(Width), fl::Height(Height));
      if (not Client) return;
      gen_vector_path(Viewport);
      gen_vector_path(Client);
      Filter = extVectorFilter::create::local(fl::Owner(Scene->UID), fl::Units(VUNIT::USERSPACE),
         fl::X(-2), fl::Y(0), fl::Width(Width + 2), fl::Height(Height));
      if (Filter) Filter->PrimitiveUnits = VUNIT::USERSPACE;
   }

   ~filter_test_scene() {
      if (Filter) FreeResource(Filter);
      if (Client) FreeResource(Client);
      if (Viewport) FreeResource(Viewport);
   }

   bool ready() const { return Filter and Client; }

   ERR draw(filter_render_result &Output) const {
      ScopedObjectAccess lock(Filter);
      if (lock.error != ERR::Okay) return lock.error;
      const auto error = render_filter(Filter, Viewport, Client, nullptr, VCS::SRGB, Output, 0, 0);
      if (error IS ERR::Okay) finish_filter_composition(Filter);
      return error;
   }
};

static bool filter_test_expect(bool Condition, CSTRING Message)
{
   if (not Condition) kt::Log("FilterTest").warning("%s", Message);
   return Condition;
}

static bool filter_test_pixel(const objBitmap *Bitmap, int X, int Y, std::array<uint8_t, 4> Expected)
{
   if ((not Bitmap) or (X < 0) or (Y < 0) or (X >= Bitmap->Width) or (Y >= Bitmap->Height)) return false;
   const auto pixel = Bitmap->Data + std::ptrdiff_t(Y) * Bitmap->LineWidth + std::ptrdiff_t(X) * 4;
   const auto &format = *Bitmap->ColourFormat;
   const std::array<uint8_t, 4> actual { pixel[format.RedPos >> 3], pixel[format.GreenPos >> 3],
      pixel[format.BluePos >> 3], pixel[format.AlphaPos >> 3] };
   if (actual IS Expected) return true;
   kt::Log("FilterTest").warning("Sample %d,%d was %u,%u,%u,%u. Expected %u,%u,%u,%u.", X, Y,
      actual[0], actual[1], actual[2], actual[3], Expected[0], Expected[1], Expected[2], Expected[3]);
   return false;
}

static ERR draw_filter_test_mask(extFilterEffect *Effect)
{
   if (not filter_test_expect((CurrentContext() IS Effect) and Effect->locked(),
       "Mask Draw did not run in the locked effect context.")) return ERR::InvalidState;
   const auto step = active_filter_step(Effect);
   if (not step) return ERR::InvalidState;
   const auto &render = *Effect->Filter->Render;

   objBitmap::create child({ fl::Width(1), fl::Height(1), fl::BitsPerPixel(32), fl::Flags(BMF::NO_DATA) });
   if (not child.ok()) return child.error;
   if (not filter_test_expect(child->Owner IS Effect, "Mask Draw resources must be owned by the effect.")) {
      return ERR::InvalidState;
   }

   const auto &region = step->Subregion;
   if (not filter_test_expect((region.left IS 1) and (region.right IS 5), "Mask enlarged the image subregion.")) {
      return ERR::InvalidValue;
   }
   filter_map_view mask;
   if (auto error = get_filter_map(Effect, "Mask", mask); error != ERR::Okay) return error;
   if (mask.Source.Data) {
      const auto alpha = mask.sample(1.5, 1.5)[0];
      auto luma = mask;
      luma.Contract.XChannel = filter_map_channel::LUMINANCE;
      const auto encoded = luma.sample(1.5, 1.5)[0];
      luma.Contract.Transfer = filter_map_transfer::LINEAR;
      const auto linear = luma.sample(1.5, 1.5)[0];
      if (not filter_test_expect((alpha IS 127.0f / 255.0f) and
          (std::abs(encoded - 0.106717f) < 0.00001f) and (std::abs(linear - 0.045892f) < 0.00001f),
          "Bitmap map alpha, encoded luminance or linear luminance was incorrect.")) return ERR::InvalidValue;

      filter_surface_view premultiplied;
      if (auto error = premultiplied_input(Effect, mask.Source, CS::SRGB, premultiplied); error != ERR::Okay) {
         return error;
      }
      luma.Source = premultiplied;
      luma.Contract.Transfer = filter_map_transfer::STORED;
      if (not filter_test_expect(std::abs(luma.sample(1.5, 1.5)[0] - encoded) < 0.002f,
          "Bitmap map was sampled with premultiplied colour channels.")) return ERR::InvalidValue;
   }

   if (auto error = COMPOSITEFX_Draw((extCompositeFX *)Effect, nullptr); error != ERR::Okay) return error;
   auto bitmap = Effect->Target;
   const auto alpha = bitmap->ColourFormat->AlphaPos >> 3;
   for (int y = 0; y < bitmap->Height; y++) {
      auto row = bitmap->Data + std::ptrdiff_t(y) * bitmap->LineWidth;
      for (int x = 0; x < bitmap->Width; x++) {
         const auto weight = mask.sample(render.VectorClip.left + filter_pixel_centre(x),
            render.VectorClip.top + filter_pixel_centre(y))[0];
         row[x * 4 + alpha] = uint8_t(std::lround(row[x * 4 + alpha] * weight));
      }
   }
   return ERR::Okay;
}

struct extFilterMaskTest : extCompositeFX {
   static constexpr CSTRING CLASS_NAME = glFilterMaskTestClass;
   static constexpr CLASSID CLASS_ID = CLASSID(kt::strihash(CLASS_NAME));
   using create = kt::Create<extFilterMaskTest>;
   using extCompositeFX::extCompositeFX;

   static ERR draw(extFilterMaskTest *Self, struct acDraw *) { return draw_filter_test_mask(Self); }
};

struct extFilterSurfaceTest : extFilterEffect {
   static constexpr CSTRING CLASS_NAME = glFilterSurfaceTestClass;
   static constexpr CLASSID CLASS_ID = CLASSID(kt::strihash(CLASS_NAME));
   using create = kt::Create<extFilterSurfaceTest>;
   using extFilterEffect::extFilterEffect;

   std::function<ERR(extFilterEffect *)> DrawRoutine;

   static ERR draw(extFilterSurfaceTest *Self, struct acDraw *) {
      if ((not active_filter_step(Self)) or (not Self->DrawRoutine)) return ERR::InvalidState;
      return Self->DrawRoutine(Self);
   }
};

template<class Effect>
static objMetaClass::create register_filter_test_class()
{
   static const ActionArray actions[] = {
      { AC::New, +[](Effect *Self) {
         new (Self) Effect(Self->Class, Self->UID);
         return ERR::Okay;
      } },
      { AC::Free, +[](Effect *Self) {
         Self->~Effect();
         return ERR::Okay;
      } },
      { AC::Draw, Effect::draw },
      { AC::NIL, nullptr }
   };
   static const FieldArray fields[] = { END_FIELD };
   return objMetaClass::create({ fl::Name(Effect::CLASS_NAME), fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(Effect::CLASS_ID), fl::Size(sizeof(Effect)), fl::Actions(actions), fl::Fields(fields) });
}

static void count_filter_test_draw(OBJECTPTR Effect, ACTIONID Action, ERR Result, APTR, APTR Meta)
{
   if ((Action IS AC::Draw) and (Result IS ERR::Okay) and Effect->locked()) ++*(int *)Meta;
}

static bool test_filter_named_input()
{
   filter_named_input slots[] {
      { "Mask", { .Kind = filter_input_kind::SCALAR_MAP, .ContributesRegion = false,
         .Fallback = { 1.0f, 0.0f }, .Sampling = { filter_access::BOUNDED, 1, 0 } } }
   };
   filter_named_input later_slots[] {
      { "Mask", { .Kind = filter_input_kind::SCALAR_MAP } }
   };
   const filter_effect_extension mask_operation { slots };
   const filter_effect_extension later_operation { later_slots };
   auto fixture_class = register_filter_test_class<extFilterMaskTest>();
   if (not fixture_class.ok()) return false;
   filter_test_scene scene;
   if (not scene.ready()) return false;
   const auto owner = fl::Owner(scene.Filter->UID);
   auto red = extFloodFX::create::local(owner, fl::X(1), fl::Width(2));
   auto blue = extFloodFX::create::local(owner, fl::X(3), fl::Width(2));
   auto mask = extFloodFX::create::local(owner);
   if ((not red) or (not blue) or (not mask)) return false;
   FRGB red_colour(1, 0, 0), blue_colour(0, 0, 1), mask_colour(128.0f / 255.0f, 0, 0, 0.5f);
   FLOODFX_SET_Colour(red, &red_colour);
   FLOODFX_SET_Colour(blue, &blue_colour);
   FLOODFX_SET_Colour(mask, &mask_colour);
   auto composite = extFilterMaskTest::create::local(owner, fl::Input(red), kt::FieldValue("Mix", blue));
   auto later = extFloodFX::create::local(owner);
   if ((not composite) or (not later)) return false;
   composite->FilterExtension = &mask_operation;
   later->FilterExtension = &later_operation;
   extVectorFilter::create other({ fl::Owner(scene.Scene->UID) });
   if (not other.ok()) return false;
   extFloodFX::create foreign({ fl::Owner(other->UID) });
   if (not foreign.ok()) return false;

   int draw_notifications = 0;
   if (SubscribeAction(composite, AC::Draw, C_FUNCTION(count_filter_test_draw, &draw_notifications)) != ERR::Okay) {
      return false;
   }
   auto unsubscribe = kt::Defer([&]() { UnsubscribeAction(composite, AC::Draw); });
   filter_render_result output;
   if (not filter_test_expect(set_named_filter_input(composite, "Mask", VSF::REFERENCE, composite) IS ERR::Recursion,
       "A named slot accepted a self-reference.")) return false;
   if (not filter_test_expect(
       set_named_filter_input(composite, "Mask", VSF::REFERENCE, foreign.get()) IS ERR::InvalidValue,
       "A named slot accepted a producer from another filter.")) return false;
   if (not filter_test_expect(set_named_filter_input(composite, "Mask", VSF::REFERENCE, later) IS ERR::Okay and
       scene.draw(output) IS ERR::InvalidValue, "A named slot accepted a forward reference during drawing.")) {
      return false;
   }
   if (not filter_test_expect(set_named_filter_input(later, "Mask", VSF::REFERENCE, composite) IS ERR::Okay and
       scene.draw(output) IS ERR::Recursion, "A cycle involving named slots passed graph validation.")) return false;
   if (FreeResource(later) != ERR::Okay) return false;
   if (not filter_test_expect((not slots[0].Reference) and (slots[0].Source IS VSF::REFERENCE) and
       scene.draw(output) IS ERR::FieldNotSet, "Deleting a named producer did not invalidate the binding.")) {
      return false;
   }
   if (set_named_filter_input(composite, "Mask", VSF::REFERENCE, mask) != ERR::Okay) return false;
   if (not filter_test_expect(scene.draw(output) IS ERR::Okay and (output.OriginX IS -1) and
       (not mask->Target), "Named sampling reach or last-consumer release was incorrect.")) return false;
   if (not filter_test_expect(draw_notifications IS 1, "The first successful mask draw must notify once.")) {
      return false;
   }
   if ((not filter_test_pixel(output.Bitmap, 2, 1, { 255, 0, 0, 127 })) or
       (not filter_test_pixel(output.Bitmap, 4, 1, { 0, 0, 255, 127 })) or
       (not filter_test_pixel(output.Bitmap, 6, 1, { 0, 0, 0, 0 }))) return false;

   if (set_named_filter_input(composite, "Mask", VSF::NONE) != ERR::Okay) return false;
   return filter_test_expect(scene.draw(output) IS ERR::Okay and (output.OriginX IS 0),
      "An unbound mask still contributed sampling reach.") and
      filter_test_pixel(output.Bitmap, 1, 1, { 255, 0, 0, 255 }) and
      filter_test_expect(draw_notifications IS 2, "Each successful mask draw must notify the subscriber once.");
}

static ERR draw_filter_test_scalar(extFilterEffect *Effect)
{
   if (not filter_test_expect((CurrentContext() IS Effect) and Effect->locked(),
       "Scalar Draw did not run in the locked effect context.")) return ERR::InvalidState;
   filter_surface_view layout;
   float *data;
   if (auto error = get_numeric_target(Effect, layout, data); error != ERR::Okay) return error;
   if ((layout.OriginX != -2) or (layout.Stride != 10 * int(sizeof(float)))) return ERR::InvalidValue;
   auto row = (float *)((uint8_t *)data + layout.Stride);
   row[0] = 0.75f; // This sample is outside the primitive region.
   row[1] = -0.12512344f;
   row[2] = 1.7512345f;
   row[3] = std::numeric_limits<float>::infinity();
   return ERR::Okay;
}

static ERR draw_filter_test_vector(extFilterEffect *Effect)
{
   filter_map_view height;
   if (auto error = get_filter_map(Effect, "Height", height); error != ERR::Okay) return error;
   const auto signed_value = height.sample(-0.5, 1.5)[0];
   const auto high_value = height.sample(0.5, 1.5)[0];
   if (not filter_test_expect((signed_value IS -0.12512344f) and (high_value IS 1.7512345f) and
       (height.sample(0.0, 1.5)[0] IS (signed_value + high_value) * 0.5f) and
       (height.sample(1.0, 1.5)[0] IS (high_value + 0.125f) * 0.5f) and
       (height.sample(-1.5, 1.5)[0] IS 0.125f) and (height.sample(1.5, 1.5)[0] IS 0.125f) and
       (height.sample(0.5, 2.5)[0] IS 0.125f), "Scalar sampling changed raw values or mishandled invalid taps.")) {
      return ERR::InvalidValue;
   }

   filter_surface_view layout;
   float *data;
   if (auto error = get_numeric_target(Effect, layout, data); error != ERR::Okay) return error;
   if (layout.Stride != 20 * int(sizeof(float))) return ERR::InvalidValue;
   auto row = (float *)((uint8_t *)data + layout.Stride);
   row[2] = signed_value;
   row[3] = high_value;
   row[4] = 0.50123f;
   row[5] = -0.68765f;
   row[6] = 0.75f;
   row[7] = std::numeric_limits<float>::infinity();
   return ERR::Okay;
}

static ERR draw_filter_test_bitmap(extFilterEffect *Effect)
{
   filter_map_view coordinates;
   if (auto error = get_filter_map(Effect, "Coordinates", coordinates); error != ERR::Okay) return error;
   if (not filter_test_expect((coordinates.sample(-0.5, 1.5) IS filter_map_value({ -0.12512344f, 1.7512345f })) and
       (coordinates.sample(0.5, 1.5) IS filter_map_value({ 0.50123f, -0.68765f })) and
       (coordinates.sample(1.5, 1.5) IS filter_map_value({ 5.0f, -3.0f })) and
       (coordinates.sample(10.5, 1.5) IS coordinates.sample(0.5, 1.5)),
       "Vector sampling changed raw values or mishandled invalid taps and repeat addressing.")) {
      return ERR::InvalidValue;
   }

   const auto &render = *Effect->Filter->Render;
   const auto live_bytes = render.LiveBytes;
   if (not filter_test_expect(live_bytes IS 4 * render.BitmapBytes, "Live numerical bytes were undercounted.")) {
      return ERR::InvalidValue;
   }
   filter_surface_view grey, rg;
   if (auto error = get_named_filter_bitmap(Effect, "Grey", grey); error != ERR::Okay) return error;
   if (auto error = get_named_filter_bitmap(Effect, "RG", rg); error != ERR::Okay) return error;
   if (not filter_test_expect(render.LiveBytes IS live_bytes + 2 * render.BitmapBytes,
       "Bitmap conversion bypassed the surface budget.")) return ERR::InvalidValue;
   if ((not filter_test_pixel(grey.Bitmap, 1, 1, { 0, 0, 0, 255 })) or
       (not filter_test_pixel(grey.Bitmap, 2, 1, { 255, 255, 255, 255 })) or
       (not filter_test_pixel(grey.Bitmap, 3, 1, { 0, 0, 0, 0 })) or
       (not filter_test_pixel(rg.Bitmap, 1, 1, { 0, 255, 0, 255 })) or
       (not filter_test_pixel(rg.Bitmap, 2, 1, { 128, 0, 0, 255 })) or
       (not filter_test_pixel(rg.Bitmap, 3, 1, { 0, 0, 0, 0 }))) return ERR::InvalidValue;

   return copy_filter_bitmap(rg.Bitmap, Effect->Target, BAF::NIL, 0, 0, rg.Width, rg.Height, 0, 0);
}

static bool test_filter_numerical_surfaces()
{
   using enum filter_surface_format;
   filter_named_input vector_inputs[] {
      { "Height", { .Kind = filter_input_kind::SCALAR_MAP, .ContributesRegion = false,
         .Transfer = filter_map_transfer::LINEAR, .Fallback = { 0.125f, 0.0f },
         .Interpolation = filter_interpolation::BILINEAR, .Sampling = { filter_access::BOUNDED, 2, 0 } } }
   };
   filter_named_input bitmap_inputs[] {
      { "Coordinates", { .Kind = filter_input_kind::VECTOR_MAP, .ContributesRegion = false,
         .Transfer = filter_map_transfer::LINEAR, .Fallback = { 5.0f, -3.0f }, .Edge = FEM::REPEAT,
         .Sampling = { filter_access::FULL_INPUT, filter_sampling::UNBOUNDED, 0 } } },
      { "Grey", { .ContributesRegion = false, .BitmapConversion = filter_bitmap_conversion::UNIT_CHANNELS } },
      { "RG", { .ContributesRegion = false, .BitmapConversion = filter_bitmap_conversion::UNIT_CHANNELS } }
   };
   const filter_effect_extension scalar_operation { {}, SCALAR_F32 };
   const filter_effect_extension vector_operation { vector_inputs, VECTOR_F32 };
   const filter_effect_extension bitmap_operation { bitmap_inputs };
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   if (not fixture_class.ok()) return false;
   filter_test_scene scene;
   if (not scene.ready()) return false;
   scene.Filter->ColourSpace = VCS::LINEAR_RGB;
   const auto owner = fl::Owner(scene.Filter->UID);
   auto scalar = extFilterSurfaceTest::create::local(owner, fl::X(-1), fl::Width(4));
   auto vector = extFilterSurfaceTest::create::local(owner);
   auto converter = extFilterSurfaceTest::create::local(owner);
   if ((not scalar) or (not vector) or (not converter)) return false;
   scalar->FilterExtension = &scalar_operation;
   vector->FilterExtension = &vector_operation;
   converter->FilterExtension = &bitmap_operation;
   scalar->DrawRoutine = draw_filter_test_scalar;
   vector->DrawRoutine = draw_filter_test_vector;
   converter->DrawRoutine = draw_filter_test_bitmap;
   if ((set_named_filter_input(vector, "Height", VSF::REFERENCE, scalar) != ERR::Okay) or
       (set_named_filter_input(converter, "Coordinates", VSF::REFERENCE, vector) != ERR::Okay) or
       (set_named_filter_input(converter, "Grey", VSF::REFERENCE, scalar) != ERR::Okay) or
       (set_named_filter_input(converter, "RG", VSF::REFERENCE, vector) != ERR::Okay)) return false;
   auto legacy = extOffsetFX::create::local(owner, fl::Input(vector));
   if (not legacy) return false;

   filter_render_result output;
   if (not filter_test_expect(scene.draw(output) IS ERR::NoSupport, "A bitmap-only effect accepted a float input.")) {
      return false;
   }
   if (FILTEREFFECT_SET_Input(legacy, converter) != ERR::Okay) return false;
   bitmap_inputs[2].Contract.BitmapConversion = filter_bitmap_conversion::NONE;
   if (not filter_test_expect(scene.draw(output) IS ERR::NoSupport, "A bitmap slot implicitly converted a field.")) {
      return false;
   }
   bitmap_inputs[2].Contract.BitmapConversion = filter_bitmap_conversion::UNIT_CHANNELS;
   vector_inputs[0].Contract.Kind = filter_input_kind::VECTOR_MAP;
   if (not filter_test_expect(scene.draw(output) IS ERR::NoSupport, "A vector slot accepted a native scalar field.")) {
      return false;
   }
   vector_inputs[0].Contract.Kind = filter_input_kind::SCALAR_MAP;
   for (int i = 0; i < 2; i++) {
      if (not filter_test_expect((scene.draw(output) IS ERR::Okay) and (output.OriginX IS -2) and
          (not scalar->Target) and (not vector->Target), "Cold or warm numerical draw failed.")) return false;
      if (not filter_test_pixel(output.Bitmap, 2, 1, { 128, 0, 0, 255 })) return false;
   }
   if (not filter_test_expect(scene.Filter->SurfaceBytes IS 960, "Numerical storage retention was undercounted.")) {
      return false;
   }
   size_t bytes;
   int stride;
   const auto oversized = filter_surface_size(8192, 5000, 2 * sizeof(float), bytes, stride);
   if (not filter_test_expect(oversized IS ERR::AllocMemory, "Oversized vector allocation passed the budget check.")) {
      return false;
   }
   return filter_test_expect((VECTORFILTER_Clear(scene.Filter) IS ERR::Okay) and (scene.Filter->SurfaceBytes IS 0),
      "Clear retained numerical surface storage.");
}

static constexpr filter_effect_extension glHeightTestProducer { {}, filter_surface_format::SCALAR_F32 };

static ERR write_test_height(extFilterEffect *Effect, auto HeightAt)
{
   filter_surface_view layout;
   float *data;
   if (auto error = get_numeric_target(Effect, layout, data); error != ERR::Okay) return error;
   const int stride = layout.Stride / sizeof(float);
   for (int y=0; y < layout.Height; ++y) {
      for (int x=0; x < layout.Width; ++x) data[y * stride + x] = HeightAt(x + layout.OriginX, y + layout.OriginY);
   }
   return ERR::Okay;
}

static constexpr float test_ramp_height(int X)
{
   return (512 + X) / 1024.0f;
}

static bool test_height_normal(const filter_surface_view &Source)
{
   // Read from three rows using the same loader as lighting. The fixture starts at a non-zero scene origin.
   const std::array<float, 10> shifted_samples { 0, 0.75f, 0, 0, 0, 0, 0.25f, INVALID_FILTER_SAMPLE, 0, 0 };
   const filter_surface_view shifted {
      .Data = (const uint8_t *)shifted_samples.data(), .Width = 3, .Height = 2, .Stride = 5 * sizeof(float),
      .OriginX = 12, .OriginY = -4, .Valid = { 1, 0, 3, 2 }, .Format = filter_surface_format::SCALAR_F32
   };
   std::array<float, 5> row;
   load_lighting_height_row(shifted, row.data(), 11, -3, 5);
   const std::array<float, 5> expected { 0, 0, 63.75f, 0, 0 };
   if (not filter_test_expect(row IS expected, "Scalar lighting ignored the stride, origin or valid samples.")) {
      return false;
   }

   constexpr int stride = 6;
   std::array<float, 3 * stride> heights;
   for (int y=0; y < 3; ++y) load_lighting_height_row(Source, heights.data() + y * stride, -2, y, stride);
   if (not filter_test_expect((heights[2] IS 255.0f * test_ramp_height(0)) and
       (heights[3] IS 255.0f * test_ramp_height(1)) and (heights[2] != heights[3]) and
       (int(heights[2]) IS int(heights[3])), "The loader lost two heights in one byte bin.")) return false;

   std::array<float, 3> dx, dy;
   for (int y=0; y < 3; ++y) {
      dx[y] = heights[y * stride + 3] - heights[y * stride + 1];
      dy[y] = heights[2 * stride + y + 1] - heights[y + 1];
   }
   float nx, ny, nz, occlusion;
   const map_height scale { 8.0 / 255.0, 8.0 / 255.0, 0.0 };
   row_slopes(heights.data() + stride + 2, stride, &dx[0], &dx[1], &dx[2], &dy[1], scale, 2, 0.25f, 0,
      &nx, &ny, &occlusion, 1);

   constexpr double slope = 2.0 * 8.0 / 1024.0;
   const double norm = 1.0 / std::hypot(slope, 1.0);
   if (not filter_test_expect((std::abs(nx - slope) < 1e-7) and (ny IS 0),
       "The sub-byte ramp gradient did not match the analytic slope.")) return false;
   row_normals(&nx, &ny, &nz, 1);
   return filter_test_expect((std::abs(nx + slope * norm) < 1e-7) and (ny IS 0) and
      (std::abs(nz - norm) < 1e-7), "The sub-byte ramp normal did not match the analytic normal.");
}

static bool test_filter_primary_height()
{
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   filter_test_scene scene;
   if ((not fixture_class.ok()) or (not scene.ready())) return false;
   auto producer = extFilterSurfaceTest::create::local(fl::Owner(scene.Filter->UID));
   if (not producer) return false;
   producer->FilterExtension = &glHeightTestProducer;
   producer->DrawRoutine = [](extFilterEffect *Effect) {
      return write_test_height(Effect, [](int X, int Y) { return test_ramp_height(X); });
   };
   filter_render_result output;
   if (not filter_test_expect(scene.draw(output) IS ERR::NoSupport, "A height became the final filter output.")) {
      return false;
   }

   auto blur = extBlurFX::create::local(fl::Owner(scene.Filter->UID), fl::Input(producer));
   auto light = extLightingFX::create::local(fl::Owner(scene.Filter->UID), fl::Input(blur));
   if ((not blur) or (not light)) return false;
   if (not filter_test_expect(scene.draw(output) IS ERR::NoSupport, "Image blur accepted a height as input.")) {
      return false;
   }

   blur->OutputMode = FOM::HEIGHT;
   if (scene.Scene->setCompliance(VSC::SVG2) != ERR::Okay) return false;
   if (not filter_test_expect(scene.draw(output) IS ERR::NoSupport, "Standard SVG lighting accepted a height.")) {
      return false;
   }
   light->Type = LT::COMBINED;
   light->Features = LFX::ALL;
   light->UnitX = light->UnitY = 2;
   light->UnitXProvided = light->UnitYProvided = true;
   auto distant = lt::SetDistantLight { 30, 40 };
   if (LIGHTINGFX_SetDistantLight(light, &distant) != ERR::Okay) return false;
   if (FILTEREFFECT_SET_Mix(light, producer) != ERR::Okay) return false;
   if (not filter_test_expect(scene.draw(output) IS ERR::NoSupport, "Lighting accepted a height as albedo.")) {
      return false;
   }
   // An image producer supplies the albedo, and the probe also reads the same height consumed by lighting.
   light->Mix = nullptr;
   light->MixType = VSF::GRAPHIC;
   filter_named_input inputs[] { { "Identity", { .Kind = filter_input_kind::SCALAR_MAP } } };
   const filter_effect_extension probe_extension { inputs };
   auto probe = extFilterSurfaceTest::create::local(fl::Owner(scene.Filter->UID));
   if (not probe) return false;
   probe->FilterExtension = &probe_extension;
   probe->DrawRoutine = [](extFilterEffect *Effect) {
      filter_map_view map;
      if (auto error = get_filter_map(Effect, "Identity", map); error != ERR::Okay) return error;
      return test_height_normal(map.Source) ? ERR::Okay : ERR::InvalidValue;
   };
   if (set_named_filter_input(probe, "Identity", VSF::REFERENCE, blur) != ERR::Okay) return false;
   if (not filter_test_expect(scene.draw(output) IS ERR::Okay,
       "Identity blur or lighting with both detail and curvature failed.")) return false;
   if (FreeResource(probe) != ERR::Okay) return false;

   // Alpha-level input at a byte-representable value must have identical positioned-light terms.
   light->Features = LFX::NIL;
   light->MixType = VSF::NONE;
   light->MapHeight = 7;
   auto point = lt::SetPointLight { 3, 2, 7 };
   auto spot = lt::SetSpotLight { 3, 2, 7, 4, 2, 0, 2, 45 };
   constexpr uint8_t alpha = 107;
   for (bool spotlight : { false, true }) {
      if ((spotlight ? LIGHTINGFX_SetSpotLight(light, &spot) : LIGHTINGFX_SetPointLight(light, &point)) !=
          ERR::Okay) return false;
      producer->FilterExtension = &glHeightTestProducer;
      producer->DrawRoutine = [](extFilterEffect *Effect) {
         return write_test_height(Effect, [](int X, int Y) { return alpha / 255.0f; });
      };
      if (scene.draw(output) != ERR::Okay) return false;
      const auto length = output.Bitmap->Size;
      const std::vector<uint8_t> scalar_pixels(output.Bitmap->Data, output.Bitmap->Data + length);
      producer->FilterExtension = nullptr;
      producer->DrawRoutine = [](extFilterEffect *Effect) {
         const auto bmp = Effect->Target;
         const auto channel = bmp->ColourFormat->AlphaPos >> 3;
         for (int y=0; y < bmp->Height; ++y) {
            auto row = bmp->Data + std::ptrdiff_t(y) * bmp->LineWidth;
            for (int x=0; x < bmp->Width; ++x) row[x * 4 + channel] = alpha;
         }
         return ERR::Okay;
      };
      blur->OutputMode = FOM::IMAGE; // The comparison must exercise the byte-height loader in LightingFX.
      if (not filter_test_expect((scene.draw(output) IS ERR::Okay) and (output.Bitmap->Size IS length) and
          std::equal(scalar_pixels.begin(), scalar_pixels.end(), output.Bitmap->Data),
          "A flat byte height disagreed with its scalar counterpart under a positioned light.")) return false;
      blur->OutputMode = FOM::HEIGHT;
   }
   return true;
}

// Direct weighted reference for a separable triangular blur. The fixture has a 4x2 input at scene (0,1).

static int height_reference_address(int Position, int Count, FEM Edge)
{
   if ((Position >= 0) and (Position < Count)) return Position;
   switch (Edge) {
      case FEM::TRANSPARENT: return -1;
      case FEM::CLAMP:       return std::clamp(Position, 0, Count - 1);
      case FEM::REPEAT:      return (Position % Count + Count) % Count;
      case FEM::MIRROR: {
         const int period = Count * 2;
         const int mapped = (Position % period + period) % period;
         return std::min(mapped, period - 1 - mapped);
      }
   }
   return -1;
}

static double height_reference_blur(auto Read, int Centre, double Radius, int Count, FEM Edge)
{
   if (Radius <= 0) return Read(height_reference_address(Centre, Count, FEM::TRANSPARENT));
   auto integral = [&](int R) {
      double sum = 0;
      for (int i=-R; i <= R; ++i) {
         sum += Read(height_reference_address(Centre + i, Count, Edge)) * (R + 1 - std::abs(i));
      }
      return sum / ((R + 1) * (R + 1));
   };
   const int r = int(Radius);
   return std::lerp(integral(r), integral(r + 1), Radius - r);
}

static bool test_filter_height_blur()
{
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   filter_test_scene scene;
   if ((not fixture_class.ok()) or (not scene.ready())) return false;
   const auto owner = fl::Owner(scene.Filter->UID);
   auto producer = extFilterSurfaceTest::create::local(owner, fl::X(0), fl::Y(1), fl::Width(4), fl::Height(2));
   auto blur = extBlurFX::create::local(owner, fl::Input(producer), fl::X(-2), fl::Y(0), fl::Width(10), fl::Height(4));
   auto probe = extFilterSurfaceTest::create::local(owner);
   if ((not producer) or (not blur) or (not probe)) return false;

   bool impulse = false;
   auto height_at = [&](int X, int Y) -> float {
      return impulse ? ((X IS 2) and (Y IS 1) ? 0.875f : 0.0f) : test_ramp_height(X + Y);
   };
   producer->FilterExtension = &glHeightTestProducer;
   producer->DrawRoutine = [&](extFilterEffect *Effect) { return write_test_height(Effect, height_at); };
   blur->OutputMode = FOM::HEIGHT;
   filter_named_input inputs[] { { "Blur", { .Kind = filter_input_kind::SCALAR_MAP } } };
   const filter_effect_extension probe_extension { inputs };
   probe->FilterExtension = &probe_extension;
   if (set_named_filter_input(probe, "Blur", VSF::REFERENCE, blur) != ERR::Okay) return false;

   probe->DrawRoutine = [&](extFilterEffect *Effect) {
      filter_map_view map;
      if (auto error = get_filter_map(Effect, "Blur", map); error != ERR::Okay) return error;
      for (int y=0; y < 4; ++y) {
         for (int x=-2; x < 8; ++x) {
            const double expected = height_reference_blur([&](int Y) -> double {
               if (Y < 0) return 0.0;
               return height_reference_blur([&](int X) -> double {
                  return X < 0 ? 0.0 : height_at(X, Y + 1);
               }, x, blur->SX * 2, 4, blur->EdgeMode);
            }, y - 1, blur->SY * 2, 2, blur->EdgeMode);
            const float actual = map.sample(x + 0.5, y + 0.5)[0];
            if (std::abs(actual - expected) >= 1e-5) {
               kt::Log("FilterTest").warning("Scalar blur mode %d at %d,%d (sx=%g sy=%g): %.9g expected %.9g",
                  int(blur->EdgeMode), x, y, blur->SX, blur->SY, actual, expected);
               return ERR::InvalidValue;
            }
         }
      }
      return ERR::Okay;
   };
   filter_render_result output;
   for (bool use_impulse : { false, true }) {
      impulse = use_impulse;
      for (auto mode : { FEM::TRANSPARENT, FEM::CLAMP, FEM::REPEAT, FEM::MIRROR }) {
         blur->EdgeMode = mode;
         for (const auto &[sx, sy] : { std::pair{0.0, 0.0}, {0.625, 0.0}, {0.0, 0.375}, {0.625, 1.25} }) {
            blur->SX = sx;
            blur->SY = sy;
            if (scene.draw(output) != ERR::Okay) return false;
         }
      }
   }
   return true;
}

// Evaluate each lattice corner directly in double precision, sharing the seeded gradient table with the producer.

static double reference_height_noise(const extTurbulenceFX &Noise, double X, double Y, int PeriodX, int PeriodY,
   int EndX, int EndY)
{
   const int floor_x = int(std::floor(X)), floor_y = int(std::floor(Y));
   const double dx = X - floor_x, dy = Y - floor_y;
   double result = 0;
   for (int j=0; j < 2; ++j) {
      const int y = floor_y + j;
      const int iy = ((PeriodY and (y >= EndY) ? y - PeriodY : y) + 4096) & 255;
      const double weight_y = dy * dy * (3 - 2 * dy);
      for (int i=0; i < 2; ++i) {
         const int x = floor_x + i;
         const int ix = ((PeriodX and (x >= EndX) ? x - PeriodX : x) + 4096) & 255;
         const auto &g = Noise.Gradient[3][Noise.Lattice[Noise.Lattice[ix] + iy]];
         const double weight_x = dx * dx * (3 - 2 * dx);
         const double dot = g[0] * (dx - i) + g[1] * (dy - j);
         result += dot * (i ? weight_x : 1 - weight_x) * (j ? weight_y : 1 - weight_y);
      }
   }
   return result;
}

static double reference_turbulence(const extTurbulenceFX &Noise, double X, double Y)
{
   double sum = 0;
   for (int o=0; o < Noise.Octaves; ++o) {
      // The authored tile spans [-4,60) x [1,33). In stitched mode use integral periods of 2 and 1.
      const int scale = 1 << o;
      const int period_x = Noise.Stitch ? 2 * scale : 0, period_y = Noise.Stitch ? scale : 0;
      const double fx = Noise.Stitch ? 2.0 / 64 : Noise.FX;
      const double fy = Noise.Stitch ? 1.0 / 32 : Noise.FY;
      const int end_x = scale; // floor(-4 * 2/64 + 2) at the first octave, then doubled
      const int end_y = scale; // floor(1 * 1/32 + 1) at the first octave, then doubled
      const double noise = reference_height_noise(Noise, X * fx * scale, Y * fy * scale, period_x, period_y,
         end_x, end_y);
      sum += ((Noise.Type IS TB::NOISE) ? noise : std::abs(noise)) / scale;
   }
   return std::clamp((Noise.Type IS TB::NOISE) ? (1 + sum) / 2 : sum, 0.0, 1.0);
}

static bool test_filter_height_turbulence()
{
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   filter_test_scene scene(80, 40);
   if ((not fixture_class.ok()) or (not scene.ready())) return false;
   const auto owner = fl::Owner(scene.Filter->UID);
   auto noise = extTurbulenceFX::create::local(owner, fl::X(-4), fl::Y(1), fl::Width(64), fl::Height(32),
      FieldValue("Seed", 7));
   auto probe = extFilterSurfaceTest::create::local(owner);
   if ((not noise) or (not probe)) return false;
   filter_named_input inputs[] { { "Noise", { .Kind = filter_input_kind::SCALAR_MAP } } };
   const filter_effect_extension probe_extension { inputs };
   probe->FilterExtension = &probe_extension;
   if (set_named_filter_input(probe, "Noise", VSF::REFERENCE, noise) != ERR::Okay) return false;
   TURBULENCEFX_SET_FX(noise, 0.028);
   TURBULENCEFX_SET_FY(noise, 0.029);
   TURBULENCEFX_SET_Octaves(noise, 3);

   probe->DrawRoutine = [&](extFilterEffect *Effect) {
      filter_map_view map;
      if (auto error = get_filter_map(Effect, "Noise", map); error != ERR::Okay) return error;
      const bool scalar = noise->OutputMode IS FOM::HEIGHT;
      const auto &view = map.Source;
      std::vector<float> heights;
      double scalar_error = 0, byte_error = 0;
      for (int y=1; y < 33; ++y) {
         if ((y < view.OriginY) or (y >= view.OriginY + view.Height)) continue;
         for (int x=view.OriginX; x < view.OriginX + view.Width; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            const float actual = map.sample(px, py)[0];
            const auto reference = [&](int Offset) {
               return reference_turbulence(*noise, px + Offset, py);
            };
            const double expected = ((x < -4) or (x >= 60)) ? 0.0 : reference(0);
            const double tolerance = scalar ? 1e-7 : 1.0 / 255.0 + 1e-7;
            if (std::abs(actual - expected) > tolerance) {
               kt::Log("FilterTest").warning("Turbulence at %d,%d was %.9g, expected %.9g.", x, y, actual, expected);
               return ERR::InvalidValue;
            }
            if (scalar and (x > std::max(-4, view.OriginX)) and (x < std::min(60, view.OriginX + view.Width) - 1)) {
               heights.push_back(actual);
               const double slope = (reference(1) - reference(-1)) * 0.5;
               const double lo = map.sample(px - 1, py)[0], hi = map.sample(px + 1, py)[0];
               const double float_delta = (hi - lo) * 0.5 - slope;
               const double byte_delta = (int(hi * 255) - int(lo * 255)) / 510.0 - slope;
               scalar_error += float_delta * float_delta;
               byte_error += byte_delta * byte_delta;
            }
         }
      }
      if (scalar) {
         const double count = heights.size();
         std::sort(heights.begin(), heights.end());
         if (not filter_test_expect((std::unique(heights.begin(), heights.end()) - heights.begin()) > 256,
             "Turbulence had at most 256 distinct heights.")) return ERR::InvalidValue;
         if (not filter_test_expect(scalar_error < byte_error * 1e-6,
             "Native turbulence failed to reduce interior slope error.")) return ERR::InvalidValue;
         kt::Log("FilterTest").msg("Height slope RMS: scalar %.6g, byte %.6g.",
            std::sqrt(scalar_error / count), std::sqrt(byte_error / count));
      }
      return ERR::Okay;
   };

   filter_render_result output;
   if (scene.draw(output) != ERR::Okay) return false;
   if (not filter_test_expect(not noise->Dirty, "The first image draw did not populate the cache.")) return false;
   noise->OutputMode = FOM::HEIGHT;
   if ((scene.draw(output) != ERR::Okay) or (scene.draw(output) != ERR::Okay)) return false;
   noise->OutputMode = FOM::IMAGE;
   if (scene.draw(output) != ERR::Okay) return false;

   noise->OutputMode = FOM::HEIGHT;
   TURBULENCEFX_SET_FX(noise, 0.066);
   if (scene.draw(output) != ERR::Okay) return false;
   if (not filter_test_expect(noise->Dirty, "The height draw cleared the image cache's dirty flag.")) return false;
   noise->OutputMode = FOM::IMAGE;
   if (scene.draw(output) != ERR::Okay) return false;

   // Shift and resize the clip around the same tile. Neither the tile origin nor its period moves.
   if ((scene.Filter->setX(5) != ERR::Okay) or (scene.Filter->setWidth(53) != ERR::Okay)) return false;
   noise->OutputMode = FOM::HEIGHT;
   TURBULENCEFX_SET_FX(noise, 0.029);
   TURBULENCEFX_SET_Stitch(noise, true);
   for (auto mode : { TB::TURBULENCE, TB::NOISE }) {
      TURBULENCEFX_SET_Type(noise, mode);
      if (scene.draw(output) != ERR::Okay) return false;
      TURBULENCEFX_SET_Stitch(noise, false);
      if (scene.draw(output) != ERR::Okay) return false;
      TURBULENCEFX_SET_Stitch(noise, true);
   }
   return true;
}

static bool test_filter_blurred_alpha_edge()
{
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   constexpr int width = 4, height = 192, edge = height / 2, alpha = 191;
   constexpr double radius = 38.5;
   filter_test_scene scene(width, height);
   if ((not fixture_class.ok()) or (not scene.ready())) return false;
   const auto owner = fl::Owner(scene.Filter->UID);
   auto mask = extFilterSurfaceTest::create::local(owner);
   auto blur = extBlurFX::create::local(owner, fl::Input(mask), FieldValue("SY", radius / 2.0));
   auto probe = extFilterSurfaceTest::create::local(owner);
   if ((not mask) or (not blur) or (not probe)) return false;
   mask->DrawRoutine = [](extFilterEffect *Effect) {
      const auto bmp = Effect->Target;
      const auto channel = bmp->ColourFormat->AlphaPos >> 3;
      for (int y=0; y < bmp->Height; ++y) {
         auto row = bmp->Data + std::ptrdiff_t(y) * bmp->LineWidth;
         for (int x=0; x < bmp->Width; ++x) row[x * 4 + channel] = (y >= edge) ? alpha : 0;
      }
      return ERR::Okay;
   };

   filter_named_input input[] { { "Edge", { .Kind = filter_input_kind::SCALAR_MAP } } };
   const filter_effect_extension probe_extension { input };
   probe->FilterExtension = &probe_extension;
   if (set_named_filter_input(probe, "Edge", VSF::REFERENCE, blur) != ERR::Okay) return false;

   double rms = 0;
   probe->DrawRoutine = [&](extFilterEffect *Effect) {
      filter_map_view map;
      if (auto error = get_filter_map(Effect, "Edge", map); error != ERR::Okay) return error;
      auto reference = [&](int Y) {
         return height_reference_blur([](int Sample) { return Sample >= edge ? alpha / 255.0 : 0.0; },
            Y, radius, height, FEM::TRANSPARENT);
      };
      double sum = 0;
      constexpr int span = 80;
      for (int y=edge - span / 2; y < edge + span / 2; ++y) {
         const double lo = map.sample(1.5, y - 0.5)[0], hi = map.sample(1.5, y + 1.5)[0];
         const double difference = (hi - lo - reference(y + 1) + reference(y - 1)) * 0.5;
         sum += difference * difference;
      }
      rms = std::sqrt(sum / span);
      return ERR::Okay;
   };
   filter_render_result output;
   if (scene.draw(output) != ERR::Okay) return false;
   const double byte_rms = rms;
   blur->OutputMode = FOM::HEIGHT;
   if (scene.draw(output) != ERR::Okay) return false;
   kt::Log("FilterTest").msg("Blurred alpha edge slope RMS: scalar %.6g, byte %.6g.", rms, byte_rms);
   return filter_test_expect((rms < 1e-7) and (rms < byte_rms), "Float blur lost the alpha edge's fractional slopes.");
}

void vector_filter_unit_tests(std::string_view Options, int &Passed, int &Total)
{
   if (Options IS "filter:named") {
      Total++;
      if (test_filter_named_input()) Passed++;
   }
   else if (Options IS "filter:numerical") {
      Total++;
      if (test_filter_numerical_surfaces()) Passed++;
   }
   else if (Options IS "filter:height") {
      Total += 4;
      if (test_filter_primary_height()) Passed++;
      if (test_filter_height_blur()) Passed++;
      if (test_filter_height_turbulence()) Passed++;
      if (test_filter_blurred_alpha_edge()) Passed++;
   }
}
