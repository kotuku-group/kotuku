/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

*********************************************************************************************************************/

#include <kotuku/main.h>
#include <kotuku/startup.h>
#include <kotuku/system/task.h>
#include <kotuku/data/script.h>
#include <kotuku/data/compression.h>
#include <kotuku/files/file.h>
#include <kotuku/functions/strtoint.c>

#include <kotuku/modules/filesystem.h>

CSTRING ProgName = "Origo";

extern struct CoreBase *CoreBase;
struct FileSystemBase *FileSystemBase;

static CSTRING STR_UNPACK = "temp:scripts/";
static CSTRING STR_MAIN   = "main.tiri";

static OBJECTID glTargetID = 0;
static STRING glDirectory = nullptr, *glArgs = nullptr;
static char glBind[40] = { 0 };

static OBJECTPTR glTask = nullptr;
static OBJECTPTR glScript = nullptr;

static ERROR decompress_archive(CSTRING);
static ERROR prep_environment(int, int, int);
static ERROR exec_script(CSTRING, OBJECTID *, int, STRING);
static ERROR PROGRAM_ActionNotify(OBJECTPTR, struct acActionNotify *);

static const char Help[] = {
"This command-line program will execute scripts written for Kotuku.  The core distribution supports Tiri (.tiri)\n\
scripts.  Quick start:\n\
\n\
   origo [args] [script.ext] arg1 arg2 ...\n\
\n\
Available commands:\n\
\n\
 --help      : Prints this help page.\n\
 --procedure : The name of a procedure to execute.\n\
 --program   : Treat the script like a program.  The process will enter message handling mode if execution succeeds.\n\
 --persistent: Similar to -program but ensures that there is no exit until a quit message has been posted to the\n\
               message queue.\n\
 --bind      : Follow this argument with an object name to bind the process to a specific object after script\n\
               execution.  If the object is not found, the program aborts.\n\
 --progonly  : If the script does not explicitly declare itself as a program, abort the execution process.\n\
 --target    : The name of an object that the script will target for the first object's initialisation.  Used in\n\
               multi-tasking environments.\n\
 --time      : Print the amount of time that it took to execute the script.\n\
\n\
 --debug    : Activates run-time debug output.\n\
 --errors   : Activates the output of run-time errors.\n"
};

//********************************************************************************************************************

extern "C" void program(void)
{
   pf::Log log;

   int i, j;

   glTask = CurrentTask();
   bool time       = false;
   int winhandle  = 0;
   STRING procedure  = nullptr;
   STRING scriptfile = nullptr;
   int width      = 0;
   int height     = 0;

   FileSystemBase = (struct FileSystemBase *)GetResourcePtr(RES::FILESYSTEM);

   // Process arguments

   STRING *Args;
   if ((!glTask->get(FID_ArgsList, &Args)) and (Args)) {
      for (i=0; Args[i]; i++) {
         if (iequals(Args[i], "--help")) {
            // Print help for the user
            printf(Help);
            goto exit;
         }
         else if (iequals(Args[i], "--time")) {
            time = true;
         }
         else if (iequals(Args[i], "--info")) {
            printf("Instance: %d\n", GetResource(RES::INSTANCE));
         }
         else if (iequals(Args[i], "--instance")) {
            glTask->get(FID_Instance, &j);
            printf("Instance: %d\n", j);
         }
         else if (iequals(Args[i], "--winhandle")) { // Target a desktop window in the host environment
            if (Args[i+1]) {
               if ((winhandle = StrToInt(Args[i+1]))) i++;
            }
         }
         else if (iequals(Args[i], "--width")) {
            if (Args[i+1]) {
               if ((width = StrToInt(Args[i+1]))) i++;
            }
         }
         else if (iequals(Args[i], "--height")) {
            if (Args[i+1]) {
               if ((height = StrToInt(Args[i+1]))) i++;
            }
         }
         else if (iequals(Args[i], "--procedure")) {
            if (procedure) { FreeResource(procedure); procedure = nullptr; }

            if (Args[i+1]) {
               for (j=0; Args[i+1][j]; j++);
               if (!AllocMemory(j+1, MEM::STRING|MEM::NO_CLEAR, &procedure)) {
                  for (j=0; Args[i+1][j]; j++) procedure[j] = Args[i+1][j];
                  procedure[j] = 0;
               }
               i++;
            }
         }
         else if (iequals(Args[i], "--target")) {
            if (Args[i+1]) {
               if (FindObject(Args[i+1], 0, FOF::SMART_NAMES, &TargetID) != ERR_Okay) {
                  printf("Warning - could not find target object \"%s\".\n", Args[i+1]);
               }
               else log.msg("Using target %d", TargetID);
            }
         }
         else if (iequals(Args[i], "--hash")) {
            if (Args[i+1]) {
               auto hash = LCASEHASH(Args[i+1]);
               printf("Hash for %s = 0x%.8x\n", Args[i+1], hash);
               i++;
            }
         }
         else {
            // If argument not recognised, assume this arg is the script file.
            if (ResolvePath(Args[i], RSF::APPROXIMATE, &scriptfile)) {
               printf("Unable to find file '%s'\n", Args[i]);
               goto exit;
            }
            if (Args[i+1]) glArgs = Args + i + 1;
            break;
         }
      }
   }

   if (!scriptfile) { // Abort if no script file to load.
      printf(Help);
      goto exit;
   }

   STRING path;
   if (!glTask->get(FID_Path, &path)) log.msg("Path: %s", path);
   else log.error("No working path.");

   if (winhandle) {
      if (prep_environment(winhandle, width, height) != ERR_Okay) {
         printf("Failed to prepare an environment for running this script.\n");
         goto exit;
      }
   }

   LOC path_type;
   if ((AnalysePath(scriptfile, &path_type) != ERR_Okay) or (path_type != LOC::FILE)) {
      printf("File '%s' does not exist.\n", scriptfile);
      goto exit;
   }

   if (exec_script(scriptfile, time, procedure)) goto exit;

   if (winhandle) acShow(TargetID);

   ProcessMessages(0, 0);

exit:
   log.msg("Origo now exiting...");

   if (CoreObjectID) {
      OBJECTPTR object;
      if (!AccessObject(CoreObjectID, 5000, &object)) {
         UnsubscribeAction(object, AC::NIL, glTask->UniqueID);
         ReleaseObject(object);
      }
   }

   if (procedure) { FreeResource(procedure); procedure = nullptr; }
   if (scriptfile) { FreeResource(scriptfile); scriptfile = nullptr; }

   if (glDirectory) {
      for (i=0; glDirectory[i]; i++);
      while ((i > 0) and (glDirectory[i-1] != '/')) i--;
      glDirectory[i] = 0;

      OBJECTPTR file;
      if (!PrivateObject(ID_FILE, 0, &file, FID_Path|TSTR, glDirectory, TAGEND)) {
         flDelete(file, 0);
         FreeResource(file);
      }

      FreeResource(glDirectory);
   }

   if (glScript) FreeResource(glScript);
}

