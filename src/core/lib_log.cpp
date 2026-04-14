/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: System
-END-

This file contains all logging functions.

Log levels are:

0  CRITICAL Display the message irrespective of the log level.
1  ERROR Major errors that should be displayed to the user.
2  WARN Any error suitable for display to a developer or technically minded user.
3  Application log message, level 1
4  INFO Application log message, level 2
5  API Top-level API messages, e.g. function entry points (default)
6  DETAIL Detailed API messages.  For messages within functions, and entry-points for minor functions.
8  TRACE Extremely detailed API messages suitable for intensive debugging only.
9  Noisy debug messages that will appear frequently, e.g. being used in inner loops.

*********************************************************************************************************************/

#include <stdio.h>

#ifdef _MSC_VER

#else
 #include <unistd.h>
#endif

#include <stdarg.h>
#include <fcntl.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "defs.h"

static void fmsg(CSTRING, STRING, int8_t, int8_t);

#ifdef __ANDROID__
static const int COLUMN1 = 40;
#else
static const int COLUMN1 = 30;
#endif

//#if !defined(_WIN32)
   #define ESC_OUTPUT 1
//#endif

enum { MS_NONE, MS_FUNCTION, MS_MSG };
enum { EL_NONE=0, EL_MINOR, EL_MAJOR, EL_MAJORBOLD };

static thread_local int tlBaseLine = 0;

/*********************************************************************************************************************

-FUNCTION-
AdjustLogLevel: Adjusts the base-line of all log messages.

This function adjusts the detail level of all outgoing log messages.  To illustrate, setting the `Delta`
value to 1 would result in level 5 (API) log messages being bumped to level 6.  If the user's maximum log level
output is 5, no further API messages will be output until the base-line is reduced to normal.

The main purpose of AdjustLogLevel() is to reduce log noise.  For instance, creating a new desktop window will result
in a large number of new log messages.  Raising the base-line by 2 before creating the window would eliminate the
noise if the user has the log level set to 5 (API).  Re-running the program with a log level of 7 or more would
make the messages visible again.

Adjustments to the base-line are accumulative, so small increments of 1 or 2 are encouraged.  To revert logging to the
previous base-line, call this function again with a negation of the previously passed value.

-INPUT-
int Delta: The level of adjustment to make to new log messages.  Zero is no change.  The maximum level is +/- 6.

-RESULT-
int: Returns the absolute base-line value that was active prior to calling this function.

*********************************************************************************************************************/

int AdjustLogLevel(int Delta)
{
   if (glLogLevel >= 9) return tlBaseLine; // Do nothing if trace logging is active.
   int old_level = tlBaseLine;
   if ((Delta >= -6) and (Delta <= 6)) tlBaseLine += Delta;
   return old_level;
}

/*********************************************************************************************************************

-FUNCTION-
VLogF: Sends formatted messages to the standard log.
ExtPrototype: VLF Flags, const char *Header, const char *Message, va_list Args
Status: Internal

This function manages the output of application log messages by sending them through a log filter, which must be
enabled by the user.  If no logging is enabled or if the filter is not passed, the function does nothing.

Log message formatting follows the same guidelines as the `printf()` function.

The following example will print the default width of a @Display object to the log.

<pre>
if (!NewObject(CLASSID::DISPLAY, &display)) {
   if (!display->init(display)) {
      VLogF(VLF::API, "Demo","The width of the display is: %d", display-&gt;Width);
   }
   FreeResource(display);
}
</pre>

-INPUT-
int(VLF) Flags: Optional flags
cstr Header: A short name for the first column.  Typically function names are placed here, so that the origin of the message is obvious.
cstr Message: A formatted message to print.
va_list Args: A `va_list` corresponding to the arguments referenced in `Message`.
-END-

*********************************************************************************************************************/

