/*********************************************************************************************************************

The source code for Kōtuku is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

-CATEGORY-
Name: Files
-END-

*********************************************************************************************************************/

//#define DEBUG
#define PRV_FILESYSTEM
#define PRV_FILE
#define _LARGE_FILES

#ifdef _WIN32
typedef int HANDLE;
#endif

#if defined(__unix__) && !defined(_WIN32)
 #include <unistd.h>
 #include <dirent.h>
 #include <fcntl.h>
 #include <grp.h>
 #include <pwd.h>
 #include <signal.h>
 #include <stdlib.h>
 #include <stdio.h>
 #include <string.h>
 #include <time.h>
 #include <utime.h>
 #include <sys/types.h>
 #include <sys/stat.h>

 #ifndef __linux__
  #include <sys/statvfs.h>
 #else
  #include <sys/vfs.h>
  #define statvfs statfs
  #define fstatvfs fstatfs
 #endif

 #include <sys/time.h>
 #include <sys/syscall.h>

 #ifdef __linux__
  #include <sys/inotify.h>
  //#include "inotify-syscalls.h"
  //#include "inotify.h"
 #endif
#endif

#include "defs.h"
#include <kotuku/main.h>
#include <kotuku/strings.hpp>

#include <stdarg.h>
#include <errno.h>
#include <map>
#include <mutex>
#include <bit>

#ifdef _WIN32
 #include <io.h>
 #include <fcntl.h>
 #include <stdlib.h>
 #include <stdio.h>
 #include <errno.h>
 #ifdef _MSC_VER
  #define S_IRUSR _S_IREAD
  #define S_IWUSR _S_IWRITE
  #ifndef _S_ISTYPE
   #define _S_ISTYPE(mode, mask)  (((mode) & _S_IFMT) == (mask))
   #define S_ISREG(mode) _S_ISTYPE((mode), _S_IFREG)
   #define S_ISDIR(mode) _S_ISTYPE((mode), _S_IFDIR)
  #endif
  #define stat64 stat
 #else
  #include <unistd.h>
 #endif
 #include <sys/types.h>
 #include <sys/stat.h>
 #include <time.h>
 #include <string.h>

 #define open64   open
 #define lseek64  lseek
#endif // _WIN32

#ifdef __APPLE__
 #include <sys/param.h>
 #include <sys/mount.h>
 #define lseek64  lseek
 #define ftruncate64 ftruncate
#endif

//********************************************************************************************************************

struct extCacheFile : public CacheFile {
   std::string FullPath;
   std::vector<int8_t> Buffer;
   int16_t Locks;       // Internal count of active locks for this element.

   extCacheFile() = default;

   extCacheFile(std::string_view pPath, int64_t pSize, int64_t pTimestamp)
      : FullPath(pPath), Buffer(pSize + 1), Locks(1)
   {
      Path      = FullPath.c_str();
      Size      = pSize;
      TimeStamp = pTimestamp;
      LastUse   = PreciseTime();

      Buffer.back() = 0; // Null terminator is added to help with text file processing
      Data = Buffer.data(); // Client has direct access
   }
};

//********************************************************************************************************************

class CacheFileIndex {
public:
   std::string path;
   int64_t timestamp{};
   int64_t size{};

   CacheFileIndex() = default;

   CacheFileIndex(std::string_view Path, int64_t Timestamp, int64_t Size)
      : path(Path), timestamp(Timestamp), size(Size) { }

   friend bool operator==(const CacheFileIndex &, const CacheFileIndex &) = default;
};

namespace std {
   template <>
   struct hash<CacheFileIndex> {
      std::size_t operator()(const CacheFileIndex& Index) const noexcept {
         std::size_t path_hash = std::hash<std::string>()(Index.path);
         const auto timestamp_hash = std::hash<int64_t>()(Index.timestamp);
         const auto size_hash = std::hash<int64_t>()(Index.size);

         path_hash ^= std::rotl(timestamp_hash, 1);
         path_hash ^= std::rotl(size_hash, 1);
         return path_hash;
      }
   };
}

static ankerl::unordered_dense::map<CacheFileIndex, extCacheFile> glCache;
static std::mutex glCacheLock;

//********************************************************************************************************************

[[nodiscard]] constexpr uint32_t get_volume_id(std::string_view Path) noexcept
{
   if ((Path.starts_with(':')) or (Path.empty())) return 0;

   uint32_t hash = 5381;
   for (char c : Path) {
      if (c IS ':') break;
      if ((c IS '/') or (c IS '\\')) return 0; // If a slash is encountered early, the path belongs to the local FS
      if ((c >= 'A') and (c <= 'Z')) hash = (hash<<5) + hash + c - 'A' + 'a';
      else hash = (hash<<5) + hash + uint8_t(std::tolower(c));
   }
   return hash;
}

//********************************************************************************************************************
// Called during shutdown

void free_file_cache(void)
{
   glCache.clear();
}

//********************************************************************************************************************

extern "C" FFR CALL_FEEDBACK(FUNCTION *Callback, FileFeedback *Feedback)
{
   if ((!Callback) or (!Feedback)) return FFR::OKAY;

   if (Callback->isC()) {
      auto routine = (FFR (*)(FileFeedback *, APTR))Callback->Routine;
      return routine(Feedback, Callback->Meta);
   }
   else if (Callback->isScript()) {
      ERR error;
      if (sc::Call(*Callback, std::to_array<ScriptArg>({
         { "Size",       Feedback->Size },
         { "Position",   Feedback->Position },
         { "Path",       Feedback->Path },
         { "Dest",       Feedback->Dest },
         { "FeedbackID", int(Feedback->FeedbackID) }
      }), error) != ERR::Okay) error = ERR::Function;

      if (error IS ERR::Okay) {
         CSTRING *results;
         int size;
         if ((Callback->Context->get(FID_Results, results, size) IS ERR::Okay) and (size > 0)) {
            return FFR(strtol(results[0], nullptr, 0));
         }
         else return FFR::OKAY;
      }
      else return FFR::OKAY;
   }
   else return FFR::OKAY;
}

//********************************************************************************************************************
// Check if a Path refers to a virtual volume, and if so, return the matching virtual_drive definition.

static const virtual_drive * get_virtual(std::string_view Path)
{
   if (Path.empty() or Path.starts_with(':')) return &glVirtual[0]; // Root level counts as virtual
   auto id = get_volume_id(Path);
   if ((id) and (glVirtual.contains(id))) return &glVirtual[id];
   return nullptr;
}

//********************************************************************************************************************
// Returns a virtual_drive structure for ALL path types. Defaults to the host file system if no virtual drive was
// identified.
//
// The Path must be resolved before you call this function, this is necessary to solve cases where a volume is a
// shortcut to multiple paths for example.

const virtual_drive * get_fs(std::string_view Path)
{
   auto id = get_volume_id(Path);
   if (glVirtual.contains(id)) return &glVirtual[id];
   return &glVirtual[0];
}

//********************************************************************************************************************
// Assigned to a timer for the purpose of checking up on the expiry of cached files.

