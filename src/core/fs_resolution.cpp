/*********************************************************************************************************************
-CATEGORY-
Name: Files
-END-
*********************************************************************************************************************/

// included by lib_filesystem.cpp

//********************************************************************************************************************
// Cleans up path strings such as "../../myfile.txt".  Note that for Linux, the targeted file/folder has to exist or
// NULL will be returned.
//
// The Path must be resolved to the native OS format.

static std::optional<std::string> true_path(CSTRING Path)
{
#ifdef _WIN32
   std::string buffer;
   buffer.resize(256);
   while (true) {
      auto size = winGetFullPathName(Path, (int)buffer.size(), buffer.data(), nullptr);
      if (size <= 0) break;
      if ((size_t)size < buffer.size()) {
         buffer.resize(size);
         return std::make_optional<std::string>(buffer);
      }

      buffer.resize(size + 1);
   }

   return std::nullopt;
#else
   if (char *rp = realpath(Path, nullptr)) {
      std::string p(rp);
      free(rp);
      return std::make_optional<std::string>(p);
   }
   else return std::nullopt;
#endif
}

/*********************************************************************************************************************

-FUNCTION-
ResolvePath: Converts volume-based paths into absolute paths applicable to the host platform.

This function will convert a file path to its resolved form, according to the host system.  For example, a Linux
system might resolve `drive1:documents/readme.txt` to `/documents/readme.txt`.  A Windows system might
resolve the path to `c:\documents\readme.txt`.

The resulting path is guaranteed to be absolute, meaning the use of sequences such as `..`, `//` and `./` will be
eliminated.

If the path can be resolved to more than one file, ResolvePath() will attempt to discover the correct path by
checking the validity of each possible location.  For instance, if resolving a path of `user:document.txt`
and the `user:` volume refers to both `system:users/joebloggs/` and `system:users/default/`, the routine will check
both directories for the existence of the `document.txt` file to determine the correct location.  This approach
can be problematic if the intent is to create a new file, in which case `RSF::NO_FILE_CHECK` will circumvent it.

When checking the file location, ResolvePath() requires an exact match to the provided file name.  If the file
name can be approximated (i.e. the file extension can be ignored) then use the `RSF::APPROXIMATE` flag.

To resolve the location of executable programs on Unix systems, use the `RSF::PATH` flag.  This uses the `PATH`
environment variable to resolve the file name specified in the `Path` parameter.

The resolved path will be copied to the string provided in the `Result` parameter.  This will overwrite any
existing content in the string.

<types lookup="RSF"/>

If the path resolves to a virtual drive, it may not be possible to confirm whether the target file exists if the
virtual driver does not support this check.  This is common when working with network drives.

-INPUT-
strview Path: The path to be resolved.
int(RSF) Flags: Optional flags.
^&string Result: Refer to a string variable so that the resolved path can be stored.  If `NULL`, ResolvePath() will work as normal and return a valid error code without the result string.  The value is unchanged if the error code is not `ERR::Okay`.

-ERRORS-
Okay:        The `Path` was resolved.
Search:       The given volume does not exist.
FileNotFound: The path was resolved, but the referenced file or folder does not exist (use `NO_FILE_CHECK` to avoid this error code).
Loop:         The volume refers back to itself.
VirtualVolume: The path refers to a virtual volume (use `CHECK_VIRTUAL` to return `Okay` instead).
InvalidPath:  The path is malformed.
InvalidData:  Volume resolution returned invalid path data.
LoadModule:   A volume extension could not be loaded.
SystemLocked: The volume registry could not be accessed.

-TAGS-
blocking, path-resolved

-END-

*********************************************************************************************************************/

static ERR resolve(const std::string &, std::string &, RSF);
static ERR resolve_path_env(std::string_view, std::string *);
static thread_local bool tlClassLoaded;

