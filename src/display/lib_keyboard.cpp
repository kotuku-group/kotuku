/*********************************************************************************************************************

-CATEGORY-
Name: Input
-END-

*********************************************************************************************************************/

#include "defs.h"
#include <map>

namespace {

struct key_pattern {
   KQ required, forbidden;
   KEY code;
   int unicode;
};

struct key_subscription {
   int handle;
   OBJECTID surface, scope;
   KBF flags;
   FUNCTION callback;
   std::vector<key_pattern> patterns;

   ~key_subscription() {
      if (callback.defined()) {
         if (callback.isScript() and not callback.Context->terminating()) {
            ((objScript *)callback.Context)->derefProcedure(callback);
         }
         callback.unpin();
      }
   }
};

struct key_sequence {
   OBJECTID surface, focus;
   int owner;
   uint64_t dispatch;
   bool cancelled = false;
};

// The dispatch path lists each focused surface's vector scopes followed by the surface itself, starting with the
// focused leaf surface and continuing through its ancestors.  Owners identify the surface of each path entry.

struct key_dispatch {
   OBJECTID surface;
   std::vector<OBJECTID> path;
   std::vector<OBJECTID> owners;
   uint64_t generation;
};

std::recursive_mutex glKeyboardLock;
std::map<int, std::shared_ptr<key_subscription>> glKeyboardSubscriptions;
std::map<OBJECTID, std::vector<OBJECTID>> glKeyboardPaths;
std::map<int, key_sequence> glKeyboardSequences;
APTR glKeyboardEvent = nullptr;
APTR glKeyboardFocusEvent = nullptr;
int glKeyboardHandle = 0;
uint64_t glKeyboardDispatch = 0;
uint64_t glKeyboardGeneration = 0;

//********************************************************************************************************************

bool modifiers_match(KQ Value, KQ Required, KQ Forbidden)
{
   if ((Value & Forbidden) != KQ::NIL) return false;

   for (auto group : { KQ::CTRL, KQ::SHIFT, KQ::ALT, KQ::COMMAND }) {
      auto required = Required & group;
      if (required IS group) {
         if ((Value & group) IS KQ::NIL) return false;
      }
      else if ((Value & required) != required) return false;
      Required &= ~group;
   }

   return (Value & Required) IS Required;
}

//********************************************************************************************************************

bool pattern_matches(const key_pattern &Pattern, const evKey &Event)
{
   return modifiers_match(Event.Qualifiers, Pattern.required, Pattern.forbidden) and
      ((Pattern.code != KEY::NIL and Pattern.code IS Event.Code) or
       (Pattern.unicode > 0 and Pattern.unicode IS Event.Unicode));
}

//********************************************************************************************************************
// ReserveKey() requires each pattern to use either a key code or a Unicode value.  Patterns of different kinds are
// not conflicts: a key that matches both is resolved at dispatch by scope depth and registration order.

bool patterns_overlap(const key_pattern &A, const key_pattern &B)
{
   if ((A.code != KEY::NIL) != (B.code != KEY::NIL)) return false;
   if (A.code != B.code or A.unicode != B.unicode) return false;

   for (unsigned qualifiers=0; qualifiers < 512; qualifiers++) {
      if (modifiers_match(KQ(qualifiers), A.required, A.forbidden) and
          modifiers_match(KQ(qualifiers), B.required, B.forbidden)) return true;
   }

   return false;
}

//********************************************************************************************************************

bool subscription_live(const std::shared_ptr<key_subscription> &Sub)
{
   const std::lock_guard lock(glKeyboardLock);
   auto it = glKeyboardSubscriptions.find(Sub->handle);
   return it != glKeyboardSubscriptions.end() and it->second IS Sub;
}

//********************************************************************************************************************

ERR call_key(const std::shared_ptr<key_subscription> &Sub, const evKey &Event)
{
   if (not subscription_live(Sub)) return ERR::Okay;
   kt::ScopedObjectLock context(Sub->callback.Context->UID, 3000);
   if (not context.granted() or context->terminating()) return ERR::Okay;
   if (Sub->scope and CheckResourceExists(Sub->scope) != ERR::True) return ERR::Okay;
   if ((Sub->flags & KBF::NO_REPEAT) != KBF::NIL and (Event.Qualifiers & KQ::REPEAT) != KQ::NIL) {
      return ERR::Okay;
   }

   kt::SwitchContext ctx(*context);
   ERR result = ERR::Okay;
   if (Sub->callback.isC()) {
      auto callback = (ERR (*)(const evKey *, APTR))Sub->callback.Routine;
      result = callback(&Event, Sub->callback.Meta);
   }
   else {
      sc::Call(Sub->callback, std::to_array<ScriptArg>({
         { "SurfaceID",  Sub->surface, FDF_OBJECTID },
         { "Qualifiers", int(Event.Qualifiers) },
         { "Code",       int(Event.Code) },
         { "Unicode",    Event.Unicode }
      }), result);
   }

   if (result IS ERR::Terminate) gfx::UnsubscribeKeyboard(Sub->handle);
   return result;
}

//********************************************************************************************************************

int scope_depth(const key_subscription &Sub, const key_dispatch &Dispatch)
{
   if (not Sub.surface and not Sub.scope) return int(Dispatch.path.size());

   const auto target = Sub.scope ? Sub.scope : Sub.surface;
   auto it = std::find(Dispatch.path.begin(), Dispatch.path.end(), target);

   if (it IS Dispatch.path.end()) return -1;
   const auto index = int(it - Dispatch.path.begin());

   return Dispatch.owners[index] IS Sub.surface ? index : -1;
}

//********************************************************************************************************************

bool dispatch_current(const key_dispatch &Dispatch)
{
   if (gfx::GetUserFocus() != Dispatch.surface) return false;
   const std::lock_guard lock(glKeyboardLock);
   return glKeyboardGeneration IS Dispatch.generation;
}

//********************************************************************************************************************

void keyboard_focus_changed(evFocus *Event, int Size, APTR Meta)
{
   const auto primary = Event->TotalWithFocus ? Event->FocusList[0] : 0;
   const std::lock_guard lock(glKeyboardLock);
   for (auto &[code, sequence] : glKeyboardSequences) {
      if (sequence.surface != primary) sequence.cancelled = true;
   }
}

//********************************************************************************************************************

void route_keyboard(evKey *Event, int Size, APTR Meta)
{
   if (Size < int(sizeof(evKey))) return;

   const auto event = *Event;
   key_dispatch dispatch { .surface = gfx::GetUserFocus(), .path = {}, .owners = {}, .generation = 0 };

   uint64_t dispatch_id;
   std::vector<std::shared_ptr<key_subscription>> subscriptions;

   {
      const std::lock_guard lock(glKeyboardLock);
      dispatch_id = ++glKeyboardDispatch;
      for (auto &[handle, sub] : glKeyboardSubscriptions) subscriptions.push_back(sub);
   }

   // Observers always see the event, including reserved keys and events without an active surface.

   for (auto &sub : subscriptions) {
      if ((sub->flags & KBF::OBSERVER) != KBF::NIL) call_key(sub, event);
   }

   if (not dispatch.surface or gfx::GetUserFocus() != dispatch.surface) return;

   RNF surface_flags;
   if (gfx::GetSurfaceFlags(dispatch.surface, &surface_flags) != ERR::Okay or
       (surface_flags & (RNF::VISIBLE|RNF::DISABLED)) != RNF::VISIBLE) return;

   // Surfaces in the focus chain receive actions after the focused leaf, nearest ancestor first.

   std::vector<OBJECTID> chain;
   {
      const std::lock_guard lock(glFocusLock);
      chain = glFocusList;
   }
   if (chain.empty() or chain.front() != dispatch.surface) return;

   // The vector adapters refresh the eligible ancestry without a Display dependency on Vector.

   for (auto &sub : subscriptions) {
      if ((sub->flags & KBF::FOCUS) IS KBF::NIL) continue;
      if (std::find(chain.begin(), chain.end(), sub->surface) != chain.end()) call_key(sub, event);
   }

   {
      const std::lock_guard lock(glKeyboardLock);
      for (auto surface_id : chain) {
         if (auto path = glKeyboardPaths.find(surface_id); path != glKeyboardPaths.end()) {
            dispatch.path.insert(dispatch.path.end(), path->second.begin(), path->second.end());
            dispatch.owners.insert(dispatch.owners.end(), path->second.size(), surface_id);
         }
         dispatch.path.push_back(surface_id);
         dispatch.owners.push_back(surface_id);
      }
      dispatch.generation = glKeyboardGeneration;
   }

   const auto focus = dispatch.path.front();
   const bool released = (event.Qualifiers & KQ::RELEASED) != KQ::NIL;
   const bool repeated = (event.Qualifiers & KQ::REPEAT) != KQ::NIL;
   std::shared_ptr<key_subscription> owner;
   bool known = false;

   {
      const std::lock_guard lock(glKeyboardLock);
      auto sequence = glKeyboardSequences.find(int(event.Code));

      if ((released or repeated) and sequence != glKeyboardSequences.end()) {
         known = true;
         auto saved = sequence->second;

         if (released) glKeyboardSequences.erase(sequence);
         if (saved.cancelled or saved.surface != dispatch.surface) return;

         if (saved.owner) {
            auto it = glKeyboardSubscriptions.find(saved.owner);
            if (it IS glKeyboardSubscriptions.end() or scope_depth(*it->second, dispatch) < 0) return;
            owner = it->second;
         }
         else if (saved.focus != focus) return;
      }
      if (not released and not known) {
         glKeyboardSequences[int(event.Code)] = { dispatch.surface, focus, 0, dispatch_id };
      }
   }

   if (owner) {
      call_key(owner, event);
      return;
   }

   std::vector<std::shared_ptr<key_subscription>> reservations, ordinary;

   for (auto &sub : subscriptions) {
      if ((sub->flags & (KBF::OBSERVER|KBF::FOCUS)) != KBF::NIL or scope_depth(*sub, dispatch) < 0) continue;

      if ((sub->flags & KBF::RESERVE) != KBF::NIL) {
         // A registration cannot claim the release/repeat of an ordinary press delivered earlier.
         if (released or known) continue;
         const std::lock_guard lock(glKeyboardLock);
         if (std::any_of(sub->patterns.begin(), sub->patterns.end(), [&](const auto &Pattern) {
               return pattern_matches(Pattern, event);
            })) reservations.push_back(sub);
      }
      else ordinary.push_back(sub);
   }

   auto order = [&](const auto &A, const auto &B) {
      auto a = scope_depth(*A, dispatch), b = scope_depth(*B, dispatch);
      if (a != b) return a < b;
      auto a_default = (A->flags & KBF::DEFAULT) != KBF::NIL;
      auto b_default = (B->flags & KBF::DEFAULT) != KBF::NIL;
      return a_default != b_default ? not a_default : A->handle < B->handle;
   };

   std::stable_sort(ordinary.begin(), ordinary.end(), order);

   // Override reservations come first, innermost scope first.  Other reservations follow, outermost scope first.

   std::stable_sort(reservations.begin(), reservations.end(), [&](const auto &A, const auto &B) {
      auto a_override = (A->flags & KBF::OVERRIDE) != KBF::NIL;
      auto b_override = (B->flags & KBF::OVERRIDE) != KBF::NIL;
      if (a_override != b_override) return a_override;
      auto a = scope_depth(*A, dispatch), b = scope_depth(*B, dispatch);
      return a_override ? a < b : a > b;
   });

   auto record_owner = [&](int Handle) {
      if (released) return;
      const std::lock_guard lock(glKeyboardLock);
      auto it = glKeyboardSequences.find(int(event.Code));
      if (it != glKeyboardSequences.end() and it->second.dispatch IS dispatch_id) it->second.owner = Handle;
   };

   for (auto &sub : reservations) {
      if (not dispatch_current(dispatch)) return;
      if (not subscription_live(sub)) continue;
      record_owner(sub->handle);
      call_key(sub, event);
      return;
   }

   for (auto &sub : ordinary) {
      if (not dispatch_current(dispatch)) return;
      if (call_key(sub, event) IS ERR(int(KRES::HANDLED))) {
         record_owner(sub->handle);
         return;
      }
   }
}

} // namespace

