/*********************************************************************************************************************

Functions that are internal to the Core.

*********************************************************************************************************************/

#ifdef __unix__
 #include <errno.h>
 #include <signal.h>
 #include <unistd.h>
 #include <sys/wait.h>
#endif

#include "defs.h"

using namespace kt;

static constexpr int align_arg_offset(int Offset)
{
   return (Offset + 7) & ~7;
}

template <class T> static ERR copy_cpp_array_arg(APTR Source, APTR *Result)
{
   if (not Source) {
      *Result = nullptr;
      return ERR::Okay;
   }

   auto copy = new (std::nothrow) kt::vector<T>(*(kt::vector<T> *)Source);
   if (not copy) return ERR::AllocMemory;
   *Result = copy;
   return ERR::Okay;
}

static ERR copy_cpp_array_arg(int Type, APTR Source, APTR *Result)
{
   if (Type & FD_STR) return copy_cpp_array_arg<std::string>(Source, Result);
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) return copy_cpp_array_arg<OBJECTPTR>(Source, Result);
   else if (Type & FD_PTR) return copy_cpp_array_arg<APTR>(Source, Result);
   else if (Type & FD_DOUBLE) return copy_cpp_array_arg<double>(Source, Result);
   else if (Type & FD_FLOAT) return copy_cpp_array_arg<float>(Source, Result);
   else if (Type & FD_INT64) return copy_cpp_array_arg<int64_t>(Source, Result);
   else if (Type & FD_INT) return copy_cpp_array_arg<int>(Source, Result);
   else if (Type & FD_WORD) return copy_cpp_array_arg<int16_t>(Source, Result);
   else if (Type & FD_BYTE) return copy_cpp_array_arg<uint8_t>(Source, Result);
   else return ERR::NoSupport;
}

template <class T> static void delete_cpp_array_arg(APTR Array)
{
   delete (kt::vector<T> *)Array;
}

static void delete_cpp_array_arg(int Type, APTR Array)
{
   if (not Array) return;

   if (Type & FD_STR) delete_cpp_array_arg<std::string>(Array);
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) delete_cpp_array_arg<OBJECTPTR>(Array);
   else if (Type & FD_PTR) delete_cpp_array_arg<APTR>(Array);
   else if (Type & FD_DOUBLE) delete_cpp_array_arg<double>(Array);
   else if (Type & FD_FLOAT) delete_cpp_array_arg<float>(Array);
   else if (Type & FD_INT64) delete_cpp_array_arg<int64_t>(Array);
   else if (Type & FD_INT) delete_cpp_array_arg<int>(Array);
   else if (Type & FD_WORD) delete_cpp_array_arg<int16_t>(Array);
   else if (Type & FD_BYTE) delete_cpp_array_arg<uint8_t>(Array);
}

static void pin_object_array(kt::vector<OBJECTPTR> *Objects)
{
   if (not Objects) return;
   for (auto object : *Objects) {
      if (object) object->pin();
   }
}

static void unpin_object_array(kt::vector<OBJECTPTR> *Objects)
{
   if (not Objects) return;
   for (auto object : *Objects) {
      if (object) object->unpin(true);
   }
}

static int argument_end_offset(int Type, int Offset)
{
   if (Type & FD_ARRAY) return align_arg_offset(Offset) + sizeof(APTR);
   else if (Type & FD_STR) {
      Offset = align_arg_offset(Offset);
      if ((Type & FD_CPP) and (not (Type & FD_MUTABLE))) return Offset + sizeof(std::string_view);
      else return Offset + sizeof(APTR);
   }
   else if (Type & FD_FUNCTION) return align_arg_offset(Offset) + sizeof(FUNCTION);
   else if (Type & (FD_PTR|FD_BUFFER|FD_STRUCT)) return align_arg_offset(Offset) + sizeof(APTR);
   else if (Type & FD_DOUBLE) return align_arg_offset(Offset) + sizeof(double);
   else if (Type & FD_INT64) return align_arg_offset(Offset) + sizeof(int64_t);
   else if (Type & FD_INT) return Offset + sizeof(int);
   else return Offset;
}

//********************************************************************************************************************

#ifdef __APPLE__
struct sockaddr_un * get_socket_path(int ProcessID, socklen_t *Size)
{
   // OSX doesn't support anonymous sockets, so we use /tmp instead.
   static thread_local struct sockaddr_un tlSocket;
   tlSocket.sun_family = AF_UNIX;
   *Size = sizeof(sa_family_t) + snprintf(tlSocket.sun_path, sizeof(tlSocket.sun_path), "/tmp/kotuku.%d", ProcessID) + 1;
   return &tlSocket;
}
#elif __unix__
struct sockaddr_un * get_socket_path(int ProcessID, socklen_t *Size)
{
   static thread_local struct sockaddr_un tlSocket;
   static thread_local bool init = false;

