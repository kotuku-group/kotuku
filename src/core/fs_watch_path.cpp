
//********************************************************************************************************************

#ifdef __linux__

void fs_ignore_file(extFile *File)
{
   if ((File->prvWatch) and (File->prvWatch->Handle > 0)) {
      {
         std::lock_guard<std::mutex> lock(glmInotifyLookup);
         glInotifyLookup.erase(File->prvWatch->Handle);
      }
      inotify_rm_watch(glInotify, File->prvWatch->Handle);
      File->prvWatch->Handle = 0;
   }
}

#elif _WIN32

void fs_ignore_file(extFile *File)
{
   if ((File->prvWatch) and (File->prvWatch->Handle)) {
      RegisterFD(File->prvWatch->Handle, RFD::REMOVE|RFD::READ|RFD::WRITE|RFD::EXCEPT, 0, 0);
      winCloseHandle(File->prvWatch->Handle);
      File->prvWatch->Handle = nullptr;
   }
}

#elif __APPLE__

// OSX uses an FSEvents device https://en.wikipedia.org/wiki/FSEvents

void fs_ignore_file(extFile *File)
{

}

#endif

//********************************************************************************************************************

#ifdef __linux__

ERR fs_watch_path(extFile *File)
{
   int nflags = 0;
   if ((File->prvWatch->Flags & MFF::READ) != MFF::NIL) nflags |= IN_ACCESS;
   if ((File->prvWatch->Flags & MFF::MODIFY) != MFF::NIL) nflags |= IN_MODIFY;
   if ((File->prvWatch->Flags & MFF::CREATE) != MFF::NIL) nflags |= IN_CREATE;
   if ((File->prvWatch->Flags & MFF::DELETE) != MFF::NIL) nflags |= IN_DELETE | IN_DELETE_SELF;
   if ((File->prvWatch->Flags & MFF::OPENED) != MFF::NIL) nflags |= IN_OPEN;
   if ((File->prvWatch->Flags & MFF::ATTRIB) != MFF::NIL) nflags |= IN_ATTRIB;
   if ((File->prvWatch->Flags & MFF::CLOSED) != MFF::NIL) nflags |= IN_CLOSE_WRITE | IN_CLOSE_NOWRITE;
   if ((File->prvWatch->Flags & (MFF::MOVED|MFF::RENAME)) != MFF::NIL)  nflags |= IN_MOVED_FROM | IN_MOVED_TO;

   auto path = File->prvResolvedPath;
   if (path.ends_with('/')) path.pop_back();
   if (auto handle = inotify_add_watch(glInotify, path.c_str(), nflags); handle != -1) {
      File->prvWatch->Handle = handle;
      {
         std::lock_guard<std::mutex> lock(glmInotifyLookup);
         glInotifyLookup[handle] = File->UID;
      }
      return ERR::Okay;
   }
   else {
      kt::Log log;
      log.warning("%s", strerror(errno));
      return ERR::SystemCall;
   }
}

#elif _WIN32

static void path_monitor(HOSTHANDLE Handle, extFile *File);

ERR fs_watch_path(extFile *File)
{
   kt::Log log(__FUNCTION__);
   HOSTHANDLE handle;
   int winflags;
   ERR error;

   // The path_monitor() function will be called whenever the Path or its content is modified.

   if (!(error = winWatchFile(int(File->prvWatch->Flags), File->prvResolvedPath.c_str(),
         File->prvWatch->Buffer.get(), &handle, &winflags))) {
      if (!(error = RegisterFD(handle, RFD::READ, (void (*)(HOSTHANDLE, APTR))&path_monitor, File))) {
         File->prvWatch->Handle   = handle;
         File->prvWatch->WinFlags = winflags;
      }
      else {
         log.warning("Failed to register folder handle.");
         winCloseHandle(handle);
         File->prvWatch->Handle = nullptr;
      }
   }
   else log.warning("Failed to watch path %s, %s", File->prvResolvedPath.c_str(), GetErrorMsg(error));

   return error;
}

#else

ERR fs_watch_path(extFile *File)
{
   return ERR::NoSupport;
}

#endif

//********************************************************************************************************************
// Incoming file notification events pass through here.