//********************************************************************************************************************

void free_keyboard()
{
   std::map<int, std::shared_ptr<key_subscription>> subscriptions;
   {
      const std::lock_guard lock(glKeyboardLock);
      subscriptions.swap(glKeyboardSubscriptions);
      glKeyboardPaths.clear();
      glKeyboardSequences.clear();
      if (glKeyboardEvent) { UnsubscribeEvent(glKeyboardEvent); glKeyboardEvent = nullptr; }
      if (glKeyboardFocusEvent) { UnsubscribeEvent(glKeyboardFocusEvent); glKeyboardFocusEvent = nullptr; }
   }
}

namespace gfx {

/*********************************************************************************************************************

-FUNCTION-
SubscribeKeyboard: Registers a keyboard action handler or observer.

SubscribeKeyboard() registers a callback with the keyboard broker, which routes each key event to the registrations
that are eligible under the current user focus.  The handle returned in `Handle` must be released with
~UnsubscribeKeyboard() when the registration is no longer required.

A registration is bound to a scope.  If `SurfaceID` is set and `ObjectID` is zero, the scope is the surface itself.  If
`ObjectID` is set, the scope is that object within the surface, normally a vector, and the registration is eligible
only while the object is in the surface's active focus path (as supplied by ~SetKeyboardFocus()).  If both values are
zero, the registration is a global fallback that runs after all surface-bound registrations.  The callback is skipped
if the `ObjectID` scope no longer exists.

An `ObjectID` scope, `KBF::RESERVE` and `KBF::FOCUS` all require a `SurfaceID`.  `KBF::OBSERVER` cannot be combined
with `KBF::RESERVE` or `KBF::FOCUS`, `KBF::OVERRIDE` requires `KBF::RESERVE`, and `KBF::FOCUS` is restricted to C
callbacks.

Key events are dispatched in the following order:

<list type="ordered">
<li>Registrations with the `OBSERVER` flag receive every key event, whether or not a surface has the focus.  Their
result is ignored, so an observer cannot consume input.</li>
<li>If no surface has the focus, or the focused surface is hidden or disabled, dispatch stops.</li>
<li>Registrations with the `RESERVE` flag are tested against the patterns added by ~ReserveKey().  The first match
receives the event exclusively and no other handler is called.  Where reservations in several scopes match, the scope
furthest from the focused object wins, unless a matching reservation has the `OVERRIDE` flag.  Those take precedence
over all others, and among them the scope nearest the focused object wins.</li>
<li>All other registrations in the focus path are called in turn, starting with the focused object and moving outwards
through its enclosing scopes, its surface and that surface's ancestors.  Within one scope, `DEFAULT` registrations run
after the others, and registrations of the same kind run in the order they were created.  Dispatch stops when a
callback returns `KRES::HANDLED`.</li>
</list>

A press and its repeats and release are treated as one sequence.  When a press is reserved or consumed, its repeats and
release go only to the registration that took it, for as long as that registration remains in the focus path.  If the
focus moves to another surface or scope before the release, the rest of the sequence is discarded.  A reservation
created part-way through a sequence does not take the remainder of that sequence.

The C++ prototype for `Callback` is `ERR callback(const evKey *Event, APTR Meta)`.  Script callbacks receive the
arguments `SurfaceID`, `Qualifiers`, `Code` and `Unicode`, in which `Qualifiers` are `KQ` flags, `Code` is the raw
`KEY` value and `Unicode` is the character produced by translating the key through the user's keymap.  The callback
runs in the context of the object that owns it.  It can return `KRES::PASS` (`ERR::Okay`) to pass the event on,
`KRES::HANDLED` to consume it, or `ERR::Terminate` to end the registration.  `ERR::Terminate` does not consume the
event.

!KBF

-INPUT-
oid SurfaceID: Surface that the registration is bound to.  Zero creates a global fallback or observer.
oid ObjectID: Object within `SurfaceID` that defines the focus scope, or zero to use the surface itself.
int(KBF) Flags: Optional flags.
ptr(func) Callback: Function that receives key events.
&int Handle: Receives the registration handle.

-ERRORS-
Okay
NullArgs: `Callback` or `Handle` is `NULL`, or `Callback` is undefined.
Args: `SurfaceID` is not a @Surface, or the combination of `SurfaceID`, `ObjectID` and `Flags` is not permitted.

-TAGS-
callback-held
-END-

*********************************************************************************************************************/

ERR SubscribeKeyboard(OBJECTID SurfaceID, OBJECTID ObjectID, KBF Flags, FUNCTION *Callback, int *Handle)
{
   bool retained = false;

   auto consume = kt::Defer([&]() {
      if (Callback and not retained) Callback->consume();
   });

   if ((not Callback) or (not Handle) or (not Callback->defined())) return ERR::NullArgs;

   // An object scope is only reachable through its owning surface's dispatch path.

   if (not SurfaceID and ObjectID) return ERR::Args;
   if (not SurfaceID and (Flags & (KBF::RESERVE|KBF::FOCUS)) != KBF::NIL) return ERR::Args;
   if ((Flags & KBF::OBSERVER) != KBF::NIL and (Flags & (KBF::RESERVE|KBF::FOCUS)) != KBF::NIL) return ERR::Args;
   if ((Flags & KBF::FOCUS) != KBF::NIL and not Callback->isC()) return ERR::Args;
   if ((Flags & KBF::OVERRIDE) != KBF::NIL and (Flags & KBF::RESERVE) IS KBF::NIL) return ERR::Args;

   if (SurfaceID and GetClassID(SurfaceID) != CLASSID::SURFACE) return ERR::Args;

   const std::lock_guard lock(glKeyboardLock);
   if (not glKeyboardEvent) {
      kt::SwitchContext context(glDisplayContext);
      auto error = SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(route_keyboard), &glKeyboardEvent);
      if (error != ERR::Okay) return error;
   }