   if (!init) {
      tlSocket.sun_family = AF_UNIX;
      clearmem(tlSocket.sun_path, sizeof(tlSocket.sun_path));
      tlSocket.sun_path[0] = '\0';
      tlSocket.sun_path[1] = 'p';
      tlSocket.sun_path[2] = 's';
      tlSocket.sun_path[3] = 'l';
      init = true;
   }

   ((int *)(tlSocket.sun_path+4))[0] = ProcessID;
   *Size = sizeof(sa_family_t) + 4 + sizeof(int);
   return &tlSocket;
}
#endif

//********************************************************************************************************************
// Fast lookup for matching file extensions with a valid class handler.

CLASSID lookup_class_by_ext(CLASSID Filter, std::string_view Ext)
{
   if (glWildClassMapTotal != std::ssize(glClassDB)) {
      // Build a lookup map based on file extensions
      for (auto it = glClassDB.begin(); it != glClassDB.end(); it++) {
         if (auto &rec = it->second; !rec.Extension.empty()) {
            std::vector<std::string> list;
            kt::split(rec.Extension, std::back_inserter(list), '|');
            for (auto &wild : list) {
               glWildClassMap.emplace(kt::strihash(wild), it->first);
            }
         }
      }

      glWildClassMapTotal = glClassDB.size();
   }

   auto hash = kt::strihash(Ext);

   if (Filter IS CLASSID::NIL) {
      if (auto search = glWildClassMap.find(hash); search != glWildClassMap.end()) {
         return search->second;
      }
   }
   else {
      auto range = glWildClassMap.equal_range(hash);
      for (auto it = range.first; it != range.second; ++it) {
         CLASSID class_id = it->second;
         if (auto rec = glClassDB.find(class_id); rec != glClassDB.end()) {
            if ((rec->second.ParentID IS Filter) or (rec->second.ClassID IS Filter)) return class_id;
         }
      }
   }

   return CLASSID::NIL;
}

//********************************************************************************************************************

static void reap_child_processes(bool CheckAllProcesses)
{
#ifdef __unix__
   kt::Log log("child_process");
   std::vector<int> finished_processes;

   // Call waitpid() to check for zombie processes first.  This covers all processes within our own context, so our child processes, children of those children etc.

   // However, it can be 'blocked' from certain processes, e.g. those started from ZTerm.  Such processes are discovered in the second search routine.

   int childprocess, status;
   while ((childprocess = waitpid(-1, &status, WNOHANG)) > 0) {
      log.trace("Process #%d exited.", childprocess);

      for (auto &task : glTasks) {
         if (childprocess IS task.ProcessID) {
            if (WIFEXITED(status)) task.ReturnCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status)) task.ReturnCode = 128 + WTERMSIG(status);
            task.Returned   = true;
            finished_processes.push_back(childprocess);
            break;
         }
      }
   }

   // Check all registered processes to see which ones are alive.  This routine can manage all processes, although exhibits
   // some problems with zombies, hence the earlier waitpid() routine to clean up such processes.

   if (CheckAllProcesses) {
      for (auto &task : glTasks) {
         if (task.Returned) continue;
         if ((kill(task.ProcessID, 0) IS -1) and (errno IS ESRCH)) {
            finished_processes.push_back(task.ProcessID);
         }
      }
   }

   for (auto process_id : finished_processes) validate_process(process_id);
#endif
}

//********************************************************************************************************************

void process_child_signals(HOSTHANDLE FD, APTR Data)
{
#ifdef __unix__
   uint8_t buffer[64];
   while (read(FD, &buffer, sizeof(buffer)) > 0);

   reap_child_processes(false);
#endif
}

//********************************************************************************************************************

ERR process_janitor(OBJECTID SubscriberID, int Elapsed, int TotalElapsed)
{
   if (glTasks.empty()) {
      glJanitorActive = false;
      return ERR::Terminate;
   }

#ifdef __unix__
   reap_child_processes(true);

#elif _WIN32
   for (auto &task : glTasks) {
      if (!winCheckProcessExists(task.ProcessID)) {
         validate_process(task.ProcessID);
      }
   }
#endif

   return ERR::Okay;
}