//********************************************************************************************************************
// Prepares a special environment for running scripts.

ERROR prep_environment(int WindowHandle, int Width, int Height)
{
   pf::Log log(__FUNCTION__);

   log.branch("Win: %d, Size: %dx%d", WindowHandle, Width, Height);

   objSurface::create target = { fl::Name("SystemSurface"), fl::WindowHandle(WindowHandle),
      fl::XCoord(0), fl::YCoord(0), fl::Width(Width), fl::Height(Height) };

   if (target.ok()) {
      objPointer::create pointer = { fl::Owner(target->UID), fl::Name("SystemPointer") }
      if (pointer.ok()) {
         objScript::create script = { fl::Path("templates:defaultvariables.tiri"), fl::Target(target->UID) };
         if (script.ok()) return acActivate(script);
         else return ERR_CreateObject;
      }
      else return ERR_CreateObject;
   }
   else return ERR_CreateObject;
}

//********************************************************************************************************************
// Runs scripts.

ERROR exec_script(CSTRING ScriptFile, OBJECTID *CoreObjectID, int ShowTime, STRING Procedure)
{
   int i, j, k;
   ERROR error;
   int8_t argbuffer[100], *argname;

   *CoreObjectID = 0;

   CLASSID class_id, subclass;
   if (!(error = IdentifyFile(ScriptFile, &class_id, &subclass))) {
      if (class_id IS ID_COMPRESSION) {
         // The Tiri source may be a compressed file that contains multiple script files.  This part of the routine will decompress the contents to "temp:scripts/".

         if (decompress_archive(ScriptFile) != ERR_Okay) {
            printf("Failed to decompress the script archive.\n");
            return(ERR_Failed);
         }
         ScriptFile = glDirectory;
         class_id = CLASSID::SCRIPT;
      }
      else if (class_id != CLASSID::SCRIPT) {
         OBJECTPTR run;

         // The script is actually a reference to a data file, in which case we may be able to run it, if it has a file association.

         if (!PrivateObject(ID_RUN, 0, &run, FID_Location|TSTR, ScriptFile, TAGEND)) {
            if (glArgs) {
               argname = argbuffer+1; // Skip the first byte... reserved for '+'
               argbuffer[0] = '+'; // Append arg indicator
               for (i=0; glArgs[i]; i++) {
                  for (j=0; (glArgs[i][j]) and (glArgs[i][j] != '=') and (j < (int)sizeof(argbuffer)-10); j++) argname[j] = glArgs[i][j];
                  argname[j] = 0;

                  if (glArgs[i][j] IS '=') {
                     j++;
                     if (glArgs[i][j] IS '{') {
                        // Array definition, e.g. files={ file1.txt file2.txt }

                        // Check in case of no gap, e.g. files={arg1 ... }

                        j++;
                        if (glArgs[i][j] > 0x20) {
                           SetKey(run, argbuffer, glArgs[i] + j);
                        }

                        i++;
                        while ((glArgs[i]) and (glArgs[i][0] != '}')) {
                           SetKey(run, argbuffer, glArgs[i]);
                           i++;
                        }
                     }
                     else if (glArgs[i][j] IS '"') {
                        j++;
                        for (k=j; (glArgs[i][k]) and (glArgs[i][k] != '"'); k++);
                        if (glArgs[i][k] IS '"') glArgs[i][k] = 0;
                        SetKey(run, argname, glArgs[i]+j);
                     }
                     else SetKey(run, argname, glArgs[i]+j);
                  }
                  else SetKey(run, argname, "1");
               }
            }

            acActivate(run);
            FreeResource(run);
         }

         return(ERR_LimitedSuccess);
      }
   }
   else {
      printf("Failed to identify the type of file for path '%s', error: %s.  Assuming CLASSID::SCRIPT.\n", ScriptFile, GetErrorMsg(error));
      subclass = CLASSID::SCRIPT;
      class_id = CLASSID::SCRIPT;
   }

   if (!NewObject(subclass ? subclass : class_id, 0, &glScript)) {
      if (!TargetID) TargetID = CurrentTaskID();

      glScript->setFields(fl::Path(ScriptFile), fl::Target(TargetID), fl::Procedure(Procedure));

      if (glArgs) {
         argname = argbuffer+1; // Skip the first byte... reserved for '+'
         argbuffer[0] = '+'; // Append arg indicator
         for (i=0; glArgs[i]; i++) {
            for (j=0; (glArgs[i][j]) and (glArgs[i][j] != '=') and (j < (int)sizeof(argbuffer)-10); j++) argname[j] = glArgs[i][j];
            argname[j] = 0;

            if (glArgs[i][j] IS '=') {
               j++;
               if (glArgs[i][j] IS '{') {
                  // Array definition, e.g. files={ file1.txt file2.txt }

                  j++;
                  if (glArgs[i][j] > 0x20) {
                     SetKey(glScript, argbuffer, glArgs[i] + j);
                  }

                  i++;
                  while ((glArgs[i]) and (glArgs[i][0] != '}')) {
                     SetKey(glScript, argbuffer, glArgs[i]);
                     i++;
                  }
                  if (!glArgs[i]) break;
                  // The last arg in the array will be the } that closes it
               }
               else if (glArgs[i][j] IS '"') {
                  j++;
                  for (k=j; (glArgs[i][k]) and (glArgs[i][k] != '"'); k++);
                  if (glArgs[i][k] IS '"') glArgs[i][k] = 0;
                  SetKey(glScript, argname, glArgs[i]+j);
               }
               else SetKey(glScript, argname, glArgs[i]+j);
            }
            else SetKey(glScript, argname, "1");
         }
      }

      // Start the timer if requested

      int64_t start_time = 0;
      if (ShowTime) start_time = PreciseTime();

      if (!(error = InitObject(glScript))) {
         if (!(error = acActivate(glScript))) {
            if (ShowTime) {
               double startseconds = (double)start_time / 1000000.0;
               double endseconds   = (double)PreciseTime() / 1000000.0;
               printf("Script executed in %f seconds.\n\n", endseconds - startseconds);
            }
         }
         else {
            printf("Script failed during processing.  Use the --debug option to examine the failure.\n");
            return(ERR_Failed);
         }
      }
      else {
         printf("Failed to load / initialise the script.\n");
         return(ERR_Failed);
      }

      log.msg("Script initialised.");
   }
   else {
      printf("Internal Failure: Failed to create a new Script object for file processing.\n");
      return(ERR_Failed);
   }

   return(ERR_Okay);
}

