/*********************************************************************************************************************

The memory management functions provide a comprehensive memory allocation system with automatic ownership tracking,
resource management, and debugging capabilities. The implementation uses platform-specific memory allocation
functions (typically stdlib malloc/free on Linux) with additional framework features for object lifecycle management.

-CATEGORY-
Name: Memory
-END-

*********************************************************************************************************************/

#include <stdlib.h> // Contains free(), malloc() etc

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

#define freemem(a)  free(a)

using namespace kt;

// Align to 64-byte cache line boundaries for better performance on modern CPUs
constexpr size_t CACHE_LINE_SIZE = 64;

//********************************************************************************************************************

static ERR memory_resource_free(ResourceRecord *Resource, APTR Address)
{
   kt::Log log("FreeMemory");

   MEMORYID memory_id = Resource->ResourceID;

   if (auto lock = std::unique_lock{glmMemory}) {
      auto mem_it = glPrivateMemory.find(memory_id);
      if ((mem_it IS glPrivateMemory.end()) or (not mem_it->second.Address)) {
         log.trace("Memory ID #%d does not exist.", memory_id);
         return ERR::MemoryDoesNotExist;
      }

      auto &active_mem = mem_it->second;

      if (active_mem.AccessCount > 0) {
         log.msg("Block #%d marked for collection (open count %d).", memory_id, active_mem.AccessCount);
         active_mem.Flags |= MEM::COLLECT;
         Resource->Collect = true;
         return ERR::Okay;
      }

      auto start_mem = (char *)active_mem.Address - sizeof(int) - sizeof(int);

      if ((active_mem.Flags & MEM::PROTECTED) != MEM::NIL) {
         #ifdef _WIN32
            winFreeProtectedMemory(start_mem, align_page_size(active_mem.Size + MEMHEADER));
         #else
            munmap(start_mem, align_page_size(active_mem.Size + MEMHEADER));
         #endif
      }
      else {
         #ifdef _WIN32
            _aligned_free(start_mem);
         #else
            free(start_mem);
         #endif
      }

      // TODO: Should be moved to the object's resource manager

      if ((active_mem.Flags & MEM::OBJECT) != MEM::NIL) {
         if (auto object_it = glObjects.find(Resource->OwnerID); object_it != glObjects.end()) {
            object_it->second.Children.erase(memory_id);
         }
         glObjects.erase(memory_id);
      }
      else if (auto object_it = glObjects.find(Resource->OwnerID); object_it != glObjects.end()) {
         object_it->second.Resources.erase(memory_id);
      }

      glResources.erase(memory_id);
      active_mem.clear();
      if (glProgramStage != STAGE_SHUTDOWN) glPrivateMemory.erase(memory_id);

      return ERR::Okay;
   }
   else return log.warning(ERR::SystemLocked);
}

static ResourceManager glResourceMemoryHandler = {
   "Memory",
   &memory_resource_free,
   false
};

/*********************************************************************************************************************

Internal resource registry helpers.

*********************************************************************************************************************/

ResourceRecord * find_resource(RESOURCEID ResourceID)
{
   auto resource = glResources.find(ResourceID);
   if (resource IS glResources.end()) return nullptr;
   return &resource->second;
}

/*********************************************************************************************************************

-FUNCTION-
TrackResource: Assign a resource manager to an address, or update an existing one.

TrackResource() registers a resource identifier with the memory manager so that later calls to ~FreeResource() can
dispatch cleanup through the supplied !ResourceManager.  If the resource identifier is already registered, the existing
record is updated with the non-zero values provided by the caller.

The supplied address and manager are retained as references only.  They must remain valid for as long as the resource is
tracked, or until the record is replaced or removed.  When an `OwnerID` is supplied for a non-object resource, the
resource is added to the owner's resource list so it can be removed during owner cleanup.  Use `RESOURCEID_INHERIT` to
preserve the existing owner when updating a resource, or to inherit the current context when registering a new resource.

-INPUT-
res ResourceID: Unique identifier for the resource to register or replace.
ptr Address: Address of the resource, or `NULL` to preserve an existing address.
res OwnerID: Optional owning resource ID, normally an object.  Use `0` when the resource is not owned.
struct(ResourceManager) Manager: Resource manager used to release the resource.
large Size: Size of the tracked resource in bytes, or `0` to preserve an existing size.

-ERRORS-
Okay
NullArgs: `ResourceID` is `0`, or `Manager` is `NULL` when registering a new resource.

-TAGS-
retains-input, does-not-take-ownership, blocking, thread-safe

-END-

*********************************************************************************************************************/