#ifdef __linux__
void path_monitor(HOSTHANDLE FD, OBJECTPTR)
{
   kt::Log log(__FUNCTION__);
   static thread_local bool recursion = false; // Recursion avoidance is essential for correct queuing
   if (recursion) return;
   recursion = true;

   AdjustLogLevel(2);
   log.branch("File monitoring event received (FD %d).", FD);

   uint8_t buffer[8192];
   while (true) {
      auto result = read(FD, buffer, sizeof(buffer));
      if (result <= 0) {
         if ((result IS -1) and ((errno IS EAGAIN) or (errno IS EWOULDBLOCK))) break;
         if ((result IS -1) and (errno IS EINTR)) continue;
         break;
      }

      size_t offset = 0;
      while (offset < size_t(result)) {
         auto event = (struct inotify_event *)(buffer + offset);
         offset += sizeof(struct inotify_event) + event->len;

         if (event->mask & IN_Q_OVERFLOW) {
            log.warning("A buffer overflow has occurred in the file monitor.");
            continue;
         }

         OBJECTID file_id = 0;
         {
            std::lock_guard<std::mutex> lookup_lock(glmInotifyLookup);
            if (auto it = glInotifyLookup.find(event->wd); it != glInotifyLookup.end()) file_id = it->second;
         }
         if (!file_id) continue;

         ScopedObjectLock<extFile> lock(file_id, 50);
         if (not lock.granted()) continue;

         auto file = lock.obj;
         if ((!file->prvWatch) or (file->prvWatch->Handle != event->wd)) {
            std::lock_guard<std::mutex> lookup_lock(glmInotifyLookup);
            if (auto it = glInotifyLookup.find(event->wd); (it != glInotifyLookup.end()) and (it->second IS file_id)) {
               glInotifyLookup.erase(it);
            }
            continue;
         }

         if ((file->prvWatch->Flags & MFF::FOLDER) != MFF::NIL) {
            if (!(event->mask & IN_ISDIR)) continue;
         }
         else if ((file->prvWatch->Flags & MFF::FILE) != MFF::NIL) {
            if (event->mask & IN_ISDIR) continue;
         }

         const char *path = nullptr;
         if ((event->len > 0) and event->name[0]) {
            path = event->name;
         }

         MFF flags = MFF::NIL;
         if (event->mask & IN_ACCESS) flags |= MFF::READ;
         if (event->mask & IN_MODIFY) flags |= MFF::MODIFY;
         if (event->mask & IN_CREATE) flags |= MFF::CREATE;
         if (event->mask & IN_DELETE) flags |= MFF::DELETE;
         if (event->mask & IN_DELETE_SELF) flags |= MFF::DELETE|MFF::SELF;
         if (event->mask & IN_OPEN) flags |= MFF::OPENED;
         if (event->mask & IN_ATTRIB) flags |= MFF::ATTRIB;
         if (event->mask & (IN_CLOSE_WRITE|IN_CLOSE_NOWRITE)) flags |= MFF::CLOSED;
         if (event->mask & (IN_MOVED_FROM|IN_MOVED_TO)) flags |= MFF::MOVED;
         if (event->mask & IN_UNMOUNT) flags |= MFF::UNMOUNT;
         if (event->mask & IN_ISDIR) flags |= MFF::FOLDER;
         else flags |= MFF::FILE;

         ERR error = ERR::Okay;
         if (flags != MFF::NIL) {
            if (file->prvWatch->Routine.isC()) {
               auto routine = (ERR (*)(extFile *, std::string_view, MFF, APTR))file->prvWatch->Routine.Routine;
               kt::SwitchContext context(file->prvWatch->Routine.Context);
               error = routine(file, path, flags, file->prvWatch->Routine.Meta);
            }
            else if (file->prvWatch->Routine.isScript()) {
               if (sc::Call(file->prvWatch->Routine, std::to_array<ScriptArg>({
                     { "File",   file, FD_OBJECTPTR },
                     { "Path",   path },
                     { "Flags",  int(flags) }
                  }), error) != ERR::Okay) error = ERR::Function;
            }
            else error = ERR::Terminate;
         }

         bool ignored = (event->mask & IN_IGNORED) != 0;

         if (ignored) {
            std::lock_guard<std::mutex> lookup_lock(glmInotifyLookup);
            if (auto it = glInotifyLookup.find(event->wd); (it != glInotifyLookup.end()) and (it->second IS file_id)) {
               glInotifyLookup.erase(it);
            }
         }
         if (error IS ERR::Terminate) Action(fl::Watch::id, file, nullptr);
      }
   }

   recursion = false;
   AdjustLogLevel(-2);
}

