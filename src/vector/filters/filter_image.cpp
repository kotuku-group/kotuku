/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
ImageFX: Renders a bitmap image in the effect pipeline.

The ImageFX class will render a source image into a given rectangle within the current user coordinate system.  The
client has the option of providing a pre-allocated @Bitmap or the path to a @Image file as the source.

If a pre-allocated @Bitmap is to be used, it must be owned by the ImageFX object before the ImageFX is initialised.
The bitmap can be created as a child of the uninitialised ImageFX, or an existing bitmap can be assigned to it with
~Core.SetOwner().  The bitmap must be initialised before the ImageFX.  The default `IMAGE` mode requires 32 bits per
pixel with an alpha channel.  The bitmap is retained for the lifetime of the ImageFX.  An attempt to free it earlier is
deferred until the ImageFX is freed.  Changes to the content of the bitmap are rendered on the next draw of the filter.
Call @Vector.Refresh() on the filtered vector to ensure that it is redrawn.

If a source image file is referenced, it will be upscaled to meet the requirements automatically as needed.

Setting #OutputMode to `HEIGHT` produces a floating-point map for effects such as @TileFX and @LightingFX.
A `BMF::GREYSCALE` bitmap supplies its full 16-bit samples, including when loaded from a 16-bit greyscale PNG.
For other formats, #HeightChannel selects the stored channel to read, with red as the default.  Heights are
normalised to the range 0 to 1 and resampled without colour-space conversion.

Technically the ImageFX object is represented by a new viewport, the bounds of which are defined by attributes `X`,
`Y`, `Width` and `Height`.  The placement and scaling of the referenced image is controlled by the #AspectRatio field.

-END-

*********************************************************************************************************************/

class extImageFX : public extFilterEffect {
   public:
   static constexpr CLASSID CLASS_ID = CLASSID::IMAGEFX;
   static constexpr CSTRING CLASS_NAME = "ImageFX";
   using create = kt::Create<extImageFX>;

   ARF AspectRatio = ARF::X_MID|ARF::Y_MID|ARF::MEET; // Aspect ratio flags.
   VSM ResampleMethod = VSM::BILINEAR; // Resample method.
   objBitmap *Bitmap = nullptr; // Bitmap containing source image data.
   FOM OutputMode = FOM::IMAGE;
   MCH HeightChannel = MCH::RED;
   objImage *Image = nullptr;   // Origin image if loading a source file.
   bool BitmapPinned = false;  // True if the client's Bitmap holds a strong pin taken by Init()
   std::string ImageHref; // Document-relative path supplied by the SVG parser.

   extImageFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::PREVIOUS;
   }

   ~extImageFX() {
      if (BitmapPinned) { Bitmap->unpin(true); BitmapPinned = false; }
      if (Image) FreeResource(Image);
   }
};

static objImage * load_filter_image(const std::string_view &Path, FOM Mode)
{
   if (Mode IS FOM::HEIGHT) {
      return objImage::create::local(fl::Path(Path), fl::Flags(PCF::FORCE_ALPHA_32|PCF::RAW_SAMPLES));
   }
   return objImage::create::local(fl::Path(Path), fl::BitsPerPixel(32), fl::Flags(PCF::FORCE_ALPHA_32));
}

//********************************************************************************************************************
// Read one height sample.  Clamp the taps to the bitmap edges without blending in zero at the boundary.

static double image_height_sample(objBitmap &Bitmap, MCH Channel, int X, int Y)
{
   const int x = std::clamp(X, 0, Bitmap.Width - 1);
   const int y = std::clamp(Y, 0, Bitmap.Height - 1);
   if ((Bitmap.Flags & BMF::GREYSCALE) != BMF::NIL) {
      uint16_t sample;
      std::memcpy(&sample, Bitmap.Data + std::ptrdiff_t(y) * Bitmap.LineWidth + std::ptrdiff_t(x) * 2, sizeof(sample));
      return sample * (1.0 / 65535.0);
   }

   RGB8 pixel {};
   Bitmap.ReadUCRPixel(&Bitmap, x, y, &pixel);
   switch (Channel) {
      case MCH::GREEN: return pixel.Green * (1.0 / 255.0);
      case MCH::BLUE:  return pixel.Blue * (1.0 / 255.0);
      case MCH::ALPHA: return (Bitmap.Flags & BMF::ALPHA_CHANNEL) != BMF::NIL ? pixel.Alpha * (1.0 / 255.0) : 1.0;
      case MCH::LUMINANCE:
         return (0.2126 * pixel.Red + 0.7152 * pixel.Green + 0.0722 * pixel.Blue) * (1.0 / 255.0);
      default: return pixel.Red * (1.0 / 255.0);
   }
}

