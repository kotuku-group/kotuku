/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
ImageFX: Renders a bitmap image in the effect pipeline.

The ImageFX class will render a source image into a given rectangle within the current user coordinate system.  The
client has the option of providing a pre-allocated @Bitmap or the path to a @Image file as the source.

A pre-allocated bitmap is assigned to the #Bitmap field, or it can be created as a child of the uninitialised ImageFX.
The bitmap must be initialised before it is assigned, or before the ImageFX is initialised in the case of a child.
The default `IMAGE` mode requires 32 bits per pixel with an alpha channel.  The ImageFX keeps the bitmap alive
until it is replaced or the ImageFX is freed; an attempt to free it earlier is deferred.  Source images loaded from a
#Path are converted to the required format automatically.

The source can be replaced at any time, including after initialisation.  Setting #Path loads a new image file, and
setting #Bitmap binds a different bitmap.  A replacement that fails, for instance because the file cannot be read or
the current #ViewBox does not fit inside the new image, leaves the previous source active.  Use #Reload() to read
the file at #Path again after it has been modified on disk.

Changes to the pixels of a bound bitmap are not detected automatically.  Call Refresh() on the ImageFX after editing
the bitmap in place, so that buffered viewports that use the filter are redrawn.

Set #ViewBox to render a section of the bitmap, such as one cell of an image atlas.  The selected pixels are
fitted to the primitive subregion according to #AspectRatio, and resampling clamps to the edges of the section so
that neighbouring pixels do not bleed into the result.

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
   bool BitmapPinned = false;  // True for a pinned child or external bitmap.
   view_box ViewBox { 0, 0, 0, 0 }; // Optional crop in bitmap pixels; zero size selects the whole bitmap.
   std::string ImageHref; // Document-relative path supplied by the SVG parser.

   extImageFX(objMetaClass *ClassPtr, OBJECTID ObjectID) noexcept : extFilterEffect(ClassPtr, ObjectID) {
      SourceType = VSF::PREVIOUS;
   }

   ~extImageFX() {
      if (BitmapPinned) { Bitmap->unpin(true); BitmapPinned = false; }
      if (Image) FreeResource(Image);
   }
};

//********************************************************************************************************************

static objImage * load_filter_image(const std::string_view &Path, FOM Mode)
{
   if (Mode IS FOM::HEIGHT) {
      return objImage::create::local(fl::Path(Path), fl::Flags(PCF::FORCE_ALPHA_32|PCF::RAW_SAMPLES));
   }
   return objImage::create::local(fl::Path(Path), fl::BitsPerPixel(32), fl::Flags(PCF::FORCE_ALPHA_32));
}

//********************************************************************************************************************
// Converts a view box to whole pixels within Bitmap.  An unset rectangle selects the entire bitmap.

static ERR image_crop(const view_box &Rect, const objBitmap &Bitmap, TClipRectangle<int> &Crop)
{
   if (not has_view_box(Rect)) {
      Crop = TClipRectangle<int>(0, 0, Bitmap.Width, Bitmap.Height);
      return ERR::Okay;
   }

   const double right = Rect[VB_X] + Rect[VB_WIDTH], bottom = Rect[VB_Y] + Rect[VB_HEIGHT];
   if ((Rect[VB_X] < 0) or (Rect[VB_Y] < 0) or (right > Bitmap.Width) or (bottom > Bitmap.Height)) {
      return ERR::OutOfRange;
   }

   Crop = TClipRectangle<int>(int(Rect[VB_X]), int(Rect[VB_Y]), int(right), int(bottom));
   return ERR::Okay;
}

//********************************************************************************************************************
// Checks that a bitmap can supply the active output mode and contains the current view box.

static ERR check_image_bitmap(extImageFX *Self, objBitmap *Bitmap, FOM Mode)
{
   kt::Log log(__FUNCTION__);

   if (not Bitmap->initialised()) return log.warning(ERR::NotInitialised);

   if ((Mode != FOM::HEIGHT) and (Bitmap->BytesPerPixel != 4)) {
      log.warning("The source bitmap #%d has %d bytes per pixel; 4 are required.", Bitmap->UID, Bitmap->BytesPerPixel);
      return ERR::InvalidValue;
   }

   TClipRectangle<int> crop;
   return image_crop(Self->ViewBox, *Bitmap, crop);
}