/*********************************************************************************************************************

copy_args: Serialise action/method argument structures into sendable messages.

This function searches an argument structure for pointer and string types.  If it encounters them, it attempts to
convert them to a format that can be passed to other memory spaces.  Note: The canonical interpreter for these
structures is in Tiri - for the most part this function should be kept in sync with it.

A PTR|RESULT or PTRBUFFER|MUTABLE followed by a PTRSIZE indicates that the user has to supply a buffer to the function.  It
is assumed that the function will fill the buffer with data, so the caller's initial buffer content is not serialised.
Example:

<pre>
Read(Bytes (FD_INT), Buffer (FD_PTRRESULT), BufferSize (FD_PTRSIZE), &BytesRead (FD_INTRESULT));
</pre>

A standard PTR followed by a PTRSIZE indicates that the user has to supply a buffer to the function.  It is assumed
that this is one-way traffic only, and the function will not fill the buffer with data if FD_MUTABLE is not set.
Example:

<pre>
Write(Bytes (FD_INT), Buffer (FD_PTRBUFFER), BufferSize (FD_PTRSIZE), &BytesWritten (FD_INTRESULT));
</pre>

If the function will return a memory block of its own, it must return the block as a MEMORYID, not a PTR.  The
allocation must be made using the object's MemFlags, as the action messaging functions will change between
public|untracked and private memory flags as necessary.  Example:

  Read(Bytes (FD_INT), &BufferMID (FD_INTRESULT), &BufferSize (FD_INTRESULT));

*********************************************************************************************************************/

