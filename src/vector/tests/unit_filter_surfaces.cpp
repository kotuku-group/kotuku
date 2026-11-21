// Included by filter.cpp to exercise the private planner, renderer and surface pool through real filter objects.

struct filter_test_scene {
   extVectorScene::create Scene { { fl::PageWidth(8), fl::PageHeight(4) } };
   extVectorViewport *Viewport = nullptr;
   extVectorRectangle *Client = nullptr;
   extVectorFilter *Filter = nullptr;

   filter_test_scene() {
      if (not Scene.ok()) return;
      Viewport = extVectorViewport::create::local(fl::Owner(Scene->UID), fl::Width(8), fl::Height(4));
      if (not Viewport) return;
      Client = extVectorRectangle::create::local(fl::Owner(Viewport->UID), fl::Width(8), fl::Height(4));
      if (not Client) return;
      gen_vector_path(Viewport);
      gen_vector_path(Client);
      Filter = extVectorFilter::create::local(fl::Owner(Scene->UID), fl::Units(VUNIT::USERSPACE),
         fl::X(-2), fl::Y(0), fl::Width(10), fl::Height(4));
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

   ERR (*DrawRoutine)(extFilterEffect *) = nullptr;

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
}
