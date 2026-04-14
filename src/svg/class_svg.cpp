/*********************************************************************************************************************

-CLASS-
SVG: Provides comprehensive support for parsing, rendering and animating SVG documents.

The SVG class serves as a complete solution for integrating Scalable Vector Graphics documents into applications.  It parses SVG statements into a scene graph consisting of @Vector objects and related constructs, providing direct programmatic access to all graphical elements.  The generated scene graph is accessible via the #Scene and #Viewport fields, enabling real-time manipulation of individual elements.

Key capabilities include:

<list type="bullet">
<li>W3C-compliant SVG parsing with support for advanced features including gradients, filters, and patterns</li>
<li>SMIL animation support with automatic frame-based playback</li>
<li>Dynamic scene graph manipulation for real-time graphics modification</li>
<li>Flexible rendering targets via the #Target field for integration with existing UI components</li>
<li>Symbol-based graphics with macro-like functionality through #ParseSymbol()</li>
<li>Resolution-independent scaling with automatic adaptation to display characteristics</li>
<li>Export capabilities to multiple formats including PNG images</li>
</list>

The class supports both file-based loading via #Path and direct string-based parsing via #Statement.  SVG documents can be integrated into existing scene graphs by setting the #Target field, or rendered independently through the automatically created scene structure.

Animation timing is controlled through the #FrameRate field, with callback support via #FrameCallback for custom rendering workflows.  The implementation maintains compatibility with the complete SVG specification while providing enhanced programmatic access unique to Kotuku.

Please refer to the W3C's online documentation for exhaustive information on the SVG standard.

*********************************************************************************************************************/

static void notify_free_frame_callback(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extSVG *)CurrentContext())->FrameCallback.clear();
}

//********************************************************************************************************************

static void notify_free_scene(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extSVG *)CurrentContext();
   if (Self->AnimationTimer) { UpdateTimer(Self->AnimationTimer, 0); Self->AnimationTimer = nullptr; }
}

/*********************************************************************************************************************
-ACTION-
Activate: Initiates playback of SVG animations.

SVG documents containing SMIL animation features will remain static until activated through this action.  Upon
activation, the animation system begins processing animation sequences in the background according to the configured
#FrameRate.  The #Scene will be automatically redrawn as each frame is computed, ensuring smooth visual transitions.

To integrate custom rendering logic with the animation cycle, configure the #FrameCallback field with an appropriate
function.  This callback will be triggered after each frame preparation, enabling applications to implement custom
rendering workflows or capture animation frames.

<b>Note:</b> If the SVG document contains no animation elements, this action completes successfully but has no visual
effect.

-END-
*********************************************************************************************************************/