ERR check_cache(OBJECTPTR Subscriber, int64_t Elapsed, int64_t CurrentTime)
{
   pf::Log log(__FUNCTION__);

   log.branch("Scanning file cache for unused entries...");

   constexpr int64_t CACHE_EXPIRY_MICROSECONDS = 60LL * 1000000LL; // 60 seconds in microseconds

   const std::lock_guard<std::mutex> lock(glCacheLock);
   for (auto it=glCache.begin(); it != glCache.end(); ) {
      if ((CurrentTime - it->second.LastUse >= CACHE_EXPIRY_MICROSECONDS) and (it->second.Locks <= 0)) {
         log.msg("Removing expired cache file: %.80s", it->second.Path);
         it = glCache.erase(it);
      }
      else it++;
   }

   if (glCache.empty()) {
      glCacheTimer = 0;
      return ERR::Terminate;
   }
   else return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
AddInfoTag: Adds new tags to !FileInfo structures.

This function adds file tags to !FileInfo structures.  It is intended for use by the Core and external drivers
only.  Tags allow extended attributes to be associated with a file, for example the number of seconds of audio
in an MP3 file.

-INPUT-
struct(FileInfo) Info: Pointer to a valid !FileInfo structure.
cstr Name: The name of the tag, which must be declared in camel-case.
cstr Value: The value to associate with the tag name.  If `NULL`, any existing tag with a matching `Name` will be removed.

-ERRORS-
Okay:
NullArgs:

*********************************************************************************************************************/

ERR AddInfoTag(FileInfo *Info, CSTRING Name, CSTRING Value)
{
   if ((!Info) or (!Name)) return ERR::NullArgs;

   if (!Value) {
      if (Info->Tags) Info->Tags->erase(Name);
      return ERR::Okay;
   }

   if (!Info->Tags) {
      Info->Tags = new (std::nothrow) ankerl::unordered_dense::map<std::string, std::string>();
      if (!Info->Tags) return ERR::CreateResource;
   }

   (*Info->Tags)[Name] = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FUNCTION-
AnalysePath: Analyses paths to determine their type (file, folder or volume).

This function will analyse a path and determine the type of file that the path is referring to.  For instance, a path
of `user:documents/` would indicate a folder reference.  A path of `system:` would be recognised as a volume. A path
of `user:documents/copyright.txt` would be recognised as a file.

Ambiguous references are analysed to get the correct type - for example `user:documents/helloworld` could refer to a
folder or file, so the path is analysed to check the file type.  On exceptional occasions where the path could be
interpreted as either a folder or a file, preference is given to the folder.

File path approximation is supported if the `Path` is prefixed with a `~` character (e.g. `~pictures:photo` could be
matched to `photo.jpg` in the same folder).

To check if a volume name is valid, call ~ResolvePath() first and then pass the resulting path to this
function.

If the queried path does not exist, a fail code is returned.  This behaviour makes the AnalysePath() function a good
candidate for testing the validity of a path string.

-INPUT-
cstr Path: The path to analyse.
&int(LOC) Type: The result will be stored in the variable referred to by this parameter.  The return types are `DIRECTORY`, `FILE` and `VOLUME`.  Set this parameter to `NULL` if you are only interested in checking if the file exists.

-ERRORS-
Okay: The path was analysed and the result is stored in the `Type` variable.
NullArgs:
DoesNotExist:

*********************************************************************************************************************/

ERR AnalysePath(CSTRING Path, LOC *PathType)
{
   pf::Log log(__FUNCTION__);

   if (PathType) *PathType = LOC::NIL;
   if ((not Path) or (not Path[0])) return ERR::NullArgs;

   // Special volumes 'string:' and 'memory:' are considered to be file paths.

   if (startswith("string:", Path)) {
      if (PathType) *PathType = LOC::FILE;
      return ERR::Okay;
   }

   log.traceBranch("%s", Path);

   RSF flags = RSF::NIL;
   if (Path[0] IS '~') {
      flags |= RSF::APPROXIMATE;
      Path++;
   }

   int len = strlen(Path);
   if (Path[len-1] IS ':') {
      if (auto lock = std::unique_lock{glmVolumes, 6s}) {
         std::string path_vol(Path, len-1);
         if (glVolumes.contains(path_vol)) {
            if (PathType) *PathType = LOC::VOLUME;
            return ERR::Okay;
         }
      }
      return ERR::DoesNotExist;
   }

   std::string test_path;
   if (ResolvePath(Path, flags, &test_path) IS ERR::Okay) {
      log.trace("Testing path type for '%s'", test_path.c_str());

      auto vd = get_fs(test_path);
      if (vd->TestPath) {
         LOC dummy;
         if (!PathType) PathType = &dummy; // Dummy variable, helps to avoid bugs
         return vd->TestPath(test_path, RSF::NIL, PathType);
      }
      else return ERR::NoSupport;
   }
   else {
      log.trace("Path '%s' does not exist.", Path);
      return ERR::DoesNotExist;
   }
}

/*********************************************************************************************************************

-FUNCTION-
CompareFilePaths: Checks if two file paths refer to the same physical file.

This function will test two file paths, checking if they refer to the same file in a storage device.  It uses a string
comparison on the resolved path names, then attempts a second test based on an in-depth analysis of file attributes if
the string comparison fails.  In the event of a match, `ERR::Okay` is returned.  All other error codes indicate a
mis-match or internal failure.

The targeted paths do not have to refer to an existing file or folder in order to match (i.e. match on string
comparison succeeds).

-INPUT-
cstr PathA: File location 1.
cstr PathB: File location 2.

-ERRORS-
Okay: The file paths refer to the same file.
False: The file paths refer to different files.
NullArgs
-END-

*********************************************************************************************************************/

ERR CompareFilePaths(CSTRING PathA, CSTRING PathB)
{
   if ((!PathA) or (!PathB)) return ERR::NullArgs;

   std::string path1, path2;
   ERR error;
   if ((error = ResolvePath(PathA, RSF::NO_FILE_CHECK, &path1)) != ERR::Okay) return error;
   if ((error = ResolvePath(PathB, RSF::NO_FILE_CHECK, &path2)) != ERR::Okay) return error;

   const virtual_drive *v1, *v2;
   v1 = get_fs(path1);
   v2 = get_fs(path2);

   if ((!v1->CaseSensitive) and (!v2->CaseSensitive)) {
      error = iequals(path1, path2) ? ERR::True : ERR::False;
   }
   else error = (std::string_view(path1) IS std::string_view(path2)) ? ERR::True : ERR::False;

   if (error != ERR::Okay) {
      if (v1 IS v2) {
         // Ask the virtual FS if the paths match

         if (v1->SameFile) {
            error = v1->SameFile(path1, path2);
         }
         else error = ERR::False; // Assume the earlier string comparison is good enough
      }
      else error = ERR::False;
   }

   return error;
}

// In this variant it is assumed that the paths are already resolved.

static ERR CompareResolvedPaths(std::string_view PathA, std::string_view PathB)
{
   const auto v1 = get_fs(PathA);
   const auto v2 = get_fs(PathB);

   ERR error;
   if ((!v1->CaseSensitive) and (!v2->CaseSensitive)) error = iequals(PathA, PathB) ? ERR::True : ERR::False;
   else error = (std::string_view(PathA) IS std::string_view(PathB)) ? ERR::True : ERR::False;

   if (error != ERR::Okay) {
      if (v1 IS v2) { // Ask the virtual FS if the paths match
         if (v1->SameFile) return v1->SameFile(PathA, PathB);
         else return ERR::False; // Assume the earlier string comparison is sufficient
      }
      else return ERR::False;
   }
   return error;
}

//********************************************************************************************************************

ERR fs_samefile(std::string_view Path1, std::string_view Path2)
{
#ifdef __unix__
   struct stat64 stat1, stat2;

   if ((!stat64(Path1.data(), &stat1)) and (!stat64(Path2.data(), &stat2))) {
      if ((stat1.st_ino IS stat2.st_ino)
            and (stat1.st_dev IS stat2.st_dev)
            and (stat1.st_mode IS stat2.st_mode)
            and (stat1.st_uid IS stat2.st_uid)
            and (stat1.st_gid IS stat2.st_gid)) {
         return ERR::True;
      }
      else return ERR::False;
   }
   else return ERR::False;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-FUNCTION-
ResolveGroupID: Converts a group ID to its corresponding name.

This function converts group ID's obtained from the file system into their corresponding names.  If the `Group` ID is
invalid then `NULL` will be returned.

-INPUT-
int Group: The group ID.

-RESULT-
cstr: The group name is returned, or `NULL` if the ID cannot be resolved.

*********************************************************************************************************************/

CSTRING ResolveGroupID(int GroupID)
{
#ifdef __unix__

   static thread_local char group[40];

   if (auto info = getgrgid(GroupID)) {
      int i;
      for (i=0; (info->gr_name[i]) and ((size_t)i < sizeof(group)-1); i++) group[i] = info->gr_name[i];
      group[i] = 0;
      return group;
   }
   else return nullptr;

#else

   return nullptr;

#endif
}

/*********************************************************************************************************************

-FUNCTION-
ResolveUserID: Converts a user ID to its corresponding name.

This function converts user ID's obtained from the file system into their corresponding names.  If the `User` ID is
invalid then `NULL` will be returned.

-INPUT-
int User: The user ID.

-RESULT-
cstr: The user name is returned, or `NULL` if the ID cannot be resolved.

*********************************************************************************************************************/

CSTRING ResolveUserID(int UserID)
{
#ifdef __unix__

   static thread_local char user[40];

   if (auto info = getpwuid(UserID)) {
      int i;
      for (i=0; (info->pw_name[i]) and ((size_t)i < sizeof(user)-1); i++) user[i] = info->pw_name[i];
      user[i] = 0;
      return user;
   }
   else return nullptr;

#else

   return nullptr;

#endif
}

/*********************************************************************************************************************

-FUNCTION-
CopyFile: Makes copies of folders and files.

This function is used to copy files and folders to new locations.  When copying folders it will do so
recursively, so as to copy all sub-folders and files within the location.

It is important that you are aware that different types of string formatting can give different results.  The following
examples illustrate:

Copying `kotuku:makefile` to `kotuku:documents` results in a file called `kotuku:documents`.

Copying `kotuku:makefile` to `kotuku:documents/` results in a file called `kotuku:documents/makefile`.

Copying `kotuku:pictures/` to `kotuku:documents/` results in a folder at `kotuku:documents/pictures` and includes
a copy of all folders and files found within the pictures folder.

Copying `kotuku:pictures/` to `kotuku:documents` results in a folder at `kotuku:documents` (if the documents folder
already exists, it receives additional content from the pictures folder).

This function will overwrite any destination file(s) that already exist.

The Source parameter should always clarify the type of location that is being copied.  For example if copying a
folder, a forward slash must terminate the string or it will be assumed that a file is the source.

The Callback parameter can be set with a function that matches this prototype:

`INT Callback(FileFeedback *)`

For each file that is processed during the copy operation, a &FileFeedback structure is passed that describes the
source file and its target.  The callback must return a constant value that can potentially affect file processing.
Valid values are `FFR::Okay` (copy the file), `FFR::Skip` (do not copy the file) and `FFR::Abort` (abort the process
completely and return `ERR::Cancelled` as an error code).

-INPUT-
cstr Source: The source location.
cstr Dest:   The destination location.
ptr(func) Callback: Optional callback for receiving feedback during the operation.

-ERRORS-
Okay: The source was copied to its destination successfully.
Args:
Failed: A failure occurred during the copy process.

*********************************************************************************************************************/

ERR CopyFile(CSTRING Source, CSTRING Dest, FUNCTION *Callback)
{
   return fs_copy(Source, Dest, Callback, FALSE);
}

/*********************************************************************************************************************

-FUNCTION-
CreateLink: Creates symbolic links on Unix file systems.

Use the CreateLink() function to create symbolic links on Unix file systems. The link connects a new file created at
`From` to an existing file referenced at `To`. The `To` link is allowed to be relative to the `From` location - for instance,
you can link `documents:myfiles/newlink.txt` to `../readme.txt` or `folder/readme.txt`. The `..` path component must be
used when making references to parent folders.

The permission flags for the link are inherited from the file that you are linking to.  If the file location referenced
at `From` already exists as a file or folder, the function will fail with an `ERR::FileExists` error code.

This function does not automatically create folders in circumstances where new folders are required to complete the
`From` link.  You will need to call ~CreateFolder() to ensure that the necessary paths exist beforehand.  If the
file referenced at To does not exist, the link will be created without error, but any attempts to open the link will
fail until the target file or folder exists.

-INPUT-
cstr From: The symbolic link will be created at the location specified here.
cstr To:   The file that you are linking to is specified here.

-ERRORS-
Okay: The link was created successfully.
NullArgs:
NoSupport: The file system or the host operating system does not support symbolic links.
NoPermission: The user does not have permission to create the link, or the file system is mounted read-only.
ResolvePath:
LowCapacity: There is no room on the device to create the new link.
Memory:
BufferOverflow: One or both of the provided arguments is too long.
FileExists: The location referenced at From already exists.

*********************************************************************************************************************/

ERR CreateLink(CSTRING From, CSTRING To)
{
#ifdef _WIN32

   return ERR::NoSupport;

#else

   pf::Log log(__FUNCTION__);

   if ((!From) or (!To)) return ERR::NullArgs;

   log.branch("From: %.40s, To: %s", From, To);

   std::string src, dest;
   if (ResolvePath(From, RSF::NO_FILE_CHECK, &src) IS ERR::Okay) {
      if (ResolvePath(To, RSF::NO_FILE_CHECK, &dest) IS ERR::Okay) {
         auto err = symlink(dest.c_str(), src.c_str());

         if (!err) return ERR::Okay;
         else return convert_errno(err, ERR::SystemCall);
      }
      else return ERR::ResolvePath;
   }
   else return ERR::ResolvePath;

#endif
}

/*********************************************************************************************************************

-FUNCTION-
DeleteFile: Deletes files and folders.

This function will delete a file or folder when given a valid file location.  The current user must have delete access
to the given file. When deleting folders, all content will be scanned and deleted recursively. Individual deletion
failures are ignored, although an error will be returned if the top-level folder still contains content on its deletion.

This function does not allow for the approximation of file names.  To approximate a file location, open it as a @File
object or use ~ResolvePath() first.

The `Callback` parameter can be set with a function that matches the prototype `INT Callback(*FileFeedback)`.

Prior to the deletion of any file, a !FileFeedback structure is passed that describes the file's location.  The
callback must return a constant value that can potentially affect file processing.  Valid values are `FFR::Okay` (delete
the file), `FFR::Skip` (do not delete the file) and `FFR::Abort` (abort the process completely and return `ERR::Cancelled`
as an error code).

-INPUT-
cstr Path: String referring to the file or folder to be deleted.  Folders must be denoted with a trailing slash.
ptr(func) Callback: Optional callback for receiving feedback during the operation.

-ERRORS-
Okay: The file or folder was deleted successfully.
NullArgs:
FileNotFound:
File: The location could not be opened for deletion.
NoSupport: The filesystem driver does not support deletion.

*********************************************************************************************************************/

ERR DeleteFile(CSTRING Path, FUNCTION *Callback)
{
   pf::Log log(__FUNCTION__);

   if (!Path) return ERR::NullArgs;

   log.branch("%s", Path);

   auto len = strlen(Path);
   if (Path[len-1] IS ':') return DeleteVolume(Path);

   std::string resolve;
   if (ResolvePath(Path, RSF::NIL, &resolve) IS ERR::Okay) {
      const virtual_drive *vd = get_fs(resolve);
      if (vd->Delete) return vd->Delete(resolve, nullptr);
      else return ERR::NoSupport;
   }
   else return ERR::ResolvePath;
}

/*********************************************************************************************************************

-FUNCTION-
SetDefaultPermissions: Forces the user and group permissions to be applied to new files and folders.

By default, user, group and permission information for new files is inherited either from the system defaults or from
the file source in copy operations.  Use this function to override this behaviour with new default values.  All
threads of the process will be affected.

To revert behaviour to the default settings, set the `User` and/or `Group` values to `-1` and the `Permissions` value to zero.

-INPUT-
int User: User ID to apply to new files.
int Group: Group ID to apply to new files.
int(PERMIT) Permissions: Permission flags to be applied to new files.
-END-

*********************************************************************************************************************/

void SetDefaultPermissions(int User, int Group, PERMIT Permissions)
{
   pf::Log log(__FUNCTION__);

   glForceUID = User;
   glForceGID = Group;

   if (Permissions IS PERMIT(-1)) { // Prevent improper permission settings
      log.warning(ERR::Args);
      Permissions = PERMIT::NIL;
   }

   glDefaultPermissions = Permissions;
}

//********************************************************************************************************************
// Internal function for getting information from files, particularly virtual volumes.  If you know that a path
// refers directly to the client's filesystem then you can revert to calling fs_getinfo() instead.

static thread_local char glNameBuffer[MAX_FILENAME]; // Not thread-safe

ERR get_file_info(std::string_view Path, FileInfo *Info, int InfoSize)
{
   pf::Log log(__FUNCTION__);
   int i;
   ERR error;

   if (Path.empty() or (!Info) or (InfoSize <= 0)) return log.warning(ERR::Args);

   clearmem(Info, InfoSize);
   Info->Name = glNameBuffer;

   // Check if the location is a volume with no file reference

   if (Path.ends_with(':')) {
      const virtual_drive *vfs = get_fs(Path);

      Info->Flags = RDF::VOLUME;

      for (i=0; (i < MAX_FILENAME-1) and (i < std::ssize(Path)) and (Path[i] != ':'); i++) glNameBuffer[i] = Path[i];
      int pos = i;
      glNameBuffer[i] = 0;

      error = ERR::Okay;

      if (auto lock = std::unique_lock{glmVolumes, 4s}) {
         if (glVolumes.contains(glNameBuffer)) {
            if (glVolumes[glNameBuffer]["Hidden"] == "Yes") Info->Flags |= RDF::HIDDEN;
         }
      }
      else error = ERR::LockFailed;

      if (pos < MAX_FILENAME-2) {
         glNameBuffer[pos++] = ':';
         glNameBuffer[pos] = 0;

         if (vfs->is_virtual()) {
            Info->Flags |= RDF::VIRTUAL;
            if (vfs->GetInfo) error = vfs->GetInfo(Path, Info, InfoSize);
         }

         return error;
      }
      else return log.warning(ERR::BufferOverflow);
   }

   log.traceBranch("%s", Path.data());

   std::string path;
   if ((error = ResolvePath(Path, RSF::NIL, &path)) IS ERR::Okay) {
      auto vfs = get_fs(path);

      if (vfs->GetInfo) {
         if (vfs->is_virtual()) Info->Flags |= RDF::VIRTUAL;

         if ((error = vfs->GetInfo(path, Info, InfoSize)) IS ERR::Okay) {
            Info->TimeStamp = calc_timestamp(&Info->Modified);
         }
      }
      else log.warning(ERR::NoSupport);
   }

   return error;
}

/*********************************************************************************************************************

-FUNCTION-
LoadFile: Loads files into a local cache for fast file processing.

The LoadFile() function loads complete files into memory and caches the content for use by other areas of the system
or application.

This function will first determine if the requested file has already been cached.  If this is true then the !CacheFile
structure is returned immediately.  Note that if the file was previously cached but then modified, this will be treated
as a cache miss and the file will be loaded into a new buffer.

File content will be loaded into a readable memory buffer that is referenced by the Data field of the
!CacheFile structure.  A hidden null byte is appended at the end of the buffer to assist the processing of text files.
Other pieces of information about the file can be derived from the !CacheFile meta data.

Calls to LoadFile() must be matched with a call to ~UnloadFile() to decrement the cache counter. When the counter
returns to zero, the file can be unloaded from the cache during the next resource collection phase.

-INPUT-
cstr Path: The location of the file to be cached.
int(LDF) Flags: Optional flags are specified here.
&resource(CacheFile) Cache: A pointer to a !CacheFile structure is returned here if successful.

-ERRORS-
Okay: The file was cached successfully.
NullArgs:
AllocMemory:
Search: If `CHECK_EXISTS` is specified, this failure indicates that the file is not cached.
-END-

*********************************************************************************************************************/

ERR LoadFile(CSTRING Path, LDF Flags, CacheFile **Cache)
{
   pf::Log log(__FUNCTION__);

   if ((!Path) or (!Cache)) return ERR::NullArgs;

   // Check if the file is already cached.  If it is, check that the file hasn't been written since the last time it was cached.

   std::string path;
   ERR error;
   if ((error = ResolvePath(Path, RSF::APPROXIMATE, &path)) != ERR::Okay) return error;

   const std::lock_guard<std::mutex> lock(glCacheLock);

   log.branch("%.80s, Flags: $%.8x", path.c_str(), int(Flags));

   auto file = objFile::create { fl::Path(path), fl::Flags(FL::READ|FL::FILE) };

   if (file.ok()) {
      auto file_size = file->get<int64_t>(FID_Size);
      auto timestamp = file->get<int64_t>(FID_TimeStamp);

      CacheFileIndex index(path, timestamp, file_size);

      if (glCache.contains(index)) {

         *((extCacheFile **)Cache) = &glCache[index];
         if ((Flags & LDF::CHECK_EXISTS) IS LDF::NIL) glCache[index].Locks++;
         return ERR::Okay;
      }

      // If the client just wanted to check for the existence of the file, do not proceed in loading it.

      if ((Flags & LDF::CHECK_EXISTS) != LDF::NIL) {
         return ERR::Search;
      }

      glCache.emplace(index, extCacheFile(path, file_size, timestamp));

      if (file_size) {
         int result;
         error = file->read(glCache[index].Data, file_size, &result);
         if ((error IS ERR::Okay) and (file_size != result)) return ERR::Read;
      }

      if (error IS ERR::Okay) {
         *((extCacheFile **)Cache) = &glCache[index];

         if (!glCacheTimer) {
            pf::SwitchContext context(CurrentTask());
            auto call = C_FUNCTION(check_cache);
            SubscribeTimer(60, &call, &glCacheTimer);
         }

         return ERR::Okay;
      }
      else return error;
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-FUNCTION-
CreateFolder: Makes new folders.

This function creates new folders.  You are required to specify the full path of the new folder.  Standard
permission flags can be passed to determine the new permissions to set against the newly created Dir(s).  If no
permission flags are passed, only the current user will have access to the new folder (assuming that the file system
supports security settings on the given media).  This function will create multiple folders if the complete path
does not exist at the time of the call.

On Unix systems you can define the owner and group ID's for the new folder by calling the
~SetDefaultPermissions() function prior to CreateFolder().

-INPUT-
cstr Path: The location of the folder.
int(PERMIT) Permissions: Security permissions to apply to the created Dir(s).  Set to `NULL` if only the current user should have access.

-ERRORS-
Okay:
NullArgs:
FileExists: An identically named file or folder already exists at the `Path`.
NoSupport:  Virtual file system does not support folder creation.
Failed:

*********************************************************************************************************************/

ERR CreateFolder(CSTRING Path, PERMIT Permissions)
{
   pf::Log log(__FUNCTION__);

   if ((!Path) or (!*Path)) return log.warning(ERR::NullArgs);

   if (glDefaultPermissions != PERMIT::NIL) Permissions = glDefaultPermissions;
   else if ((Permissions IS PERMIT::NIL) or ((Permissions & PERMIT::INHERIT) != PERMIT::NIL)) {
      Permissions |= get_parent_permissions(Path, nullptr, nullptr);
      if (Permissions IS PERMIT::NIL) Permissions = PERMIT::READ|PERMIT::WRITE|PERMIT::EXEC|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE|PERMIT::GROUP_EXEC; // If no permissions are set, give current user full access
   }

   std::string resolve;
   if (ResolvePath(Path, RSF::NO_FILE_CHECK, &resolve) IS ERR::Okay) {
      const virtual_drive *vd = get_fs(resolve);
      if (vd->CreateFolder) return vd->CreateFolder(resolve, Permissions);
      else return ERR::NoSupport;
   }
   else return ERR::ResolvePath;
}

/*********************************************************************************************************************

-FUNCTION-
MoveFile: Moves folders and files to new locations.

This function is used to move files and folders to new locations.  It can also be used for renaming purposes and is
able to move data from one type of media to another.  When moving folders, any contents within the folder will
also be moved across to the new location.

It is important that you are aware that different types of string formatting can give different results.  The
following examples illustrate:

<pre>
<b>Source               Destination          Result</b>
kotuku:makefile     kotuku:documents    kotuku:documents
kotuku:makefile     kotuku:documents/   kotuku:documents/makefile
kotuku:pictures/    kotuku:documents/   kotuku:documents/pictures
kotuku:pictures/    kotuku:documents    kotuku:documents (Existing documents folder destroyed)
</>

This function will overwrite the destination location if it already exists.

The `Source` argument should always clarify the type of location that is being copied - e.g. if you are copying a
folder, you must specify a forward slash at the end of the string or the function will assume that you are moving a
file.

The `Callback` parameter can be set with a function that matches this prototype:

`INT Callback(*FileFeedback)`

For each file that is processed during the move operation, a !FileFeedback structure is passed that describes the
source file and its target.  The callback must return a constant value that can potentially affect file processing.
Valid values are `FFR::Okay` (move the file), `FFR::Skip` (do not move the file) and `FFR::Abort` (abort the process
completely and return `ERR::Cancelled` as an error code).

-INPUT-
cstr Source: The source path.
cstr Dest:   The destination path.
ptr(func) Callback: Optional callback for receiving feedback during the operation.

-ERRORS-
Okay
NullArgs
Failed

*********************************************************************************************************************/

ERR MoveFile(CSTRING Source, CSTRING Dest, FUNCTION *Callback)
{
   pf::Log log(__FUNCTION__);

   if ((!Source) or (!Dest)) return ERR::NullArgs;

   log.branch("%s to %s", Source, Dest);
   return fs_copy(Source, Dest, Callback, TRUE);
}

/*********************************************************************************************************************

-FUNCTION-
ReadFileToBuffer: Reads a file into a buffer.

This function provides a simple method for reading file content into a `Buffer`.  In some cases this procedure may be
optimised for the host platform, which makes it the fastest way to read file content in simple cases.

File path approximation is supported if the `Path` is prefixed with a `~` character (e.g. `~pictures:photo` could be
matched to `photo.jpg` in the same folder).

-INPUT-
cstr Path: The path of the file.
buf(ptr) Buffer: Pointer to a buffer that will receive the file content.
bufsize BufferSize: The byte size of the `Buffer`.
&int Result: The total number of bytes read into the `Buffer` will be returned here (optional).

-ERRORS-
Okay
Args
NullArgs
OpenFile
InvalidPath
Read
File
-END-

*********************************************************************************************************************/

ERR ReadFileToBuffer(CSTRING Path, APTR Buffer, int BufferSize, int *BytesRead)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("Path: %s, Buffer Size: %d", Path, BufferSize);

#if defined(__unix__) || defined(_WIN32)
   if ((!Path) or (BufferSize <= 0) or (!Buffer)) return ERR::Args;

   bool approx;
   if (*Path IS '~') {
      Path++;
      approx = true;
   }
   else approx = false;

   if (BytesRead) *BytesRead = 0;

   std::string res_path;
   if (auto error = ResolvePath(Path, RSF::CHECK_VIRTUAL | (approx ? RSF::APPROXIMATE : RSF::NIL), &res_path); error IS ERR::Okay) {
      if (res_path.starts_with("/dev/")) return ERR::InvalidPath;
      else if (auto handle = open(res_path.c_str(), O_RDONLY|O_NONBLOCK|O_LARGEFILE|WIN32OPEN, nullptr); handle != -1) {
         if (auto result = read(handle, Buffer, BufferSize); result IS -1) {
            close(handle);
            return ERR::Read;
         }
         else if (BytesRead) *BytesRead = result;

         close(handle);
         return ERR::Okay;
      }
      else return ERR::OpenFile;
   }
   else if (error IS ERR::VirtualVolume) {
      extFile::create file = { fl::Path(res_path), fl::Flags(FL::READ|FL::FILE|(approx ? FL::APPROXIMATE : FL::NIL)) };

      if (file.ok()) return file->read(Buffer, BufferSize, BytesRead);
      else return ERR::File;
   }
   else return ERR::FileNotFound;

#else

   extFile::create file = { fl::Path(Path), fl::Flags(FL::READ|FL::FILE|(approx ? FL::APPROXIMATE : FL::NIL)) };

   if (file.ok()) {
      int result;
      if (!file->read(Buffer, BufferSize, &result)) {
         if (BytesRead) *BytesRead = result;
         return ERR::Okay;
      }
      else return ERR::Read;
   }
   else return ERR::File;

#endif
}

/*********************************************************************************************************************

-FUNCTION-
ReadInfoTag: Read a named tag from a !FileInfo structure.

ReadInfoTag() will read the value of a named tag in a !FileInfo structure.  The tag must have been added with
~AddInfoTag() or `ERR::NotFound` will be returned.

-INPUT-
struct(FileInfo) Info: Pointer to a valid !FileInfo structure.
cstr Name: The name of the tag, which must be declared in camel-case as tags are case-sensitive.
&cstr Value: The discovered string value is returned here if found.

-ERRORS-
Okay:
NullArgs:
NotFound:

*********************************************************************************************************************/

ERR ReadInfoTag(FileInfo *Info, CSTRING Name, CSTRING *Value)
{
   if ((!Info) or (!Name) or (!Value)) {
      pf::Log log(__FUNCTION__);
      return ERR::NullArgs;
   }

   if ((Info->Tags) and (Info->Tags->contains(Name))) {
      *Value = Info->Tags[0][Name].c_str();
      return ERR::Okay;
   }
   else *Value = nullptr;

   return ERR::NotFound;
}

//********************************************************************************************************************
// The Path passed to this function must be a completely resolved path.

static ERR test_path(std::string &Path, RSF Flags)
{
   pf::Log log(__FUNCTION__);

   log.trace("%s", Path.c_str());

   if (auto vd = get_virtual(Path)) {
      if (vd->TestPath) {
         LOC type;
         if (vd->TestPath(Path, Flags, &type) IS ERR::Okay) {
            return ERR::Okay;
         }
         else return ERR::FileNotFound;
      }
      else return ERR::Okay;
   }

#ifdef _WIN32
   // Convert forward slashes to back slashes
   for (int j=0; j < std::ssize(Path); j++) if (Path[j] IS '/') Path[j] = '\\';
#endif

   if (Path.ends_with('/') or Path.ends_with('\\')) {
      // This code handles testing for folder locations

      #ifdef __unix__

         if (Path.size() IS 1) return ERR::Okay; // Do not lstat() the root '/' folder

         Path.pop_back();
         struct stat64 info;
         auto result = lstat64(Path.c_str(), &info);
         Path.push_back('/');

         if (!result) return ERR::Okay;

      #elif _WIN32

         if (winCheckDirectoryExists(Path.c_str())) return ERR::Okay;
         else log.trace("Folder does not exist.");

      #else
         #error Require folder testing code for this platform.
      #endif
   }
   else {
      // This code handles testing for file locations

      #ifdef __unix__
      struct stat64 info;
      #endif

      if ((Flags & RSF::APPROXIMATE) != RSF::NIL) {
         if (findfile(Path) IS ERR::Okay) return ERR::Okay;
      }
      #ifdef __unix__
      else if (!lstat64(Path.c_str(), &info)) {
         if (S_ISDIR(info.st_mode)) Path.append("/");
         return ERR::Okay;
      }
      #else
      else if (!access(Path.c_str(), 0)) return ERR::Okay;
      //else log.trace("access() failed.");
      #endif
   }

   return ERR::FileNotFound;
}

/*********************************************************************************************************************

-FUNCTION-
UnloadFile: Unloads files from the file cache.

This function unloads cached files that have been previously loaded with the ~LoadFile() function.

-INPUT-
resource(CacheFile) Cache: A pointer to a !CacheFile structure returned from ~LoadFile().
-END-

*********************************************************************************************************************/

void UnloadFile(CacheFile *Cache)
{
   if (!Cache) return;

   pf::Log log(__FUNCTION__);

   log.function("%.80s, Locks: %d", Cache->Path, ((extCacheFile *)Cache)->Locks);

   const std::lock_guard<std::mutex> lock(glCacheLock);

   if (((extCacheFile*)Cache)->Locks > 0) ((extCacheFile*)Cache)->Locks--;

   // Cache entries are never removed here because this determination is handled by check_cache().
}

//********************************************************************************************************************
// NOTE: The argument passed as the folder must be a large buffer to compensate for the resulting filename.

#ifdef __unix__

struct olddirent {
   long d_ino;                 // inode number
   off_t d_off;                // offset to next dirent
   unsigned short d_reclen;    // length of this dirent
   char d_name[];              // file name (null-terminated)
};

ERR findfile(std::string &Path)
{
   pf::Log log("FindFile");

   if (Path.empty() or Path.starts_with(':')) return ERR::Args;

   // Return if the file exists at the specified Path and is not a folder

   struct stat64 info;
   if (lstat64(Path.c_str(), &info) != -1) {
      if (!S_ISDIR(info.st_mode)) return ERR::Okay;
   }

   auto len = Path.find_last_of(":/\\");
   auto folder = (len IS std::string::npos) ? "" : Path.substr(0, len);
   auto name = (len IS std::string::npos) ? std::string_view(Path) : std::string_view(Path.c_str() + len + 1);

   // Scan the files at the Path to find a similar filename (ignore the filename extension).

   log.trace("Scanning Path %s", folder.c_str());

   if (auto dir = opendir(folder.c_str())) {
      rewinddir(dir);

      struct dirent *entry;
      while ((entry = readdir(dir))) {
         if (entry->d_name[0] IS '.') {
            if (entry->d_name[1] IS 0) continue;
            if ((entry->d_name[1] IS '.') and (entry->d_name[2] IS 0)) continue;
         }

         std::string_view filename(entry->d_name);
         auto dot = filename.find_last_of(".");
         if (dot != std::string::npos) filename.remove_suffix(filename.size() - dot);

         if (iequals(name, filename)) {
            Path.resize(folder.size());
            if (!Path.ends_with('/')) Path.append("/");
            Path.append(entry->d_name);

            // If it turns out that the Path is a folder, ignore it

            if (auto dummydir = opendir(Path.c_str())) {
               closedir(dummydir);
               continue;
            }

            closedir(dir);
            return ERR::Okay;
         }
      }

      closedir(dir);
   }

   return ERR::Search;
}

#elif _WIN32

ERR findfile(std::string &Path)
{
   if (Path.empty() or Path.starts_with(':')) return ERR::Args;

   // Find a file with the standard Path

   if (auto filehandle = open(Path.c_str(), O_RDONLY|O_LARGEFILE|WIN32OPEN, nullptr); filehandle != -1) {
      close(filehandle);
      return ERR::Okay;
   }

   // Find a file with an extension

   Path.append(".*");

   char buffer[130];
   APTR handle = nullptr;
   if ((handle = winFindFile(Path.data(), &handle, buffer))) {
      auto len = Path.find_last_of(":/\\");
      if (len IS std::string::npos) len = 0;
      Path.resize(len + 1);
      Path.append(buffer);
      winFindClose(handle);
      return ERR::Okay;
   }

   return ERR::Search;
}

#endif

//********************************************************************************************************************

int convert_permissions(PERMIT Permissions)
{
   int flags = 0;

#ifdef __unix__
   if ((Permissions & PERMIT::READ) != PERMIT::NIL)         flags |= S_IRUSR;
   if ((Permissions & PERMIT::WRITE) != PERMIT::NIL)        flags |= S_IWUSR;
   if ((Permissions & PERMIT::EXEC) != PERMIT::NIL)         flags |= S_IXUSR;

   if ((Permissions & PERMIT::GROUP_READ) != PERMIT::NIL)   flags |= S_IRGRP;
   if ((Permissions & PERMIT::GROUP_WRITE) != PERMIT::NIL)  flags |= S_IWGRP;
   if ((Permissions & PERMIT::GROUP_EXEC) != PERMIT::NIL)   flags |= S_IXGRP;

   if ((Permissions & PERMIT::OTHERS_READ) != PERMIT::NIL)  flags |= S_IROTH;
   if ((Permissions & PERMIT::OTHERS_WRITE) != PERMIT::NIL) flags |= S_IWOTH;
   if ((Permissions & PERMIT::OTHERS_EXEC) != PERMIT::NIL)  flags |= S_IXOTH;

   if ((Permissions & PERMIT::USERID) != PERMIT::NIL)       flags |= S_ISUID;
   if ((Permissions & PERMIT::GROUPID) != PERMIT::NIL)      flags |= S_ISGID;
#else
   if ((Permissions & PERMIT::ALL_READ) != PERMIT::NIL)     flags |= S_IREAD;
   if ((Permissions & PERMIT::ALL_WRITE) != PERMIT::NIL)    flags |= S_IWRITE;
   if ((Permissions & PERMIT::ALL_EXEC) != PERMIT::NIL)     flags |= S_IEXEC;
#endif

   return flags;
}

//********************************************************************************************************************

PERMIT convert_fs_permissions(int Permissions)
{
   PERMIT flags = PERMIT::NIL;

#ifdef __unix__
   if (Permissions & S_IRUSR) flags |= PERMIT::READ;
   if (Permissions & S_IWUSR) flags |= PERMIT::WRITE;
   if (Permissions & S_IXUSR) flags |= PERMIT::EXEC;

   if (Permissions & S_IRGRP) flags |= PERMIT::GROUP_READ;
   if (Permissions & S_IWGRP) flags |= PERMIT::GROUP_WRITE;
   if (Permissions & S_IXGRP) flags |= PERMIT::GROUP_EXEC;

   if (Permissions & S_IROTH) flags |= PERMIT::OTHERS_READ;
   if (Permissions & S_IWOTH) flags |= PERMIT::OTHERS_WRITE;
   if (Permissions & S_IXOTH) flags |= PERMIT::OTHERS_EXEC;

   if (Permissions & S_ISGID) flags |= PERMIT::GROUPID;
   if (Permissions & S_ISUID) flags |= PERMIT::USERID;
#else
   if (Permissions & S_IREAD)  flags |= PERMIT::READ;
   if (Permissions & S_IWRITE) flags |= PERMIT::WRITE;
   if (Permissions & S_IEXEC)  flags |= PERMIT::EXEC;
#endif
   return flags;
}

//********************************************************************************************************************
// Strips the filename and calls CreateFolder() to create all paths leading up to the filename.

ERR check_paths(CSTRING Path, PERMIT Permissions)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch("%s", Path);

   std::string path(Path);
   if (auto i = path.find_last_of(":/\\"); i != std::string::npos) {
      path.resize(i);
      return CreateFolder(path.c_str(), Permissions);
   }
   else return ERR::InvalidPath;
}

//********************************************************************************************************************
// This low level function is used for copying/moving/renaming files and folders.

ERR fs_copy(std::string_view Source, std::string_view Dest, FUNCTION *Callback, bool Move)
{
   pf::Log log(Move ? "MoveFile" : "CopyFile");
#ifdef __unix__
   int gid, uid;
#endif
   int permissions;
   ERR error;

   if ((Source.empty()) or (Dest.empty())) return log.warning(ERR::NullArgs);

   log.traceBranch("\"%s\" to \"%s\"", Source.data(), Dest.data());

   std::string src, dest;
   if ((error = ResolvePath(Source, RSF::NIL, &src)) != ERR::Okay) return ERR::FileNotFound;
   if ((error = ResolvePath(Dest, RSF::NO_FILE_CHECK, &dest)) != ERR::Okay) return ERR::ResolvePath;

   const virtual_drive *srcvirtual  = get_fs(src);
   const virtual_drive *destvirtual = get_fs(dest);

   bool srcdir = (src.ends_with('/') or src.ends_with('\\'));

   // If the destination is a folder, we need to copy the name of the source to create the new file or dir.

   if (dest.ends_with('/') or dest.ends_with('\\') or dest.ends_with(':')) {
      if (src.ends_with('/') or src.ends_with('\\') or src.ends_with(':')) src.pop_back();
      auto len = src.find_last_of("/\\:");
      dest.append(src, len, std::string::npos);
   }

   log.trace("Copy: %s TO %s", src.c_str(), dest.c_str());

   if (CompareResolvedPaths(src, dest) IS ERR::Okay) {
      log.trace("The source and destination refer to the same location.");
      if (Move) return ERR::IdenticalPaths; // Move fails if source and dest are identical, since the source is not deleted
      else return ERR::Okay; // Copy succeeds if source and dest are identical
   }

   FileFeedback feedback;
   if (Move) feedback.FeedbackID = FBK::MOVE_FILE;
   else feedback.FeedbackID = FBK::COPY_FILE;

   feedback.Path = src.data();
   feedback.Dest = dest.data();

   if (srcvirtual->is_virtual() or destvirtual->is_virtual()) {
      log.trace("Using virtual copy routine.");

      // Open the source and destination

      extFile::create srcfile = { fl::Path(Source.data()), fl::Flags(FL::READ) };

      if (srcfile.ok()) {
         if ((Move) and (srcvirtual IS destvirtual)) {
            // If the source and destination use the same virtual volume, execute the move method.
            fl::Move args = { Dest.data(), nullptr };
            return Action(fl::Move::id, *srcfile, &args);
         }
      }
      else return ERR::FileNotFound;

      extFile::create destfile = {
         fl::Path(Dest.data()),
         fl::Flags(FL::WRITE|FL::NEW),
         fl::Permissions(srcfile->Permissions)
      };

      if (!destfile.ok()) return ERR::CreateFile;

      // Folder copy

      if ((srcfile->Flags & FL::FOLDER) != FL::NIL) {
         if ((destfile->Flags & FL::FOLDER) IS FL::NIL) { // You cannot copy from a folder to a file
            return ERR::Mismatch;
         }

         std::string srcbuffer(src);

         // Check if the copy would cause recursion  - e.g. "/kotuku/system/" to "/kotuku/system/temp/".

         if (src.size() <= dest.size()) {
            if (pf::startswith(src, dest)) {
               log.warning("The copy operation would cause recursion.");
               return ERR::Loop;
            }
         }

         // Create the destination folder, then copy the source folder across using a recursive routine.

         if (glDefaultPermissions != PERMIT::NIL) CreateFolder(dest.c_str(), glDefaultPermissions);
         else CreateFolder(dest.c_str(), PERMIT::USER|PERMIT::GROUP);

         if ((error = fs_copydir(srcbuffer, dest, &feedback, Callback, Move)) IS ERR::Okay) {
            // Delete the source if we are moving folders
            if (Move) return DeleteFile(srcbuffer.c_str(), nullptr);
         }
         else log.warning("Folder copy process failed, error %d.", int(error));

         return error;
      }

      // Standard file copy

      feedback.Position = 0;

      // Use a reasonably small read buffer so that we can provide continuous feedback

      const int bufsize = ((Callback) and (Callback->defined())) ? 65536 : 65536 * 2;

      // This routine is designed to handle streams - where either the source is a stream or the destination is a stream.

      std::vector<int8_t> data(bufsize);
      error = ERR::Okay;
      const int64_t STREAM_TIMEOUT = 10000LL;
      int64_t time = PreciseTime() / 1000LL;
      while (srcfile->Position < srcfile->Size) {
         int len;
         error = srcfile->read(data.data(), bufsize, &len);
         if (error != ERR::Okay) {
            log.warning("acRead() failed: %s", GetErrorMsg(error));
            return error;
         }

         feedback.Position += len;

         if (len) time = PreciseTime() / 1000LL;
         else {
            log.msg("Failed to read any data, position %" PF64 " / %" PF64 ".", (long long)srcfile->Position, (long long)srcfile->Size);

            if ((PreciseTime() / 1000LL) - time > STREAM_TIMEOUT) {
               log.warning("Timeout - stopped reading at offset %" PF64 " of %" PF64 "", (long long)srcfile->Position, (long long)srcfile->Size);
               return ERR::TimeOut;
            }
         }

         // Write the data

         while (len > 0) {
            int result;
            if (acWrite(*destfile, data.data(), len, &result) != ERR::Okay) return ERR::Write;

            if (result) time = (PreciseTime() / 1000LL);
            else if ((PreciseTime() / 1000LL) - time > STREAM_TIMEOUT) {
               log.warning("Timeout - failed to write remaining %d bytes.", len);
               return ERR::TimeOut;
            }

            len -= result;
            if ((destfile->Flags & FL::STREAM) != FL::NIL) {

            }
            else if (len > 0) {
               log.warning("Out of space - wrote %d bytes, %d left.", result, len);
               return ERR::OutOfSpace;
            }

            if (len > 0) ProcessMessages(PMF::NIL, 0);
         } // while()

         if ((Callback) and (Callback->defined())) {
            if (feedback.Size < feedback.Position) feedback.Size = feedback.Position;
            FFR result = CALL_FEEDBACK(Callback, &feedback);
            if (result IS FFR::ABORT) return ERR::Cancelled;
            else if (result IS FFR::SKIP) break;
         }

         ProcessMessages(PMF::NIL, 0);
      } // while()

      if ((Move) and (error IS ERR::Okay)) Action(fl::Delete::id, *srcfile, nullptr);

      return error;
   }

#ifdef __unix__
   // This code manages symbolic links

   struct stat64 stinfo;
   int result;
   if (srcdir) {
      src.pop_back();
      result = lstat64(src.c_str(), &stinfo);
      src.append("/");
   }
   else result = lstat64(src.c_str(), &stinfo);

   if ((!result) and (S_ISLNK(stinfo.st_mode))) {
      char linkto[512];

      if (srcdir) src.pop_back();

      if (auto i = readlink(src.c_str(), linkto, sizeof(linkto)-1); i != -1) {
         linkto[i] = 0;

         if ((Callback) and (Callback->defined())) {
            FFR result = CALL_FEEDBACK(Callback, &feedback);
            if (result IS FFR::ABORT) return ERR::Cancelled;
            else if (result IS FFR::SKIP) return ERR::Okay;
         }

         unlink(dest.c_str()); // Remove any existing file first

         if (!symlink(linkto, dest.c_str())) return ERR::Okay;
         else {
            // On failure, it may be possible that precursing folders need to be created for the link.  Do this here and then try
            // creating the link a second time.

            check_paths(dest.c_str(), PERMIT::READ|PERMIT::WRITE|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE);

            if (!symlink(linkto, dest.c_str())) error = ERR::Okay;
            else {
               log.warning("Failed to create link \"%s\"", dest.c_str());
               return ERR::CreateFile;
            }
         }
      }
      else {
        log.warning("Failed to read link \"%s\"", src.c_str());
        return ERR::Read;
      }

      if ((Move) and (error IS ERR::Okay)) { // Delete the source
         return DeleteFile(src.c_str(), nullptr);
      }

      return error;
   }

   feedback.Size = stinfo.st_size;
#endif

   if (Move) {
      // Attempt to move the source to the destination using a simple rename operation.  If the rename fails,
      // the files may need to be copied across drives, so we don't abort in the case of failure.

      error = ERR::Okay;

      if ((Callback) and (Callback->defined())) {
         FFR result = CALL_FEEDBACK(Callback, &feedback);
         if (result IS FFR::ABORT) return ERR::Cancelled;
         else if (result IS FFR::SKIP) return ERR::Okay;
      }

#ifdef _WIN32
      if (rename(src.c_str(), dest.c_str())) {
         // failed - drop through to file copy
      }
      else return ERR::Okay;
#else
      if (rename(src.c_str(), dest.c_str()) IS -1) {
         // failed - drop through to file copy
      }
      else {
         int parent_uid, parent_gid;

         // Move successful.  Now assign the user and group id's from the parent folder to the file.

         auto parentpermissions = get_parent_permissions(dest, &parent_uid, &parent_gid) & (~PERMIT::ALL_EXEC);

         gid = -1;
         uid = -1;

         if ((parentpermissions & PERMIT::USERID) != PERMIT::NIL) uid = parent_uid;
         if ((parentpermissions & PERMIT::GROUPID) != PERMIT::NIL) gid = parent_gid;

         if (glForceGID != -1) gid = glForceGID;
         if (glForceUID != -1) uid = glForceUID;

         if ((uid != -1) or (gid != -1)) chown(dest.c_str(), uid, gid);

         return ERR::Okay;
      }
#endif
   }

   if (srcdir) {
      // The source location is expressed as a folder string.  Confirm that the folder exists before continuing.

      #ifdef _WIN32
         if (winCheckDirectoryExists(src.c_str()));
         else return ERR::File;
      #else
         DIR *dirhandle;
         if ((dirhandle = opendir(src.c_str()))) closedir(dirhandle);
         else return ERR::File;
      #endif

      // Check if the copy would cause recursion  - e.g. "/kotuku/system/" to "/kotuku/system/temp/".

      if (src.size() <= dest.size()) {
         if (pf::startswith(src, dest)) {
            log.warning("The requested copy would cause recursion.");
            return ERR::Loop;
         }
      }

      // Create the destination folder, then copy the source folder across using a recursive routine.

      if (glDefaultPermissions != PERMIT::NIL) CreateFolder(dest.c_str(), glDefaultPermissions);
      else {
#ifdef _WIN32
         CreateFolder(dest.c_str(), PERMIT::USER|PERMIT::GROUP);
#else
         if (stat64(src.c_str(), &stinfo) != -1) {
            CreateFolder(dest.c_str(), convert_fs_permissions(stinfo.st_mode));
            chown(dest.c_str(), (glForceUID != -1) ? glForceUID : stinfo.st_uid, (glForceGID != -1) ? glForceGID : stinfo.st_gid);
         }
         else {
            log.warning("stat64() failed for %s", src.c_str());
            CreateFolder(dest.c_str(), PERMIT::USER|PERMIT::GROUP);
         }
#endif
      }

      std::string srcbuffer(src);
      if ((error = fs_copydir(srcbuffer, dest, &feedback, Callback, Move)) IS ERR::Okay) {
         // Delete the source if we are moving folders
         if (Move) return DeleteFile(srcbuffer.c_str(), nullptr);
      }
      else log.warning("Folder copy process failed, error %d.", int(error));

      return error;
   }

   if (!Move) { // (If Move is enabled, we would have already sent feedback during the earlier rename() attempt
      if ((Callback) and (Callback->defined())) {
         FFR result = CALL_FEEDBACK(Callback, &feedback);
         if (result IS FFR::ABORT) return ERR::Cancelled;
         else if (result IS FFR::SKIP) return ERR::Okay;
      }
   }

   if (int handle = open(src.c_str(), O_RDONLY|O_NONBLOCK|WIN32OPEN|O_LARGEFILE, nullptr); handle != -1) {
      auto dc_handle = deferred_call([&handle] { close(handle); });

      // Get permissions of the source file to apply to the destination file

      #ifdef _WIN32
      if (glDefaultPermissions != PERMIT::NIL) {
         if ((glDefaultPermissions & PERMIT::INHERIT) != PERMIT::NIL) {
            //auto parentpermissions = get_parent_permissions(dest, NULL, NULL) & (~PERMIT::ALL_EXEC);
            //permissions = convert_permissions((parentpermissions & (~(PERMIT::USERID|PERMIT::GROUPID))) | glDefaultPermissions);
            permissions = S_IREAD|S_IWRITE;
         }
         else permissions = convert_permissions(glDefaultPermissions);
      }
      else permissions = S_IREAD|S_IWRITE;

      size_t fsize;
      winFileInfo(src.c_str(), &fsize, nullptr, nullptr);
      feedback.Size = fsize;
      #else
      auto parentpermissions = get_parent_permissions(dest, nullptr, nullptr) & (~PERMIT::ALL_EXEC);
      if (glDefaultPermissions != PERMIT::NIL) {
         if ((glDefaultPermissions & PERMIT::INHERIT) != PERMIT::NIL) {
            permissions = convert_permissions((parentpermissions & (~(PERMIT::USERID|PERMIT::GROUPID))) | glDefaultPermissions);
         }
         else permissions = convert_permissions(glDefaultPermissions);
      }
      else {
         if (fstat64(handle, &stinfo) IS -1) permissions = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
         else permissions = stinfo.st_mode;
      }

      feedback.Size = stinfo.st_size;
      #endif

      // Delete any existing destination file first so that we can give it new permissions.
      // This will also help when assessing the amount of free space on the destination device.

      #if defined(__unix__) || defined(_WIN32)
      unlink(dest.c_str());
      #else
      DeleteFile(dest.c_str(), nullptr);
      #endif

      // Check if there is enough room to copy this file to the destination

      objStorageDevice::create device = { fl::Volume(dest) };
      if (device.ok()) {
         if ((device->BytesFree >= 0) and (device->BytesFree - 1024LL <= feedback.Size)) {
            log.warning("Not enough space on device (%" PF64 "/%" PF64 " < %" PF64 ")", (long long)device->BytesFree, (long long)device->DeviceSize, (long long)feedback.Size);
            return ERR::OutOfSpace;
         }
      }

      int dhandle;
      if ((dhandle = open(dest.c_str(), O_WRONLY|O_CREAT|O_TRUNC|O_LARGEFILE|WIN32OPEN, permissions)) IS -1) {
         // If the initial open failed, we may need to create preceding paths
         check_paths(dest.c_str(), convert_fs_permissions(permissions));
         dhandle = open(dest.c_str(), O_WRONLY|O_CREAT|O_TRUNC|O_LARGEFILE|WIN32OPEN, permissions);
      }

#ifdef __unix__
      // Set the owner and group for the file to match the original.  This only works if the user is root.  If the sticky bits are set on the parent folder,
      // do not override the group and user settings (the id's will be inherited from the parent folder instead).

      // fchown() ignores id's when they are set to -1.

      if (glForceGID != -1) gid = glForceGID;
      else gid = stinfo.st_gid;

      if (glForceUID != -1) uid = glForceUID;
      else uid = stinfo.st_uid;

      if ((parentpermissions & PERMIT::GROUPID) != PERMIT::NIL) gid = -1;
      if ((parentpermissions & PERMIT::USERID) != PERMIT::NIL) uid = -1;

      if ((uid != -1) or (gid != -1)) fchown(dhandle, uid, gid);

#endif

      feedback.Position = 0;

      if (dhandle != -1) {
         auto dc_dhandle = deferred_call([&dhandle] { close(dhandle); });

         // Use a reasonably small read buffer so that we can provide continuous feedback

         int bufsize = ((Callback) and (Callback->defined())) ? 65536 : 524288;
         std::vector<int8_t> data(bufsize);
         int len;
         error = ERR::Okay;
         while ((len = read(handle, data.data(), bufsize)) > 0) {
            if (int result = write(dhandle, data.data(), len); result IS -1) {
               if (errno IS ENOSPC) return log.warning(ERR::OutOfSpace);
               else return log.warning(ERR::Write);
            }
            else if (result < len) return log.warning(ERR::OutOfSpace);

            if ((Callback) and (Callback->defined())) {
               feedback.Position += len;
               if (feedback.Size < feedback.Position) feedback.Size = feedback.Position;
               FFR result = CALL_FEEDBACK(Callback, &feedback);
               if (result IS FFR::ABORT) return ERR::Cancelled;
               else if (result IS FFR::SKIP) break;
            }
         }

         if (len IS -1) return log.warning(ERR::Read);

#ifdef __unix__
         // If the sticky bits were set, we need to set them again because Linux sneakily turns off those bits when a
         // file is written (for security reasons).

         if ((error IS ERR::Okay) and (permissions & (S_ISUID|S_ISGID))) {
            fchmod(dhandle, permissions);
         }
#endif
      }
      else return log.warning(ERR::CreateFile);
   }
   else return log.warning(ERR::FileNotFound);

   if ((Move) and (error IS ERR::Okay)) { // Delete the source
      return DeleteFile(src.c_str(), nullptr);
   }
   else return error;
}

//********************************************************************************************************************
// Generic routine for copying folders, intended to be used in conjunction with fs_copy()

ERR fs_copydir(std::string &Source, std::string &Dest, FileFeedback *Feedback, FUNCTION *Callback, int8_t Move)
{
   pf::Log log("copy_file");

   const auto vsrc = get_fs(Source);
   const auto vdest = get_fs(Dest);

   auto src_len = Source.size();
   auto dest_len = Dest.size();

   if ((!Source.ends_with('/')) and (!Source.ends_with('\\')) and (!Source.ends_with(':'))) Source.append("/");
   if ((!Dest.ends_with('/')) and (!Dest.ends_with('\\')) and (!Dest.ends_with(':'))) Dest.append("/");

   DirInfo *dir;
   if (auto error = OpenDir(Source.c_str(), RDF::FILE|RDF::FOLDER|RDF::PERMISSIONS, &dir); error IS ERR::Okay) {
      while ((error = ScanDir(dir)) IS ERR::Okay) {
         FileInfo *file = dir->Info;
         if ((file->Flags & RDF::LINK) != RDF::NIL) {
            if ((vsrc->ReadLink) and (vdest->CreateLink)) {
               Source.append(file->Name);
               Dest.append(file->Name);

               if ((Callback) and (Callback->defined())) {
                  Feedback->Path = Source.data();
                  Feedback->Dest = Dest.data();
                  FFR result = CALL_FEEDBACK(Callback, Feedback);
                  if (result IS FFR::ABORT) { error = ERR::Cancelled; break; }
                  else if (result IS FFR::SKIP) continue;
               }

               STRING link;
               if ((error = vsrc->ReadLink(Source, &link)) IS ERR::Okay) {
                  DeleteFile(Dest.c_str(), nullptr);
                  error = vdest->CreateLink(Dest, link);
               }
            }
            else {
               log.warning("Cannot copy linked file to destination.");
               error = ERR::NoSupport;
            }
         }
         else if ((file->Flags & RDF::FILE) != RDF::NIL) {
            Source.append(file->Name);
            Dest.append(file->Name);

            AdjustLogLevel(1);
               error = fs_copy(Source, Dest, Callback, false);
            AdjustLogLevel(-1);
         }
         else if ((file->Flags & RDF::FOLDER) != RDF::NIL) {
            Dest.append(file->Name);

            if ((Callback) and (Callback->defined())) {
               Feedback->Path = Source.data();
               Feedback->Dest = Dest.data();
               FFR result = CALL_FEEDBACK(Callback, Feedback);
               if (result IS FFR::ABORT) { error = ERR::Cancelled; break; }
               else if (result IS FFR::SKIP) continue;
            }

            AdjustLogLevel(1);
               error = CreateFolder(Dest.c_str(), (glDefaultPermissions != PERMIT::NIL) ? glDefaultPermissions : file->Permissions);
#ifdef __unix__
               if (vdest->is_default()) {
                  chown(Dest.c_str(), (glForceUID != -1) ? glForceUID : file->UserID, (glForceGID != -1) ? glForceGID : file->GroupID);
               }
#endif
               if (error IS ERR::FileExists) error = ERR::Okay;
            AdjustLogLevel(-1);

            // Copy everything under the folder to the destination

            if (error IS ERR::Okay) {
               Source.append(file->Name);
               fs_copydir(Source, Dest, Feedback, Callback, Move);
            }
         }
      }

      FreeResource(dir);

      Source.resize(src_len);
      Dest.resize(dest_len);
      return error;
   }
   else if (error IS ERR::DirEmpty) return ERR::Okay;
   else {
      log.msg("Folder list failed for \"%s\"", Source.c_str());
      return error;
   }
}

//********************************************************************************************************************
// Gets the permissions of the parent folder.  Typically used for permission inheritance. NB: It is often wise to
// remove exec and suid flags returned from this function.

PERMIT get_parent_permissions(std::string_view Path, int *UserID, int *GroupID)
{
   std::string_view folder(Path);
   while (folder.ends_with('/') or folder.ends_with('\\') or folder.ends_with(':')) folder.remove_suffix(1);

   while (!folder.empty()) {
      auto i = folder.find_last_of("/\\:");
      if (i IS std::string::npos) break;
      folder = folder.substr(0, i);

      if (!folder.empty()) {
         FileInfo info;
         if (get_file_info(folder, &info, sizeof(info)) IS ERR::Okay) {
            if (UserID) *UserID = info.UserID;
            if (GroupID) *GroupID = info.GroupID;
            return info.Permissions;
         }
         folder.remove_suffix(1);
      }
   }

   return PERMIT::NIL;
}

//********************************************************************************************************************

ERR fs_readlink(std::string_view Source, STRING *Link)
{
#ifdef __unix__
   char buffer[512];
   if (int i = readlink(Source.data(), buffer, sizeof(buffer)-1); i != -1) {
      buffer[i] = 0;
      *Link = strclone(buffer);
      return ERR::Okay;
   }
   else return ERR::SystemCall;
#else
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************

ERR fs_createlink(std::string_view Target, std::string_view Link)
{
#ifdef __unix__
   if (symlink(Link.data(), Target.data()) IS -1) {
      return convert_errno(errno, ERR::CreateFile);
   }
   else return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************

ERR fs_delete(std::string_view ResolvedPath, FUNCTION *Callback)
{
   if (ResolvedPath.ends_with('/') or ResolvedPath.ends_with('\\')) ResolvedPath.remove_suffix(1);

#ifdef _WIN32
   FileFeedback feedback;
   std::string buffer(ResolvedPath);
   if ((Callback) and (Callback->defined())) feedback.FeedbackID = FBK::DELETE_FILE;
   return delete_tree(buffer, Callback, &feedback);
#else
   if (!unlink(ResolvedPath.data())) { // unlink() works if the folder is empty
      return ERR::Okay;
   }
   else if (errno IS EISDIR) {
      FileFeedback feedback;
      std::string buffer(ResolvedPath);
      if ((Callback) and (Callback->defined())) feedback.FeedbackID = FBK::DELETE_FILE;
      return delete_tree(buffer, Callback, &feedback);
   }
   else return convert_errno(errno, ERR::SystemCall);
#endif
}

//********************************************************************************************************************

ERR fs_scandir(DirInfo *Dir)
{
#ifdef __unix__
   struct dirent *de;
   struct stat64 info, link;
   struct tm *local;
   int j;

   char pathbuf[256];
   int path_end = strcopy(Dir->prvResolvedPath, pathbuf, sizeof(pathbuf));
   if ((size_t)path_end >= sizeof(pathbuf)-12) return(ERR::BufferOverflow);
   if (pathbuf[path_end-1] != '/') pathbuf[path_end++] = '/';

   while ((de = readdir((DIR *)Dir->prvHandle))) {
      if ((de->d_name[0] IS '.') and (de->d_name[1] IS 0)) continue;
      if ((de->d_name[0] IS '.') and (de->d_name[1] IS '.') and (de->d_name[2] IS 0)) continue;

      strcopy(de->d_name, pathbuf + path_end, sizeof(pathbuf) - path_end);

      FileInfo *file = Dir->Info;
      if (!(stat64(pathbuf, &info))) {
         if (S_ISDIR(info.st_mode)) {
            if ((Dir->prvFlags & RDF::FOLDER) IS RDF::NIL) continue;
            file->Flags |= RDF::FOLDER;
         }
         else {
            if ((Dir->prvFlags & RDF::FILE) IS RDF::NIL) continue;
            file->Flags |= RDF::FILE|RDF::SIZE|RDF::DATE|RDF::PERMISSIONS;
         }
      }
      else if (!(lstat64(pathbuf, &info))) {
         if ((Dir->prvFlags & RDF::FILE) IS RDF::NIL) continue;
         file->Flags |= RDF::FILE|RDF::SIZE|RDF::DATE|RDF::PERMISSIONS;
      }
      else continue;

      if (lstat64(pathbuf, &link) != -1) {
         if (S_ISLNK(link.st_mode)) file->Flags |= RDF::LINK;
      }

      j = strcopy(de->d_name, file->Name, MAX_FILENAME);

      if (((file->Flags & RDF::FOLDER) != RDF::NIL) and ((Dir->prvFlags & RDF::QUALIFY) != RDF::NIL)) {
         file->Name[j++] = '/';
         file->Name[j] = 0;
      }

      if ((file->Flags & RDF::FILE) != RDF::NIL) file->Size = info.st_size;
      else file->Size = 0;

      if ((Dir->prvFlags & RDF::PERMISSIONS) != RDF::NIL) {
         if (info.st_mode & S_IRUSR) file->Permissions |= PERMIT::READ;
         if (info.st_mode & S_IWUSR) file->Permissions |= PERMIT::WRITE;
         if (info.st_mode & S_IXUSR) file->Permissions |= PERMIT::EXEC;
         if (info.st_mode & S_IRGRP) file->Permissions |= PERMIT::GROUP_READ;
         if (info.st_mode & S_IWGRP) file->Permissions |= PERMIT::GROUP_WRITE;
         if (info.st_mode & S_IXGRP) file->Permissions |= PERMIT::GROUP_EXEC;
         if (info.st_mode & S_IROTH) file->Permissions |= PERMIT::OTHERS_READ;
         if (info.st_mode & S_IWOTH) file->Permissions |= PERMIT::OTHERS_WRITE;
         if (info.st_mode & S_IXOTH) file->Permissions |= PERMIT::OTHERS_EXEC;
         if (info.st_mode & S_ISUID) file->Permissions |= PERMIT::USERID;
         if (info.st_mode & S_ISGID) file->Permissions |= PERMIT::GROUPID;
         file->UserID = info.st_uid;
         file->GroupID = info.st_gid;
      }

      if ((Dir->prvFlags & RDF::DATE) != RDF::NIL) {
         local = localtime(&info.st_mtime);
         file->Modified.Year   = 1900 + local->tm_year;
         file->Modified.Month  = local->tm_mon + 1;
         file->Modified.Day    = local->tm_mday;
         file->Modified.Hour   = local->tm_hour;
         file->Modified.Minute = local->tm_min;
         file->Modified.Second = local->tm_sec;

         local = localtime(&info.st_ctime);
         file->Created.Year   = 1900 + local->tm_year;
         file->Created.Month  = local->tm_mon + 1;
         file->Created.Day    = local->tm_mday;
         file->Created.Hour   = local->tm_hour;
         file->Created.Minute = local->tm_min;
         file->Created.Second = local->tm_sec;
      }
      return ERR::Okay;
   }

#elif _WIN32

   int8_t dir, hidden, readonly, archive;
   int i;

   while (winScan(&Dir->prvHandle, Dir->prvResolvedPath, Dir->Info->Name, &Dir->Info->Size, &Dir->Info->Created, &Dir->Info->Modified, &dir, &hidden, &readonly, &archive)) {
      if (hidden)   Dir->Info->Flags |= RDF::HIDDEN;
      if (readonly) Dir->Info->Flags |= RDF::READ_ONLY;
      if (archive)  Dir->Info->Flags |= RDF::ARCHIVE;

      if (dir) {
         if ((Dir->prvFlags & RDF::FOLDER) IS RDF::NIL) { Dir->Info->Name[0] = 0; continue; }
         Dir->Info->Flags |= RDF::FOLDER;

         if ((Dir->prvFlags & RDF::QUALIFY) != RDF::NIL) {
            i = strlen(Dir->Info->Name);
            Dir->Info->Name[i++] = '/';
            Dir->Info->Name[i] = 0;
         }
      }
      else {
         if ((Dir->prvFlags & RDF::FILE) IS RDF::NIL) { Dir->Info->Name[0] = 0; continue; }
         Dir->Info->Flags |= RDF::FILE|RDF::SIZE|RDF::DATE;
      }

      return ERR::Okay;
   }

#else
   #error Platform requires support for ScanDir();
#endif

   return ERR::DirEmpty;
}

//********************************************************************************************************************

ERR fs_opendir(DirInfo *Info)
{
   pf::Log log(__FUNCTION__);

   log.trace("Resolve '%.40s'/ '%.40s'", Info->prvPath, Info->prvResolvedPath);

#ifdef __unix__

   if ((Info->prvHandle = opendir(Info->prvResolvedPath))) {
      rewinddir((DIR *)Info->prvHandle);
      return ERR::Okay;
   }
   else return ERR::InvalidPath;

#elif _WIN32

   if (Info->prvResolveLen < MAX_FILENAME-1) {
      STRING str = Info->prvResolvedPath;
      str[Info->prvResolveLen-1] = '*'; // The -1 is because the length includes the null terminator.
      str[Info->prvResolveLen++] = 0;

      Info->prvHandle = (WINHANDLE)-1; // No handle is required for windows until ScanDir() is called.  TODO: See winScan() - we should probably call FindFirstFile() here to ensure that the folder exists and initialise the search.
      return ERR::Okay;
   }
   else return log.warning(ERR::BufferOverflow);

#else
   #error Platform requires support for OpenDir()
#endif
}

//********************************************************************************************************************

ERR fs_closedir(DirInfo *Dir)
{
   pf::Log log(__FUNCTION__);

   log.trace("Dir: %p, VirtualID: %d", Dir, Dir->prvVirtualID);

   if ((!Dir->prvVirtualID) or (Dir->prvVirtualID IS DEFAULT_VIRTUALID)) {
      #ifdef __unix__
         if (Dir->prvHandle) closedir((DIR *)Dir->prvHandle);
      #elif _WIN32
         if ((Dir->prvHandle != (WINHANDLE)-1) and (Dir->prvHandle)) {
            winFindClose(Dir->prvHandle);
         }
      #else
         #error Platform requires support for CloseDir()
      #endif
   }

   if (Dir->Info) {
      if ((Dir->prvFlags & RDF::OPENDIR) != RDF::NIL) {
         // OpenDir() allocates Dir->Info as part of the Dir structure, so no need for a FreeResource(Dir->Info) here.

         if (Dir->Info->Tags) { delete Dir->Info->Tags; Dir->Info->Tags = nullptr; }
      }
      else {
         FileInfo *list = Dir->Info;
         while (list) {
            FileInfo *next = list->Next;
            if (list->Tags) { delete list->Tags; list->Tags = nullptr; }
            FreeResource(list);
            list = next;
         }
         Dir->Info = nullptr;
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

ERR fs_rename(std::string_view CurrentPath, std::string_view NewPath)
{
   return ERR::NoSupport;
}

//********************************************************************************************************************

ERR fs_testpath(std::string &Path, RSF Flags, LOC *Type)
{
   if (Path.ends_with(':')) {
      std::string str;
      if (ResolvePath(Path, RSF::NIL, &str) IS ERR::Okay) {
         if (Type) *Type = LOC::VOLUME;
         return ERR::Okay;
      }
      else return ERR::DoesNotExist;
   }

   LOC type;

   #ifdef __unix__

      struct stat64 info;
      type = LOC::NIL;
      if (!stat64(Path.c_str(), &info)) {
         if (S_ISDIR(info.st_mode)) type = LOC::DIRECTORY;
         else type = LOC::FILE;
      }
      else if (!lstat64(Path.c_str(), &info)) type = LOC::FILE; // The file is a broken symbolic link

   #elif _WIN32

      type = LOC(winTestLocation(Path.c_str(), ((Flags & RSF::CASE_SENSITIVE) != RSF::NIL) ? true : false));

   #endif

   if (type != LOC::NIL) {
      if (Type) *Type = type;
      return ERR::Okay;
   }
   else return ERR::DoesNotExist;
}

//********************************************************************************************************************

ERR fs_getinfo(std::string_view Path, FileInfo *Info, int InfoSize)
{
   pf::Log log(__FUNCTION__);

#ifdef __unix__
   // In order to tell if a folder is a symbolic link or not, we have to remove any trailing slash...

   char path_ref[256];
   int len = strcopy(Path.data(), path_ref, sizeof(path_ref));
   if ((size_t)len >= sizeof(path_ref)-1) return ERR::BufferOverflow;
   if ((path_ref[len-1] IS '/') or (path_ref[len-1] IS '\\')) path_ref[len-1] = 0;

   // Get the file info.  Use lstat64() and if it turns out that the file is a symbolic link, set the RDF::LINK flag
   // and then switch to stat64().

   struct stat64 info;
   if (lstat64(path_ref, &info) IS -1) return ERR::FileNotFound;

   Info->Flags = RDF::NIL;

   if (S_ISLNK(info.st_mode)) {
      Info->Flags |= RDF::LINK;
      if (stat64(path_ref, &info) IS -1) {
         // We do not abort in the case of a broken link, just warn and treat it as an empty file
         log.warning("Broken link detected.");
      }
   }

   if (S_ISDIR(info.st_mode)) Info->Flags |= RDF::FOLDER|RDF::TIME|RDF::PERMISSIONS;
   else Info->Flags |= RDF::FILE|RDF::SIZE|RDF::TIME|RDF::PERMISSIONS;

   // Extract file/folder name

   int i = len;
   while ((i > 0) and (path_ref[i-1] != '/') and (path_ref[i-1] != '\\') and (path_ref[i-1] != ':')) i--;
   i = strcopy(path_ref + i, Info->Name, MAX_FILENAME-2);

   if ((Info->Flags & RDF::FOLDER) != RDF::NIL) {
      Info->Name[i++] = '/';
      Info->Name[i] = 0;
   }

   Info->Tags = nullptr;
   Info->Size = info.st_size;

   // Set file security information

   Info->Permissions = PERMIT::NIL;
   if (info.st_mode & S_IRUSR) Info->Permissions |= PERMIT::READ;
   if (info.st_mode & S_IWUSR) Info->Permissions |= PERMIT::WRITE;
   if (info.st_mode & S_IXUSR) Info->Permissions |= PERMIT::EXEC;
   if (info.st_mode & S_IRGRP) Info->Permissions |= PERMIT::GROUP_READ;
   if (info.st_mode & S_IWGRP) Info->Permissions |= PERMIT::GROUP_WRITE;
   if (info.st_mode & S_IXGRP) Info->Permissions |= PERMIT::GROUP_EXEC;
   if (info.st_mode & S_IROTH) Info->Permissions |= PERMIT::OTHERS_READ;
   if (info.st_mode & S_IWOTH) Info->Permissions |= PERMIT::OTHERS_WRITE;
   if (info.st_mode & S_IXOTH) Info->Permissions |= PERMIT::OTHERS_EXEC;
   if (info.st_mode & S_ISUID) Info->Permissions |= PERMIT::USERID;
   if (info.st_mode & S_ISGID) Info->Permissions |= PERMIT::GROUPID;

   Info->UserID = info.st_uid;
   Info->GroupID = info.st_gid;

   // Get time information.  NB: The timestamp is calculated by the filesystem's GetFileInfo() manager, using
   // calc_timestamp().

   struct tm *local;
   if ((local = localtime(&info.st_mtime))) {
      Info->Modified.Year   = 1900 + local->tm_year;
      Info->Modified.Month  = local->tm_mon + 1;
      Info->Modified.Day    = local->tm_mday;
      Info->Modified.Hour   = local->tm_hour;
      Info->Modified.Minute = local->tm_min;
      Info->Modified.Second = local->tm_sec;
   }

#else
   int8_t dir;
   int i;

   Info->Flags = RDF::NIL;
   size_t isize;
   if (!winFileInfo(Path.data(), &isize, &Info->Modified, &dir)) return ERR::File;
   Info->Size = isize;

   // TimeStamp has to match that produced by GET_TimeStamp

   struct stat64 stats;
   if (!stat64(Path.data(), &stats)) {
      if (auto local = localtime(&stats.st_mtime)) {
         Info->Modified.Year   = 1900 + local->tm_year;
         Info->Modified.Month  = local->tm_mon + 1;
         Info->Modified.Day    = local->tm_mday;
         Info->Modified.Hour   = local->tm_hour;
         Info->Modified.Minute = local->tm_min;
         Info->Modified.Second = local->tm_sec;
      }
   }

   if (Path.ends_with('/') or Path.ends_with('\\')) Info->Flags |= RDF::FOLDER|RDF::TIME;
   else if (dir) Info->Flags |= RDF::FOLDER|RDF::TIME;
   else Info->Flags |= RDF::FILE|RDF::SIZE|RDF::TIME;

   // Extract the file name

   size_t fi;
   if (Path.ends_with('/') or Path.ends_with('\\')) fi = Path.find_last_of("/\\:", Path.size()-2);
   else fi = Path.find_last_of("/\\:");

   if (fi IS std::string::npos) i = strcopy(Path.data(), Info->Name, MAX_FILENAME - 2);
   else i = strcopy(Path.data() + fi + 1, Info->Name, MAX_FILENAME - 2);

   if ((Info->Flags & RDF::FOLDER) != RDF::NIL) {
      if (Info->Name[i-1] IS '\\') Info->Name[i-1] = '/';
      else if (Info->Name[i-1] != '/') {
         Info->Name[i++] = '/';
         Info->Name[i] = 0;
      }
   }

   Info->Permissions = PERMIT::NIL;
   Info->UserID      = 0;
   Info->GroupID     = 0;
   Info->Tags        = nullptr;

#endif

   return ERR::Okay;
}

//********************************************************************************************************************

ERR fs_getdeviceinfo(std::string_view Path, objStorageDevice *Info)
{
   pf::Log log("GetDeviceInfo");

   std::string location, resolve;
   ERR error;

restart:
   auto pathend = Path.find(':');
   std::string vol(Path, 0, pathend);

   if (auto lock = std::unique_lock{glmVolumes, 2s}) {
      // We keep this lock localised so that it doesn't impact ResolvePath()
      if (glVolumes.contains(vol)) {
         if (!glVolumes[vol]["Path"].compare(0, 6, "EXT:")) Info->DeviceFlags |= DEVICE::SOFTWARE; // Virtual device

         if (glVolumes[vol].contains("Device")) {
            auto &device = glVolumes[vol]["Device"];
            if (!device.compare("disk"))     Info->DeviceFlags |= DEVICE::FLOPPY_DISK|DEVICE::REMOVABLE|DEVICE::READ|DEVICE::WRITE;
            else if (!device.compare("fixed")) Info->DeviceFlags |= DEVICE::HARD_DISK|DEVICE::READ|DEVICE::WRITE;
            else if (!device.compare("hd"))  Info->DeviceFlags |= DEVICE::HARD_DISK|DEVICE::READ|DEVICE::WRITE;
            else if (!device.compare("cd"))  Info->DeviceFlags |= DEVICE::COMPACT_DISC|DEVICE::REMOVABLE|DEVICE::READ;
            else if (!device.compare("usb")) Info->DeviceFlags |= DEVICE::USB|DEVICE::REMOVABLE;
            else if (!device.compare("portable")) Info->DeviceFlags |= DEVICE::REMOVABLE;
            else log.warning("Device '%s' unrecognised.", device.c_str());
         }
      }
   }
   else return log.warning(ERR::SystemLocked);

   if (Info->DeviceFlags IS DEVICE::NIL) {
      // Unable to find a device reference for the volume, so try to resolve the path and try again.

      Info->DeviceFlags |= DEVICE::BOOKMARK;
      if (!resolve.empty()) { // We've done what we can - drop through
         #ifdef _WIN32
            // On win32 we can get the drive information from the drive letter
            // TODO: Write Win32 code to discover the drive type in GetDeviceInfo().
         #endif
         location.assign(resolve);
      }
      else {
         if (ResolvePath(Path, RSF::NO_FILE_CHECK, &resolve) != ERR::Okay) return ERR::ResolvePath;
         Path = std::string_view(resolve);
         goto restart;
      }
   }

   // Assume that the device is read/write if the device type cannot be assessed

   if (Info->DeviceFlags IS DEVICE::NIL) Info->DeviceFlags |= DEVICE::READ|DEVICE::WRITE;

   // Calculate the amount of available disk space

#ifdef _WIN32

   int64_t bytes_avail, total_size;

   if (location.empty()) error = ResolvePath(Path, RSF::NO_FILE_CHECK, &location);
   else error = ERR::Okay;

   if (error IS ERR::Okay) {
      if (!(winGetFreeDiskSpace(location[0], &bytes_avail, &total_size))) {
         log.msg("Failed to read disk space for \"%s\" (%s)", location.c_str(), Path.data());
         Info->BytesFree  = -1;
         Info->BytesUsed  = 0;
         Info->DeviceSize = -1;
         return ERR::Okay; // Even though the disk space calculation failed, we succeeded on resolving other device information
      }
      else {
         Info->BytesFree  = bytes_avail;
         Info->BytesUsed  = total_size - bytes_avail;
         Info->DeviceSize = total_size;
         return ERR::Okay;
      }
   }
   else error = ERR::ResolvePath;

   return log.warning(error);

#elif __unix__

   if ((Info->DeviceFlags & DEVICE::HARD_DISK) != DEVICE::NIL) {
      if (location.empty()) error = ResolvePath(Path, RSF::NO_FILE_CHECK, &location);
      else error = ERR::Okay;

      if (error IS ERR::Okay) {
         struct statfs fstat;
         int result = statfs(location.c_str(), &fstat);

         if (result != -1) {
            double blocksize = (double)fstat.f_bsize;
            Info->BytesFree  = ((double)fstat.f_bavail) * blocksize;
            Info->DeviceSize = ((double)fstat.f_blocks) * blocksize;
            Info->BytesUsed  = Info->DeviceSize - Info->BytesFree;

            /* statvfs()
            if (fstat.f_flag & ST_RDONLY) {
               Info->DeviceFlags &= ~DEVICE::WRITE;
            }
            */

            // Floating point corrections

            if (Info->BytesFree < 1)  Info->BytesFree = 0;
            if (Info->BytesUsed < 1)  Info->BytesUsed = 0;
            if (Info->DeviceSize < 1) Info->DeviceSize = 0;
            return ERR::Okay;
         }
         else return log.warning(convert_errno(errno, ERR::File));
      }
      else return log.warning(ERR::ResolvePath);
   }
   else {
      Info->BytesFree  = -1;
      Info->DeviceSize = -1;
      Info->BytesUsed  = 0;
      return ERR::Okay;
   }
#endif

   return ERR::NoSupport;
}

//********************************************************************************************************************

ERR fs_makedir(std::string_view Path, PERMIT Permissions)
{
   pf::Log log(__FUNCTION__);

#ifdef __unix__

   int err, i;

   // The 'executable' bit must be set for folders in order to have any sort of access to their content.  So, if
   // the read or write flags are set, we automatically enable the executable bit for that folder.

   Permissions |= PERMIT::EXEC; // At a minimum, ensure the owner has folder access initially
   if ((Permissions & PERMIT::GROUP) != PERMIT::NIL) Permissions |= PERMIT::GROUP_EXEC;
   if ((Permissions & PERMIT::OTHERS) != PERMIT::NIL) Permissions |= PERMIT::OTHERS_EXEC;

   log.branch("%s, Permissions: $%.8x %s", Path.data(), int(Permissions), (glDefaultPermissions != PERMIT::NIL) ? "(forced)" : "");

   int secureflags = convert_permissions(Permissions);

   if (mkdir(Path.data(), secureflags) IS -1) {
      auto buffer = std::make_unique<char[]>(Path.size()+1);

      if (errno IS EEXIST) {
         log.msg("A folder or file already exists at \"%s\"", Path.data());
         return ERR::FileExists;
      }

      // This loop will go through the complete path attempting to create multiple folders.

      for (i=0; i < std::ssize(Path); i++) {
         buffer[i] = Path[i];
         if ((i > 0) and (buffer[i] IS '/')) {
            buffer[i+1] = 0;

            log.msg("%s", buffer.get());

            if (((err = mkdir(buffer.get(), secureflags)) IS -1) and (errno != EEXIST)) break;

            if (!err) {
               if ((glForceUID != -1) or (glForceGID != -1)) chown(buffer.get(), glForceUID, glForceGID);
               if (secureflags & (S_ISUID|S_ISGID)) chmod(buffer.get(), secureflags);
            }
         }
      }

      if (i < std::ssize(Path)) {
         log.warning("Failed to create folder \"%s\".", Path.data());
         return ERR::SystemCall;
      }
      else if (!Path.ends_with('/')) {
         // If the path did not end with a slash, there is still one last folder to create
         buffer[i] = 0;
         log.msg("%s", buffer.get());
         if (((err = mkdir(buffer.get(), secureflags)) IS -1) and (errno != EEXIST)) {
            log.warning("Failed to create folder \"%s\".", Path.data());
            return convert_errno(errno, ERR::SystemCall);
         }
         if (!err) {
            if ((glForceUID != -1) or (glForceGID != -1)) chown(buffer.get(), glForceUID, glForceGID);
            if (secureflags & (S_ISUID|S_ISGID)) chmod(buffer.get(), secureflags);
         }
      }
   }
   else {
      if ((glForceUID != -1) or (glForceGID != -1)) chown(Path.data(), glForceUID, glForceGID);
      if (secureflags & (S_ISUID|S_ISGID)) chmod(Path.data(), secureflags);
   }

   return ERR::Okay;

#elif _WIN32

   if (Path.size() < 3) return ERR::Args;

   if (auto error = winCreateDir(Path.data()); error != ERR::Okay) {
      std::string buffer;
      buffer.reserve(Path.size()+1);

      if (error IS ERR::FileExists) return ERR::FileExists;

      log.trace("Creating parent folders.");

      std::size_t start = 0;
      while (start != std::string::npos) {
         auto end = Path.find('\\', start+1);
         if (end != std::string::npos) buffer.append(Path, start, end-start);
         else buffer.append(Path, start);
         if (buffer.size() > 3) {
            if (auto error = winCreateDir(buffer.c_str()); error != ERR::Okay) {
               if (error != ERR::FileExists) {
                  log.traceWarning("Failed to create folder \"%s\".", Path.data());
                  return ERR::File;
               }
            }
         }
         start = end;
      }
   }

   return ERR::Okay;
#endif
}

//********************************************************************************************************************
// Private function for deleting files and folders recursively.

#ifdef __unix__

ERR delete_tree(std::string &Path, FUNCTION *Callback, FileFeedback *Feedback)
{
   pf::Log log(__FUNCTION__);
   ERR error;

   log.trace("Path: %s", Path.c_str());

   if ((Callback) and (Callback->defined())) {
      Feedback->Path = Path.data();
      FFR result = CALL_FEEDBACK(Callback, Feedback);
      if (result IS FFR::ABORT) {
         log.trace("Feedback requested abort at file '%s'", Path.c_str());
         return ERR::Cancelled;
      }
      else if (result IS FFR::SKIP) {
         log.trace("Feedback requested skip at file '%s'", Path.c_str());
         return ERR::Okay;
      }
   }

   // Check if the folder is actually a symbolic link (we don't want to recurse into them)

   struct stat64 info;
   if (lstat64(Path.c_str(), &info) != -1) {
      if (S_ISLNK(info.st_mode)) {
         if (unlink(Path.c_str())) {
            log.error("unlink() failed on symbolic link '%s'", Path.c_str());
            return convert_errno(errno, ERR::SystemCall);
         }
         else return ERR::Okay;
      }
   }

   if (auto stream = opendir(Path.c_str())) {
      Path.append("/");
      auto folder_len = Path.size();
      error = ERR::Okay;
      rewinddir(stream);
      struct dirent *direntry;
      while ((direntry = readdir(stream))) {
         if ((direntry->d_name[0] IS '.') and (direntry->d_name[1] IS 0));
         else if ((direntry->d_name[0] IS '.') and (direntry->d_name[1] IS '.') and (direntry->d_name[2] IS 0));
         else {
            Path.resize(folder_len);
            Path.append(direntry->d_name);
            if (auto dummydir = opendir(Path.c_str())) {
               closedir(dummydir);
               if (delete_tree(Path, Callback, Feedback) IS ERR::Cancelled) {
                  error = ERR::Cancelled;
                  break;
               }
            }
            else { // Delete a file within the folder
               if (unlink(Path.c_str())) {
                  log.error("unlink() failed on '%s'", Path.c_str());
                  error = convert_errno(errno, ERR::SystemCall);
                  break;
               }
            }
         }
      }
      closedir(stream);

      Path.resize(folder_len);
      Path.pop_back();

      if ((error IS ERR::Okay) and (rmdir(Path.c_str()))) {
         log.error("rmdir(%s) error: %s", Path.c_str(), strerror(errno));
         return convert_errno(errno, ERR::SystemCall);
      }

      return error;
   }
   else {
      log.error("Failed to open folder \"%s\" using opendir().", Path.c_str());
      return convert_errno(errno, ERR::SystemCall);
   }
}

#endif

//********************************************************************************************************************

#include "fs_identify.cpp"
#include "fs_resolution.cpp"
#include "fs_folders.cpp"
#include "fs_volumes.cpp"
#include "fs_watch_path.cpp"