void VLogF(VLF Flags, CSTRING Header, CSTRING Message, va_list Args)
{
   if (tlLogStatus <= 0) { if ((Flags & VLF::BRANCH) != VLF::NIL) tlDepth++; return; }

   static const VLF log_levels[10] = {
      VLF::CRITICAL,
      VLF::ERROR|VLF::CRITICAL,
      VLF::WARNING|VLF::ERROR|VLF::CRITICAL,
      VLF::INFO|VLF::WARNING|VLF::ERROR|VLF::CRITICAL,
      VLF::INFO|VLF::WARNING|VLF::ERROR|VLF::CRITICAL,
      VLF::API|VLF::INFO|VLF::WARNING|VLF::ERROR|VLF::CRITICAL,
      VLF::DETAIL|VLF::API|VLF::INFO|VLF::WARNING|VLF::ERROR|VLF::CRITICAL,
      VLF::DETAIL|VLF::API|VLF::INFO|VLF::WARNING|VLF::ERROR|VLF::CRITICAL,
      VLF::TRACE|VLF::DETAIL|VLF::API|VLF::INFO|VLF::WARNING|VLF::ERROR|VLF::CRITICAL,
      VLF::TRACE|VLF::DETAIL|VLF::API|VLF::INFO|VLF::WARNING|VLF::ERROR|VLF::CRITICAL
   };

   if ((Flags & VLF::CRITICAL) != VLF::NIL) { // Print the message irrespective of the log level
      #ifdef __ANDROID__
         __android_log_vprint(ANDROID_LOG_ERROR, Header ? Header : "Kotuku", Message, Args);
      #else
         #ifdef ESC_OUTPUT
            if (!Header) Header = "";
            #ifdef _WIN32
               fprintf(stderr, "!%s ", Header);
            #else
               fprintf(stderr, "\033[1m%s ", Header);
            #endif
         #else
            if (Header) fprintf(stderr, "%s ", Header);
         #endif

         vfprintf(stderr, Message, Args);

         #ifdef ESC_OUTPUT
            #ifndef _WIN32
               fprintf(stderr, "\033[0m");
            #endif
         #endif

         fprintf(stderr, "\n");
      #endif

      if ((Flags & VLF::BRANCH) != VLF::NIL) tlDepth++;
      return;
   }

   int level = glLogLevel - tlBaseLine;
   if (level > 9) level = 9;
   else if (level < 0) level = 0;

   if (((log_levels[level] & Flags) != VLF::NIL) or
       ((glLogLevel > 1) and ((Flags & (VLF::WARNING|VLF::ERROR|VLF::CRITICAL)) != VLF::NIL)))  {
      CSTRING name, action;
      int8_t msgstate;
      int8_t adjust = 0;

      std::lock_guard lock(glmPrint);

      if ((Header) and (!*Header)) Header = nullptr;

      if ((Flags & (VLF::BRANCH|VLF::FUNCTION)) != VLF::NIL) msgstate = MS_FUNCTION;
      else msgstate = MS_MSG;

      if (glLogThreads) fprintf(stderr, "%.4d ", int(get_thread_id()));

      #if defined(__unix__) and !defined(__ANDROID__)
         bool flushdbg;
         if (glLogLevel >= 3) {
            flushdbg = true;
            if (tlPublicLockCount) flushdbg = false;
            if (flushdbg) fcntl(STDERR_FILENO, F_SETFL, glStdErrFlags & (~O_NONBLOCK));
         }
         else flushdbg = false;
      #endif

      #ifdef ESC_OUTPUT // Highlight errors if the log output is crowded
         if ((glLogLevel > 2) and ((Flags & (VLF::ERROR|VLF::WARNING)) != VLF::NIL)) {
            #ifdef _WIN32
               fprintf(stderr, "!");
               adjust = 1;
            #else
               fprintf(stderr, "\033[1m");
            #endif
         }
      #endif

      // If no header is provided, make one to match the current context

      auto &ctx = tlContext.back();
      auto obj = ctx.obj;
      if (ctx.action > AC::NIL) action = ActionTable[int(ctx.action)].Name;
      else if (ctx.action < AC::NIL) {
         if (obj->Class) action = ((extMetaClass *)obj->Class)->Methods[-int(ctx.action)].Name;
         else action = "Method";
      }
      else action = "App";

      if (!Header) {
         Header = action;
         action = nullptr;
      }

      #ifdef __ANDROID__
         char msgheader[COLUMN1+1];

         fmsg(Header, msgheader, msgstate, 0);

         if (obj->Class) {
            char msg[180];

            if (obj->Name[0]) name = obj->Name;
            else name = obj->Class->Name;

            if (glLogLevel > 5) {
               if (ctx->Field) snprintf(msg, sizeof(msg), "[%s%s%s:%d:%s] %s", (action) ? action : (STRING)"", (action) ? ":" : "", name, obj->UID, ctx->Field->Name, Message);
               else snprintf(msg, sizeof(msg), "[%s%s%s:%d] %s", (action) ? action : (STRING)"", (action) ? ":" : "", name, obj->UID, Message);
            }
            else {
               if (ctx->Field) snprintf(msg, sizeof(msg), "[%s:%d:%s] %s", name, obj->UID, ctx->Field->Name, Message);
               else snprintf(msg, sizeof(msg), "[%s:%d] %s", name, obj->UID, Message);
            }

            __android_log_vprint((level <= 2) ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, msgheader, msg, Args);
         }
         else {
            __android_log_vprint((level <= 2) ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, msgheader, Message, Args);
         }

      #else
         char msgheader[COLUMN1+1];
         if (glLogLevel > 2) {
            fmsg(Header, msgheader, msgstate, adjust); // Print header with indenting
         }
         else {
            size_t len;
            for (len=0; (Header[len]) and (len < sizeof(msgheader)-2); len++) msgheader[len] = Header[len];
            msgheader[len++] = ' ';
            msgheader[len] = 0;
         }

         if (obj->Class) {
            name = obj->Name[0] ? obj->Name : obj->Class->ClassName;

            if (glLogLevel > 5) {
               if (ctx.field) {
                  fprintf(stderr, "%s[%s%s%s:%d:%s] ", msgheader, (action) ? action : (STRING)"", (action) ? ":" : "", name, obj->UID, ctx.field->Name);
               }
               else fprintf(stderr, "%s[%s%s%s:%d] ", msgheader, (action) ? action : (STRING)"", (action) ? ":" : "", name, obj->UID);
            }
            else if (ctx.field) {
               fprintf(stderr, "%s[%s:%d:%s] ", msgheader, name, obj->UID, ctx.field->Name);
            }
            else fprintf(stderr, "%s[%s:%d] ", msgheader, name, obj->UID);
         }
         else fprintf(stderr, "%s", msgheader);

         vfprintf(stderr, Message, Args);

         #if defined(ESC_OUTPUT) and !defined(_WIN32)
            if ((glLogLevel > 2) and ((Flags & (VLF::ERROR|VLF::WARNING)) != VLF::NIL)) fprintf(stderr, "\033[0m");
         #endif

         fprintf(stderr, "\n");

         #if defined(__unix__) and !defined(__ANDROID__)
            if (flushdbg) {
               fflush(0); // A fflush() appears to be enough - using fsync() will synchronise to disk, which we don't want by default (slow)
               if (glSync) fsync(STDERR_FILENO);
               fcntl(STDERR_FILENO, F_SETFL, glStdErrFlags);
            }
         #endif
      #endif
   }

   if ((Flags & VLF::BRANCH) != VLF::NIL) tlDepth++;
}