ERR TrackResource(RESOURCEID ResourceID, APTR Address, RESOURCEID OwnerID, ResourceManager *Manager, int64_t Size)
{
   std::lock_guard lock(glmMemory); // TODO: Use a resource specific mutex

   if (not ResourceID) return ERR::NullArgs;

   if (auto existing = glResources.find(ResourceID); existing != glResources.end()) {
      const auto previous_owner_id = existing->second.OwnerID;
      const auto previous_manager = existing->second.Manager;

      if (Address) existing->second.Address = Address;
      if (Manager) existing->second.Manager = Manager;
      if (Size) existing->second.Size = uint32_t(Size);

      const bool inherit_owner = OwnerID IS RESOURCEID_INHERIT;
      const auto effective_owner_id = inherit_owner ? previous_owner_id : OwnerID;

      if ((previous_owner_id != effective_owner_id) or (previous_manager != existing->second.Manager)) {
         // TODO: Call the previous manager's RemoveChild() implementation, if defined.
         // TODO: Get the OwnerID's resource manager, then call its AddChild() implementation, if defined.
         if (auto object_rec = glObjects.find(previous_owner_id); object_rec != glObjects.end()) {
            object_rec->second.Resources.erase(ResourceID);
            object_rec->second.Children.erase(ResourceID);
         }

         if (not inherit_owner) existing->second.OwnerID = OwnerID;
      }

      if ((existing->second.Manager != &glResourceObject) and effective_owner_id) {
         // TODO: Get the OwnerID's resource manager, then call its AddChild() implementation, if defined.
         if (auto object_rec = glObjects.find(effective_owner_id); object_rec != glObjects.end()) {
            object_rec->second.Resources.insert(ResourceID);
         }
      }
   }
   else {
      if (not Manager) return ERR::NullArgs;

      if (OwnerID IS RESOURCEID_INHERIT) { // Get the owner from the current context
         if (tlContext.size() > 1) OwnerID = current_resource()->UID;
         else if (glCurrentTask) OwnerID = glCurrentTask->UID;
         else OwnerID = 0;
      }

      glResources.insert_or_assign(ResourceID, ResourceRecord(ResourceID, Address, OwnerID, Manager, Size));

      if ((Manager != &glResourceObject) and OwnerID) {
         // TODO: Get the OwnerID's resource manager, then call its AddChild() implementation, if defined.
         if (auto object_rec = glObjects.find(OwnerID); object_rec != glObjects.end()) {
            object_rec->second.Resources.insert(ResourceID);
         }
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

void UntrackResource(RESOURCEID ResourceID)
{
   std::lock_guard lock(glmMemory);

   auto resource = glResources.find(ResourceID);
   if (resource IS glResources.end()) return;

   if (resource->second.OwnerID) {
      // TODO: Get the OwnerID's resource manager, then call its RemoveChild() implementation, if defined.
      if (auto object_rec = glObjects.find(resource->second.OwnerID); object_rec != glObjects.end()) {
         object_rec->second.Resources.erase(ResourceID);
         object_rec->second.Children.erase(ResourceID);
      }
   }

   glResources.erase(resource);
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
if (AllocMemory(1000, MEM::DATA, &address) IS ERR::Okay) {
   // Use memory block...
   FreeResource(address);
}
</pre>

Memory allocation behavior is controlled through MEM flags:

<types lookup="MEM"/>

The function can return both a memory address pointer and a unique memory identifier. For most applications,
retrieving only the address pointer is sufficient. When both parameters are requested, the memory block is
automatically locked, requiring an explicit call to ReleaseMemory() before freeing.

The resulting memory block is zero-initialized unless the `MEM::NO_CLEAR` flag is specified. For large
allocations where initialization overhead is a concern, utilising `MEM::NO_CLEAR` is recommended.

Memory blocks are automatically associated with their owning object context, enabling automatic cleanup when
the owner is destroyed. This prevents memory leaks in object-oriented code.

-INPUT-
large Size:     The size of the memory block in bytes. Must be greater than zero.
int(MEM) Flags: Optional allocation flags controlling behavior and ownership.
&ptr Address: Pointer to store the address of the allocated memory block.

-ERRORS-
Okay: Memory block successfully allocated.
Args: Invalid parameters (size <= 0 or Address is NULL).
AllocMemory: Insufficient memory available for the requested allocation.
AccessMemory: Memory block was allocated but could not be locked when both Address and ID were requested.
SystemLocked: Memory management system is currently locked by another thread.

-TAGS-
caller-owns-result, creates-resource, blocking
-END-

*********************************************************************************************************************/

ERR AllocMemory(int64_t Size, MEM Flags, APTR *Address)
{
   kt::Log log(__FUNCTION__);

   if ((Size <= 0) or (not Address)) {
      log.warning("Bad args - Size %" PF64 ", Address %p", (long long)Size, Address);
      return ERR::Args;
   }

   if (Address) *Address = nullptr;

   // Determine the object that will own the memory block.  The preferred default is for it to belong to the current context.

   OBJECTID owner_id = 0;
   if ((Flags & MEM::UNTRACKED) != MEM::NIL);
   else if (tlContext.size() > 1) owner_id = current_resource()->UID;
   else if (glCurrentTask) owner_id = glCurrentTask->UID;

   size_t full_size = Size + MEMHEADER;
   size_t aligned_size = full_size;

   // Check if memory protection is requested
   bool use_protection = ((Flags & (MEM::READ|MEM::WRITE)) != MEM::NIL);
   APTR start_mem = nullptr;

   if (use_protection) {
      // Use OS-level memory protection with mmap/VirtualAlloc
      aligned_size = align_page_size(full_size);
      #ifdef _WIN32
         start_mem = winAllocProtectedMemory(aligned_size, int(Flags));
      #else
         int prot = PROT_NONE;
         if ((Flags & MEM::READ) != MEM::NIL) prot |= PROT_READ;
         if ((Flags & MEM::WRITE) != MEM::NIL) prot |= PROT_WRITE;

         start_mem = mmap(nullptr, aligned_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         if (start_mem IS MAP_FAILED) start_mem = nullptr;
      #endif

      if (start_mem) {
         Flags |= MEM::PROTECTED; // Mark as protected for proper cleanup
         if ((Flags & MEM::NO_CLEAR) IS MEM::NIL) {
            if ((Flags & MEM::WRITE) != MEM::NIL) kt::clearmem(start_mem, full_size);
            else log.trace("Note: Read-only memory will not be cleared.");
         }
      }
   }
   else {
      // Use standard aligned allocation (typically 64-bit) for non-protected memory
      full_size = ((full_size + CACHE_LINE_SIZE - 1) / CACHE_LINE_SIZE) * CACHE_LINE_SIZE;

      #ifdef _WIN32
         start_mem = _aligned_malloc(full_size, CACHE_LINE_SIZE);
      #else
         if (posix_memalign(&start_mem, CACHE_LINE_SIZE, full_size) != 0) start_mem = nullptr;
      #endif

      if (start_mem) {
         if ((Flags & MEM::NO_CLEAR) IS MEM::NIL) kt::clearmem(start_mem, full_size);
      }
   }

   if (not start_mem) {
      log.warning("Failed to allocate %" PF64 " bytes.", (long long)Size);
      return ERR::AllocMemory;
   }

   APTR data_start = (char *)start_mem + sizeof(int) + sizeof(int); // Skip unique ID and alignment padding.

   if (auto lock = std::unique_lock{glmMemory}) { // To keep threads synced, it is essential that this lock is made early.
      MEMORYID unique_id = glPrivateIDCounter++;

      // Configure the memory header.

      ((int *)start_mem)[0]  = unique_id;

      // Record memory details such as the size, ID and flags.  This helps us
      // with resource tracking, identifying the memory block and freeing it later on.  Hidden blocks are never recorded.

      glPrivateMemory.insert(std::pair<MEMORYID, PrivateAddress>(unique_id, PrivateAddress(data_start, unique_id, (uint32_t)Size, Flags)));
      TrackResource(unique_id, data_start, owner_id, &glResourceMemoryHandler, Size);

      if (Address) *Address = data_start;

      if (glShowPrivate) log.pmsg("AllocMemory(%p/#%d, %" PF64 ", $%.8x, Owner: #%d)", data_start, unique_id, (long long)Size, int(Flags), owner_id);
      return ERR::Okay;
   }
   else {
      if (use_protection) {
         #ifdef _WIN32
            winFreeProtectedMemory(start_mem, aligned_size);
         #else
            munmap(start_mem, aligned_size);
         #endif
      }
      else {
         #ifdef _WIN32
            _aligned_free(start_mem);
         #else
            free(start_mem);
         #endif
      }
      return log.warning(ERR::SystemLocked);
   }
}

/*********************************************************************************************************************

-FUNCTION-
CheckMemoryExists: Verifies the existence of a memory block.

CheckMemoryExists() validates whether a memory block with the specified identifier still exists in the system's
memory tracking structures. This function is useful for defensive programming when working with memory identifiers
that may have been freed by other code paths.

-INPUT-
mem ID: The unique identifier of the memory block to verify.

-ERRORS-
True: The memory block exists and is valid.
False: The memory block does not exist or has been freed.

-TAGS-
blocking, pure-query
-END-

*********************************************************************************************************************/

ERR CheckMemoryExists(MEMORYID MemoryID)
{
   if (auto lock = std::unique_lock{glmMemory}) {
      if (glPrivateMemory.contains(MemoryID)) return ERR::True;
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

When a memory block is currently locked (AccessCount > 0), it is marked for delayed collection rather than
immediate deallocation. This prevents use-after-free errors while ensuring eventual cleanup when all references
are released.

-INPUT-
res ID: The unique identifier of the resource to be freed.

-ERRORS-
Okay: The resource was successfully freed or marked for delayed collection.
MemoryDoesNotExist: The specified memory block identifier is not valid or already freed.
SystemLocked: Memory management system is currently locked by another thread.
InUse: The memory block is a busy managed resource.  The removal behaviour rules are dependent on the manager (automatic termination may be employed).

-TAGS-
closes-handle, blocking
-END-

*********************************************************************************************************************/

ERR FreeResource(RESOURCEID MemoryID)
{
   kt::Log log(__FUNCTION__);

   if (auto lock = std::unique_lock{glmMemory}) { // TODO Use a new mutex
      RESOURCEID resource_id = MemoryID;
      auto resource_it = glResources.find(resource_id);
      if ((resource_it != glResources.end()) and (resource_it->second.Address)) {
         ResourceRecord *resource = &resource_it->second; // Take a copy

         if (glShowPrivate) log.branch("FreeResource(#%d, %p, Size: %d, Owner: #%d)", MemoryID,
            resource->Address, resource->Size, resource->OwnerID);

         // TODO: This only applies to memory resources and should be moved to memory_resource_free

         auto mem_it = glPrivateMemory.find(MemoryID);
         if ((mem_it != glPrivateMemory.end()) and (mem_it->second.Address) and (mem_it->second.AccessCount > 0)) {
            log.msg("Block #%d marked for collection (open count %d).", MemoryID, mem_it->second.AccessCount);
            mem_it->second.Flags |= MEM::COLLECT;
            resource_it->second.Collect = true;
            return ERR::Okay;
         }

         // Fast route for direct memory deallocation

         if (resource->Manager IS &glResourceMemoryHandler) {
            return memory_resource_free(&resource_it->second, resource->Address);
         }

         // Use the resource manager if possible.  Resource managers are not considered safe in an uncontrolled shutdown.

         if (not glCrashStatus) {
            auto free_address = resource->Address;
            auto resource_manager = resource->Manager;

            // If the manager can block, drop the memory lock to prevent deadlocking.
            if (resource_manager->CanBlock) lock.unlock();

            // The following responses apply to error codes returned from the resource manager:
            //
            // ERR::Okay      - The manager deallocated the resource, return to user immediately
            // ERR::InUse     - Resource cannot be deallocated yet, do nothing and return error to user
            // ERR::Terminate - Deallocate the resource as a standard memory block
            // ERR::*         - Return code to user

            auto error = resource_manager->Free(resource, free_address);

            if (resource_manager->CanBlock) lock.lock();

            if (error IS ERR::Okay) {
               // Resource manager took care of the deallocation, we just need to remove the record
               // TODO: Using a glDirtyResources marker could eliminate the need for this
               if (auto current_resource = glResources.find(resource_id); current_resource != glResources.end()) {
                  glResources.erase(current_resource);
               }
               return ERR::Okay;
            }
            else if (error != ERR::Terminate) return error; // Do nothing and return the error code

            // Drop-through to standard memory deallocation

            resource_it = glResources.find(resource_id);
            if ((resource_it IS glResources.end()) or (not resource_it->second.Address)) return ERR::Okay;
            resource = &resource_it->second;

            TrackResource(resource_id, resource->Address, RESOURCEID_INHERIT, &glResourceMemoryHandler,
               resource->Size);

            mem_it = glPrivateMemory.find(MemoryID);
            if ((mem_it != glPrivateMemory.end()) and (mem_it->second.Address) and (mem_it->second.AccessCount > 0)) {
               log.msg("Block #%d marked for collection (open count %d).", MemoryID, mem_it->second.AccessCount);
               mem_it->second.Flags |= MEM::COLLECT;
               resource->Collect = true;
               return ERR::Okay;
            }
         }

         // If resource manager unavailable, revert to using memory deallocation if the UID is recognised.

         mem_it = glPrivateMemory.find(MemoryID);
         if ((mem_it != glPrivateMemory.end()) and (mem_it->second.Address)) {
            return memory_resource_free(resource, mem_it->second.Address);
         }

         glResources.erase(resource_id);
         return ERR::Okay;
      }
      log.trace("Resource ID #%d does not exist.", resource_id);
      return ERR::MemoryDoesNotExist;
   }
   else return log.warning(ERR::SystemLocked);
}

/*********************************************************************************************************************

-FUNCTION-
MemoryIDInfo: Returns information on memory ID's.

This function returns the attributes of a memory block, including the start address, parent object, memory ID, size
and flags.  The following example illustrates correct use of this function:

<pre>
MemInfo info;
if (MemoryIDInfo(memid, &info) IS ERR::Okay) {
   log.msg("Memory block #%d is %d bytes large.", info.MemoryID, info.Size);
}
</pre>

If the call fails, the !MemInfo structure's fields will be driven to `NULL` and an error code is returned.

-INPUT-
mem ID: Pointer to a valid memory ID.
buf(struct(MemInfo)) MemInfo:  Pointer to a !MemInfo structure.
structsize Size: Size of the !MemInfo structure.

-ERRORS-
Okay
NullArgs
Args
MemoryDoesNotExist
SystemLocked

-TAGS-
mutates-input, blocking, pure-query
-END-

*********************************************************************************************************************/

ERR MemoryIDInfo(MEMORYID MemoryID, MemInfo *MemInfo, int Size)
{
   kt::Log log(__FUNCTION__);

   if ((not MemInfo) or (not MemoryID)) return log.warning(ERR::NullArgs);
   if ((size_t)Size < sizeof(MemInfo)) return log.warning(ERR::Args);

   clearmem(MemInfo, Size);

   if (auto lock = std::unique_lock{glmMemory}) {
      auto mem = glPrivateMemory.find(MemoryID);
      if ((mem != glPrivateMemory.end()) and (mem->second.Address)) {
         MemInfo->Start       = mem->second.Address;
         MemInfo->Size        = mem->second.Size;
         MemInfo->AccessCount = mem->second.AccessCount;
         MemInfo->Flags       = mem->second.Flags;
         MemInfo->MemoryID    = mem->second.MemoryID;
         return ERR::Okay;
      }
      else return ERR::MemoryDoesNotExist;
   }
   else return log.warning(ERR::SystemLocked);
}

/*********************************************************************************************************************

-FUNCTION-
MemoryPtrInfo: Returns information on memory addresses.

This function returns the attributes of a memory block.  Information includes the start address, parent object,
memory ID, size and flags of the memory address that you are querying.  The following code segment illustrates
correct use of this function:

<pre>
MemInfo info;
if (MemoryPtrInfo(ptr, &info) IS ERR::Okay) {
   log.msg("Address %p is %d bytes large.", info.Start, info.Size);
}
</pre>

If the call to MemoryPtrInfo() fails then the !MemInfo structure's fields will be driven to `NULL` and an error code
will be returned.

Please note that referencing by a pointer requires a slow reverse-lookup to be employed in this function's search
routine.  We recommend that calls to this function are avoided unless circumstances absolutely require it.

-INPUT-
ptr Address:  Pointer to a valid memory area.
buf(struct(MemInfo)) MemInfo: Pointer to a !MemInfo structure to be populated.
structsize Size: Size of the !MemInfo structure.

-ERRORS-
Okay
NullArgs
Args
MemoryDoesNotExist
SystemLocked

-TAGS-
mutates-input, blocking, pure-query

*********************************************************************************************************************/

ERR MemoryPtrInfo(APTR Memory, MemInfo *MemInfo, int Size)
{
   kt::Log log(__FUNCTION__);

   if ((not MemInfo) or (not Memory)) return log.warning(ERR::NullArgs);
   if ((size_t)Size < sizeof(MemInfo)) return log.warning(ERR::Args);

   clearmem(MemInfo, Size);

   // Search private addresses.  This is a bit slow, but if the memory pointer is guaranteed to have
   // come from AllocMemory() then the optimal solution for the client is to pull the ID from
   // (int *)Memory)[-2] first and call MemoryIDInfo() instead.

   if (auto lock = std::unique_lock{glmMemory}) {
      for (const auto & [ id, mem ] : glPrivateMemory) {
         if (Memory IS mem.Address) {
            MemInfo->Start       = Memory;
            MemInfo->Size        = mem.Size;
            MemInfo->AccessCount = mem.AccessCount;
            MemInfo->Flags       = mem.Flags;
            MemInfo->MemoryID    = mem.MemoryID;
            return ERR::Okay;
         }
      }
      log.warning("Private memory address %p is not valid.", Memory);
      return ERR::MemoryDoesNotExist;
   }
   else return log.warning(ERR::SystemLocked);
}

/*********************************************************************************************************************

-FUNCTION-
ProtectMemory: Change the access permissions of a memory block.

This function changes the access permissions of a memory block that was allocated with the `MEM::READ` and/or
`MEM::WRITE` flags.  This allows you to tighten or relax the access permissions of a memory block as your program's
logic requires.

-INPUT-
ptr Address: Pointer to a memory block obtained from ~AllocMemory().
int(MEM) Flags: New access flags (MEM::READ, MEM::WRITE).

-ERRORS-
Okay
NullArgs: Address is NULL.
Args: Invalid flags specified or memory block is not protected.
MemoryDoesNotExist: The memory block is not valid or was not allocated with protection.
SystemCall: A system call failed.

-TAGS-
blocking
-END-

*********************************************************************************************************************/

ERR ProtectMemory(APTR Address, MEM Flags)
{
   kt::Log log(__FUNCTION__);

   if (not Address) return ERR::NullArgs;
   if ((Flags & (MEM::READ | MEM::WRITE)) IS MEM::NIL) return ERR::Args;

   if (glShowPrivate) log.branch("ProtectMemory(%p, $%.8x)", Address, int(Flags));

   MemInfo meminfo;
   if (MemoryIDInfo(GetMemoryID(Address), &meminfo, sizeof(meminfo)) IS ERR::Okay) {
      if ((meminfo.Flags & MEM::PROTECTED) IS MEM::NIL) {
         log.warning("Memory block at %p is not protected.", Address);
         return ERR::Args;
      }

      auto start_mem = (char *)Address - sizeof(int) - sizeof(int);
      auto full_size = meminfo.Size + MEMHEADER;
      auto aligned_size = align_page_size(full_size);

      #ifdef _WIN32
         if (winProtectMemory(start_mem, aligned_size, (Flags & MEM::READ) != MEM::NIL, (Flags & MEM::WRITE) != MEM::NIL, false)) {
            return ERR::Okay;
         }
         else return log.warning(ERR::SystemCall);
      #else
         int prot = PROT_NONE;
         if ((Flags & MEM::READ) != MEM::NIL) prot |= PROT_READ;
         if ((Flags & MEM::WRITE) != MEM::NIL) prot |= PROT_WRITE;

         if (mprotect(start_mem, aligned_size, prot) IS 0) {
            return ERR::Okay;
         }
         else return log.warning(ERR::SystemCall);
      #endif
   }
   else return ERR::MemoryDoesNotExist;
}

/*********************************************************************************************************************

-FUNCTION-
ReallocMemory: Reallocates memory blocks.

This function is used to reallocate memory blocks to new lengths. You can shrink or expand a memory block as you
wish.  The data of your original memory block will be copied over to the new block.  If the new block is of a
larger size, the left-over bytes will be populated with zero-byte values. If the new block is smaller, you will
lose some of the original data.

The original block will be destroyed as a result of calling this function unless the reallocation process fails, in
which case your existing memory block will remain valid.

-INPUT-
ptr Memory:   Pointer to a memory block obtained from ~AllocMemory().
uint Size:    The size of the new memory block.
!ptr Address: Point to an `APTR` variable to store the resulting pointer to the new memory block.

-ERRORS-
Okay
Args
NullArgs
AllocMemory
Memory: The memory block to be re-allocated is invalid.

-TAGS-
caller-owns-result, creates-resource, closes-handle, blocking
-END-

*********************************************************************************************************************/

ERR ReallocMemory(APTR Address, uint32_t NewSize, APTR *Memory)
{
   kt::Log log(__FUNCTION__);

   if (Memory) *Memory = Address; // If we fail, return the original memory block

   if ((not Address) or (NewSize <= 0) or (not Memory)) {
      log.function("Address: %p, NewSize: %d, &Memory: %p", Address, NewSize, Memory);
      return log.warning(ERR::Args);
   }

   // Check the validity of what we have been sent

   MemInfo meminfo;
   if (MemoryIDInfo(GetMemoryID(Address), &meminfo, sizeof(meminfo)) != ERR::Okay) {
      return log.warning(ERR::Memory);
   }

   if (meminfo.Size IS NewSize) return ERR::Okay;

   if (glShowPrivate) log.branch("Address: %p, NewSize: %d", Address, NewSize);

   // Allocate the new memory block and copy the data across

   if (AllocMemory(NewSize, meminfo.Flags, Memory) IS ERR::Okay) {
      auto copysize = (NewSize < meminfo.Size) ? NewSize : meminfo.Size;
      copymem(Address, *Memory, copysize);

      // Free the old memory block.  If it is locked then we also release it for the caller.

      if (meminfo.AccessCount > 0) ReleaseMemory(Address);
      FreeResource(Address);

      return ERR::Okay;
   }
   else return log.warning(ERR::AllocMemory);
}
