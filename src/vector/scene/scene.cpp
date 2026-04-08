/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
VectorScene: Manages the scene graph for a collection of vectors.

The VectorScene class acts as a container and control point for the management of vector definitions.  Its main
purpose is to draw the scene to a target @Bitmap or @Surface provided by the client.

Vector scenes are created by initialising multiple Vector objects such as @VectorPath and @VectorViewport and
positioning them within a vector tree.  The VectorScene must lie at the root of the tree.

The default mode of operation is for scenes to be manually drawn, for which the client must set the target #Bitmap
and call the #Draw() action as required.  Automated drawing can be enabled by setting the target #Surface prior
to initialisation.  In automated mode the #PageWidth and #PageHeight will reflect the dimensions of the target surface
at all times.

Vector definitions can be saved and loaded from permanent storage by using the @SVG class.
-END-

*********************************************************************************************************************/

#include "agg_rasterizer_outline_aa.h"
#include "agg_curves.h"
#include "agg_image_accessors.h"
#include "agg_renderer_base.h"
#include "agg_renderer_outline_aa.h"
#include "agg_span_interpolator_linear.h"
#include "agg_renderer_outline_image.h"
#include "agg_conv_smooth_poly1.h"
#include "agg_span_gradient.h"
#include "agg_conv_contour.h"

//#include "../vector.h"

//********************************************************************************************************************

class VectorState;

static void fill_image(VectorState &, const TClipRectangle<double> &, agg::path_storage &, VSM,
   const agg::trans_affine &, double, double, extVectorImage &, agg::renderer_base<agg::pixfmt_psl> &,
   agg::rasterizer_scanline_aa<> &, double Alpha = 1.0);

static void fill_gradient(VectorState &, const TClipRectangle<double> &, agg::path_storage *,
   const agg::trans_affine &, double, double, extVectorGradient &, GRADIENT_TABLE *,
   agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &);

static void fill_pattern(VectorState &, const TClipRectangle<double> &, agg::path_storage *,
   VSM, const agg::trans_affine &, double ViewWidth, double, extVectorPattern &,
   agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &);

//********************************************************************************************************************

#include "scene_draw.cpp"
#include "scene_fill.cpp"
#include "scene_clipping.cpp"

//********************************************************************************************************************

static ERR VECTORSCENE_Reset(extVectorScene *);

void apply_focus(extVectorScene *, extVector *);
static void scene_key_event(evKey *, int, extVectorScene *);
static void process_resize_msgs(extVectorScene *);

//********************************************************************************************************************

static void render_to_surface(extVectorScene *Self, objSurface *Surface, objBitmap *Bitmap)
{
   Self->Bitmap = Bitmap;

   if ((!Self->PageWidth) or (!Self->PageHeight)) {
      if (Self->Viewport) mark_dirty(Self->Viewport, RC::BASE_PATH|RC::TRANSFORM); // Base-paths need to be recomputed if they use scaled coordinates.
   }

   acDraw(Self);

   Self->Bitmap = nullptr;
}

//********************************************************************************************************************

void render_scene_from_viewport(extVectorScene *Self, objBitmap *Bitmap, objVectorViewport *Viewport)
{
   SceneRenderer renderer(Self);

   if ((Self->Flags & VPF::RENDER_TIME) != VPF::NIL) { // Client wants to know how long the rendering takes to complete
      auto time = PreciseTime();
      renderer.draw(Bitmap, Viewport);
      if ((Self->RenderTime = PreciseTime() - time) < 1) Self->RenderTime = 1;
   }
   else renderer.draw(Bitmap, Viewport);

// For debugging purposes, draw a boundary around the target area.
//   static RGB8 highlightA = { .Red = 255, .Green = 0, .Blue = 0, .Alpha = 255 };
//   uint32_t highlight = PackPixelRGBA(bmp, &highlightA);
//   gfx::DrawRectangle(bmp, bmp->Clip.Left, bmp->Clip.Top, bmp->Clip.Right-bmp->Clip.Left, bmp->Clip.Bottom-bmp->Clip.Top, highlight, BAF::NIL);
}

//********************************************************************************************************************
// Clear references to definition objects when they are freed.  Note that this function can sometimes be called after
// the Defs table has been cleared.

