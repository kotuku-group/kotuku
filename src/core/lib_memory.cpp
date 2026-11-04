/*********************************************************************************************************************

The memory management functions provide a comprehensive memory allocation system with automatic ownership tracking,
resource management, and debugging capabilities. The implementation uses platform-specific memory allocation
functions (typically stdlib malloc/free on Linux) with additional framework features for object lifecycle management.

-CATEGORY-
Name: Memory
-END-

*********************************************************************************************************************/

#ifdef _WIN32
#include <malloc.h> // For _aligned_malloc, _aligned_free
#endif

#ifdef __unix__
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "defs.h"
#include <kotuku/modules/core.h>

using namespace kt;

//********************************************************************************************************************
// Requires a glmResources lock

static void erase_resource(ResourceRecord &Resource)
{
   if ((not glCrashStatus) and Resource.OwnerID) {
      std::lock_guard object_lock(glmObjects);
      if (auto owner = glObjects.find(Resource.OwnerID); owner != glObjects.end()) {
         owner->second.Resources.erase(Resource.ResourceID);
      }
   }

   glResources.erase(Resource.ResourceID);
}

//********************************************************************************************************************
// Calling this function with a non-existent MemoryID is safe

static ERR free_private_memory_resource(MEMORYID MemoryID)
{
   std::unique_lock lock(glmResources);
   auto mem_it = glResources.find(MemoryID);
   if ((mem_it IS glResources.end()) or (not mem_it->second.Address)) {
      if (glCrashStatus) return ERR::Okay;
      else return ERR::DoesNotExist;
   }

   auto &active_mem = mem_it->second;
   auto start_mem = (char *)active_mem.Address - MEMHEADER;

   #ifdef _WIN32
      _aligned_free(start_mem);
   #else
      free(start_mem);
   #endif

   return ERR::Okay;
}

//********************************************************************************************************************
// Resource manager for AllocResource()

static ERR memory_resource_free(ResourceRecord &Resource, APTR Address)
{
   return free_private_memory_resource(Resource.ResourceID);
}

static ResourceManager glResourceMemoryHandler = { "Memory", &memory_resource_free, false };

static_assert(sizeof(ResourceRecord) IS 32);

//********************************************************************************************************************
// The caller must have claimed the pointer-stable record by setting Terminating while holding glmResources.  Resource
// manager callbacks are deliberately invoked without the registry lock because custom managers may block or re-enter
// Core APIs.

static ERR destroy_claimed_resource(ResourceRecord *Resource)
{
   auto error = ERR::Okay;

   if (Resource->Manager IS &glResourceMemoryHandler) {
      error = free_private_memory_resource(Resource->ResourceID);
   }
   else if (not glCrashStatus) {
      error = Resource->Manager->Free(*Resource, Resource->Address);

      if (error IS ERR::Terminate) {
         free_private_memory_resource(Resource->ResourceID);
         error = ERR::Okay;
      }
   }

   std::lock_guard lock(glmResources);

   if (error IS ERR::Okay) erase_resource(*Resource);
   else {
      Resource->Terminating = false;
      if (not Resource->PinCount) Resource->CollectOnUnlock = false;
   }

   return error;
}

//********************************************************************************************************************

void UntrackResource(RESOURCEID ResourceID)
{
   std::unique_lock lock(glmResources);

   auto resource = glResources.find(ResourceID);
   if (resource IS glResources.end()) return;
   if (resource->second.PinCount or resource->second.CollectOnUnlock or resource->second.Terminating) {
      #ifndef NDEBUG
         lock.unlock();
         kt::Log(__FUNCTION__).warning("Resource ID #%d cannot be untracked while pinned or being collected.",
            ResourceID);
      #endif
      return;
   }

   erase_resource(resource->second);
}

