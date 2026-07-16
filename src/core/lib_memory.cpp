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
   if (not glCrashStatus) {
      if (Resource.OwnerManagesChildren) {
         auto parent = glResources.find(Resource.OwnerID);
         if ((parent != glResources.end()) and (parent->second.Manager) and (parent->second.Manager->RemoveChild)) {
            parent->second.Manager->RemoveChild(parent->second, Resource);
         }
         Resource.OwnerManagesChildren = false;
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

static ERR memory_resource_free(ResourceRecord &Resource, APTR Address)
{
   return free_private_memory_resource(Resource.ResourceID);
}

static ResourceManager glResourceMemoryHandler = { "Memory", &memory_resource_free, nullptr, nullptr, false };

//********************************************************************************************************************

void UntrackResource(RESOURCEID ResourceID)
{
   std::lock_guard lock(glmResources);

   auto resource = glResources.find(ResourceID);
   if (resource IS glResources.end()) return;
   if (resource->second.Terminating) return;

   erase_resource(resource->second);
}

/*********************************************************************************************************************

-FUNCTION-
AllocMemory: Allocates a managed memory block on the heap.

AllocMemory() provides comprehensive memory allocation with automatic ownership tracking, resource management, and
debugging features. The function allocates a new block of memory and associates it with the current execution context,
allowing it to be automatically cleaned up when the context is destroyed.

Example usage:

<pre>
APTR address;
if (!AllocMemory(1000, MEM::NIL, &address)) {
   // Use memory block...
   FreeResource(address);
}
</pre>

Memory allocation behaviour is controlled through MEM flags:

<types lookup="MEM"/>

The resulting memory block is zero-initialised unless the `MEM::NO_CLEAR` flag is specified.  For large
allocations where initialisation overhead is a concern, utilising `MEM::NO_CLEAR` is recommended.

Memory blocks are automatically associated with their owning object context, enabling automatic cleanup when
the owner is destroyed. This prevents memory leaks in object-oriented code.

-INPUT-
large Size:     The size of the memory block in bytes. Must be greater than zero.
int(MEM) Flags: Optional allocation flags controlling behaviour and ownership.
&ptr Address: Pointer to store the address of the allocated memory block.

-ERRORS-
Okay: Memory block successfully allocated.
Args: Invalid parameters (size <= 0 or Address is NULL).
AllocMemory: Insufficient memory available for the requested allocation.

-TAGS-
caller-owns-result, creates-resource, blocking
-END-

*********************************************************************************************************************/

ERR AllocMemory(int64_t Size, MEM Flags, APTR *Address)
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

   if (auto error = TrackResource(unique_id, data_start, owner_id, &glResourceMemoryHandler); error != ERR::Okay) {
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
resource tracking collection. This function is useful for defensive programming when working with resources
such as memory or objects that may have been freed by other code paths.

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
   std::unique_lock lock(glmResources);
   if (auto it = glResources.find(ResourceID); it != glResources.end()) {
      if ((it->second.Terminating) or (it->second.CollectOnUnlock)) return ERR::False;
      return ERR::True;
   }
   return ERR::False;
}

/*********************************************************************************************************************

-FUNCTION-
FreeResource: Safely deallocates resources allocated by AllocMemory() and similar functions.

FreeResource() provides safe deallocation of resources with comprehensive validation and cleanup. The function
accepts resource identifiers for optimal safety, though C++ headers also provide pointer-based variants for convenience.

The deallocation process includes lock-aware deallocation that respects access counting, resource manager integration
for managed memory blocks, and automatic cleanup of ownership tracking structures.

If a resource is locked at the time of the call, it is marked for delayed collection only. This prevents
use-after-free errors while ensuring eventual cleanup when all references are released.

-INPUT-
res ID: The unique identifier of the resource to be freed.

-ERRORS-
Okay: The resource was successfully freed or marked for delayed collection.
DoesNotExist: The specified memory block identifier is not valid or already freed.
InUse: The resource is busy.  The removal behaviour rules are dependent on the manager (automatic termination may be employed).
Terminate

-TAGS-
closes-handle, blocking
-END-

*********************************************************************************************************************/

ERR FreeResource(RESOURCEID ResourceID)
{
   // Resource pointers are assumed to remain stable according to the map rules.
   // The Terminating flag is set to true to prevent other threads from interfering with the deallocation process.

   // The following responses apply to error codes returned from the resource manager:
   //
   // ERR::Okay      - The manager deallocated the resource, return to user immediately
   // ERR::InUse     - Resource cannot be deallocated yet, do nothing and return error to user
   // ERR::Terminate - Deallocate the resource as a standard memory block
   // ERR::*         - Return code to user

   ResourceRecord *resource;

   {
      std::lock_guard lock(glmResources);

      auto resource_it = glResources.find(ResourceID);
      if ((resource_it IS glResources.end()) or (not resource_it->second.Address)) {
         kt::Log(__FUNCTION__).trace("Resource ID #%d does not exist.", ResourceID);
         return ERR::DoesNotExist;
      }

      if (resource_it->second.Terminating) return ERR::InUse;

      resource_it->second.Terminating = true;
      resource = &resource_it->second;
   }

   auto error = ERR::Okay;

   // Fast route for direct memory deallocation

   if (resource->Manager IS &glResourceMemoryHandler) {
      error = memory_resource_free(*resource, resource->Address);
   }
   else {
      // Use the resource manager if under normal operating conditions.

      if (not glCrashStatus) {
         error = resource->Manager->Free(*resource, resource->Address);

         if (error IS ERR::Terminate) { // Manager requested regular memory cleanup.
            free_private_memory_resource(ResourceID);
            error = ERR::Okay;
         }
      }
      else error = free_private_memory_resource(ResourceID);
   }

   if (!error) {
      std::lock_guard lock(glmResources);
      erase_resource(*resource);
   }
   else resource->Terminating = false; // No glmResources lock necessary, the resource will ultimately be deallocated

   return error;
}

/*********************************************************************************************************************

-FUNCTION-
TrackResource: Assign a resource manager to an address, or update an existing one.

TrackResource() registers a resource identifier with the memory manager so that later calls to ~FreeResource() can
dispatch cleanup through the supplied `ResourceManager`.  If the resource identifier is already registered, the existing
record is updated with the non-zero values provided by the caller.

The supplied address and manager are retained as references only.  They must remain valid for as long as the resource is
tracked, or until the record is replaced or removed.  When an `OwnerID` is supplied for a non-object resource, the
resource is added to the owner's resource list so it can be removed during owner cleanup.  Use `RESOURCEID_INHERIT` to
preserve the existing owner when updating a resource, or to inherit the current context when registering a new resource.

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
      if (record.Terminating) return ERR::InUse;

      if (Address) record.Address = Address; // Assigning a new address to an existing ID is permitted
      if (Manager) record.Manager = Manager; // Switching between the memory manager and custom managers is permitted

      const auto new_owner = (OwnerID IS RESOURCEID_INHERIT) ? record.OwnerID : OwnerID;

      if (record.OwnerID != new_owner) {
         if (record.OwnerManagesChildren) {
            auto current_owner = glResources.find(record.OwnerID);
            if ((current_owner != glResources.end()) and (current_owner->second.Manager->RemoveChild)) {
               current_owner->second.Manager->RemoveChild(current_owner->second, record);
            }
         }

         record.OwnerID = new_owner;
         record.OwnerManagesChildren = false; // Revert to standard behaviour

         if (new_owner) {
            auto owner_record = glResources.find(OwnerID);
            if ((owner_record != glResources.end()) and (owner_record->second.Manager->AddChild)) {
               owner_record->second.Manager->AddChild(owner_record->second, record);
               record.OwnerManagesChildren = true;
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

      auto resource = glResources.insert_or_assign(ResourceID, ResourceRecord(ResourceID, Address, OwnerID, Manager));

      if (OwnerID) {
         auto owner_record = glResources.find(OwnerID);
         if ((owner_record != glResources.end()) and (owner_record->second.Manager->AddChild)) {
            owner_record->second.Manager->AddChild(owner_record->second, resource.first->second);
            resource.first->second.OwnerManagesChildren = true;
         }
      }
   }

   return ERR::Okay;
}
