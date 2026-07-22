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

static bool align_arg_offset(size_t Offset, size_t Alignment, size_t *Result)
{
   if ((not Alignment) or (Alignment & (Alignment - 1))) return false;
   if (Offset > std::numeric_limits<size_t>::max() - (Alignment - 1)) return false;
   *Result = (Offset + Alignment - 1) & ~(Alignment - 1);
   return true;
}

static int align_arg_offset(int Offset)
{
   size_t result;
   if ((Offset < 0) or (not align_arg_offset(size_t(Offset), 8, &result)) or
       (result > size_t(std::numeric_limits<int>::max()))) return -1;
   return int(result);
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

static ERR span_descriptor_element_layout(const FunctionField &Field, size_t *ElementSize, size_t *ElementAlignment)
{
   if (Field.Type & FD_STRUCT) {
      if (not Field.Name) return ERR::InvalidData;
      const std::string_view name(Field.Name);
      const auto separator = name.find(':');
      if ((separator IS std::string_view::npos) or (separator IS 0)) return ERR::InvalidData;

      const auto it = glStructSizes.find(kt::strhash(name.substr(0, separator)));
      if (it IS glStructSizes.end()) return ERR::InvalidData;
      *ElementSize = it->second.Size;
      *ElementAlignment = it->second.Alignment;
   }
   else {
      const int type_count = bool(Field.Type & FD_DOUBLE) + bool(Field.Type & FD_INT64) +
         bool(Field.Type & FD_FLOAT) + bool(Field.Type & FD_INT) + bool(Field.Type & FD_WORD) +
         bool(Field.Type & FD_BYTE) + bool(Field.Type & FD_PTR) + bool(Field.Type & FD_STR);
      if (type_count != 1) return ERR::InvalidData;

      if (Field.Type & FD_DOUBLE) { *ElementSize = sizeof(double); *ElementAlignment = alignof(double); }
      else if (Field.Type & FD_INT64) { *ElementSize = sizeof(int64_t); *ElementAlignment = alignof(int64_t); }
      else if (Field.Type & FD_FLOAT) { *ElementSize = sizeof(float); *ElementAlignment = alignof(float); }
      else if (Field.Type & FD_INT) { *ElementSize = sizeof(int); *ElementAlignment = alignof(int); }
      else if (Field.Type & FD_WORD) { *ElementSize = sizeof(int16_t); *ElementAlignment = alignof(int16_t); }
      else { *ElementSize = sizeof(int8_t); *ElementAlignment = alignof(int8_t); }
   }

   if ((not *ElementSize) or (not *ElementAlignment) or
       (*ElementAlignment > alignof(std::max_align_t))) return ERR::InvalidData;
   return ERR::Okay;
}

using GenericSpan = std::span<int8_t>;

static ERR span_field_layout(const FunctionField &Field, size_t Offset, size_t *AlignedOffset, size_t *EndOffset)
{
   if ((Field.Type & FDF_SPAN) != FDF_SPAN) return ERR::InvalidData;

   size_t aligned_offset;
   if (not align_arg_offset(Offset, alignof(GenericSpan), &aligned_offset)) return ERR::InvalidData;
   if (aligned_offset > std::numeric_limits<size_t>::max() - sizeof(GenericSpan)) return ERR::InvalidData;

   if (AlignedOffset) *AlignedOffset = aligned_offset;
   if (EndOffset) *EndOffset = aligned_offset + sizeof(GenericSpan);
   return ERR::Okay;
}

static ERR span_field_value(const FunctionField &Field, CPTR Storage, GenericSpan *Value, size_t *ByteSize)
{
   if ((not Storage) or (not Value) or (not ByteSize)) return ERR::NullArgs;
   if (span_field_layout(Field, 0, nullptr, nullptr) != ERR::Okay) return ERR::InvalidData;

   size_t element_size, element_alignment;
   if (span_descriptor_element_layout(Field, &element_size, &element_alignment) != ERR::Okay) {
      return ERR::InvalidData;
   }

   const auto value = *(const GenericSpan *)Storage;
   if ((not value.data()) and value.size()) return ERR::InvalidData;
   if (value.size() > std::numeric_limits<size_t>::max() / element_size) return ERR::InvalidData;

   *Value = value;
   *ByteSize = value.size() * element_size;
   return ERR::Okay;
}

static void rebind_span_field(APTR Storage, CPTR Data, size_t Extent)
{
   *(GenericSpan *)Storage = GenericSpan((int8_t *)Data, Extent);
}

static int argument_end_offset(const FunctionField &Field, int Offset)
{
   if (Offset < 0) return -1;

   size_t end_offset;
   if ((Field.Type & FDF_SPAN) IS FDF_SPAN) {
      if (span_field_layout(Field, size_t(Offset), nullptr, &end_offset) != ERR::Okay) return -1;
   }
   else {
      size_t aligned_offset = size_t(Offset);
      size_t field_size;

      if (Field.Type & FD_ARRAY) field_size = sizeof(APTR);
      else if (Field.Type & FD_STR) {
         field_size = ((Field.Type & FD_CPP) and (not (Field.Type & FD_MUTABLE))) ?
            sizeof(std::string_view) : sizeof(APTR);
      }
      else if (Field.Type & FD_FUNCTION) field_size = sizeof(FUNCTION);
      else if (Field.Type & (FD_PTR|FD_STRUCT)) field_size = sizeof(APTR);
      else if (Field.Type & FD_DOUBLE) field_size = sizeof(double);
      else if (Field.Type & FD_INT64) field_size = sizeof(int64_t);
      else if (Field.Type & FD_INT) {
         field_size = sizeof(int);
         end_offset = size_t(Offset) + field_size;
         if (end_offset > size_t(std::numeric_limits<int>::max())) return -1;
         return int(end_offset);
      }
      else return -1;

      if (not align_arg_offset(size_t(Offset), 8, &aligned_offset)) return -1;
      if (aligned_offset > std::numeric_limits<size_t>::max() - field_size) return -1;
      end_offset = aligned_offset + field_size;
   }

   if (end_offset > size_t(std::numeric_limits<int>::max())) return -1;
   return int(end_offset);
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

If passing buffers, use FDF_SPAN.

*********************************************************************************************************************/

ERR copy_args(const FunctionField *Args, int ArgsSize, int8_t *Parameters, std::vector<int8_t> &Buffer)
{
   kt::Log log(__FUNCTION__);

   if ((not Args) or (not Parameters)) return ERR::NullArgs;
   if (ArgsSize <= 0) return ERR::InvalidData;
   Buffer.clear();

   const auto max_copy_size = std::min(Buffer.max_size(), size_t(INT_MAX - sizeof(ActionMessage)));
   if (size_t(ArgsSize) > max_copy_size) return ERR::InvalidData;

   // Buffer size must be computed in advance

   size_t size = ArgsSize;
   int pos = 0;
   bool function_found = false;
   for (int i=0; Args[i].Name; i++) {
      if (pos >= ArgsSize) return ERR::InvalidData; // Sanity check, the pos can't exceed ArgsSize.

      int type = Args[i].Type;
      if ((type & FD_ARRAY) and (!(type & FD_CPP))) {
         // Array parameters are considered legacy and effectively unused in the current system.
         return ERR::NoSupport;
      }
      else if ((type & FDF_SPAN) IS FDF_SPAN) {
         if (type & (FD_RESULT|FD_ALLOC)) return log.warning(ERR::InvalidData);

         size_t aligned_offset, end_offset;
         if ((span_field_layout(Args[i], size_t(pos), &aligned_offset, &end_offset) != ERR::Okay) or
             (end_offset > size_t(ArgsSize))) return log.warning(ERR::InvalidData);

         GenericSpan value;
         size_t byte_size;
         if (span_field_value(Args[i], Parameters + aligned_offset, &value, &byte_size) != ERR::Okay) {
            return log.warning(ERR::InvalidData);
         }
         if (byte_size) {
            size_t element_size, element_alignment, payload_offset;
            if (span_descriptor_element_layout(Args[i], &element_size, &element_alignment) != ERR::Okay) {
               return log.warning(ERR::InvalidData);
            }
            if ((not align_arg_offset(size, element_alignment, &payload_offset)) or
                (payload_offset > max_copy_size) or (byte_size > max_copy_size - payload_offset)) {
               return log.warning(ERR::InvalidData);
            }
            size = payload_offset + byte_size;
         }
         pos = int(end_offset);
      }
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return log.warning(ERR::InvalidData);
         if (not (type & FD_RESULT)) {
            if ((type & FD_CPP) and (not (type & FD_MUTABLE))) {
               auto view = (std::string_view *)(Parameters + pos);
               if ((size > max_copy_size) or (view->length() >= max_copy_size - size)) {
                  return log.warning(ERR::InvalidData);
               }
               size += view->length() + 1;
            }
            else if (auto str = *(CSTRING *)(Parameters + pos)) {
               auto length = strlen(str);
               if ((size > max_copy_size) or (length >= max_copy_size - size)) {
                  return log.warning(ERR::InvalidData);
               }
               size += length + 1;
            }
         }
         pos = argument_end_offset(Args[i], pos);
         if (pos < 0) return log.warning(ERR::InvalidData);
      }
      else if (type & FD_FUNCTION) {
         // There is a hard limit of one embedded function per action call.
         // Functions are always expressed as an embedded type.
         if (function_found) return log.warning(ERR::NoSupport);
         function_found = true;
         pos = argument_end_offset(Args[i], pos);
         if (pos < 0) return log.warning(ERR::InvalidData);
      }
      else if (type & FD_PTR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return log.warning(ERR::InvalidData);
         else if (not (type & (FD_OBJECT|FD_RESULT))) return log.warning(ERR::NoSupport);
         pos += sizeof(APTR);
      }
      else if (type & (FD_DOUBLE|FD_INT64|FD_INT)) {
         pos = argument_end_offset(Args[i], pos);
         if (pos < 0) return log.warning(ERR::InvalidData);
      }
      else if (type & FD_TAGS) return log.warning(ERR::NoSupport);
      else return log.warning(ERR::NoSupport);

      if (pos > ArgsSize) return log.warning(ERR::InvalidData);
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
      if ((type & FDF_SPAN) IS FDF_SPAN); // Embedded value; payload ownership is handled below.
      else if (type & FD_ARRAY) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
         APTR copy = nullptr;
         if (auto error = copy_cpp_array_arg(type, *(APTR *)(Parameters + pos), &copy); error != ERR::Okay) {
            for (auto &array : cpp_arrays) delete_cpp_array_arg(array.first, array.second);
            Buffer.clear();
            return error;
         }
         *(APTR *)(Buffer.data() + pos) = copy;
         cpp_arrays.emplace_back(type, copy);
      }
      pos = argument_end_offset(Args[i], pos);
      if ((pos < 0) or (pos > ArgsSize)) return ERR::InvalidData;
   }

   pos = 0;
   for (int i=0; Args[i].Name; i++) {
      int type = Args[i].Type;

      if ((type & FDF_SPAN) IS FDF_SPAN) {
         size_t aligned_offset, end_offset;
         if ((span_field_layout(Args[i], size_t(pos), &aligned_offset, &end_offset) != ERR::Okay) or
             (end_offset > size_t(ArgsSize))) return ERR::InvalidData;

         GenericSpan value;
         size_t byte_size;
         if (span_field_value(Args[i], Parameters + aligned_offset, &value, &byte_size) != ERR::Okay) {
            return ERR::InvalidData;
         }

         if (byte_size) {
            size_t element_size, element_alignment, payload_offset;
            if ((span_descriptor_element_layout(Args[i], &element_size, &element_alignment) != ERR::Okay) or
                (not align_arg_offset(Buffer.size(), element_alignment, &payload_offset)) or
                (payload_offset > size) or (byte_size > size - payload_offset)) return ERR::InvalidData;

            Buffer.resize(payload_offset);
            const auto payload_end = payload_offset + byte_size;
            Buffer.resize(payload_end);
            auto payload = Buffer.data() + payload_offset;
            if (not (type & FD_MUTABLE)) copymem(value.data(), payload, byte_size);
            rebind_span_field(Buffer.data() + aligned_offset, payload, value.size());
         }
         else rebind_span_field(Buffer.data() + aligned_offset, nullptr, 0);

         pos = int(end_offset);
      }
      else if (type & FD_ARRAY) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
         auto param = (APTR *)(Buffer.data() + pos);
         if ((type & FD_OBJECT) and (type & FD_PTR)) {
            pin_object_array((kt::vector<OBJECTPTR> *)*param);
         }
         pos += sizeof(APTR);
      }
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
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
         pos = argument_end_offset(Args[i], pos);
         if (pos < 0) return ERR::InvalidData;
      }
      else if (type & FD_FUNCTION) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
         auto function = (FUNCTION *)(Buffer.data() + pos);
         if (function->defined()) function->pin();
         pos += sizeof(FUNCTION);
      }
      else if (type & FD_PTR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
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
         pos += sizeof(APTR);
      }
      else {
         pos = argument_end_offset(Args[i], pos);
         if ((pos < 0) or (pos > ArgsSize)) return ERR::InvalidData;
      }
   }

   if (Buffer.size() != size) return ERR::InvalidData;
   return ERR::Okay;
}

