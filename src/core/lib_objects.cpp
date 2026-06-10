/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: Objects
-END-

*********************************************************************************************************************/

#ifdef __unix__
#include <unistd.h>
#ifndef __ANDROID__
#include <sys/msg.h>
#endif
#endif

#include <stdlib.h>
#include <cassert>
#include <chrono>
#include <thread>
#include <ranges>

#include "defs.h"

using namespace kt;

static void drain_action_queue(OBJECTID ObjectID, bool Terminating = false, bool PreserveActive = false);
static void async_wait_callback(OBJECTID, bool);

// AsyncWait() state — guarded by glmAsyncWait.

static std::atomic<int> glAsyncWaitCounter;
static std::atomic<bool> glAsyncWaiting;
static ankerl::unordered_dense::set<OBJECTID> glAsyncWaitTargets;
static std::mutex glmAsyncWait;

//********************************************************************************************************************

void stop_async_actions(void)
{
   {
      std::lock_guard<std::recursive_mutex> lock(glmAsyncActions);
      if (not glAsyncThreads.empty()) {
         kt::Log log(__FUNCTION__);
         log.msg("Stopping %d async action threads...", int(glAsyncThreads.size()));

         for (auto &thread_ptr : glAsyncThreads) {
            if (thread_ptr and thread_ptr->joinable()) {
               thread_ptr->request_stop();
            }
         }

         // Give threads time to respond to stop request
         constexpr auto STOP_TIMEOUT = std::chrono::milliseconds(2000);
         constexpr auto POLL_INTERVAL = std::chrono::milliseconds(50);
         auto start_time = std::chrono::steady_clock::now();

         while (not glAsyncThreads.empty() and (std::chrono::steady_clock::now() - start_time) < STOP_TIMEOUT) {
            // Remove completed threads
            std::erase_if(glAsyncThreads, [](const auto &ptr) { return !ptr or !ptr->joinable(); });

            if (not glAsyncThreads.empty()) {
               std::this_thread::sleep_for(POLL_INTERVAL);
            }
         }

         if (not glAsyncThreads.empty()) {
            log.warning("%d action threads failed to stop in time.", int(glAsyncThreads.size()));
         }

         glAsyncThreads.clear();
      }
   }

   // Clear any remaining queued actions (no callbacks are sent during shutdown).
   {
      std::lock_guard<std::mutex> lock(glmActionQueue);
      glActionQueues.clear();
      glActiveAsyncObjects.clear();
      glCancelledAsyncObjects.clear();
      glAsyncObjectThreads.clear();
   }
}

//********************************************************************************************************************
// These globals pertain to action subscriptions.  Variables are global to all threads, so need to be locked via
// glSubLock.

struct subscription {
   OBJECTID ObjectID;
   ACTIONID ActionID;
   FUNCTION Callback;

   subscription(OBJECTID pObject, ACTIONID pAction, FUNCTION pCallback) :
      ObjectID(pObject), ActionID(pAction), Callback(pCallback) { }
};

struct unsubscription {
   OBJECTID ObjectID;
   ACTIONID ActionID;

   unsubscription(OBJECTID pObject, ACTIONID pAction) : ObjectID(pObject), ActionID(pAction) { }
};

static std::recursive_mutex glSubLock; // The following variables are locked by this mutex
static ankerl::unordered_dense::map<OBJECTID, ankerl::unordered_dense::map<int, std::vector<ActionSubscription> > > glSubscriptions;
static std::vector<unsubscription> glDelayedUnsubscribe;
static std::vector<subscription> glDelayedSubscribe;
static int glSubReadOnly = 0; // To prevent modification of glSubscriptions

static void free_children(OBJECTPTR Object);

//********************************************************************************************************************

static void async_wait_callback(OBJECTID ObjectID, bool Active)
{
   bool break_wait = false;

   {
      std::lock_guard<std::mutex> lock(glmAsyncWait);
      if (not glAsyncWaiting.load(std::memory_order_relaxed)) return;
      if (not glAsyncWaitTargets.contains(ObjectID)) return;

      if (Active) glAsyncWaitCounter.fetch_add(1, std::memory_order_relaxed);
      else glAsyncWaitCounter.fetch_sub(1, std::memory_order_relaxed);

      break_wait = (glAsyncWaitCounter.load(std::memory_order_relaxed) IS 0);
   }

   if (break_wait) SendMessage(MSGID::BREAK, MSF::NIL, nullptr, 0);
}

//********************************************************************************************************************
// Hook for MSGID::FREE, used for delaying collection until the next message processing cycle.