ERR copy_args(const FunctionField *Args, int ArgsSize, int8_t *Parameters, std::vector<int8_t> &Buffer)
{
   kt::Log log(__FUNCTION__);

   if ((not Args) or (not Parameters)) return ERR::NullArgs;

   // Buffer size must be computed in advance

   int size = ArgsSize;
   int pos = 0;
   bool function_found = false;
   for (int i=0; Args[i].Name; i++) {
      if (pos >= ArgsSize) return ERR::InvalidData; // Sanity check, the pos can't exceed ArgsSize.

      int type = Args[i].Type;
      if ((type & FD_ARRAY) and (!(type & FD_CPP))) {
         // Array parameters are considered legacy and effectively unused in the current system.
         return ERR::NoSupport;
      }
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if (not (type & FD_RESULT)) {
            if ((type & FD_CPP) and (not (type & FD_MUTABLE))) {
               auto view = (std::string_view *)(Parameters + pos);
               if (view->length() >= size_t(INT_MAX - size)) return log.warning(ERR::InvalidData);
               size += int(view->length()) + 1;
            }
            else if (auto str = *(CSTRING *)(Parameters + pos)) {
               auto length = strlen(str);
               if (length >= size_t(INT_MAX - size)) return log.warning(ERR::InvalidData);
               size += int(length) + 1;
            }
         }
         pos = argument_end_offset(type, pos);
      }
      else if (type & FD_FUNCTION) {
         // There is a hard limit of one embedded function per action call.
         // Functions are always expressed as an embedded type.
         if (function_found) return log.warning(ERR::NoSupport);
         function_found = true;
         pos = argument_end_offset(type, pos);
      }
      else if (type & (FD_PTR|FD_BUFFER)) {
         pos = align_arg_offset(pos);
         if ((not (type & (FD_OBJECT|FD_RESULT))) and (Args[i+1].Type & FD_PTRSIZE)) {
            int64_t memsize;
            if (Args[i+1].Type & FD_INT64) memsize = *(int64_t *)(Parameters + pos + sizeof(APTR));
            else memsize = *(int *)(Parameters + pos + sizeof(APTR));
            if ((memsize < 0) or (memsize > INT_MAX - size)) return log.warning(ERR::InvalidData);
            size += int(memsize);
         }
         else if (not (type & (FD_OBJECT|FD_RESULT))) return log.warning(ERR::NoSupport);
         pos += sizeof(APTR);
      }
      else if (type & (FD_DOUBLE|FD_INT64|FD_INT)) pos = argument_end_offset(type, pos);
      else if (type & FD_TAGS) return log.warning(ERR::NoSupport);
      else return log.warning(ERR::NoSupport);
   }

   Buffer.reserve(size); // Ensures that the buffer space remains stable when extended.
   Buffer.resize(ArgsSize);
   copymem(Parameters, Buffer.data(), ArgsSize);

   // C++ arrays require an owned vector copy.  Complete these allocations before pinning any referenced objects or
   // callbacks so that allocation failure cannot leave partially acquired resources behind.

   std::vector<std::pair<int, APTR>> cpp_arrays;
   pos = 0;
   for (int i=0; Args[i].Name; i++) {
      int type = Args[i].Type;
      if (type & FD_ARRAY) {
         pos = align_arg_offset(pos);
         APTR copy = nullptr;
         if (auto error = copy_cpp_array_arg(type, *(APTR *)(Parameters + pos), &copy); error != ERR::Okay) {
            for (auto &array : cpp_arrays) delete_cpp_array_arg(array.first, array.second);
            Buffer.clear();
            return error;
         }
         *(APTR *)(Buffer.data() + pos) = copy;
         cpp_arrays.emplace_back(type, copy);
      }
      pos = argument_end_offset(type, pos);
   }

   pos = 0;
   for (int i=0; Args[i].Name; i++) {
      int type = Args[i].Type;

      if (type & FD_ARRAY) {
         pos = align_arg_offset(pos);
         auto param = (APTR *)(Buffer.data() + pos);
         if ((type & FD_OBJECT) and (type & FD_PTR)) {
            pin_object_array((kt::vector<OBJECTPTR> *)*param);
         }
         pos += sizeof(APTR);
      }
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         APTR param = Buffer.data() + pos;
         if (type & FD_RESULT) {
            // Result values are not available to queued callers, so do not retain references to caller-owned storage.
            if ((type & FD_CPP) and (not (type & FD_MUTABLE))) new (param) std::string_view;
            else *(APTR *)param = nullptr;
         }
         else if ((type & FD_CPP) and (not (type & FD_MUTABLE))) {
            auto insert = (STRING)(Buffer.data() + Buffer.size());
            auto view = (std::string_view *)(Parameters + pos);
            Buffer.resize(Buffer.size() + view->length() + 1);
            ((std::string_view *)param)[0] = std::string_view(insert, view->length());
            if (not view->empty()) copymem(view->data(), insert, view->length());
            insert[view->length()] = 0;
         }
         else {
            auto insert = (STRING)(Buffer.data() + Buffer.size());
            if (auto str = *(CSTRING *)(Parameters + pos)) {
               auto len = strlen(str);
               Buffer.resize(Buffer.size() + len + 1);
               ((STRING *)param)[0] = insert;
               copymem(str, insert, len + 1);
            }
            else ((STRING *)param)[0] = nullptr;
         }
         pos = argument_end_offset(type, pos);
      }
      else if (type & FD_FUNCTION) {
         pos = align_arg_offset(pos);
         auto function = (FUNCTION *)(Buffer.data() + pos);
         if (function->defined()) function->pin();
         pos += sizeof(FUNCTION);
      }
      else if (type & (FD_PTR|FD_BUFFER)) {
         pos = align_arg_offset(pos);
         auto param = (APTR *)(Buffer.data() + pos);
         if (type & FD_OBJECT) {
            if (type & FD_RESULT) *param = nullptr;
            else {
               auto object = *(OBJECTPTR *)(Parameters + pos);
               *param = object;
               if (object) object->pin();
            }
         }
         else if (type & FD_RESULT) *param = nullptr;
         else if (Args[i+1].Type & FD_PTRSIZE) {
            int64_t memsize;
            if (Args[i+1].Type & FD_INT64) memsize = *(int64_t *)(Parameters + pos + sizeof(APTR));
            else memsize = *(int *)(Parameters + pos + sizeof(APTR));
            if (memsize > 0) {
               if (type & FD_MUTABLE) { // Receive buffer
                  auto insert = Buffer.data() + Buffer.size();
                  *param = insert;
                  Buffer.resize(Buffer.size() + memsize);
               }
               else { // Send buffer
                  if (int8_t *src = *(int8_t **)(Parameters + pos)) {
                     auto insert = Buffer.data() + Buffer.size();
                     *param = insert;
                     Buffer.resize(Buffer.size() + memsize);
                     copymem(src, insert, memsize);
                  }
                  else *param = nullptr;
               }
            }
            else *param = nullptr;
         }
         pos += sizeof(APTR);
      }
      else pos = argument_end_offset(type, pos);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
A buffer produced by copy_args() embeds absolute pointers into its own storage for string and sized buffer arguments.
Those pointers survive moves of the owning std::vector, but not byte-for-byte duplication (e.g. serialisation into
the message queue).  make_args_relative() converts the self-referential pointers to offsets from the start of the
argument block so that make_args_absolute() can rebase them against the receiving copy.  Object, array and function
references are position independent and are left untouched, as are null pointers (offsets always exceed zero because
extension data follows the argument block).
*********************************************************************************************************************/