   if (not glKeyboardFocusEvent) {
      kt::SwitchContext context(glDisplayContext);
      auto error = SubscribeEvent(EVID_GUI_SURFACE_FOCUS, C_FUNCTION(keyboard_focus_changed), &glKeyboardFocusEvent);
      if (error != ERR::Okay) return error;
   }

   auto sub = std::make_shared<key_subscription>();
   sub->handle   = ++glKeyboardHandle;
   sub->surface  = SurfaceID;
   sub->scope    = ObjectID IS SurfaceID ? 0 : ObjectID;
   sub->flags    = Flags;
   sub->callback = *Callback;
   Callback->pin();

   retained = true;
   *Handle = sub->handle;
   glKeyboardSubscriptions.emplace(sub->handle, std::move(sub));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
UnsubscribeKeyboard: Removes a keyboard registration and its reservations.

Removes a registration created by ~SubscribeKeyboard(), together with any key combinations added to it by
~ReserveKey(), and releases its callback.  The callback is not called again once this function returns, so it is safe
to call this function from inside the registration's own callback.  An unknown or already-removed handle is ignored.

-INPUT-
int Handle: Handle returned by ~SubscribeKeyboard().

-ERRORS-
Okay

-TAGS-
idempotent
-END-

*********************************************************************************************************************/

ERR UnsubscribeKeyboard(int Handle)
{
   std::shared_ptr<key_subscription> removed;
   {
      const std::lock_guard lock(glKeyboardLock);
      auto it = glKeyboardSubscriptions.find(Handle);
      if (it != glKeyboardSubscriptions.end()) {
         removed = std::move(it->second);
         glKeyboardSubscriptions.erase(it);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
ReserveKey: Adds a key combination to an exclusive keyboard registration.

ReserveKey() adds a key combination to a registration that was created with the `KBF::RESERVE` flag.  While the
registration's scope is in the focus path, a matching key press goes only to that registration, ahead of every ordinary
handler (see ~SubscribeKeyboard()).  A registration can hold several combinations; call ReserveKey() once for each.

A combination identifies the key by exactly one of `Code` or `Unicode`.  Use `Code` to match a physical key regardless
of keyboard layout, or `Unicode` to match the character the key produces.

Modifiers are limited to the Ctrl, Shift, Alt and Command groups of `KQ`.  A group value such as `KQ::CTRL` in
`Required` is satisfied by either the left or right key, whereas a side-specific value such as `KQ::L_CTRL` requires
that side.  The event must not carry any modifier in `Forbidden`.  Modifiers not mentioned in either mask are ignored,
so Ctrl+= with no forbidden modifiers also matches Ctrl+Shift+=.

Two registrations with the same surface and scope cannot reserve combinations that could match the same event, and the
second attempt fails with `ERR::AlreadyDefined`.  A `Code` combination and a `Unicode` combination are not treated as a
conflict, even if one key event could match both.

-INPUT-
int Handle: Handle of a `KBF::RESERVE` registration, as returned by ~SubscribeKeyboard().
int(KQ) Required: Modifiers that must be held.
int(KQ) Forbidden: Modifiers that must not be held.
int(KEY) Code: Key code to match, or zero if `Unicode` is used.
int Unicode: Unicode character to match, or zero if `Code` is used.

-ERRORS-
Okay
Args: The combination is invalid or can never match, or the registration lacks `KBF::RESERVE`.
NotFound: `Handle` does not refer to an active registration.
AlreadyDefined: The combination overlaps one reserved by another registration with the same surface and scope.
-END-

*********************************************************************************************************************/

ERR ReserveKey(int Handle, KQ Required, KQ Forbidden, int Code, int Unicode)
{
   constexpr auto modifiers = KQ::CTRL|KQ::SHIFT|KQ::ALT|KQ::COMMAND;

   if ((Code IS 0) IS (Unicode IS 0) or Unicode < 0 or Unicode > 0x10ffff or
       ((Required|Forbidden) & ~modifiers) != KQ::NIL) return ERR::Args;

   key_pattern pattern { Required, Forbidden, KEY(Code), Unicode };

   bool possible = false;
   for (unsigned value=0; value < 512; value++) {
      if (modifiers_match(KQ(value), Required, Forbidden)) { possible = true; break; }
   }

   if (not possible) return ERR::Args;

   const std::lock_guard lock(glKeyboardLock);
   auto found = glKeyboardSubscriptions.find(Handle);
   if (found IS glKeyboardSubscriptions.end()) return ERR::NotFound;

   auto &sub = found->second;
   if ((sub->flags & KBF::RESERVE) IS KBF::NIL) return ERR::Args;

   for (auto &[handle, other] : glKeyboardSubscriptions) {
      if (handle IS Handle or other->surface != sub->surface or other->scope != sub->scope) continue;
      for (auto &existing : other->patterns) {
         if (patterns_overlap(pattern, existing)) return ERR::AlreadyDefined;
      }
   }

   sub->patterns.push_back(pattern);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetKeyboardFocus: Supplies eligible keyboard scopes from a vector focus adapter.
Status: Internal

SetKeyboardFocus() is called by the vector module's focus adapter to tell the keyboard broker which objects in a
surface are in the focus path.  `Objects` lists the focused object first, followed by each enclosing scope in turn.
The list replaces any path previously set for `SurfaceID`, and an empty list clears it.  Registrations made by
~SubscribeKeyboard() with an `ObjectID` scope are eligible only while that object is in this path.

If the path changes, any key event currently being dispatched for the surface stops at its next handler.  Unfinished
press sequences are also discarded if the scope that owns them has left the path, or, for unconsumed presses, if the
focused object has changed.  Passing the path that is already set has no effect.

-INPUT-
oid SurfaceID: Surface that contains the objects.
ptr(int) Objects: Object IDs in order from the focused object outwards, or `NULL` if `Count` is zero.
int Count: Number of entries in `Objects`.

-ERRORS-
Okay
Args: `Count` is negative, or `Objects` is `NULL` when `Count` is not zero.
-END-

*********************************************************************************************************************/

ERR SetKeyboardFocus(OBJECTID SurfaceID, OBJECTID *Objects, int Count)
{
   if (Count < 0 or (Count and not Objects)) return ERR::Args;

   std::vector<OBJECTID> path;

   if (Count) path.assign(Objects, Objects + Count);

   const std::lock_guard lock(glKeyboardLock);
   auto existing = glKeyboardPaths.find(SurfaceID);
   if (existing != glKeyboardPaths.end() ? existing->second != path : not path.empty()) {
      for (auto &[code, sequence] : glKeyboardSequences) {
         if (sequence.surface != SurfaceID) continue;

         if (path.empty()) sequence.cancelled = true;
         else if (sequence.owner) {
            auto owner = glKeyboardSubscriptions.find(sequence.owner);
            if (owner IS glKeyboardSubscriptions.end()) continue;
            auto scope = owner->second->scope;
            if (scope and scope != SurfaceID and std::find(path.begin(), path.end(), scope) IS path.end()) {
               sequence.cancelled = true;
            }
         }
         else if (sequence.focus != path.front()) sequence.cancelled = true;
      }

      if (path.empty()) glKeyboardPaths.erase(SurfaceID);
      else glKeyboardPaths[SurfaceID] = std::move(path);
      ++glKeyboardGeneration;
   }
   return ERR::Okay;
}

} // namespace gfx
