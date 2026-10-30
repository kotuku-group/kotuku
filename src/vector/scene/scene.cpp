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
#include "agg_span_gouraud_rgba.h"
#include "agg_span_gouraud_rgba_linear.h"
#include "agg_span_gouraud_rgba_quantise.h"
#include "agg_rasterizer_compound_aa.h"
#include "agg_pixfmt_amask_adaptor.h"
#include "agg_conv_contour.h"

//#include "../vector.h"

//********************************************************************************************************************

class SceneRenderer;
class VectorState;

static void fill_image(VectorState &, const TClipRectangle<double> &, agg::path_storage &, VSM,
   const agg::trans_affine &, double, double, extVectorImage &, agg::renderer_base<agg::pixfmt_psl> &,
   agg::rasterizer_scanline_aa<> &, double Alpha = 1.0, SceneRenderer *Render = nullptr);

static void fill_gradient(VectorState &, const TClipRectangle<double> &, agg::path_storage *,
   const agg::trans_affine &, double, double, extGradient &, GRADIENT_TABLE *,
   agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &, SceneRenderer *Render = nullptr);

static void fill_gouraud(VectorState &, const TClipRectangle<double> &, double, double, extGradientGouraud &, double,
   agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &, const agg::trans_affine &,
   SceneRenderer *Render = nullptr);

static void fill_mesh(VectorState &, const TClipRectangle<double> &, double, double, extGradientMesh &, double,
   agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &, const agg::trans_affine &,
   SceneRenderer *Render = nullptr);

static void fill_diffusion(VectorState &, const TClipRectangle<double> &, double, double, extGradientDiffusion &,
   double, agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &, const agg::trans_affine &,
   SceneRenderer *Render = nullptr);

static void fill_pattern(VectorState &, const TClipRectangle<double> &, agg::path_storage *,
   VSM, const agg::trans_affine &, double ViewWidth, double, extVectorPattern &,
   agg::renderer_base<agg::pixfmt_psl> &, agg::rasterizer_scanline_aa<> &, double Alpha = 1.0,
   SceneRenderer *Render = nullptr);

//********************************************************************************************************************

#include "scene_draw.cpp"
#include "scene_fill.cpp"
#include "scene_clipping.cpp"

//********************************************************************************************************************

static ERR VECTORSCENE_Reset(extVectorScene *);

void apply_focus(extVectorScene *, extVector *);
extVectorViewport * get_drop_target_at_xy(extVectorScene *, double, double);
static void scene_key_event(evKey *, int, extVectorScene *);
static void deliver_key_event(evKey *, extVectorScene *);
static bool process_resize_msgs(extVectorScene *);

// Surfaces that have a drop router, mapped to the routing VectorScene.  Only one scene per surface can route drops,
// which ensures that each drop is delivered exactly once.

static ankerl::unordered_dense::map<OBJECTID, OBJECTID> glDropRouters;
static std::mutex glDropRoutersLock;

// Key dispatch tracking.  A key handler can move the vector focus to another scene while the event is still being
// dispatched, e.g. an Enter press that closes a popup returns the focus to the window that opened it.  That scene is
// marked to skip its next key event, which is the same event, so that the press is not acted on twice.
//
// The module subscribes before any scene.  Events are delivered in reverse subscription order, so its callback runs
// after all scenes and advances the dispatch counter even when consecutive events have identical contents and reuse
// the same buffer.  The counter is protected by glVectorFocusLock; tlKeyScene identifies the scene delivering a key
// event on the calling thread.

static thread_local extVectorScene *tlKeyScene = nullptr;
static int64_t glKeyDispatch = 1; // Zero is reserved for scenes that have not received an event.

static void finish_key_dispatch(evKey *Event, int Size, APTR Meta)
{
   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);
   glKeyDispatch++;
}

//********************************************************************************************************************

// Disabling a vector also blocks input to its descendants.  Check at delivery time because callbacks can disable
// their target or an ancestor while an input event is being dispatched.

static bool input_disabled(extVector *Vector)
{
   for (auto node=Vector; node; node=(extVector *)node->Parent) {
      if (node->Class->BaseClassID != CLASSID::VECTOR) break;
      if ((node->Flags & VF::DISABLED) != VF::NIL) return true;
   }
   return false;
}

// A subscriber can take the focus from the tab cycle only if neither it nor a vector ancestor is disabled or hidden,
// as for Vector.Focus().

static bool tab_focusable(extVector *Vector)
{
   for (auto node=Vector; node; node=(extVector *)node->Parent) {
      if (node->Class->BaseClassID != CLASSID::VECTOR) break;
      if (((node->Flags & VF::DISABLED) != VF::NIL) or (node->Visibility IS VIS::HIDDEN) or
          (node->Visibility IS VIS::COLLAPSE)) return false;
   }
   return true;
}

// Moves the focus to the next or previous focusable keyboard subscriber, then sends FM::TAB_FOCUS to the focused vector
// and its ancestors.  The event is sent even if the focus did not move, e.g. when the scene has a single subscriber.

