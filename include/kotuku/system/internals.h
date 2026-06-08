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
   OBJECTPTR Subscriber;      // The object that is subscribed (pointer, if private)
   OBJECTID  SubscriberID;    // The object that is subscribed
   FUNCTION  Routine;         // Routine to call if not using AC::Timer - ERR Routine(OBJECTID, int, int);
   uint8_t   Cycle;
   bool      Locked;
};

//********************************************************************************************************************
// Memory management record.

class PrivateAddress {
public:
   union {
      APTR      Address;
      OBJECTPTR Object;
   };
   MEMORYID MemoryID;   // Unique identifier
   uint32_t Size;       // 4GB max (user-requested size)
   THREADID ThreadLockID = THREADID(0);
   MEM      Flags;
   int16_t  AccessCount = 0; // Total number of locks

   PrivateAddress(APTR aAddress, MEMORYID aMemoryID, uint32_t aSize, MEM aFlags) :
      Address(aAddress), MemoryID(aMemoryID), Size(aSize), Flags(aFlags) { };

   void clear() {
      Address  = 0;
      MemoryID = 0;
      Flags    = MEM::NIL;
      ThreadLockID = THREADID(0);
   }
};

//********************************************************************************************************************
// Unified resource management record.  These records are keyed by RESOURCEID in glResources and provide the
// ResourceManager dispatch and generic ownership metadata for live resources.

class ResourceRecord {
public:
   APTR       Address = nullptr; // Direct pointer to the resource (optional, can rely on ResourceID instead)
   ResourceManager *Manager = nullptr; // Reference to the resource manager for this record
   RESOURCEID ResourceID = 0;  // Unique identifier
   OBJECTID   OwnerID = 0;     // Owner of the resource
   bool       Collect = false; // Marked for collection while currently in use.
   bool       Terminating = false; // A FreeResource() call currently owns the destruction path.
   bool       OwnerManagesChildren = false; // True if the current OwnerID manages its child resources

   ResourceRecord() = default;

   ResourceRecord(RESOURCEID AResourceID, APTR AAddress, OBJECTID AOwnerID, ResourceManager *AManager) :
      Address(AAddress), Manager(AManager), ResourceID(AResourceID), OwnerID(AOwnerID) { };

   void clear() {
      ResourceID = 0;
      Address = nullptr;
      OwnerID = 0;
      Manager = nullptr;
      Collect = false;
      Terminating = false;
   }
};

//********************************************************************************************************************
// Object management record.  These records are keyed by OBJECTID in glObjects and are used for live object lookup,
// object ownership, and non-child resources tracked to each object.

class ObjectRecord {
public:
   OBJECTPTR Object;
   OBJECTID OwnerID;
   ankerl::unordered_dense::set<OBJECTID> Children; // Object children
   ankerl::unordered_dense::set<RESOURCEID> Resources; // Non-object resources

   ObjectRecord() : Object(nullptr), OwnerID(0) { };

   ObjectRecord(OBJECTPTR AObject, OBJECTID AOwnerID = 0) :
      Object(AObject), OwnerID(AOwnerID) { };

   void clear() {
      Object = nullptr;
      OwnerID = 0;
      Children.clear();
      Resources.clear();
   }
};