//********************************************************************************************************************
// Swaps in a new source.  Image is the owning image for a file source, or null for a client bitmap, which is pinned
// for as long as it remains the source.  The previous source is released after the swap.

static void replace_image_source(extImageFX *Self, objImage *Image, objBitmap *Bitmap)
{
   if (not Image) Bitmap->pin();
   auto old_bitmap = std::exchange(Self->Bitmap, Bitmap);
   auto old_pinned = std::exchange(Self->BitmapPinned, not Image);
   auto old_image = std::exchange(Self->Image, Image);

   effect_modified(Self);
   if (old_pinned) old_bitmap->unpin(true);
   if (old_image) FreeResource(old_image);
}

//********************************************************************************************************************

static ERR replace_image_file(extImageFX *Self, std::string_view Path, FOM Mode)
{
   auto image = load_filter_image(Path, Mode);
   if (not image) return ERR::CreateObject;

   if (auto error = check_image_bitmap(Self, image->Bitmap, Mode); error != ERR::Okay) {
      FreeResource(image);
      return error;
   }

   replace_image_source(Self, image, image->Bitmap);
   return ERR::Okay;
}

//********************************************************************************************************************
// Read one height sample.  The caller clamps taps to the selected view box.

static double image_height_sample(objBitmap &Bitmap, MCH Channel, int X, int Y)
{
   if ((Bitmap.Flags & BMF::GREYSCALE) != BMF::NIL) {
      uint16_t sample;
      std::memcpy(&sample, Bitmap.Data + std::ptrdiff_t(Y) * Bitmap.LineWidth + std::ptrdiff_t(X) * 2, sizeof(sample));
      return sample * (1.0 / 65535.0);
   }

   RGB8 pixel {};
   Bitmap.ReadUCRPixel(&Bitmap, X, Y, &pixel);
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

static ERR draw_image_height(extImageFX *Self, const TClipRectangle<int> &Crop)
{
   auto &bitmap = *Self->Bitmap;
   const int width = Crop.width(), height = Crop.height();
   agg::trans_affine forward, inverse;
   if (auto error = filter_image_transform(Self, width, height, Self->AspectRatio, forward, inverse);
       error != ERR::Okay) {
      return (error IS ERR::NothingDone) ? ERR::Okay : error;
   }

   filter_surface_view layout;
   float *data;
   if (auto error = get_numeric_target(Self, layout, data); error != ERR::Okay) return error;

   TClipRectangle<double> bounds { 0, 0, double(width), double(height) };
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
   auto tap = [&](int X, int Y) {
      return image_height_sample(bitmap, Self->HeightChannel,
         Crop.left + std::clamp(X, 0, width - 1), Crop.top + std::clamp(Y, 0, height - 1));
   };

   for (int y = top; y < bottom; y++) {
      auto row = data + std::ptrdiff_t(y) * (layout.Stride / sizeof(float));
      for (int x = left; x < right; x++) {
         double sx, sy;
         map_filter_pixel_centre(inverse, x, y, sx, sy);
         // Pixels outside the image (including letterboxing) retain the surface's invalid sample.
         if (not ((sx >= 0) and (sx < width) and (sy >= 0) and (sy < height))) continue;
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
   if (not Self->Bitmap) return ERR::Okay; // A missing source produces transparent pixels.

   ScopedObjectAccess access(Self->Bitmap);
   if (not access.granted()) return access.error;

   // The bitmap can be resized after the crop was validated, so the crop is checked on every draw.

   TClipRectangle<int> crop;
   if (auto error = image_crop(Self->ViewBox, *Self->Bitmap, crop); error != ERR::Okay) return error;

   if (Self->OutputMode IS FOM::HEIGHT) return draw_image_height(Self, crop);
   const bool cropped = has_view_box(Self->ViewBox);
   return render_to_filter(Self, Self->Bitmap, Self->AspectRatio, Self->ResampleMethod, cropped ? &crop : nullptr);
}

//********************************************************************************************************************
// A source is optional at this stage; one can be assigned to Bitmap or Path after initialisation.  A child
// bitmap is only validated here because it is attached before its own initialisation.

static ERR IMAGEFX_Init(extImageFX *Self)
{
   if (not Self->Bitmap) return ERR::Okay;

   if (auto error = check_image_bitmap(Self, Self->Bitmap, Self->OutputMode); error != ERR::Okay) {
      if (Self->BitmapPinned) { Self->Bitmap->unpin(); Self->BitmapPinned = false; }
      Self->Bitmap = nullptr;
      return error;
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

Controls how the source image, or the section of it selected by #ViewBox, is aligned and scaled to fit the
primitive subregion.  The default is `X_MID|Y_MID|MEET`.

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

Reading the Bitmap field returns the @Bitmap that is being used as the image source.  If an image has been loaded
from #Path, the Bitmap refers to the content of that image.

Writing to the Bitmap field replaces the current source with a client-supplied @Bitmap.  The field can be set before
or after initialisation, and buffered viewports that use the filter are redrawn on their next draw.  Any image
previously loaded from #Path is released, and #Path reads as empty from then on.

The ImageFX pins the bitmap so that it remains valid while it is the source.  An attempt to free the bitmap in the
meantime is deferred until it is replaced or the ImageFX is freed.  The ImageFX does not detect changes to the
bitmap's pixels; call Refresh() after editing them.

*********************************************************************************************************************/

static ERR IMAGEFX_SET_Bitmap(extImageFX *Self, objBitmap *Value)
{
   if (not Value) return ERR::InvalidValue;
   if (Value->classID() != CLASSID::BITMAP) return ERR::WrongClass;
   if (Value IS Self->Bitmap) return ERR::Okay;
   if (auto error = check_image_bitmap(Self, Value, Self->OutputMode); error != ERR::Okay) return error;

   replace_image_source(Self, nullptr, Value);
   Self->ImageHref.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

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

The default `IMAGE` mode renders the source as colour.  The `HEIGHT` mode produces a floating-point map from the
channel selected by #HeightChannel, for consumption by effects such as @LightingFX.  The mode can only be set before
initialisation.  If an image has already been loaded from #Path, it is loaded again in the format that the new mode
requires.

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
      if (auto error = replace_image_file(Self, path, Value); error != ERR::Okay) return error;
   }

   Self->OutputMode = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Path to an image file supported by the @Image class.

Setting the Path loads the referenced file as the image source, replacing any existing source.  The path can be set
before or after initialisation.  The image is converted to the format that #OutputMode requires.

Reading the Path returns an empty string if the source is a client bitmap.  SVG export requires a path relative to
the document folder.

*********************************************************************************************************************/

static ERR IMAGEFX_GET_Path(extImageFX *Self, std::string_view &Value)
{
   if (Self->Image) return Self->Image->getPath(Value);
   else Value = std::string_view{};
   return ERR::Okay;
}

static ERR IMAGEFX_SET_Path(extImageFX *Self, const std::string_view &Value)
{
   if (Value.empty()) return ERR::InvalidValue;
   if (auto error = replace_image_file(Self, Value, Self->OutputMode); error != ERR::Okay) return error;
   Self->ImageHref.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Reload: Reloads the image file referenced by #Path.

Reload reads the file at #Path again and replaces the current image with its new content.  Use it when the file has
been modified since it was loaded.  The #ViewBox, #OutputMode and the document-relative reference used for SVG
export are retained.

If the file cannot be loaded, or the #ViewBox does not fit inside the new image, the previous image remains active.

-RESULT-
Okay:
FieldNotSet: The source is not a file, either because #Path has not been set or because a bitmap was assigned to #Bitmap.
CreateObject: The file could not be loaded.
InvalidValue: The image format is not supported by the #OutputMode.
OutOfRange: The #ViewBox does not fit inside the reloaded image.

-END-

*********************************************************************************************************************/

static ERR IMAGEFX_Reload(extImageFX *Self, APTR Void)
{
   if (not Self->Image) return ERR::FieldNotSet;

   std::string_view path;
   if (auto error = Self->Image->getPath(path); error != ERR::Okay) return error;

   return replace_image_file(Self, path, Self->OutputMode);
}

/*********************************************************************************************************************

-ACTION-
Refresh: Redraws filter clients after the pixels of the source bitmap are changed.

The ImageFX cannot detect edits to the pixels of its source @Bitmap.  Call Refresh after editing them so that
buffered viewports that use the filter discard their cached content.  Changes to the fields of the ImageFX are
detected automatically and do not need a Refresh.

Refresh does not read the source file again; use #Reload() for that.  It also does not schedule drawing.
-END-
*********************************************************************************************************************/

static ERR IMAGEFX_Refresh(extImageFX *Self, APTR Void)
{
   effect_modified(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ViewBox: Selects a section of the source bitmap, in pixels.

By default the entire source bitmap is rendered.  Set ViewBox to an array of four values, `{ x, y, width, height }`,
to render only a section of it, such as one cell of an image atlas.  The section is fitted to the primitive subregion
according to #AspectRatio, as if it were the whole image.  Unlike the `viewBox` of an SVG viewport, the section is a
strict crop: resampling clamps to its edges, so pixels outside of it never contribute to the result.

All values must be whole numbers, the width and height must be greater than zero, and the rectangle must lie inside
the current source bitmap if one is set.  Otherwise the new value is rejected with `ERR::OutOfRange` and the current
rectangle is retained.  An array that does not have four values is rejected with `ERR::InvalidValue`.

Set an empty array, or a zero width and height, to clear the rectangle.  Reading the field returns an empty array
when no rectangle is set.

If the source bitmap shrinks after the rectangle is set so that it no longer fits, drawing the effect fails with
`ERR::OutOfRange`.

In SVG, the equivalent attribute is `kotuku:viewBox="x y width height"` on the `feImage` element.

*********************************************************************************************************************/

static ERR IMAGEFX_GET_ViewBox(extImageFX *Self, std::span<double> &Array)
{
   Array = view_box_field(Self->ViewBox);
   return ERR::Okay;
}

static ERR IMAGEFX_SET_ViewBox(extImageFX *Self, std::span<const double> &Array)
{
   view_box rect;
   if (auto error = read_view_box(Array, rect); error != ERR::Okay) return error;

   if (has_view_box(rect)) {
      for (double value : rect) {
         if (std::trunc(value) != value) return ERR::OutOfRange;
      }

      if (Self->Bitmap) {
         TClipRectangle<int> crop;
         if (auto error = image_crop(rect, *Self->Bitmap, crop); error != ERR::Okay) return error;
      }
   }

   Self->ViewBox = rect;
   effect_modified(Self);
   return ERR::Okay;
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
   write_filter_xml_view_box(Element, Effect.ViewBox);

   if (Effect.ResampleMethod IS VSM::LANCZOS) Element.attribute("image-rendering", "optimizeQuality");
   else write_filter_xml_resampling(Element, Effect.ResampleMethod);

   if (Effect.OutputMode IS FOM::HEIGHT) Element.attribute("kotuku:outputMode", "height");
   write_filter_xml_channel(Element, "kotuku:heightChannel", Effect.HeightChannel);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "filter_image_def.c"

static const FieldArray clImageFXFields[] = {
   { "AspectRatio",    FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, IMAGEFX_SET_AspectRatio, &clAspectRatio },
   { "ResampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, IMAGEFX_SET_ResampleMethod, &clImageFXVSM },
   { "Bitmap",         FDF_OBJECT|FDF_RW, nullptr, IMAGEFX_SET_Bitmap, CLASSID::BITMAP },
   { "OutputMode",     FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, IMAGEFX_SET_OutputMode, &clImageFXFOM },
   { "HeightChannel",  FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, IMAGEFX_SET_HeightChannel, &clImageFXMCH },
   { "Path",           FDF_VIRTUAL|FDF_CPPSTRING|FDF_RW, IMAGEFX_GET_Path, IMAGEFX_SET_Path },
   { "ViewBox",     FDF_VIRTUAL|FDF_ARRAY|FDF_DOUBLE|FDF_RW|FDF_PURE, IMAGEFX_GET_ViewBox, IMAGEFX_SET_ViewBox },
   { "XMLDef",         FDF_VIRTUAL|FDF_CPPSTRING|FDF_STORE|FDF_R|FDF_PURE, FILTEREFFECT_GET_XMLDef },
   { "ImageHref",      FDF_VIRTUAL|FDF_SYSTEM|FDF_CPPSTRING|FDF_W|FDF_PURE, nullptr, IMAGEFX_SET_ImageHref },
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
      fl::Methods(clImageFXMethods),
      fl::Fields(clImageFXFields),
      fl::Size(sizeof(extImageFX)),
      fl::Path(MOD_PATH));

   return clImageFX ? ERR::Okay : ERR::AddClass;
}