static void cycle_focus(extVectorScene *Self, bool Reverse)
{
   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);

   auto &subs = Self->KeyboardSubscriptions;
   if (subs.empty()) return;

   // Start from the subscriber that holds the focus.  If none does, the search starts before the first subscriber
   // in either direction.

   auto current = subs.end();
   for (auto it=glVectorFocusList.begin(); it != glVectorFocusList.end(); it++) {
      if (auto find = subs.find(*it); find != subs.end()) { current = find; break; }
   }
   if (current IS subs.end()) Reverse = false;

   // Step through the cycle, wrapping at either end, until a focusable subscriber is found.  The current subscriber is
   // the last candidate, so the focus stays where it is if nothing else can take it.

   extVector *target = nullptr;
   auto it = current;
   for (size_t i=0; i < subs.size(); i++) {
      if (Reverse) {
         if (it IS subs.begin()) it = subs.end();
         --it;
      }
      else if ((it IS subs.end()) or (++it IS subs.end())) it = subs.begin();

      if (tab_focusable(*it)) { target = *it; break; }
   }

   if (not target) return;

   auto target_id = target->UID;
   apply_focus(Self, target);

   // A focus callback may have moved the focus elsewhere or destroyed the target, in which case the event is stale.

   if ((glVectorFocusList.empty()) or (glVectorFocusList.front()->UID != target_id)) return;

   std::vector<OBJECTID> nodes;
   for (auto node=target; node; node=(extVector *)node->Parent) {
      if (node->Class->BaseClassID IS CLASSID::VECTOR) nodes.push_back(node->UID);
      else break;
   }

   for (auto id : nodes) {
      kt::ScopedObjectLock<extVector> vec(id, 1000);
      if (not vec.granted()) continue;
      if ((glVectorFocusList.empty()) or (glVectorFocusList.front()->UID != target_id)) return;
      send_feedback(vec.obj, FM::TAB_FOCUS, glVectorFocusList.front());
   }
}

//********************************************************************************************************************
// Send a dummy input event to the mouse cursor to ensure that changes to underlying vector paths will generate the
// necessary crossing events.

static void send_dummy_input_event(extVectorScene *Self)
{
   if ((Self->SurfaceID) and (Self->RefreshCursor)) {
      double abs_x, abs_y;
      int s_x, s_y;

      Self->RefreshCursor = false;

      if (Self->ButtonLock) {
         // A captured vector is already tracking real pointer movement.  Sending a
         // synthetic movement event here can feed resizing back into scene drawing.
         // Button-release handling recalculates the active vector and cursor later.
         return;
      }

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
}

//********************************************************************************************************************

static void render_to_surface(extVectorScene *Self, objSurface *Surface, objBitmap *Bitmap)
{
   if ((Self->Flags & VPF::MANUAL_RENDER) != VPF::NIL) return;

   auto prev_bitmap = Self->Bitmap;
   Self->Bitmap = Bitmap;

   if ((!Self->PageWidth) or (!Self->PageHeight)) {
      if (Self->Viewport) mark_dirty(Self->Viewport, RC::BASE_PATH|RC::TRANSFORM); // Base-paths need to be recomputed if they use scaled coordinates.
   }

   acDraw(Self);

   Self->Bitmap = prev_bitmap;
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

   if (!Self->Defs.size()) return; // Necessary when dealing with poor quality mapping libs

   for (auto it = Self->Defs.begin(); it != Self->Defs.end(); ) {
      if (it->second IS Object) {
         it = Self->Defs.erase(it);
      }
      else it++;
   }
}

//********************************************************************************************************************

static void clear_def_host_scene(OBJECTPTR Def, extVectorScene *Scene)
{
   if (Def->baseClassID() IS CLASSID::GRADIENT) {
      if (((extGradient *)Def)->HostScene IS Scene) ((extGradient *)Def)->HostScene = nullptr;
      return;
   }

   switch(Def->classID()) {
      case CLASSID::VECTORIMAGE:
         if (((extVectorImage *)Def)->HostScene IS Scene) ((extVectorImage *)Def)->HostScene = nullptr;
         break;
      case CLASSID::VECTORPATH:
         if (((extVectorPath *)Def)->HostScene IS Scene) ((extVectorPath *)Def)->HostScene = nullptr;
         break;
      case CLASSID::VECTORPATTERN:
         if (((extVectorPattern *)Def)->HostScene IS Scene) ((extVectorPattern *)Def)->HostScene = nullptr;
         break;
      case CLASSID::VECTORTRANSITION:
         if (((extVectorTransition *)Def)->HostScene IS Scene) ((extVectorTransition *)Def)->HostScene = nullptr;
         break;
      case CLASSID::VECTORCLIP:
         if (((extVectorClip *)Def)->HostScene IS Scene) ((extVectorClip *)Def)->HostScene = nullptr;
         break;
      default:
         break;
   }
}

//********************************************************************************************************************

static void clear_defs(extVectorScene *Self)
{
   for (auto &entry : Self->Defs) {
      auto def = entry.second;
      if (def) {
         UnsubscribeAction(def, AC::Free);
         clear_def_host_scene(def, Self);
      }
   }

   Self->Defs.clear();
}

//********************************************************************************************************************

static void notify_free(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extVectorScene *)CurrentContext();
   if (Self->SurfaceID IS Object->UID) Self->SurfaceID = 0;
}

//********************************************************************************************************************
// Read the pointer position relative to the scene's surface.  If Dragging is true, the position is only reported when
// a drag and drop operation is active and the pointer is over the surface.

static bool get_pointer_position(extVectorScene *Self, bool Dragging, double &X, double &Y)
{
   if (not Self->SurfaceID) return false;

   bool found = false;
   if (auto pointer = gfx::AccessPointer()) {
      if (pointer->OverObjectID IS Self->SurfaceID) {
         if ((not Dragging) or (pointer->DragSourceID)) {
            X = pointer->OverX;
            Y = pointer->OverY;
            found = true;
         }
      }
      else if (not Dragging) {
         int abs_x, abs_y;
         if (gfx::GetSurfaceCoords(Self->SurfaceID, nullptr, nullptr, &abs_x, &abs_y, nullptr, nullptr) IS ERR::Okay) {
            X = pointer->X - abs_x;
            Y = pointer->Y - abs_y;
            found = true;
         }
      }
      ReleaseObject(pointer);
   }
   return found;
}

//********************************************************************************************************************
// Return the registered viewport under an active drag, with the position translated to the viewport's coordinate
// system.  Pointer capture is deliberately ignored so that a drag source holding the button lock does not obscure the
// destination.