//********************************************************************************************************************

static ERROR decompress_archive(CSTRING Location)
{
   if (!Location) return(ERR_NullArgs);

   int len, i, j;
   for (len=0; Location[len]; len++);

   objCompression::create compress = { fl::Path(Location) };
   if (compress.ok()) {
      if (!(error = AllocMemory(sizeof(STR_UNPACK) + len + sizeof(STR_MAIN) + 2, MEM::STRING, &glDirectory))) {
         for (i=0; STR_UNPACK[i]; i++) glDirectory[i] = STR_UNPACK[i];
         for (j=len; (j > 1) and (Location[j-1] != '/') and (Location[j-1] != '\\') and (Location[j-1] != ':'); j--);
         while (Location[j]) {
            glDirectory[i++] = Location[j];
            j++;
         }
         glDirectory[i++] = '/';
         glDirectory[i] = 0;

         if (!(error = cmpDecompressFile(compress, "*", glDirectory, 0))) {
            for (j=0; STR_MAIN[j]; j++) glDirectory[i++] = STR_MAIN[j];
            glDirectory[i] = 0;

            return ERR_Okay;
         }
         else printf("Failed to decompress the file contents.\n");
      }
      else printf("Failed to allocate a memory block.\n");
   }
   else printf("Failed to open the compressed file.\n");

   return ERR_Failed;
}