/*********************************************************************************************************************

-FUNCTION-
FuncError: Sends basic error messages to the application log.
Status: Internal

This function outputs a message to the application log.  It uses the codes listed in the system/errors.h file to
display the correct string to the user.  The following example `FuncError(ERR::Write)` would produce input such
as the following: `WriteFile: Error writing data to file.`.

Notice that the Header parameter is not provided in the example.  It is not necessary to supply this parameter in
C/C++ as the function name is automatically entered by the C pre-processor.

-INPUT-
cstr Header: A short string that names the function that is making the call.
error Error: An error code from the `system/errors.h` include file.  Valid error codes and their descriptions can be found in the Kotuku Wiki.

-RESULT-
error: Returns the same code that was specified in the `Error` parameter.

*********************************************************************************************************************/

ERR FuncError(CSTRING Header, ERR Code)
{
   if (tlLogStatus <= 0) return Code;
   if (glLogLevel < 2) return Code;
   if ((tlDepth >= glMaxDepth) or (tlLogStatus <= 0)) return Code;

   auto &ctx = tlContext.back();
   auto obj = ctx.obj;
   if (!Header) {
      if (ctx.action > AC::NIL) Header = ActionTable[int(ctx.action)].Name;
      else if (ctx.action < AC::NIL) {
         if (obj->Class) Header = ((extMetaClass *)obj->Class)->Methods[-int(ctx.action)].Name;
         else Header = "Method";
      }
      else Header = "Function";
   }

   #ifdef __ANDROID__
      if (obj->Class) {
         STRING name = obj->Name[0] ? obj->Name : obj->Class->Name;

         if (ctx->Field) {
             __android_log_print(ANDROID_LOG_ERROR, Header, "[%s:%d:%s] %s", name, obj->UID, ctx->Field->Name, glMessages[Code]);
         }
         else __android_log_print(ANDROID_LOG_ERROR, Header, "[%s:%d] %s", name, obj->UID, glMessages[Code]);
      }
      else __android_log_print(ANDROID_LOG_ERROR, Header, "%s", glMessages[Code]);
   #else
      char msgheader[COLUMN1+1];
      CSTRING histart = "", hiend = "";

      #ifdef ESC_OUTPUT
         if (glLogLevel > 2) {
            #ifdef _WIN32
               histart = "!";
            #else
               histart = "\033[1m";
               hiend = "\033[0m";
            #endif
         }
      #endif

      fmsg(Header, msgheader, MS_MSG, 2);

      if (obj->Class) {
         CSTRING name = obj->Name[0] ? obj->Name : obj->Class->ClassName;

         if (ctx.field) {
            fprintf(stderr, "%s%s[%s:%d:%s] %s%s\n", histart, msgheader, name, obj->UID, ctx.field->Name, glMessages[int(Code)], hiend);
         }
         else fprintf(stderr, "%s%s[%s:%d] %s%s\n", histart, msgheader, name, obj->UID, glMessages[int(Code)], hiend);
      }
      else fprintf(stderr, "%s%s%s%s\n", histart, msgheader, glMessages[int(Code)], hiend);

      #if defined(__unix__) && !defined(__ANDROID__)
         if (glSync) { fflush(0); fsync(STDERR_FILENO); }
      #endif
   #endif

   return Code;
}