/*********************************************************************************************************************
A buffer produced by copy_args() embeds absolute pointers into its own storage for spans, strings and sized buffers.
Those pointers survive moves of the owning std::vector, but not byte-for-byte duplication (e.g. serialisation into
the message queue).  make_args_relative() converts the self-referential pointers to offsets from the start of the
argument block so that make_args_absolute() can rebase them against the receiving copy.  Object, array and function
references are position independent and are left untouched, as are null pointers (offsets always exceed zero because
extension data follows the argument block).
*********************************************************************************************************************/

ERR make_args_relative(const FunctionField *Args, int ArgsSize, int8_t *Buffer, size_t BufferSize)
{
   if ((not Args) or (not Buffer)) return ERR::NullArgs;
   if ((ArgsSize <= 0) or (size_t(ArgsSize) > BufferSize)) return ERR::InvalidData;

   int pos = 0;
   for (int i=0; Args[i].Name and (pos < ArgsSize); i++) {
      int type = Args[i].Type;
      if ((type & FDF_SPAN) IS FDF_SPAN) {
         size_t aligned_offset, end_offset;
         if ((span_field_layout(Args[i], size_t(pos), &aligned_offset, &end_offset) != ERR::Okay) or
             (end_offset > size_t(ArgsSize))) return ERR::InvalidData;

         GenericSpan value;
         size_t byte_size;
         auto storage = Buffer + aligned_offset;
         if (span_field_value(Args[i], storage, &value, &byte_size) != ERR::Okay) return ERR::InvalidData;
         if (value.empty()) rebind_span_field(storage, nullptr, 0);
         else {
            const auto base_address = uintptr_t(Buffer);
            const auto data_address = uintptr_t(value.data());
            if ((data_address < base_address) or (data_address - base_address < size_t(ArgsSize))) {
               return ERR::InvalidData;
            }
            const auto payload_offset = data_address - base_address;
            if ((payload_offset > BufferSize) or (byte_size > BufferSize - payload_offset)) return ERR::InvalidData;
            rebind_span_field(storage, (CPTR)payload_offset, value.size());
         }
         pos = int(end_offset);
         continue;
      }
      else if (type & FD_ARRAY); // Owned heap copy; position independent
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
         if ((type & FD_CPP) and (not (type & FD_MUTABLE))) {
            auto view = (std::string_view *)(Buffer + pos);
            if (view->data()) *view = std::string_view((CSTRING)(view->data() - (CSTRING)Buffer), view->length());
         }
         else if (auto str = (int8_t **)(Buffer + pos); *str) {
            *str = (int8_t *)(*str - Buffer);
         }
      }
      else if (type & FD_PTR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
      }
      pos = argument_end_offset(Args[i], pos);
      if ((pos < 0) or (pos > ArgsSize)) return ERR::InvalidData;
   }

   return ERR::Okay;
}