ERR msg_free(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   // Lock the object via conventional means to guarantee thread safety.
   OBJECTPTR obj;
   if (AccessObject(((OBJECTID *)Message)[0], 10000, &obj) IS ERR::Okay) {
      // Use PermitTerminate to inform object_free() that the object can be terminated safely while the lock is held.
      obj->setFlag(NF::PERMIT_TERMINATE);
      FreeResource(obj);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Releases the heap block backing an object.  Object memory is allocated directly in NewObject() and must only be
// released once all object locks and contexts have been released (see object_free()).

static void free_object_block(OBJECTPTR Object)
{
   if (not Object) return;
   Object->~Object();
   aligned_block_free(Object);
}

//********************************************************************************************************************
// Object termination hook for FreeResource()

ERR object_free(ResourceRecord &Resource, Object *Object)
{
   kt::Log log("Free");

   {
      ScopedObjectAccess objlock(Object);
      if (not objlock.granted()) return ERR::AccessObject;

      extObjectContext new_context(Object, AC::Free);

      auto mc = Object->ExtClass;

      // If the object is locked then we mark it for collection and return.
      // Collection is achieved via the message queue for maximum safety.

      if (((Object->Queue > 1) or (Object->isPinned())) and (not Object->defined(NF::PERMIT_TERMINATE))) {
         log.detail("Object #%d locked/pinned; marking for deletion.", Object->UID);
         if ((Object->Owner) and (Object->Owner->collecting())) Object->Owner = nullptr; // The Owner pointer is no longer safe to use
         Object->setFlag(NF::FREE_ON_UNLOCK);
         Resource.CollectOnUnlock = true; // Defer destruction until the object is unlocked
         return ERR::InUse;
      }

      if (Object->terminating()) {
         log.trace("Object already being terminated.");
         return ERR::InUse;
      }

#ifndef NDEBUG
      if (Object->ActionDepth > 0) {
         // The object is still in use.  This indicates that the object wasn't locked with LockObject() or pinned, which
         // is considered a critical error.
         log.error("Cannot free object #%d; current state: in use without a lock.", Object->UID);
         return ERR::InUse;
      }
#endif

      if (Object->classID() IS CLASSID::METACLASS)   log.branch("%s, Owner: %d", Object->className(), Object->ownerID());
      else if (Object->classID() IS CLASSID::MODULE) log.branch("%s, Owner: %d", ((extModule *)Object)->Name.c_str(), Object->ownerID());
      else if (Object->Name[0])                      log.branch("Name: %s, Owner: %d", Object->Name, Object->ownerID());
      else log.branch("Owner: %d", Object->ownerID());

      // If the object wants to be warned when the free process is about to be executed, it will subscribe to the
      // FreeWarning action.  The process can be aborted by returning ERR::InUse.

      if (mc->ActionTable[int(AC::FreeWarning)].PerformAction) {
         if (mc->ActionTable[int(AC::FreeWarning)].PerformAction(Object, nullptr) IS ERR::InUse) {
            if (Object->collecting()) {
               // If the object is marked for deletion then it is not possible to avoid destruction (this prevents objects
               // from locking up the shutdown process).

               log.msg("Object will be destroyed despite being in use.");
            }
            else {
               if ((Object->Owner) and (Object->Owner->collecting())) Object->Owner = nullptr;
               return ERR::InUse;
            }
         }
      }

      if (mc->Base) { // Derived class detected, so call the base class
         if (mc->Base->ActionTable[int(AC::FreeWarning)].PerformAction) {
            if (mc->Base->ActionTable[int(AC::FreeWarning)].PerformAction(Object, nullptr) IS ERR::InUse) {
               if (Object->collecting()) {
                  // If the object is marked for deletion then it is not possible to avoid destruction (this prevents
                  // objects from locking up the shutdown process).
                  log.msg("Object will be destroyed despite being in use.");
               }
               else {
                  if ((Object->Owner) and (Object->Owner->collecting())) Object->Owner = nullptr;
                  return ERR::InUse;
               }
            }
         }
      }

      // Object destruction is guaranteed; queued async actions can be cancelled safely.

      drain_action_queue(Object->UID, true);

      // Mark the object as being in the free process.  The mark prevents any further access to the object via
      // AccessObject().  Classes may also use the flag to check if an object is in the process of being freed.

      Object->setFlag(NF::FREE);
      Object->clearFlag(NF::FREE_ON_UNLOCK);

      NotifySubscribers(Object, AC::Free, nullptr, ERR::Okay);

      if (mc->ActionTable[int(AC::Free)].PerformAction) {  // Could be derived class or base-class
         mc->ActionTable[int(AC::Free)].PerformAction(Object, nullptr);
      }

      if (mc->Base) { // Derived class detected, so call the base class
         if (mc->Base->ActionTable[int(AC::Free)].PerformAction) {
            mc->Base->ActionTable[int(AC::Free)].PerformAction(Object, nullptr);
         }
      }

      if (mc->ActionTable[int(AC::FreePlacement)].PerformAction) {
         mc->ActionTable[int(AC::FreePlacement)].PerformAction(Object, nullptr);
      }
      else if ((mc->Base) and (mc->Base->ActionTable[int(AC::FreePlacement)].PerformAction)) { // Fall-back to base class
         mc->Base->ActionTable[int(AC::FreePlacement)].PerformAction(Object, nullptr);
      }

      if (Object->NotifyFlags.load()) {
         const std::lock_guard<std::recursive_mutex> lock(glSubLock);
         glSubscriptions.erase(Object->UID);
      }

      if (Object->DerivedPtr) {
         if (FreeResource(Object->DerivedPtr) != ERR::Okay) log.warning("Invalid DerivedPtr address %p.", Object->DerivedPtr);
         Object->DerivedPtr = nullptr;
      }

      free_children(Object);

      if (Object->defined(NF::TIMER_SUB)) {
         if (auto lock = std::unique_lock{glmTimer, 1000ms}) {
            for (auto it=glTimers.begin(); it != glTimers.end(); ) {
               if (it->SubscriberID IS Object->UID) {
                  log.warning("%s object #%d has an unfreed timer subscription, routine %p, interval %" PF64,
                     mc->ClassName.c_str(), Object->UID, &it->Routine, (long long)it->Interval);
                  if (it->Routine.isScript()) {
                     ((objScript *)it->Routine.Context)->derefProcedure(it->Routine);
                  }
                  it = glTimers.erase(it);
               }
               else it++;
            }
         }
      }

      if ((mc->Base) and (mc->Base->OpenCount > 0)) mc->Base->OpenCount--; // Derived class detected
      if (mc->OpenCount > 0) mc->OpenCount--;

      if (Object->Name[0]) { // Remove the object from the name lookup list
         if (auto olock = std::unique_lock{glmObjectLookup, 4s}) {
            remove_object_hash(Object);
         }
      }

      // Clear the object header to help raise use-after-free errors.

      Object->Class = nullptr;
      Object->UID   = 0;

      {
         std::lock_guard lock(glmObjects);
         glObjects.erase(Resource.ResourceID);
      }
   } // Object lock

   free_object_block(Object);
   return ERR::Okay;
}

//********************************************************************************************************************

void object_add_child(ResourceRecord &Parent, ResourceRecord &Child)
{
   std::lock_guard lock(glmObjects);

   if (auto parent_object = glObjects.find(Parent.ResourceID); parent_object != glObjects.end()) {
      if (Child.Manager IS &glResourceObject) parent_object->second.Children.insert(Child.ResourceID);
      else parent_object->second.Resources.insert(Child.ResourceID);
   }
}

//********************************************************************************************************************

void object_remove_child(ResourceRecord &Parent, ResourceRecord &Child)
{
   std::lock_guard lock(glmObjects);

   if (auto object_rec = glObjects.find(Parent.ResourceID); object_rec != glObjects.end()) {
      object_rec->second.Resources.erase(Child.ResourceID);
      object_rec->second.Children.erase(Child.ResourceID);
   }
}

//********************************************************************************************************************

constexpr CSTRING action_name(OBJECTPTR Object, ACTIONID ActionID)
{
   if (ActionID > AC::NIL) {
      if (ActionID < AC::END) return ActionTable[int(ActionID)].Name;
      else return "Action";
   }
   else if ((Object) and (not ((extMetaClass *)Object->Class)->Methods.empty())) {
      return ((extMetaClass *)Object->Class)->Methods[-int(ActionID)].Name;
   }
   else return "Method";
}

//********************************************************************************************************************
// Free all private memory resources tracked to an object.  Called from object_free()

static void free_children(OBJECTPTR Object)
{
   kt::Log log;

   std::vector<RESOURCEID> resources;

   {
      std::lock_guard resource_lock(glmResources);
      std::lock_guard object_lock(glmObjects);

      auto object_rec = glObjects.find(Object->UID);
      if (object_rec IS glObjects.end()) return;

      // Free all children associated with this object.

      if (not object_rec->second.Children.empty()) {
         for (const auto id : object_rec->second.Children) {
            auto child_rec = glObjects.find(id);
            if ((child_rec IS glObjects.end()) or (not child_rec->second.Object)) continue;

            auto child = child_rec->second.Object;
            if (not child->collecting()) {
               if (child->defined(NF::LOCAL)) {
                  log.warning("Found unfreed child object #%d (class %s) belonging to %s object #%d.", child->UID, ResolveClassID(child->classID()), Object->className(), Object->UID);
               }
               resources.push_back(child->UID);
            }
         }
      }

      // Free all non-object resources associated with this object

      if (not object_rec->second.Resources.empty()) {
         for (const auto id : object_rec->second.Resources) {
            auto resource = glResources.find(id);
            if (resource IS glResources.end()) continue;
            auto &rec = resource->second;
            if (rec.CollectOnUnlock or rec.Terminating) continue;

            if (glLogLevel >= 3) log.warning("Unfreed %s resource #%d", rec.Manager->Name, rec.ResourceID);
            resources.push_back(id);
         }
      }
   }

   for (const auto &id : resources) FreeResource(id);
}

static void launch_async_thread(OBJECTPTR, AC, int, std::vector<int8_t>, FUNCTION);

//********************************************************************************************************************
// Dispatch the next queued action for an object.  Called from msg_threadaction() on the main thread
// after a callback has been processed (or when no callback was defined).  If the queue is empty,
// the object is removed from glActiveAsyncObjects.

void dispatch_queued_action(OBJECTID ObjectID)
{
   kt::Log log(__FUNCTION__);

   // Do not consume queued actions if the process is shutting down.
   if (glTaskState IS TSTATE::STOPPING) {
      drain_action_queue(ObjectID);
      return;
   }

   QueuedAction next;
   bool queue_empty = false;
   {
      std::lock_guard<std::mutex> lock(glmActionQueue);
      // Cancellation state applies only to the action whose completion is being processed.
      glCancelledAsyncObjects.erase(ObjectID);
      auto it = glActionQueues.find(ObjectID);
      if ((it IS glActionQueues.end()) or it->second.empty()) {
         glActiveAsyncObjects.erase(ObjectID);
         if (it != glActionQueues.end()) glActionQueues.erase(it);
         queue_empty = true;
      }
      else {
         next = std::move(it->second.front());
         it->second.pop_front();
      }
   }

   if (queue_empty) {
      // Clear the async flag now that no more actions are pending.
      ScopedObjectLock obj(ObjectID);
      if (obj.granted()) {
         if (obj->defined(NF::ASYNC_ACTIVE)) {
            obj->clearFlag(NF::ASYNC_ACTIVE);
            async_wait_callback(obj->UID, false);
         }
      }
      return;
   }

   // Queue mutex released before thread launch to avoid holding two locks simultaneously.

   // Validate the object pointer via its UID before dereferencing.  The object may have been freed
   // between queueing and dispatch.

   ScopedObjectLock obj(ObjectID);
   if (obj.granted()) {
      if (obj->terminating() or obj->collecting()) {
         if (next.Callback.defined()) {
            ThreadActionMessage msg = {
               .ActionID = next.ActionID,
               .ObjectID = ObjectID,
               .Error    = ERR::DoesNotExist,
               .Callback = next.Callback,
               .DispatchNext = false
            };
            SendMessage(MSGID::THREAD_ACTION, MSF::NIL, &msg, sizeof(msg));
         }

         drain_action_queue(ObjectID);
         return;
      }

      launch_async_thread(*obj, next.ActionID, next.ArgsSize, std::move(next.Parameters), next.Callback);
   }
   else {
      // The object is no longer accessible (freed or otherwise invalid).  Treat this identically
      // to the terminating case: send an error callback and drain the remaining queue.

      log.traceWarning(obj.error);

      if (next.Callback.defined()) {
         ThreadActionMessage msg = {
            .ActionID = next.ActionID,
            .ObjectID = ObjectID,
            .Error    = obj.error,
            .Callback = next.Callback,
            .DispatchNext = false
         };
         SendMessage(MSGID::THREAD_ACTION, MSF::NIL, &msg, sizeof(msg));
      }

      drain_action_queue(ObjectID, true);
   }
}

//********************************************************************************************************************
// Drain all queued actions for an object, sending error callbacks for each.  Called when the object
// is being freed or is otherwise no longer valid.

static void drain_action_queue(OBJECTID ObjectID, bool Terminating, bool PreserveActive)
{
   kt::Log log(__FUNCTION__);

   std::deque<QueuedAction> drained;
   bool clear_active = not PreserveActive;
   {
      std::lock_guard<std::mutex> lock(glmActionQueue);
      auto it = glActionQueues.find(ObjectID);
      if (it != glActionQueues.end()) {
         drained = std::move(it->second);
         glActionQueues.erase(it);
      }
      if (clear_active) {
         glActiveAsyncObjects.erase(ObjectID);
         glCancelledAsyncObjects.erase(ObjectID);
      }
   }

   // Clear the async flag.  The object may already be freed in the Terminating case, so tolerate lock failure.

   if ((not Terminating) and clear_active) {
      ScopedObjectLock obj(ObjectID);
      if (obj.granted()) {
         if (obj->defined(NF::ASYNC_ACTIVE)) {
            obj->clearFlag(NF::ASYNC_ACTIVE);
            async_wait_callback(obj->UID, false);
         }
      }
   }

   if (not drained.empty()) {
      log.trace("Draining %d queued actions for object #%d", int(drained.size()), ObjectID);
   }

   for (auto &action : drained) {
      if (action.Callback.defined()) {
         ThreadActionMessage msg = {
            .ActionID = action.ActionID,
            .ObjectID = ObjectID,
            .Error    = ERR::DoesNotExist,
            .Callback = action.Callback,
            .DispatchNext = false
         };
         SendMessage(MSGID::THREAD_ACTION, MSF::NIL, &msg, sizeof(msg));
      }
   }
}

//********************************************************************************************************************
// Helper to launch an async action thread for an object.

static void launch_async_thread(OBJECTPTR Object, ACTIONID ActionID, int ArgsSize, std::vector<int8_t> Parameters,
   FUNCTION Callback)
{
   auto object_uid = Object->UID;

   // Lock global async now so that we don't incur the unlikely event of the thread executing
   // and removing itself from the group before we've managed to add it.

   std::lock_guard<std::recursive_mutex> lock(glmAsyncActions);

   auto thread_ptr = std::make_shared<std::jthread>();

   *thread_ptr = std::jthread([Object, ActionID, ArgsSize, Parameters = std::move(Parameters), Callback,
      object_uid, thread_ptr](std::stop_token stop_token) {

      // Cleanup function to remove thread from tracking
      auto cleanup = [thread_ptr, object_uid]() {
         {
            std::lock_guard<std::mutex> lock(glmActionQueue);
            glAsyncObjectThreads.erase(object_uid);
         }
         deregister_thread();
         std::lock_guard<std::recursive_mutex> lock(glmAsyncActions);
         glAsyncThreads.erase(thread_ptr);
      };

      // Check for stop request before proceeding

      auto thread_rec = get_thread_record();

      // Register the mapping from object ID to thread ID so that AsyncCancel() can target this thread.
      {
         std::lock_guard<std::mutex> lock(glmActionQueue);
         glAsyncObjectThreads[object_uid] = int(get_thread_id());
      }

      auto is_stopping = [&stop_token, &thread_rec]() {
         return stop_token.stop_requested()
            or (thread_rec and thread_rec->state.load(std::memory_order_acquire) IS TSTATE::STOPPING);
      };

      auto is_cancelled = [object_uid]() {
         std::lock_guard<std::mutex> lock(glmActionQueue);
         return glCancelledAsyncObjects.contains(object_uid);
      };

      if (is_stopping() or is_cancelled()) {
         ThreadActionMessage msg = {
            .ActionID = ActionID,
            .ObjectID = object_uid,
            .Error    = ERR::Cancelled,
            .Callback = Callback
         };
         SendMessage(MSGID::THREAD_ACTION, MSF::NIL, &msg, sizeof(msg));
         cleanup();
         return;
      }

      ERR error;
      if (error = LockObject(Object, 5000); !error) { // Access the object and process the action.
         // Check for stop request before executing action
         if ((not is_stopping()) and (not is_cancelled())) {
            error = Action(ActionID, Object, ArgsSize ? (APTR)Parameters.data() : nullptr);
         }

         if (Object->terminating()) {
            ReleaseObject(Object);
            // NOTE: The Object will be deleted on release.
         }
         else ReleaseObject(Object);
      }

      // Always send a completion message so that msg_threadaction() can dispatch the next queued action.
      // Preserve the callback on cancellation so script-side cleanup still runs.

      auto completion_error = error;
      if (is_stopping() or is_cancelled()) completion_error = ERR::Cancelled;

      ThreadActionMessage msg = {
         .ActionID = ActionID,
         .ObjectID = object_uid,
         .Error    = completion_error,
         .Callback = Callback
      };
      SendMessage(MSGID::THREAD_ACTION, MSF::NIL, &msg, sizeof(msg));

      cleanup();
   });

   glAsyncThreads.insert(thread_ptr);

   thread_ptr->detach();
}

/*********************************************************************************************************************

-FUNCTION-
Action: This function is responsible for executing action routines.

This function is the key entry point for executing actions and method routines.  An action is a predefined function
call that can be called on any object, while a method is a function call that is specific to a class implementation.
You can find a complete list of available actions and their associated details in the Kotuku Wiki.
The actions and methods supported by any class will be referenced in their auto-generated documentation.

Here are two examples that demonstrate how to make an action call.  The first performs an activation, which
does not require any additional arguments.  The second performs a move operation, which requires three additional
arguments to be passed to the Action() function:

<pre>
1. Action(AC::Activate, Image, nullptr);

2. struct acMove move = { 30, 15, 0 };
   Action(AC::Move, Window, &move);
</pre>

In all cases, action calls in C++ can be simplified by using their corresponding stub functions:

<pre>
1.  acActivate(Image);

2a. acMove(Window, 30, 15, 0);

2b. Window->move(30, 15, 0);
</pre>

If the class of an object does not support the `Action` ID, an error code of `ERR::NoSupport` is returned.  To test
an object to see if its class supports an action, use the ~CheckAction() function.

-INPUT-
int(AC) Action: An action or method ID must be specified.
obj Object:     The target object.
ptr Parameters: Optional parameter structure associated with `Action`.

-ERRORS-
Okay:
NullArgs:
AccessObject:
NoAction:        The `Action` is not supported by the object's supporting class.

-TAGS-
mutates-object, blocking, callback-inlines
-END-

**********************************************************************************************************************/

ERR Action(ACTIONID ActionID, OBJECTPTR Object, APTR Parameters)
{
   if (not Object) return ERR::NullArgs;

   ScopedObjectAccess lock(Object);
   if (not lock.granted()) return ERR::AccessObject;

   extObjectContext new_context(Object, ActionID);
#ifndef NDEBUG
   Object->ActionDepth++;
#endif
   auto cl = Object->ExtClass;

   ERR error;
   if (ActionID >= AC::NIL) {
      if (cl->ActionTable[int(ActionID)].PerformAction) { // Can be a base-class or derived class call
         error = cl->ActionTable[int(ActionID)].PerformAction(Object, Parameters);

         if (error IS ERR::NoAction) {
            if ((cl->Base) and (cl->Base->ActionTable[int(ActionID)].PerformAction)) { // Base is set only if this is a derived class
               error = cl->Base->ActionTable[int(ActionID)].PerformAction(Object, Parameters);
            }
         }
      }
      else if ((cl->Base) and (cl->Base->ActionTable[int(ActionID)].PerformAction)) { // Base is set only if this is a derived class
         error = cl->Base->ActionTable[int(ActionID)].PerformAction(Object, Parameters);
      }
      else error = ERR::NoAction;
   }
   else { // Method call
      // Note that derived classes may return ERR::NoAction if propagation to the base class is desirable.
      auto routine = (ERR (*)(OBJECTPTR, APTR))cl->Methods[-int(ActionID)].Routine;
      if (routine) error = routine(Object, Parameters);
      else error = ERR::NoAction;

      if ((error IS ERR::NoAction) and (cl->Base)) {  // If this is a derived class, check the base class
         auto routine = (ERR (*)(OBJECTPTR, APTR))cl->Base->Methods[-int(ActionID)].Routine;
         if (routine) error = routine(Object, Parameters);
      }
   }

   // If the object has action subscribers, check if any of them are listening to this particular action, and if so, notify them.

   if (int(error) & int(ERR::Notified)) {
      error = ERR(int(error) & ~int(ERR::Notified));
   }
   else if ((ActionID > AC::NIL) and (Object->NotifyFlags.load() & (1LL<<(int(ActionID) & 63)))) {
      std::lock_guard<std::recursive_mutex> lock(glSubLock);

      glSubReadOnly++;

      if (auto it = glSubscriptions.find(Object->UID); it != glSubscriptions.end()) {
         if (it->second.contains(int(ActionID))) {
            for (auto &list : it->second[int(ActionID)]) {
               #ifndef NDEBUG
               // Locked subscribers can sometimes warrant investigation
               if ((int(ActionID) > 0) and list.Subscriber->locked()) {
                  kt::Log(__FUNCTION__).msg("Notifying %s subscriber #%d (lock-status: %d) with action %s",
                     list.Subscriber->className(), list.Subscriber->UID, list.Subscriber->locked(), ActionTable[int(ActionID)].Name);
               }
               #endif
               kt::SwitchContext ctx(list.Subscriber);
               list.Callback(Object, ActionID, (error IS ERR::NoAction) ? ERR::Okay : error, Parameters, list.Meta);
            }
         }
      }

      glSubReadOnly--;
   }

#ifndef NDEBUG
   Object->ActionDepth--;
#endif
   return error;
}

/*********************************************************************************************************************

-FUNCTION-
ActionList: Returns a pointer to the global action table.

This function returns an array of all actions supported by the Core, including name, arguments and structure
size.  The ID of each action is indicated by its index within the array.

The `Name` field specifies the name of the action.  The `Args` field refers to the action's argument definition structure,
which lists the argument names and their relevant types.  This is matched by the `Size` field, which indicates the
byte-size of the action's related argument structure.  If the action does not support arguments, the `Args` and `Size`
fields will be set to `NULL`.  The following illustrates two argument definition examples:

<pre>
struct FunctionField argsCopyData[] = {
   { "Destination", FD_INT  },
   { nullptr, 0 }
};

struct FunctionField argsResize[] = {
   { "Width",  FD_DOUBLE },
   { "Height", FD_DOUBLE },
   { "Depth",  FD_DOUBLE },
   { nullptr, 0 }
};
</pre>

The argument types that can be used by actions are limited to those listed in the following table:

<types lookup="FD">
<type name="INT">A 32-bit integer value ranging from -2,147,483,647 to 2,147,483,648.</>
<type name="INT64">A 64-bit integer value.</>
<type name="PTR">A standard address space pointer.</>
<type name="STRING">A pointer to a null-terminated string.</>
<type name="DOUBLE">A 64-bit floating point value.</>
<type name="OBJECT">This flag is sometimes set in conjunction with the `FD_INT` type.  It indicates that the argument refers to an object ID.</>
<type name="PTRSIZE">This argument type can only be used if it follows an `FD_PTR` type, and if the argument itself is intended to reflect the size of the buffer referred to by the previous `FD_PTR` argument.</>
<type name="RESULT">This special flag is set in conjunction with the other data-based argument types. Example: If the developer is required to supply a pointer to an `int` field in which the function will store a result, the correct argument definition will be `FD_RESULT|FD_INT|FD_PTR`. To make the definition of these argument types easier, `FD_PTRRESULT` and `FD_INTRESULT` macros are also available for use.</>
</>

-INPUT-
&array(struct(ActionTable)) Actions: A pointer to the Core's action table `struct ActionTable *` is returned. Please note that the first entry in the `ActionTable` list has all fields driven to `NULL`, because valid action ID's start from one, not zero.  The final action in the list is also terminated with `NULL` fields in order to indicate an end to the list.  Knowing this is helpful when scanning the list or calculating the total number of actions supported by the Core.
&arraysize Size: Total number of elements in the returned list.

-TAGS-
static-result, non-null-result, pure-query

*********************************************************************************************************************/

void ActionList(struct ActionTable **List, int *Size)
{
   if (List) *List = (struct ActionTable *)ActionTable;
   if (Size) *Size = int(AC::END);
}

/*********************************************************************************************************************

-FUNCTION-
AsyncAction: Submit an action for asynchronous execution against an object.

This function submits an action or method for asynchronous execution against `Object`.  The runtime allocates a worker
thread to execute the action; the caller does not manage threads directly.  Please refer to the ~Action() function for
general information on action execution.

To receive feedback of the action's completion, use the `Callback` parameter and supply a function.  The
prototype for the callback routine is `callback(ACTIONID ActionID, OBJECTPTR Object, ERR Error, APTR Meta)`

Actions targeting the same object are serialised through a per-object FIFO queue.  If an async action is already
in-flight for the given object, subsequent calls to AsyncAction() will queue the request rather than spawning a
competing thread.  The next queued action is dispatched after the current action's callback has been processed on the
main thread (or immediately after the action completes if no callback was provided).  Actions targeting different
objects execute in parallel as independent workers.  Any actions submitted during callback execution — including
actions targeting the same object — are appended to the tail of the queue and do not preempt previously queued work.

Execution proceeds in two phases per action.  During the 'worker phase', the worker thread holds an exclusive lock
on the object and executes the action.  On completion, ownership transfers directly to the main thread for the
'callback phase'.  At no point between worker completion and callback return is the object available to another worker.
Only after the callback returns does the next queued action begin.

Callbacks are processed when the main thread makes a call to ~ProcessMessages(), so as to maintain an orderly
execution process within the application.  It is crucial that the target object is not destroyed while actions are
executing or queued.  Use the `Callback` routine to receive notification of each action's completion.  If an object
is freed while actions are still queued, the remaining callbacks will be invoked with an `ERR::DoesNotExist` error
and a `NULL` object pointer.

The 'Error' parameter in the callback reflects the error code returned by the action after it has been called.  Note
that if AsyncAction() fails, the callback will never be executed because the attempt will have been aborted.

This function is at its most effective when used to perform lengthy processes such as the loading and parsing of data.

NOTE: Tiri scripts must use the `async.action|method()` interfaces for asynchronous activity instead of this function.

-INPUT-
int(AC) Action: An action or method ID must be specified here.
obj Object: The target object to execute the action against.
ptr Args: If the action or method is documented as taking parameters, provide the correct parameter structure here.
ptr(func) Callback: Optional function called on the main thread after the action completes.

-ERRORS-
Okay
NullArgs
InvalidData
MarkedForDeletion
MissingClass

-TAGS-
copies-input, callback-held, blocking
-END-

*********************************************************************************************************************/

ERR AsyncAction(ACTIONID ActionID, OBJECTPTR Object, APTR Parameters, FUNCTION *Callback)
{
   kt::Log log(__FUNCTION__);

   if ((ActionID IS AC::NIL) or (not Object)) return ERR::NullArgs;

   SharedObjectAccess objref(Object);
   if (not objref.granted()) return ERR::MarkedForDeletion;
   auto object_id = Object->UID;

   log.traceBranch("Action: %d, Object: %d, Parameters: %p, Callback: %p", int(ActionID), object_id, Parameters, Callback);

   auto error = ERR::Okay;

   // Prepare the parameter buffer for passing to the thread routine.

   int argssize = 0;
   std::vector<int8_t> param_buffer;
   const FunctionField *args = nullptr;

   if (Parameters) {
      if (int(ActionID) > 0) {
         args = ActionTable[int(ActionID)].Args;
         if ((argssize = ActionTable[int(ActionID)].Size) > 0) {
            error = copy_args(args, argssize, (int8_t *)Parameters, param_buffer);
         }
      }
      else if (auto cl = Object->ExtClass) {
         args = cl->Methods[-int(ActionID)].Args;
         if ((argssize = cl->Methods[-int(ActionID)].Size) > 0) {
            error = copy_args(args, argssize, (int8_t *)Parameters, param_buffer);
         }
      }
      else error = log.warning(ERR::MissingClass);
   }

   if (!error) {
      FUNCTION cb;
      if (Callback) cb = *Callback;

      // Check if an async action is already active for this object.  If so, queue the request
      // instead of spawning a competing thread.

      {
         std::lock_guard<std::mutex> lock(glmActionQueue);
         if (glActiveAsyncObjects.contains(object_id)) {
            glActionQueues[object_id].push_back(QueuedAction {
               .ObjectID   = object_id,
               .ActionID   = ActionID,
               .ArgsSize   = argssize,
               .Parameters = std::move(param_buffer),
               .Callback   = cb
            });

            log.trace("Queued action %d for object #%d (queue depth: %d)",
               int(ActionID), object_id, int(glActionQueues[object_id].size()));

            return ERR::Okay;
         }
         else {
            glActiveAsyncObjects.insert(object_id);
            Object->setFlag(NF::ASYNC_ACTIVE);
            async_wait_callback(object_id, true);
         }
      }

      launch_async_thread(Object, ActionID, argssize, std::move(param_buffer), cb);
   }

   return error;
}

/*********************************************************************************************************************

-FUNCTION-
AsyncCancel: Drain the pending queue for listed objects without executing the remaining actions.

Call AsyncCancel() to cancel all pending asynchronous actions for the listed objects.  Each object's currently active
async thread (if any) is interrupted via ~WakeThread() with Stop set to true, and the remaining action queue is
drained without execution.

Callbacks for queued actions will receive `ERR::DoesNotExist` to indicate cancellation.

-INPUT-
vector(oid) Objects: A list of object IDs to cancel.

-ERRORS-
Okay
NullArgs

-TAGS-
blocking

*********************************************************************************************************************/

ERR AsyncCancel(kt::vector<OBJECTID> &Objects)
{
   kt::Log log(__FUNCTION__);

   for (auto object_id : Objects) {
      log.traceBranch("Cancelling async actions for object #%d", object_id);

      // Wake the thread with Stop=true to interrupt any active async operation.

      int thread_id;
      {
         std::lock_guard<std::mutex> lock(glmActionQueue);
         if (glActiveAsyncObjects.contains(object_id)) glCancelledAsyncObjects.insert(object_id);
         auto it = glAsyncObjectThreads.find(object_id);
         if (it != glAsyncObjectThreads.end()) thread_id = it->second;
         else thread_id = 0;
      }

      if (thread_id) WakeThread(thread_id, true);

      drain_action_queue(object_id, false, true);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
AsyncPending: Return the number of queued and in-flight async actions for an object.

Call AsyncPending() to query the total number of asynchronous actions that are either currently executing or waiting
in the queue for a given object.  The returned count includes the in-flight action (if any) plus all queued actions
that have not yet been dispatched.

A return value of zero indicates that no async activity is associated with the object.

-INPUT-
oid Object: The object to query.

-RESULT-
int: The number of pending async actions (in-flight + queued), or zero if none.

-TAGS-
blocking, pure-query
-END-

*********************************************************************************************************************/

int AsyncPending(OBJECTID ObjectID)
{
   std::lock_guard<std::mutex> lock(glmActionQueue);
   int count = 0;
   // Match AsyncWait() state: worker-thread bookkeeping can outlive completion dispatch briefly.
   if (glActiveAsyncObjects.contains(ObjectID)) count++;
   if (auto it = glActionQueues.find(ObjectID); it != glActionQueues.end()) {
      count += int(it->second.size());
   }
   return count;
}

/*********************************************************************************************************************

-FUNCTION-
AsyncWait: Block until all queued async actions for the listed objects have completed.

Call AsyncWait() to suspend the current thread until every asynchronous action that is executing or queued against
the listed objects has finished.  Messages continue to be processed while waiting, so async completion callbacks
fire normally.

Only one AsyncWait() call may be active at any time.  If a second call is made while the first is still blocking,
`ERR::InUse` is returned.

-INPUT-
vector(oid) Objects: A list of object IDs to wait on.
int TimeOut: Maximum time to wait in milliseconds, or `-1` for an indefinite wait.

-ERRORS-
Okay: All async actions completed.
TimeOut: The timeout expired before all actions completed.
NullArgs
InUse: Another AsyncWait() call is already active.
OutsideMainThread:
Recursion:
SystemLocked:
Terminate:

-TAGS-
main-thread-only, blocking, callback-inlines
-END-

*********************************************************************************************************************/

ERR AsyncWait(kt::vector<OBJECTID> &Objects, int TimeOut)
{
   kt::Log log(__FUNCTION__);

   // Message processing is only possible from the main thread (for system design and synchronisation reasons)
   if (!tlMainThread) return log.warning(ERR::OutsideMainThread);

   // Prevent recursive/concurrent use — only one wait can be active at a time.
   bool expected = false;
   if (not glAsyncWaiting.compare_exchange_strong(expected, true)) return log.warning(ERR::InUse);

   {
      std::lock_guard<std::mutex> lock(glmAsyncWait);
      glAsyncWaitCounter.store(0, std::memory_order_relaxed);
      glAsyncWaitTargets.clear();
   }

   // Collect target object UIDs and count those already running async actions.
   // Check glActiveAsyncObjects under the queue mutex rather than locking each object individually.

   {
      std::lock_guard<std::mutex> ql(glmActionQueue);
      std::lock_guard<std::mutex> wl(glmAsyncWait);
      for (auto id : Objects) {
         if (glAsyncWaitTargets.insert(id).second and glActiveAsyncObjects.contains(id)) {
            glAsyncWaitCounter.fetch_add(1, std::memory_order_relaxed);
         }
      }
      if (glAsyncWaitTargets.empty()) {
         glAsyncWaiting.store(false, std::memory_order_relaxed);
         return ERR::Okay;
      }
   }

   int pending = glAsyncWaitCounter.load(std::memory_order_relaxed);
   log.branch("Objects: %d, Pending: %d, Timeout: %d ms", int(glAsyncWaitTargets.size()), pending, TimeOut);

   ERR error;
   if (pending) error = ProcessMessages(PMF::NIL, TimeOut);
   else error = ERR::Okay;

   glAsyncWaiting.store(false, std::memory_order_relaxed);

   {
      std::lock_guard<std::mutex> lock(glmAsyncWait);
      glAsyncWaitTargets.clear();
      glAsyncWaitCounter.store(0, std::memory_order_relaxed);
   }

   return error;
}

//********************************************************************************************************************
// Called whenever a MSGID::THREAD_ACTION message is caught by ProcessMessages().  Messages are sent by the
// async action thread on completion.  After processing the callback, the next queued action for the same
// object is dispatched.

ERR msg_threadaction(APTR Custom, int MsgID, int MsgType, APTR Message, int MsgSize)
{
   auto msg = (ThreadActionMessage *)Message;
   if (not msg) return ERR::Okay;

   if (msg->Callback.isC()) {
      auto routine = (void (*)(ACTIONID, OBJECTPTR, ERR, APTR))msg->Callback.Routine;
      ScopedObjectLock obj(msg->ObjectID);
      if (obj.granted()) {
         routine(msg->ActionID, *obj, msg->Error, msg->Callback.Meta);
      }
      else routine(msg->ActionID, nullptr, ERR::DoesNotExist, msg->Callback.Meta);
   }
   else if (msg->Callback.isScript()) {
      auto script = msg->Callback.Context;
      if (!LockObject(script, 5000)) {
         sc::Call(msg->Callback, std::to_array<ScriptArg>({
            { "ActionID", int(msg->ActionID) },
            { "Object",   msg->ObjectID, FD_OBJECTID },
            { "Error",    int(msg->Error) },
            { "Meta",     msg->Callback.MetaValue }
         }));

         // Dereference the callback procedure to release the script registry reference.
         sc::DerefProcedure deref = { &msg->Callback };
         Action(sc::DerefProcedure::id, script, &deref);

         ReleaseObject(script);
      }
   }

   // Dispatch the next queued action for this object (if any).
   if (msg->DispatchNext) dispatch_queued_action(msg->ObjectID);

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
CheckAction: Checks objects to see whether or not they support certain actions.

This function returns `ERR::True` if an object's class supports a given action ID.  For example:

<pre>
if (CheckAction(pic, AC::Query) IS ERR::True) {
   // The Query action is supported.
}
</pre>

-INPUT-
obj Object: The target object.
int(AC) Action: A registered action or method ID.

-ERRORS-
True: The object supports the specified action.
False: The action is not supported.
NullArgs:
LostClass:
OutOfRange:

-TAGS-
pure-query

*********************************************************************************************************************/

ERR CheckAction(OBJECTPTR Object, ACTIONID ActionID)
{
   kt::Log log(__FUNCTION__);

   if ((ActionID <= AC::NIL) or (ActionID >= AC::END)) return log.warning(ERR::OutOfRange);

   if (Object) {
      if (not Object->Class) return ERR::False;
      else if (Object->classID() IS CLASSID::METACLASS) {
         if (((extMetaClass *)Object)->ActionTable[int(ActionID)].PerformAction) return ERR::True;
         else return ERR::False;
      }
      else if (auto cl = Object->ExtClass) {
         if (cl->ActionTable[int(ActionID)].PerformAction) return ERR::True;
         else if (cl->Base) {
            if (cl->Base->ActionTable[int(ActionID)].PerformAction) return ERR::True;
         }
         return ERR::False;
      }
      else return log.warning(ERR::LostClass);
   }
   else return log.warning(ERR::NullArgs);
}

/*********************************************************************************************************************

-FUNCTION-
ClassDatabase: Returns an array of all classes known to the system.

Call ClassDatabase() to obtain an array of all classes known to the system.

-INPUT-
^&vector(struct(*ClassRecord)) Classes: A pointer to an array of class records is returned here.

-ERRORS-
Okay
NullArgs
SystemLocked

-TAGS-
caller-owns-result, creates-resource, blocking

*********************************************************************************************************************/

ERR ClassDatabase(kt::vector<ClassRecord *> *Classes)
{
   if (not Classes) return ERR::NullArgs;

   Classes->clear();
   if (auto lock = std::unique_lock{glmClassDB, 3s}) {
      for (auto &entry : glClassDB) Classes->push_back(&entry.second);
      return ERR::Okay;
   }
   else return kt::Log().warning(ERR::SystemLocked);
}

/*********************************************************************************************************************

-FUNCTION-
CurrentContext: Returns a pointer to the object that has the current context.

This function returns a pointer to the object that has the current context.  Context is primarily used to manage
resource allocations.  Manipulating the context is sometimes necessary to ensure that a resource is tracked to
the correct object.

To get the context of the caller (the client), use ~ParentContext().

-RESULT-
obj: Returns an object pointer (of which the process has exclusive access to).  Cannot return `NULL` except in the initial start-up and late shut-down sequence of the Core.

-TAGS-
api-owns-result, nullable-result, pure-query

*********************************************************************************************************************/

OBJECTPTR CurrentContext(void)
{
   return tlContext.back().obj;
}

/*********************************************************************************************************************

-FUNCTION-
ParentContext: Returns the context of the client.

This function is used to return the context of the caller (the client), as opposed to ~CurrentContext(), which
returns the operating context.  This feature is commonly used by methods that need to acquire a reference to the
client for resource management reasons.

Note that this function can return `NULL` if called when running at process-level, although this would never be
the case when called from an action or method.

-RESULT-
obj: An object reference is returned, or `NULL` if there is no parent context.

-TAGS-
api-owns-result, nullable-result, pure-query

*********************************************************************************************************************/

OBJECTPTR ParentContext(void)
{
   for (auto it=tlContext.rbegin()+1; it != tlContext.rend(); ++it) {
      if (it->obj != tlContext.back().obj) return it->obj;
   }

   return nullptr;
}

/*********************************************************************************************************************

-FUNCTION-
FindClass: Returns the internal MetaClass for a given class ID.

This function will find a specific class by ID and return its @MetaClass.  If the class is not already loaded, the
internal dictionary is checked to discover a module binary registered with that ID.  If this succeeds, the module is
loaded into memory and the correct MetaClass will be returned.

In any event of failure, `NULL` is returned.

If the ID of a named class is not known, call ~ResolveClassName() first and pass the resulting ID to this function.

NOTE: To retrieve a list of all derived classes associated with a base-class, read the SubClasses field of the @MetaClass.

-INPUT-
cid ClassID: A class ID such as one retrieved from ~ResolveClassName().

-RESULT-
obj(MetaClass): Returns a pointer to the @MetaClass structure that has been found as a result of the search, or `NULL` if no matching class was found.

-TAGS-
api-owns-result, nullable-result, blocking

*********************************************************************************************************************/

objMetaClass * FindClass(CLASSID ClassID)
{
   auto it = glClassMap.find(ClassID);
   if (it != glClassMap.end()) return it->second;

   if (glProgramStage IS STAGE_SHUTDOWN) return nullptr; // No new module loading during shutdown

   // Class is not loaded.  Try and find the class in the dictionary.  If we find one, we can
   // initialise the module and then find the new Class.
   //
   // Note: Derived classes are not automatically loaded into memory if they are unavailable at the time.  Doing so
   // would result in lost CPU and memory resources due to loading code that may not be needed.

   kt::Log log(__FUNCTION__);
   if (glClassDB.contains(ClassID)) {
      if (auto &path = glClassDB[ClassID].Path; !path.empty()) {
         // Load the module from the associated location and then find the class that it contains.  If the module fails,
         // we keep on looking for other installed modules that may handle the class.

         log.branch("Attempting to load module \"%s\" for class $%.8x.", path.c_str(), uint32_t(ClassID));

         objModule::create mod = { fl::Name(path) };
         if (mod.ok()) {
            it = glClassMap.find(ClassID);
            if (it != glClassMap.end()) return it->second;
            else log.warning("Module \"%s\" did not configure class \"%s\"", path.c_str(), glClassDB[ClassID].Name.c_str());
         }
         else log.warning("Failed to load module \"%s\"", path.c_str());
      }
      else log.warning("No module path defined for class \"%s\"", glClassDB[ClassID].Name.c_str());
   }
   else log.warning("Could not find class $%.8x in memory or dictionary (%d registered).", uint32_t(ClassID), int(std::ssize(glClassDB)));

   return nullptr;
}

/*********************************************************************************************************************

-FUNCTION-
FindObject: Searches for objects by name.

The FindObject() function searches for all objects that match a given name and can filter by class.

The following example illustrates typical usage, and finds the most recent object created with a given name:

<pre>
OBJECTID id;
FindObject("SystemPointer", CLASSID::POINTER, &id);
</pre>

If FindObject() cannot find any matching objects then it will return an error code.

-INPUT-
strview Name: The name of an object to search for.
cid ClassID:   Optional.  Set to a class ID to filter the results down to a specific class type.
&oid ObjectID: An object id variable for storing the result.

-ERRORS-
Okay: At least one matching object was found and stored in the `ObjectID`.
NullArgs:
EmptyString:
Search: No objects matching the given name could be found.

-TAGS-
blocking, case-insensitive
-END-

*********************************************************************************************************************/

ERR FindObject(const std::string_view &InitialName, CLASSID ClassID, OBJECTID *Result)
{
   kt::Log log(__FUNCTION__);

   if (not Result) return ERR::NullArgs;
   if (InitialName.empty()) return log.warning(ERR::EmptyString);

   if (auto lock = std::shared_lock{glmObjectLookup, 4s}) {
      auto list = glObjectLookup.find(kt::strihash(InitialName));
      if (list != glObjectLookup.end()) {
         if (ClassID IS CLASSID::NIL) {
            *Result = list->second.back()->UID;
            return ERR::Okay;
         }

         for (auto it=list->second.rbegin(); it != list->second.rend(); it++) {
            auto obj = *it;
            if ((obj->classID() IS ClassID) or (obj->Class->BaseClassID IS ClassID)) {
               *Result = obj->UID;
               return ERR::Okay;
            }
         }
      }
   }

   return ERR::Search;
}

/*********************************************************************************************************************

-FUNCTION-
GetActionMsg: Returns a message structure if called from an action that was executed by the message system.

This function is for use by action and method support routines only.  It will return a !Message structure if the
action currently under execution has been called directly from the ~ProcessMessages() function.  In all other
cases a `NULL` pointer is returned.

Only works when called from the main thread, which acts as the message dispatcher.

-INPUT-
int(AC) Action: Action identifier represented by the caller.

-RESULT-
resource(Message): A !Message structure is returned if the function is called in valid circumstances, otherwise `NULL`.

-TAGS-
volatile-result, nullable-result

*********************************************************************************************************************/

Message * GetActionMsg(ACTIONID ActionID)
{
   // Ref: msg_action()
   if ((tlMainThread) and (glCurrentActionMsg)) {
      if (OBJECTPTR obj = current_action()) {
         if ((glCurrentActionMsg->ObjectID IS obj->UID) and (glCurrentActionMsg->ActionID IS ActionID)) {
            glCurrentActionMsg = nullptr; // Calling GetActionMsg() is a one-shot operation
            return (Message *)tlCurrentMsg;
         }
      }
   }
   return nullptr;
}

/*********************************************************************************************************************

-FUNCTION-
GetClassID: Returns the class ID of an ID-referenced object.

Call this function with any valid object ID to learn the identifier for its base class.  This is the quickest way to
retrieve the class of an object without having to gain exclusive access to the object first.

Note that if the object's pointer is already known, the quickest way to learn of its class is to call the `classID()`
C++ method.

-INPUT-
oid Object: The object to be examined.

-RESULT-
cid: Returns the base class ID of the object or zero if failure.

-TAGS-
blocking, pure-query

*********************************************************************************************************************/

CLASSID GetClassID(OBJECTID ObjectID)
{
   if (auto object = GetObjectPtr(ObjectID)) return object->Class->BaseClassID;
   else return CLASSID::NIL;
}

/*********************************************************************************************************************

-FUNCTION-
GetObjectPtr: Returns a direct pointer for any object ID.

This function translates an object ID to its respective address pointer.

-INPUT-
oid Object: The ID of the object to lookup.

-RESULT-
obj: The address of the object is returned, or `NULL` if the ID does not relate to an object.

-TAGS-
api-owns-result, nullable-result, blocking

*********************************************************************************************************************/

OBJECTPTR GetObjectPtr(OBJECTID ObjectID)
{
   {
      std::unique_lock lock(glmObjects);
      if (auto object_rec = glObjects.find(ObjectID); object_rec != glObjects.end()) {
         if ((object_rec->second.Object) and (object_rec->second.Object->UID IS ObjectID)) return object_rec->second.Object;
      }
   }

   return nullptr;
}

/*********************************************************************************************************************

-FUNCTION-
GetOwnerID: Returns the unique ID of an object's owner.

This function returns an identifier for the owner of any valid object.  This is the fastest way to retrieve the
owner of an object if only the ID is known.

If the object address is already known, use the `ownerID()` C++ class method instead of this function.

-INPUT-
oid Object: The ID of an object to query.

-RESULT-
oid: Returns the ID of the object's owner.  If the object does not have a owner (i.e. if it is untracked) or if the provided ID is invalid, this function will return 0.

-TAGS-
blocking, pure-query

*********************************************************************************************************************/

OBJECTID GetOwnerID(OBJECTID ObjectID)
{
   {
      std::unique_lock lock(glmObjects);
      if (auto object_rec = glObjects.find(ObjectID); object_rec != glObjects.end()) {
         if (object_rec->second.Object) return object_rec->second.Object->ownerID();
      }
   }
   return 0;
}

/*********************************************************************************************************************

-FUNCTION-
InitObject: Initialises an object so that it is ready for use.

This function initialises objects so that they can be used for their intended purpose. Initialisation is compulsory,
and a client may not call any actions or methods on an object until it has been initialised.  Exceptions to
this rule only apply to the `GetKey()` and `SetKey()` actions.

If the initialisation of an object fails due to a support problem (for example, if a PNG @Image object attempts to
load a JPEG file), the initialiser will search for a derived class that can handle the data.  If a derived class that can
support the object's configuration is available, the object's interface will be shared between both the base-class
and the derived class.

If an object does not support the data or its configuration, an error code of `ERR::NoSupport` will be returned.
Other appropriate error codes can be returned if initialisation fails.

-INPUT-
obj Object: The object to initialise.

-ERRORS-
Okay: The object was initialised.
LostClass
DoubleInit
ObjectCorrupt

-TAGS-
mutates-object, blocking, callback-inlines

*********************************************************************************************************************/

ERR InitObject(OBJECTPTR Object)
{
   kt::Log log("Init");

   ScopedObjectAccess objlock(Object);

   auto cl = Object->ExtClass;

   if (Object->initialised()) {  // Initialising twice does not cause an error, but send a warning and return
      log.warning(ERR::DoubleInit);
      return ERR::Okay;
   }

   if (Object->Name[0]) log.branch("%s #%d, Name: %s, Owner: %d", cl->ClassName.c_str(), Object->UID, Object->Name, Object->ownerID());
   else log.branch("%s #%d, Owner: %d", cl->ClassName.c_str(), Object->UID, Object->ownerID());

   extObjectContext new_context(Object, AC::Init);

   bool use_derived = false;
   ERR error = ERR::Okay;
   if (Object->isDerived()) {
      // For derived classes, the base-class gets called first.  It should verify that
      // the object is derived classed so as to prevent it from doing 'too much' initialisation.

      if (cl->Base->ActionTable[int(AC::Init)].PerformAction) {
         error = cl->Base->ActionTable[int(AC::Init)].PerformAction(Object, nullptr);
      }

      if (!error) {
         if (cl->ActionTable[int(AC::Init)].PerformAction) {
            error = cl->ActionTable[int(AC::Init)].PerformAction(Object, nullptr);
         }

         if (!error) Object->setFlag(NF::INITIALISED);
      }

      return error;
   }
   else {
      // Meaning of special error codes:
      //
      // ERR::NoSupport: The source data is not recognised.  Search for a derived class that might have better luck.  Note
      //   that in the first case we can only support classes that are already in memory.  The second part of this
      //   routine supports checking of derived classes that aren't loaded yet.
      //
      // ERR::UseDerived: Can be returned by the base-class.  Similar to ERR::NoSupport, but avoids scanning of
      // derived classes that aren't loaded in memory.

      auto &subclasses = Object->ExtClass->SubClasses;
      auto subindex = subclasses.begin();
      bool stop = false;

      while (not stop) {
         if (Object->ExtClass->ActionTable[int(AC::Init)].PerformAction) {
            error = Object->ExtClass->ActionTable[int(AC::Init)].PerformAction(Object, nullptr);
         }
         else error = ERR::Okay; // If no initialiser defined, auto-OK

         if (!error) {
            Object->setFlag(NF::INITIALISED);

            if (Object->isDerived()) {
               // Increase the open count of the derived class (see NewObject() for details on object
               // reference counting).

               log.msg("Object class switched to derived class \"%s\".", Object->className());

               Object->ExtClass->OpenCount++;
               Object->setFlag(NF::RECLASSED); // This flag indicates that the object originally belonged to the base-class
            }

            return ERR::Okay;
         }
         else if (error IS ERR::UseDerived) {
            log.trace("Requested to use registered derived class.");
            use_derived = true;
         }
         else if (error != ERR::NoSupport) break;

         // Attempt to initialise with the next known derived class.

         if (subindex != subclasses.end()) {
            Object->ExtClass = *subindex;
            subindex++;
         }
         else stop = true;
      }
   }

   Object->Class = cl;  // Put back the original to retain integrity

   // If the base class and its loaded derived classes failed, check the object for a Path field and check the data
   // against derived classes that are not currently in memory.
   //
   // This is the only way we can support the automatic loading of derived classes without causing undue load on CPU and
   // memory resources (loading each derived class into memory just to check whether or not the data is supported is overkill).

   std::string_view path;
   if (use_derived) { // If ERR::UseDerived was set and the derived class was not registered, do not call IdentifyFile()
      log.warning("ERR::UseDerived was used but no suitable derived class was registered.");
   }
   else if ((error IS ERR::NoSupport) and (!Object->get(FID_Path, path)) and (not path.empty())) {
      CLASSID class_id, derived_id;
      if (!IdentifyFile(path, cl->BaseClassID, &class_id, &derived_id)) {
         if ((class_id IS Object->classID()) and (derived_id != CLASSID::NIL)) {
            log.msg("Searching for derived class $%.8x", uint32_t(derived_id));
            if ((Object->ExtClass = (extMetaClass *)FindClass(derived_id))) {
               if (Object->ExtClass->ActionTable[int(AC::Init)].PerformAction) {
                  if ((error = Object->ExtClass->ActionTable[int(AC::Init)].PerformAction(Object, nullptr)) IS ERR::Okay) {
                     log.msg("Object class switched to derived class \"%s\".", Object->className());
                     Object->setFlag(NF::INITIALISED);
                     Object->ExtClass->OpenCount++;
                     return ERR::Okay;
                  }
               }
               else return ERR::Okay;
            }
            else log.warning("Failed to load module for class #%d.", uint32_t(derived_id));
         }
      }
      else log.warning("File '%.*s' does not belong to class '%s', got $%.8x.",
         int(path.size()), path.data(), Object->className(), uint32_t(class_id));

      Object->Class = cl;  // Put back the original to retain object integrity
   }

   return error;
}

/*********************************************************************************************************************

-FUNCTION-
ListChildren: Returns a list of all children belonging to an object.

The ListChildren() function returns a list of all children belonging to an object.  The client must provide an empty
vector of !ChildEntry structures to host the results, which include unique object ID's and their class identifiers.

Note that any child objects marked with the `LOCAL` flag will be excluded because they are private members of the
targeted object.

-INPUT-
oid Object: An object to query.
ptr(vector(resource(ChildEntry))) List: Must refer to an array of !ChildEntry structures.

-ERRORS-
Okay: Zero or more children were found and listed.
NullArgs
LockFailed

-TAGS-
mutates-input, blocking, pure-query

*********************************************************************************************************************/

ERR ListChildren(OBJECTID ObjectID, kt::vector<ChildEntry> *List)
{
   kt::Log log(__FUNCTION__);

   if ((not ObjectID) or (not List)) return log.warning(ERR::NullArgs);

   log.trace("#%d, List: %p", ObjectID, List);

   std::unique_lock lock(glmObjects);
   if (auto object_rec = glObjects.find(ObjectID); object_rec != glObjects.end()) {
      for (const auto id : object_rec->second.Children) {
         auto child_rec = glObjects.find(id);
         if (child_rec IS glObjects.end()) continue;

         auto child = child_rec->second.Object;
         if (not child) continue;
         if (not child->defined(NF::LOCAL)) {
            List->emplace_back(child->UID, child->classID());
         }
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
NewObject: Creates new objects.

The NewObject() function is used to create new objects and register them for use within the Core.  After creating
a new object, the client can proceed to set the object's field values and initialise it with #Init() so that it
can be used as intended.

The new object will be modeled according to the class blueprint indicated by `ClassID`.  Pre-defined class ID's are
defined in their documentation and the `kotuku/system/register.h` include file.  ID's for unregistered classes can
be computed using the ~ResolveClassName() function.

A pointer to the new object will be returned in the `Object` parameter.  By default, object allocations are context
sensitive and will be collected when their owner is terminated.  It is possible to track an object to a different
owner by using the ~SetOwner() function.

To destroy an object, call ~FreeResource().

-INPUT-
cid ClassID: A class ID from `system/register.h` or generated by ~ResolveClassName().
flags(NF) Flags:  Optional flags.
&obj Object: Pointer to an address variable that will store a reference to the new object.

-ERRORS-
Okay
NullArgs
MissingClass: The `ClassID` is invalid or refers to a class that is not installed.
AllocMemory

-TAGS-
caller-owns-result, creates-resource, blocking, callback-inlines
-END-

*********************************************************************************************************************/

ERR NewObject(CLASSID ClassID, NF Flags, OBJECTPTR *Object)
{
   kt::Log log(__FUNCTION__);

   auto class_id = ClassID;
   if ((class_id IS CLASSID::NIL) or (not Object)) return log.warning(ERR::NullArgs);

   auto mc = (extMetaClass *)FindClass(class_id);
   if (not mc) return log.warning(ERR::MissingClass);

   if (Object) *Object = nullptr;

   Flags &= (NF::UNTRACKED|NF::LOCAL|NF::UNIQUE|NF::NAME|NF::SUPPRESS_LOG); // Very important to eliminate any internal flags.

   // If the object is local then turn off use of the UNTRACKED flag (otherwise the child will
   // end up being tracked to its task rather than its parent object).

   if ((Flags & NF::LOCAL) != NF::NIL) Flags &= ~NF::UNTRACKED;

   // Force certain flags on the class' behalf

   if ((mc->Flags & CLF::NO_OWNERSHIP) != CLF::NIL) Flags |= NF::UNTRACKED;

   if ((Flags & NF::SUPPRESS_LOG) IS NF::NIL) {
      log.branch("%s #%d, Flags: $%x", mc->ClassName.c_str(),
         glResourceID.load(std::memory_order_relaxed), int(Flags));
   }

   OBJECTPTR head = nullptr;

   // Object memory is allocated directly on the heap and tracked through glResources/glObjects rather than
   // glMemory.  Only 8-byte alignment is required for the object header.

   if (APTR start_mem = aligned_block_alloc(mc->Size, OBJECT_ALIGNMENT)) {
      head = (OBJECTPTR)start_mem;

      OBJECTID object_id = glResourceID++;

      new (head) class Object; // Class constructors aren't expected to initialise the Object header, we do it for them
      kt::clearmem(head + 1, mc->Size - sizeof(class Object));

      // NB: Clients are not permitted to allocate Kotuku resources in NewPlacement due to
      // the object context not yet being established.  Such allocations must be deferred to the NewObject hook.

      ERR error = ERR::Okay;
      if (mc->ActionTable[int(AC::NewPlacement)].PerformAction) {
         // Derived classes have priority over base for NewPlacement.  Base classes that need NewPlacement should either specify
         // initialisers in the class definition (where the derived class can also see them) or defer to the NewObject hook.
         error = mc->ActionTable[int(AC::NewPlacement)].PerformAction(head, nullptr);
      }
      else if ((mc->Base) and (mc->Base->ActionTable[int(AC::NewPlacement)].PerformAction)) {
         error = mc->Base->ActionTable[int(AC::NewPlacement)].PerformAction(head, nullptr);
      }

      if (error != ERR::Okay) {
         free_object_block(head);
         return error;
      }

      head->UID     = object_id;
      head->Class   = (extMetaClass *)mc;
      head->setFlag(Flags);

      {
         std::lock_guard resource_lock(glmResources);
         std::lock_guard object_lock(glmObjects);
         glResources.insert_or_assign(object_id, ResourceRecord(object_id, head, 0, &glResourceObject));
         glObjects.insert_or_assign(object_id, ObjectRecord(head));
      }

      // Tracking for our new object is configured here.

      if ((mc->Flags & CLF::NO_OWNERSHIP) != CLF::NIL) { } // Used by classes like RootModule to avoid tracking back to the task.
      else if ((Flags & NF::UNTRACKED) != NF::NIL) {
         if (class_id IS CLASSID::MODULE); // Untracked modules have no owner, due to the expunge process.
         else {
            // Untracked objects are owned by the current task.  This ensures that the object
            // is deallocated correctly when the Core is closed.

            if (glCurrentTask) {
               ScopedObjectAccess lock(glCurrentTask);
               SetOwner(head, glCurrentTask);
            }
         }
      }
      else { // Track the object to the current context
         auto obj = current_resource();
         if (obj IS &glDummyObject) { // If dummy object, track to the task
            if (glCurrentTask) {
               ScopedObjectAccess lock(glCurrentTask);
               SetOwner(head, glCurrentTask);
            }
         }
         else SetOwner(head, obj);
      }

      // After the header has been created we can set the context, then call the base class's NewObject() support.  If this
      // object belongs to a derived class, we will also call its supporting NewObject() action if it has specified one.
      //
      // Note: Hooking into NewObject gives derived classes an opportunity to detect that they have been targeted by the client
      // on creation, as opposed to during initialisation.  This can allow DerivedPtr to be configured early on in the
      // process, making it possible to set custom fields that would depend on it.

      kt::SwitchContext context(head);

      if (mc->Base) {
         if (mc->Base->ActionTable[int(AC::NewObject)].PerformAction) {
            if ((error = mc->Base->ActionTable[int(AC::NewObject)].PerformAction(head, nullptr)) != ERR::Okay) {
               log.warning(error);
            }
         }
      }

      if ((!error) and (mc->ActionTable[int(AC::NewObject)].PerformAction)) {
         if ((error = mc->ActionTable[int(AC::NewObject)].PerformAction(head, nullptr)) != ERR::Okay) {
            log.warning(error);
         }
      }

      if (!error) {
         ((extMetaClass *)head->Class)->OpenCount++;
         if (mc->Base) mc->Base->OpenCount++;

         *Object = head;
         return ERR::Okay;
      }

      FreeResource(head);
      return error;
   }
   else return ERR::AllocMemory;
}

/*********************************************************************************************************************

-FUNCTION-
NotifySubscribers: Send a notification event to action subscribers.

This function can be used by classes that need fine-tuned control over notification events, as managed by the
~SubscribeAction() function.  Normally the Core will automatically notify subscribers after an action
is executed.  Using NotifySubscribers(), the client can instead manually notify subscribers during the
execution of the action.

Another useful aspect is that the client can control the parameter values that are passed on to the subscribers.

NOTE: The use of NotifySubscribers() does not prevent the core from sending out an action notification as it
normally would, which will cause duplication.  To prevent this, the client must logical-or the return code of
the action function with `ERR::Notified`, e.g. `ERR::Okay|ERR::Notified`.

In the following example the @Surface class uses NotifySubscribers() to convert a Move event to a
Redimension event.  The parameter values are customised to support this, and the function returns `ERR::Notified` to
prevent the core from sending out a Move notification.

<pre>
ERR SURFACE_Move(extSurface *Self, struct acMove *Args)
{
   if (not Args) return ERR::NullArgs|ERR::Notified;

   ...

   struct acRedimension redimension = { Self->X, Self->Y, 0, Self->Width, Self->Height, 0 };
   NotifySubscribers(Self, AC::Redimension, &redimension, ERR::Okay);
   return ERR::Okay|ERR::Notified;
}
</pre>

-INPUT-
obj Object: Pointer to the object that is to receive the notification message.
int(AC) Action: The action ID for notification.
ptr Args: Pointer to an action parameter structure that is relevant to the `Action` ID.
error Error: The error code that is associated with the action result.

-TAGS-
blocking, callback-inlines, does-not-take-ownership

-END-

*********************************************************************************************************************/

void NotifySubscribers(OBJECTPTR Object, ACTIONID ActionID, APTR Parameters, ERR ErrorCode)
{
   kt::Log log(__FUNCTION__);

   // No need for prv_access() since this function is called from within class action code only.

   if (not Object) { log.warning(ERR::NullArgs); return; }
   if ((ActionID <= AC::NIL) or (ActionID >= AC::END)) { log.warning(ERR::Args); return; }

   if (not (Object->NotifyFlags.load() & (1LL<<(int(ActionID) & 63)))) return;

   const std::lock_guard<std::recursive_mutex> lock(glSubLock);

   if ((not glSubscriptions[Object->UID].empty()) and (not glSubscriptions[Object->UID][int(ActionID)].empty())) {
      glSubReadOnly++; // Prevents changes to glSubscriptions while we're processing it.
      for (auto &sub : glSubscriptions[Object->UID][int(ActionID)]) {
         if (sub.Subscriber) {
            kt::SwitchContext ctx(sub.Subscriber);
            sub.Callback(Object, ActionID, ErrorCode, Parameters, sub.Meta);
         }
      }
      glSubReadOnly--;

      if (not glSubReadOnly) {
         if (not glDelayedSubscribe.empty()) { // Check if SubscribeAction() was called during the notification process
            for (auto &entry : glDelayedSubscribe) {
               glSubscriptions[entry.ObjectID][int(entry.ActionID)].emplace_back(entry.Callback.Context, entry.Callback.Routine, entry.Callback.Meta);
            }
            glDelayedSubscribe.clear();
         }

         if (not glDelayedUnsubscribe.empty()) {
            for (auto &entry : glDelayedUnsubscribe) {
               if (Object->UID IS entry.ObjectID) UnsubscribeAction(Object, entry.ActionID);
               else {
                  OBJECTPTR obj;
                  if (!AccessObject(entry.ObjectID, 3000, &obj)) {
                     UnsubscribeAction(obj, entry.ActionID);
                     ReleaseObject(obj);
                  }
               }
            }
            glDelayedUnsubscribe.clear();
         }
      }
   }
   else {
      log.warning("Unstable subscription flags discovered for object #%d, action %d", Object->UID, int(ActionID));
      Object->NotifyFlags.fetch_and(~(1<<(int(ActionID) & 63)), std::memory_order::relaxed);
   }
}

/*********************************************************************************************************************

-FUNCTION-
QueueAction: Delay the execution of an action by adding the call to the message queue.

Use QueueAction() to execute an action by way of the local message queue.  This means that the supplied `Action` and
`Args` will be serialised into a message for the queue.  This function then returns immediately.

The action will be executed on the next cycle of ~ProcessMessages() in line with the FIFO order of queued messages.

-INPUT-
int(AC) Action: The ID of an action or method to execute.
oid Object: The target object.
ptr Args:   The relevant argument structure for the `Action`, or `NULL` if not required.

-ERRORS-
Okay:
NullArgs:
OutOfRange: The `Action` ID is invalid.
MissingClass:

-TAGS-
copies-input, blocking
-END-

*********************************************************************************************************************/

ERR QueueAction(ACTIONID ActionID, OBJECTID ObjectID, APTR Args)
{
   kt::Log log(__FUNCTION__);

   if ((ActionID IS AC::NIL) or (not ObjectID)) return log.warning(ERR::NullArgs);
   if (ActionID >= AC::END) return log.warning(ERR::OutOfRange);

   std::vector<int8_t> buffer;

   ActionMessage action = { .ObjectID = ObjectID, .Time = 0, .ActionID = ActionID, .SendArgs = false };

   if (Args) {
      if (ActionID > AC::NIL) {
         if (ActionTable[int(ActionID)].Size) {
            if (auto error = copy_args(ActionTable[int(ActionID)].Args, ActionTable[int(ActionID)].Size,
                  (int8_t *)Args, buffer); error != ERR::Okay) {
               return error;
            }

            action.SendArgs = true;
         }
      }
      else if (auto cl = (extMetaClass *)FindClass(GetClassID(ObjectID))) {
         if (auto error = copy_args(cl->Methods[-int(ActionID)].Args, cl->Methods[-int(ActionID)].Size,
               (int8_t *)Args, buffer); error != ERR::Okay) {
            return error;
         }
         action.SendArgs = true;
      }
      else return log.warning(ERR::MissingClass);
   }

   buffer.insert(buffer.begin(), (int8_t *)&action, (int8_t *)&action + sizeof(ActionMessage));

   if (auto error = SendMessage(MSGID::ACTION, MSF::NIL, buffer.data(), buffer.size()); error != ERR::Okay) {
      return error;
   }
   else return error;
}

/*********************************************************************************************************************

-FUNCTION-
ResolveClassName: Resolves any class name to a `CLASSID` UID.

This function will resolve a class `Name` to its `CLASSID` UID and verifies that the class is installed.  Class
names are case insensitive.

-INPUT-
strview Name: The name of the class that requires resolution.

-RESULT-
cid: Returns the class ID identified from the class name, or `NULL` if the class could not be found.

-TAGS-
case-insensitive, pure-query
-END-

*********************************************************************************************************************/

CLASSID ResolveClassName(const std::string_view &ClassName)
{
   if (ClassName.empty()) {
      kt::Log(__FUNCTION__).warning(ERR::NullArgs);
      return CLASSID::NIL;
   }

   auto cid = CLASSID(strihash(ClassName));
   if (glClassDB.contains(cid)) return cid;
   else return CLASSID::NIL;
}

/*********************************************************************************************************************

-FUNCTION-
ResolveClassID: Resolve a valid `CLASSID` to its name.

This function will resolve a valid class ID to its equivalent name.  The name is resolved by checking the
class database, so the class must be registered in the database for this function to return successfully.

Registration is achieved by ensuring that the class is compiled into the build.

-INPUT-
cid ID: The ID of the class that needs to be resolved.

-RESULT-
cstr: Returns the name of the class, or `NULL` if the ID is not recognised.  Standard naming conventions apply, so it can be expected that the string is capitalised and without spaces, e.g. `NetSocket`.

-TAGS-
api-owns-result, null-terminated-result, nullable-result, pure-query
-END-

*********************************************************************************************************************/

CSTRING ResolveClassID(CLASSID ID)
{
   if (glClassDB.contains(ID)) return glClassDB[ID].Name.c_str();

   kt::Log(__FUNCTION__).warning("Failed to resolve ID $%.8x", uint32_t(ID));
   return nullptr;
}

/*********************************************************************************************************************

-FUNCTION-
SetOwner: Changes object ownership dynamically.

This function changes the ownership of an existing object.  Ownership is an attribute that affects an object's
placement within the object hierarchy as well as impacting on the resource tracking of the object in question.
Internally, setting a new owner will cause three things to happen:

<list type="ordered">
<li>The new owner's class will receive notification via the #NewChild() action.  If the owner rejects the object by sending back an error, SetOwner() will fail immediately.</li>
<li>The object's class will then receive notification via the #NewOwner() action.</li>
<li>The resource tracking of the new owner will be modified so that the object is accepted as its child.  This means that if and when the owning object is destroyed, the new child object will be destroyed with it.</li>
</list>

If the `Object` does not support the NewOwner action, or the `Owner` does not support the NewChild action, then the
process will not fail.  It will continue on the assumption that neither party is concerned about ownership management.

-INPUT-
obj Object: The object to modify.
obj Owner: The new owner for the `Object`.

-ERRORS-
Okay
NullArgs
Recursion
SystemCorrupt
SystemLocked

-TAGS-
mutates-object, blocking, callback-inlines
-END-

*********************************************************************************************************************/

ERR SetOwner(OBJECTPTR Object, OBJECTPTR Owner)
{
   kt::Log log(__FUNCTION__);

   if ((not Object) or (not Owner)) return log.warning(ERR::NullArgs);

   if (Object->Owner IS Owner) return ERR::Okay;

   if ((Object->ExtClass->Flags & CLF::NO_OWNERSHIP) != CLF::NIL) {
      log.traceWarning("Cannot set the object owner as CLF::NO_OWNERSHIP is set in its class.");
      return ERR::Okay;
   }

   if (Object IS Owner) return log.warning(ERR::Recursion);

   //log.msg("Object: %d, New Owner: %d, Current Owner: %d", Object->UID, Owner->UID, Object->ownerID());

   // Send a new child alert to the owner.  If the owner returns an error then we return immediately.

   ScopedObjectAccess objlock(Object);

   if (!CheckAction(Owner, AC::NewChild)) {
      struct acNewChild newchild = { .Object = Object };
      if (auto error = Action(AC::NewChild, Owner, &newchild); error != ERR::NoSupport) {
         if (error != ERR::Okay) { // If the owner has passed the object through to another owner, return ERR::Okay, otherwise error.
            if (error IS ERR::OwnerPassThrough) return ERR::Okay;
            else return error;
         }
      }
   }

   struct acNewOwner newowner = { .NewOwner = Owner };
   Action(AC::NewOwner, Object, &newowner);

   //if (Object->Owner) log.trace("SetOwner:","Changing the owner for object %d from %d to %d.", Object->UID, Object->ownerID(), Owner->UID);

   // Track the object resource record to the new owner.  Owner resource managers update their own child lists.

   std::unique_lock lock(glmResources);
   std::lock_guard object_lock(glmObjects);
   auto object_rec   = glObjects.find(Object->UID);
   auto resource_rec = glResources.find(Object->UID);
   auto owner_rec    = glResources.find(Owner->UID);

   if ((object_rec IS glObjects.end()) or (resource_rec IS glResources.end())) return log.warning(ERR::SystemCorrupt);

   if ((owner_rec IS glResources.end()) or (not owner_rec->second.Manager) or
      (not owner_rec->second.Manager->AddChild)) {
      return log.warning(ERR::SystemCorrupt);
   }

   auto &resource = resource_rec->second;

   if (resource.OwnerManagesChildren) {
      if (auto previous_owner = glResources.find(resource.OwnerID);
         (previous_owner != glResources.end()) and (previous_owner->second.Manager) and
         (previous_owner->second.Manager->RemoveChild)) {
         previous_owner->second.Manager->RemoveChild(previous_owner->second, resource);
      }
   }
   else if (auto previous_owner = glObjects.find(object_rec->second.OwnerID); previous_owner != glObjects.end()) {
      previous_owner->second.Children.erase(Object->UID);
   }

   object_rec->second.OwnerID = Owner->UID;
   resource.OwnerID = Owner->UID;
   resource.OwnerManagesChildren = false;
   Object->Owner = Owner;

   owner_rec->second.Manager->AddChild(owner_rec->second, resource);
   resource.OwnerManagesChildren = true;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
SetObjectContext: Private.

For internal use only.  Provides an access point for the Object class to manage object context in the Core.

Set either one of Field or ActionID, never both.  If both are empty, the context is that of a resource node.
Resource managers are expected to check up the stack if the operating context is required.

This function does not acquire an object lock, that is the responsibility of the caller if needed.

-INPUT-
obj Object: Object to host the current context.  If NULL, the current context is popped.
struct(Field) Field: Active field, if any.
int(AC) ActionID: Active action, if any.

*********************************************************************************************************************/

void SetObjectContext(OBJECTPTR Object, Field *Field, ACTIONID ActionID)
{
   if (not Object) tlContext.pop_back();
   else tlContext.emplace_back(Object, Field, ActionID);
}

/*********************************************************************************************************************

-FUNCTION-
SetName: Sets the name of an object.

This function sets the name of an `Object`.  This enhances log messages and allows the object to be found in searches.
Note that the length of the `Name` will be limited to the `MAX_NAME_LEN` value in the core header file.  Names
exceeding the allowed length are trimmed to fit.

Object names are limited to alpha-numeric characters and the underscore symbol.  Invalid characters are replaced with
an underscore.

-INPUT-
obj Object: The target object.
strview Name: The new name for the object, or an empty string to clear an existing name.

-ERRORS-
Okay:
NullArgs:
LockFailed:

-TAGS-
copies-input, mutates-object, blocking

*********************************************************************************************************************/

static const char sn_lookup[256] = {
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '0', '1', '2', '3', '4', '5', '6', '7', '8', '_', '_', '_', '_', '_', '_', '_', '_', 'a',
   'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w',
   'x', 'y', '_', '_', '_', '_', '_', '_', '_', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
   'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_',
   '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_', '_'
};

ERR SetName(OBJECTPTR Object, const std::string_view &NewName)
{
   kt::Log log(__FUNCTION__);

   if (not Object) return log.warning(ERR::NullArgs);

   ScopedObjectAccess objlock(Object);

   if (auto lock = std::unique_lock{glmObjectLookup, 4s}) {
      // Remove any existing name first.

      if (Object->Name[0]) remove_object_hash(Object);

      int i, max = std::min<int>(NewName.size(), MAX_NAME_LEN-1);
      for (i=0; i < max; i++) Object->Name[i] = sn_lookup[uint8_t(NewName[i])];
      Object->Name[i] = 0;

      if (Object->Name[0]) glObjectLookup[kt::strihash(Object->Name)].push_back(Object);
      return ERR::Okay;
   }
   else return log.warning(ERR::LockFailed);
}

/*********************************************************************************************************************

-FUNCTION-
SubscribeAction: Monitor action calls made against an object.

The SubscribeAction() function allows a client to receive a callback each time that an action is executed on
an object.  This strategy is referred to as "action monitoring" and is often used for responding to UI
events and the termination of objects.

Subscriptions are context sensitive, so the `Callback` will execute in the space attributed to to the caller.

The following example illustrates how to listen to a @Surface object's Redimension action and respond to resize
events:

<pre>
SubscribeAction(surface, AC::Redimension, C_FUNCTION(notify_resize, meta_ptr));
</pre>

The template below illustrates how the `Callback` function should be constructed:

<pre>
void notify_resize(OBJECTPTR Object, ACTIONID Action, ERR Result, APTR Parameters, APTR CallbackMeta)
{
   auto Self = (objClassType *)CurrentContext();

   // Code here...
   if ((!Result) and (Parameters)) {
      auto resize = (struct acRedimension *)Parameters;
   }
}
</pre>

The `Object` is the original subscription target, as-is the Action ID.  The Result is the error code that was generated
at the end of the action call.  If this is not set to `ERR::Okay`, assume that the action did not have an effect on
state.  The `Parameters` are the original arguments provided by the client - be aware that these can legitimately be
`NULL` even if an action specifies a required parameter structure.  Notice that because subscriptions are context
sensitive, ~CurrentContext() can be used to get a reference to the object that initiated the subscription.

To terminate an action subscription, use the ~UnsubscribeAction() function.  Subscriptions are not resource tracked,
so it is critical to match the original call with an unsubscription.

-INPUT-
obj Object: The target object.
int(AC) Action: The ID of the action that will be monitored.  Methods are not supported.
ptr(func) Callback: A C/C++ function to callback when the action is triggered.

-ERRORS-
Okay:
NullArgs:
Args:
OutOfRange: The Action parameter is invalid.

-TAGS-
callback-held, does-not-take-ownership, blocking

*********************************************************************************************************************/

ERR SubscribeAction(OBJECTPTR Object, ACTIONID ActionID, FUNCTION *Callback)
{
   kt::Log log(__FUNCTION__);

   if ((not Object) or (not Callback)) return log.warning(ERR::NullArgs);
   if ((ActionID < AC::NIL) or (ActionID >= AC::END)) return log.warning(ERR::OutOfRange);
   if (not Callback->isC()) return log.warning(ERR::Args);
   if (Object->collecting()) return ERR::Okay;

   if (glSubReadOnly) {
      glDelayedSubscribe.emplace_back(Object->UID, ActionID, *Callback);
      Object->NotifyFlags.fetch_or(1LL<<(int(ActionID) & 63), std::memory_order::relaxed);
   }
   else {
      std::lock_guard<std::recursive_mutex> lock(glSubLock);

      glSubscriptions[Object->UID][int(ActionID)].emplace_back(Callback->Context, Callback->Routine, Callback->Meta);
      Object->NotifyFlags.fetch_or(1LL<<(int(ActionID) & 63), std::memory_order::relaxed);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
UnsubscribeAction: Terminates action subscriptions.

The UnsubscribeAction() function will terminate subscriptions made by ~SubscribeAction().

To terminate multiple subscriptions in a single call, set the `Action` parameter to zero.

-INPUT-
obj Object: The object that you are unsubscribing from.
int(AC) Action: The ID of the action that will be unsubscribed, or zero for all actions.

-ERRORS-
Okay:
NullArgs:
Args:

-TAGS-
blocking
-END-

*********************************************************************************************************************/

ERR UnsubscribeAction(OBJECTPTR Object, ACTIONID ActionID)
{
   kt::Log log(__FUNCTION__);

   if (not Object) return log.warning(ERR::NullArgs);
   if ((ActionID < AC::NIL) or (ActionID >= AC::END)) return log.warning(ERR::Args);

   if (glSubReadOnly) {
      glDelayedUnsubscribe.emplace_back(Object->UID, ActionID);
      return ERR::Okay;
   }

   std::lock_guard<std::recursive_mutex> lock(glSubLock);

   if (ActionID IS AC::NIL) { // Unsubscribe all actions associated with the subscriber.
      if (glSubscriptions.contains(Object->UID)) {
         auto subscriber = tlContext.back().obj->UID;
         bool need_restart = true;
         while (need_restart) {
            need_restart = false;
            for (auto & [action, list] : glSubscriptions[Object->UID]) {
               // Use C++20 std::erase_if for cleaner removal
               std::erase_if(list, [subscriber](const auto &sub) { return sub.SubscriberID IS subscriber; });

               if (list.empty()) {
                  Object->NotifyFlags.fetch_and(~(1<<(action & 63)), std::memory_order::relaxed);

                  if (not Object->NotifyFlags.load()) {
                     glSubscriptions.erase(Object->UID);
                     break;
                  }
                  else {
                     glSubscriptions[Object->UID].erase(action);
                     need_restart = true;
                     break;
                  }
               }
            }
         }
      }
   }
   else if ((glSubscriptions.contains(Object->UID)) and (glSubscriptions[Object->UID].contains(int(ActionID)))) {
      auto subscriber = tlContext.back().obj->UID;

      auto &list = glSubscriptions[Object->UID][int(ActionID)];
      // Use C++20 std::erase_if for cleaner removal
      std::erase_if(list, [subscriber](const auto &sub) { return sub.SubscriberID IS subscriber; });

      if (list.empty()) {
         Object->NotifyFlags.fetch_and(~(1<<(int(ActionID) & 63)), std::memory_order::relaxed);

         if (not Object->NotifyFlags.load()) {
            glSubscriptions.erase(Object->UID);
         }
         else glSubscriptions[Object->UID].erase(int(ActionID));
      }
   }

   return ERR::Okay;
}