/*********************************************************************************************************************

-FUNCTION-
LogReturn: Revert to the previous branch in the application logging tree.
Status: Internal

Use LogReturn() to reverse any previous log message that created an indented branch.  This function is considered
internal, and clients must use the scope-managed `pf::Log` class for branched log output.

-END-

*********************************************************************************************************************/

void LogReturn(void)
{
   if (tlLogStatus <= 0) return;
   if ((--tlDepth) < 0) tlDepth = 0;
}

//********************************************************************************************************************

static void fmsg(CSTRING Header, STRING Buffer, int8_t Colon, int8_t Sub) // Buffer must be COLUMN1+1 in size
{
   if (!Header) Header = "";

   int16_t pos = 0;
   int16_t depth;
   int16_t col = COLUMN1;

   if (glLogLevel < 3) depth = 0;
   else if (tlDepth > col) depth = col;
   else {
      depth = tlDepth;
      #ifdef _WIN32
         if (Sub and (depth > 0)) {
            col--;
            depth--; // Make a correction to the depth level if an error mark is printed.
         }
      #endif
   }

   if (glTimeLog) {
      double time = PreciseTime() - glTimeLog;
      pos += snprintf(Buffer+pos, col, "%09.5f ", time/1000000.0);
   }

   if (glLogLevel >= 3) {
      while ((depth > 0) and (pos < col)) {
         #ifdef __ANDROID__
            Buffer[pos++] = '_';
         #else
            Buffer[pos++] = ' ';
         #endif
         depth--;
      }

      while ((depth < 0) and (pos < col)) { // Print depth warnings if the counter is negative.
         Buffer[pos++] = '-';
         depth++;
      }
   }

   if (pos < col) { // Print as many function letters as possible.
      int16_t len;
      for (len=0; (Header[len]) and (pos < col); len++) Buffer[pos++] = Header[len];
      if ((!Colon) and (Header[len-1] != ':') and (Header[len-1] != ')')) Colon = MS_MSG;
      if (Colon IS MS_MSG) {
         if ((Header[len-1] != ':') and (Header[len-1] != ')') and (pos < col)) Buffer[pos++] = ':';
      }
      else if (Colon IS MS_FUNCTION) {
         if ((Header[len-1] != ':') and (Header[len-1] != ')') and (pos < col-1)) {
            Buffer[pos++] = '(';
            Buffer[pos++] = ')';
         }
      }
      if (glLogLevel >= 3) while (pos < col) Buffer[pos++] = ' '; // Add any extra spaces
   }

   Buffer[pos] = 0; // NB: Buffer is col + 1, so there is always room for the null byte.
}
