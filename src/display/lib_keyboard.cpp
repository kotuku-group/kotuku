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

bool pattern_matches(const key_pattern &Pattern, const evKey &Event)
{
   return modifiers_match(Event.Qualifiers, Pattern.required, Pattern.forbidden) and
      ((Pattern.code != KEY::NIL and Pattern.code IS Event.Code) or
       (Pattern.unicode > 0 and Pattern.unicode IS Event.Unicode));
}

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

bool subscription_live(const std::shared_ptr<key_subscription> &Sub)
{
   const std::lock_guard lock(glKeyboardLock);
   auto it = glKeyboardSubscriptions.find(Sub->handle);
   return it != glKeyboardSubscriptions.end() and it->second IS Sub;
}

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
         { "SurfaceID", Sub->surface, FDF_OBJECTID },
         { "Qualifiers", int(Event.Qualifiers) },
         { "Code", int(Event.Code) },
         { "Unicode", Event.Unicode }
      }), result);
   }
   if (result IS ERR::Terminate) gfx::UnsubscribeKeyboard(Sub->handle);
   return result;
}

int scope_depth(const key_subscription &Sub, const key_dispatch &Dispatch)
{
   if (not Sub.surface and not Sub.scope) return int(Dispatch.path.size());
   const auto target = Sub.scope ? Sub.scope : Sub.surface;
   auto it = std::find(Dispatch.path.begin(), Dispatch.path.end(), target);
   if (it IS Dispatch.path.end()) return -1;
   const auto index = int(it - Dispatch.path.begin());
   return Dispatch.owners[index] IS Sub.surface ? index : -1;
}

bool dispatch_current(const key_dispatch &Dispatch)
{
   if (gfx::GetUserFocus() != Dispatch.surface) return false;
   const std::lock_guard lock(glKeyboardLock);
   return glKeyboardGeneration IS Dispatch.generation;
}

void keyboard_focus_changed(evFocus *Event, int Size, APTR Meta)
{
   const auto primary = Event->TotalWithFocus ? Event->FocusList[0] : 0;
   const std::lock_guard lock(glKeyboardLock);
   for (auto &[code, sequence] : glKeyboardSequences) {
      if (sequence.surface != primary) sequence.cancelled = true;
   }
}

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
   std::stable_sort(reservations.begin(), reservations.end(), [&](const auto &A, const auto &B) {
      return scope_depth(*A, dispatch) > scope_depth(*B, dispatch);
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

Action handlers receive input while their surface is in the focus chain.  Object-scoped handlers run from the focused
vector towards its ancestors, followed by surface handlers, then the handlers of each ancestor surface.  Return `KRES::HANDLED` to consume an event, `ERR::Terminate` to unsubscribe,
or zero to continue.  A reservation takes precedence over ordinary handlers regardless of registration order.
Observers receive every event and cannot consume it.  The C callback is `ERR callback(const evKey *, APTR Meta)`;
script callbacks receive SurfaceID, Qualifiers, Code and Unicode.  Remove the handle with ~UnsubscribeKeyboard().

-INPUT-
oid SurfaceID: Surface receiving the actions; zero registers a global fallback or observer.
oid ObjectID: Focus scope within SurfaceID, or zero for the surface.  Vector adapters supply eligible scopes.
int(KBF) Flags: Subscription options.
ptr(func) Callback: Keyboard callback.
&int Handle: Receives the registration handle.

-ERRORS-
Okay
NullArgs
Args
-END-
*********************************************************************************************************************/
ERR SubscribeKeyboard(OBJECTID SurfaceID, OBJECTID ObjectID, KBF Flags, FUNCTION *Callback, int *Handle)
{
   bool retained = false;
   auto consume = kt::Defer([&]() { if (Callback and not retained) Callback->consume(); });
   if (not Callback or not Handle or not Callback->defined()) return ERR::NullArgs;
   // An object scope is only reachable through its owning surface's dispatch path.

   if ((not SurfaceID and ObjectID) or (not SurfaceID and (Flags & (KBF::RESERVE|KBF::FOCUS)) != KBF::NIL) or
       ((Flags & KBF::OBSERVER) != KBF::NIL and (Flags & (KBF::RESERVE|KBF::FOCUS)) != KBF::NIL) or
       ((Flags & KBF::FOCUS) != KBF::NIL and not Callback->isC())) return ERR::Args;
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
   sub->handle = ++glKeyboardHandle;
   sub->surface = SurfaceID;
   sub->scope = ObjectID IS SurfaceID ? 0 : ObjectID;
   sub->flags = Flags;
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

A key sequence owned by a removed registration remains suppressed through its release.

-INPUT-
int Handle: Handle returned by ~SubscribeKeyboard().

-ERRORS-
Okay
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

The registration must use `KBF::RESERVE`.  Set either Code or Unicode, leaving the other zero.  Required modifier groups
such as `KQ::CTRL` match either side; a single-side flag matches that side.  Forbidden modifiers must be absent.  Other
modifiers and lock flags are ignored.  Earlier ancestors take precedence over descendants.  Conflicting registrations
at the same scope return `ERR::AlreadyDefined`; multiple alternatives within one registration are permitted.

-INPUT-
int Handle: Keyboard registration handle.
int(KQ) Required: Required Ctrl, Shift, Alt or Command modifiers.
int(KQ) Forbidden: Forbidden modifiers.
int(KEY) Code: Physical key code, or zero to match Unicode.
int Unicode: Translated character, or zero to match Code.

-ERRORS-
Okay
Args
NotFound
AlreadyDefined
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

Scopes are ordered from the focused leaf towards the root.  An adapter refreshes this list before action dispatch and
when focus changes.  A changed path stops an event already being dispatched from reaching a new focus target.

-INPUT-
oid SurfaceID: Surface containing the scopes.
ptr(int) Objects: Object IDs, or NULL for an empty path.
int Count: Number of object IDs.

-ERRORS-
Okay
Args
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
