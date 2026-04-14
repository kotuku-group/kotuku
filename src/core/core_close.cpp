
static void free_private_memory(void);

//********************************************************************************************************************

static void free_classes(void)
{
   #ifdef __ANDROID__
   if (glAssetClass) { FreeResource(glAssetClass); glAssetClass = 0; }
   #endif
   if (glCompressedStreamClass) { FreeResource(glCompressedStreamClass); glCompressedStreamClass  = 0; }
   if (glArchiveClass)      { FreeResource(glArchiveClass);      glArchiveClass      = 0; }
   if (glCompressionClass)  { FreeResource(glCompressionClass);  glCompressionClass  = 0; }
   if (glScriptClass)       { FreeResource(glScriptClass);       glScriptClass       = 0; }
   if (glFileClass)         { FreeResource(glFileClass);         glFileClass         = 0; }
   if (glStorageClass)      { FreeResource(glStorageClass);      glStorageClass      = 0; }
   if (glConfigClass)       { FreeResource(glConfigClass);       glConfigClass       = 0; }
   if (glTimeClass)         { FreeResource(glTimeClass);         glTimeClass         = 0; }
   if (glModuleClass)       { FreeResource(glModuleClass);       glModuleClass       = 0; }
   if (glThreadClass)       { FreeResource(glThreadClass);       glThreadClass       = 0; }
   if (glRootModuleClass)   { FreeResource(glRootModuleClass);   glRootModuleClass   = 0; }
}

//********************************************************************************************************************
// Remove the Task object and remaining children

static void remove_task(void)
{
   if (glCurrentTask) {
      pf::Log log("Shutdown");
      log.branch("Freeing the task object and its resources.");
      FreeResource(glCurrentTask);
      glCurrentTask = nullptr;
   }
}

//********************************************************************************************************************

static void remove_schedulers(void)
{
   if (glCacheTimer) {
      auto id = glCacheTimer;
      glCacheTimer = 0;
      UpdateTimer(id, 0);
   }

   if (glProcessJanitor) {
      auto id = glProcessJanitor;
      glProcessJanitor = 0;
      UpdateTimer(id, 0);
   }
}

//********************************************************************************************************************
// Remove orphaned object locks

static void remove_object_locks(void)
{
   pf::Log log(__FUNCTION__);

   if (auto lock = std::unique_lock{glmMemory}) {
      for (const auto & [ id, mem ] : glPrivateMemory) {
         if (((mem.Flags & MEM::OBJECT) != MEM::NIL) and (mem.AccessCount > 0)) {
            if (auto obj = mem.Object) {
               log.warning("Removing locks on object #%d, Owner: %d, Locks: %d", obj->UID, obj->Owner ? obj->Owner->UID : 0, mem.AccessCount);
               for (auto count=mem.AccessCount; count > 0; count--) ReleaseObject(obj);
            }
         }
      }
   }
}

//********************************************************************************************************************