ERR make_args_absolute(const FunctionField *Args, int ArgsSize, int8_t *Buffer, size_t BufferSize)
{
   if ((not Args) or (not Buffer)) return ERR::NullArgs;
   if ((ArgsSize <= 0) or (size_t(ArgsSize) > BufferSize)) return ERR::InvalidData;

   int pos = 0;
   for (int i=0; Args[i].Name and (pos < ArgsSize); i++) {
      int type = Args[i].Type;
      if ((type & FDF_SPAN) IS FDF_SPAN) {
         size_t aligned_offset, end_offset;
         if ((span_field_layout(Args[i], size_t(pos), &aligned_offset, &end_offset) != ERR::Okay) or
             (end_offset > size_t(ArgsSize))) return ERR::InvalidData;

         auto storage = Buffer + aligned_offset;
         GenericSpan value;
         size_t byte_size;
         if (span_field_value(Args[i], storage, &value, &byte_size) != ERR::Okay) return ERR::InvalidData;
         if (value.empty()) rebind_span_field(storage, nullptr, 0);
         else {
            const auto payload_offset = uintptr_t(value.data());
            if ((payload_offset < size_t(ArgsSize)) or (payload_offset > BufferSize) or
                (byte_size > BufferSize - payload_offset)) return ERR::InvalidData;
            rebind_span_field(storage, Buffer + payload_offset, value.size());
         }
         pos = int(end_offset);
         continue;
      }
      else if (type & FD_ARRAY); // Owned heap copy; position independent
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
         if ((type & FD_CPP) and (not (type & FD_MUTABLE))) {
            auto view = (std::string_view *)(Buffer + pos);
            if (view->data()) *view = std::string_view((CSTRING)Buffer + (MAXINT)view->data(), view->length());
         }
         else if (auto str = (int8_t **)(Buffer + pos); *str) {
            *str = Buffer + (MAXINT)*str;
         }
      }
      else if (type & FD_PTR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return ERR::InvalidData;
      }
      pos = argument_end_offset(Args[i], pos);
      if ((pos < 0) or (pos > ArgsSize)) return ERR::InvalidData;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

void release_copied_args(const FunctionField *Args, int ArgsSize, int8_t *Parameters, bool DereferenceAll,
   FUNCTION *DeferredFunction)
{
   if ((not Args) or (not Parameters)) return;

   for (int i=0, pos=0; Args[i].Name and (pos < ArgsSize); i++) {
      auto type = Args[i].Type;

      if ((type & FDF_SPAN) IS FDF_SPAN) {
         pos = argument_end_offset(Args[i], pos);
         if ((pos < 0) or (pos > ArgsSize)) return;
      }
      else if (type & FD_ARRAY) {
         pos = align_arg_offset(pos);
         if (pos < 0) return;
         auto array = *(APTR *)(Parameters + pos);
         if ((type & FD_OBJECT) and (type & FD_PTR)) {
            unpin_object_array((kt::vector<OBJECTPTR> *)array);
         }
         delete_cpp_array_arg(type, array);
         *(APTR *)(Parameters + pos) = nullptr;
         pos += sizeof(APTR);
      }
      else if (type & FD_STR) {
         pos = align_arg_offset(pos);
         if (pos < 0) return;
         if ((type & FD_CPP) and (not (type & FD_MUTABLE))) pos += sizeof(std::string_view);
         else pos += sizeof(APTR);
      }
      else if (type & FD_FUNCTION) {
         pos = align_arg_offset(pos);
         if (pos < 0) return;
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
         if (pos < 0) return;
         if ((type & FD_OBJECT) and (not (type & FD_RESULT))) {
            if (auto object = *(OBJECTPTR *)(Parameters + pos)) object->unpin(true);
            *(OBJECTPTR *)(Parameters + pos) = nullptr;
         }
         pos += sizeof(APTR);
      }
      else if (type & (FD_INT|FD_DOUBLE|FD_INT64)) {
         pos = argument_end_offset(Args[i], pos);
         if (pos < 0) return;
      }
      else if (type & FD_TAGS) break;
   }
}
