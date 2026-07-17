// All records in the system internals subject to change, no guarantees are made as to stability

#pragma once

//********************************************************************************************************************
// These values are set against glProgramStage to indicate the current state of the program (either starting up, active
// or shutting down).

enum {
   STAGE_STARTUP=1,
   STAGE_ACTIVE,
   STAGE_SHUTDOWN
};

//********************************************************************************************************************
// Crash index numbers.  Please note that the order of this index must match the order in which resources are freed in
// the shutdown process.

enum {
   CP_START=1,
   CP_PRINT_CONTEXT,
   CP_PRINT_ACTION,
   CP_REMOVE_PRIVATE_LOCKS,
   CP_BROADCAST,
   CP_REMOVE_TASK,
   CP_REMOVE_TABLES,
   CP_FREE_ACTION_MANAGEMENT,
   CP_FREE_COREBASE,
   CP_FREE_PRIVATE_MEMORY,
   CP_FINISHED
};

//********************************************************************************************************************
// This structure is used for internally timed broadcasting.

class CoreTimer {
public:
   int64_t   NextCall;        // Cycle when PreciseTime() reaches this value (us)
   int64_t   LastCall;        // PreciseTime() recorded at the last call (us)
   int64_t   Interval;        // The amount of microseconds to wait at each interval
   int64_t   PendingInterval; // Switch to this interval after completing the next cycle
   OBJECTPTR Subscriber;      // Weak-pinned subscriber, or null for internal subscriptions with no subscriber
   FUNCTION  Routine;         // Routine to call on trigger
   uint8_t   Cycle;
   bool      Locked;
};

//********************************************************************************************************************
// Object management record.  These records are keyed by OBJECTID in glObjects and are used for live object lookup,
// object ownership, and non-child resources tracked to each object.

class ObjectRecord {
public:
   OBJECTPTR Object;
   // Can be null or a valid pointer whilst glmObjects is held.  Nulled by object_free() when the owner dies first.
   OBJECTPTR Owner; 
   // Object children entries are valid pointers whilst glmObjects is held.  Pinning of child objects is unnecessary,
   // the code has been designed for this and maintaining that behaviour is essential.
   ankerl::unordered_dense::set<OBJECTPTR> Children;
   ankerl::unordered_dense::set<RESOURCEID> Resources; // Non-object resources

   ObjectRecord() : Object(nullptr), Owner(nullptr) { };

   ObjectRecord(OBJECTPTR AObject, OBJECTPTR AOwner = nullptr) :
      Object(AObject), Owner(AOwner) { };

   void clear() {
      Object = nullptr;
      Owner = nullptr;
      Children.clear();
      Resources.clear();
   }
};
