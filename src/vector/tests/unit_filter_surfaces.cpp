// Included by filter.cpp to exercise the private planner, renderer and surface pool through real filter objects.

#include <map>
#include <set>
#include <tuple>
#include "../filters/paving_pattern.h"

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
   auto tile = extTileFX::create::local(owner);
   if (not tile) return false;
   tile->Input = tile; // Bypass the setter to check validation of the native fields during format inference.
   if (not filter_test_expect(scene.draw(output) IS ERR::Recursion, "A tile accepted a self-reference.")) return false;
   if (FreeResource(tile) != ERR::Okay) return false;

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

static bool test_filter_displacement_vector_map()
{
   constexpr filter_effect_extension vector_operation { {}, filter_surface_format::VECTOR_F32 };
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   filter_test_scene scene;
   if ((not fixture_class.ok()) or (not scene.ready())) return false;

   const auto owner = fl::Owner(scene.Filter->UID);
   auto image = extFloodFX::create::local(owner, fl::X(3), fl::Y(2), fl::Width(1), fl::Height(1));
   auto mix = extFloodFX::create::local(owner);
   auto field = extFilterSurfaceTest::create::local(owner);
   auto effect = extDisplacementFX::create::local(owner, fl::Input(image));
   if ((not image) or (not mix) or (not field) or (not effect)) return false;
   FRGB red { 1, 0, 0, 1 };
   if ((FLOODFX_SET_Colour(image, &red) != ERR::Okay) or (FILTEREFFECT_SET_Mix(effect, mix) != ERR::Okay)) {
      return false;
   }

   field->FilterExtension = &vector_operation;
   field->DrawRoutine = [](extFilterEffect *Effect) {
      filter_surface_view layout;
      float *data;
      if (auto error = get_numeric_target(Effect, layout, data); error != ERR::Okay) return error;
      auto row = (float *)((uint8_t *)data + layout.Stride);
      const int x = 4 - layout.OriginX;
      row[x * 2] = -0.5f;
      row[x * 2 + 1] = 0.5f;
      return ERR::Okay;
   };
   if (DISPLACEMENTFX_SET_VectorMap(effect, field) != ERR::Okay) return false;
   effect->Scale = 2;
   effect->NeutralX = effect->NeutralY = 1; // Only the dormant colour map uses these.

   filter_render_result output;
   if (not filter_test_expect(scene.draw(output) IS ERR::Okay, "Vector displacement failed to draw.")) return false;
   if ((not filter_test_pixel(output.Bitmap, 4 - output.OriginX, 1, { 255, 0, 0, 255 })) or
       (not filter_test_pixel(output.Bitmap, 3 - output.OriginX, 2, { 255, 0, 0, 255 }))) {
      return filter_test_expect(false, "Signed VectorMap or invalid-sample fallback gave the wrong displacement.");
   }

   if (not filter_test_expect((DISPLACEMENTFX_SET_VectorMap(effect, image) IS ERR::NoSupport) and
       (effect->VectorMap.Reference IS field), "An RGBA8 producer replaced the native VectorMap.")) return false;
   return DISPLACEMENTFX_SET_VectorMap(effect, nullptr) IS ERR::Okay and scene.draw(output) IS ERR::Okay and
      filter_test_pixel(output.Bitmap, 4 - output.OriginX, 1, { 0, 0, 0, 0 }) and
      filter_test_pixel(output.Bitmap, 3 - output.OriginX, 2, { 255, 0, 0, 255 });
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

static bool test_filter_native_blur_range()
{
   const std::array<float, 12> samples { 1, 0x1p60f, 1, 1, 1, 1, 1, 1, -0x1p120f, 2, 2, 2 };
   const int count = int(samples.size());
   const auto sample = [&](int Index) { return (Index < 0) ? 0.0f : samples[Index]; };

   for (const auto mode : { FEM::TRANSPARENT, FEM::CLAMP, FEM::REPEAT, FEM::MIRROR }) {
      for (const double radius : { 0.5, 1.0, 1.25, 2.0, double(MAX_BLUR_RADIUS) }) {
         for (const bool vertical : { false, true }) {
            std::array<float, samples.size() * 2> result;
            const int width = vertical ? 1 : count, height = vertical ? count : 1;
            blur_numeric_axis([&](int X, int Y) { return sample(vertical ? Y : X); },
               result.data() + 1, width, height, width * 2, 2, vertical, radius, { 0, count, mode }, false);
            for (int i=0; i < count; ++i) {
               const auto expected = float(height_reference_blur([&](int Index) { return double(sample(Index)); },
                  i, radius, count, mode));
               const float actual = result[i * 2 + 1];
               if (not (std::abs(actual - expected) <= std::max(1.0f, std::abs(expected)) * 1e-6f)) {
                  kt::Log("FilterTest").warning("Native blur mode %d at sample %d (radius %g): %.9g expected %.9g",
                     int(mode), i, radius, actual, expected);
                  return false;
               }
            }
         }
      }
   }
   return true;
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

static bool test_filter_numeric_neighbourhoods(filter_surface_format Format)
{
   const filter_effect_extension producer_extension { {}, Format };
   filter_test_scene scene;
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   if ((not fixture_class.ok()) or (not scene.ready())) return false;

   const auto owner = fl::Owner(scene.Filter->UID);
   auto producer = extFilterSurfaceTest::create::local(owner);
   auto blur = extBlurFX::create::local(owner, fl::Input(producer),
      FieldValue("OutputMode", int(FOM::NATIVE)));
   constexpr double identity[] { 1 };
   auto convolve = extConvolveFX::create::local(owner, fl::Input(blur), fl::MatrixColumns(1), fl::MatrixRows(1),
      FieldValue(kt::fieldhash("Matrix"), std::span<const double>(identity)));
   auto morph = extMorphologyFX::create::local(owner, fl::Input(producer),
      FieldValue("KernelShape", int(MKS::ELLIPSE)));
   auto probe = extFilterSurfaceTest::create::local(owner);
   if ((not producer) or (not blur) or (not convolve) or (not morph) or (not probe)) return false;

   constexpr double kernel[] { 1, -1 };
   if (set_convolution_kernel(convolve, 2, 1, kernel, 2, 0, 0) != ERR::Okay) return false;
   convolve->Bias = -0.125;
   blur->SX = blur->SY = 0.5;
   morph->RadiusX = morph->RadiusY = 2;

   const int components = filter_components(Format);
   producer->FilterExtension = &producer_extension;
   producer->DrawRoutine = [=](extFilterEffect *Effect) {
      filter_surface_view layout;
      float *data;
      if (auto error = get_numeric_target(Effect, layout, data); error != ERR::Okay) return error;
      const auto &clip = layout.Valid;
      for (int y=clip.top; y < clip.bottom; ++y) {
         auto row = (float *)((uint8_t *)data + std::ptrdiff_t(y) * layout.Stride);
         for (int x=clip.left; x < clip.right; ++x) {
            const bool impulse = (x + layout.OriginX IS 3) and (y + layout.OriginY IS 2);
            row[x * components] = impulse ? -8.0f : 0.0f;
            if (components IS 2) row[x * components + 1] = impulse ? 4.25f : 0.0f;
         }
      }
      return ERR::Okay;
   };

   const filter_input_contract native { .Kind = filter_input_kind_of(Format), .NativeOnly = true };
   filter_named_input inputs[] { { "Blur", native }, { "Convolution", native }, { "Morphology", native } };
   const filter_effect_extension probe_extension { inputs };
   probe->FilterExtension = &probe_extension;
   if ((set_named_filter_input(probe, "Blur", VSF::REFERENCE, blur) != ERR::Okay) or
       (set_named_filter_input(probe, "Convolution", VSF::REFERENCE, convolve) != ERR::Okay) or
       (set_named_filter_input(probe, "Morphology", VSF::REFERENCE, morph) != ERR::Okay)) return false;

   bool fractional = false;
   probe->DrawRoutine = [&](extFilterEffect *Effect) {
      filter_map_view blurred, convolved, shaped;
      if (auto error = get_filter_map(Effect, "Blur", blurred); error != ERR::Okay) return error;
      if (auto error = get_filter_map(Effect, "Convolution", convolved); error != ERR::Okay) return error;
      if (auto error = get_filter_map(Effect, "Morphology", shaped); error != ERR::Okay) return error;
      const auto centre = blurred.sample(3.5, 2.5), diagonal = blurred.sample(4.5, 3.5);
      const auto derivative = convolved.sample(2.5, 2.5);
      const auto expected = fractional ? filter_map_value { -0.375f, 0.0078125f } :
         filter_map_value { -0.625f, 0.140625f };

      if (not filter_test_expect((centre[0] IS -2.0f) and (diagonal[0] IS -0.5f) and
          ((components IS 1) or ((centre[1] IS 1.0625f) and (diagonal[1] IS 0.265625f))),
          "Blur changed a signed float or mixed components.")) return ERR::InvalidValue;
      if (not filter_test_expect((derivative[0] IS expected[0]) and
          ((components IS 1) or (derivative[1] IS expected[1])),
          "Convolution failed to preserve the signed derivative, divisor, bias or fractional tap.")) {
         return ERR::InvalidValue;
      }
      const float inner = (morph->KernelShape IS MKS::CROSS) ? 0.0f : -8.0f;
      const float outer = (morph->KernelShape IS MKS::RECTANGLE) ? -8.0f : 0.0f;
      if (not filter_test_expect((shaped.sample(4.5, 3.5)[0] IS inner) and (shaped.sample(5.5, 3.5)[0] IS outer),
          "Float erosion failed to preserve the signed extreme or kernel footprint.")) return ERR::InvalidValue;
      return ERR::Okay;
   };

   filter_render_result output;
   if (scene.draw(output) != ERR::Okay) return false;
   fractional = true;
   if (CONVOLVEFX_SET_UnitX(convolve, 0.5) != ERR::Okay) return false;
   for (const auto shape : { MKS::ELLIPSE, MKS::RECTANGLE, MKS::CROSS }) {
      if (MORPHOLOGYFX_SET_KernelShape(morph, shape) != ERR::Okay) return false;
      if (scene.draw(output) != ERR::Okay) return false;
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

// Draws a HEIGHT CellularFX through a probe and compares every processed sample with cellular_noise.h, evaluated at
// a lattice position that Lattice derives independently from the pixel centre in client coordinates.  Pixels outside
// Covered must be zero.  The filter region is (-2, 0) to (80, 40) in client coordinates.

struct cellular_fixture {
   CSTRING Name;
   cellular_params Params;
   Unit CellWidth, CellHeight, OriginX, OriginY;
   double Angle = 0;
   VUNIT PrimitiveUnits = VUNIT::USERSPACE;
   TClipRectangle<double> Subregion = { 0.0 }; // Client coordinates; empty for the default subregion
   int PageWidth = 80, PageHeight = 40;
   std::function<void(double, double, double &, double &)> Lattice;
   std::function<bool(double, double)> Covered;
};

static bool test_filter_cellular_case(const cellular_fixture &Case)
{
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   filter_test_scene scene(80, 40);
   if ((not fixture_class.ok()) or (not scene.ready())) return false;
   scene.Filter->PrimitiveUnits = Case.PrimitiveUnits;
   scene.Scene->PageWidth = Case.PageWidth;
   scene.Scene->PageHeight = Case.PageHeight;

   const auto owner = fl::Owner(scene.Filter->UID);
   auto noise = Case.Subregion.valid() and (Case.Subregion.width() > 0) ?
      extCellularFX::create::local(owner, fl::X(Case.Subregion.left), fl::Y(Case.Subregion.top),
         fl::Width(Case.Subregion.width()), fl::Height(Case.Subregion.height())) :
      extCellularFX::create::local(owner);
   auto probe = extFilterSurfaceTest::create::local(owner);
   if ((not noise) or (not probe)) return false;

   const auto &p = Case.Params;
   auto cell_width = Case.CellWidth, cell_height = Case.CellHeight, origin_x = Case.OriginX, origin_y = Case.OriginY;
   if ((CELLULARFX_SET_Seed(noise, p.Seed) != ERR::Okay) or
       (CELLULARFX_SET_WorleyMode(noise, WLF(int(p.Mode))) != ERR::Okay) or
       (CELLULARFX_SET_WorleyMetric(noise, p.Metric) != ERR::Okay) or
       (CELLULARFX_SET_Jitter(noise, p.Jitter) != ERR::Okay) or
       (CELLULARFX_SET_HeightMin(noise, p.HeightMin) != ERR::Okay) or
       (CELLULARFX_SET_HeightMax(noise, p.HeightMax) != ERR::Okay) or
       (CELLULARFX_SET_Octaves(noise, p.Octaves) != ERR::Okay) or
       (CELLULARFX_SET_Persistence(noise, p.Persistence) != ERR::Okay) or
       (CELLULARFX_SET_Lacunarity(noise, p.Lacunarity) != ERR::Okay) or
       (CELLULARFX_SET_PeriodX(noise, p.PeriodX) != ERR::Okay) or
       (CELLULARFX_SET_PeriodY(noise, p.PeriodY) != ERR::Okay) or
       (CELLULARFX_SET_CellWidth(noise, cell_width) != ERR::Okay) or
       (CELLULARFX_SET_CellHeight(noise, cell_height) != ERR::Okay) or
       (CELLULARFX_SET_OriginX(noise, origin_x) != ERR::Okay) or
       (CELLULARFX_SET_OriginY(noise, origin_y) != ERR::Okay) or
       (CELLULARFX_SET_Angle(noise, Case.Angle) != ERR::Okay) or
       (CELLULARFX_SET_OutputMode(noise, FOM::HEIGHT) != ERR::Okay)) {
      kt::Log("FilterTest").warning("%s: a CellularFX field was rejected.", Case.Name);
      return false;
   }

   filter_named_input inputs[] { { "Noise", { .Kind = filter_input_kind::SCALAR_MAP } } };
   const filter_effect_extension probe_extension { inputs };
   probe->FilterExtension = &probe_extension;
   if (set_named_filter_input(probe, "Noise", VSF::REFERENCE, noise) != ERR::Okay) return false;

   const cellular_sampler reference(p);
   int covered = 0;
   probe->DrawRoutine = [&](extFilterEffect *Effect) {
      filter_map_view map;
      if (auto error = get_filter_map(Effect, "Noise", map); error != ERR::Okay) return error;
      const auto &view = map.Source;
      for (int y=view.OriginY; y < view.OriginY + view.Height; ++y) {
         for (int x=view.OriginX; x < view.OriginX + view.Width; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            double expected = 0;
            if (Case.Covered(px, py)) {
               double lx, ly;
               Case.Lattice(px, py, lx, ly);
               expected = reference.sample(lx, ly);
               covered++;
            }
            const float actual = map.sample(px, py)[0];
            if (std::abs(actual - expected) > 1e-6 * std::max(1.0, std::abs(expected))) {
               kt::Log("FilterTest").warning("%s: sample at %d,%d was %.9g, expected %.9g.", Case.Name, x, y,
                  actual, expected);
               return ERR::InvalidValue;
            }
         }
      }
      return ERR::Okay;
   };

   filter_render_result output;
   if (auto error = scene.draw(output); error != ERR::Okay) {
      kt::Log("FilterTest").warning("%s: draw failed: %s", Case.Name, GetErrorMsg(error));
      return false;
   }
   return filter_test_expect(covered >= 100, "The CellularFX fixture compared too few covered samples.");
}

static bool test_filter_cellular()
{
   // The filter region spans x = [-2, 80) and y = [0, 40) in client units, which equal scene pixels here.

   constexpr double region_left = -2, region_top = 0, region_width = 82, region_height = 40;

   cellular_fixture offset {
      .Name = "Offset subregion",
      .Params = { .Mode = cellular_mode::WEIGHTED, .Seed = -77, .Jitter = 0.8, .HeightMin = 1.5, .HeightMax = -0.5,
         .Octaves = 2, .Persistence = 0.6, .Lacunarity = 2.5 },
      .CellWidth = Unit(10.0), .CellHeight = Unit(0.125, FD_SCALED), .OriginX = Unit(3.0), .OriginY = Unit(-2.0),
      .Subregion = { 6, 3, 56, 33 },
      .Lattice = [&](double X, double Y, double &LX, double &LY) {
         LX = (X - (region_left + 3.0)) / 10.0;
         LY = (Y - (region_top - 2.0)) / (region_height * 0.125);
      },
      .Covered = [](double X, double Y) { return (X >= 6) and (X < 56) and (Y >= 3) and (Y < 33); }
   };

   cellular_fixture rotated {
      .Name = "Rotated domain",
      .Params = { .Mode = cellular_mode::F2_F1, .Metric = WLM::MANHATTAN, .Seed = 12345, .Octaves = 3,
         .Lacunarity = 2.0 },
      .CellWidth = Unit(7.0), .CellHeight = Unit(9.0), .OriginX = Unit(0.25, FD_SCALED), .OriginY = Unit(10.0),
      .Angle = 30,
      .Lattice = [&](double X, double Y, double &LX, double &LY) {
         const double du = X - (region_left + 0.25 * region_width), dv = Y - (region_top + 10.0);
         const double c = std::cos(30.0 * agg::pi / 180.0), s = std::sin(30.0 * agg::pi / 180.0);
         LX = ((c * du) + (s * dv)) / 7.0;
         LY = ((c * dv) - (s * du)) / 9.0;
      },
      .Covered = [&](double X, double Y) {
         return (X >= region_left) and (X < region_left + region_width) and (Y >= region_top) and
            (Y < region_top + region_height);
      }
   };

   // In BOUNDING_BOX units, primitive coordinates are fractions of the client bounds, (0, 0) to (80, 40).

   cellular_fixture bounding_box {
      .Name = "Bounding box units",
      .Params = { .Mode = cellular_mode::PEAKS, .Metric = WLM::CHEBYSHEV, .Seed = 9, .Jitter = 0.5, .HeightMin = -1,
         .HeightMax = 2, .Octaves = 2, .Persistence = 0.75, .Lacunarity = 3.0, .PeriodX = 4, .PeriodY = 3 },
      .CellWidth = Unit(0.1), .CellHeight = Unit(0.25, FD_SCALED), .OriginX = Unit(0.05), .OriginY = Unit(0.0),
      .PrimitiveUnits = VUNIT::BOUNDING_BOX,
      .Lattice = [&](double X, double Y, double &LX, double &LY) {
         const double domain_left = region_left / 80.0, domain_height = region_height / 40.0;
         LX = (X / 80.0 - (domain_left + 0.05)) / 0.1;
         LY = (Y / 40.0) / (0.25 * domain_height);
      },
      .Covered = rotated.Covered
   };

   // The page limits processing to part of the subregion.  Samples must not depend on the processing area.

   auto clipped = offset;
   clipped.Name = "Clipped redraw";
   clipped.PageWidth = 37;
   clipped.PageHeight = 23;

   return test_filter_cellular_case(offset) and test_filter_cellular_case(rotated) and
      test_filter_cellular_case(bounding_box) and test_filter_cellular_case(clipped);
}

//********************************************************************************************************************
// PavingFX pattern evaluator (paving_pattern.h).

template <class... Args> static bool paving_fail(CSTRING Format, Args... Values)
{
   kt::Log("FilterTest").warning(Format, Values...);
   return false;
}

static paving_params paving_fixture(paving_layout Layout, double Width, double Height, double RowOffset = 0.5)
{
   return { .Layout = Layout, .ElementWidth = Width, .ElementHeight = Height, .RowOffset = RowOffset };
}

using paving_key = std::tuple<int64_t, int64_t, int64_t>;

struct paving_anchor {
   double X, Y;
   bool Vertical;
};

// The pattern position of an element's top-left corner, derived from a sample within it.

static paving_anchor paving_sample_anchor(const paving_sample &Sample, double QX, double QY)
{
   if (Sample.Vertical) return { QX - Sample.LY, QY - Sample.LX, true };
   else return { QX - Sample.LX, QY - Sample.LY, false };
}

// Every sampled position must lie within the bounds of exactly one element, every element must have one anchor and
// orientation, and the interior of every element must map back to it.

static bool test_paving_partition(CSTRING Name, const paving_params &Params)
{
   if (paving_check(Params) != ERR::Okay) return paving_fail("%s: the parameters were rejected.", Name);

   const paving_sampler sampler(Params);
   const double length = sampler.length(), thickness = sampler.thickness();
   const double span = 4.0 * length * ((Params.Layout IS paving_layout::HERRINGBONE) ? 2.0 : 1.0);
   const double tolerance = 1e-9 * span;
   std::map<paving_key, paving_anchor> elements;
   std::map<std::tuple<int64_t, int64_t, bool>, paving_key> anchors;
   int horizontal = 0, vertical = 0;

   auto visit = [&](double QX, double QY) -> bool {
      const auto s = sampler.locate(QX, QY);
      if ((s.LX < 0) or (s.LX >= length) or (s.LY < 0) or (s.LY >= thickness) or (s.U < 0) or (s.U >= 1.0) or
          (s.V < 0) or (s.V >= 1.0)) {
         return paving_fail("%s: local position %.9g,%.9g (u,v %.9g,%.9g) at %.9g,%.9g is out of bounds.", Name,
            s.LX, s.LY, s.U, s.V, QX, QY);
      }

      if ((std::abs(s.U * length - s.LX) > tolerance) or (std::abs(s.V * thickness - s.LY) > tolerance)) {
         return paving_fail("%s: (u, v) does not match the local position at %.9g,%.9g.", Name, QX, QY);
      }

      const paving_key key { s.KeyX, s.KeyY, s.KeyZ };
      const auto anchor = paving_sample_anchor(s, QX, QY);
      auto [it, inserted] = elements.try_emplace(key, anchor);
      if (inserted) {
         if (anchor.Vertical) vertical++;
         else horizontal++;
         const std::tuple<int64_t, int64_t, bool> rounded { std::llround(anchor.X * 4096.0),
            std::llround(anchor.Y * 4096.0), anchor.Vertical };
         if (not anchors.try_emplace(rounded, key).second) {
            return paving_fail("%s: two element keys share the anchor %.9g,%.9g.", Name, anchor.X, anchor.Y);
         }
      }
      else if ((std::abs(it->second.X - anchor.X) > tolerance) or (std::abs(it->second.Y - anchor.Y) > tolerance) or
               (it->second.Vertical != anchor.Vertical)) {
         return paving_fail("%s: element %" PRId64 ",%" PRId64 ",%" PRId64 " has inconsistent anchors %.9g,%.9g and "
            "%.9g,%.9g.", Name, s.KeyX, s.KeyY, s.KeyZ, it->second.X, it->second.Y, anchor.X, anchor.Y);
      }

      if ((Params.Layout IS paving_layout::CHECKER) and (s.Parity != int((s.KeyX + s.KeyY) & 1))) {
         return paving_fail("%s: checker parity is wrong at %.9g,%.9g.", Name, QX, QY);
      }
      return true;
   };

   // Irregular sample positions, then positions exactly on element boundaries.

   const double step = std::min(length, thickness) / 7.3;
   for (double qy = -span + 0.0137; qy < span; qy += step) {
      for (double qx = -span + 0.0291; qx < span; qx += step) {
         if (not visit(qx, qy)) return false;
      }
   }

   for (double qy = -span; qy < span; qy += thickness * 0.5) {
      for (double qx = -span; qx < span; qx += thickness * 0.5) {
         if (not visit(qx, qy)) return false;
      }
   }

   // The interior of each element must belong to it, which excludes overlapping elements.

   const double inset = 1e-6 * length;
   for (const auto &[key, anchor] : elements) {
      const double width = anchor.Vertical ? thickness : length;
      const double height = anchor.Vertical ? length : thickness;
      const double probes[5][2] = {
         { anchor.X + width * 0.5, anchor.Y + height * 0.5 }, { anchor.X + inset, anchor.Y + inset },
         { anchor.X + width - inset, anchor.Y + inset }, { anchor.X + inset, anchor.Y + height - inset },
         { anchor.X + width - inset, anchor.Y + height - inset }
      };
      for (const auto &probe : probes) {
         const auto s = sampler.locate(probe[0], probe[1]);
         if (paving_key { s.KeyX, s.KeyY, s.KeyZ } != key) {
            return paving_fail("%s: position %.9g,%.9g inside element %" PRId64 ",%" PRId64 ",%" PRId64 " maps to "
               "another element.", Name, probe[0], probe[1], std::get<0>(key), std::get<1>(key), std::get<2>(key));
         }
      }

      // Layout-specific anchor positions.

      const auto [kx, ky, kz] = key;
      double ex = 0, ey = 0;
      bool known = true;
      if (Params.Layout IS paving_layout::RUNNING) {
         ex = (double(kx) + (double(ky) * Params.RowOffset)) * length;
         ey = double(ky) * thickness;
      }
      else if ((Params.Layout IS paving_layout::STACK) or (Params.Layout IS paving_layout::CHECKER)) {
         ex = double(kx) * length;
         ey = double(ky) * thickness;
      }
      else if (Params.Layout IS paving_layout::HERRINGBONE) {
         ex = double(kx) * thickness;
         ey = double(ky) * thickness;
         const int64_t phase = paving_floor_mod(kx - ky, 2 * sampler.ratio());
         if (phase != (kz ? 2 * sampler.ratio() - 1 : 0)) {
            return paving_fail("%s: herringbone anchor %" PRId64 ",%" PRId64 " has phase %" PRId64 ".", Name, kx, ky,
               phase);
         }
      }
      else known = false;

      if (known and ((std::abs(anchor.X - ex) > tolerance) or (std::abs(anchor.Y - ey) > tolerance))) {
         return paving_fail("%s: element %" PRId64 ",%" PRId64 " is anchored at %.9g,%.9g, expected %.9g,%.9g.", Name,
            kx, ky, anchor.X, anchor.Y, ex, ey);
      }
   }

   const bool rotated = (Params.Layout IS paving_layout::BASKET_WEAVE) or (Params.Layout IS paving_layout::HERRINGBONE);
   if (rotated and ((horizontal IS 0) or (vertical IS 0))) {
      return paving_fail("%s: expected both horizontal and vertical elements (%d, %d).", Name, horizontal, vertical);
   }
   if ((not rotated) and (vertical != 0)) return paving_fail("%s: unexpected vertical elements.", Name);

   // The sampled area is filled exactly by whole elements in its interior, so the element count must match its area
   // to within the elements crossing its boundary.

   const double expected = (4.0 * span * span) / (length * thickness);
   const double boundary = (8.0 * span) / std::min(length, thickness) + 8;
   if (std::abs(double(elements.size()) - expected) > boundary) {
      return paving_fail("%s: found %d elements, expected about %.1f.", Name, int(elements.size()), expected);
   }
   return true;
}

static bool test_paving_layouts()
{
   using enum paving_layout;
   return test_paving_partition("Running 0", paving_fixture(RUNNING, 12, 5, 0.0)) and
      test_paving_partition("Running 1/2", paving_fixture(RUNNING, 12, 5, 0.5)) and
      test_paving_partition("Running 1/3", paving_fixture(RUNNING, 9, 4, 1.0 / 3.0)) and
      test_paving_partition("Running -1/4", paving_fixture(RUNNING, 10.5, 3.25, -0.25)) and
      test_paving_partition("Running 2.75", paving_fixture(RUNNING, 8, 6, 2.75)) and
      test_paving_partition("Stack", paving_fixture(STACK, 7, 11)) and
      test_paving_partition("Checker", paving_fixture(CHECKER, 6, 6)) and
      test_paving_partition("Basket weave k=2", paving_fixture(BASKET_WEAVE, 16, 8)) and
      test_paving_partition("Basket weave k=3", paving_fixture(BASKET_WEAVE, 15, 5)) and
      test_paving_partition("Basket weave k=3 near", paving_fixture(BASKET_WEAVE, 15, 5.0001)) and
      test_paving_partition("Herringbone k=2", paving_fixture(HERRINGBONE, 12, 6)) and
      test_paving_partition("Herringbone k=3", paving_fixture(HERRINGBONE, 9, 3)) and
      test_paving_partition("Herringbone k=4", paving_fixture(HERRINGBONE, 10, 2.5));
}

//********************************************************************************************************************

static bool test_paving_check()
{
   using enum paving_layout;
   struct check_case {
      CSTRING Name;
      paving_params Params;
      ERR Expected;
   };

   auto with = [](paving_params Params, auto Modify) { Modify(Params); return Params; };
   const auto running = paving_fixture(RUNNING, 12, 6);

   const check_case cases[] = {
      { "Default running", running, ERR::Okay },
      { "Zero width", paving_fixture(STACK, 0, 6), ERR::InvalidValue },
      { "NaN height", paving_fixture(STACK, 6, std::nan("")), ERR::InvalidValue },
      { "Negative joint", with(running, [](auto &P) { P.JointWidth = -1; }), ERR::InvalidValue },
      { "Negative corner radius", with(running, [](auto &P) { P.CornerRadius = -1; }), ERR::InvalidValue },
      { "NaN corner radius", with(running, [](auto &P) { P.CornerRadius = std::nan(""); }), ERR::InvalidValue },
      { "Negative edge softness", with(running, [](auto &P) { P.EdgeSoftness = -1; }), ERR::InvalidValue },
      { "NaN edge softness", with(running, [](auto &P) { P.EdgeSoftness = std::nan(""); }), ERR::InvalidValue },
      { "Edge softness", with(running, [](auto &P) { P.EdgeSoftness = 2; }), ERR::Okay },
      { "Basket 2.5", paving_fixture(BASKET_WEAVE, 15, 6), ERR::InvalidValue },
      { "Basket 1", paving_fixture(BASKET_WEAVE, 6, 6), ERR::InvalidValue },
      { "Basket within tolerance", paving_fixture(BASKET_WEAVE, 12, 6.0002), ERR::Okay },
      { "Herringbone 1.5", paving_fixture(HERRINGBONE, 9, 6), ERR::InvalidValue },
      { "Herringbone 4", paving_fixture(HERRINGBONE, 24, 6), ERR::Okay },
      { "Rotated periodic", with(running, [](auto &P) { P.PeriodX = 2; P.Angle = 30; }), ERR::InvalidValue },
      { "Full turn periodic", with(running, [](auto &P) { P.PeriodX = 2; P.Angle = -720; }), ERR::Okay },
      { "Rotated aperiodic", with(running, [](auto &P) { P.Angle = 30; }), ERR::Okay },
      { "Running odd rows", with(running, [](auto &P) { P.PeriodY = 3; }), ERR::InvalidValue },
      { "Running even rows", with(running, [](auto &P) { P.PeriodY = 2; }), ERR::Okay },
      { "Running thirds", with(running, [](auto &P) { P.PeriodY = 3; P.RowOffset = 1.0 / 3.0; }), ERR::Okay },
      { "Running horizontal only", with(running, [](auto &P) { P.PeriodX = 3; }), ERR::Okay },
      { "Checker odd", with(paving_fixture(CHECKER, 6, 6), [](auto &P) { P.PeriodX = 3; }), ERR::InvalidValue },
      { "Checker even", with(paving_fixture(CHECKER, 6, 6), [](auto &P) { P.PeriodX = 4; P.PeriodY = 2; }),
         ERR::Okay },
      { "Basket odd", with(paving_fixture(BASKET_WEAVE, 12, 6), [](auto &P) { P.PeriodY = 1; }), ERR::InvalidValue },
      { "Herringbone odd", with(paving_fixture(HERRINGBONE, 12, 6), [](auto &P) { P.PeriodX = 1; }), ERR::Okay },
      { "Period limit", with(paving_fixture(STACK, 6, 6), [](auto &P) { P.PeriodX = PAVING_MAX_PERIOD; }),
         ERR::Okay },
      { "Period overflow", with(paving_fixture(STACK, 6, 6), [](auto &P) { P.PeriodX = PAVING_MAX_PERIOD + 1; }),
         ERR::OutOfRange }
   };

   for (const auto &c : cases) {
      const auto error = paving_check(c.Params);
      if (error != c.Expected) {
         return paving_fail("%s: paving_check() returned %s, expected %s.", c.Name, GetErrorMsg(error),
            GetErrorMsg(c.Expected));
      }
   }

   const paving_sampler stack(paving_fixture(STACK, 1, 1));
   if (not stack.lattice_fits(-1e15, -1e15, 1e15, 1e15)) return paving_fail("A 1e15 extent must fit the lattice.");
   if (stack.lattice_fits(0, 0, 1e16, 1)) return paving_fail("A 1e16 extent must not fit the lattice.");
   if (stack.lattice_fits(0, 0, std::numeric_limits<double>::infinity(), 1)) {
      return paving_fail("An infinite extent must not fit the lattice.");
   }

   const paving_sampler skewed(paving_fixture(RUNNING, 1, 1, 1e6));
   if (skewed.lattice_fits(0, 0, 1, 1e10)) return paving_fail("Row offsets must count towards the lattice extent.");
   return true;
}

//********************************************************************************************************************

static bool test_paving_profiles()
{
   using enum paving_layout;

   // Elements of 20 x 10 with 1 unit half-joints and a 3 unit bevel.  Along y = 5 the interior distance is
   // min(x, 20 - x, 5) - 1.

   auto params = paving_fixture(STACK, 20, 10);
   params.JointWidth = 2;
   params.BevelWidth = 3;
   params.JointHeightValue = -0.5;
   params.ElementHeightValue = 2.0;

   const GEZ profiles[] = { GEZ::LINEAR, GEZ::IN, GEZ::OUT, GEZ::IN_OUT, GEZ::CUBIC_IN, GEZ::CUBIC_OUT,
      GEZ::CUBIC_IN_OUT, GEZ::CIRCLE_IN, GEZ::CIRCLE_OUT, GEZ::CIRCLE_IN_OUT };

   double midpoints[10];
   for (int p=0; p < 10; p++) {
      params.BevelProfile = profiles[p];
      const paving_sampler sampler(params);
      double previous = -1e9;
      for (double x=0; x <= 10.0; x += 0.01) {
         const double h = sampler.height(sampler.locate(x, 5.0), 0.0);
         if (h < previous - 1e-12) return paving_fail("Profile %d: height decreases at x = %.3f.", p, x);
         if ((x <= 1.0) and (h != -0.5)) return paving_fail("Profile %d: joint height %.9g at x = %.3f.", p, h, x);
         if ((x >= 4.0) and (h != 2.0)) return paving_fail("Profile %d: element height %.9g at x = %.3f.", p, h, x);
         previous = h;
      }
      midpoints[p] = sampler.height(sampler.locate(2.5, 5.0), 0.0);
      if ((midpoints[p] <= -0.5) or (midpoints[p] >= 2.0)) {
         return paving_fail("Profile %d: the bevel midpoint %.9g is not between the joint and element.", p,
            midpoints[p]);
      }

      // Seeded height offsets raise the element top.

      if (sampler.height(sampler.locate(10, 5), 0.25) != 2.25) return paving_fail("Height offsets must apply.");
   }

   if (std::abs(midpoints[0] - 0.75) > 1e-9) return paving_fail("The linear bevel midpoint is %.9g.", midpoints[0]);
   if (not ((midpoints[8] > midpoints[0]) and (midpoints[0] > midpoints[7]))) {
      return paving_fail("CIRCLE_OUT must be above LINEAR and CIRCLE_IN below it at the bevel midpoint.");
   }

   // No bevel: a step from joint to element.

   params.BevelProfile = GEZ::LINEAR;
   params.BevelWidth = 0;
   {
      const paving_sampler sampler(params);
      if ((sampler.height(sampler.locate(0.99, 5), 0) != -0.5) or (sampler.height(sampler.locate(1.01, 5), 0) != 2.0)) {
         return paving_fail("A zero bevel width must step from the joint to the element.");
      }
   }

   // A bevel wider than the element peaks along the centre ridge without overshoot or discontinuity.

   params.BevelWidth = 8;
   {
      const paving_sampler sampler(params);
      double previous = sampler.height(sampler.locate(0, 5), 0);
      for (double x=0.01; x < 20.0; x += 0.01) {
         const double h = sampler.height(sampler.locate(x, 5), 0);
         if ((h >= 2.0) or (std::abs(h - previous) > 0.01)) {
            return paving_fail("Wide bevel: height %.9g at x = %.2f overshoots or jumps.", h, x);
         }
         previous = h;
      }
   }

   // No joint: full coverage everywhere, with the bevel still sloping from the element edge.

   params.JointWidth = 0;
   params.BevelWidth = 3;
   {
      const paving_sampler sampler(params, 1.3, 0.7);
      for (double y=0.05; y < 20; y += 0.5) {
         for (double x=0.05; x < 40; x += 0.5) {
            if (sampler.coverage(sampler.locate(x, y)) != 1.0) {
               return paving_fail("Coverage must be 1 without joints, at %.2f,%.2f.", x, y);
            }
         }
      }
      if (sampler.height(sampler.locate(0, 5), 0) != -0.5) {
         return paving_fail("The element edge must be at joint height.");
      }
      if (std::abs(sampler.height(sampler.locate(1.5, 5), 0) - 0.75) > 1e-9) return paving_fail("Joint-free bevel.");
      if (sampler.id(sampler.locate(0, 5), 0.5) != 0.5) return paving_fail("IDs must cover edges without joints.");
   }

   // Joints at least as wide as the element leave only joint output.

   for (double joint : { 10.0, 25.0 }) {
      params.JointWidth = joint;
      const paving_sampler sampler(params, 0.8, 0.8);
      for (double y=0.0; y < 20; y += 0.25) {
         for (double x=0.0; x < 40; x += 0.25) {
            const auto s = sampler.locate(x, y);
            if ((sampler.height(s, 0) != -0.5) or (sampler.coverage(s) != 0.0) or (sampler.id(s, 0.5) != 0.0)) {
               return paving_fail("A %.0f unit joint must cover %.2f,%.2f entirely.", joint, x, y);
            }
         }
      }
   }

   // Rounded corners: exact circular signed distance, clamping to the inscribed radius, smooth bevels and zero-joint
   // corner spandrels.

   params = paving_fixture(STACK, 20, 10);
   params.JointWidth = 2;
   params.BevelWidth = 4;
   params.CornerRadius = 3;
   {
      const paving_sampler sampler(params, 1.0, 1.0);
      if (std::abs(sampler.corner_radius() - 3.0) > 1e-12) return paving_fail("Corner radius must remain 3.");
      // Corner pocket at (1.1, 1.1): ex = 0.1, ey = 0.1, dist to arc centre (3, 3) = hypot(2.9, 2.9) > 3 -> outside.
      const auto corner = sampler.locate(1.1, 1.1);
      if ((sampler.interior(corner) >= 0) or (sampler.id(corner, 0.5) != 0.0) or
          (sampler.height(corner, 0) != 0.0)) {
         return paving_fail("Rounded corner pocket must behave as joint.");
      }
      // On the arc at ex = 3 - 3/sqrt(2), ey = 3 - 3/sqrt(2): interior == 0, coverage == 0.5.
      const double arc_off = 1.0 + 3.0 - (3.0 / std::sqrt(2.0));
      const auto on_arc = sampler.locate(arc_off, arc_off);
      if ((std::abs(sampler.interior(on_arc)) > 1e-9) or (std::abs(sampler.coverage(on_arc) - 0.5) > 1e-9)) {
         return paving_fail("On-arc point must have interior 0 and coverage 0.5.");
      }
      // Arc centre at (4, 4) has interior == 3 -> height = 3 / 4 = 0.75.
      const auto arc_centre = sampler.locate(4.0, 4.0);
      if ((std::abs(sampler.interior(arc_centre) - 3.0) > 1e-9) or
          (std::abs(sampler.height(arc_centre, 0) - 0.75) > 1e-9)) {
         return paving_fail("Arc centre must have interior equal to corner radius.");
      }
   }

   // Oversized corner radius clamps to half the interior shortest side: (10/2) - 1 = 4.

   params.CornerRadius = 100;
   {
      const paving_sampler sampler(params, 1.0, 1.0);
      if (std::abs(sampler.corner_radius() - 4.0) > 1e-12) {
         return paving_fail("Oversized corner radius must clamp to 4, got %.9g.", sampler.corner_radius());
      }
   }

   // Zero joint width with rounded corners keeps full coverage on straight edges while opening corner pockets.

   params = paving_fixture(STACK, 20, 20);
   params.JointWidth = 0;
   params.CornerRadius = 6;
   {
      const paving_sampler sampler(params, 1.0, 1.0);
      if (sampler.coverage(sampler.locate(10.0, 0.0)) != 1.0) {
         return paving_fail("Straight edge midpoint must stay at full coverage when JointWidth is 0.");
      }
      if (sampler.coverage(sampler.locate(0.1, 0.1)) != 0.0) {
         return paving_fail("Corner spandrel must open to zero coverage when CornerRadius is 6.");
      }
   }

   // On an axis-aligned integer grid, coverage at pixel centres is exactly 0 or 1 and agrees with the element ID.

   params = paving_fixture(STACK, 8, 4);
   params.JointWidth = 2;
   {
      const paving_sampler sampler(params);
      for (int y=-8; y < 8; y++) {
         for (int x=-16; x < 16; x++) {
            const auto s = sampler.locate(x + 0.5, y + 0.5);
            const double cov = sampler.coverage(s);
            if ((cov != 0.0) and (cov != 1.0)) {
               return paving_fail("Coverage %.9g at pixel %d,%d is not binary.", cov, x, y);
            }
            if ((cov IS 1.0) != (sampler.id(s, 0.5) IS 0.5)) {
               return paving_fail("Coverage and ID disagree at pixel %d,%d.", x, y);
            }
         }
      }
   }

   // A fractional joint gives partial coverage across a one pixel ramp, scaled by the pixel footprint.

   params.JointWidth = 1.5;
   {
      const paving_sampler sampler(params, 2.0, 1.0);
      const double c0 = sampler.coverage(sampler.locate(0.75, 2.0));  // On the joint edge: half coverage
      const double c1 = sampler.coverage(sampler.locate(1.25, 2.0));  // A quarter pixel inside the element
      if ((std::abs(c0 - 0.5) > 1e-12) or (std::abs(c1 - 0.75) > 1e-12)) {
         return paving_fail("Joint edge coverage was %.9g and %.9g, expected 0.5 and 0.75.", c0, c1);
      }
   }

   // Vertical elements swap the pixel footprints.

   params = paving_fixture(BASKET_WEAVE, 8, 4);
   params.JointWidth = 1;
   {
      const paving_sampler sampler(params, 2.0, 1.0);
      const auto s = sampler.locate(10.0, 0.75); // Vertical macro-cell: LX = 0.75, LY = 2
      if ((not s.Vertical) or (std::abs(sampler.coverage(s) - 0.75) > 1e-12)) {
         return paving_fail("Vertical element coverage was %.9g, expected 0.75.", sampler.coverage(s));
      }
   }

   // EdgeSoftness widens the one pixel ramp into a linear fringe centred on the element edge.  Thin joints blend both
   // neighbours, so a fringe wider than the joint leaves no pure joint colour at the joint centre.

   params = paving_fixture(STACK, 20, 10);
   params.JointWidth = 2;
   params.EdgeSoftness = 4;
   {
      const paving_sampler sampler(params, 1.0, 1.0);
      const std::pair<double, double> expected[] = { { 0.0, 0.5 }, { 1.0, 0.5 }, { 2.0, 0.75 }, { 3.0, 1.0 } };
      for (auto [x, want] : expected) {
         const double cov = sampler.coverage(sampler.locate(x, 5.0));
         if (std::abs(cov - want) > 1e-12) {
            return paving_fail("Soft thin joint coverage at x = %.2f was %.9g, expected %.9g.", x, cov, want);
         }
      }
      double previous = sampler.coverage(sampler.locate(0.0, 5.0));
      for (double x=0.05; x <= 4.0; x += 0.05) {
         const double cov = sampler.coverage(sampler.locate(x, 5.0));
         if (cov < previous - 1e-12) return paving_fail("Soft coverage decreases at x = %.2f.", x);
         previous = cov;
      }
   }

   // An element occupies one quarter of a footprint centred on its square corner on either side of the threshold.

   for (double softness : { 1.99999, 2.0, 2.00001 }) {
      params.EdgeSoftness = softness;
      const paving_sampler sampler(params, 1.0, 1.0);
      const double coverage = sampler.coverage(sampler.locate(1.0, 1.0));
      if (std::abs(coverage - 0.25) > 1e-12) {
         return paving_fail("Corner coverage at softness %.5f was %.9g, expected 0.25.", softness, coverage);
      }
   }

   // Wide joints take the single element ramp, which is also widened.

   params.JointWidth = 6;
   params.EdgeSoftness = 4;
   {
      const paving_sampler sampler(params, 1.0, 1.0);
      const double c0 = sampler.coverage(sampler.locate(3.0, 5.0)); // On the edge
      const double c1 = sampler.coverage(sampler.locate(4.0, 5.0)); // One unit inside
      const double c2 = sampler.coverage(sampler.locate(0.0, 5.0)); // Joint centre
      if ((std::abs(c0 - 0.5) > 1e-12) or (std::abs(c1 - 0.75) > 1e-12) or (c2 != 0.0)) {
         return paving_fail("Soft wide joint coverage was %.9g, %.9g and %.9g, expected 0.5, 0.75 and 0.", c0, c1, c2);
      }
   }

   // A softness below the pixel footprint leaves the one pixel ramp unchanged.

   params.JointWidth = 1.5;
   params.EdgeSoftness = 0.5;
   {
      const paving_sampler soft(params, 1.0, 1.0);
      params.EdgeSoftness = 0;
      const paving_sampler hard(params, 1.0, 1.0);
      for (double x=0.0; x < 3.0; x += 0.125) {
         if (soft.coverage(soft.locate(x, 5.0)) != hard.coverage(hard.locate(x, 5.0))) {
            return paving_fail("A sub-pixel EdgeSoftness changed coverage at x = %.3f.", x);
         }
      }
   }
   return true;
}

//********************************************************************************************************************

static bool test_paving_colour()
{
   auto lab_of = [](const oklab::LinearRGB &C) { return paving_lch_from_linear(C.r, C.g, C.b); };
   auto in_gamut = [](const oklab::LinearRGB &C) {
      return (C.r >= 0) and (C.r <= 1) and (C.g >= 0) and (C.g <= 1) and (C.b >= 0) and (C.b <= 1);
   };

   // Without variation fields, every element offset is zero and the colour round-trips.  The ten digit OKLab
   // coefficients limit round-trip precision to about 1e-7.

   auto params = paving_fixture(paving_layout::STACK, 8, 4);
   params.Seed = 99;
   if (params.colour_varied()) return paving_fail("Default parameters must not vary colour.");
   {
      const paving_sampler sampler(params);
      const auto v = sampler.variation(sampler.element_key(sampler.locate(3, 3)));
      if ((v.Hue != 0) or (v.Chroma != 0) or (v.Light != 0) or (v.Height != 0)) {
         return paving_fail("Zero variation fields must give zero offsets.");
      }
      const auto base = paving_lch_from_linear(0.4, 0.2, 0.1);
      const auto out = paving_adjust_oklch(base, v.Light, v.Chroma, v.Hue, v.HueUnit, false);
      if ((std::abs(out.r - 0.4) > 1e-6) or (std::abs(out.g - 0.2) > 1e-6) or (std::abs(out.b - 0.1) > 1e-6)) {
         return paving_fail("Unvaried colours must round-trip.");
      }
   }

   // Variates span the configured ranges and are deterministic per element.

   params.HueVariation = 12;
   params.ChromaVariation = 0.05;
   params.LightVariation = 0.1;
   params.HeightVariation = 0.3;
   {
      const paving_sampler sampler(params);
      double min_hue = 1e9, max_hue = -1e9;
      std::set<uint64_t> keys;
      for (int y=0; y < 16; y++) {
         for (int x=0; x < 16; x++) {
            const auto key = sampler.element_key(sampler.locate(x * 8 + 1, y * 4 + 1));
            keys.insert(key);
            const auto v = sampler.variation(key);
            if ((std::abs(v.Hue) > 12) or (std::abs(v.Chroma) > 0.05) or (std::abs(v.Light) > 0.1) or
                (std::abs(v.Height) > 0.3) or (v.Id < 0) or (v.Id >= 1)) {
               return paving_fail("Element variation is out of range.");
            }
            min_hue = std::min(min_hue, v.Hue);
            max_hue = std::max(max_hue, v.Hue);
            if (sampler.element_key(sampler.locate(x * 8 + 6.5, y * 4 + 2.5)) != key) {
               return paving_fail("Keys must be constant within an element.");
            }
         }
      }
      if (keys.size() != 256) return paving_fail("Expected 256 distinct element keys, found %d.", int(keys.size()));
      if ((min_hue > -6) or (max_hue < 6)) return paving_fail("Hue variates do not span their range.");
   }

   // 64-bit seeds are not truncated, and seed zero is seeded.

   const int64_t seeds[] = { 0, 1, int64_t(1) << 32, (int64_t(1) << 32) + 1, -1, INT64_MIN };
   std::set<uint64_t> seeded;
   for (auto seed : seeds) seeded.insert(paving_element_key(seed, 0, 0, 0));
   if (seeded.size() != std::size(seeds)) return paving_fail("Distinct seeds must give distinct element keys.");
   if (paving_element_key(0, 0, 0, 0) IS 0) return paving_fail("Seed zero must be mixed.");

   // Hue shifts preserve lightness and chroma when in gamut.

   const auto brick = paving_lch_from_linear(0.3, 0.12, 0.06);
   {
      const auto out = lab_of(paving_adjust_oklch(brick, 0, 0, 25, 0, true));
      double dh = out.H - (brick.H + 25.0 * agg::pi / 180.0);
      dh = std::remainder(dh, 2.0 * agg::pi);
      if ((std::abs(out.L - brick.L) > 1e-6) or (std::abs(out.C - brick.C) > 1e-6) or (std::abs(dh) > 1e-6)) {
         return paving_fail("An in-gamut hue shift changed lightness, chroma or hue.");
      }
   }

   // Out-of-gamut results reduce chroma at fixed lightness and hue.

   const auto red = paving_lch_from_linear(0.9, 0.08, 0.04);
   for (double dh : { -30.0, 0.0, 40.0 }) {
      const auto rgb = paving_adjust_oklch(red, -0.05, 0.25, dh, 0, true);
      if (not in_gamut(rgb)) return paving_fail("The gamut-mapped colour is out of gamut.");
      const auto out = lab_of(rgb);
      const double target_h = red.H + dh * agg::pi / 180.0;
      const double hue_error = std::remainder(out.H - target_h, 2.0 * agg::pi);
      if ((std::abs(out.L - (red.L - 0.05)) > 2e-3) or (std::abs(hue_error) > 1e-2) or (out.C >= red.C + 0.25)) {
         return paving_fail("Gamut mapping at %+.0f degrees moved L %.6f -> %.6f or hue by %.6f.", dh, red.L - 0.05,
            out.L, hue_error);
      }
   }

   // Lightness is clamped.

   {
      const auto white = paving_adjust_oklch(brick, 2.0, 0, 0, 0, false);
      if ((std::abs(white.r - 1) > 1e-6) or (std::abs(white.g - 1) > 1e-6) or (std::abs(white.b - 1) > 1e-6)) {
         return paving_fail("Lightness above 1 must clamp to white.");
      }
      const auto black = paving_adjust_oklch(brick, -2.0, 0, 0, 0, false);
      if ((black.r > 1e-9) or (black.g > 1e-9) or (black.b > 1e-9)) {
         return paving_fail("Lightness must clamp to black.");
      }
   }

   // Neutral greys stay neutral without hue variation, and take the seeded hue with it.

   const auto grey = paving_lch_from_linear(0.2, 0.2, 0.2);
   if (not grey.Neutral) return paving_fail("Grey must be classified as neutral.");
   {
      const auto rgb = paving_adjust_oklch(grey, 0.05, 0.04, 30, 0.25, false);
      const auto out = lab_of(rgb);
      if ((std::abs(rgb.r - rgb.g) > 1e-9) or (std::abs(rgb.g - rgb.b) > 1e-9) or (out.C > 1e-6) or
          (std::abs(out.L - (grey.L + 0.05)) > 1e-6)) {
         return paving_fail("Grey acquired a tint without hue variation.");
      }
   }
   for (double unit : { 0.1, 0.4, 0.8 }) {
      const auto out = lab_of(paving_adjust_oklch(grey, 0, 0.03, 30, unit, true));
      if ((std::abs(out.C - 0.03) > 1e-6) or
          (std::abs(std::remainder(out.H - unit * 2.0 * agg::pi, 2.0 * agg::pi)) > 1e-6)) {
         return paving_fail("A tinted grey must take chroma 0.03 at the seeded hue (C %.6f).", out.C);
      }
   }
   return true;
}

//********************************************************************************************************************
// Periodic pavings must repeat exactly, including seeded appearance, and wrapping must not merge distinct elements
// within one period.

static bool test_paving_period(CSTRING Name, paving_params Params, int ElementsPerPeriod)
{
   Params.Seed = -123456789;
   Params.JointWidth = 0.75;
   Params.BevelWidth = 1.25;
   Params.HueVariation = 10;
   Params.ChromaVariation = 0.03;
   Params.LightVariation = 0.08;
   Params.HeightVariation = 0.2;
   if (paving_check(Params) != ERR::Okay) return paving_fail("%s: the parameters were rejected.", Name);

   const paving_sampler sampler(Params);
   double px, py;
   sampler.period_vector(px, py);
   const double extent_x = (px > 0) ? px : 4.0 * sampler.length();
   const double extent_y = (py > 0) ? py : 4.0 * sampler.length();
   const double step = sampler.thickness() / 3.1;
   std::set<uint64_t> wrapped;

   const double shifts[][2] = { { px, 0 }, { 0, py }, { px, py }, { -px, 0 }, { 0, -py }, { 3 * px, -2 * py } };

   for (double qy = 0.0113; qy < extent_y; qy += step) {
      for (double qx = 0.0071; qx < extent_x; qx += step) {
         const auto a = sampler.locate(qx, qy);
         const auto key = sampler.element_key(a);
         wrapped.insert(key);
         const double ha = sampler.height(a, sampler.variation(key).Height);

         for (const auto &shift : shifts) {
            if ((shift[0] IS 0) and (shift[1] IS 0)) continue;
            const auto b = sampler.locate(qx + shift[0], qy + shift[1]);
            const auto key_b = sampler.element_key(b);
            if ((key_b != key) or (std::abs(a.LX - b.LX) > 1e-9) or (std::abs(a.LY - b.LY) > 1e-9) or
                (a.Vertical != b.Vertical) or (a.Parity != b.Parity) or
                (std::abs(sampler.height(b, sampler.variation(key_b).Height) - ha) > 1e-9) or
                (std::abs(sampler.coverage(a) - sampler.coverage(b)) > 1e-9)) {
               return paving_fail("%s: position %.4f,%.4f does not repeat over %.4f,%.4f.", Name, qx, qy, shift[0],
                  shift[1]);
            }
         }
      }
   }

   if ((px > 0) and (py > 0) and (int(wrapped.size()) != ElementsPerPeriod)) {
      return paving_fail("%s: found %d distinct elements per period, expected %d.", Name, int(wrapped.size()),
         ElementsPerPeriod);
   }

   // Without periods, the same translation must change the element.

   auto aperiodic = Params;
   aperiodic.PeriodX = aperiodic.PeriodY = 0;
   const paving_sampler unwrapped(aperiodic);
   const double qx = sampler.length() * 0.5, qy = sampler.thickness() * 0.5;
   if (unwrapped.element_key(unwrapped.locate(qx, qy)) IS
       unwrapped.element_key(unwrapped.locate(qx + extent_x, qy + extent_y))) {
      return paving_fail("%s: aperiodic elements must not repeat.", Name);
   }
   return true;
}

static bool test_paving_periods()
{
   using enum paving_layout;
   auto periodic = [](paving_params Params, int X, int Y) { Params.PeriodX = X; Params.PeriodY = Y; return Params; };

   return test_paving_period("Running 1/2", periodic(paving_fixture(RUNNING, 12, 5, 0.5), 3, 2), 6) and
      test_paving_period("Running 1/3", periodic(paving_fixture(RUNNING, 9, 4, 1.0 / 3.0), 4, 3), 12) and
      test_paving_period("Running -1/4", periodic(paving_fixture(RUNNING, 10, 4, -0.25), 2, 4), 8) and
      test_paving_period("Running 3/2", periodic(paving_fixture(RUNNING, 10, 4, 1.5), 3, 2), 6) and
      test_paving_period("Running rows only", periodic(paving_fixture(RUNNING, 12, 5, 0.5), 0, 2), 0) and
      test_paving_period("Running columns only", periodic(paving_fixture(RUNNING, 12, 5, 0.5), 5, 0), 0) and
      test_paving_period("Stack", periodic(paving_fixture(STACK, 7, 3), 3, 5), 15) and
      test_paving_period("Checker", periodic(paving_fixture(CHECKER, 6, 6), 2, 4), 8) and
      test_paving_period("Basket weave k=2", periodic(paving_fixture(BASKET_WEAVE, 16, 8), 2, 2), 8) and
      test_paving_period("Basket weave k=3", periodic(paving_fixture(BASKET_WEAVE, 15, 5), 4, 2), 24) and
      test_paving_period("Herringbone k=2", periodic(paving_fixture(HERRINGBONE, 12, 6), 1, 1), 8) and
      test_paving_period("Herringbone k=3", periodic(paving_fixture(HERRINGBONE, 9, 3), 2, 1), 24) and
      test_paving_period("Herringbone k=4", periodic(paving_fixture(HERRINGBONE, 10, 2.5), 1, 1), 16);
}

//********************************************************************************************************************
// Draws a HEIGHT-output PavingFX through a probe and compares every sample with paving_sampler evaluated from pixel
// centres in client coordinates.  Pixels outside Covered must be zero.

struct paving_pipeline_fixture {
   CSTRING Name;
   LMD Mode = LMD::HEIGHT;
   paving_params Params;
   Unit ElementWidth, ElementHeight, OriginX, OriginY, JointWidth, BevelWidth, CornerRadius = Unit(0.0);
   double Angle = 0;
   VUNIT PrimitiveUnits = VUNIT::USERSPACE;
   TClipRectangle<double> Subregion = { 0.0 };
   int PageWidth = 80, PageHeight = 40;
   double AAX = 1.0, AAY = 1.0;
   ERR DrawError = ERR::Okay;
   std::function<void(double, double, double &, double &)> Pattern;
   std::function<bool(double, double)> Covered;
};

static bool test_filter_paving_case(const paving_pipeline_fixture &Case)
{
   auto fixture_class = register_filter_test_class<extFilterSurfaceTest>();
   filter_test_scene scene(80, 40);
   if ((not fixture_class.ok()) or (not scene.ready())) return false;
   scene.Filter->PrimitiveUnits = Case.PrimitiveUnits;
   scene.Scene->PageWidth = Case.PageWidth;
   scene.Scene->PageHeight = Case.PageHeight;

   const auto owner = fl::Owner(scene.Filter->UID);
   auto paving = Case.Subregion.valid() and (Case.Subregion.width() > 0) ?
      extPavingFX::create::local(owner, fl::X(Case.Subregion.left), fl::Y(Case.Subregion.top),
         fl::Width(Case.Subregion.width()), fl::Height(Case.Subregion.height())) :
      extPavingFX::create::local(owner);
   auto probe = extFilterSurfaceTest::create::local(owner);
   if ((not paving) or (not probe)) return false;

   const auto &p = Case.Params;
   auto elem_w = Case.ElementWidth, elem_h = Case.ElementHeight;
   auto orig_x = Case.OriginX, orig_y = Case.OriginY;
   auto joint_w = Case.JointWidth, bevel_w = Case.BevelWidth, corner_r = Case.CornerRadius;
   if ((PAVINGFX_SET_Layout(paving, LAY(int(p.Layout))) != ERR::Okay) or
       (PAVINGFX_SET_Mode(paving, Case.Mode) != ERR::Okay) or
       (PAVINGFX_SET_Seed(paving, p.Seed) != ERR::Okay) or
       (PAVINGFX_SET_ElementWidth(paving, elem_w) != ERR::Okay) or
       (PAVINGFX_SET_ElementHeight(paving, elem_h) != ERR::Okay) or
       (PAVINGFX_SET_OriginX(paving, orig_x) != ERR::Okay) or
       (PAVINGFX_SET_OriginY(paving, orig_y) != ERR::Okay) or
       (PAVINGFX_SET_JointWidth(paving, joint_w) != ERR::Okay) or
       (PAVINGFX_SET_BevelWidth(paving, bevel_w) != ERR::Okay) or
       (PAVINGFX_SET_CornerRadius(paving, corner_r) != ERR::Okay) or
       (PAVINGFX_SET_BevelProfile(paving, p.BevelProfile) != ERR::Okay) or
       (PAVINGFX_SET_Angle(paving, Case.Angle) != ERR::Okay) or
       (PAVINGFX_SET_RowOffset(paving, p.RowOffset) != ERR::Okay) or
       (PAVINGFX_SET_ElementHeightValue(paving, p.ElementHeightValue) != ERR::Okay) or
       (PAVINGFX_SET_JointHeightValue(paving, p.JointHeightValue) != ERR::Okay) or
       (PAVINGFX_SET_HeightVariation(paving, p.HeightVariation) != ERR::Okay) or
       (PAVINGFX_SET_PeriodX(paving, p.PeriodX) != ERR::Okay) or
       (PAVINGFX_SET_PeriodY(paving, p.PeriodY) != ERR::Okay) or
       (PAVINGFX_SET_OutputMode(paving, FOM::HEIGHT) != ERR::Okay)) {
      return paving_fail("%s: a PavingFX field was rejected.", Case.Name);
   }

   filter_named_input inputs[] { { "Paving", { .Kind = filter_input_kind::SCALAR_MAP } } };
   const filter_effect_extension probe_extension { inputs };
   probe->FilterExtension = &probe_extension;
   if (set_named_filter_input(probe, "Paving", VSF::REFERENCE, paving) != ERR::Okay) return false;

   const paving_sampler reference(p, Case.AAX, Case.AAY);
   int covered = 0;
   probe->DrawRoutine = [&](extFilterEffect *Effect) {
      filter_map_view map;
      if (auto error = get_filter_map(Effect, "Paving", map); error != ERR::Okay) return error;
      const auto &view = map.Source;
      for (int y=view.OriginY; y < view.OriginY + view.Height; ++y) {
         for (int x=view.OriginX; x < view.OriginX + view.Width; ++x) {
            const double px = x + 0.5, py = y + 0.5;
            double expected = 0;
            if (Case.Covered(px, py)) {
               double qx, qy;
               Case.Pattern(px, py, qx, qy);
               const auto s = reference.locate(qx, qy);
               if (Case.Mode IS LMD::HEIGHT) {
                  const double dz = (p.HeightVariation != 0) ?
                     reference.variation(reference.element_key(s)).Height : 0.0;
                  expected = reference.height(s, dz);
               }
               else if (Case.Mode IS LMD::MASK) expected = reference.coverage(s);
               else expected = reference.id(s, cellular_hash(reference.element_key(s), 0));
               covered++;
            }
            const float actual = map.sample(px, py)[0];
            if (std::abs(actual - expected) > 1e-5 * std::max(1.0, std::abs(expected))) {
               kt::Log("FilterTest").warning("%s: sample at %d,%d was %.9g, expected %.9g.", Case.Name, x, y,
                  actual, expected);
               return ERR::InvalidValue;
            }
         }
      }
      return ERR::Okay;
   };

   filter_render_result output;
   if (auto error = scene.draw(output); error != Case.DrawError) {
      return paving_fail("%s: draw returned %s, expected %s.", Case.Name, GetErrorMsg(error),
         GetErrorMsg(Case.DrawError));
   }
   return (Case.DrawError != ERR::Okay) or
      filter_test_expect(covered >= 100, "The PavingFX fixture compared too few covered samples.");
}

static bool test_filter_paving()
{
   // The filter region spans x = [-2, 80) and y = [0, 40) in client units, which equal scene pixels here.

   constexpr double region_left = -2, region_top = 0, region_width = 82, region_height = 40;
   const double region_diag = filter_render_context::normalised_diagonal(region_width, region_height);

   paving_pipeline_fixture height_offset {
      .Name = "Height subregion",
      .Mode = LMD::HEIGHT,
      .Params = { .Layout = paving_layout::RUNNING, .ElementWidth = 16, .ElementHeight = 8,
         .JointWidth = 0.02 * region_diag, .BevelWidth = 3.0, .RowOffset = 0.5, .ElementHeightValue = 1.5,
         .JointHeightValue = -0.25, .HeightVariation = 0.25, .BevelProfile = GEZ::CIRCLE_OUT, .Seed = -77 },
      .ElementWidth = Unit(16.0), .ElementHeight = Unit(8.0), .OriginX = Unit(3.0), .OriginY = Unit(-2.0),
      .JointWidth = Unit(0.02, FD_SCALED), .BevelWidth = Unit(3.0),
      .Subregion = { 6, 3, 56, 33 },
      .Pattern = [&](double X, double Y, double &QX, double &QY) {
         QX = X - (region_left + 3.0);
         QY = Y - (region_top - 2.0);
      },
      .Covered = [](double X, double Y) { return (X >= 6) and (X < 56) and (Y >= 3) and (Y < 33); }
   };

   paving_pipeline_fixture mask_rotated {
      .Name = "Rotated herringbone mask",
      .Mode = LMD::MASK,
      .Params = { .Layout = paving_layout::HERRINGBONE, .ElementWidth = 16, .ElementHeight = 8,
         .JointWidth = 2.0, .Angle = 30 },
      .ElementWidth = Unit(16.0), .ElementHeight = Unit(8.0), .OriginX = Unit(0.25, FD_SCALED),
      .OriginY = Unit(10.0), .JointWidth = Unit(2.0), .BevelWidth = Unit(0.0),
      .Angle = 30,
      .Pattern = [&](double X, double Y, double &QX, double &QY) {
         const double du = X - (region_left + 0.25 * region_width), dv = Y - (region_top + 10.0);
         const double c = std::cos(30.0 * agg::pi / 180.0), s = std::sin(30.0 * agg::pi / 180.0);
         QX = (c * du) + (s * dv);
         QY = (c * dv) - (s * du);
      },
      .Covered = [&](double X, double Y) {
         return (X >= region_left) and (X < region_left + region_width) and (Y >= region_top) and
            (Y < region_top + region_height);
      }
   };

   // In BOUNDING_BOX units, primitive coordinates are fractions of the client bounds (0, 0) to (80, 40).

   const double bb_w = region_width / 80.0, bb_h = region_height / 40.0;
   const double bb_diag = filter_render_context::normalised_diagonal(bb_w, bb_h);
   paving_pipeline_fixture id_bbox {
      .Name = "Bounding box periodic ID",
      .Mode = LMD::ID,
      .Params = { .Layout = paving_layout::BASKET_WEAVE, .ElementWidth = 0.2, .ElementHeight = 0.1,
         .JointWidth = 0.02 * bb_diag, .Seed = 9, .PeriodX = 2, .PeriodY = 2 },
      .ElementWidth = Unit(0.2), .ElementHeight = Unit(0.1), .OriginX = Unit(0.05), .OriginY = Unit(0.0),
      .JointWidth = Unit(0.02, FD_SCALED), .BevelWidth = Unit(0.0),
      .PrimitiveUnits = VUNIT::BOUNDING_BOX,
      .AAX = 1.0 / 80.0, .AAY = 1.0 / 40.0,
      .Pattern = [&](double X, double Y, double &QX, double &QY) {
         QX = (X / 80.0) - ((region_left / 80.0) + 0.05);
         QY = (Y / 40.0) - (region_top / 40.0);
      },
      .Covered = mask_rotated.Covered
   };

   auto clipped = height_offset;
   clipped.Name = "Clipped redraw";
   clipped.PageWidth = 37;
   clipped.PageHeight = 23;

   auto oversized_height = height_offset;
   oversized_height.Name = "Height exceeds scalar range";
   oversized_height.Params.ElementHeightValue = 1e40;
   oversized_height.DrawError = ERR::OutOfRange;

   auto rounded_height = height_offset;
   rounded_height.Name = "Rounded height subregion";
   rounded_height.Params.CornerRadius = 0.03 * region_diag;
   rounded_height.CornerRadius = Unit(0.03, FD_SCALED);

   return test_filter_paving_case(height_offset) and test_filter_paving_case(rounded_height) and
      test_filter_paving_case(mask_rotated) and test_filter_paving_case(id_bbox) and
      test_filter_paving_case(clipped) and test_filter_paving_case(oversized_height);
}

//********************************************************************************************************************

void vector_filter_unit_tests(std::string_view Options, int &Passed, int &Total)
{
   if (Options IS "filter:named") {
      Total++;
      if (test_filter_named_input()) Passed++;
   }
   else if (Options IS "filter:numerical") {
      Total++;
      if (test_filter_numerical_surfaces() and test_filter_displacement_vector_map()) Passed++;
   }
   else if (Options IS "filter:height") {
      Total += 4;
      if (test_filter_primary_height()) Passed++;
      if (test_filter_native_blur_range() and test_filter_height_blur() and
          test_filter_numeric_neighbourhoods(filter_surface_format::SCALAR_F32) and
          test_filter_numeric_neighbourhoods(filter_surface_format::VECTOR_F32)) Passed++;
      if (test_filter_height_turbulence()) Passed++;
      if (test_filter_blurred_alpha_edge()) Passed++;
   }
   else if (Options IS "filter:cellular") {
      Total++;
      if (test_filter_cellular()) Passed++;
   }
   else if (Options IS "filter:paving") {
      Total += 6;
      if (test_paving_check()) Passed++;
      if (test_paving_layouts()) Passed++;
      if (test_paving_profiles()) Passed++;
      if (test_paving_colour()) Passed++;
      if (test_paving_periods()) Passed++;
      if (test_filter_paving()) Passed++;
   }
}