static extVectorViewport * get_drag_target(extVectorScene *Self, double &X, double &Y)
{
   if (not Self->DropRouting) return nullptr;
   if (not get_pointer_position(Self, true, X, Y)) return nullptr;

   if (auto target = get_drop_target_at_xy(Self, X, Y)) {
      auto invert = ~target->Transform;
      invert.transform(&X, &Y);
      return target;
   }
   else return nullptr;
}

//********************************************************************************************************************
// The transfer owns the snapshot; participant destruction expires it without retaining the participant itself.

static void drop_participant_free(OBJECTPTR, ACTIONID, ERR, APTR)
{
   FreeResource(CurrentContext());
}

static void drop_snapshot_free(OBJECTPTR Object, ACTIONID, ERR, APTR, APTR Meta)
{
   std::unique_ptr<std::array<OBJECTID, 2>> participants((std::array<OBJECTID, 2> *)Meta);
   kt::SwitchContext ctx(Object);
   for (auto id : *participants) {
      kt::ScopedObjectLock participant(id);
      if (participant.granted()) UnsubscribeAction(*participant, AC::Free);
   }
}

static ERR prepare_native_drop(OBJECTID SurfaceID, objConfig *Snapshot, double X, double Y, APTR)
{
   auto scene = (extVectorScene *)CurrentContext();
   auto target = get_drop_target_at_xy(scene, X, Y);
   if (not target) return ERR::NoData;
   auto invert = ~target->Transform;
   invert.transform(&X, &Y);

   Snapshot->write("Drop", "Scene", std::to_string(scene->UID));
   Snapshot->write("Drop", "Target", std::to_string(target->UID));
   auto coordinate = [](double Value) {
      std::ostringstream stream;
      stream.precision(17);
      stream << Value;
      return stream.str();
   };
   Snapshot->write("Drop", "X", coordinate(X));
   Snapshot->write("Drop", "Y", coordinate(Y));
   {
      // The module outlives scene destruction, which can itself expire this snapshot.
      auto participants = std::make_unique<std::array<OBJECTID, 2>>(std::array{target->UID, scene->UID});
      kt::SwitchContext ctx(glVectorModule);
      auto error = SubscribeAction(Snapshot, AC::Free, C_FUNCTION(drop_snapshot_free, participants.get()));
      if (error != ERR::Okay) return error;
      participants.release();
   }
   {
      kt::SwitchContext ctx(Snapshot);
      if ((SubscribeAction(target, AC::Free, C_FUNCTION(drop_participant_free)) != ERR::Okay) or
            (SubscribeAction(scene, AC::Free, C_FUNCTION(drop_participant_free)) != ERR::Okay)) return ERR::Failed;
   }
   auto callback = target->vpDropPrepare;
   if (not callback.defined()) return ERR::Okay;
   if (callback.stale()) return ERR::Cancelled;
   callback.pin();
   auto unpin = kt::Defer([&]() { callback.unpin(); });
   ERR result = ERR::Okay;
   if (callback.isC()) {
      kt::SwitchContext ctx(callback.Context);
      auto routine = (ERR (*)(extVectorViewport *, objConfig *, double, double, APTR))callback.Routine;
      result = routine(target, Snapshot, X, Y, callback.Meta);
   }
   else sc::Call(callback, std::to_array<ScriptArg>({
      { "Viewport", target, FD_OBJECTPTR }, { "Snapshot", Snapshot, FD_OBJECTPTR }, { "X", X }, { "Y", Y }
   }), result);
   return result;
}

//********************************************************************************************************************
// Forward a drop on the scene's surface to exactly one registered viewport.  The destination and its local
// coordinates are captured before delivery so that pointer movement during the notification cannot alter them.  The
// delivery is synchronous so that the receiver can retrieve data from the source within the drop's lifetime.

static void notify_dragdrop(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acDragDrop *Args)
{
   auto Self = (extVectorScene *)CurrentContext();
   if ((not Args) or (Self->DropTargetID)) return; // Nested drops are ignored while a delivery is in progress

   double x = 0, y = 0;
   OBJECTID session_id = 0;
   if (auto pointer = gfx::AccessPointer()) {
      pointer->get(strhash("dropSession"), session_id);
      ReleaseObject(pointer);
   }
   extVectorViewport *target = nullptr;
   std::optional<kt::ScopedObjectLock<extVectorViewport>> saved_target;
   OBJECTID target_id = 0, scene_id = 0;
   if (session_id) {
      kt::ScopedObjectLock<objConfig> snapshot(session_id);
      if (snapshot.granted() and
            (snapshot->read("Drop", "Scene", scene_id) IS ERR::Okay) and (scene_id IS Self->UID) and
            (snapshot->read("Drop", "Target", target_id) IS ERR::Okay) and
            (snapshot->read("Drop", "X", x) IS ERR::Okay) and
            (snapshot->read("Drop", "Y", y) IS ERR::Okay)) {
         saved_target.emplace(target_id);
         target = saved_target->granted() ? **saved_target : nullptr;
         if (target and ((target->classID() != CLASSID::VECTORVIEWPORT) or
               (target->Scene != Self) or not target->vpDropTarget)) target = nullptr;
      }
   }
   else {
      if (not get_pointer_position(Self, false, x, y)) return;
      target = get_drop_target_at_xy(Self, x, y);
      if (target) {
         auto invert = ~target->Transform;
         invert.transform(&x, &y);
      }
   }
   if (not target) {
      if (auto pointer = gfx::AccessPointer()) {
         pointer->DragFeedback = 0;
         ReleaseObject(pointer);
      }
      return;
   }

   scene_id = Self->UID;
   Self->DropTargetID = target->UID;
   Self->DropX = x;
   Self->DropY = y;

   acDragDrop(target, Args->Source, Args->Item, Args->Datatype);

   if (CheckResourceExists(scene_id) IS ERR::True) Self->DropTargetID = 0;
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

      kt::ScopedObjectLock<objSurface> surface(Self->SurfaceID);
      if (surface.granted()) surface->scheduleRedraw(0);
   }
}