/*********************************************************************************************************************

-FUNCTION-
AllocResource: Allocates a managed memory block on the heap.

AllocResource() reserves an area of memory of `Size` bytes and tracks it using the supplied resource manager.  If
no `Manager` is provided, the default memory manager is used.  The function returns a pointer to the allocated memory
block in `Address`.  The allocated memory is automatically associated with the current execution context,
allowing it to be automatically cleaned up when the context is destroyed.

Example usage:

<pre>
APTR address;
if (!AllocResource(1000, MEM::NIL, &address, nullptr)) {
   // Use memory block...
   FreeResource(address);
}
</pre>

Memory blocks are automatically associated with their owning object context, enabling automatic cleanup when
the owner is destroyed. This prevents memory leaks in object-oriented code.

-INPUT-
large Size:     The size of the memory block in bytes. Must be greater than zero.
int(MEM) Flags: Optional allocation flags controlling behaviour and ownership.
&ptr Address: Pointer to store the address of the allocated memory block.
struct(ResourceManager) Manager: Resource manager used to release the resource.

-ERRORS-
Okay: Memory block successfully allocated.
Args: Invalid parameters (size <= 0 or Address is NULL).
AllocMemory: Insufficient memory available for the requested allocation.

-TAGS-
caller-owns-result, creates-resource, blocking
-END-

*********************************************************************************************************************/