void CloseCore(void)
{
   pf::Log log("Shutdown");

   if (glCodeIndex IS CP_FINISHED) return;

   log.msg("PROGRAM TERMINATING");

   AdjustLogLevel(1);

   #ifdef __unix__
      log.msg("UID: %d, EUID: %d, CrashStatus: %d", getuid(), geteuid(), glCrashStatus);
   #endif

   glProgramStage = STAGE_SHUTDOWN;

   // Critical variables are re-calculated for the close process - this will 'repair' any possible damage that may have
   // been caused to our internal data.

   tlContext.resize(1);
   tlDepth   = 0;

   if (glClassFile) { FreeResource(glClassFile); glClassFile = nullptr; }

   free_events(); // Remove event subscriptions

   // Destroy all other tasks in our instance that we have created.

   {
      pf::Log log("Shutdown");
      log.branch("Removing any child processes...");

      #ifdef KILL_PROCESS_GROUP
         // Kill all processes that have been created by this process and its children
         killpg(0, SIGHUP);
      #else
         for (auto &task : glTasks) {
            log.msg("Removing sub-process #%d (pid %d).", task.TaskID, task.ProcessID);

            #ifdef __unix__
               // SIGHUP will convert to MSGID::QUIT in the signal handlers.  The main reason for us to use it is
               // to stop foreign processes that we've launched.
               kill(task.ProcessID, SIGHUP);
            #else
               SendMessage(MSGID::QUIT, MSF::NIL, nullptr, 0);
            #endif

            WaitTime(-0.1); // Wait 0.1 seconds without processing messages
         }
      #endif
   }

   // Wait for sub-tasks to die within the designated time limit

   log.msg("Waiting for %d child processes to terminate...", int(glTasks.size()));

   static const int TIME_TO_DIE = 6; // Seconds to wait before a task has to die
   auto wait_until = PreciseTime() + (TIME_TO_DIE * 1000000);
   while ((!glTasks.empty()) and (PreciseTime() < wait_until)) {
      for (auto it = glTasks.begin(); it != glTasks.end(); ) {
         if (it->ProcessID) {
            #ifdef __unix__
            if (kill(it->ProcessID, 0)) { // Process exists?
               it = glTasks.erase(it);
               continue;
            }
            #endif
            log.msg("Process %d is still live.", it->ProcessID);
         }
         it++;
      }

      WaitTime(-0.1); // Wait 0.1 seconds without processing messages
   }

   // If the time-to-die has elapsed and sub-tasks are still in the system, send kill messages to force them out.

   #ifdef __unix__
      if (!glTasks.empty()) {
         for (auto &task : glTasks) {
            log.warning("Sending a kill signal to sub-task #%d (process %d).", task.TaskID, task.ProcessID);
            if ((task.ProcessID) and (task.ProcessID != glProcessID)) {
               kill(task.ProcessID, SIGTERM);
            }
         }
         WaitTime(-0.2); // Wait 0.2 seconds without processing messages
         glTasks.clear();
      }
   #endif

   // Run the video recovery routine if one has been set and we have crashed

   if ((glCrashStatus) and (glVideoRecovery)) {
      void (*routine)(void);
      routine = glVideoRecovery;
      glVideoRecovery = nullptr;
      routine();
   }

   stop_async_actions();
   deregister_thread(); // Remove main thread from the thread registry

   if ((glCurrentTask) or (glProcessID)) remove_process_waitlocks();

   if (!glCrashStatus) { // This code is only safe to execute if the process hasn't crashed.
      if (glTime) { FreeResource(glTime); glTime = nullptr; }

      // Removing objects that are tracked to the task before the first expunge will make for a cleaner exit.

      if (glCurrentTask) {
         const auto children = glObjectChildren[glCurrentTask->UID]; // Take an immutable copy of the resource list

         if (children.size() > 0) {
            log.branch("Freeing %d objects allocated to task #%d.", (int)children.size(), glCurrentTask->UID);

            for (const auto id : children) FreeResource(id);
         }
         else log.msg("There are no child objects belonging to task #%d.", glCurrentTask->UID);
      }

      // Make our first attempt at expunging all modules.

      Expunge(false);

      remove_schedulers();
      remove_task();
      remove_object_locks();

      Expunge(false); // Second expunge.  Safety measures are still engaged.

      if (!glCrashStatus) {
         #ifdef __linux__
            if (glFileMonitor) { FreeResource(glFileMonitor); glFileMonitor = nullptr; }
         #endif

         free_file_cache();

         if (glInotify != -1) { close(glInotify); glInotify = -1; }
      }

      Expunge(true); // Third and final expunge.  Forcibly unloads modules.

      VirtualVolume("archive", VAS::DEREGISTER, TAGEND);

      while (glMsgHandlers) FreeResource(glMsgHandlers);
      glLastMsgHandler = nullptr;

      free_classes();

      #ifdef __unix__
         if (glSocket != -1) RegisterFD(glSocket, RFD::REMOVE, nullptr, nullptr);
      #endif

      // Report FD's that have not been removed by the client

      if (!glCrashStatus) {
         for (auto &fd : glFDTable) {
            log.warning("FD %" PRId64 " was not deregistered prior to program close.  Routine: %p, Data: %p, Flags: $%.8x", (int64_t)fd.FD, fd.Routine, fd.Data, int(fd.Flags));
         }
      }
   }

   if (glCodeIndex < CP_REMOVE_PRIVATE_LOCKS) {
      glCodeIndex = CP_REMOVE_PRIVATE_LOCKS;

      log.msg("Removing all resource locks.");

      if (auto lock = std::unique_lock{glmMemory}) {
         for (auto & [ id, mem ] : glPrivateMemory) {
            if ((mem.Address) and (mem.AccessCount > 0)) {
               if (!glCrashStatus) log.msg("Removing %d locks on private memory block #%d, size %d.", mem.AccessCount, mem.MemoryID, mem.Size);
               mem.AccessCount = 0;
            }
         }
      }
   }

   if (!glCrashStatus) {
      if (glTaskClass) { FreeResource(glTaskClass); glTaskClass = 0; }
   }

   if (glCodeIndex < CP_FREE_COREBASE) {
      glCodeIndex = CP_FREE_COREBASE;
      if (LocalCoreBase) { FreeResource(LocalCoreBase); LocalCoreBase = nullptr; }
   }

   if (glCodeIndex < CP_FREE_PRIVATE_MEMORY) {
      glCodeIndex = CP_FREE_PRIVATE_MEMORY;
      free_private_memory();
   }

   #ifdef _WIN32
      free_threadlocks();
      winShutdown();
   #endif

   #ifdef __APPLE__
      int socklen;
      struct sockaddr_un *sockpath = get_socket_path(glProcessID, &socklen);
      unlink(sockpath->sun_path);
   #endif

   glProcessID = 0;

   if (glCodeIndex < CP_FINISHED) glCodeIndex = CP_FINISHED;

   fflush(stdout);
   fflush(stderr);

   // NOTE: LeakSanitizer can sometimes report segfault errors on closure.  These can go away on their own and may
   // not be easily duplicated.  One possible explanation is tom-foolery from LuaJIT resulting in false positives that
   // are hard to replicate.  Another is C++ destructors.  Module expunging can also result in false positives.
}

