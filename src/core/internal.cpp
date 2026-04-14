/*********************************************************************************************************************

Functions that are internal to the Core.

*********************************************************************************************************************/

#ifdef __unix__
 #include <signal.h>
 #include <sys/wait.h>
#endif

#include "defs.h"

using namespace pf;

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
         if (auto &rec = it->second; !rec.Match.empty()) {
            std::vector<std::string> list;
            pf::split(rec.Match, std::back_inserter(list), '|');
            for (auto & wild : list) {
               if (wild.starts_with("*.")) {
                  glWildClassMap.emplace(pf::strihash(wild.c_str() + 2), it->first);
               }
            }
         }
      }

      glWildClassMapTotal = glClassDB.size();
   }

   auto hash = pf::strihash(Ext);

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

ERR process_janitor(OBJECTID SubscriberID, int Elapsed, int TotalElapsed)
{
   if (glTasks.empty()) {
      glJanitorActive = false;
      return ERR::Terminate;
   }

#ifdef __unix__
   pf::Log log(__FUNCTION__);

   // Call waitpid() to check for zombie processes first.  This covers all processes within our own context, so our child processes, children of those children etc.

   // However, it can be 'blocked' from certain processes, e.g. those started from ZTerm.  Such processes are discovered in the second search routine.

   int childprocess, status;
   while ((childprocess = waitpid(-1, &status, WNOHANG)) > 0) {
      log.warning("Zombie process #%d discovered.", childprocess);

      for (auto &task : glTasks) {
         if (childprocess IS task.ProcessID) {
            task.ReturnCode = WEXITSTATUS(status);
            task.Returned   = true;
            validate_process(childprocess);
            break;
         }
      }
   }

   // Check all registered processes to see which ones are alive.  This routine can manage all processes, although exhibits
   // some problems with zombies, hence the earlier waitpid() routine to clean up such processes.

   for (auto &task : glTasks) {
      if ((kill(task.ProcessID, 0) IS -1) and (errno IS ESRCH)) {
         validate_process(task.ProcessID);
      }
   }

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

copy_args: Serialise argument structures into sendable messages.

This function searches an argument structure for pointer and string types.  If it encounters them, it attempts to
convert them to a format that can be passed to other memory spaces.

A PTR|RESULT followed by a PTRSIZE indicates that the user has to supply a buffer to the function.  It is assumed that
the function will fill the buffer with data, which means that a result set has to be returned to the caller.  Example:

<pre>
Read(Bytes (FD_INT), Buffer (FD_PTRRESULT), BufferSize (FD_PTRSIZE), &BytesRead (FD_INTRESULT));
</pre>

A standard PTR followed by a PTRSIZE indicates that the user has to supply a buffer to the function.  It is assumed
that this is one-way traffic only, and the function will not fill the buffer with data.  Example:

<pre>
Write(Bytes (FD_INT, Buffer (FD_PTR), BufferSize (FD_PTRSIZE), &BytesWritten (FD_INTRESULT));
</pre>

If the function will return a memory block of its own, it must return the block as a MEMORYID, not a PTR.  The
allocation must be made using the object's MemFlags, as the action messaging functions will change between
public|untracked and private memory flags as necessary.  Example:

  Read(Bytes (FD_INT), &BufferMID (FD_INTRESULT), &BufferSize (FD_INTRESULT));

*********************************************************************************************************************/

ERR copy_args(const FunctionField *Args, int ArgsSize, int8_t *Parameters, std::vector<int8_t> &Buffer)
{
   pf::Log log(__FUNCTION__);

   if ((!Args) or (!Parameters)) return ERR::NullArgs;

   // Buffer size must be computed in advance

   int size = ArgsSize;
   int pos = 0;
   for (int i=0; Args[i].Name; i++) {
      if (pos >= ArgsSize) return ERR::InvalidData; // Sanity check, the pos can't exceed ArgsSize.

      if (Args[i].Type & FD_STR) {
         if (Args[i].Type & FD_CPP) {
            if (std::string *str = ((std::string **)(Parameters + pos))[0]) size += str->length() + 1;
         }
         else if (auto str = ((STRING *)(Parameters + pos))[0]) size += strlen(str) + 1;
         pos += sizeof(APTR);
      }
      else if (Args[i].Type & FD_PTR) {
         if (Args[i].Type & FD_RESULT); // Result will be stored in the parameter buffer.
         else if (Args[i].Type & (FD_INT|FD_PTRSIZE)) { // Pointer to int.
            pos += sizeof(int);
         }
         else if (Args[i].Type & (FD_DOUBLE|FD_INT64)) { // Pointer to large/double
            pos += sizeof(int64_t);
         }
         else if (Args[i+1].Type & FD_PTRSIZE) {
            if (int memsize = ((int *)(Parameters + pos + sizeof(APTR)))[0]; memsize > 0) {
               size += memsize;
            }
         }
         else { // No PTRSIZE is a problem
            log.warning(ERR::InvalidType);
         }
         pos += sizeof(APTR);
      }
      else if (Args[i].Type & (FD_DOUBLE|FD_INT64)) pos += sizeof(int64_t);
      else pos += sizeof(int);
   }

   Buffer.reserve(size); // Ensures that the buffer space remains stable when extended.
   Buffer.resize(ArgsSize);
   copymem(Parameters, Buffer.data(), ArgsSize);

   pos = 0;
   for (int i=0; Args[i].Name; i++) {
      APTR param = Buffer.data() + pos;

      if (Args[i].Type & FD_STR) {
         if (Args[i].Type & FD_CPP) {
            auto insert = (STRING)(Buffer.data() + Buffer.size());
            if (std::string *str = ((std::string **)(Parameters + pos))[0]) {
               Buffer.resize(Buffer.size() + str->length() + 1);
               ((STRING *)param)[0] = insert;
               copymem(str->c_str(), insert, str->length() + 1);
            }
            else ((STRING *)param)[0] = nullptr;
            pos += sizeof(std::string);
         }
         else {
            auto insert = (STRING)(Buffer.data() + Buffer.size());
            if (auto str = ((STRING *)(Parameters + pos))[0]) {
               auto len = strlen(str);
               Buffer.resize(Buffer.size() + len + 1);
               ((STRING *)param)[0] = insert;
               copymem(str, Buffer.data() + Buffer.size(), len + 1);
            }
            else ((STRING *)param)[0] = nullptr;
            pos += sizeof(STRING);
         }
      }
      else if (Args[i].Type & FD_PTR) {
         if (Args[i].Type & FD_INT) { // Pointer to int.
            auto insert = (int *)(Buffer.data() + Buffer.size());
            Buffer.resize(Buffer.size() + sizeof(int));
            insert[0] = ((int *)(Parameters + pos))[0];
            ((APTR *)param)[0] = insert;
         }
         else if (Args[i].Type & (FD_DOUBLE|FD_INT64)) { // Pointer to large/double
            auto insert = (int64_t *)(Buffer.data() + Buffer.size());
            Buffer.resize(Buffer.size() + sizeof(int64_t));
            insert[0] = ((int64_t *)(Parameters + pos))[0];
            ((APTR *)param)[0] = insert;
         }
         else if (Args[i+1].Type & FD_PTRSIZE) { // Pointer to a buffer
            if (int memsize = ((int *)(Parameters + pos + sizeof(APTR)))[0]; memsize > 0) {
               if (Args[i].Type & FD_RESULT) { // 'Receive' buffer
                  Buffer.resize(Buffer.size() + memsize);
               }
               else { // 'Send' buffer
                  if (int8_t *src = ((int8_t **)param)[0]) { // Get the data source pointer
                     auto insert = Buffer.data() + Buffer.size();
                     ((APTR *)param)[0] = insert;
                     Buffer.resize(Buffer.size() + memsize);
                     copymem(src, insert, memsize);
                  }
               }
            }
            else ((APTR *)param)[0] = nullptr;
         }
         else ((APTR *)param)[0] = nullptr;
         pos += sizeof(APTR);
      }
      else if (Args[i].Type & (FD_DOUBLE|FD_INT64)) pos += sizeof(int64_t);
      else pos += sizeof(int);
   }

   return ERR::Okay;
}