//********************************************************************************************************************
// Subscribe the scene to key events if it is not already subscribed.  A subscription made while a key event is being
// dispatched does not receive that event, which is recorded so that the scene is not marked to skip it.

static void subscribe_key_events(extVectorScene *Self)
{
   if (Self->KeyHandle) return;

   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);
   SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(scene_key_event, Self), &Self->KeyHandle);
   Self->KeySubscribedDispatch = tlKeyScene ? glKeyDispatch : 0;
   Self->KeyDispatchSkip = 0;
}

//********************************************************************************************************************
// Called when the subscribed Surface loses the focus.  The focused vector is recorded so that it can be restored when
// the surface regains the focus.  The focus list is left alone if it belongs to another scene, as that scene's surface
// may already have received the focus.

static void notify_lostfocus(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extVectorScene *)CurrentContext();
   if (Self->KeyHandle) { UnsubscribeEvent(Self->KeyHandle); Self->KeyHandle = nullptr; }

   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);
   if ((not glVectorFocusList.empty()) and ((extVectorScene *)glVectorFocusList.front()->Scene IS Self)) {
      Self->LastFocusID = glVectorFocusList.front()->UID;
      apply_focus(Self, nullptr);
   }
}

//********************************************************************************************************************
// Called when the subscribed Surface receives the focus.  Keyboard events are only delivered to vectors in the focus
// list, so if none of this scene's vectors has the focus, it is restored to the vector that last held it.  Failing
// that, the first keyboard subscriber in tab order receives the focus.  This ensures that a window receiving the focus
// from the desktop, rather than a click, can process keyboard input.

static void notify_focus(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extVectorScene *)CurrentContext();

   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);
   subscribe_key_events(Self);
   if ((not glVectorFocusList.empty()) and ((extVectorScene *)glVectorFocusList.front()->Scene IS Self)) return;

   if (Self->LastFocusID) {
      kt::ScopedObjectLock<extVector> last(Self->LastFocusID, 1000);
      if ((last.granted()) and ((extVectorScene *)last.obj->Scene IS Self)) {
         apply_focus(Self, last.obj);
         return;
      }
   }

   if (not Self->KeyboardSubscriptions.empty()) apply_focus(Self, *Self->KeyboardSubscriptions.begin());
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
@VectorScene, @Gradient, @VectorImage, @VectorPath, @VectorPattern, @VectorFilter, @VectorTransition,
@VectorClip.

-INPUT-
strview Name: The unique name to associate with the definition.
obj Def: Reference to the definition object.

-ERRORS-
Okay
NullArgs
ResourceExists: The given name is already in use as a definition.
InvalidObject: The definition is not an accepted object class.
UnsupportedOwner: The definition is not owned by the scene.

-TAGS-
mutates-object, retains-input

*********************************************************************************************************************/

static ERR VECTORSCENE_AddDef(extVectorScene *Self, struct sc::AddDef *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Def)) return log.warning(ERR::NullArgs);

   if (Self->HostScene) { // Forward all definitions if a hosting scene is active.
      return Self->HostScene->addDef(Args->Name, Args->Def);
   }

   OBJECTPTR def = Args->Def;
   std::string name(Args->Name);

   if (def->baseClassID() IS CLASSID::GRADIENT) {
      ((extGradient *)def)->HostScene = Self;
   }
   else switch(def->classID()) {
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
      log.warning("The %s must belong to VectorScene #%d, but is owned by object #%d.", def->Class->ClassName.c_str(), Self->UID, def->ownerID());
      return ERR::UnsupportedOwner;
   }

   if (Self->Defs.contains(name)) { // Check that the definition name is unique.
      log.detail("The vector definition name '%s' is already in use.", name.c_str());
      return ERR::ResourceExists;
   }

   log.detail("Adding definition '%s' referencing %s #%d", name.c_str(), def->Class->ClassName.c_str(), def->UID);

   SubscribeAction(def, AC::Free, C_FUNCTION(notify_def_free));

   Self->Defs[name] = def;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Debug: Internal functionality for debugging.

This internal method prints comprehensive information that describes the scene graph to the log.

-ERRORS-
Okay:

-TAGS-
private

*********************************************************************************************************************/

