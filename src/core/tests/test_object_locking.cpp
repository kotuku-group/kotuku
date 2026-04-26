/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

This program tests the locking of private objects between threads.

*********************************************************************************************************************/

#include <pthread.h>
#include <kotuku/startup.h>
#include <kotuku/vector.hpp>
#include <kotuku/strings.hpp>

using namespace pf;

CSTRING ProgName = "ObjectLocking";
static volatile OBJECTPTR glConfig = nullptr;
static uint32_t glTotalThreads = 8;
static uint32_t glLockAttempts = 200;
static int glAccessGap = 200000;
static bool glTerminateObject = false;

struct thread_info{
   pthread_t thread;
   int index;
};

#define QUICKLOCK

//********************************************************************************************************************

static void * thread_entry(void *Arg)
{
   pf::Log log(__FUNCTION__);
   ERR error;

   auto info = (thread_info *)Arg;

   info->index = GetResource(RES::THREAD_ID);
   log.msg("----- Thread %d is starting now.", info->index);

   for (unsigned i=0; i < glLockAttempts; i++) {
      if (!glConfig) break;
      //log.branch("Attempt %d.%d: Acquiring the object.", info->index, i);
      #ifdef QUICKLOCK
      if ((error = glConfig->lock()) IS ERR::Okay) {
      #else
      if (!(error = LockObject(glConfig, 30000))) {
      #endif
         glConfig->ActionDepth++;
         log.msg("%d.%d: Object acquired.", info->index, i);
         WaitTime(0.002); // Wait 2 milliseconds
         if (glConfig->ActionDepth > 1) log.error("--- MAJOR ERROR: More than one thread has access to this object!");
         glConfig->ActionDepth--;

         // Test that object removal works in ReleaseObject() and that waiting threads fail peacefully.

         if (glTerminateObject) {
            if (i >= glLockAttempts-2) {
               FreeResource(glConfig);
               #ifdef QUICKLOCK
               glConfig->unlock();
               #else
               ReleaseObject(glConfig);
               #endif
               glConfig = nullptr;
               break;
            }
         }

         #ifdef QUICKLOCK
         glConfig->unlock();
         #else
         ReleaseObject(glConfig);
         #endif

         #ifdef __unix__
            sched_yield();
         #endif
         if (glAccessGap > 0) WaitTime(glAccessGap / 1000000.0); // Convert microseconds to seconds
      }
      else log.msg("Attempt %d.%d: Failed to acquire a lock, error: %s", info->index, i, GetErrorMsg(error));
   }

   log.msg("----- Thread %d is finished.", info->index);
   return nullptr;
}

//********************************************************************************************************************

int main(int argc, CSTRING *argv)
{
   pf::Log log;
   pf::vector<std::string> *args;

   if (auto msg = init_kotuku(argc, argv)) {
      printf("%s\n", msg);
      return -1;
   }

   if ((CurrentTask()->get(FID_Parameters, args) IS ERR::Okay) and (args)) {
      for (unsigned i=0; i < args->size(); i++) {
         if (iequals(args[0][i], "-threads")) {
            if (++i < args->size()) glTotalThreads = strtol(args[0][i].c_str(), nullptr, 0);
            else break;
         }
         else if (iequals(args[0][i], "-attempts")) {
            if (++i < args->size()) glLockAttempts = strtol(args[0][i].c_str(), nullptr, 0);
            else break;
         }
         else if (iequals(args[0][i], "-gap")) {
            if (++i < args->size()) glAccessGap = strtol(args[0][i].c_str(), nullptr, 0);
            else break;
         }
         else if (iequals(args[0][i], "-terminate")) glTerminateObject = true;
      }
   }

   glConfig = objConfig::create::global();

   #ifdef QUICKLOCK
      log.msg("Quick-locking will be tested.");
   #endif

   log.msg("Spawning %d threads...", glTotalThreads);

   thread_info glThreads[glTotalThreads];

   for (unsigned i=0; i < glTotalThreads; i++) {
      glThreads[i].index = i;
      pthread_create(&glThreads[i].thread, nullptr, &thread_entry, &glThreads[i]);
   }

   // Main block now waits for all threads to terminate before it exits.
   // If main block exits, all threads exit, even if the threads have not
   // finished their work

   log.msg("Waiting for thread completion.");

   for (unsigned i=0; i < glTotalThreads; i++) {
      pthread_join(glThreads[i].thread, nullptr);
   }

   FreeResource(glConfig);

   printf("Testing complete.\n");

   close_kotuku();
}