//********************************************************************************************************************
// Calls all loaded modules with an Expunge notification.
//
// NOTE: If forced expunging occurs, it usually means that two modules have loaded each other.  This means that they
// will always have an open count of at least 1 each.

// TODO: Expunging cannot occur while other threads are active.  In that case, the expunge should be delayed until no additional threads are running.

__export void Expunge(int16_t Force)
{
   pf::Log log(__FUNCTION__);

   if (!tlMainThread) {
      log.warning("Only the main thread can expunge modules.");
      return;
   }

   log.branch("Expunging loaded modules.");

   int16_t mod_count = -1;
   int16_t ccount   = 0;
   int16_t Pass     = 1;

   while (ccount > mod_count) {
      mod_count = ccount;
      auto mod_master = glModuleList;
      log.msg("Stage 1 pass #%d", Pass++);

      while (mod_master) {
         auto next = mod_master->Next;

         if (mod_master->OpenCount <= 0) {
            // Search for classes that have been created by this module and check their open count values to figure out
            // if the module code is in use.

            bool class_in_use = false;
            for (const auto & id : glObjectChildren[mod_master->UID]) {
               auto mem = glPrivateMemory.find(id);
               if (mem IS glPrivateMemory.end()) continue;

               auto mc = (extMetaClass *)mem->second.Address;
               if ((mc) and (mc->classID() IS CLASSID::METACLASS) and (mc->OpenCount > 0)) {
                  log.msg("Module %s manages a class that is in use - Class: %s, Count: %d.", mod_master->Name.c_str(), mc->ClassName, mc->OpenCount);
                  class_in_use = true;
               }
            }

            if (!class_in_use) {
               if (mod_master->Expunge) {
                  pf::Log log(__FUNCTION__);
                  log.branch("Expunging %s module #%d.", mod_master->Name.c_str(), mod_master->UID);
                  if (auto error = mod_master->Expunge(); error IS ERR::Okay) {
                     ccount++;
                     if (FreeResource(mod_master) != ERR::Okay) {
                        log.warning("RootModule is corrupt");
                        mod_count = ccount; // Break the loop because the chain links are broken.
                        break;
                     }
                  }
                  else if (error != ERR::DoNotExpunge) log.msg("Module \"%s\" rejected expunge request.",mod_master->Name.c_str());
               }
               else {
                  ccount++;
                  if (FreeResource(mod_master) != ERR::Okay) {
                     log.warning("RootModule is corrupt");
                     mod_count = ccount; // Break the loop because the chain links are broken.
                     break;
                  }
               }
            }
         }
         else log.msg("Module \"%s\" has an open count of %d.", mod_master->Name.c_str(), mod_master->OpenCount);
         mod_master = next;
      }
   }

   if (Force) {
      // Any modules that are still in the system are probably there because they have created classes that have
      // objects still in use.  This routine prints warning messages to let the developer know about this.  (NB: There
      // can be times where objects are tracked outside of the process space and therefore will not be
      // destroyed by earlier routines.  This is normal and they will be taken out when the private memory resources
      // are deallocated).

      log.msg("Stage 2 expunge testing.");

      mod_count = -1;
      ccount   = 0;
      while (ccount > mod_count) {
         mod_count = ccount;
         auto mod_master = glModuleList;
         log.msg("Stage 2 pass #%d", Pass++);
         while (mod_master) {
            auto next = mod_master->Next;
            if (mod_master->OpenCount <= 0) {
               // Search for classes that have been created by this module and check their open count values to figure
               // out if the module code is in use.

               for (const auto & id : glObjectChildren[mod_master->UID]) {
                  auto mem = glPrivateMemory.find(id);
                  if (mem IS glPrivateMemory.end()) continue;

                  auto mc = (extMetaClass *)mem->second.Address;
                  if ((mc) and (mc->classID() IS CLASSID::METACLASS) and (mc->OpenCount > 0)) {
                     log.warning("Warning: The %s module holds a class with existing objects (Class: %s, Objects: %d)", mod_master->Name.c_str(), mc->ClassName, mc->OpenCount);
                  }
               }
            }
            else log.msg("Module \"%s\" has an open count of %d.", mod_master->Name.c_str(), mod_master->OpenCount);
            mod_master = next;
         }
      }

      // If we are shutting down, force the expunging of any stubborn modules

      auto mod_master = glModuleList;
      while (mod_master) {
         auto next = mod_master->Next;
         if (mod_master->Expunge) {
            pf::Log log(__FUNCTION__);
            log.branch("Forcing the expunge of stubborn module %s.", mod_master->Name.c_str());
            mod_master->Expunge();
            mod_master->NoUnload = true; // Do not actively destroy the module code as a precaution (e.g. X11 Display module doesn't like it)
            FreeResource(mod_master);
         }
         else {
            ccount++;
            FreeResource(mod_master);
         }
         mod_master = next;
      }
   }
}