//********************************************************************************************************************

static ERR draw_image_height(extImageFX *Self)
{
   auto &bitmap = *Self->Bitmap;
   agg::trans_affine forward, inverse;
   if (auto error = filter_image_transform(Self, bitmap, Self->AspectRatio, forward, inverse); error != ERR::Okay) {
      return (error IS ERR::NothingDone) ? ERR::Okay : error;
   }

   filter_surface_view layout;
   float *data;
   if (auto error = get_numeric_target(Self, layout, data); error != ERR::Okay) return error;

   TClipRectangle<double> bounds { 0, 0, double(bitmap.Width), double(bitmap.Height) };
   if (not transform_filter_region(bounds, forward)) return ERR::OutOfRange;
   const auto &clip = layout.Valid;
   const int left = int(std::clamp(std::floor(bounds.left), double(clip.left), double(clip.right)));
   const int right = int(std::clamp(std::ceil(bounds.right), double(clip.left), double(clip.right)));
   const int top = int(std::clamp(std::floor(bounds.top), double(clip.top), double(clip.bottom)));
   const int bottom = int(std::clamp(std::ceil(bounds.bottom), double(clip.top), double(clip.bottom)));

   const auto method = Self->ResampleMethod;
   const bool nearest = method IS VSM::NEIGHBOUR;
   const bool bilinear = (method IS VSM::BILINEAR) or (method IS VSM::AUTO);
   const auto kernel = (nearest or bilinear) ? nullptr : &get_filter(method, inverse);
   auto tap = [&](int X, int Y) { return image_height_sample(bitmap, Self->HeightChannel, X, Y); };

   for (int y = top; y < bottom; y++) {
      auto row = data + std::ptrdiff_t(y) * (layout.Stride / sizeof(float));
      for (int x = left; x < right; x++) {
         double sx, sy;
         map_filter_pixel_centre(inverse, x, y, sx, sy);
         // Pixels outside the image (including letterboxing) retain the surface's invalid sample.
         if (not ((sx >= 0) and (sx < bitmap.Width) and (sy >= 0) and (sy < bitmap.Height))) continue;
         if (nearest) {
            row[x] = float(tap(int(sx), int(sy)));
            continue;
         }

         sx -= 0.5;
         sy -= 0.5;
         const int ix = int(std::floor(sx)), iy = int(std::floor(sy));
         const double fx = sx - ix, fy = sy - iy;
         if (bilinear) {
            if ((fx IS 0.0) and (fy IS 0.0)) {
               row[x] = float(tap(ix, iy));
               continue;
            }
            const double a = std::lerp(tap(ix, iy), tap(ix + 1, iy), fx);
            const double b = std::lerp(tap(ix, iy + 1), tap(ix + 1, iy + 1), fx);
            row[x] = float(std::lerp(a, b, fy));
         }
         else {
            // Use the image kernel weights, accumulating the height itself without integer rounding.
            const auto weights = kernel->weight_array();
            const int start = kernel->start(), diameter = kernel->diameter();
            constexpr int subpixel_scale = agg::image_subpixel_scale;
            const int phase_x = agg::image_subpixel_mask - int(fx * subpixel_scale);
            const int phase_y = agg::image_subpixel_mask - int(fy * subpixel_scale);
            double height = 0;
            for (int ky = 0; ky < diameter; ky++) {
               const double wy = weights[phase_y + ky * subpixel_scale];
               for (int kx = 0; kx < diameter; kx++) {
                  const double wx = weights[phase_x + kx * subpixel_scale];
                  height += wx * wy * tap(ix + start + kx, iy + start + ky);
               }
            }
            constexpr double scale = agg::image_filter_scale;
            row[x] = float(std::clamp(height / (scale * scale), 0.0, 1.0));
         }
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Render the effect to the target bitmap.
-END-
*********************************************************************************************************************/

static ERR IMAGEFX_Draw(extImageFX *Self, struct acDraw *Args)
{
   if (not active_filter_step(Self)) return ERR::InvalidState;
   if (Self->OutputMode IS FOM::HEIGHT) return draw_image_height(Self);
   return render_to_filter(Self, Self->Bitmap, Self->AspectRatio, Self->ResampleMethod);
}


//********************************************************************************************************************

static ERR IMAGEFX_Init(extImageFX *Self)
{
   kt::Log log;

   if (not Self->Bitmap) return log.warning(ERR::FieldNotSet);

   if (not Self->Bitmap->initialised()) {
      if (Self->BitmapPinned) { Self->Bitmap->unpin(); Self->BitmapPinned = false; }
      return log.warning(ERR::NotInitialised);
   }

   if (Self->OutputMode IS FOM::HEIGHT) {
      if (not (Self->Bitmap->Data and Self->Bitmap->ReadUCRPixel)) return log.warning(ERR::InvalidValue);
   }
   else if (Self->Bitmap->BytesPerPixel != 4) {
      log.warning("The source bitmap #%d has %d bytes per pixel; 4 are required.", Self->Bitmap->UID,
         Self->Bitmap->BytesPerPixel);
      return ERR::InvalidValue;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// If the client attaches a bitmap as a child of our object, we use it as the primary image source.

static ERR IMAGEFX_NewChild(extImageFX *Self, struct acNewChild *Args)
{
   kt::Log log;
   if (Args->Object->classID() IS CLASSID::BITMAP) {
      if ((Self->initialised()) or (Self->Bitmap)) log.warning(ERR::AlreadyDefined);
      else if (Self->Bitmap) log.warning(ERR::AlreadyDefined);
      else {
         Self->Bitmap = (objBitmap *)Args->Object;
         Self->Bitmap->pin();
         Self->BitmapPinned = true;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AspectRatio: SVG compliant aspect ratio settings.
Lookup: ARF

*********************************************************************************************************************/

static ERR IMAGEFX_SET_AspectRatio(extImageFX *Self, ARF Value)
{
   Self->AspectRatio = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Bitmap: The @Bitmap being used as the image source.

Reading the Bitmap field will return the @Bitmap that is being used as the image source.  A custom Bitmap is
assigned by making the ImageFX object its owner prior to initialisation, as described in the class overview.

If an image has been processed by setting the #Path, the Bitmap will refer to the content that has been
processed.

-FIELD-
HeightChannel: Selects the stored channel to read in height mode.
Lookup: MCH

A 16-bit greyscale bitmap supplies its grey samples regardless of this setting.  For other bitmaps, alpha reads
as 1 when the bitmap has no alpha channel.

*********************************************************************************************************************/

static ERR IMAGEFX_SET_HeightChannel(extImageFX *Self, MCH Value)
{
   if ((Value < MCH::RED) or (Value > MCH::LUMINANCE)) return ERR::InvalidValue;
   Self->HeightChannel = Value;
   effect_modified(Self);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR IMAGEFX_SET_ImageHref(extImageFX *Self, const std::string_view &Value)
{
   Self->ImageHref = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
OutputMode: Selects image or floating-point height output.
Lookup: FOM

*********************************************************************************************************************/

static ERR IMAGEFX_SET_OutputMode(extImageFX *Self, FOM Value)
{
   if ((Value != FOM::IMAGE) and (Value != FOM::HEIGHT)) return ERR::InvalidValue;
   if (Self->initialised()) return ERR::Immutable;
   if (Value IS Self->OutputMode) return ERR::Okay;

   // Path and OutputMode may be supplied in either order during construction.

   if (Self->Image) {
      std::string_view path;
      if (auto error = Self->Image->getPath(path); error != ERR::Okay) return error;
      auto image = load_filter_image(path, Value);
      if (not image) return ERR::CreateObject;
      FreeResource(Self->Image);
      Self->Image = image;
      Self->Bitmap = image->Bitmap;
   }

   Self->OutputMode = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Path to an image file supported by the @Image class.

Set this field before initialisation.  SVG export requires a path relative to the document folder.

*********************************************************************************************************************/

static ERR IMAGEFX_GET_Path(extImageFX *Self, std::string_view &Value)
{
   if (Self->Image) return Self->Image->getPath(Value);
   else Value = std::string_view{};
   return ERR::Okay;
}

static ERR IMAGEFX_SET_Path(extImageFX *Self, const std::string_view &Value)
{
   if ((Self->Bitmap) or (Self->Image)) return ERR::Immutable;

   if ((Self->Image = load_filter_image(Value, Self->OutputMode))) {
      Self->Bitmap = Self->Image->Bitmap;
      return ERR::Okay;
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-FIELD-
ResampleMethod: The resample algorithm to use for transforming the source image.

!VSM

*********************************************************************************************************************/

static ERR IMAGEFX_SET_ResampleMethod(extImageFX *Self, VSM Value)
{
   Self->ResampleMethod = Value;
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
XMLDef: Describes the primitive using the @FilterEffect.XMLDef format.
-END-

*********************************************************************************************************************/

static ERR image_xml(extImageFX &Effect, filter_xml_element &Element)
{
   std::string_view reference = Effect.ImageHref;
   if (reference.empty()) {
      if (auto error = IMAGEFX_GET_Path(&Effect, reference); error != ERR::Okay) return error;
   }
   if ((not safe_filter_path(reference)) or reference.starts_with('#')) return ERR::NoSupport;

   Element.Name = "feImage";
   Element.attribute("xlink:href", reference);
   write_filter_xml_aspect(Element, Effect.AspectRatio);
   if (Effect.ResampleMethod IS VSM::LANCZOS) Element.attribute("image-rendering", "optimizeQuality");
   else write_filter_xml_resampling(Element, Effect.ResampleMethod);
   if (Effect.OutputMode IS FOM::HEIGHT) Element.attribute("kotuku:outputMode", "height");
   write_filter_xml_channel(Element, "kotuku:heightChannel", Effect.HeightChannel);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_image_def.c"

// Keep the private field after the generated ImageFX accessors' indexed fields in the dictionary.
static_assert(kt::fieldhash("ImageHref") > kt::fieldhash("AspectRatio"));

//********************************************************************************************************************

static const FieldArray clImageFXFields[] = {
   { "AspectRatio",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, IMAGEFX_SET_AspectRatio, &clAspectRatio },
   { "ResampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, IMAGEFX_SET_ResampleMethod, &clImageFXVSM },
   { "Bitmap",         FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::BITMAP },
   { "OutputMode",     FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, IMAGEFX_SET_OutputMode, &clImageFXFOM },
   { "HeightChannel",  FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, IMAGEFX_SET_HeightChannel, &clImageFXMCH },
   { "Path",           FDF_VIRTUAL|FDF_CPPSTRING|FDF_RI, IMAGEFX_GET_Path, IMAGEFX_SET_Path },
   { "XMLDef",         FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   { "ImageHref",      FDF_SYSTEM|FDF_VIRTUAL|FDF_CPPSTRING|FDF_W|FDF_PURE, nullptr, IMAGEFX_SET_ImageHref },
   END_FIELD
};

//********************************************************************************************************************

ERR init_imagefx(void)
{
   clImageFX = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::FILTEREFFECT),
      fl::ClassID(CLASSID::IMAGEFX),
      fl::Name("ImageFX"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clImageFXActions),
      fl::Fields(clImageFXFields),
      fl::Size(sizeof(extImageFX)),
      fl::Path(MOD_PATH));

   return clImageFX ? ERR::Okay : ERR::AddClass;
}