//********************************************************************************************************************

#elif _WIN32

static std::string_view watched_filename(const extFile *File)
{
   std::string_view path(File->prvResolvedPath);
   auto slash = path.find_last_of("\\/");
   if (slash != std::string_view::npos) path.remove_prefix(slash + 1);
   return path;
}

static void path_monitor(HOSTHANDLE Handle, extFile *File)
{
   kt::Log log(__FUNCTION__);

   static thread_local bool recursion = false; // Recursion avoidance is essential for correct queuing
   if ((recursion) or (!File->prvWatch)) return;
   recursion = true;

   AdjustLogLevel(2);

   log.branch("File monitoring event received (Handle %p, File #%d).", Handle, File->UID);

   ERR error;
   if (File->prvWatch->Handle) {
      constexpr size_t path_buffer_size = 2048;
      std::array<char, path_buffer_size> path { };
      int status;

      // Keep in mind that the state of the File object might change during the loop due to the code in the user's callback.
      // Validate resources before each iteration to prevent crashes

      while ((File->prvWatch) and (File->prvWatch->Handle IS Handle)) {
         ERR read_result = winReadChanges(File->prvWatch->Handle, File->prvWatch->Buffer.get(),
            File->prvWatch->WinFlags, path.data(), int(path.size()), &status);
         if (read_result != ERR::Okay) {
            // If we get an error other than NothingDone, stop monitoring
            if (read_result != ERR::NothingDone) {
               log.warning("winReadChanges() failed with error %s", GetErrorMsg(read_result));
               break;
            }
            break; // NothingDone -> no more events
         }

         std::string_view event_path(path.data());

         if ((File->prvWatch->Flags & MFF::DEEP) IS MFF::NIL) { // Ignore if path is in a sub-folder and the deep option is not enabled.
            if (event_path.find('\\') != std::string_view::npos) continue;
         }

         if ((!File->isFolder) and (!iequals(event_path, watched_filename(File)))) continue;

         if (File->prvWatch->Routine.isC()) {
            kt::SwitchContext context(File->prvWatch->Routine.Context);
            auto routine = (ERR (*)(extFile *, std::string_view, MFF, APTR))File->prvWatch->Routine.Routine;
            error = routine(File, event_path, MFF(status), File->prvWatch->Routine.Meta);
         }
         else if (File->prvWatch->Routine.isScript()) {
            if (sc::Call(File->prvWatch->Routine, std::to_array<ScriptArg>({
               { "File",   File, FD_OBJECTPTR },
               { "Path",   path.data() },
               { "Flags",  int(status) }
            }), error) != ERR::Okay) error = ERR::Function;
         }
         else error = ERR::Terminate;

         if (error IS ERR::Terminate) {
            Action(fl::Watch::id, File, nullptr);
            break;
         }

         if (!File->prvWatch) { // Sanity check
            log.traceWarning("Watch removed during callback.");
            break;
         }
      }
   }
   else if (File->prvWatch->Routine.isC()) {
      auto routine = (ERR (*)(extFile *, CSTRING, MFF, APTR))File->prvWatch->Routine.Routine;
      kt::SwitchContext context(File->prvWatch->Routine.Context);
      error = routine(File, File->Path.c_str(), MFF::NIL, File->prvWatch->Routine.Meta);

      if (error IS ERR::Terminate) Action(fl::Watch::id, File, nullptr);
   }

   if (winValidateHandle(Handle)) {
      winFindNextChangeNotification(Handle);
   }
   else {
      log.warning("Handle invalid, cease monitoring File #%d.", File->UID);
      Action(fl::Watch::id, File, nullptr);
   }

   recursion = false;

   AdjustLogLevel(-2);
}

#endif