ERR ResolvePath(const std::string_view &pPath, RSF Flags, std::string *Result)
{
   kt::Log log(__FUNCTION__);

   log.traceBranch("%s, Flags: $%.8x", pPath.data(), int(Flags));

   tlClassLoaded = false;

   std::string_view Path = pPath; // Copy

   if (Path.starts_with('~')) {
      Flags |= RSF::APPROXIMATE;
      Path.remove_prefix(1);
   }
   else if (startswith("string:", Path)) {
      if (Result) Result->assign(Path);
      return ERR::Okay;
   }

   std::string src, dest;
   src.reserve(MAX_FILENAME);
   dest.reserve(MAX_FILENAME);

   // Check if the Path parameter contains a volume character.  If it does not, make a clone of the string and return it.

   bool resolved = false;
#ifdef _WIN32
   if ((Path.size() >= 2) and (std::tolower(Path[0]) >= 'a') and (std::tolower(Path[0]) <= 'z') and (Path[1] IS ':')) {
      resolved = true;
      if ((Path.size() IS 2) or ((Path[2] != '/') and (Path[2] != '\\'))) {
         // Ensure that the path is correctly formed in order to pass test_path()
         src = { Path[0], ':', '\\' };
         if (Path.size() >= 3) src.append(Path, 2, std::string::npos);
         Path = std::string_view(src);
      }
   }
   else if (Path.starts_with("//")) resolved = true; // UNC path discovered
   else if (Path.starts_with("\\\\")) resolved = true; // UNC path discovered

#elif __unix__
   if (Path.starts_with('/') or Path.starts_with('\\')) resolved = true;
#endif

   if (!resolved) {
      auto sep = Path.find_first_of(":/\\");

      // Use the PATH environment variable to resolve the filename.  This can only be done if the path is relative
      // (ideally with no leading folder references).

      if (((sep IS std::string::npos) or (Path[sep] != ':')) and ((Flags & RSF::PATH) != RSF::NIL)) {
         if (!resolve_path_env(Path, Result)) return ERR::Okay;
      }

      if ((sep IS std::string::npos) or (Path[sep] != ':')) resolved = true;
   }

   std::string_view final_path(Path);
   if (resolved) {
      dest.assign(final_path);

      if ((Flags & RSF::APPROXIMATE) != RSF::NIL) {
         if (!test_path(dest, RSF::APPROXIMATE)) final_path = dest;
         else return ERR::FileNotFound;
      }
      else if ((Flags & RSF::NO_FILE_CHECK) IS RSF::NIL) {
         if (!test_path(dest, RSF::NIL)) final_path = dest;
         else return ERR::FileNotFound;
      }

      if (!Result) return ERR::Okay;

      auto tp = true_path(dest.c_str());
      if (tp.has_value()) Result->assign(tp.value());
      else Result->assign(dest);
      return ERR::Okay;
   }

   src.assign(final_path);

   // Keep looping until the volume is resolved

   int loop;
   auto error = ERR::ResolvePath;
   for (loop=10; loop > 0; loop--) {
      error = resolve(src, dest, Flags);

      if (error IS ERR::VirtualVolume) {
         log.trace("Detected virtual volume '%s'", dest.c_str());

         // If RSF::CHECK_VIRTUAL is set, return ERR::VirtualVolume for reserved volume names, otherwise Okay.

         if ((Flags & RSF::CHECK_VIRTUAL) IS RSF::NIL) error = ERR::Okay;

         if (Result) {
            if ((Flags & RSF::APPROXIMATE) != RSF::NIL) { // Ensure that the resolved path is accurate
               if (test_path(dest, RSF::APPROXIMATE) != ERR::Okay) error = ERR::FileNotFound;
            }

            Result->assign(dest);
         }

         break;
      }
      else if (error != ERR::Okay) break;
      else {
         #ifdef _WIN32 // UNC network path check
            if (((dest[0] IS '\\') and (dest[1] IS '\\')) or ((dest[0] IS '/') and (dest[1] IS '/'))) {
               if (Result) Result->assign(dest);
               return ERR::Okay;
            }
         #endif

         // Check if the path has been resolved by checking for a volume label.

         if (auto i = dest.find_first_of(":/\\"); i != std::string::npos) {
            #ifdef _WIN32
            if ((dest[i] IS ':') and (i > 1)) {
            #else
            if (dest[i] IS ':') {
            #endif
               // Copy the destination to the source buffer and repeat the resolution process.

               if ((Flags & RSF::NO_DEEP_SCAN) != RSF::NIL) return ERR::Search;
               src = dest;
               continue; // Keep resolving
            }
         }
      }

      if (Result) {
         auto tp = true_path(dest.c_str());
         if (tp.has_value()) Result->assign(tp.value());
         else Result->assign(dest);
      }

      break;
   } // for()

   if (loop > 0) { // Note that loop starts at 10 and decrements to zero
      if ((!error) and dest.empty()) return ERR::InvalidPath;
      return error;
   }
   else return ERR::Loop;
}

//********************************************************************************************************************
// For resolving file references via the host environment's PATH variable.  This will only work for relative paths.

#ifdef __unix__

static ERR resolve_path_env(std::string_view RelativePath, std::string *Result)
{
   // If a path to the file isn't available, scan the PATH environment variable. In Unix the separator is :

   CSTRING path;
   if ((path = getenv("PATH")) and (path[0])) {
      std::string src;
      src.reserve(512);

      auto vp = std::string_view(path);
      while (!vp.empty()) {
         auto sep = vp.find(':');
         src.assign(vp, 0, sep);
         if (!src.ends_with('/')) src.append("/");
         src.append(RelativePath);

         struct stat64 info;
         if (!stat64(src.c_str(), &info)) {
            if (!S_ISDIR(info.st_mode)) { // Successfully identified file location
               if (Result) {
                  auto tp = true_path(src.c_str());
                  if (tp.has_value()) Result->assign(tp.value());
                  else Result->assign(src);
               }
               return ERR::Okay;
            }
         }

         if (sep != std::string::npos) vp.remove_prefix(sep + 1);
         else break;
      }
   }

   return ERR::NothingDone;
}

#elif _WIN32

static ERR resolve_path_env(std::string_view RelativePath, std::string *Result)
{
   // If a path to the file isn't available, scan the PATH environment variable. In Windows the separator is ;

   CSTRING path;
   if ((path = getenv("PATH")) and (path[0])) {
      std::string src;
      src.reserve(512);

      auto vp = std::string_view(path);
      while (!vp.empty()) {
         auto sep = vp.find(';');
         src.assign(vp, 0, sep);
         if (!src.ends_with('/')) src.append("/");
         src.append(RelativePath);

         struct stat64 info;
         if (!stat64(src.c_str(), &info)) {
            if (!S_ISDIR(info.st_mode)) { // Successfully identified file location
               if (Result) {
                  auto tp = true_path(src.c_str());
                  if (tp.has_value()) Result->assign(tp.value());
                  else Result->assign(src);
               }
               return ERR::Okay;
            }
         }

         if (sep != std::string::npos) vp.remove_prefix(sep + 1);
         else break;
      }
   }

   return ERR::NothingDone;
}

#endif

/*********************************************************************************************************************
** Note: This function calls itself recursively.  For use by ResolvePath() only.
**
** Config  - The SystemVolumes configuration object.
** Source  - The file string that we are trying to resolve.
** Dest    - Buffer area; the resolved location will be stored here.
** Flags   - Optional RSF flags.
*/

static ERR resolve(const std::string &Source, std::string &Dest, RSF Flags)
{
   kt::Log log("ResolvePath");

   if (&Source IS &Dest) return log.warning(ERR::SanityCheckFailed);

   if (get_virtual(Source)) {
      Dest.assign(Source);
      return ERR::VirtualVolume;
   }

   auto vol_pos = Source.find(':');
   if (vol_pos IS std::string::npos) return log.warning(ERR::InvalidData);

   std::string fullpath;
   if (auto lock = std::shared_lock{glmVolumes, 2s}) {
      auto vol = glVolumes.find(std::string_view(Source.data(), vol_pos));
      if (vol != glVolumes.end()) {
         if (auto path = vol->second.find("Path"); path != vol->second.end()) fullpath.assign(path->second);
      }
      else {
         log.msg("No matching volume for \"%s\".", Source.c_str());
         return ERR::Search;
      }
   }
   else return log.warning(ERR::SystemLocked);

   log.traceBranch("%s, Resolved Path: %s, Flags: $%.8x", Source.c_str(), fullpath.c_str(), int(Flags));

   auto path = std::string_view(fullpath);

   // Check if the EXT: reference is used.  If so, respond by loading the module or class that handles the volume.
   // The loaded code should replace the volume with the correct information for discovery on the next resolution phase.

   if (path.starts_with("EXT:")) {
      Dest = Source; // Return an exact duplicate of the original source string

      if (get_virtual(Source)) return ERR::VirtualVolume;

      if (tlClassLoaded) { // Already attempted to load the module on a previous occasion - we must fail
         return ERR::LoadModule;
      }

      // An external reference can refer to a module for auto-loading (preferred) or a class name.

      path.remove_prefix(4);
      objModule::create mod = { fl::Name(path.data()) };
      if (!mod.ok()) FindClass(ResolveClassName(path.data()));

      tlClassLoaded = true; // This setting will prevent recursion
      return ERR::VirtualVolume;
   }

   std::string buffer;
   buffer.reserve(MAX_FILENAME);
   while (true) {
      auto sep = path.find('|');
      if (sep IS std::string::npos) Dest.assign(path);
      else Dest.assign(path, 0, sep);

      if ((!Dest.ends_with('/')) and (!Dest.ends_with('\\'))) Dest.append("/");

      // Copy the rest of the source to the destination buffer

      std::size_t j = vol_pos + 1;
      while ((Source[j] IS '/') or (Source[j] IS '\\')) j++;
      Dest.append(Source, j);

      // Fully resolve the path to a system folder before testing it (e.g. "scripts:" to "kotuku:scripts/" to "c:\kotuku\scripts\" will be resolved through this recursion).

      #ifdef _WIN32
         if ((Dest[1] IS ':') and ((Dest[2] IS '/') or (Dest[2] IS '\\'))) j = std::string::npos;
         else if ((Dest[0] IS '/') and (Dest[1] IS '/')) j = std::string::npos;
         else if ((Dest[0] IS '\\') and (Dest[1] IS '\\')) j = std::string::npos;
         else j = Dest.find_first_of(":/");
      #else
         j = Dest.find_first_of(":/");
      #endif

      int loop;
      auto error = ERR(-1);
      for (loop=10; loop > 0; loop--) {
         if ((j != std::string::npos) and (j > 1) and (Dest[j] IS ':')) { // Remaining ':' indicates more path resolution is required.
            if (!(error = resolve(Dest, buffer, Flags))) {
               Dest.assign(buffer);
               j = Dest.find_first_of(":/"); // Reexamine the result for the presence of a colon.
            }
            else break; // Path not resolved or virtual volume detected.
         }
         else break;
      }

      if (loop <= 0) {
         log.warning("Infinite loop on path '%s'", Dest.c_str());
         return ERR::Loop;
      }

      if (!error) return ERR::Okay;
      else if ((Flags & RSF::NO_FILE_CHECK) != RSF::NIL) return ERR::Okay;
      else if (!test_path(Dest, Flags)) return ERR::Okay;

      log.trace("File does not exist at %s", Dest.c_str());

      if ((Flags & RSF::NO_DEEP_SCAN) != RSF::NIL) {
         log.trace("No deep scanning - additional paths will not be checked.");
         break;
      }

      if (sep != std::string::npos) path.remove_prefix(sep + 1);
      else break;
   }

   log.trace("Resolved path but no matching file for %s\"%s\".", ((Flags & RSF::APPROXIMATE) != RSF::NIL) ? "~" : "", Source.c_str());
   return ERR::FileNotFound;
}