static ERR SVG_Activate(extSVG *Self)
{
   if (!Self->Animations.empty()) {
      if (!Self->AnimationTimer) {
         SubscribeTimer(1.0 / (double)Self->FrameRate, C_FUNCTION(animation_timer), &Self->AnimationTimer);
         SubscribeAction(Self->Scene, AC::Free, C_FUNCTION(notify_free_scene));
      }
      else UpdateTimer(Self->AnimationTimer, 1.0 / (double)Self->FrameRate);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Deactivate: Halts all SVG animation playback and suspends frame processing.

This action immediately terminates any active animation playback, stopping all animation timers and suspending frame processing.  The SVG document will remain visible in its current state, but no further animation updates will occur until the object is reactivated.

The deactivation process is immediate and does not affect the underlying scene graph structure.  Animation sequences can be resumed from their current positions by calling the #Activate() action again.

This action is particularly useful for implementing pause functionality or conserving system resources when animations are not required.

-END-
*********************************************************************************************************************/

static ERR SVG_Deactivate(extSVG *Self)
{
   if (Self->AnimationTimer) { UpdateTimer(Self->AnimationTimer, 0); Self->AnimationTimer = 0; }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Processes SVG data streams for incremental document parsing.

The DataFeed action enables real-time processing of SVG data streams, allowing documents to be parsed incrementally as data becomes available.  This is particularly useful for network-based loading scenarios or when processing large SVG documents that may arrive in segments.

The action accepts XML data streams and integrates them into the existing document structure.  Multiple DataFeed calls can be made to build up complex SVG documents progressively.

<b>Supported data types:</b> `DATA::XML` for SVG content streams.

This mechanism provides an alternative to the static #Statement field for scenarios requiring dynamic content loading or streaming workflows.

-END-
*********************************************************************************************************************/

static ERR SVG_DataFeed(extSVG *Self, struct acDataFeed *Args)
{
   if (!Args) return ERR::NullArgs;

   if (Args->Datatype IS DATA::XML) {
      return parse_svg(Self, 0, (CSTRING)Args->Buffer);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SVG_Free(extSVG *Self)
{
   if (Self->AnimationTimer) {
      UpdateTimer(Self->AnimationTimer, 0);
      if (Self->Scene) UnsubscribeAction(Self->Scene, AC::Free);
      Self->AnimationTimer = 0;
   }

   if (Self->FrameCallback.isScript()) {
      UnsubscribeAction(Self->FrameCallback.Context, AC::Free);
      Self->FrameCallback.clear();
   }

   if ((Self->Target) and (Self->Target IS Self->Scene) and (Self->Scene->Owner IS Self)) {
      FreeResource(Self->Target);
      Self->Target = nullptr;
   }

   if (Self->Path)      { FreeResource(Self->Path);      Self->Path = nullptr; }
   if (Self->Title)     { FreeResource(Self->Title);     Self->Title = nullptr; }
   if (Self->Statement) { FreeResource(Self->Statement); Self->Statement = nullptr; }
   if (Self->XML)       { FreeResource(Self->XML);       Self->XML = nullptr; }

   if (!Self->Resources.empty()) {
      for (auto id : Self->Resources) FreeResource(id);
   }

   Self->~extSVG();

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Init: Initialises the SVG object and processes source content.

The initialisation process establishes the scene graph structure and processes any specified SVG source content.  If a #Path has been configured, the referenced SVG file will be loaded and parsed immediately.  Alternatively, if #Statement contains SVG data, that content will be processed instead.

The default behaviour creates a local @VectorScene object to contain the generated scene graph.  This can be overridden by setting the #Target field to redirect content into an existing scene graph structure, enabling integration with existing UI components.

The initialisation sequence includes:

<list type="ordered">
<li>Scene graph structure creation or validation of the specified #Target</li>
<li>SVG document parsing and scene graph population</li>
<li>Resolution of SVG references, definitions, and symbol libraries</li>
<li>Animation sequence preparation for documents containing SMIL features</li>
</list>

Successfully initialised SVG objects provide immediate access to the generated scene graph via the #Scene and #Viewport fields, enabling programmatic manipulation of individual graphic elements.

-END-
*********************************************************************************************************************/

static ERR SVG_Init(extSVG *Self)
{
   if (!Self->Target) {
      if ((Self->Target = objVectorScene::create::local())) {
         Self->Scene = (objVectorScene *)Self->Target;
      }
      else return ERR::NewObject;
   }

   if (Self->Path) return parse_svg(Self, Self->Path, nullptr);
   else if (Self->Statement) return parse_svg(Self, nullptr, Self->Statement);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SVG_NewPlacement(extSVG *Self)
{
   new (Self) extSVG;
   #ifdef __ANDROID__
      Self->FrameRate = 30; // Choose a lower frame rate for Android devices, so as to minimise power consumption.
   #else
      Self->FrameRate = 60;
   #endif
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ParseSymbol: Instantiates an SVG symbol definition within a target viewport.

ParseSymbol() enables dynamic instantiation of SVG symbol definitions that were declared within the loaded document.
This method provides macro-like functionality, allowing complex graphical elements to be replicated and positioned as
needed throughout the application.  This approach promotes efficient memory usage and consistent visual design while
enabling dynamic scene graph construction.

The specified `ID` must correspond to a symbol element that exists within the current document's definition library.
The generated content will be structured within the provided @VectorViewport, which must be part of an established
scene graph.

-INPUT-
cstr ID: Name of the symbol to parse.
obj(VectorViewport) Viewport: The target viewport.

-RESULT-
Okay: Symbol successfully parsed and instantiated.
NullArgs: Required parameters were not provided.
NotFound: The specified symbol ID does not exist in the document.
-END-

*********************************************************************************************************************/

static ERR SVG_ParseSymbol(extSVG *Self, struct svg::ParseSymbol *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->ID) or (!Args->Viewport)) return log.warning(ERR::NullArgs);

   if (auto tagref = find_href_tag(Self, Args->ID)) {
      svgState state(Self);
      state.process_children(*tagref, Args->Viewport);
      return ERR::Okay;
   }
   else {
      log.warning("Symbol '%s' not found.", Args->ID);
      return ERR::NotFound;
   }
}

/*********************************************************************************************************************

-METHOD-
Render: Performs high-quality rasterisation of the SVG document to a target bitmap.

This method executes complete rasterisation of the SVG scene graph, producing a pixel-based representation within the specified target bitmap.  The rendering process handles all vector elements, gradients, filters, and effects with full anti-aliasing and precision.

The rendered output is positioned at coordinates `(X,Y)` within the target bitmap and scaled to the specified `(Width,Height)` dimensions.  The scaling operation maintains aspect ratios and applies appropriate filtering to ensure optimal visual quality.

The scene's page dimensions are temporarily adjusted to match the specified width and height, ensuring that the entire document content is properly scaled and positioned within the target area.  This approach enables flexible rendering at arbitrary resolutions without affecting the original scene graph.

<b>Performance considerations:</b> Rendering complex SVG documents with multiple effects and high resolutions may require significant processing time.  Consider using appropriate dimensions that balance quality requirements with performance constraints.

-INPUT-
obj(Bitmap) Bitmap: The target bitmap object to receive the rendered content.
int X: Horizontal position within the target bitmap.
int Y: Vertical position within the target bitmap.
int Width: Desired width of the rendered output in pixels.
int Height: Desired height of the rendered output in pixels.

-RESULT-
Okay: Rendering completed successfully.
NullArgs: Required bitmap parameter was not provided.
-END-

*********************************************************************************************************************/

static ERR SVG_Render(extSVG *Self, struct svg::Render *Args)
{
   if (!Args) return ERR::NullArgs;

   objBitmap *bmp = Args->Bitmap;
   int page_width = Args->Width;
   int page_height = Args->Height;

   Self->Scene->setBitmap(bmp);

   Self->Scene->setPageWidth(page_width);
   Self->Scene->setPageHeight(page_height);

//   Self->Scene->Viewport->setViewX(Args->X);
//   Self->Scene->Viewport->setViewY(Args->Y);

   auto data = bmp->offset(Args->X, Args->Y);
   Action(AC::Draw, Self->Scene, nullptr);
   bmp->Data = data;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveImage: Saves the SVG document as a PNG image.

This action will render the SVG document to a bitmap and save the resulting image.  The size of the image is
determined by the @VectorScene.PageWidth and @VectorScene.PageHeight of the #Scene, or if not defined, the
default of 1920x1080 is applied.

The image will be saved in PNG format by default, but can be changed by specifying an alternate `ClassID`.  PNG
is recommended in particular because it supports an alpha channel.

-END-
*********************************************************************************************************************/

static ERR SVG_SaveImage(extSVG *Self, struct acSaveImage *Args)
{
   ERR error;

   if (!Args) return ERR::NullArgs;

   int width = 0;
   int height = 0;
   Self->Scene->get(FID_PageWidth, width);
   Self->Scene->get(FID_PageHeight, height);

   if (!width) width = 1920;
   if (!height) height = 1080;

   auto pic = objPicture::create { fl::Width(width), fl::Height(height), fl::Flags(PCF::ALPHA|PCF::NEW) };
   if (pic.ok()) {
      if ((error = Self->render(pic->Bitmap, 0, 0, width, height)) IS ERR::Okay) {
         if ((error = acSaveImage(*pic, Args->Dest, Args->ClassID)) IS ERR::Okay) {
            return ERR::Okay;
         }
      }
   }
   else error = ERR::CreateObject;

   return error;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Saves the SVG document to a data object.
-END-
*********************************************************************************************************************/

static ERR SVG_SaveToObject(extSVG *Self, struct acSaveToObject *Args)
{
   pf::Log log;
   static const char header[] =
"<?xml version=\"1.0\" standalone=\"no\"?>\n\
<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\n";
   ERR (**actions)(OBJECTPTR, APTR);

   if (!Self->Viewport) return log.warning(ERR::NoData);

   if ((Args->ClassID != CLASSID::NIL) and (Args->ClassID != CLASSID::SVG)) {
      auto mc = (objMetaClass *)FindClass(Args->ClassID);
      if ((mc->get(FID_ActionTable, actions) IS ERR::Okay) and (actions)) {
         if ((actions[int(AC::SaveToObject)]) and (actions[int(AC::SaveToObject)] != (APTR)SVG_SaveToObject)) {
            return actions[int(AC::SaveToObject)](Self, Args);
         }
         else if ((actions[int(AC::SaveImage)]) and (actions[int(AC::SaveImage)] != (APTR)SVG_SaveImage)) {
            struct acSaveImage saveimage = { .Dest = Args->Dest };
            return actions[int(AC::SaveImage)](Self, &saveimage);
         }
         else return log.warning(ERR::NoSupport);
      }
      else return log.warning(ERR::GetField);
   }
   else {
      auto xml = objXML::create { fl::Flags(XMF::NEW|XMF::READABLE) };

      if (xml.ok()) {
         Self->XML = *xml;

         ERR error = xml->insertXML(0, XMI::NIL, header, nullptr);
         int index = xml->Tags.back().ID;

         XTag *tag;
         if ((error = xml->insertStatement(index, XMI::NEXT, "<svg version=\"1.1\" xmlns=\"http://www.w3.org/2000/svg\" xmlns:kotuku=\"http://www.kotuku.dev/xmlns/svg\"/>", &tag)) IS ERR::Okay) {
            bool multiple_viewports = (Self->Scene->Viewport->Next) ? true : false;
            if (multiple_viewports) {
               if ((error = save_svg_defs(Self, *xml, Self->Scene, index)) IS ERR::Okay) {
                  for (auto scan=Self->Scene->Viewport; scan; scan=(objVectorViewport *)scan->Next) {
                     if (!scan->Child) continue; // Ignore dummy viewports with no content
                     save_svg_scan(Self, *xml, scan, index);
                  }

                  error = xml->saveToObject(Args->Dest);
               }
            }
            else {
               double x, y, width, height;

               if (error IS ERR::Okay) error = Self->Viewport->get(FID_ViewX, x);
               if (error IS ERR::Okay) error = Self->Viewport->get(FID_ViewY, y);
               if (error IS ERR::Okay) error = Self->Viewport->get(FID_ViewWidth, width);
               if (error IS ERR::Okay) error = Self->Viewport->get(FID_ViewHeight, height);

               if (error IS ERR::Okay) {
                  char buffer[80];
                  snprintf(buffer, sizeof(buffer), "%g %g %g %g", x, y, width, height);
                  xml::NewAttrib(tag, "viewBox", buffer);
               }

               if (error IS ERR::Okay) {
                  auto dim = Self->Viewport->get<DMF>(FID_Dimensions);
                  if (dmf::hasAnyX(dim) and (Self->Viewport->get(FID_X, x) IS ERR::Okay))
                     set_dimension(tag, "x", x, dmf::hasScaledX(dim));

                  if (dmf::hasAnyY(dim) and (Self->Viewport->get(FID_Y, y) IS ERR::Okay))
                     set_dimension(tag, "y", y, dmf::hasScaledY(dim));

                  if (dmf::hasAnyWidth(dim) and (Self->Viewport->get(FID_Width, width) IS ERR::Okay))
                     set_dimension(tag, "width", width, dmf::hasScaledWidth(dim));

                  if (dmf::hasAnyHeight(dim) and (Self->Viewport->get(FID_Height, height) IS ERR::Okay))
                     set_dimension(tag, "height", height, dmf::hasScaledHeight(dim));
               }

               if (error IS ERR::Okay) {
                  if ((error = save_svg_defs(Self, *xml, Self->Scene, index)) IS ERR::Okay) {
                     for (auto scan=((objVector *)Self->Viewport)->Child; scan; scan=scan->Next) {
                        save_svg_scan(Self, *xml, scan, index);
                     }

                     error = xml->saveToObject(Args->Dest);
                  }
               }
            }

            Self->XML = nullptr;
         }

         return error;
      }
      else return ERR::CreateObject;
   }
}

/*********************************************************************************************************************

-FIELD-
Colour: Defines the default fill to use for `currentColor` references.

Set the Colour value to alter the default fill that is used for `currentColor` references.  Typically a standard RGB
painter fill reference should be used for this purpose, e.g. `rgb(255,255,255)`.  It is however, also acceptable to use
URL references to named definitions such as gradients and images.  This will work as long as the named definition is
registered in the top-level @VectorScene object.

<b>Supported formats:</b>
<list type="bullet">
<li>RGB values: `rgb(red, green, blue)`</li>
<li>Hexadecimal notation: `#RRGGBB` or `#RGB`</li>
<li>Named colours: Standard SVG colour names</li>
<li>URL references: `url(#gradientId)` for complex paint definitions</li>
</list>
*********************************************************************************************************************/

static ERR GET_Colour(extSVG *Self, CSTRING *Value)
{
   *Value = Self->Colour.c_str();
   return ERR::Okay;
}

static ERR SET_Colour(extSVG *Self, CSTRING Value)
{
   if ((Value) and (*Value)) {
      Self->Colour.assign(Value);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Configuration flags that modify SVG processing behaviour.
Lookup: SVF

-FIELD-
Frame: Constrains rendering to a specific frame number for frame-based display systems.

This field enables frame-synchronised rendering by restricting graphics display to specific frame numbers within
frame-based container systems.  When set to a non-zero value, the SVG content will only be rendered when the
container's current frame matches this field's value.

The default value of 0 disables frame-based filtering, allowing the SVG content to be rendered continuously regardless
of the container's frame state.

-FIELD-
FrameCallback: Function callback executed after each animation frame preparation.

This field enables integration of custom logic into the animation processing pipeline by specifying a callback
function that executes after each animation frame is computed.  The callback mechanism provides precise timing for
implementing custom rendering workflows, frame capture systems, or animation synchronisation logic.

The callback function receives a pointer to the SVG object, enabling access to the current scene state and rendering
control.  This is commonly used for rendering the animated SVG content to target bitmaps, implementing video capture,
or synchronising with external animation systems.

<b>Timing behaviour:</b> The callback executes immediately after frame preparation but before automatic scene
redrawing, ensuring that custom logic can modify or capture the scene state at the optimal moment.

<b>Animation dependency:</b> Callbacks are only triggered for SVG documents containing SMIL animation features.
Static documents will not invoke the callback function.

<b>Function prototype:</b> `void Function(*SVG)`

*********************************************************************************************************************/

static ERR GET_FrameCallback(extSVG *Self, FUNCTION **Value)
{
   if (Self->FrameCallback.defined()) {
      *Value = &Self->FrameCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_FrameCallback(extSVG *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->FrameCallback.isScript()) UnsubscribeAction(Self->FrameCallback.Context, AC::Free);
      Self->FrameCallback = *Value;
      if (Self->FrameCallback.isScript()) {
         SubscribeAction(Self->FrameCallback.Context, AC::Free, C_FUNCTION(notify_free_frame_callback));
      }
   }
   else Self->FrameCallback.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FrameRate: Controls the maximum frame rate for SVG animation playback.

This field establishes the upper limit for animation frame processing, measured in frames per second.  The frame rate directly impacts animation smoothness and system resource consumption, requiring careful balance between visual quality and performance efficiency.

<b>Recommended ranges:</b>
<list type="bullet">
<li><b>Standard displays:</b> 60 FPS matches most modern display refresh rates</li>
<li><b>Balanced performance:</b> 30-50 FPS provides smooth animation with moderate resource usage</li>
<li><b>Low-power devices:</b> 20-30 FPS conserves battery while maintaining acceptable quality</li>
</list>

<b>Performance considerations:</b> Higher frame rates increase CPU usage proportionately.  A frame rate of 100 FPS consumes approximately twice the processing power of 50 FPS, with corresponding impact on power consumption and thermal characteristics.

<b>Valid range:</b> 20-1000 FPS, though values above 120 FPS rarely provide perceptible improvements on standard displays.

The frame rate only affects animated SVG documents containing SMIL features.  Static documents are unaffected by this setting.

*********************************************************************************************************************/

static ERR SET_FrameRate(extSVG *Self, int Value)
{
   if ((Value >= 20) and (Value <= 1000)) {
      Self->FrameRate = Value;
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
Path: File system path to the source SVG document.

This field specifies the location of the SVG file to be loaded and processed during object initialisation.  The path supports both absolute and relative file references, with relative paths resolved according to the current working directory context.

The loading process occurs automatically during initialisation when a valid path is specified.  The referenced file must contain well-formed SVG content that conforms to W3C SVG standards for successful parsing.

<b>Supported file types:</b> Standard SVG files (*.svg) and compressed SVG files (*.svgz) are both supported, with automatic decompression handling for compressed formats.

<b>Path resolution:</b> The file system path is resolved through the standard Kotuku file access mechanisms, supporting virtual file systems, archives, and network-accessible resources where configured.

When both #Path and #Statement are specified, the Path field takes precedence and the Statement content is ignored during initialisation.

*********************************************************************************************************************/

static ERR GET_Path(extSVG *Self, STRING *Value)
{
   *Value = Self->Path;
   return ERR::Okay;
}

static ERR SET_Path(extSVG *Self, CSTRING Value)
{
   if (Self->Path)   { FreeResource(Self->Path); Self->Path = nullptr; }
   Self->Folder.clear();

   if ((Value) and (*Value)) {
      if (!(Self->Path = strclone(Value))) return ERR::AllocMemory;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Scene: Reference to the @VectorScene object containing the SVG scene graph.

This read-only field provides direct access to the @VectorScene object that manages the complete SVG scene graph structure.  The scene object serves as the root container for all generated vector elements and provides essential rendering coordination.

The scene reference remains valid throughout the SVG object's lifetime and enables direct manipulation of scene-wide properties including page dimensions, rendering settings, and global definitions.  This field simplifies access to the scene graph for applications requiring programmatic control over the complete document structure.

<b>Scene relationship:</b> When a #Target is specified, the Scene field references the @VectorScene that owns the target object.  For automatically generated scenes, this field references the internally created scene object.

*********************************************************************************************************************/

static ERR GET_Scene(extSVG *Self, objVectorScene **Value)
{
   *Value = Self->Scene;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Statement: String containing complete SVG document markup.

SVG data can be loaded from a string by specifying it here prior to initialisation.  If the #Path field has been
defined, it will take precedent and the Statement is ignored.

For incremental data parsing after initialisation, consider using the #DataFeed() action instead, which supports
progressive document construction from data streams.

*********************************************************************************************************************/

static ERR SET_Statement(extSVG *Self, CSTRING Value)
{
   if (Self->Statement) { FreeResource(Self->Statement); Self->Statement = nullptr; }

   if ((Value) and (*Value)) {
      if (!(Self->Statement = strclone(Value))) return ERR::AllocMemory;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Target: Destination container for the generated SVG scene graph elements.

This field redirects the generated SVG scene graph to an existing container object instead of creating an independent
scene structure.  The target approach enables seamless integration of SVG content into established UI hierarchies and
composite scene graphs.

<b>Default behaviour:</b> When no target is specified, the SVG object creates and manages a dedicated @VectorViewport
to contain the generated content.  This viewport and its children remain under direct SVG object ownership.

<b>Target requirements:</b> The target object must be part of an existing scene graph owned by a @VectorScene object.
While any vector object can serve as a target, @VectorViewport objects are recommended for optimal compatibility and
performance.

<b>Ownership implications:</b> Specifying a target makes the generated scene graph independent of the SVG object
lifecycle.  The SVG object can be terminated without affecting the created vector elements, enabling flexible
resource management patterns.

<b>Resource tracking:</b> When independent operation is not desired, enable the `ENFORCE_TRACKING` flag to maintain
resource tracking relationships between the SVG object and generated definitions, ensuring proper cleanup on object
destruction.

*********************************************************************************************************************/

static ERR SET_Target(extSVG *Self, OBJECTPTR Value)
{
   if (Value->classID() IS CLASSID::VECTORSCENE) {
      Self->Target = Value;
      Self->Scene = (objVectorScene *)Value;
      if (Self->Scene->Viewport) Self->Viewport = Self->Scene->Viewport;
   }
   else {
      auto owner = Value->Owner;
      while ((owner) and (owner->classID() != CLASSID::VECTORSCENE)) {
         owner = owner->Owner;
      }

      if (owner) {
         Self->Scene = (objVectorScene *)owner;
         Self->Target = Value;
         if (Self->Scene->Viewport) Self->Viewport = Self->Scene->Viewport;
      }
      else return ERR::Failed;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Title: The title of the SVG document.

The title of an SVG document is declared with a title element that can embedded anywhere in the document.  In cases
where a title has been specified, it will be possible to read it from this field.  If no title is in the document then
`NULL` will be returned.

*********************************************************************************************************************/

static ERR SET_Title(extSVG *Self, CSTRING Value)
{
   if (Self->Title) { FreeResource(Self->Title); Self->Title = nullptr; }
   if (Value) Self->Title = strclone(Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Viewport: Reference to the primary @VectorViewport containing the SVG document content.

This read-only field provides direct access to the main @VectorViewport object that contains the root-level SVG
content.

-END-

*********************************************************************************************************************/

static ERR GET_Viewport(extSVG *Self, OBJECTPTR *Value)
{
   if (!Self->initialised()) return ERR::NotInitialised;
   *Value = Self->Viewport;
   return ERR::Okay;
}

//********************************************************************************************************************

#include "anim_metrics.cpp"
#include "anim_timing.cpp"
#include "anim_parsing.cpp"
#include "anim_motion.cpp"
#include "anim_transform.cpp"
#include "anim_value.cpp"
#include "gradients.cpp"
#include "parser.cpp"

//********************************************************************************************************************

#include "class_svg_def.c"

static const FieldArray clSVGFields[] = {
   { "Target",    FDF_OBJECT|FDF_RI, nullptr, SET_Target },
   { "Path",      FDF_STRING|FDF_RW, nullptr, SET_Path },
   { "Title",     FDF_STRING|FDF_RW, nullptr, SET_Title },
   { "Statement", FDF_STRING|FDF_RW, nullptr, SET_Statement },
   { "Frame",     FDF_INT|FDF_RW, nullptr, nullptr },
   { "Flags",     FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clSVGFlags },
   { "FrameRate", FDF_INT|FDF_RW, nullptr, SET_FrameRate },
   // Virtual Fields
   { "Colour",        FDF_VIRTUAL|FDF_STRING|FDF_RW, GET_Colour, SET_Colour },
   { "FrameCallback", FDF_VIRTUAL|FDF_FUNCTION|FDF_RW, GET_FrameCallback, SET_FrameCallback },
   { "Src",           FDF_VIRTUAL|FDF_SYNONYM|FDF_STRING|FDF_RW, GET_Path, SET_Path },
   { "Scene",         FDF_VIRTUAL|FDF_OBJECT|FDF_R, GET_Scene, nullptr },
   { "Viewport",      FDF_VIRTUAL|FDF_OBJECT|FDF_R, GET_Viewport, nullptr },
   END_FIELD
};

//********************************************************************************************************************

static ERR init_svg(void)
{
   clSVG = objMetaClass::create::global(
      fl::ClassVersion(VER_SVG),
      fl::Name("SVG"),
      fl::FileExtension("*.svg"),
      fl::FileDescription("Scalable Vector Graphics (SVG)"),
      fl::Icon("filetypes/vectorgfx"),
      fl::Category(CCF::GUI),
      fl::Actions(clSVGActions),
      fl::Methods(clSVGMethods),
      fl::Fields(clSVGFields),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::Size(sizeof(extSVG)),
      fl::Path(MOD_PATH));

   return clSVG ? ERR::Okay : ERR::AddClass;
}