static ERR VECTORSCENE_Debug(extVectorScene *Self)
{
   kt::Log log("debug_tree");

   kt::vector<ChildEntry> list;
   if ((!ListChildren(Self->UID, &list)) and (list.size() > 1)) {
      log.msg("Scene #%d has %d children:", Self->UID, int(std::ssize(list)-1));
      for (auto &rec : list) {
         auto obj = GetObjectPtr(rec.ObjectID);
         if (obj IS Self->Viewport) continue;
         log.msg(" #%d %s %s", rec.ObjectID, obj->Class->ClassName.c_str(), obj->Name);
      }
   }

   if (Self->Viewport) {
      log.msg("Scene #%d scene graph follows:", Self->UID);
      int level = 0;
      debug_tree((extVector *)Self->Viewport, level);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Draw: Renders the scene to a bitmap.

The Draw action will render the scene to the target #Bitmap immediately.  If #Bitmap is NULL, an error will be
returned.

The #RenderTime field will be updated if the `RENDER_TIME` flag is defined.

With `MANUAL_RENDER`, Draw locks and clears the entire target bitmap before rasterising, and does not dispatch
resize or pointer callbacks.  The client must keep the scene graph unchanged until Draw completes.  For direct
window rendering, use the surface's independently owned buffer as #Bitmap and enable `Surface` `MANUAL_RENDER`
to preserve it across automatic redraws.  Call Flush and expose the completed buffer from the main thread.

-ERRORS-
Okay
FieldNotSet: The Bitmap field is NULL.
Recursion: The scene is already drawing or dispatching callbacks.

*********************************************************************************************************************/

static ERR VECTORSCENE_Draw(extVectorScene *Self, struct acDraw *Args)
{
   kt::Log log;

   if (not Self->Bitmap) return log.warning(ERR::FieldNotSet);

   if ((Self->Flags & VPF::MANUAL_RENDER) != VPF::NIL) {
      // Surface exposure also locks its bitmap.  Do not acquire the surface or dispatch callbacks while holding
      // this lock: surface drawing uses the opposite lock order and script callbacks belong on the main thread.
      kt::ScopedObjectLock<objBitmap> bitmap(Self->Bitmap->UID);
      if (not bitmap.granted()) return bitmap.error;
      auto clip = bitmap->Clip;
      bitmap->Clip = { 0, 0, bitmap->Width, bitmap->Height };
      gfx::DrawRectangle(*bitmap, 0, 0, bitmap->Width, bitmap->Height, 0, BAF::FILL);
      render_scene_from_viewport(Self, *bitmap, Self->Viewport);
      bitmap->Clip = clip;
      return ERR::Okay;
   }

   if (Self->BorderlessSurface) {
      // Borderless surfaces get a slightly different approach that cuts down on jitter
      if (!Self->ProcessingMessages) {
         Self->ProcessingMessages = true;
         process_resize_msgs(Self); // All pending resize messages are processed prior to drawing.
         send_dummy_input_event(Self); // Process cursor crossing events
         Self->ProcessingMessages = false;
      }

      render_scene_from_viewport(Self, Self->Bitmap, Self->Viewport);
   }
   else {
      if (!Self->ProcessingMessages) {
         Self->ProcessingMessages = true;

         // Generate current viewport geometry before dispatching resize callbacks.  The callbacks can then update
         // dependent vectors using the current dimensions rather than values retained from the previous frame.

         render_scene_from_viewport(Self, Self->Bitmap, Self->Viewport);

         if (process_resize_msgs(Self) and Self->Bitmap) {
            // Resize callbacks may have dirtied vectors, so redraw the backing bitmap before it is exposed.
            render_scene_from_viewport(Self, Self->Bitmap, Self->Viewport);
         }

         send_dummy_input_event(Self);

         Self->ProcessingMessages = false;
      }
      else return log.warning(ERR::Recursion);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
FindDef: Search for a vector definition by name.

Use the FindDef method to search for a vector definition by name.  A reference to the definition will be returned if
the search is successful.

Definitions are created with the #AddDef() method.

-INPUT-
strview Name: The name of the definition.
&obj Def: A pointer to the definition object is returned here if discovered.

-ERRORS-
Okay
NullArgs
Search: A definition with the given Name was not found.

-TAGS-
pure-query, object-owns-result
-END-

*********************************************************************************************************************/

static ERR VECTORSCENE_FindDef(extVectorScene *Self, struct sc::FindDef *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (Self->HostScene) return Self->HostScene->findDef(Args->Name, &Args->Def);

   std::string_view name = Args->Name;

   if ((!name.empty()) and (name.front() IS '#')) name.remove_prefix(1);
   else if (name.starts_with("url(#")) {
      auto end = name.find(')', 5);
      if (end IS std::string_view::npos) end = name.size();
      std::string lookup(name.substr(5, end - 5));

      if (auto def = Self->Defs.find(lookup); def != Self->Defs.end()) {
         Args->Def = def->second;
         return ERR::Okay;
      }
      else return ERR::Search;
   }

   std::string lookup(name);
   if (auto def = Self->Defs.find(lookup); def != Self->Defs.end()) {
      Args->Def = def->second;
      return ERR::Okay;
   }
   else return ERR::Search;
}

/*********************************************************************************************************************

-ACTION-
Flush: Dispatches pending scene resize and pointer events on the main thread.

Use Flush after a manual Draw has completed and before exposing the surface.  This dispatches pending resize
notifications and refreshes pointer hit testing without rasterising the scene.  If a callback changes geometry, the
client must render another frame before those changes become visible.  Never call Flush concurrently with Draw.

-ERRORS-
Okay
OutsideMainThread
Recursion

*********************************************************************************************************************/

static ERR VECTORSCENE_Flush(extVectorScene *Self)
{
   if (not GetResource(RES::MAIN_THREAD)) return ERR::OutsideMainThread;
   if (Self->ProcessingMessages) return ERR::Recursion;
   Self->ProcessingMessages = true;
   process_resize_msgs(Self);
   send_dummy_input_event(Self);
   Self->ProcessingMessages = false;
   return ERR::Okay;
}

//********************************************************************************************************************

extVectorScene::~extVectorScene()
{
   clear_defs(this);

   // Child vectors skip releasing their resize subscription pins when the scene is collecting, so any records
   // remaining at this point still hold a weak pin on the callback context and must be released here.

   for (auto &view : ResizeSubscriptions) {
      for (auto &sub : view.second) release_callback(sub.second);
   }

   if (Viewport) Viewport->Parent = nullptr;
   if (Buffer)   delete Buffer;
   if (InputHandle) gfx::UnsubscribeInput(InputHandle);

   if (DropRouting) {
      if (SurfaceID) gfx::WindowHook(SurfaceID, WH::DROP_SNAPSHOT, nullptr);
      const std::lock_guard lock(glDropRoutersLock);
      std::erase_if(glDropRouters, [this](const auto &Entry) { return Entry.second IS UID; });
   }

   if (SurfaceID) {
      OBJECTPTR surface;
      if (!AccessObject(SurfaceID, 5000, &surface)) {
         UnsubscribeAction(surface, AC::NIL);
         ReleaseObject(surface);
      }
   }
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

   if ((Self->Flags & VPF::DROP_TARGETS) != VPF::NIL) {
      kt::Log log;
      if (not Self->SurfaceID) return log.warning(ERR::FieldNotSet);

      const std::lock_guard lock(glDropRoutersLock);
      if (glDropRouters.contains(Self->SurfaceID)) {
         log.warning("Scene #%d already routes drops for surface #%d.", glDropRouters[Self->SurfaceID],
            Self->SurfaceID);
         return ERR::InUse;
      }
      glDropRouters[Self->SurfaceID] = Self->UID;
      Self->DropRouting = true;
   }

   if (Self->SurfaceID) {
      kt::ScopedObjectLock<objSurface> surface(Self->SurfaceID, 5000);
      if (surface.granted()) {
         surface->addCallback(C_FUNCTION(render_to_surface));

         if ((!Self->PageWidth) or (!Self->PageHeight)) {
            Self->Flags |= VPF::RESIZE;
            Unit surface_width, surface_height;
            surface->getWidth(surface_width);
            surface->getHeight(surface_height);
            Self->PageWidth = int(surface_width);
            Self->PageHeight = int(surface_height);
         }

         SubscribeAction(*surface, AC::Redimension, C_FUNCTION(notify_redimension));
         SubscribeAction(*surface, AC::Free, C_FUNCTION(notify_free));
         SubscribeAction(*surface, AC::Focus, C_FUNCTION(notify_focus));
         SubscribeAction(*surface, AC::LostFocus, C_FUNCTION(notify_lostfocus));
         if (Self->DropRouting) {
            SubscribeAction(*surface, AC::DragDrop, C_FUNCTION(notify_dragdrop));
            auto prepare = C_FUNCTION(prepare_native_drop);
            gfx::WindowHook(Self->SurfaceID, WH::DROP_SNAPSHOT, &prepare);
         }

         if (surface->hasFocus()) subscribe_key_events(Self);

         SPT presence;
         if ((!surface->getPresence(presence)) and (presence != SPT::HOST)) {
            Self->BorderlessSurface = true;
         }
      }

      auto callback = C_FUNCTION(scene_input_events);
      const auto mask = JTYPE::MOVEMENT|JTYPE::CROSSING|JTYPE::BUTTON|JTYPE::REPEATED|JTYPE::EXT_MOVEMENT|JTYPE::DRAG_ITEM;
      if (gfx::SubscribeInput(&callback, Self->SurfaceID, mask, 0, &Self->InputHandle) != ERR::Okay) {
         return ERR::Function;
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Next: Cycles the focus to the next control.

Moves the focus forward to the next vector in the scene's tab order.  Only vectors that have called
@Vector.SubscribeKeyboard() take part in the cycle, and they are ordered by their @Vector.TabOrder value.  Subscribers
that are disabled or hidden, or that have a disabled or hidden ancestor, are skipped because they cannot receive the
focus.  The cycle starts from the subscriber that currently holds the focus, or the nearest ancestor of the focused
vector that has a subscription.  When the last subscriber in the tab order is reached, the focus wraps around to the
first.

If no subscriber currently has the focus, the first focusable subscriber in the tab order receives it.  If the scene
has no focusable keyboard subscribers, the call has no effect.

This action produces the same result as pressing the Tab key, but works regardless of the `IGNORE_TAB` flag.  The
focused vector and each of its ancestors receive a `TAB_FOCUS` feedback event, even if the focus did not move.

-ERRORS-
Okay
-END-
*********************************************************************************************************************/

static ERR VECTORSCENE_Next(extVectorScene *Self)
{
   cycle_focus(Self, false);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Prev: Cycles the focus to the previous control.

Moves the focus back to the previous vector in the scene's tab order.  This is the reverse of #Next(), and it
selects subscribers and determines the starting point in the same way.  When the first subscriber in the tab order is
reached, the focus wraps around to the last.

If no subscriber currently has the focus, the first focusable subscriber in the tab order receives it.  If the scene
has no focusable keyboard subscribers, the call has no effect.

This action produces the same result as pressing Shift+Tab, but works regardless of the `IGNORE_TAB` flag.  The
focused vector and each of its ancestors receive a `TAB_FOCUS` feedback event, even if the focus did not move.

-ERRORS-
Okay
-END-
*********************************************************************************************************************/

static ERR VECTORSCENE_Prev(extVectorScene *Self)
{
   cycle_focus(Self, true);
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
   clear_defs(Self);
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
Compliance: Selects the standard that the scene complies with, where it differs from the default behaviour.

Compliance declares the standard that the content of the scene was written for.  The default is `NONE`, which
permits Kōtuku's extensions where they differ from the standards.  The @SVG class sets `SVG2` on the scenes that it
creates.

No features currently change their behaviour in response to this setting.  Features that extend a standard by
default will consult it so that the standard behaviour can be restored for the scene.
-END-
*********************************************************************************************************************/

/*********************************************************************************************************************

-FIELD-
Defs: Obtain direct access to the SVG definition table.

Reading the Defs field returns direct access to the SVG definition table, which maps definition names to vector
objects.

Direct access is provided for internal use only and not for the benefit of client programs.

*********************************************************************************************************************/

static ERR GET_Defs(extVectorScene *Self, ankerl::unordered_dense::map<std::string, OBJECTPTR> **Value)
{
   *Value = &Self->Defs;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DropTarget: The registered viewport that will receive a drop at the pointer's position.

When the `DROP_TARGETS` flag is set and a drag and drop operation is active over the scene's surface, DropTarget
refers to the @VectorViewport that would receive the drop if it were released at the pointer's current position.  The
result is computed on demand and reflects registration, visibility and layout changes immediately.  It is
`NULL` if no drag is active, the pointer is not over the surface or no registered viewport is under the pointer.

While a DragDrop action is being delivered to a viewport, DropTarget refers to that viewport and does not change if the
pointer moves during the delivery.  Native drops save the viewport at button release, before asynchronous payload
acquisition.  A destroyed or replaced viewport cannot receive that pending drop through a later hit-test.

*********************************************************************************************************************/

static ERR GET_DropTarget(extVectorScene *Self, OBJECTPTR &Value)
{
   if (Self->DropTargetID) Value = GetObjectPtr(Self->DropTargetID);
   else {
      double x, y;
      Value = get_drag_target(Self, x, y);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DropX: The horizontal drop position, relative to the #DropTarget.

DropX is the horizontal coordinate of the drop, expressed in the coordinate system of the #DropTarget so that
transforms applied to the target and its ancestors are taken into account.  During the delivery of a DragDrop action
the value is a snapshot taken at button release, before native payload acquisition.  Later transforms and layout changes
do not change this value during delivery.  At other times it reflects the pointer's current position,
or zero if there is no #DropTarget.

*********************************************************************************************************************/

static ERR GET_DropX(extVectorScene *Self, double &Value)
{
   if (Self->DropTargetID) Value = Self->DropX;
   else {
      double x, y;
      Value = get_drag_target(Self, x, y) ? x : 0;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DropY: The vertical drop position, relative to the #DropTarget.

DropY is the vertical coordinate of the drop, expressed in the coordinate system of the #DropTarget.  Refer to #DropX
for further information.

*********************************************************************************************************************/

static ERR GET_DropY(extVectorScene *Self, double &Value)
{
   if (Self->DropTargetID) Value = Self->DropY;
   else {
      double x, y;
      Value = get_drag_target(Self, x, y) ? y : 0;
   }
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
RootScene: Returns the top-most VectorScene object.

Returns the top-most VectorScene object according to the object graph.  If the targeted scene has no parent vectors,
it is considered the root and is returned as the value.

If a HostScene is defined, it will be used as the starting point and its parents will be traversed using the same
process.

*********************************************************************************************************************/

static ERR GET_RootScene(extVectorScene *Self, extVectorScene * &Value)
{
   extVectorScene *scene = Self;

   if (scene->HostScene) scene = (extVectorScene *)scene->HostScene;

   OBJECTPTR owner = scene->Owner;
   while (owner) {
      if (owner->classID() IS CLASSID::VECTORSCENE) scene = (extVectorScene *)owner;
      owner = owner->Owner;
   }

   Value = scene;
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

   // A scene that loses the vector focus to another scene records its focused vector, so that the focus can be
   // restored when its surface becomes the primary focus again.  This cannot be left to notify_lostfocus(), because
   // a surface that keeps the focus by inheritance, such as the parent of a focused popup, does not lose the focus.

   if ((Vector) and (!glVectorFocusList.empty())) {
      auto front = glVectorFocusList.front();
      if (auto front_scene = (extVectorScene *)front->Scene; front_scene and (front_scene != Vector->Scene)) {
         front_scene->LastFocusID = front->UID;
      }
   }

   // If the focus moves to another scene while a key event is being dispatched, that scene skips the event unless it
   // has already received it.

   if ((tlKeyScene) and (Vector)) {
      if (auto scene = (extVectorScene *)Vector->Scene; scene and (scene != tlKeyScene) and (scene->KeyHandle)) {
         if ((scene->KeyDispatchSeen != glKeyDispatch) and (scene->KeySubscribedDispatch != glKeyDispatch)) {
            scene->KeyDispatchSkip = glKeyDispatch;
         }
      }
   }

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
         kt::ScopedObjectLock<extVector> vec(fgv, 1000);
         if (vec.granted()) {
            fgv->Flags |= VF::HAS_FOCUS;
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
            kt::ScopedObjectLock<extVector> vec(fv, 1000);
            if (vec.granted()) {
               fv->Flags &= ~VF::HAS_FOCUS;
               send_feedback(fv, FM::LOST_FOCUS, Vector);
            }
         }
         else break;
      }
   }

   glVectorFocusList = std::move(focus_gained);
}

//********************************************************************************************************************

static bool process_resize_msgs(extVectorScene *Self)
{
   kt::Log log(__FUNCTION__);

   bool callback_made = false;
   if (Self->PendingResizeMsgs.size() > 0) {
      std::unordered_set<extVectorViewport *> pending;
      pending.swap(Self->PendingResizeMsgs);

      for (auto view : pending) {
         auto subscriptions = Self->ResizeSubscriptions.find(view);
         if (subscriptions IS Self->ResizeSubscriptions.end()) continue;

         auto list = subscriptions->second; // Take a copy because callbacks can change subscriptions.
         for (auto &record : list) {
            ERR result;
            auto vector = record.first;
            FUNCTION &sub = record.second;

            // Print warnings if the subscription list changed - resizing shouldn't mutate the list, so the developer
            // needs to make an improvement.

            auto live_view = Self->ResizeSubscriptions.find(view);
            if (live_view IS Self->ResizeSubscriptions.end()) {
               log.warning("ResizeEvent subscription was cleared during resize dispatch; skipping stale callback.");
               continue;
            }

            auto live_sub = live_view->second.find(vector);
            if ((live_sub IS live_view->second.end()) or (!(live_sub->second IS sub))) {
               log.warning("ResizeEvent subscription was cleared or replaced during resize dispatch; skipping stale callback.");
               continue;
            }

            if (sub.Context->terminating()) {
               release_callback(live_sub->second);
               live_view->second.erase(live_sub);
               if (live_view->second.empty()) Self->ResizeSubscriptions.erase(live_view);
               continue;
            }

            callback_made = true;

            if (sub.isC()) {
               kt::SwitchContext ctx(sub.Context);
               auto callback = (ERR (*)(extVectorViewport *, objVector *, double, double, double, double, APTR))sub.Routine;
               result = callback(view, vector, view->FinalX, view->FinalY, view->vpFixedWidth, view->vpFixedHeight,
                  sub.Meta);
            }
            else if (sub.isScript()) {
               sc::Call(sub, std::to_array<ScriptArg>({
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
   }

   return callback_made;
}

//********************************************************************************************************************
// Receiver for keyboard events

static ERR vector_keyboard_events(extVector *Vector, const evKey *Event)
{
   for (auto it=Vector->KeyboardSubscriptions->begin(); it != Vector->KeyboardSubscriptions->end(); ) {
      if (input_disabled(Vector)) break;
      ERR result = ERR::Terminate;
      auto &sub = *it;
      if (sub.Context->terminating()) {
         release_callback(sub);
         it = Vector->KeyboardSubscriptions->erase(it);
         continue;
      }

      if (sub.isC()) {
         kt::SwitchContext ctx(sub.Context);
         auto callback = (ERR (*)(objVector *, KQ, KEY, int, APTR))sub.Routine;
         result = callback(Vector, Event->Qualifiers, Event->Code, Event->Unicode, sub.Meta);
      }
      else if (sub.isScript()) {
         // In this implementation the script function will receive all the events chained via the Next field
         sc::Call(sub, std::to_array<ScriptArg>({
            { "Vector",     Vector, FDF_OBJECT },
            { "Qualifiers", int(Event->Qualifiers) },
            { "Code",       int(Event->Code) },
            { "Unicode",    Event->Unicode }
         }), result);
      }

      if (result IS ERR::Terminate) {
         release_callback(sub);
         it = Vector->KeyboardSubscriptions->erase(it);
      }
      else it++;
   }

   if ((Vector->KeyboardSubscriptions->empty()) and (Vector->Scene) and (not Vector->Scene->collecting())) {
      ((extVectorScene *)Vector->Scene)->KeyboardSubscriptions.erase(Vector);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Distribute input events to any vectors that have subscribed and have the focus.
//
// A scene on a hidden surface ignores key events.  Hiding a surface hands its focus to an ancestor, but a host or
// driver can still leave a hidden surface focused, and its scene must not then take the vector focus on Tab.
//
// A scene that received the vector focus while this event was being dispatched to another scene ignores the event,
// as it was meant for the vector that held the focus when the key was pressed.

static void scene_key_event(evKey *Event, int Size, extVectorScene *Self)
{
   RNF surface_flags;
   if (gfx::GetSurfaceFlags(Self->SurfaceID, &surface_flags) != ERR::Okay) return;

   const std::lock_guard<std::recursive_mutex> lock(glVectorFocusLock);

   // The skip marker is consumed by the next call, whether or not it applies, so that it can never suppress a later
   // event.

   Self->KeyDispatchSeen = glKeyDispatch;
   auto skip = Self->KeyDispatchSkip IS glKeyDispatch;
   Self->KeyDispatchSkip = 0;

   if ((skip) or ((surface_flags & RNF::VISIBLE) IS RNF::NIL)) return;

   auto prev_scene = tlKeyScene;
   tlKeyScene = Self;
   deliver_key_event(Event, Self);
   tlKeyScene = prev_scene;
}

//********************************************************************************************************************

static void deliver_key_event(evKey *Event, extVectorScene *Self)
{
   if ((Self->Flags & VPF::IGNORE_TAB) IS VPF::NIL) {
      if ((Event->Code IS KEY::TAB) and ((Event->Qualifiers & (KQ::ALT|KQ::CTRL|KQ::COMMAND)) IS KQ::NIL)) {
         // Tab is intercepted to allow cycling through the keyboard subscribers in tab order.  If the Shift key is
         // held, the order is reversed.

         if ((Event->Qualifiers & KQ::RELEASED) IS KQ::NIL) return;

         cycle_focus(Self, (Event->Qualifiers & KQ::SHIFT) != KQ::NIL);
         return;
      }
   }

   // Iterate over a snapshot because vector_keyboard_events() can erase vectors from KeyboardSubscriptions.

   std::vector<extVector *> subscribers(Self->KeyboardSubscriptions.begin(), Self->KeyboardSubscriptions.end());
   for (auto const vector : subscribers) {
      if (not Self->KeyboardSubscriptions.contains(vector)) continue;
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
   { "RenderTime",   FDF_INT64|FDF_R|FDF_PURE, GET_RenderTime },
   { "Gamma",        FDF_DOUBLE|FDF_RW },
   { "HostScene",    FDF_OBJECT|FDF_RI,   nullptr, nullptr, CLASSID::VECTORSCENE },
   { "Viewport",     FDF_OBJECT|FD_R,     nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Bitmap",       FDF_OBJECT|FDF_RW,   nullptr, SET_Bitmap, CLASSID::BITMAP },
   { "Surface",      FDF_OBJECTID|FDF_RI, nullptr, nullptr, CLASSID::SURFACE },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, nullptr, &clVectorSceneFlags },
   { "PageWidth",    FDF_INT|FDF_RW,      nullptr, SET_PageWidth },
   { "PageHeight",   FDF_INT|FDF_RW,      nullptr, SET_PageHeight },
   { "SampleMethod", FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_SampleMethod, &clVectorSceneSampleMethod },
   { "Compliance",   FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, nullptr, &clVectorSceneCompliance },
   { "RootScene",    FDF_OBJECT|FDF_R,    GET_RootScene, nullptr, CLASSID::VECTORSCENE },
   // Virtual fields
   { "Defs",         FDF_PTR|FDF_SYSTEM|FDF_R|FDF_PURE, GET_Defs, nullptr },
   { "DropTarget",   FDF_OBJECT|FDF_R|FDF_PURE, GET_DropTarget, nullptr, CLASSID::VECTORVIEWPORT },
   { "DropX",        FDF_DOUBLE|FDF_R|FDF_PURE, GET_DropX },
   { "DropY",        FDF_DOUBLE|FDF_R|FDF_PURE, GET_DropY },
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

   if (not clVectorScene) return ERR::AddClass;

   return SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(finish_key_dispatch), &glKeyDispatchHandle);
}