static void notify_def_free(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extVectorScene *)CurrentContext();

restart:
   if (!Self->Defs.size()) return; // Necessary when dealing with poor quality mapping libs

   for (auto it = Self->Defs.begin(); it != Self->Defs.end(); it++) {
      if (it->second IS Object) {
         Self->Defs.erase(it);
         goto restart;
      }
   }
}

//********************************************************************************************************************

static void notify_free(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extVectorScene *)CurrentContext();
   if (Self->SurfaceID IS Object->UID) Self->SurfaceID = 0;
}

//********************************************************************************************************************

static void notify_redimension(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acRedimension *Args)
{
   auto Self = (extVectorScene *)CurrentContext();

   if ((Self->Flags & VPF::RESIZE) != VPF::NIL) {
      Self->PageWidth  = Args->Width;
      Self->PageHeight = Args->Height;

      if (Self->Viewport) {
         mark_dirty(Self->Viewport, RC::BASE_PATH|RC::TRANSFORM); // Base-paths need to be recomputed if they use scaled coordinates.
      }

      pf::ScopedObjectLock<objSurface> surface(Self->SurfaceID);
      if (surface.granted()) surface->scheduleRedraw();
   }
}

//********************************************************************************************************************
// Called when the subscribed Surface loses the focus.

static void notify_lostfocus(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extVectorScene *)CurrentContext();
   if (Self->KeyHandle) { UnsubscribeEvent(Self->KeyHandle); Self->KeyHandle = nullptr; }

   apply_focus(Self, nullptr);
}

//********************************************************************************************************************
// Called when the subscribed Surface receives the focus.

static void notify_focus(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extVectorScene *)CurrentContext();
   if (!Self->KeyHandle) {
      SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(scene_key_event, Self), &Self->KeyHandle);
   }
}

/*********************************************************************************************************************

-METHOD-
AddDef: Registers a named definition object within a scene graph.

This method will add a new definition object to the root of a vector tree and gives it a name.  This feature is
provided to support SVG style referencing for features such as gradients, images and patterns.  By providing a name
with the definition object, the object can then be referenced in URL strings.

For example, if creating a gradient with a name of `redGradient` it would be possible to reference it with
`url(#redGradient)` in common graphics attributes such as `fill` and `stroke`.

At the time of writing, the provided object must belong to one of the following classes to be valid: @Vector,
@VectorScene, @VectorGradient, @VectorImage, @VectorPath, @VectorPattern, @VectorFilter, @VectorTransition,
@VectorClip.

-INPUT-
cstr Name: The unique name to associate with the definition.
obj Def: Reference to the definition object.

-ERRORS-
Okay
NullArgs
ResourceExists: The given name is already in use as a definition.
InvalidObject: The definition is not an accepted object class.
UnsupportedOwner: The definition is not owned by the scene.

*********************************************************************************************************************/

static ERR VECTORSCENE_AddDef(extVectorScene *Self, struct sc::AddDef *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Name) or (!Args->Def)) return log.warning(ERR::NullArgs);

   if (Self->HostScene) { // Defer all definitions if a hosting scene is active.
      return Self->HostScene->addDef(Args->Name, Args->Def);
   }

   OBJECTPTR def = Args->Def;

   switch(def->classID()) {
      case CLASSID::VECTORGRADIENT:   ((extVectorGradient*)def)->HostScene = Self; break;
      case CLASSID::VECTORIMAGE:      ((extVectorImage *)def)->HostScene = Self; break;
      case CLASSID::VECTORPATH:       ((extVectorPath*)def)->HostScene = Self; break;
      case CLASSID::VECTORPATTERN:    ((extVectorPattern*)def)->HostScene = Self; break;
      case CLASSID::VECTORTRANSITION: ((extVectorTransition*)def)->HostScene = Self; break;
      case CLASSID::VECTORCLIP:       ((extVectorClip*)def)->HostScene = Self; break;
      case CLASSID::VECTORSCENE:      break;

      default:
         switch(def->baseClassID()) {
            case CLASSID::VECTOR:       break;
            case CLASSID::VECTORFILTER: break;
            default: return log.warning(ERR::InvalidObject);
         }
   }

   // If the resource does not belong to the Scene object, this can lead to invalid pointer references

   if (!def->hasOwner(Self->UID)) {
      log.warning("The %s must belong to VectorScene #%d, but is owned by object #%d.", def->Class->ClassName, Self->UID, def->ownerID());
      return ERR::UnsupportedOwner;
   }

   if (Self->Defs.contains(Args->Name)) { // Check that the definition name is unique.
      log.detail("The vector definition name '%s' is already in use.", Args->Name);
      return ERR::ResourceExists;
   }

   log.detail("Adding definition '%s' referencing %s #%d", Args->Name, def->Class->ClassName, def->UID);

   SubscribeAction(def, AC::Free, C_FUNCTION(notify_def_free));

   Self->Defs[Args->Name] = def;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Debug: Internal functionality for debugging.