ERR AllocResource(int64_t Size, MEM Flags, APTR *Address, ResourceManager *Manager)
{
   if ((Size <= 0) or (not Address)) return kt::Log(__FUNCTION__).warning(ERR::Args);

   *Address = nullptr;

   size_t full_size = Size + MEMHEADER;
   APTR start_mem = nullptr;
   full_size = ((full_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;

   #ifdef _WIN32
      start_mem = _aligned_malloc(full_size, CACHE_LINE_SIZE);
   #else
      if (posix_memalign(&start_mem, CACHE_LINE_SIZE, full_size) != 0) start_mem = nullptr;
   #endif

   if (not start_mem) return kt::Log(__FUNCTION__).warning(ERR::AllocMemory);

   if ((Flags & MEM::NO_CLEAR) IS MEM::NIL) kt::clearmem(start_mem, full_size);

   APTR data_start = (char *)start_mem + MEMHEADER;
   MEMORYID unique_id = glResourceID++;
   ((int *)data_start)[RESOURCE_ID_OFFSET] = unique_id;

   OBJECTID owner_id;
   if (tlContext.size() > 1) owner_id = current_resource()->UID;
   else if (glCurrentTask) owner_id = glCurrentTask->UID;
   else owner_id = 0;

   if (Manager IS nullptr) Manager = &glResourceMemoryHandler;

   if (auto error = TrackResource(unique_id, data_start, owner_id, Manager); error != ERR::Okay) {
      #ifdef _WIN32
         _aligned_free(start_mem);
      #else
         free(start_mem);
      #endif
      return error;
   }

   *Address = data_start;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
CheckResourceExists: Verifies the existence of a resource.

CheckResourceExists() verifies whether a resource with the specified identifier still exists in the system's
object or resource registry. This function is useful for defensive programming when working with resources such as
memory or objects that may have been freed by other code paths.  Objects that are terminating or awaiting deferred
collection are reported as unavailable.

-INPUT-
res ID: The unique identifier of the resource to verify.

-ERRORS-
True: The resource exists and is valid.
False: The resource does not exist or has been freed.

-TAGS-
blocking, pure-query
-END-

*********************************************************************************************************************/

ERR CheckResourceExists(RESOURCEID ResourceID)
{
   {
      std::lock_guard lock(glmObjects);
      if (auto it = glObjects.find(ResourceID); it != glObjects.end()) {
         if ((it->second.Terminating) or (it->second.CollectOnUnlock)) return ERR::False;
         return ERR::True;
      }
   }

   std::lock_guard lock(glmResources);
   if (auto it = glResources.find(ResourceID); it != glResources.end()) {
      if ((it->second.Terminating) or (it->second.CollectOnUnlock)) return ERR::False;
      return ERR::True;
   }
   return ERR::False;
}

/*********************************************************************************************************************

-FUNCTION-
PinResource: Protects a resource from termination until a matching unpin.

PinResource() acquires a lifetime pin for a tracked non-object resource.  Multiple callers may pin the same resource,
but pinning does not serialise access to its contents or make mutation thread-safe.  A successful pin prevents
~FreeResource() and the resource manager from releasing the resource until every acquired pin has been released with
~UnpinResource().

-INPUT-
res ResourceID: The unique identifier of the resource to pin.

-ERRORS-
Okay: One lifetime pin was acquired.
NullArgs: `ResourceID` is zero.
DoesNotExist: No usable non-object resource with this identifier is registered.
MarkedForDeletion: Destruction is pending or already in progress.
OutOfRange: The pin counter is saturated.

-TAGS-
blocking, thread-safe
-END-

*********************************************************************************************************************/

ERR PinResource(RESOURCEID ResourceID)
{
   if (not ResourceID) return ERR::NullArgs;

   std::lock_guard lock(glmResources);
   auto resource = glResources.find(ResourceID);
   if ((resource IS glResources.end()) or (not resource->second.Address)) return ERR::DoesNotExist;

   auto &record = resource->second;
   if (record.Terminating or record.CollectOnUnlock) return ERR::MarkedForDeletion;

   if (record.PinCount IS UINT32_MAX) {
      #ifndef NDEBUG
         kt::Log(__FUNCTION__).warning("Resource ID #%d has reached the lifetime pin limit.", ResourceID);
      #endif
      return ERR::OutOfRange;
   }

   record.PinCount++;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
UnpinResource: Releases a lifetime pin from a resource.

UnpinResource() releases one pin previously acquired with ~PinResource().  If ~FreeResource() requested collection
while the resource was pinned, the caller releasing the final pin performs the deferred manager call and receives its
result.  A failed deferred collection restores the resource to a live, retryable state.

-INPUT-
res ResourceID: The unique identifier of the resource to unpin.

-ERRORS-
Okay: One pin was released and any required deferred destruction succeeded.
NullArgs: `ResourceID` is zero.
DoesNotExist: No usable non-object resource with this identifier is registered.
ResourceNotLocked: The resource has no pin to release.

-TAGS-
blocking, thread-safe
-END-

*********************************************************************************************************************/

ERR UnpinResource(RESOURCEID ResourceID)
{
   if (not ResourceID) return ERR::NullArgs;

   ResourceRecord *resource;
   {
      std::lock_guard lock(glmResources);
      auto resource_it = glResources.find(ResourceID);
      if ((resource_it IS glResources.end()) or (not resource_it->second.Address)) return ERR::DoesNotExist;

      auto &record = resource_it->second;
      if (not record.PinCount) return ERR::ResourceNotLocked;

      record.PinCount--;
      if (record.PinCount or (not record.CollectOnUnlock)) return ERR::Okay;

      record.Terminating = true;
      resource = &record;
   }

   return destroy_claimed_resource(resource);
}

/*********************************************************************************************************************

-FUNCTION-
FreeResource: Safely deallocates resources allocated by AllocResource() and similar functions.

FreeResource() provides safe deallocation of resources with comprehensive validation and cleanup. The function
accepts resource identifiers for optimal safety, though C++ headers also provide pointer-based variants for convenience.

Object identifiers are detected in the object registry and dispatched to ~FreeObject().  All other identifiers are
resolved through the non-object resource registry and its associated `ResourceManager`.

The deallocation process includes lock-aware deallocation that respects access counting, resource manager integration
for managed memory blocks, and automatic cleanup of ownership tracking structures.

If a resource is pinned at the time of the call, it is marked for delayed collection.  The final ~UnpinResource()
performs the deferred destruction and returns the resource manager's result.

-INPUT-
res ID: The unique identifier of the resource to be freed.

-ERRORS-
Okay: The resource was successfully freed.
DoesNotExist: The specified memory block identifier is not valid or already freed.
InUse: The resource is pinned or another caller already owns its destruction.
Terminate

-TAGS-
closes-handle, blocking
-END-

*********************************************************************************************************************/

ERR FreeResource(RESOURCEID ResourceID)
{
   bool is_object;
   {
      std::lock_guard lock(glmObjects);
      is_object = glObjects.contains(ResourceID);
   }
   if (is_object) return FreeObject(ResourceID);

   // Resource pointers are assumed to remain stable according to the map rules.
   // The Terminating flag is set to true to prevent other threads from interfering with the deallocation process.

   // The following responses apply to error codes returned from the resource manager:
   //
   // ERR::Okay      - The manager deallocated the resource, return to user immediately
   // ERR::InUse     - Resource cannot be deallocated yet, do nothing and return error to user
   // ERR::Terminate - Deallocate the resource as a memory block originating from AllocResource()
   // ERR::*         - Return code to user

   ResourceRecord *resource;

   {
      std::lock_guard lock(glmResources);

      auto resource_it = glResources.find(ResourceID);
      if ((resource_it IS glResources.end()) or (not resource_it->second.Address)) {
         kt::Log(__FUNCTION__).trace("Resource ID #%d does not exist.", ResourceID);
         return ERR::DoesNotExist;
      }

      auto &record = resource_it->second;
      if (record.Terminating) return ERR::InUse;

      if (record.PinCount) {
         record.CollectOnUnlock = true;
         return ERR::InUse;
      }

      record.Terminating = true;
      resource = &record;
   }

   return destroy_claimed_resource(resource);
}

/*********************************************************************************************************************

-FUNCTION-
TrackResource: Assign a resource manager to an address, or update an existing one.

TrackResource() registers a resource identifier with the memory manager so that later calls to ~FreeResource() can
dispatch cleanup through the supplied `ResourceManager`.  If the resource identifier is already registered, the existing
record is updated with the non-zero values provided by the caller.

The supplied address and manager are retained as references only.  They must remain valid for as long as the resource is
tracked, or until the record is replaced or removed.  When an `OwnerID` names an object, the resource is added directly
to that object's resource list so it can be removed during object cleanup.  Use `RESOURCEID_INHERIT` to preserve the
existing owner when updating a resource, or to inherit the current context when registering a new resource.

A unique `ResourceID` can be obtained from ~AllocateID() by using `IDTYPE::RESOURCE`.

-INPUT-
res ResourceID: Unique identifier for the resource to register or replace.
ptr Address: Address of the resource, or `NULL` to preserve an existing address.
res OwnerID: Optional owning resource ID, normally an object.  Use `0` when the resource is not owned.
struct(ResourceManager) Manager: Resource manager used to release the resource.

-ERRORS-
Okay
NullArgs: `ResourceID` is `0`, or `Manager` is `NULL` when registering a new resource.
InUse

-TAGS-
retains-input, does-not-take-ownership, blocking, thread-safe

-END-

*********************************************************************************************************************/

ERR TrackResource(RESOURCEID ResourceID, APTR Address, RESOURCEID OwnerID, ResourceManager *Manager)
{
   kt::Log log(__FUNCTION__);
   std::lock_guard lock(glmResources);

   if (not ResourceID) return log.warning(ERR::NullArgs);

   if (auto existing = glResources.find(ResourceID); existing != glResources.end()) {
      auto &record = existing->second;
      if (record.PinCount or record.CollectOnUnlock or record.Terminating) return ERR::InUse;

      if (Address) record.Address = Address; // Assigning a new address to an existing ID is permitted
      if (Manager) record.Manager = Manager; // Switching between the memory manager and custom managers is permitted

      const auto new_owner = (OwnerID IS RESOURCEID_INHERIT) ? record.OwnerID : OwnerID;

      if (record.OwnerID != new_owner) {
         std::lock_guard object_lock(glmObjects);

         if (record.OwnerID) {
            if (auto current_owner = glObjects.find(record.OwnerID); current_owner != glObjects.end()) {
               current_owner->second.Resources.erase(ResourceID);
            }
         }

         record.OwnerID = new_owner;

         if (new_owner) {
            if (auto owner = glObjects.find(new_owner); owner != glObjects.end()) {
               owner->second.Resources.insert(ResourceID);
            }
         }
      }
   }
   else {
      if (not Manager) return log.warning(ERR::NullArgs);

      if (OwnerID IS RESOURCEID_INHERIT) { // Get the owner from the current context
         if (tlContext.size() > 1) OwnerID = current_resource()->UID;
         else if (glCurrentTask) OwnerID = glCurrentTask->UID;
         else OwnerID = 0;
      }

      glResources.insert_or_assign(ResourceID, ResourceRecord(ResourceID, Address, OwnerID, Manager));

      if (OwnerID) {
         std::lock_guard object_lock(glmObjects);
         if (auto owner = glObjects.find(OwnerID); owner != glObjects.end()) {
            owner->second.Resources.insert(ResourceID);
         }
      }
   }

   return ERR::Okay;
}