void make_args_relative(const FunctionField *Args, int ArgsSize, int8_t *Buffer)
{
   if ((not Args) or (not Buffer)) return;

   int pos = 0;
   for (int i=0; Args[i].Name and (pos < ArgsSize); i++) {
      int type = Args[i].Type;
      if (type & FD_ARRAY); // Owned heap copy; position independent
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if ((type & FD_CPP) and (not (type & FD_MUTABLE))) {
            auto view = (std::string_view *)(Buffer + pos);
            if (view->data()) *view = std::string_view((CSTRING)(view->data() - (CSTRING)Buffer), view->length());
         }
         else if (auto str = (int8_t **)(Buffer + pos); *str) {
            *str = (int8_t *)(*str - Buffer);
         }
      }
      else if (type & (FD_PTR|FD_BUFFER)) {
         pos = align_arg_offset(pos);
         if ((not (type & (FD_OBJECT|FD_RESULT))) and (Args[i+1].Type & FD_PTRSIZE)) {
            if (auto ptr = (int8_t **)(Buffer + pos); *ptr) *ptr = (int8_t *)(*ptr - Buffer);
         }
      }
      pos = argument_end_offset(type, pos);
   }
}

void make_args_absolute(const FunctionField *Args, int ArgsSize, int8_t *Buffer)
{
   if ((not Args) or (not Buffer)) return;

   int pos = 0;
   for (int i=0; Args[i].Name and (pos < ArgsSize); i++) {
      int type = Args[i].Type;
      if (type & FD_ARRAY); // Owned heap copy; position independent
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if ((type & FD_CPP) and (not (type & FD_MUTABLE))) {
            auto view = (std::string_view *)(Buffer + pos);
            if (view->data()) *view = std::string_view((CSTRING)Buffer + (MAXINT)view->data(), view->length());
         }
         else if (auto str = (int8_t **)(Buffer + pos); *str) {
            *str = Buffer + (MAXINT)*str;
         }
      }
      else if (type & (FD_PTR|FD_BUFFER)) {
         pos = align_arg_offset(pos);
         if ((not (type & (FD_OBJECT|FD_RESULT))) and (Args[i+1].Type & FD_PTRSIZE)) {
            if (auto ptr = (int8_t **)(Buffer + pos); *ptr) *ptr = Buffer + (MAXINT)*ptr;
         }
      }
      pos = argument_end_offset(type, pos);
   }
}

//********************************************************************************************************************

void release_copied_args(const FunctionField *Args, int ArgsSize, int8_t *Parameters, bool DereferenceAll,
   FUNCTION *DeferredFunction)
{
   if ((not Args) or (not Parameters)) return;

   for (int i=0, pos=0; Args[i].Name and (pos < ArgsSize); i++) {
      auto type = Args[i].Type;

      if (type & FD_ARRAY) {
         pos = align_arg_offset(pos);
         auto array = *(APTR *)(Parameters + pos);
         if ((type & FD_OBJECT) and (type & FD_PTR)) {
            unpin_object_array((kt::vector<OBJECTPTR> *)array);
         }
         delete_cpp_array_arg(type, array);
         *(APTR *)(Parameters + pos) = nullptr;
         pos += sizeof(APTR);
      }
      else if ((type & FD_BUFFER) or (Args[i+1].Type & FD_BUFSIZE)) {
         pos = align_arg_offset(pos) + sizeof(APTR);
      }
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if ((type & FD_CPP) and (not (type & FD_MUTABLE))) pos += sizeof(std::string_view);
         else pos += sizeof(APTR);
      }
      else if (type & FD_FUNCTION) {
         pos = align_arg_offset(pos);
         auto &function = *(FUNCTION *)(Parameters + pos);

         if (function.defined()) {
            if (function.isScript() and (not function.stale()) and (DereferenceAll or function.consumed())) {
               if (DeferredFunction) {
                  *DeferredFunction = function;
                  function.disable();
                  pos += sizeof(FUNCTION);
                  continue;
               }
               else {
                  auto script = function.Context;
                  sc::DerefProcedure deref = { function };
                  Action(sc::DerefProcedure::id, script, &deref);
               }
            }
            function.unpin();
            function.disable();
         }

         pos += sizeof(FUNCTION);
      }
      else if (type & FD_PTR) {
         pos = align_arg_offset(pos);
         if ((type & FD_OBJECT) and (not (type & FD_RESULT))) {
            if (auto object = *(OBJECTPTR *)(Parameters + pos)) object->unpin(true);
            *(OBJECTPTR *)(Parameters + pos) = nullptr;
         }
         pos += sizeof(APTR);
      }
      else if (type & (FD_INT|FD_DOUBLE|FD_INT64)) pos = argument_end_offset(type, pos);
      else if (type & FD_TAGS) break;
   }
}
