/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: System
-END-

This file contains all logging functions.

Log levels are:

0  CRITICAL Display the message irrespective of the log level.
1  ERROR    Major errors that should be displayed to the user.
2  WARN     Any error suitable for display to a developer or technically minded user.
3  APP      Application log message, level 1
4  INFO     Application log message, level 2
5  API      Top-level API messages, e.g. function entry points (default)
6  DETAIL   Detailed API messages.  For messages within functions, and entry-points for minor functions.
8  TRACE    Extremely detailed API messages suitable for intensive debugging only.
9  Noisy debug messages that will appear frequently, e.g. being used in inner loops.

*********************************************************************************************************************/

#include <stdio.h>

#ifdef _MSC_VER

#else
 #include <unistd.h>
#endif

#include <stdarg.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "defs.h"

static void fmsg(CSTRING, STRING, int8_t, int8_t);
static void dispatch_log_callbacks(VLF, CSTRING, CSTRING, va_list, uint8_t);
static void dispatch_log_message_callbacks(CSTRING, CSTRING, uint8_t, uint8_t);
static void invoke_log_callbacks(LogCallback **, int, CSTRING, CSTRING, uint8_t, uint8_t);
static CSTRING log_callback_header(CSTRING);
static int collect_log_callbacks(uint8_t, LogCallback **);
static uint8_t callback_depth(void);
static uint8_t effective_log_level(void);
static uint8_t message_log_level(VLF);

#ifdef __ANDROID__
static const int COLUMN1 = 40;
#else
static const int COLUMN1 = 30;
#endif

#define ESC_OUTPUT 1

enum { MS_FUNCTION = 1, MS_MSG = 2 };

static thread_local int tlBaseLine = 0;
static thread_local bool tlLogCallbackActive = false;
static thread_local std::vector<char> tlLogCallbackBuffer;

inline FILE * log_output(void)
{
   return glLogFile ? glLogFile : stderr;
}

inline bool log_output_is_file(void)
{
   return glLogFile;
}

inline bool log_callbacks_available(void)
{
   return glLogCallbackCount.load(std::memory_order_relaxed) > 0;
}

/*********************************************************************************************************************

-FUNCTION-
SetLogCallback: Register a callback for log messages.

This function registers `Callback` to observe log messages while logging is active.  The callback must use the
`LOG_CALLBACK` signature:

<pre>void Callback(CSTRING Header, CSTRING Message, INT Depth, INT MsgLevel, INT LogLevel)</pre>

The `Header` argument identifies the origin of the message.  When the caller did not supply a header, Core derives one
from the current action, method or application context.  The `Message` argument contains the formatted log text.  The
header and message pointers are valid only for the duration of the callback call, so a callback must copy either string
if it needs to retain it.  `Depth` is the current branch depth, `MsgLevel` is the severity or detail level assigned to
the message, and `LogLevel` is the effective active log level after baseline adjustment.

The `DepthLimit` and `LogLimit` parameters are used to reduce log noise.  `DepthLimit` suppresses callbacks when the
current branch depth is greater than the limit, and `LogLimit` suppresses callbacks when the message level is
greater than the limit.  Use a suitably large limit to receive all messages for that filter.  Messages emitted from
inside a log callback are not forwarded recursively to log callbacks on the same thread.

Passing `NULL` for `Callback` has no effect.  Registering the same callback address again updates its `DepthLimit` and
`LogLimit` values.  Passing zero for both limits unregisters a matching callback if it exists.  The number of unique
callbacks that can be registered is limited; when all callback slots are in use, additional registrations are ignored.

-INPUT-
ptr Callback: Pointer to the log callback function to register.
int DepthLimit: Maximum branch depth to forward to the callback.
int LogLimit: Maximum message log level to forward to the callback.

-TAGS-
callback-held, does-not-take-ownership, non-blocking

-END-

*********************************************************************************************************************/

void SetLogCallback(APTR Callback, int DepthLimit, int LogLimit)
{
   if (not Callback) return;

   auto routine = (LOG_CALLBACK)Callback;
   bool unregister = (not DepthLimit) and (not LogLimit);

   // Check if the callback is already registered.

   for (auto &slot : glLogCallbacks) {
      if (slot.State.load(std::memory_order_acquire) != LCS_ACTIVE) continue;

      auto &callback = slot.Callback;
      if (callback.Callback IS routine) {
         if (LogLimit or DepthLimit) {
            // These updates are not a threading concern
            callback.LogLimit = LogLimit;
            callback.DepthLimit = DepthLimit;
         }
         else {
            slot.State.store(uint8_t(LCS_EMPTY), std::memory_order_release);
            glLogCallbackCount.fetch_sub(1, std::memory_order_relaxed);
         }
         return;
      }
   }

   if (unregister) return;

   // Store the callback in the first available slot.

   for (auto &slot : glLogCallbacks) {
      auto state = uint8_t(LCS_EMPTY);

      if (slot.State.compare_exchange_strong(state, uint8_t(LCS_WRITING), std::memory_order_acq_rel,
            std::memory_order_acquire)) {
         slot.Callback.Callback = routine;
         slot.Callback.LogLimit = LogLimit;
         slot.Callback.DepthLimit = DepthLimit;
         slot.State.store(uint8_t(LCS_ACTIVE), std::memory_order_release);
         glLogCallbackCount.fetch_add(1, std::memory_order_relaxed);
         return;
      }
   }
}

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