//********************************************************************************************************************

static void free_private_memory(void)
{
   pf::Log log("Shutdown");

   log.branch("Checking for orphaned memory allocations...");

   int count = 0;

   if (auto lock = std::unique_lock{glmMemory}) {
      // Free strings first

      for (auto & [ id, mem ] : glPrivateMemory) {
         if ((mem.Address) and ((mem.Flags & MEM::STRING) != MEM::NIL)) {
            if (!glCrashStatus) log.warning("Unfreed string \"%.80s\" (%p, #%d)", (CSTRING)mem.Address, mem.Address, mem.MemoryID);
            mem.AccessCount = 0;
            FreeResource(mem.Address);
            mem.Address = nullptr;
            count++;
         }
      }

      // Free all other memory blocks

      for (auto & [ id, mem ] : glPrivateMemory) {
         if (mem.Address) {
            if (!glCrashStatus) {
               if ((mem.Flags & MEM::OBJECT) != MEM::NIL) {
                  log.warning("Unfreed object #%d, Size %d, Class: $%.8x, Container: #%d.", 
                     mem.MemoryID, mem.Size, uint32_t(mem.Object->classID()), mem.OwnerID);
               }
               else log.warning("Unfreed memory #%d/%p, Size %d, Container: #%d, Locks: %d, ThreadLock: %d.", 
                  mem.MemoryID, mem.Address, mem.Size, mem.OwnerID, mem.AccessCount, int(mem.ThreadLockID));
            }
            mem.AccessCount = 0;
            FreeResource(mem.Address);
            mem.Address = nullptr;
            count++;
         }
      }
   }

   if ((glCrashStatus) and (count > 0)) log.msg("%d memory blocks were freed.", count);
}