This internal method prints comprehensive information that describes the scene graph to the log.

-ERRORS-
Okay:

*********************************************************************************************************************/

static ERR VECTORSCENE_Debug(extVectorScene *Self)
{
   pf::Log log("debug_tree");

   pf::vector<ChildEntry> list;
   if ((ListChildren(Self->UID, &list) IS ERR::Okay) and (list.size() > 1)) {
      log.msg("Scene #%d has %d children:", Self->UID, int(std::ssize(list)-1));
      for (auto &rec : list) {
         auto obj = GetObjectPtr(rec.ObjectID);
         if (obj IS Self->Viewport) continue;
         log.msg(" #%d %s %s", rec.ObjectID, obj->Class->ClassName, obj->Name);
      }
   }

   log.msg("Scene #%d scene graph follows:", Self->UID);

   int level = 0;
   debug_tree((extVector *)Self->Viewport, level);
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Draw: Renders the scene to a bitmap.

The Draw action will render the scene to the target #Bitmap immediately.  If #Bitmap is NULL, an error will be
returned.

In addition, the #RenderTime field will be updated if the `RENDER_TIME` flag is defined.

-ERRORS-
Okay
FieldNotSet: The Bitmap field is NULL.

*********************************************************************************************************************/

static ERR VECTORSCENE_Draw(extVectorScene *Self, struct acDraw *Args)
{
   if (!Self->Bitmap) {
      pf::Log log;
      return log.warning(ERR::FieldNotSet);
   }

   acFlush(Self);

   render_scene_from_viewport(Self, Self->Bitmap, Self->Viewport);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
FindDef: Search for a vector definition by name.

Use the FindDef method to search for a vector definition by name.  A reference to the definition will be returned if
the search is successful.

Definitions are created with the #AddDef() method.

-INPUT-
cstr Name: The name of the definition.
&obj Def: A pointer to the definition object is returned here if discovered.

-ERRORS-
Okay
NullArgs
Search: A definition with the given Name was not found.
-END-

*********************************************************************************************************************/

static ERR VECTORSCENE_FindDef(extVectorScene *Self, struct sc::FindDef *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Name)) return log.warning(ERR::NullArgs);

   if (Self->HostScene) return Self->HostScene->findDef(Args->Name, &Args->Def);

   CSTRING name = Args->Name;

   if (*name IS '#') name = name + 1;
   else if (startswith("url(#", name)) {
      int i;
      for (i=5; (name[i] != ')') and name[i]; i++);
      std::string lookup;
      lookup.assign(name, 5, i-5);

      if (auto def = Self->Defs.find(lookup); def != Self->Defs.end()) {
         Args->Def = def->second;
         return ERR::Okay;
      }
      else return ERR::Search;
   }

   if (auto def = Self->Defs.find(name); def != Self->Defs.end()) {
      Args->Def = def->second;
      return ERR::Okay;
   }
   else return ERR::Search;
}

//********************************************************************************************************************
// Flush pending activity messages.  This should always be performed prior to drawing.