A secondary use of this function is to increase the verbosity of log messages when debugging an area of interest.
For instance, if a program is run with a warning level of 2 and we want to see the log output for a function at API
level, call AdjustLogLevel() with a `Delta` of -3 to raise the base-line to 5.

Adjustments to the base-line are accumulative, so small increments of 1 or 2 are encouraged.  To revert logging to the
previous base-line, call this function again with a negation of the previously passed value.

Note: This function is complemented by the ~SetResource() function with the `RES::LOG_LEVEL` option, which
permanently changes the log level for the duration of the program.

-INPUT-
int Delta: The level of adjustment to make to new log messages.  Zero is no change.  The maximum level is +/- 9.

-RESULT-
int: Returns the absolute base-line value that was active prior to calling this function.

*********************************************************************************************************************/

int AdjustLogLevel(int Delta)
{
   if (glLogLevel >= 9) return tlBaseLine; // Do nothing if trace logging is active.
   int old_level = tlBaseLine;
   if ((Delta >= -9) and (Delta <= 9)) tlBaseLine += Delta;
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
if (NewObject(CLASSID::DISPLAY, &display) IS ERR::Okay) {
   if (display->init(display) IS ERR::Okay) {
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
   FILE *fd = log_output();
   const bool file_output = log_output_is_file();
   const uint8_t log_level = effective_log_level();

   if (log_callbacks_available()) dispatch_log_callbacks(Flags, Header, Message, Args, log_level);

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
            if (file_output) {
               if (Header) fprintf(fd, "%s ", Header);
            }
            else {
               if (!Header) Header = "";
               #ifdef _WIN32
                  fprintf(fd, "!%s ", Header);
               #else
                  fprintf(fd, "\033[1m%s ", Header);
               #endif
            }
         #else
            if (Header) fprintf(fd, "%s ", Header);
         #endif

         vfprintf(fd, Message, Args);

         #ifdef ESC_OUTPUT
            #ifndef _WIN32
               if (!file_output) fprintf(fd, "\033[0m");
            #endif
         #endif

         fprintf(fd, "\n");
      #endif

      if ((Flags & VLF::BRANCH) != VLF::NIL) tlDepth++;
      return;
   }

   int level = log_level;

   if (((log_levels[level] & Flags) != VLF::NIL) or
       ((level > 1) and ((Flags & (VLF::WARNING|VLF::ERROR|VLF::CRITICAL)) != VLF::NIL)))  {
      CSTRING name, action;
      int8_t msgstate;
      int8_t adjust = 0;

      std::lock_guard lock(glmPrint);

      if ((Header) and (!*Header)) Header = nullptr;

      if ((Flags & (VLF::BRANCH|VLF::FUNCTION)) != VLF::NIL) msgstate = MS_FUNCTION;
      else msgstate = MS_MSG;

      if (glLogThreads) fprintf(fd, "%.4d ", int(get_thread_id()));

      #if defined(__unix__) and !defined(__ANDROID__)
         bool flushdbg;
         if ((fd IS stderr) and (level >= 3)) {
            flushdbg = true;
            if (flushdbg) fcntl(STDERR_FILENO, F_SETFL, glStdErrFlags & (~O_NONBLOCK));
         }
         else flushdbg = false;
      #endif

      #ifdef ESC_OUTPUT // Highlight errors if the log output is crowded
         if ((not file_output) and (level > 2) and ((Flags & (VLF::ERROR|VLF::WARNING)) != VLF::NIL)) {
            #ifdef _WIN32
               fprintf(fd, "!");
               adjust = 1;
            #else
               fprintf(fd, "\033[1m");
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

            if (level > 5) {
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
         if (level > 2) {
            fmsg(Header, msgheader, msgstate, adjust); // Print header with indenting
         }
         else {
            size_t len;
            for (len=0; (Header[len]) and (len < sizeof(msgheader)-2); len++) msgheader[len] = Header[len];
            msgheader[len++] = ' ';
            msgheader[len] = 0;
         }

         if (obj->Class) {
            name = obj->Name[0] ? obj->Name : obj->Class->ClassName.c_str();

            if (level > 5) {
               if (ctx.field) {
                  fprintf(fd, "%s[%s%s%s:%d:%s] ", msgheader, (action) ? action : (STRING)"", (action) ? ":" : "", name, obj->UID, ctx.field->Name);
               }
               else fprintf(fd, "%s[%s%s%s:%d] ", msgheader, (action) ? action : (STRING)"", (action) ? ":" : "", name, obj->UID);
            }
            else if (ctx.field) {
               fprintf(fd, "%s[%s:%d:%s] ", msgheader, name, obj->UID, ctx.field->Name);
            }
            else fprintf(fd, "%s[%s:%d] ", msgheader, name, obj->UID);
         }
         else fprintf(fd, "%s", msgheader);

         vfprintf(fd, Message, Args);

         #if defined(ESC_OUTPUT) and !defined(_WIN32)
            if ((not file_output) and (level > 2) and ((Flags & (VLF::ERROR|VLF::WARNING)) != VLF::NIL)) fprintf(fd, "\033[0m");
         #endif

         fprintf(fd, "\n");

         #if defined(__unix__) and !defined(__ANDROID__)
            if (flushdbg) {
               fflush(nullptr); // A fflush() appears to be enough - using fsync() will synchronise to disk, which we don't want by default (slow)
               if (glSync) fsync(STDERR_FILENO);
               fcntl(STDERR_FILENO, F_SETFL, glStdErrFlags);
            }
            else if (glSync) {
               fflush(fd);
               if (fd IS stderr) fsync(STDERR_FILENO);
            }
         #else
            if (glSync) fflush(fd);
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
   int level = effective_log_level();
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

   if (log_callbacks_available()) dispatch_log_message_callbacks(Header, glMessages[int(Code)], 2, uint8_t(level));

   if (level < 2) return Code;

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
      FILE *fd = log_output();
      const bool file_output = log_output_is_file();
      char msgheader[COLUMN1+1];
      CSTRING histart = "", hiend = "";

      #ifdef ESC_OUTPUT
         if ((not file_output) and (level > 2)) {
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
         CSTRING name = obj->Name[0] ? obj->Name : obj->Class->ClassName.c_str();

         if (ctx.field) {
            fprintf(fd, "%s%s[%s:%d:%s] %s%s\n", histart, msgheader, name, obj->UID, ctx.field->Name, glMessages[int(Code)], hiend);
         }
         else fprintf(fd, "%s%s[%s:%d] %s%s\n", histart, msgheader, name, obj->UID, glMessages[int(Code)], hiend);
      }
      else fprintf(fd, "%s%s%s%s\n", histart, msgheader, glMessages[int(Code)], hiend);

      #if defined(__unix__) && !defined(__ANDROID__)
         if (glSync) {
            fflush(fd);
            if (fd IS stderr) fsync(STDERR_FILENO);
         }
      #else
         if (glSync) fflush(fd);
      #endif
   #endif

   return Code;
}

/*********************************************************************************************************************

-FUNCTION-
LogReturn: Revert to the previous branch in the application logging tree.
Status: Internal

Use LogReturn() to reverse any previous log message that created an indented branch.  This function is considered
internal, and clients must use the scope-managed `kt::Log` class for branched log output.

-END-

*********************************************************************************************************************/

void LogReturn(void)
{
   if (tlLogStatus <= 0) return;
   if ((--tlDepth) < 0) tlDepth = 0;
}

//********************************************************************************************************************
// Returns the active log level after applying this thread's temporary baseline adjustment.

static uint8_t effective_log_level(void)
{
   int level = glLogLevel - tlBaseLine;
   if (level > 9) return 9;
   else if (level < 0) return 0;
   else return uint8_t(level);
}

//********************************************************************************************************************
// Converts VLF flags to the numeric log level used by callback filters.

static uint8_t message_log_level(VLF Flags)
{
   if ((Flags & VLF::CRITICAL) != VLF::NIL) return 0;
   if ((Flags & VLF::ERROR) != VLF::NIL) return 1;
   if ((Flags & VLF::WARNING) != VLF::NIL) return 2;
   if ((Flags & VLF::TRACE) != VLF::NIL) return 8;
   if ((Flags & VLF::DETAIL) != VLF::NIL) return 6;
   if ((Flags & VLF::API) != VLF::NIL) return 5;
   if ((Flags & VLF::INFO) != VLF::NIL) return 3;

   return 4;
}

//********************************************************************************************************************
// Returns the current branch depth in the byte-sized range passed to log callbacks.

static uint8_t callback_depth(void)
{
   if (tlDepth <= 0) return 0;
   else if (tlDepth > 255) return 255;
   else return uint8_t(tlDepth);
}

//********************************************************************************************************************
// Resolves the header string that will be passed to callbacks when the caller did not provide one.

static CSTRING log_callback_header(CSTRING Header)
{
   if ((Header) and (*Header)) return Header;

   auto &ctx = tlContext.back();
   auto obj = ctx.obj;

   if (ctx.action > AC::NIL) return ActionTable[int(ctx.action)].Name;
   else if (ctx.action < AC::NIL) {
      if (obj->Class) return ((extMetaClass *)obj->Class)->Methods[-int(ctx.action)].Name;
      else return "Method";
   }
   else return "App";
}

//********************************************************************************************************************
// Gathers registered callbacks that should receive a message at the supplied level.

static int collect_log_callbacks(uint8_t MsgLevel, LogCallback **Callbacks)
{
   int total = 0;

   if (tlLogCallbackActive) return total;

   for (auto &slot : glLogCallbacks) {
      if (slot.State.load(std::memory_order_acquire) != LCS_ACTIVE) continue;

      auto &callback = slot.Callback;
      if (not callback.Callback) continue;
      if (tlDepth > callback.DepthLimit) continue;
      if (MsgLevel > callback.LogLimit) continue;

      Callbacks[total++] = &callback;
   }

   return total;
}

//********************************************************************************************************************
// Invokes a prepared callback list while suppressing recursive callback dispatch on this thread.

static void invoke_log_callbacks(LogCallback **Callbacks, int Total, CSTRING Header, CSTRING Message, uint8_t MsgLevel,
   uint8_t LogLevel)
{
   tlLogCallbackActive = true;
   uint8_t depth = callback_depth();
   for (int i=0; i < Total; i++) {
      Callbacks[i]->Callback(Header, Message, depth, MsgLevel, LogLevel);
   }
   tlLogCallbackActive = false;
}

//********************************************************************************************************************
// Dispatches a preformatted message, such as an error string, to interested log callbacks.

static void dispatch_log_message_callbacks(CSTRING Header, CSTRING Message, uint8_t MsgLevel, uint8_t LogLevel)
{
   LogCallback *callbacks[LC_LIMIT];
   int total = collect_log_callbacks(MsgLevel, callbacks);

   if (not total) return;

   invoke_log_callbacks(callbacks, total, Header, Message, MsgLevel, LogLevel);
}

//********************************************************************************************************************
// Formats a variadic log message and dispatches it to interested log callbacks.

static void dispatch_log_callbacks(VLF Flags, CSTRING Header, CSTRING Message, va_list Args, uint8_t LogLevel)
{
   uint8_t msg_level = message_log_level(Flags);
   LogCallback *callbacks[LC_LIMIT];
   int total = collect_log_callbacks(msg_level, callbacks);

   if (not total) return;

   Header = log_callback_header(Header);

   char buffer[256];
   va_list copy;
   va_copy(copy, Args);
   int len = vsnprintf(buffer, sizeof(buffer), Message ? Message : "", copy);
   va_end(copy);

   if (len < 0) return;

   if (size_t(len) < sizeof(buffer)) {
      invoke_log_callbacks(callbacks, total, Header, buffer, msg_level, LogLevel);
   }
   else {
      if (tlLogCallbackBuffer.size() < size_t(len) + 1) tlLogCallbackBuffer.resize(size_t(len) + 1);

      va_copy(copy, Args);
      vsnprintf(tlLogCallbackBuffer.data(), tlLogCallbackBuffer.size(), Message ? Message : "", copy);
      va_end(copy);

      invoke_log_callbacks(callbacks, total, Header, tlLogCallbackBuffer.data(), msg_level, LogLevel);
   }
}

//********************************************************************************************************************
// Formats the fixed-width log prefix used before printed log messages.

static void fmsg(CSTRING Header, STRING Buffer, int8_t Colon, int8_t Sub) // Buffer must be COLUMN1+1 in size
{
   if (!Header) Header = "";

   int16_t pos = 0;
   int16_t depth;
   int16_t col = COLUMN1;
   int level = glLogLevel - tlBaseLine;

   if (level < 3) depth = 0;
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

   if (level >= 3) {
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
      bool has_suffix = (len > 0) and ((Header[len - 1] IS ':') or (Header[len - 1] IS ')'));
      if (Colon IS MS_MSG) {
         if ((not has_suffix) and (pos < col)) Buffer[pos++] = ':';
      }
      else if (Colon IS MS_FUNCTION) {
         if ((not has_suffix) and (pos < col - 1)) {
            Buffer[pos++] = '(';
            Buffer[pos++] = ')';
         }
      }
      if (level >= 3) while (pos < col) Buffer[pos++] = ' '; // Add any extra spaces
   }

   Buffer[pos] = 0; // NB: Buffer is col + 1, so there is always room for the null byte.
}