static ERR VECTORSCENE_Flush(extVectorScene *Self)
{
   // Protect against recursion from client callbacks.

   static bool recurse = false;
   if (recurse) return ERR::Recursion;
   recurse = true;

   // Any pending resize messages for viewports must be processed prior to drawing.

   process_resize_msgs(Self);

   // Send a dummy input event to the mouse cursor to ensure that changes to underlying
   // vector paths will generate the necessary crossing events.

   if ((Self->SurfaceID) and (Self->RefreshCursor)) {
      double abs_x, abs_y;
      int s_x, s_y;

      Self->RefreshCursor = false;
      gfx::GetSurfaceCoords(Self->SurfaceID, nullptr, nullptr, &s_x, &s_y, nullptr, nullptr);
      gfx::GetCursorPos(&abs_x, &abs_y);

      const InputEvent event = {
         .Next        = nullptr,
         .Value       = 0,
         .Timestamp   = 0,
         .RecipientID = Self->SurfaceID,
         .OverID      = Self->SurfaceID,
         .AbsX        = abs_x,
         .AbsY        = abs_y,
         .X           = abs_x - s_x,
         .Y           = abs_y - s_y,
         .DeviceID    = 0,
         .Type        = JET::ABS_XY,
         .Flags       = JTYPE::MOVEMENT,
         .Mask        = JTYPE::MOVEMENT
      };
      scene_input_events(&event, 0);
   }

   recurse = false;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORSCENE_Free(extVectorScene *Self, APTR Args)
{
   Self->Defs.clear(); // Required because the standard destructor is lazy and doesn't clear the table size
   Self->~extVectorScene();

   if (Self->Viewport) Self->Viewport->Parent = nullptr;
   if (Self->Buffer)   { delete Self->Buffer; Self->Buffer = nullptr; }
   if (Self->InputHandle) { gfx::UnsubscribeInput(Self->InputHandle); Self->InputHandle = 0; }

   if (Self->SurfaceID) {
      OBJECTPTR surface;
      if (AccessObject(Self->SurfaceID, 5000, &surface) IS ERR::Okay) {
         UnsubscribeAction(surface, AC::NIL);
         ReleaseObject(surface);
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORSCENE_Init(extVectorScene *Self)
{
   Self->Cursor = PTC::DEFAULT;

   // Setting the SurfaceID is optional and enables auto-rendering to the display.  The
   // alternative for the client is to set the Bitmap field and manage rendering manually.
   //
   // As long as PageWidth and PageHeight aren't set prior to initialisation, the scene will
   // match the width and height of the surface at all times when in this mode.

   if (Self->SurfaceID) {
      pf::ScopedObjectLock<objSurface> surface(Self->SurfaceID, 5000);
      if (surface.granted()) {
         surface->addCallback(C_FUNCTION(render_to_surface));

         if ((!Self->PageWidth) or (!Self->PageHeight)) {
            Self->Flags |= VPF::RESIZE;
            Self->PageWidth = surface->get<int>(FID_Width);
            Self->PageHeight = surface->get<int>(FID_Height);
         }

         SubscribeAction(*surface, AC::Redimension, C_FUNCTION(notify_redimension));
         SubscribeAction(*surface, AC::Free, C_FUNCTION(notify_free));
         SubscribeAction(*surface, AC::Focus, C_FUNCTION(notify_focus));
         SubscribeAction(*surface, AC::LostFocus, C_FUNCTION(notify_lostfocus));

         if (surface->hasFocus()) {
            SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(scene_key_event, Self), &Self->KeyHandle);
         }
      }

      auto callback = C_FUNCTION(scene_input_events);
      if (gfx::SubscribeInput(&callback, Self->SurfaceID, JTYPE::MOVEMENT|JTYPE::CROSSING|JTYPE::BUTTON|JTYPE::REPEATED|JTYPE::EXT_MOVEMENT, 0, &Self->InputHandle) != ERR::Okay) {
         return ERR::Function;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR VECTORSCENE_NewObject(extVectorScene *Self)
{
   Self->SampleMethod = VSM::AUTO;

   // Please refer to the Reset action for setting variable defaults
   return VECTORSCENE_Reset(Self);
}

static ERR VECTORSCENE_NewPlacement(extVectorScene *Self)
{
   new (Self) extVectorScene;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Redimension: Redefines the size of the page.
-END-
*********************************************************************************************************************/

static ERR VECTORSCENE_Redimension(extVectorScene *Self, struct acRedimension *Args)
{
   if (!Args) return ERR::NullArgs;

   if (Args->Width >= 1.0)  Self->PageWidth  = int(Args->Width);
   if (Args->Height >= 1.0) Self->PageHeight = int(Args->Height);

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Reset: Clears all registered definitions and resets field values.  Child vectors are unmodified.
-END-
*********************************************************************************************************************/

static ERR VECTORSCENE_Reset(extVectorScene *Self)
{
   if (Self->Buffer)  { delete Self->Buffer; Self->Buffer = nullptr; }
   Self->Defs.clear();
   Self->Gamma = 1.0;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Resize: Redefines the size of the page.
-END-
*********************************************************************************************************************/

static ERR VECTORSCENE_Resize(extVectorScene *Self, struct acResize *Args)
{
   if (!Args) return ERR::NullArgs;
   if (Args->Width >= 1.0)  Self->PageWidth  = int(Args->Width);
   if (Args->Height >= 1.0) Self->PageHeight = int(Args->Height);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SearchByID: Search for a vector by numeric ID.

This method will search a scene for an object that matches a given `ID` (vector ID's can be set with the
@Vector.NumericID or @Vector.ID fields).  If multiple vectors are using the same ID, repeated calls can be made
to this method to find them all.  This is achieved by calling this method on the vector that was last returned
as a `Result`.

Note that searching for string-based ID's is achieved by converting the string to a case-sensitive hash
with `strhash()` and using that as the ID.

-INPUT-
int ID: The ID to search for.
&obj Result: This parameter will be updated with the discovered vector, or `NULL` if not found.

-ERRORS-
Okay
NullArgs
Search: A vector with a matching ID was not found.
-END-

*********************************************************************************************************************/

static ERR VECTORSCENE_SearchByID(extVectorScene *Self, struct sc::SearchByID *Args)
{
   if (!Args) return ERR::NullArgs;
   Args->Result = nullptr;

   auto vector = (extVector *)Self->Viewport;
   while (vector) {
      //log.msg("Search","%.3d: %p <- #%d -> %p Child %p", vector->Index, vector->Prev, vector->UID, vector->Next, vector->Child);
cont:
      if (vector->NumericID IS Args->ID) {
         Args->Result = vector;
         return ERR::Okay;
      }

      if (vector->Child) vector = (extVector *)vector->Child;
      else if (vector->Next) vector = (extVector *)vector->Next;
      else {
         while ((vector = get_parent(vector))) { // Unwind back up the stack, looking for the first Parent with a Next field.
            if (vector->Class->BaseClassID != CLASSID::VECTOR) return ERR::Search;
            if (vector->Next) {
               vector = (extVector *)vector->Next;
               goto cont;
            }
         }
         return ERR::Search;
      }
   }

   return ERR::Search;
}

/*********************************************************************************************************************
-FIELD-
Bitmap: Target bitmap for drawing vectors.

The target bitmap to use when drawing the vectors must be specified here.
-END-
*********************************************************************************************************************/

static ERR SET_Bitmap(extVectorScene *Self, objBitmap *Value)
{
   if (Value) {
      if (Self->Buffer) delete Self->Buffer;

      Self->Buffer = new (std::nothrow) agg::rendering_buffer;
      if (Self->Buffer) {
         Self->Buffer->attach(Value->Data, Value->Width, Value->Height, Value->LineWidth);
         Self->Bitmap = Value;

         if ((Self->Flags & VPF::BITMAP_SIZED) != VPF::NIL) {
            Self->PageWidth = Value->Width;
            Self->PageHeight = Value->Height;
         }
      }
      else return ERR::Memory;
   }
   else Self->Buffer = nullptr;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Defs: Obtain direct access to the SVG definition table.

Reading the Defs field will return a direct pointer to the SVG definition table, which is declared as a key-value C++
type:

<pre>ankerl::unordered_dense::map&lt;std::string, OBJECTPTR&gt;</pre>

Direct access is provided for internal use only and not for the benefit of client programs.

*********************************************************************************************************************/

static ERR GET_Defs(extVectorScene *Self, ankerl::unordered_dense::map<std::string, OBJECTPTR> **Value)
{
   *Value = &Self->Defs;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.

-FIELD-
Gamma: Private. Not currently implemented.

-FIELD-
HostScene: Refers to a top-level VectorScene object, if applicable.

Set HostScene to another VectorScene object if it is intended that this scene is a child of the other.  This allows
some traits such as vector definitions to be automatically inherited from the host scene.

This feature is useful in circumstances where a hidden group of vectors need to be managed separately, while retaining
access to established definitions and vectors in the main.

-FIELD-
PageHeight: The height of the page that contains the vector.

This value defines the pixel height of the page that contains the vector scene graph.  If the `RESIZE` #Flags
option is used then the viewport will be scaled to fit within the page.

*********************************************************************************************************************/

static ERR SET_PageHeight(extVectorScene *Self, int Value)
{
   if (Value IS Self->PageHeight) return ERR::Okay;

   if (Value < 1) Self->PageHeight = 1;
   else Self->PageHeight = Value;

   if (Self->Viewport) mark_dirty(Self->Viewport, RC::BASE_PATH|RC::TRANSFORM); // Base-paths need to be recomputed if they use scaled coordinates.
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
PageWidth: The width of the page that contains the vector.

This value defines the pixel width of the page that contains the vector scene graph.  If the `RESIZE` #Flags
option is used then the viewport will be scaled to fit within the page.

*********************************************************************************************************************/

static ERR SET_PageWidth(extVectorScene *Self, int Value)
{
   if (Value IS Self->PageWidth) return ERR::Okay;

   if (Value < 1) Self->PageWidth = 1;
   else Self->PageWidth = Value;

   if (Self->Viewport) mark_dirty(Self->Viewport, RC::BASE_PATH|RC::TRANSFORM);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RenderTime: Returns the rendering time of the last scene.

RenderTime returns the rendering time of the last scene that was drawn, measured in microseconds.  This value can also
be used to compute frames-per-second with `1000000 / RenderTime`.

The `RENDER_TIME` flag must be set before fetching this value, as it is required to enable the timing feature.  If
`RENDER_TIME` is not set, it will be set automatically so that subsequent calls return a correct value.

*********************************************************************************************************************/

static ERR GET_RenderTime(extVectorScene *Self, int64_t *Value)
{
   Self->Flags |= VPF::RENDER_TIME;
   *Value = Self->RenderTime;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SampleMethod: The sampling method to use when interpolating images and patterns.

The SampleMethod controls the sampling algorithm that is used when images and patterns in the vector definition are affected
by rotate, skew and scale transforms.  The choice of method will have a significant impact on the speed and quality of
the images that are displayed in the rendered scene.  The recommended default is `AUTO`, which allows the drawing
algorithms to choose the best quality option dynamically.

*********************************************************************************************************************/

static ERR SET_SampleMethod(extVectorScene* Self, VSM Value)
{
   if ((Value != Self->SampleMethod) and (Self->Viewport)) mark_dirty(Self->Viewport, RC::FINAL_PATH);
   Self->SampleMethod = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Surface: May refer to a @Surface object for enabling automatic rendering.

Setting the Surface field will enable automatic rendering to a display @Surface.  The use of features such as input
event handling and user focus management will also require an associated surface as a pre-requisite.

*********************************************************************************************************************/

static ERR SET_Surface(extVectorScene *Self, OBJECTID Value)
{
   Self->SurfaceID = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Viewport: References the first object in the scene, which must be a @VectorViewport object.

The first object in the vector scene is referenced here.  It must belong to the @VectorViewport class, which will
be used to define the size and location of the area rendered by the scene.

The Viewport value cannot be set by the client.  It will be automatically defined when the first @VectorViewport
owned by the VectorScene is initialised.
-END-

*********************************************************************************************************************/

//********************************************************************************************************************
// Apply focus to a vector and all other vectors within that tree branch (not necessarily just the viewports).
// Also sends LostFocus notifications to vectors that previously had the focus.
// The glVectorFocusList maintains the current focus state, with the most foreground vector at the beginning.
//
// If Vector is NULL then the focus is dropped from all vectors.

void apply_focus(extVectorScene *Scene, extVector *Vector)
{
   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);

   if ((!glVectorFocusList.empty()) and (Vector IS glVectorFocusList.front())) return;

   std::vector<extVector *> focus_gained; // The first reference is the most foreground object

   for (auto node=Vector; node; node=(extVector *)node->Parent) {
      if (node->Class->BaseClassID IS CLASSID::VECTOR) {
         focus_gained.emplace_back(node);
      }
      else break;
   }

#if 0
   std::string vlist;
   for (auto const fv : focus_gained) {
      char buffer[30];
      snprintf(buffer, sizeof(buffer), "#%d ", fv->UID);
      vlist.append(buffer);
   }

   log.msg("Vector focus list now: %s", vlist.c_str());
#endif

   // Report focus events to vector subscribers.

   auto focus_event = FM::HAS_FOCUS;
   for (auto const fgv : focus_gained) {
      bool no_focus = true, lost_focus_to_child = false, was_child_now_primary = false;

      if (!glVectorFocusList.empty()) {
         no_focus = std::find(glVectorFocusList.begin(), glVectorFocusList.end(), fgv) IS glVectorFocusList.end();
         if (!no_focus) {
            lost_focus_to_child = ((fgv IS glVectorFocusList.front()) and (focus_event IS FM::CHILD_HAS_FOCUS));
            was_child_now_primary = ((fgv != glVectorFocusList.front()) and (focus_event IS FM::HAS_FOCUS));
         }
      }

      if ((no_focus) or (lost_focus_to_child) or (was_child_now_primary)) {
         pf::ScopedObjectLock<extVector> vec(fgv, 1000);
         if (vec.granted()) {
            send_feedback((extVector *)fgv, focus_event, Vector);
            focus_event = FM::CHILD_HAS_FOCUS;
         }
      }
   }

   // Report lost focus events, starting from the foreground.

   if (!glVectorFocusList.empty()) { // Check required to avoid crashing on empty std::vector :-/
      auto copy = glVectorFocusList; // Take a copy of the list in case it gets modified on feedback.
      for (auto const fv : copy) {
         if (std::find(focus_gained.begin(), focus_gained.end(), fv) IS focus_gained.end()) {
            pf::ScopedObjectLock<extVector> vec(fv, 1000);
            if (vec.granted()) send_feedback(fv, FM::LOST_FOCUS, Vector);
         }
         else break;
      }
   }

   glVectorFocusList = std::move(focus_gained);
}

//********************************************************************************************************************

static void process_resize_msgs(extVectorScene *Self)
{
   if (Self->PendingResizeMsgs.size() > 0) {
      for (auto it=Self->PendingResizeMsgs.begin(); it != Self->PendingResizeMsgs.end(); it++) {
         extVectorViewport *view = *it;

         auto list = Self->ResizeSubscriptions[view]; // take copy
         for (auto &sub : list) {
            ERR result;
            auto vector = sub.first;
            auto func   = sub.second;
            if (func.isC()) {
               pf::SwitchContext ctx(func.Context);
               auto callback = (ERR (*)(extVectorViewport *, objVector *, double, double, double, double, APTR))func.Routine;
               result = callback(view, vector, view->FinalX, view->FinalY, view->vpFixedWidth, view->vpFixedHeight, func.Meta);
            }
            else if (func.isScript()) {
               sc::Call(func, std::to_array<ScriptArg>({
                  { "Viewport",       view, FDF_OBJECT },
                  { "Vector",         vector, FDF_OBJECT },
                  { "ViewportX",      view->FinalX },
                  { "ViewportY",      view->FinalY },
                  { "ViewportWidth",  view->vpFixedWidth },
                  { "ViewportHeight", view->vpFixedHeight }
               }));
            }
         }
      }

      Self->PendingResizeMsgs.clear();
   }
}

//********************************************************************************************************************
// Receiver for keyboard events

static ERR vector_keyboard_events(extVector *Vector, const evKey *Event)
{
   for (auto it=Vector->KeyboardSubscriptions->begin(); it != Vector->KeyboardSubscriptions->end(); ) {
      ERR result = ERR::Terminate;
      auto &sub = *it;
      if (sub.Callback.isC()) {
         pf::SwitchContext ctx(sub.Callback.Context);
         auto callback = (ERR (*)(objVector *, KQ, KEY, int, APTR))sub.Callback.Routine;
         result = callback(Vector, Event->Qualifiers, Event->Code, Event->Unicode, sub.Callback.Meta);
      }
      else if (sub.Callback.isScript()) {
         // In this implementation the script function will receive all the events chained via the Next field
         sc::Call(sub.Callback, std::to_array<ScriptArg>({
            { "Vector",     Vector, FDF_OBJECT },
            { "Qualifiers", int(Event->Qualifiers) },
            { "Code",       int(Event->Code) },
            { "Unicode",    Event->Unicode }
         }), result);
      }

      if (result IS ERR::Terminate) Vector->KeyboardSubscriptions->erase(it);
      else it++;
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Distribute input events to any vectors that have subscribed and have the focus

static void scene_key_event(evKey *Event, int Size, extVectorScene *Self)
{
   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);

   if (Event->Code IS KEY::TAB) {
      if ((Event->Qualifiers & KQ::RELEASED) IS KQ::NIL) return;

      bool reverse = (((Event->Qualifiers & KQ::QUALIFIERS) & KQ::SHIFT) != KQ::NIL);

      if (((Event->Qualifiers & KQ::QUALIFIERS) IS KQ::NIL) or (reverse)) {
         if (Self->KeyboardSubscriptions.size() > 1) {
            for (auto it=glVectorFocusList.begin(); it != glVectorFocusList.end(); it++) {
               auto find = Self->KeyboardSubscriptions.find(*it);
               if (find != Self->KeyboardSubscriptions.end()) {
                  if (reverse) {
                     if (find IS Self->KeyboardSubscriptions.begin()) {
                        apply_focus(Self, *Self->KeyboardSubscriptions.rbegin());
                     }
                     else apply_focus(Self, *(--find));
                  }
                  else {
                     if (++find IS Self->KeyboardSubscriptions.end()) find = Self->KeyboardSubscriptions.begin();
                     apply_focus(Self, *find);
                  }
                  return;
               }
            }
         }
      }

      if (!Self->KeyboardSubscriptions.empty()) {
         apply_focus(Self, *Self->KeyboardSubscriptions.begin());
      }

      return;
   }

   for (auto vi = Self->KeyboardSubscriptions.begin(); vi != Self->KeyboardSubscriptions.end(); vi++) {
      auto const vector = *vi;
      // Use the focus list to determine where the key event needs to be sent.
      for (auto it=glVectorFocusList.begin(); it != glVectorFocusList.end(); it++) {
         if (*it IS vector) {
            vector_keyboard_events(vector, Event);
            break;
         }
      }
   }
}

//********************************************************************************************************************

#include "scene_input.cpp"

#include "scene_def.c"

static const FieldArray clSceneFields[] = {
   { "RenderTime",   FDF_INT64|FDF_R, GET_RenderTime },
   { "Gamma",        FDF_DOUBLE|FDF_RW },
   { "HostScene",    FDF_OBJECT|FDF_RI,    nullptr, nullptr, CLASSID::VECTORSCENE },
   { "Viewport",     FDF_OBJECT|FD_R,      nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Bitmap",       FDF_OBJECT|FDF_RW,    nullptr, SET_Bitmap, CLASSID::BITMAP },
   { "Surface",      FDF_OBJECTID|FDF_RI,  nullptr, SET_Surface, CLASSID::SURFACE },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clVectorSceneFlags },
   { "PageWidth",    FDF_INT|FDF_RW,      nullptr, SET_PageWidth },
   { "PageHeight",   FDF_INT|FDF_RW,      nullptr, SET_PageHeight },
   { "SampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_SampleMethod, &clVectorSceneSampleMethod },
   // Virtual fields
   { "Defs",         FDF_PTR|FDF_SYSTEM|FDF_R, GET_Defs, nullptr },
   END_FIELD
};

//********************************************************************************************************************

ERR init_vectorscene(void)
{
   clVectorScene = objMetaClass::create::global(
      fl::ClassVersion(VER_VECTORSCENE),
      fl::Name("VectorScene"),
      fl::Category(CCF::GRAPHICS),
      fl::Actions(clVectorSceneActions),
      fl::Methods(clVectorSceneMethods),
      fl::Fields(clSceneFields),
      fl::Size(sizeof(extVectorScene)),
      fl::Path(MOD_PATH));

   return clVectorScene ? ERR::Okay : ERR::AddClass;
}
