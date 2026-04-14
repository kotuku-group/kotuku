/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
File: Enables access to the file system.

The File class provides extensive support for file management and I/O.  The class supports the notion of individual
file compression and file finding capabilities.  Since all File objects are tracked, there is no chance of the system
leaving locked files behind after a program exits.  Folder management is also integrated into this class to ease the
management of both file types.

To read or write to a file, set the #Path of the file as well as the correct I/O file flags before initialisation.
See the #Flags field for information on the available I/O flags.  Functionality for read and write operations is
provided through the #Read() and #Write() actions.  The #Seek() action can be used to change the read/write position
in a file.
-END-

*********************************************************************************************************************/

#define PRV_FILE
#define PRV_FILESYSTEM
#define _LARGEFILE64_SOURCE
#define _POSIX_THREAD_SAFE_FUNCTIONS // For localtime_r
#define _TIME64_T
#define _LARGE_TIME_API
#include "../defs.h"

#if defined(__unix__) && !defined(_WIN32)
 #include <unistd.h>
 #include <dirent.h>
 #include <fcntl.h>
 #include <grp.h>
 #include <pwd.h>
 #include <signal.h>
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

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

#ifdef _WIN32
 #include <io.h>
 #ifndef _MSC_VER
  #include <unistd.h>
 #endif

 #define open64   open
 #define lseek64  lseek
#endif // _WIN32

#ifdef __APPLE__
 #include <sys/param.h>
 #include <sys/mount.h>
 #define lseek64  lseek
 #define ftruncate64 ftruncate
#endif

#ifdef _MSC_VER
 #define S_IRUSR _S_IREAD
 #define S_IWUSR _S_IWRITE
 #ifndef _S_ISTYPE
  #define _S_ISTYPE(mode, mask)  (((mode) & _S_IFMT) == (mask))
  #define S_ISREG(mode) _S_ISTYPE((mode), _S_IFREG)
  #define S_ISDIR(mode) _S_ISTYPE((mode), _S_IFDIR)
 #endif
 #define stat64 stat
 #define fstat64 fstat
#endif

#include <kotuku/main.h>

extern "C" void path_monitor(HOSTHANDLE, extFile *);

static ERR FILE_Init(extFile *);
static ERR FILE_Watch(extFile *, struct fl::Watch *);

static ERR SET_Path(extFile *, CSTRING);
static ERR SET_Size(extFile *, int64_t);

static ERR GET_ResolvedPath(extFile *, CSTRING *);

static ERR set_permissions(extFile *, PERMIT);

/*********************************************************************************************************************
-ACTION-
Activate: Opens the file.  Performed automatically if `NEW`, `READ` or `WRITE` flags were specified on initialisation.
-END-
*********************************************************************************************************************/

static ERR FILE_Activate(extFile *Self)
{
   pf::Log log;

   if (Self->Handle != -1) return ERR::Okay;
   if ((Self->Flags & (FL::NEW|FL::READ|FL::WRITE)) IS FL::NIL) return log.warning(ERR::NothingDone);

   // Setup the open flags.  Note that for new files, the owner will always have read/write/delete permissions by
   // default.  Extra flags can be set through the Permissions field.  If the user wishes to turn off his access to
   // the created file then he must do so after initialisation.

   int openflags = 0;
   if ((Self->Flags & FL::NEW) != FL::NIL) openflags |= O_CREAT|O_TRUNC;

   CSTRING path;
   if (GET_ResolvedPath(Self, &path) != ERR::Okay) return ERR::ResolvePath;

#ifdef __unix__
   int secureflags = S_IRUSR|S_IWUSR|convert_permissions(Self->Permissions);

   // Opening /dev/ files from Kōtuku is disallowed because it can cause problems

   if ((Self->Flags & FL::DEVICE) != FL::NIL) {
      openflags |= O_NOCTTY; // Prevent device from becoming the controlling terminal
   }
   else if (std::string_view(path).starts_with("/dev/")) {
      log.warning("Opening devices not permitted without the DEVICE flag.");
      return ERR::NoPermission;
   }
#else
   int secureflags = S_IRUSR|S_IWUSR;
#endif

   if ((Self->Flags & (FL::READ|FL::WRITE)) IS (FL::READ|FL::WRITE)) {
      log.msg("Open \"%s\" [RW]", path);
      openflags |= O_RDWR;
   }
   else if ((Self->Flags & FL::READ) != FL::NIL) {
      log.msg("Open \"%s\" [R]", path);
      openflags |= O_RDONLY;
   }
   else if ((Self->Flags & FL::WRITE) != FL::NIL) {
      log.msg("Open \"%s\" [W|%s]", path, ((Self->Flags & FL::NEW) != FL::NIL) ? "New" : "Existing");
      openflags |= O_RDWR;
   }
   else log.msg("Open \"%s\" [-]", path);

   #ifdef __unix__
      // Set O_NONBLOCK to stop the task from being halted in the event that we accidentally try to open a pipe like a
      // FIFO file.  This can happen when scanning the /dev/ folder and can cause tasks to hang.

      openflags |= O_NONBLOCK;
   #endif

   #ifdef _WIN32
      if ((Self->Flags & FL::NEW) != FL::NIL) {
         // Make sure that we'll be able to recreate the file from new if it already exists and is marked read-only.

         chmod(path, S_IRUSR|S_IWUSR);
      }
   #endif

   if ((Self->Handle = open(path, openflags|WIN32OPEN|O_LARGEFILE, secureflags)) IS -1) {
      int err = errno;

      if ((Self->Flags & FL::NEW) != FL::NIL) {
         // Attempt to create the necessary directories that might be required for this new file.

         if (check_paths(path, Self->Permissions) IS ERR::Okay) {
            Self->Handle = open(path, openflags|WIN32OPEN|O_LARGEFILE, secureflags);
         }

         if (Self->Handle IS -1) {
            log.warning("New file error \"%s\"", path);
            if (err IS EACCES) return log.warning(ERR::NoPermission);
            else if (err IS ENAMETOOLONG) return log.warning(ERR::BufferOverflow);
            else return ERR::CreateFile;
         }
      }
      else if ((errno IS EROFS) and ((Self->Flags & FL::READ) != FL::NIL)) {
         // Drop requested access rights to read-only and try again
         log.warning("Reverting to read-only access for this read-only file.");
         openflags = O_RDONLY;
         Self->Flags &= ~FL::WRITE;
         Self->Handle = open(path, openflags|WIN32OPEN|O_LARGEFILE, secureflags);
      }
      else if ((Self->Flags & FL::LINK) != FL::NIL) {
         // The file is a broken symbolic link (i.e. refers to a file that no longer exists).  Even
         // though we won't be able to get a valid handle for the link, we'll allow the initialisation
         // to continue because the user may want to delete the symbolic link or get some information
         // about it.
      }

      if ((Self->Handle IS -1) and ((Self->Flags & FL::LINK) IS FL::NIL)) {
         switch(errno) {
            case EACCES: return log.warning(ERR::NoPermission);
            case EEXIST: return log.warning(ERR::FileExists);
            case EINVAL: return log.warning(ERR::Args);
            case ENOENT: return log.warning(ERR::FileNotFound);
            default:
               log.warning("Could not open \"%s\", error: %s", path, strerror(errno));
               return ERR::SystemCall;
         }
      }
   }

   // File size management

   if (Self->Handle != -1) {
      if ((Self->Flags & FL::NEW) != FL::NIL);
      else if ((Self->Size = lseek64(Self->Handle, 0, SEEK_END)) != -1) {  // Get the size of the file.  Could be zero if the file is a stream.
         lseek64(Self->Handle, 0, SEEK_SET);
      }
      else { // lseek64() can fail if the file is special
         Self->Size = 0;
      }
   }

   if ((Self->Flags & FL::NEW) != FL::NIL) {
      if (Self->Permissions != PERMIT::NIL) set_permissions(Self, Self->Permissions);
   }

   // If the BUFFER flag is set, load the entire file into RAM and treat it as a read/write memory buffer.

   if ((Self->Flags & FL::BUFFER) != FL::NIL) return Self->bufferContent();

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
BufferContent: Reads all file content into a local memory buffer.

File content may be buffered at any time by calling the BufferContent method.  This will allocate a buffer that matches
the current file size and the file's content will be read into that buffer.  The `BUFFER` flag is set in the file object
and a pointer to the content is referenced in the file's #Buffer field.  Standard file operations such as read, write
and seek have the same effect when a file is in buffer mode.

Once a file has been buffered, the original file handle and any locks on that file are returned to the system.
Physical operations on the file object such as delete, rename and attribute settings no longer have meaning when
applied to a buffered file.  It is not possible to drop the buffer and return the file object to its original state
once buffering has been enabled.

-ERRORS-
Okay: The file content was successfully buffered.
AllocMemory:
Read: Failed to read the file content.

*********************************************************************************************************************/

static ERR FILE_BufferContent(extFile *Self)
{
   pf::Log log;
   int len;

   if (Self->Buffer) return ERR::Okay;

   acSeek(Self, 0, SEEK::START);

   if (!Self->Size) {
      // If the file has no size, it could be a stream (or simply empty).  This routine handles this situation.

      char ch;
      if (acRead(Self, &ch, 1, &len) IS ERR::Okay) {
         Self->Flags |= FL::STREAM;
         // Allocate a 1 MB memory block, read the stream into it, then reallocate the block to the correct size.

         uint8_t *buffer;
         if (AllocMemory(1024 * 1024, MEM::NO_CLEAR, (APTR *)&buffer, nullptr) IS ERR::Okay) {
            acSeekStart(Self, 0);
            acRead(Self, buffer, 1024 * 1024, &len);
            if (len > 0) {
               if (AllocMemory(len, MEM::NO_CLEAR, (APTR *)&Self->Buffer, nullptr) IS ERR::Okay) {
                  copymem(buffer, Self->Buffer, len);
                  Self->Size = len;
               }
            }
            FreeResource(buffer);
         }
      }
   }
   else {
      // Allocate buffer and load file content.  A NULL byte is added so that there is some safety in the event that
      // the file content is treated as a string.

      int8_t *buffer;
      if (AllocMemory(Self->Size+1, MEM::NO_CLEAR, (APTR *)&buffer, nullptr) IS ERR::Okay) {
         buffer[Self->Size] = 0;
         if (acRead(Self, buffer, Self->Size, &len) IS ERR::Okay) {
            Self->Buffer = buffer;
         }
         else {
            FreeResource(buffer);
            return log.warning(ERR::Read);
         }
      }
      else return log.warning(ERR::AllocMemory);
   }

   // If the file was empty, allocate a 1-byte memory block for the Buffer field, in order to satisfy condition tests.

   if (!Self->Buffer) {
      if (AllocMemory(1, MEM::DATA, (APTR *)&Self->Buffer, nullptr) != ERR::Okay) {
         return log.warning(ERR::AllocMemory);
      }
   }

   log.msg("File content now buffered in a %" PF64 " byte memory block.", (long long)Self->Size);

   close(Self->Handle);
   Self->Handle = -1;
   Self->Position = 0;
   Self->Flags |= FL::BUFFER;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Data can be streamed to any file as a method of writing content.

Streaming data of any type to a file will result in the content being written to the file at the current seek
#Position.

*********************************************************************************************************************/

static ERR FILE_DataFeed(extFile *Self, struct acDataFeed *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);

   if (Args->Size) return acWrite(Self, Args->Buffer, Args->Size, nullptr);
   else return acWrite(Self, Args->Buffer, strlen((CSTRING)Args->Buffer), nullptr);
}

/*********************************************************************************************************************

-METHOD-
Copy: Copies the data of a file to another location.

This method is used to copy the data of a file object to another location.  All of the data will be copied, effectively
creating a clone of the original file information.  The file object must have been initialised with the `FL::READ` flag,
or the copy operation will not work (this restriction does not apply to directories).  If a matching file name already
exists at the destination path, it will be over-written with the new data.

The #Position field will be reset as a result of calling this method.

When copying directories with this method, the entire folder structure (i.e. all of the folder contents) will be
copied to the new location.  If an error occurs when copying a sub-folder or file, the procedure will be aborted
and an error code will be returned.

-INPUT-
cstr Dest: The destination file path for the copy operation.
ptr(func) Callback: Optional callback for receiving feedback during the operation.

-ERRORS-
Okay: The file data was copied successfully.
NullArgs:
Args:
FieldNotSet: The #Path field has not been set in the file object.
Failed:
Read: Data could not be read from the source path.
Write: Data could not be written to the destination path.
ResolvePath:
Loop: Performing the copy would cause infinite recursion.
AllocMemory:

*********************************************************************************************************************/

static ERR FILE_Copy(extFile *Self, struct fl::Copy *Args)
{
   return CopyFile(Self->Path.c_str(), Args->Dest, Args->Callback);
}

/*********************************************************************************************************************

-METHOD-
Delete: Deletes a file from its source location.

This method is used to delete files from their source location.  If used on a folder, all of the folder's
contents will be deleted in the call.   Once a file is deleted, the object effectively becomes unusable.  For this
reason, file deletion should normally be followed up with a call to the Free action.

-INPUT-
ptr(func) Callback: Optional callback for receiving feedback during the operation.

-ERRORS-
Okay:  File deleted successfully.
Failed: The deletion attempt failed (specific condition not available).
MissingPath:
ResolvePath:
NoPermission: The user does not have the necessary permissions to delete the file.
ReadOnly: The file is on a read-only filesystem.
Locked: The file is in use.
BufferOverflow: The file path string is too long.

*********************************************************************************************************************/

static ERR FILE_Delete(extFile *Self, struct fl::Delete *Args)
{
   pf::Log log;

   if (Self->Path.empty()) return log.warning(ERR::MissingPath);

   if ((Self->Stream) and ((Self->Flags & FL::LINK) IS FL::NIL)) {
      log.branch("Delete Folder: %s", Self->Path.c_str());

      // Check if the Path is a volume

      if (Self->Path.ends_with(':')) {
         if (DeleteVolume(Self->Path.c_str()) IS ERR::Okay) {
            #ifdef __unix__
               closedir((DIR *)Self->Stream);
            #endif
            Self->Stream = 0;
            return ERR::Okay;
         }
         else return ERR::DeleteFile;
      }

      // Delete the folder and its contents

      CSTRING path;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         std::string buffer(path);

         #ifdef __unix__
            closedir((DIR *)Self->Stream);
         #endif
         Self->Stream = 0;

         if (buffer.ends_with('/') or buffer.ends_with('\\')) buffer.pop_back();

         FileFeedback fb;
         if ((Args->Callback) and (Args->Callback->defined())) {
            fb.FeedbackID = FBK::DELETE_FILE;
            fb.Path       = buffer.data();
         }

         ERR error;
         if ((error = delete_tree(buffer, Args->Callback, &fb)) IS ERR::Okay);
         else if (error != ERR::Cancelled) log.warning("Failed to delete folder \"%s\"", buffer.c_str());

         return error;
      }
      else return log.warning(ERR::ResolvePath);
   }
   else {
      log.branch("Delete File: %s", Self->Path.c_str());

      CSTRING path;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         std::string buffer(path);
         if (buffer.ends_with('/') or buffer.ends_with('\\')) buffer.pop_back();

         if (Self->Handle != -1) { close(Self->Handle); Self->Handle = -1; }

         // Unlinking the file deletes it

         if (!unlink(buffer.c_str())) return ERR::Okay;
         else {
            log.warning("unlink() failed on file \"%s\": %s", buffer.c_str(), strerror(errno));
            return convert_errno(errno, ERR::SystemCall);
         }
      }
      else return log.warning(ERR::ResolvePath);
   }
}

//********************************************************************************************************************

static ERR FILE_Free(extFile *Self)
{
   pf::Log log;

   if (Self->prvWatch) Action(fl::Watch::id, Self, nullptr);

#ifdef _WIN32
   std::string path;
   if ((Self->Flags & FL::RESET_DATE) != FL::NIL) {
      // If we have to reset the date, get the file path
      log.trace("Resetting the file date.");
      ResolvePath(Self->Path, RSF::NIL, &path);
   }
#endif

   if (Self->ProgressDialog) { FreeResource(Self->ProgressDialog); Self->ProgressDialog = nullptr; }
   if (Self->prvList) { FreeResource(Self->prvList); Self->prvList = nullptr; }
   if (Self->Buffer)  { FreeResource(Self->Buffer); Self->Buffer = nullptr; }

   if (Self->Handle != -1) {
      if (close(Self->Handle) IS -1) {
         #ifdef __unix__
            log.warning("Unix filesystem error: %s", strerror(errno));
         #endif
      }
      Self->Handle = -1;
   }

   if (Self->Stream) {
      #ifdef __unix__
         closedir((DIR *)Self->Stream);
      #endif
      Self->Stream = 0;
   }

#ifdef _WIN32
   if (((Self->Flags & FL::RESET_DATE) != FL::NIL) and (!path.empty())) {
      winResetDate(path.data());
   }
#endif

   Self->~extFile();
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Init: Initialises a file.

This action will prepare a file or folder at the given #Path for use.

To create a new file from scratch, specify the `NEW` flag.  This will overwrite any file that exists at the target path.

To read and write data to the file, specify the `READ` and/or `WRITE` modes in the #Flags field prior to initialisation.
If a file is read-only and the `WRITE` and `READ` flags are set in combination, the `WRITE` flag will be dropped and
initialisation will continue as normal.

If neither of the `NEW`, `READ` or `WRITE` flags are specified, the file object is prepared and queried from disk (if it
exists), but will not be opened.  It will be necessary to #Activate() the file in order to open it.

The File class supports RAM based file buffering - this is activated by using the `BUFFER` flag and setting the Size
field to the desired buffer size.  A file path is not required unless the buffer needs to be filled with content on
initialisation.  Because buffered files exist virtually, their functionality is restricted to read/write access.

Strings can also be loaded into file buffers for read/write access.  This is achieved by specifying the
#Path `string:Data\0`, where `Data` is a sequence of characters to be loaded into a virtual memory space.

-ERRORS-
Okay:
MissingPath:
SetField: An error occurred while updating the #Path field.
FileNotFound:
ResolvePath:
Search: The file could not be found.
NoPermission: Permission was denied when accessing or creating the file.

*********************************************************************************************************************/

static ERR FILE_Init(extFile *Self)
{
   pf::Log log;
   ERR error;

   // If the BUFFER flag is set then the file will be located in RAM.  Very little initialisation is needed for this.
   // If a path has been specified, we'll load the entire file into memory.  Please see the end of this
   // initialisation routine for more info.

   if (((Self->Flags & FL::BUFFER) != FL::NIL) and (Self->Path.empty())) {
      if (Self->Size < 0) Self->Size = 0;
      Self->Flags |= FL::READ|FL::WRITE;
      if (!Self->Buffer) {
         // Allocate buffer if none specified.  An extra byte is allocated for a NULL byte on the end, in case the file
         // content is treated as a string.

         if (AllocMemory((Self->Size < 1) ? 1 : Self->Size+1, MEM::NO_CLEAR, (APTR *)&Self->Buffer, nullptr) != ERR::Okay) {
            return log.warning(ERR::AllocMemory);
         }
         ((int8_t *)Self->Buffer)[Self->Size] = 0;
      }
      return ERR::Okay;
   }

   if (Self->Path.empty()) return log.warning(ERR::MissingPath);

   if (glDefaultPermissions != PERMIT::NIL) Self->Permissions = glDefaultPermissions;

   if (pf::startswith("string:", Self->Path)) {
      Self->Size = Self->Path.size() - 7;

      if (Self->Size > 0) {
         if (AllocMemory(Self->Size, MEM::DATA, (APTR *)&Self->Buffer, nullptr) IS ERR::Okay) {
            Self->Flags |= FL::READ|FL::WRITE;
            copymem(Self->Path.c_str() + 7, Self->Buffer, Self->Size);
            return ERR::Okay;
         }
         else return log.warning(ERR::AllocMemory);
      }
      else return log.warning(ERR::InvalidPath);
   }
   else if (pf::startswith("std:", Self->Path)) {
      // Special accessors for direct access to standard input/output/error streams.

      Self->Flags &= ~(FL::NEW|FL::READ|FL::WRITE);

      if (iequals("std:in", Self->Path)) {
         Self->Flags |= FL::READ;
         #ifdef _WIN32
            Self->Handle = _fileno(stdin);
         #else
            Self->Handle = STDIN_FILENO;
         #endif
      }
      else if (iequals("std:out", Self->Path)) {
         Self->Flags |= FL::WRITE;
         #ifdef _WIN32
            Self->Handle = _fileno(stdout);
         #else
            Self->Handle = STDOUT_FILENO;
         #endif
      }
      else if (iequals("std:err", Self->Path)) {
         Self->Flags |= FL::WRITE;
         #ifdef _WIN32
            Self->Handle = _fileno(stderr);
         #else
            Self->Handle = STDERR_FILENO;
         #endif
      }
      else return log.warning(ERR::InvalidPath);

      return ERR::Okay;
   }

   if ((Self->Permissions IS PERMIT::NIL) or ((Self->Permissions & PERMIT::INHERIT) != PERMIT::NIL)) {
      FileInfo info;

      // If the file already exists, pull the permissions from it.  Otherwise use a default set of permissions (if
      // possible, inherit permissions from the file's folder).

      if (((Self->Flags & FL::NEW) != FL::NIL) and (get_file_info(Self->Path, &info, sizeof(info)) IS ERR::Okay)) {
         log.msg("Using permissions of the original file.");
         Self->Permissions |= info.Permissions;
      }
      else {
#ifdef __unix__
         Self->Permissions |= get_parent_permissions(Self->Path, nullptr, nullptr) & (PERMIT::ALL_READ|PERMIT::ALL_WRITE);
         if (Self->Permissions IS PERMIT::NIL) Self->Permissions = PERMIT::READ|PERMIT::WRITE|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE;
         else log.msg("Inherited permissions: $%.8x", int(Self->Permissions));
#else
         Self->Permissions = PERMIT::READ|PERMIT::WRITE|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE;
#endif
       }
   }

   // Do not do anything if the File is used as a static object in a script

   if (Self->Static and Self->Path.empty()) return ERR::Okay;

   if (Self->Path.starts_with(':')) {
      if ((Self->Flags & FL::FILE) != FL::NIL) return log.warning(ERR::ExpectedFile);
      log.trace("Root folder initialised.");
      return ERR::Okay;
   }

   // If the FL::FOLDER flag was defined AFTER the Path field was set, we may need to reset the Path field so
   // that the trailing folder slash is added to it.

retrydir:
   if ((Self->Flags & FL::FOLDER) != FL::NIL) {
      if ((!Self->Path.ends_with('/')) and (!Self->Path.ends_with('\\')) and (!Self->Path.ends_with(':'))) {
         std::string buffer(Self->Path);
         if (Self->setPath(buffer.c_str()) != ERR::Okay) {
            return log.warning(ERR::SetField);
         }
      }
   }

   if (Self->Stream) {
      log.trace("Folder stream already set.");
      return ERR::Okay;
   }

   // Use RSF::CHECK_VIRTUAL to cause failure if the volume name is reserved by a support class.  By doing this we can
   // return ERR::UseSubClass and a support class can then initialise the file instead.

   auto resolveflags = RSF::NIL;
   if ((Self->Flags & FL::NEW) != FL::NIL) resolveflags |= RSF::NO_FILE_CHECK;
   if ((Self->Flags & FL::APPROXIMATE) != FL::NIL) resolveflags |= RSF::APPROXIMATE;

   Self->prvResolvedPath.clear();
   if ((error = ResolvePath(Self->Path, resolveflags|RSF::CHECK_VIRTUAL, &Self->prvResolvedPath)) != ERR::Okay) {
      if (error IS ERR::VirtualVolume) {
         // For virtual volumes, update the path to ensure that the volume name is referenced in the path string.
         // Then return ERR::UseSubClass to have support delegated to the correct File sub-class.
         if (!iequals(Self->Path, Self->prvResolvedPath)) {
            SET_Path(Self, Self->prvResolvedPath.c_str());
         }
         log.trace("ResolvePath() reports virtual volume, will delegate to sub-class...");
         return ERR::UseSubClass;
      }
      else {
         // The file may path may actually be a folder.  Add a / and retest to see if this is the case.

         if ((Self->Flags & FL::FOLDER) IS FL::NIL) {
            Self->Flags |= FL::FOLDER;
            goto retrydir;
         }

         log.msg("File not found \"%s\".", Self->Path.c_str());
         return ERR::FileNotFound;
      }
   }

   // Check if ResolvePath() resolved the path from a file string to a folder

   if ((!Self->isFolder) and
       (Self->prvResolvedPath.ends_with('/') or Self->prvResolvedPath.ends_with('\\')) and
       ((Self->Flags & FL::FOLDER) IS FL::NIL)) {
      Self->Flags |= FL::FOLDER;
      goto retrydir;
   }

#ifdef __unix__
   // Establishing whether or not the path is a link is required on initialisation.
   struct stat64 info;
   if (Self->prvResolvedPath != "/") {
      if (Self->prvResolvedPath.ends_with('/')) Self->prvResolvedPath.pop_back(); // For lstat64() symlink we need to remove the slash
      if (lstat64(Self->prvResolvedPath.c_str(), &info) != -1) { // Prefer to get a stat on the link rather than the file it refers to
         if (S_ISLNK(info.st_mode)) Self->Flags |= FL::LINK;
      }
   }
#endif

   if (Self->isFolder) { // Open the folder
      if ((Self->Flags & FL::FILE) != FL::NIL) return log.warning(ERR::ExpectedFile);

      Self->Flags |= FL::FOLDER;

      acQuery(Self);

      #ifdef __unix__
         if ((Self->Stream = opendir(Self->prvResolvedPath.c_str()))) return ERR::Okay;
      #elif _WIN32
         // Note: The CheckDiretoryExists() function does not return a true handle, just a code of 1 to indicate that the folder is present.

         if ((Self->Stream = winCheckDirectoryExists(Self->prvResolvedPath.c_str()))) return ERR::Okay;
      #else
         #error Require folder open or folder marking code.
      #endif

      if ((Self->Flags & FL::NEW) != FL::NIL) {
         log.msg("Making dir \"%s\", Permissions: $%.8x", Self->prvResolvedPath.c_str(), int(Self->Permissions));
         if (CreateFolder(Self->prvResolvedPath.c_str(), Self->Permissions) IS ERR::Okay) {
            #ifdef __unix__
               if (!(Self->Stream = opendir(Self->prvResolvedPath.c_str()))) {
                  log.warning("Failed to open the folder after creating it.");
               }
            #elif _WIN32
               if (!(Self->Stream = winCheckDirectoryExists(Self->prvResolvedPath.c_str()))) {
                  log.warning("Failed to open the folder after creating it.");
               }
            #else
               #error Require folder open or folder marking code.
            #endif

            return ERR::Okay;
         }
         else return log.warning(ERR::CreateFile);
      }
      else {
         log.warning("Could not open folder \"%s\", %s.", Self->prvResolvedPath.c_str(), strerror(errno));
         return ERR::File;
      }
   }
   else {
      Self->Flags |= FL::FILE;

      // Automatically open the file if access is required on initialisation.

      if ((Self->Flags & (FL::NEW|FL::READ|FL::WRITE)) != FL::NIL) {
         ERR error = acActivate(Self);
         if (error IS ERR::Okay) error = acQuery(Self);
         return error;
      }
      else return acQuery(Self);
   }
}

/*********************************************************************************************************************

-METHOD-
Move: Moves a file to a new location.

This method is used to move the data of a file to another location.  If the file object represents a folder, then
the folder and all of its contents will be moved.  The file object must have been initialised with the `FL::READ` flag,
or the move operation will not work (this restriction does not apply to directories).  If a file already exists at the
destination path then it will be over-written with the new data.

The #Position field will be reset as a result of calling this method.

-INPUT-
cstr Dest: The desired path for the file.
ptr(func) Callback: Optional callback for receiving feedback during the operation.

-ERRORS-
Okay: The File was moved successfully.
NullArgs
Args
FieldNotSet: The #Path field has not been set in the file object.
InvalidPath: Attempted to move a volume.

*********************************************************************************************************************/

static ERR FILE_MoveFile(extFile *Self, struct fl::Move *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Dest) or (!Args->Dest[0])) return log.warning(ERR::NullArgs);
   if (Self->Path.empty()) return log.warning(ERR::FieldNotSet);

   auto src = std::string_view(Self->Path);
   auto dest = std::string_view(Args->Dest, strlen(Args->Dest));

   log.msg("%s to %s", src.data(), dest.data());

   if ((dest.ends_with('/')) or (dest.ends_with('\\')) or (dest.ends_with(':'))) {
      // If a trailing slash has been specified, we are moving the file into a folder, rather than to a direct path.

      while ((src.ends_with('/')) or (src.ends_with('\\'))) src.remove_suffix(1);

      if (src.ends_with(':')) return log.warning(ERR::InvalidPath);

      auto i = src.find_last_of(":/\\");
      auto folder = src.substr(0, i);
      auto newpath = std::string(dest);
      newpath.append(src.substr(i + 1));

      #ifdef _WIN32
         if (Self->Handle != -1) { close(Self->Handle); Self->Handle = -1; }
      #endif

      ERR error;
      if ((error = fs_copy(src, newpath, Args->Callback, true)) IS ERR::Okay) {
         Self->Path.assign(newpath);
      }
      else log.warning("Failed to move %s to %s", src.data(), newpath.data());

      return error;
   }
   else {
      #ifdef _WIN32
         if (Self->Handle != -1) { close(Self->Handle); Self->Handle = -1; }
      #endif

      if (auto error = fs_copy(src, dest, Args->Callback, true); error IS ERR::Okay) {
         Self->Path.assign(dest);
         return ERR::Okay;
      }
      else return log.warning(error);
   }
}

//********************************************************************************************************************

static ERR FILE_NewPlacement(extFile *Self)
{
   new (Self) extFile;
   Self->Handle = -1;
   Self->Permissions = PERMIT::READ|PERMIT::WRITE|PERMIT::GROUP_READ|PERMIT::GROUP_WRITE;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Next: Retrieve meta information describing the next indexed file in the folder list.

If a file object represents a folder, calling the Next() method will retrieve meta information about the next file
in the folder's index.  This information will be returned as a new File object that is partially initialised (the file
will not be opened, but information such as size, timestamps and permissions will be retrievable).

If desired, the resulting file object can be opened by setting the `READ` or `WRITE` bits in #Flags and then
calling the #Activate() action.

It is the responsibility of the caller to free the resulting File object once it is no longer required.

The file index can be reset by calling the #Reset() action.

-INPUT-
&!obj(File) File: A pointer to a new File object will be returned in this parameter if the call is successful.

-ERRORS-
Okay
Args
NullArgs
DirEmpty: The index has reached the end of the file list.

*********************************************************************************************************************/

static ERR FILE_NextFile(extFile* Self, struct fl::Next* Args) // Not to be confused with acNext()
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & FL::FOLDER) IS FL::NIL) return log.warning(ERR::ExpectedFolder);

   if (!Self->prvList) {
      auto flags = RDF::QUALIFY;

      if ((Self->Flags & FL::EXCLUDE_FOLDERS) != FL::NIL) flags |= RDF::FILE;
      else if ((Self->Flags & FL::EXCLUDE_FILES) != FL::NIL) flags |= RDF::FOLDER;
      else flags |= RDF::FILE|RDF::FOLDER;

      if (auto error = OpenDir(Self->Path.c_str(), flags, &Self->prvList); error != ERR::Okay) return error;
   }

   ERR error;
   if ((error = ScanDir(Self->prvList)) IS ERR::Okay) {
      std::string path(Self->Path);
      path.append(Self->prvList->Info->Name);

      if (auto file = extFile::create::global(fl::Path(path))) {
         Args->File = file;
         return ERR::Okay;
      }
      else return log.warning(ERR::CreateObject);
   }
   else {
      // Automatically close the list in the event of an error and repurpose the return code.  Subsequent
      // calls to Next() will start from the start of the file index.
      FreeResource(Self->prvList);
      Self->prvList = nullptr;
   }

   return error;
}

/*********************************************************************************************************************
-ACTION-
Query: Read a file's meta information from source.
-END-
*********************************************************************************************************************/

static ERR FILE_Query(extFile *Self)
{
#ifdef _WIN32
   return ERR::Okay;
#else
   return ERR::Okay;
#endif
}

/*********************************************************************************************************************

-ACTION-
Read: Reads data from a file.

Reads data from a file into the given buffer.  Increases the value in the #Position field by the amount of
bytes read from the file data.  The `FL::READ` bit in the #Flags field must have been set on file initialisation,
or the call will fail.

It is normal behaviour for this call to report success in the event that no data has been read from the file, e.g.
if the end of the file has been reached.  The `Result` parameter will be returned as zero in such cases.  Check the
current #Position against the #Size to confirm that the end has been reached.

-ERRORS-
Okay: The file information was read into the buffer.
Args
NullArgs
NotInitialised
OutOfRange: Invalid `Length` parameter.
FileReadFlag: The `FL::READ` flag was not specified on initialisation.
ExpectedFolder: The file object refers to a folder.
Failed: The file object refers to a folder, or the object is corrupt.
-END-

*********************************************************************************************************************/

static ERR FILE_Read(extFile *Self, struct acRead *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);
   else if (Args->Length == 0) return ERR::Okay;
   else if (Args->Length < 0) return ERR::OutOfRange;

   if ((Self->Flags & FL::READ) IS FL::NIL) return log.warning(ERR::FileReadFlag);

   if (Self->Buffer) {
      if ((Self->Flags & FL::LOOP) != FL::NIL) {
         // In loop mode, we must make the file buffer appear to be of infinite length in terms of the read/write
         // position marker.

         auto dest = (int8_t *)Args->Buffer;
         int len;
         for (int readlen=Args->Length; readlen > 0; readlen -= len) {
            if (Self->Size) len = Self->Size - (Self->Position % Self->Size); // Calculate amount of space ahead of us.
            else len = 0; // Avoid division by zero if the file size is zero.
            if (len > readlen) len = readlen; // Restrict the length of the read operation to the length of the destination.

            copymem(Self->Buffer + (Self->Position % Self->Size), dest, len);
            dest += len;
            Self->Position += len;
         }
/*
         readlen = Self->Position % Self->Size;
         for (len=0; len < Args->Length; len++) {
            ((BYTE *)Args->Buffer)[len] = Self->Buffer[readlen++];
            if (readlen >= Self->Size) readlen = 0;
         }
         Self->Position += Args->Length;
*/
         Args->Result = Args->Length;
         return ERR::Okay;
      }
      else {
         if (Self->Position + Args->Length > Self->Size) Args->Result = Self->Size - Self->Position;
         else Args->Result = Args->Length;
         copymem(Self->Buffer + Self->Position, Args->Buffer, Args->Result);
         Self->Position += Args->Result;
         return ERR::Okay;
      }
   }

   if (Self->isFolder) return log.warning(ERR::ExpectedFile);

   if (Self->Handle IS -1) return ERR::NotInitialised;

   Args->Result = read(Self->Handle, Args->Buffer, (int)Args->Length);

   if (Args->Result != Args->Length) {
      if (Args->Result IS -1) {
         log.msg("Failed to read %d bytes from the file.", Args->Length);
         Args->Result = 0;
         return ERR::SystemCall;
      }

      // Return ERR::Okay because even though not all data was read, this was not due to a failure.

      log.trace("%d of the requested %d bytes were read from the file.", Args->Result, Args->Length);
      Self->Position += Args->Result;
      return ERR::Okay;
   }
   else {
      Self->Position += Args->Result;
      return ERR::Okay;
   }
}

/*********************************************************************************************************************

-METHOD-
ReadLine: Reads the next line from the file.

Reads one line from the file into an internal buffer, which is returned in the Result argument.  Reading a line will
increase the #Position field by the amount of bytes read from the file.  You must have set the `FL::READ` bit in
the #Flags field when you initialised the file, or the call will fail.

The line buffer is managed internally, so there is no need to free the `Result` string.  `ERR::NoData` is returned
once all lines have been read.

-INPUT-
&str Result: The resulting string is returned in this parameter.

-ERRORS-
Okay: The file information was read into the buffer.
Args
FileReadFlag: The `FL::READ` flag was not specified on initialisation.
Failed: The file object refers to a folder.
ObjectCorrupt: The internal file handle is missing.
BufferOverflow: The line is too long for the read routine (4096 byte limit).
NoData: There is no more data left to read.

*********************************************************************************************************************/

static ERR FILE_ReadLine(extFile *Self, struct fl::ReadLine *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Self->Flags & FL::READ) IS FL::NIL) return log.warning(ERR::FileReadFlag);

   if (Self->Buffer) {
      if (Self->Position >= Self->Size) return ERR::NoData;
      auto content = std::string_view((char *)Self->Buffer, Self->Size);
      auto line_feed = content.find('\n', Self->Position);
      if (line_feed IS std::string::npos) {
         Self->prvLine.assign((char *)Self->Buffer, Self->Position);
         Self->Position = Self->Size;
      }
      else {
         Self->prvLine.assign((char *)Self->Buffer, Self->Position, line_feed - Self->Position);
         Self->Position = line_feed + 1;
      }
      return ERR::Okay;
   }
   else {
      if (Self->isFolder) return log.warning(ERR::ExpectedFile);
      if (Self->Handle IS -1) return log.warning(ERR::ObjectCorrupt);

      Self->prvLine.resize(4096); // We'll shrink it later
      int result;
      const int CHUNK = 256;
      std::size_t line_offset = 0;
      while ((result = read(Self->Handle, Self->prvLine.data()+line_offset, CHUNK)) > 0) {
         int i;
         for (i=0; (i < result) and (Self->prvLine[line_offset] != '\n'); i++, line_offset++);
         if (i < result) break;

         if (line_offset + CHUNK >= Self->prvLine.size()) {
            lseek64(Self->Handle, Self->Position, SEEK_SET); // Reset the file position back to normal
            return log.warning(ERR::BufferOverflow);
         }
      }

      if (!line_offset) return ERR::NoData;

      Self->Position += line_offset;
      if (Self->prvLine[line_offset] IS '\n') {
         Self->Position++; // Skip the line feed
         lseek64(Self->Handle, Self->Position, SEEK_SET); // Reset position to the start of the next line
      }

      Self->prvLine.resize(line_offset);
      Args->Result = Self->prvLine.data();
      return ERR::Okay;
   }
}

/*********************************************************************************************************************
-ACTION-
Rename: Changes the name of a file.
-END-
*********************************************************************************************************************/

static ERR FILE_Rename(extFile *Self, struct acRename *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Name) or (!Args->Name[0])) return log.warning(ERR::NullArgs);
   if (Self->Path.empty()) return log.warning(ERR::FieldNotSet);

   log.branch("%s to %s", Self->Path.c_str(), Args->Name);

   auto name = std::string_view(Args->Name, strlen(Args->Name));

   if ((Self->isFolder) or ((Self->Flags & FL::FOLDER) != FL::NIL)) {
      if (Self->Path.ends_with(':')) { // Renaming a volume
         std::string n(name, 0, name.find_first_of(":/\\"));
         if (RenameVolume(Self->Path.c_str(), n.c_str()) IS ERR::Okay) {
            Self->Path = n + ":";
            return ERR::Okay;
         }
         else return log.warning(ERR::SystemCall);
      }
      else { // We are renaming a folder
         std::string n(Self->Path, 0, Self->Path.find_last_of(":/\\"));
         n.append(name, 0, name.find_last_of("/\\:"));

         if (fs_copy(Self->Path, n, nullptr, true) IS ERR::Okay) {
            if (!n.ends_with('/')) Self->Path = n + "/";
            else Self->Path = n;
            return ERR::Okay;
         }
         else return log.warning(ERR::SystemCall);
      }
   }
   else { // We are renaming a file
      std::string n(Self->Path, 0, Self->Path.find_last_of(":/\\"));

      auto i = name.find_last_of("/\\:");
      if (i IS std::string::npos) n.append(name);
      else n.append(name, 0, i + 1);

      #ifdef _WIN32
         if (Self->Handle != -1) { close(Self->Handle); Self->Handle = -1; }
      #endif

      if (fs_copy(Self->Path, n, nullptr, true) IS ERR::Okay) {
         Self->Path = n;
         return ERR::Okay;
      }
      else return log.warning(ERR::SystemCall);
   }
}

/*********************************************************************************************************************
-ACTION-
Reset: If the file represents a folder, the file list index is reset by this action.
-END-
*********************************************************************************************************************/

static ERR FILE_Reset(extFile *Self)
{
   if ((Self->Flags & FL::FOLDER) != FL::NIL) {
      if (Self->prvList) { FreeResource(Self->prvList); Self->prvList = nullptr; }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Seek: Seeks to a new read/write position within a file.
-END-
*********************************************************************************************************************/

static ERR FILE_Seek(extFile *Self, struct acSeek *Args)
{
   pf::Log log;

   int64_t oldpos = Self->Position;

   // Set the new setting for the Self->Position field

   if (Args->Position IS SEEK::START) {
      Self->Position = (int64_t)Args->Offset;
   }
   else if (Args->Position IS SEEK::END) {
      Self->Position = Self->get<int64_t>(FID_Size) - (int64_t)Args->Offset;
   }
   else if (Args->Position IS SEEK::CURRENT) {
      Self->Position = Self->Position + (int64_t)Args->Offset;
   }
   else return log.warning(ERR::Args);

   // Make sure we are greater than zero, otherwise set as zero

   if (Self->Position < 0) Self->Position = 0;

   if (Self->Buffer) {
      if ((Self->Flags & FL::LOOP) != FL::NIL) return ERR::Okay; // In loop mode, the position marker can legally be above the buffer size
      else if (Self->Position > Self->Size) Self->Position = Self->Size;
      return ERR::Okay;
   }

   if (Self->Handle IS -1) return log.warning(ERR::ObjectCorrupt);

   int64_t ret;
   if ((ret = lseek64(Self->Handle, Self->Position, SEEK_SET)) != Self->Position) {
      log.warning("Failed to Seek to new position of %" PF64 " (return %" PF64 ").", (long long)Self->Position, (long long)ret);
      Self->Position = oldpos;
      return ERR::SystemCall;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetDate: Sets the date on a file.

The SetDate method provides a convenient way to set the date and time information for a file object.  Date information
is set in a human readable year, month, day, hour, minute and second format for your convenience.

Depending on the filesystem type, multiple forms of datestamp may be supported.  The default datestamp, `FDT::MODIFIED`
defines the time at which the file data was last altered.  Other types include the date on which the file was created
and the date it was last archived (backed up).  The following types are supported by the Type argument:

<types lookup="FDT"/>

If the specified datestamp is not supported by the filesystem, `ERR::NoSupport` is returned by this method.

-INPUT-
int Year: Year (-ve for BC, +ve for AD).
int Month: Month (1 - 12)
int Day: Day (1 - 31)
int Hour: Hour (0 - 23
int Minute: Minute (0 - 59)
int Second: Second (0 - 59)
int(FDT) Type: The type of date to set (filesystem dependent).

-ERRORS-
Okay
NullArgs
ResolvePath
SystemCall
NoSupport: The platform does not support file date setting.

*********************************************************************************************************************/

static ERR FILE_SetDate(extFile *Self, struct fl::SetDate *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.msg("%d/%d/%d %.2d:%.2d:%.2d", Args->Day, Args->Month, Args->Year, Args->Hour, Args->Minute, Args->Second);

   #ifdef _WIN32
      CSTRING path;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         auto result = winSetFileTime(path, Self->isFolder,
            Args->Year, Args->Month, Args->Day, Args->Hour, Args->Minute, Args->Second);

         if (result) {
            Self->Flags |= FL::RESET_DATE;
            return ERR::Okay;
         }
         else return log.warning(ERR::SystemCall);
      }
      else return ERR::ResolvePath;

   #elif __unix__

      CSTRING path;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         struct tm time;
         time.tm_year  = Args->Year - 1900;
         time.tm_mon   = Args->Month - 1;
         time.tm_mday  = Args->Day;
         time.tm_hour  = Args->Hour;
         time.tm_min   = Args->Minute;
         time.tm_sec   = Args->Second;
         time.tm_isdst = -1;
         time.tm_wday  = 0;
         time.tm_yday  = 0;

         struct timeval filetime[2];
         if ((filetime[0].tv_sec = mktime(&time)) != -1) {
            filetime[0].tv_usec = 0;
            filetime[1] = filetime[0];

            if (utimes(path, filetime) != -1) {
               Self->Flags |= FL::RESET_DATE;
               return ERR::Okay;
            }
            else {
               log.warning("Failed to set the file date.");
               return log.warning(ERR::SystemCall);
            }
         }
         else return log.warning(ERR::SystemCall);
      }
      else return ERR::ResolvePath;

   #else
      return ERR::NoSupport;
   #endif
}

/*********************************************************************************************************************

-METHOD-
StartStream: Starts streaming data from a file source.

If a file object is a stream (indicated by the `STREAM` flag), the StartStream method should be used for reading or
writing data to the file object.  Although it is possible to call the Read and Write actions on streamed files, they
will be limited to returning only the amount of data that is cached locally (if any), or writing as much as buffers
will allow in software.

A single file object can support read or write streams (pass `FL::READ` or `FL::WRITE` in the `Flags` parameter).
However, only one of the two can be active at any time.  To switch between read and write modes, the stream must be
stopped with the #StopStream() method and then restarted with StartStream.

A stream can be limited by setting the Length parameter to a non-zero value.

If the StartStream request is successful, the file object will return action notifications to the Subscriber to
indicate activity on the file stream.  When reading from a stream, `AC::Write` notifications will be received to indicate
that new data has been written to the file cache.  The Buffer parameter of the reported acWrite structure may refer to
a private address that contains the data that was received from the stream and the Result indicates the amount of new
data available.

When writing to a stream, `AC::Read` notifications will be received to indicate that the stream is ready to accept more
data.  The Result parameter will indicate the maximum amount of data that should be written to the stream using the
#Write() action.

A stream can be cancelled at any time by calling #StopStream().

-INPUT-
oid Subscriber: Reference to an object that will receive streamed data notifications.
int(FL) Flags: Use `READ` for incoming data, `WRITE` for outgoing data.
int Length: Limits the total amount of data to be streamed.

-ERRORS-
Okay
Args
NoSupport: The file is not streamed.

*********************************************************************************************************************/

static ERR FILE_StartStream(extFile *Self, struct fl::StartStream *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->SubscriberID)) return log.warning(ERR::NullArgs);

   // Streaming from standard files is pointless - it's the virtual drives that provide streaming features.

   return ERR::NoSupport;
}

/*********************************************************************************************************************

-METHOD-
StopStream: Stops streaming data from a file source.

This method terminates data streaming from a file (instantiated by the #StartStream() method).  Any resources
related to the streaming process will be deallocated.

-ERRORS-
Okay
Args
NoSupport: The file is not streamed.

*********************************************************************************************************************/

static ERR FILE_StopStream(extFile *Self)
{
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-METHOD-
Watch: Monitors files and folders for file system events.

The Watch() method configures event based reporting for changes to any file or folder in the file system.
The capabilities of this method are dependent on the host platform, with Windows and Linux systems being able to
support most of the current feature set.

The path that will be monitored is determined by the File object's #Path field.  Both files and folders
are supported as targets.

The optional !MFF Flags are used to filter events to those that are desired for monitoring.

The client must provide a `Callback` that will be triggered when a monitored event is triggered.  The `Callback` must
follow the format `ERR Routine(*File, STRING Path, INT64 Custom, INT Flags)`

Each event will be delivered in the sequence that they are originally raised.  The `Flags` parameter will reflect the
specific event that has occurred.  The `Custom` parameter is identical to the `Custom` argument originally passed to this
method.  The `Path` is a string that is relative to the File's #Path field.

If the callback routine returns `ERR::Terminate`, the watch will be disabled.  It is also possible to disable an existing
watch by calling this method with no parameters, or by setting the `Flags` parameter to `0`.

-INPUT-
ptr(func) Callback: The routine that will be called when a file change is triggered by the system.
large Custom: A custom 64-bit value that will passed to the Callback routine as a parameter.
int(MFF) Flags: Filter events to those indicated in these flags.

-ERRORS-
Okay
Args
NullArgs

*********************************************************************************************************************/

static ERR FILE_Watch(extFile *Self, struct fl::Watch *Args)
{
   pf::Log log;

   log.branch("%s, Flags: $%.8x", Self->Path.c_str(), (Args) ? int(Args->Flags) : 0);

   // Drop any previously configured watch.

   if (Self->prvWatch) {
      auto id = Self->prvWatch->VirtualID;
      if (glVirtual.contains(id)) {
         if (glVirtual[id].IgnoreFile) glVirtual[id].IgnoreFile(Self);
      }
      else log.warning("Failed to find virtual volume ID $%.8x", id);

      FreeResource(Self->prvWatch);
      Self->prvWatch = nullptr;
   }

   if ((!Args) or (!Args->Callback) or (Args->Flags IS MFF::NIL)) return ERR::Okay;

#ifdef __linux__ // Initialise inotify if not done already.
   if (glInotify IS -1) {
      ERR error;
      if ((glInotify = inotify_init()) != -1) {
         fcntl(glInotify, F_SETFL, fcntl(glInotify, F_GETFL)|O_NONBLOCK);
         error = RegisterFD(glInotify, RFD::READ, (void (*)(HOSTHANDLE, APTR))path_monitor, nullptr);
      }
      else error = log.warning(ERR::SystemCall);

      if (error != ERR::Okay) return error;
   }
#endif

   CSTRING resolve;
   ERR error;
   if ((error = GET_ResolvedPath(Self, &resolve)) IS ERR::Okay) {
      auto vd = get_fs(resolve);

      if (vd->WatchPath) {
         #ifdef _WIN32
         if (AllocMemory(sizeof(rkWatchPath) + winGetWatchBufferSize(), MEM::DATA, (APTR *)&Self->prvWatch, nullptr) IS ERR::Okay) {
         #else
         if (AllocMemory(sizeof(rkWatchPath), MEM::DATA, (APTR *)&Self->prvWatch, nullptr) IS ERR::Okay) {
         #endif
            Self->prvWatch->VirtualID = vd->VirtualID;
            Self->prvWatch->Routine   = *Args->Callback;
            Self->prvWatch->Flags     = Args->Flags;
            Self->prvWatch->Custom    = Args->Custom;

            error = vd->WatchPath(Self);
         }
         else error = ERR::AllocMemory;
      }
      else error = ERR::NoSupport;
   }

   return error;
}

/*********************************************************************************************************************

-ACTION-
Write: Writes data to a file.

Writes data from the provided buffer into the file, then updates the #Position field to reflect the new
read/write position.  You must have set the `FL::WRITE` bit in the #Flags field when you initialised the file, or
the call will fail.

-ERRORS-
Okay: All of the data was written to the file.
Args:
NullArgs:
ReallocMemory:
ExpectedFile:
ObjectCorrupt:
FileWriteFlag: The `FL::WRITE` flag was not specified when initialising the file.
LimitedSuccess: Only some of the data was written to the file.  Check the Result parameter to see how much data was written.
-END-

*********************************************************************************************************************/

static ERR FILE_Write(extFile *Self, struct acWrite *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if (Args->Length <= 0) return ERR::Args;
   if ((Self->Flags & FL::WRITE) IS FL::NIL) return log.warning(ERR::FileWriteFlag);

   if (Self->Buffer) {
      if ((Self->Flags & FL::LOOP) != FL::NIL) {
         // In loop mode, we must make the file buffer appear to be of infinite length in terms of the read/write
         // position marker.

         CSTRING src = (CSTRING)Args->Buffer;
         int len;
         for (int writelen=Args->Length; writelen > 0; writelen -= len) {
            len = Self->Size - (Self->Position % Self->Size); // Calculate amount of space ahead of us.
            if (len > writelen) len = writelen; // Restrict the length to the requested amount to write.

            copymem(src, Self->Buffer + (Self->Position % Self->Size), len);
            src += len;
            Self->Position += len;
         }

/*
         writelen = Self->Position % Self->Size;
         for (len=0; len < Args->Length; len++) {
            Self->Buffer[writelen++] = ((BYTE *)Args->Buffer)[len];
            if (writelen >= Self->Size) writelen = 0;
         }
         Self->Position += Args->Length;
*/

         Args->Result = Args->Length;
         return ERR::Okay;
      }
      else {
         if (Self->Position + Args->Length > Self->Size) {
            // Increase the size of the buffer to cater for the write.  A null byte (not included in the official size)
            // is always placed at the end.
            if (ReallocMemory(Self->Buffer, Self->Position + Args->Length + 1, (APTR *)&Self->Buffer, nullptr) IS ERR::Okay) {
               Self->Size = Self->Position + Args->Length;
               Self->Buffer[Self->Size] = 0;
            }
            else return log.warning(ERR::ReallocMemory);
         }

         Args->Result = Args->Length;

         copymem(Args->Buffer, Self->Buffer + Self->Position, Args->Result);

         Self->Position += Args->Result;
         return ERR::Okay;
      }
   }

   if ((Self->isFolder) or ((Self->Flags & FL::FOLDER) != FL::NIL)) return log.warning(ERR::ExpectedFile);

   if (Self->Handle IS -1) return log.warning(ERR::ObjectCorrupt);

   // If no buffer was supplied then we will write out null values to a limit indicated by the Length field.

   if (!Args->Buffer) {
      uint8_t nullbyte = 0;
      Args->Result = 0;
      for (int i=0; i < Args->Length; i++) {
         int result = write(Self->Handle, &nullbyte, 1);
         if (result IS -1) break;
         else {
            Self->Position += result;
            Args->Result += result;
         }
      }

      if (Self->Position > Self->Size) Self->Size = Self->Position;
   }
   else {
      Args->Result = write(Self->Handle, Args->Buffer, Args->Length);

      if (Args->Result > -1) {
         Self->Position += Args->Result;
         if (Self->Position > Self->Size) Self->Size = Self->Position;
      }
      else Args->Result = 0;
   }

   if (Args->Result != Args->Length) {
      log.msg("%d of the intended %d bytes were written to the file.", Args->Result, Args->Length);
      return ERR::LimitedSuccess;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Buffer: Points to the internal data buffer if the file content is held in memory.

If a file has been created with an internal buffer (by setting the `BUFFER` flag on creation), this field will point to
the address of that buffer.  The size of the buffer will match the #Size field.

*********************************************************************************************************************/

static ERR GET_Buffer(extFile *Self, APTR *Value, int *Elements)
{
   *Value = Self->Buffer;
   *Elements = Self->Size;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Created: The creation date stamp for the file.

The Created field returns the time at which the file was first created, if supported by the filesystem.  If not
supported directly, the most recent 'modification date' is normally returned.

To simplify time management, information is read and set via a !DateTime structure.

*********************************************************************************************************************/

static ERR GET_Created(extFile *Self, DateTime **Value)
{
   pf::Log log;

   *Value = 0;

   if (Self->Handle != -1) {
      struct stat64 stats;
      if (!fstat64(Self->Handle, &stats)) {
         // Timestamp has to match that produced by fs_getinfo()

         if (auto local = localtime(&stats.st_mtime)) {
            Self->prvCreated = DateTime {
               .Year   = int16_t(1900 + local->tm_year),
               .Month  = int8_t(local->tm_mon + 1),
               .Day    = int8_t(local->tm_mday),
               .Hour   = int8_t(local->tm_hour),
               .Minute = int8_t(local->tm_min),
               .Second = int8_t(local->tm_sec)
            };

            *Value = &Self->prvCreated;
            return ERR::Okay;
         }
         else return log.warning(ERR::SystemCall);
      }
      else return log.warning(ERR::SystemCall);
   }
   else {
      CSTRING path;
      ERR error;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         char buffer[512];
         int len = strcopy(path, buffer, sizeof(buffer));
         if ((buffer[len-1] IS '/') or (buffer[len-1] IS '\\')) buffer[len-1] = 0;

         struct stat64 stats;
         if (!stat64(buffer, &stats)) {
            // Timestamp has to match that produced by fs_getinfo()

            if (auto local = localtime(&stats.st_mtime)) {
               Self->prvCreated = {
                  .Year   = int16_t(1900 + local->tm_year),
                  .Month  = int8_t(local->tm_mon + 1),
                  .Day    = int8_t(local->tm_mday),
                  .Hour   = int8_t(local->tm_hour),
                  .Minute = int8_t(local->tm_min),
                  .Second = int8_t(local->tm_sec)
               };

               *Value = &Self->prvCreated;
               error = ERR::Okay;
            }
            else error = log.warning(ERR::SystemCall);
         }
         else error = log.warning(ERR::SystemCall);
      }
      else error = log.warning(ERR::ResolvePath);

      return error;
   }
}

/*********************************************************************************************************************
-FIELD-
Date: The 'last modified' date stamp on the file.

The Date field reflects the time at which the file was last modified.  It can also be used to set a new modification
date.  Please note that if the file is open for writing, then date-stamped, then modified; the file system driver
will overwrite the previously defined date stamp with the time at which the file was last written.

Information is read and set using a standard !DateTime structure.

*********************************************************************************************************************/

static ERR GET_Date(extFile *Self, DateTime **Value)
{
   pf::Log log;

   *Value = 0;

   if (Self->Handle != -1) {
      struct stat64 stats;
      ERR error;
      if (!fstat64(Self->Handle, &stats)) {
         // Timestamp has to match that produced by fs_getinfo()

         if (auto local = localtime(&stats.st_mtime)) {
            Self->prvModified = DateTime {
               .Year   = int16_t(1900 + local->tm_year),
               .Month  = int8_t(local->tm_mon + 1),
               .Day    = int8_t(local->tm_mday),
               .Hour   = int8_t(local->tm_hour),
               .Minute = int8_t(local->tm_min),
               .Second = int8_t(local->tm_sec)
            };

            *Value = &Self->prvModified;
            error = ERR::Okay;
         }
         else error = log.warning(ERR::SystemCall);
      }
      else error = log.warning(ERR::SystemCall);

      return error;
   }
   else {
      CSTRING path;
      ERR error;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         char buffer[512];
         int len = strcopy(path, buffer, sizeof(buffer));
         if ((buffer[len-1] IS '/') or (buffer[len-1] IS '\\')) buffer[len-1] = 0;

         struct stat64 stats;
         if (!stat64(buffer, &stats)) {
            // Timestamp has to match that produced by fs_getinfo()

            if (auto local = localtime(&stats.st_mtime)) {
               Self->prvModified = DateTime {
                  .Year   = int16_t(1900 + local->tm_year),
                  .Month  = int8_t(local->tm_mon + 1),
                  .Day    = int8_t(local->tm_mday),
                  .Hour   = int8_t(local->tm_hour),
                  .Minute = int8_t(local->tm_min),
                  .Second = int8_t(local->tm_sec)
               };

               *Value = &Self->prvModified;
               error = ERR::Okay;
            }
            else error = log.warning(ERR::SystemCall);
         }
         else error = log.warning(ERR::SystemCall);
      }
      else error = log.warning(ERR::ResolvePath);

      return error;
   }
}

ERR SET_Date(extFile *Self, DateTime *Date)
{
   pf::Log log;

   if (!Date) return log.warning(ERR::NullArgs);

#ifdef _WIN32
   CSTRING path;
   if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
      if (winSetFileTime(path, Self->isFolder, Date->Year, Date->Month, Date->Day, Date->Hour, Date->Minute, Date->Second)) {
         Self->Flags |= FL::RESET_DATE;
         return ERR::Okay;
      }
      else return log.warning(ERR::SystemCall);
   }
   else return log.warning(ERR::ResolvePath);

#elif __unix__

   CSTRING path;
   time_t datetime;
   struct utimbuf utm;
   if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
      struct tm time;
      time.tm_year  = Date->Year - 1900;
      time.tm_mon   = Date->Month - 1;
      time.tm_mday  = Date->Day;
      time.tm_hour  = Date->Hour;
      time.tm_min   = Date->Minute;
      time.tm_sec   = Date->Second;
      time.tm_isdst = -1;
      time.tm_wday  = 0;
      time.tm_yday  = 0;

      if ((datetime = mktime(&time)) != -1) {
         utm.modtime = datetime;
         utm.actime  = datetime;

         if (utime(path, &utm) != -1) {
            Self->Flags |= FL::RESET_DATE;
            return ERR::Okay;
         }
         else return log.warning(ERR::SystemCall);
      }
      else return log.warning(ERR::SystemCall);
   }
   else return ERR::ResolvePath;

#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-FIELD-
Flags: File flags and options.

Note: If setting flags post-initialisation, the following restrictions apply: 1. The file must not have been
activated (opened) yet; 2. Only the `NEW`, `READ` and `WRITE` flags can be utilised.

*********************************************************************************************************************/

static ERR SET_Flags(extFile *Self, FL Value)
{
   if (Self->initialised()) {
      if (Self->Handle != -1) return ERR::AlreadyDefined;
      else if ((Self->Flags & ~(FL::FILE|FL::FOLDER|FL::READ|FL::WRITE|FL::NEW)) IS FL::NIL) {
         Self->Flags |= Value & (FL::READ|FL::WRITE|FL::NEW); // Setting R/W/N flags is permitted post-initialisation if the file hasn't been activated yet.
      }
      else return ERR::AlreadyDefined;
   }
   else Self->Flags = Value;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Group: Retrieve or change the group ID of a file.

The group ID assigned to a file can be read from this field.  The ID is retrieved from the file system in real time in
case the ID has been changed after initialisation of the file object.

You can also change the group ID of a file by writing an integer value to this field.

If the file system does not support group ID's, `ERR::NoSupport` is returned.

*********************************************************************************************************************/

static ERR GET_Group(extFile *Self, int *Value)
{
#ifdef __unix__
   struct stat64 info;
   if (fstat64(Self->Handle, &info) IS -1) return ERR::FileNotFound;
   *Value = info.st_gid;
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

static ERR SET_Group(extFile *Self, int Value)
{
#ifdef __unix__
   pf::Log log;
   if (Self->initialised()) {
      log.msg("Changing group to #%d", Value);
      if (!fchown(Self->Handle, -1, Value)) return ERR::Okay;
      else return log.warning(convert_errno(errno, ERR::SystemCall));
   }
   else return log.warning(ERR::NotInitialised);
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************
-FIELD-
Handle: The native system handle for the file opened by the file object.

This field returns the native file system handle for the file opened by the file object.  The native handle may be an
integer or pointer value in 32 or 64-bit format.  In order to manage this issue in a multi-platform manner, the value
is returned as a 64-bit integer.

*********************************************************************************************************************/

static ERR GET_Handle(extFile *Self, int64_t *Value)
{
   *Value = Self->Handle;
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Icon: Returns an icon reference that is suitable for this file in the UI.

This field returns the name of the best icon to use when representing the file to the user, for instance in a file
list.  The icon style is determined by analysing the File's #Path.

The resulting string is returned in the format `icons:category/name` and will refer to an SVG file.

*********************************************************************************************************************/

static ERR GET_Icon(extFile *Self, CSTRING *Value)
{
   if (!Self->prvIcon.empty()) {
      *Value = Self->prvIcon.c_str();
      return ERR::Okay;
   }

   pf::SwitchContext context(Self);

   if (Self->Path.empty()) {
      Self->prvIcon = "icons:filetypes/empty";
      *Value = Self->prvIcon.c_str();
      return ERR::Okay;
   }

   // If the location is a volume, look the icon up in the SystemVolumes object

   if (Self->Path.ends_with(':')) {
      std::string icon("icons:folders/folder");

      if (auto lock = std::unique_lock{glmVolumes, 6s}) {
         std::string volume(Self->Path, 0, Self->Path.size()-1);

         if ((glVolumes.contains(volume)) and (glVolumes[volume].contains("Icon"))) {
            icon = "icons:" + glVolumes[volume]["Icon"];
         }
      }

      Self->prvIcon = icon;
      *Value = Self->prvIcon.c_str();
      return ERR::Okay;
   }

   FileInfo info;
   bool link = false;
   if (get_file_info(Self->Path, &info, sizeof(info)) IS ERR::Okay) {
      if ((info.Flags & RDF::LINK) != RDF::NIL) link = true;

      if ((info.Flags & RDF::VIRTUAL) != RDF::NIL) { // Virtual drives can specify custom icons, even for folders
         Self->prvIcon = info.Tags[0]["Icon"];
         *Value = Self->prvIcon.c_str();
         if (*Value) return ERR::Okay;
      }

      if ((info.Flags & RDF::FOLDER) != RDF::NIL) {
         if (link) Self->prvIcon = "icons:folders/folder_shortcut";
         else Self->prvIcon = "icons:folders/folder";
         *Value = Self->prvIcon.c_str();
         return ERR::Okay;
      }
   }

   if ((Self->Path.ends_with('/')) or (Self->Path.ends_with('\\'))) {
      if (link) Self->prvIcon = "icons:folders/folder_shortcut";
      else Self->prvIcon = "icons:folders/folder";
      *Value = Self->prvIcon.c_str();
      return ERR::Okay;
   }

   std::string icon;

   // Match file extensions first, because this saves us from having to open and read the file content.

   if (auto sep = Self->Path.find_last_of("."); sep != std::string::npos) {
      auto ext = Self->Path.substr(sep + 1, std::string::npos);
      if (auto ext_class_id = lookup_class_by_ext(CLASSID::NIL, ext); ext_class_id != CLASSID::NIL) {
         icon = glClassDB[ext_class_id].Icon;
      }
   }

   if (icon.empty()) {
      // Use IdentifyFile() to see if this file can be associated with a class

      CLASSID class_id, subclass_id;
      if (IdentifyFile(Self->Path.c_str(), CLASSID::NIL, &class_id, &subclass_id) IS ERR::Okay) {
         if (glClassDB.contains(subclass_id)) {
            auto &record = glClassDB[subclass_id];
            if (!record.Icon.empty()) icon = record.Icon;
         }

         if (icon.empty()) {
            auto &record = glClassDB[class_id];
            if (!record.Icon.empty()) icon = record.Icon;
         }
      }
   }

   if (icon.empty()) {
      if (link) Self->prvIcon = "icons:filetypes/empty_shortcut";
      else Self->prvIcon = "icons:filetypes/empty";
      *Value = Self->prvIcon.c_str();
      return ERR::Okay;
   }

   if (!pf::startswith("icons:", icon)) Self->prvIcon = "icons:" + icon;
   else Self->prvIcon = icon;

   *Value = Self->prvIcon.c_str();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Link: Returns the link path for symbolically linked files.

If a file represents a symbolic link (indicated by the `SYMLINK` flag setting) then reading the Link field will return
the link path.  No assurance is made as to the validity of the path.  If the path is not absolute, then the parent
folder containing the link will need to be taken into consideration when calculating the path that the link refers to.

*********************************************************************************************************************/

static ERR GET_Link(extFile *Self, STRING *Value)
{
#ifdef __unix__
   pf::Log log;
   std::string path;

   if (!Self->prvLink.empty()) { // The link has already been read previously, just re-use it
      *Value = Self->prvLink.data();
      return ERR::Okay;
   }

   *Value = nullptr;
   if ((Self->Flags & FL::LINK) != FL::NIL) {
      if (ResolvePath(Self->Path, RSF::NIL, &path) IS ERR::Okay) {
         if (path.ends_with('/')) path.pop_back();
         int i;
         char buffer[512];
         if (((i = readlink(path.c_str(), buffer, sizeof(buffer)-1)) > 0) and ((size_t)i < sizeof(buffer)-1)) {
            Self->prvLink.assign(buffer, 0, i);
            *Value = Self->prvLink.data();
         }

         if (*Value) return ERR::Okay;
         else return ERR::SystemCall;
      }
      else return ERR::ResolvePath;
   }

   return ERR::SanityCheckFailed;
#else
   return ERR::NoSupport;
#endif
}

static ERR SET_Link(extFile *Self, STRING Value)
{
#ifdef __unix__
   //symlink().
#endif
   return ERR::NoSupport;
}

/*********************************************************************************************************************
-FIELD-
Path: Specifies the location of a file or folder.

This field is required for initialisation and must either be in the format of a universal path string, or a path
that is compatible with the host system.  The standard format for a universal path is `volume:folder/file`, for
instance `kotuku:system/classes.bin`.

To reference a folder in a way that is distinct from a file, use a trailing slash as in `volume:folder/`.

Referencing a `volume:` is optional.  In the event that a volume is not defined, the current working path is used
as the point of origin.

The accepted method for referencing parent folders is `../`, which can be repeated for as many parent folders as needed
to traverse the folder hierarchy.

Access to `std*` handles is achieved by using the path strings `std:in`, `std:out` and `std:err`.

*********************************************************************************************************************/

static ERR GET_Path(extFile *Self, CSTRING *Value)
{
   if (!Self->Path.empty()) {
      *Value = Self->Path.c_str();
      return ERR::Okay;
   }
   else {
      *Value = nullptr;
      return ERR::FieldNotSet;
   }
}

static ERR SET_Path(extFile *Self, CSTRING Value)
{
   pf::Log log;

   if (Self->initialised()) return log.warning(ERR::Immutable);

   if (Self->Stream) {
      #ifdef __unix__
         closedir((DIR *)Self->Stream);
      #endif
      Self->Stream = 0;
   }
   else if (Self->Handle != -1) {
      close(Self->Handle);
      Self->Handle = -1;
   }

   if ((Value) and (*Value)) {
      std::string_view val;
      if (pf::startswith("string:", Value)) {
         int len;
         for (len=0; (Value[len]) and (Value[len] != '|'); len++);
         Self->Path.assign(Value, 0, len);
      }
      else {
         // If the path is set to ':' then this is the equivalent of asking for a folder list of all volumes in
         // the system.  No further initialisation is necessary in such a case.

         val = std::string_view(Value, strlen(Value));
         if (val == ":") {
            Self->Path.assign(":");
            Self->isFolder = true;
         }
         else {
            while (val.starts_with(':')) val.remove_prefix(1);
            auto sep = val.find('|');
            if (sep != std::string::npos) val = val.substr(0, sep);
            Self->Path.assign(val);

            // Check if the path is a folder/volume or a file

            if (Self->Path.ends_with(':') or Self->Path.ends_with('/') or Self->Path.ends_with('\\')) {
               Self->isFolder = true;
            }
            else if ((Self->Flags & FL::FOLDER) != FL::NIL) {
               Self->Path.append("/");
               Self->isFolder = true;
            }
         }
      }
   }
   else Self->Path.clear();

   Self->prvResolvedPath.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Permissions: Manages the permissions of a file.
Lookup: PERMIT
-END-
*********************************************************************************************************************/

static ERR GET_Permissions(extFile *Self, PERMIT *Value)
{
   pf::Log log;

   *Value = PERMIT::NIL;

#ifdef __unix__

   // Always read permissions straight off the disk rather than returning an internal field, because some other
   // process could always have changed the permission flags.

   CSTRING path;
   if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
      int i = strlen(path);
      while ((i >= 0) and (path[i] != '/') and (path[i] != ':') and (path[i] != '\\')) i--;
      if (path[i+1] IS '.') Self->Permissions = PERMIT::HIDDEN;
      else Self->Permissions = PERMIT::NIL;

      if (Self->Handle != -1) {
         struct stat64 info;
         if (fstat64(Self->Handle, &info) != -1) {
            Self->Permissions |= convert_fs_permissions(info.st_mode);
         }
         else return convert_errno(errno, ERR::SystemCall);
      }
      else if (Self->Stream) {
         struct stat64 info;
         if (stat64(path, &info) != -1) Self->Permissions |= convert_fs_permissions(info.st_mode);
         else return convert_errno(errno, ERR::SystemCall);
      }

      *Value = Self->Permissions;
      return ERR::Okay;
   }
   else return ERR::ResolvePath;

#elif _WIN32

   CSTRING path;
   if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
      winGetAttrib(path, (int *)(Value)); // Supports PERMIT::HIDDEN/ARCHIVE/OFFLINE/READ/WRITE
      return ERR::Okay;
   }
   else return ERR::ResolvePath;

#endif

   return ERR::NoSupport;
}

static ERR SET_Permissions(extFile *Self, PERMIT Value)
{
   if (!Self->initialised()) {
      Self->Permissions = Value;
      return ERR::Okay;
   }
   else return set_permissions(Self, Value);
}

//********************************************************************************************************************

static ERR set_permissions(extFile *Self, PERMIT Permissions)
{
   pf::Log log(__FUNCTION__);
#ifdef __unix__

   if (Self->Handle != -1) {
      int flags = 0;
      if ((Permissions & PERMIT::READ) != PERMIT::NIL)  flags |= S_IRUSR;
      if ((Permissions & PERMIT::WRITE) != PERMIT::NIL) flags |= S_IWUSR;
      if ((Permissions & PERMIT::EXEC) != PERMIT::NIL)  flags |= S_IXUSR;

      if ((Permissions & PERMIT::GROUP_READ) != PERMIT::NIL)  flags |= S_IRGRP;
      if ((Permissions & PERMIT::GROUP_WRITE) != PERMIT::NIL) flags |= S_IWGRP;
      if ((Permissions & PERMIT::GROUP_EXEC) != PERMIT::NIL)  flags |= S_IXGRP;

      if ((Permissions & PERMIT::OTHERS_READ) != PERMIT::NIL)  flags |= S_IROTH;
      if ((Permissions & PERMIT::OTHERS_WRITE) != PERMIT::NIL) flags |= S_IWOTH;
      if ((Permissions & PERMIT::OTHERS_EXEC) != PERMIT::NIL)  flags |= S_IXOTH;

      int err = fchmod(Self->Handle, flags);

      // Note that you need to be root to set the UID/GID flags, so we do it in this subsequent fchmod() call.

      if ((err != -1) and ((Permissions & (PERMIT::USERID|PERMIT::GROUPID)) != PERMIT::NIL)) {
         if ((Permissions & PERMIT::USERID) != PERMIT::NIL)  flags |= S_ISUID;
         if ((Permissions & PERMIT::GROUPID) != PERMIT::NIL) flags |= S_ISGID;
         err = fchmod(Self->Handle, flags);
      }

      if (err != -1) {
         Self->Permissions = Permissions;
         return ERR::Okay;
      }
      else return convert_errno(errno, ERR::SystemCall);
   }
   else if (Self->Stream) {
      // File represents a folder

      CSTRING path;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         int flags = 0;
         if ((Permissions & PERMIT::READ) != PERMIT::NIL)  flags |= S_IRUSR;
         if ((Permissions & PERMIT::WRITE) != PERMIT::NIL) flags |= S_IWUSR;
         if ((Permissions & PERMIT::EXEC) != PERMIT::NIL)  flags |= S_IXUSR;

         if ((Permissions & PERMIT::GROUP_READ) != PERMIT::NIL)  flags |= S_IRGRP;
         if ((Permissions & PERMIT::GROUP_WRITE) != PERMIT::NIL) flags |= S_IWGRP;
         if ((Permissions & PERMIT::GROUP_EXEC) != PERMIT::NIL)  flags |= S_IXGRP;

         if ((Permissions & PERMIT::OTHERS_READ) != PERMIT::NIL)  flags |= S_IROTH;
         if ((Permissions & PERMIT::OTHERS_WRITE) != PERMIT::NIL) flags |= S_IWOTH;
         if ((Permissions & PERMIT::OTHERS_EXEC) != PERMIT::NIL)  flags |= S_IXOTH;

         if ((Permissions & PERMIT::GROUPID) != PERMIT::NIL) flags |= S_ISGID;
         if ((Permissions & PERMIT::USERID) != PERMIT::NIL)  flags |= S_ISUID;

         if (chmod(path, flags) != -1) {
            Self->Permissions = Permissions;
            return ERR::Okay;
         }
         else return log.warning(convert_errno(errno, ERR::SystemCall));
      }
      else return log.warning(ERR::ResolvePath);
   }
   else return log.warning(ERR::InvalidHandle);

#elif _WIN32

   log.branch("$%.8x", int(Permissions));

   CSTRING path;
   if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
      ERR error;
      if (winSetAttrib(path, int(Permissions))) error = log.warning(ERR::SystemCall);
      else error = ERR::Okay;
      return error;
   }
   else return log.warning(ERR::ResolvePath);

#else

   return ERR::NoSupport;

#endif
}

/*********************************************************************************************************************
-FIELD-
Position: The current read/write byte position in a file.

This field indicates the current byte position of an open file (this affects read and write operations).  Writing to
this field performs a #Seek() operation.

The Position will always remain at zero if the file object represents a folder.

*********************************************************************************************************************/

static ERR SET_Position(extFile *Self, int64_t Value)
{
   if (Self->initialised()) {
      return acSeekStart(Self, Value);
   }
   else {
      Self->Position = Value;
      return ERR::Okay;
   }
}

/*********************************************************************************************************************
-FIELD-
ResolvedPath: Returns a resolved copy of the Path string.

The ResolvedPath will return a resolved copy of the #Path string.  The resolved path will be in a format that is native
to the host platform.  Please refer to the ~ResolvePath() function for further information.

*********************************************************************************************************************/

static ERR GET_ResolvedPath(extFile *Self, CSTRING *Value)
{
   if (Self->Path.empty()) return ERR::FieldNotSet;

   if (Self->prvResolvedPath.empty()) {
      auto flags = ((Self->Flags & FL::APPROXIMATE) != FL::NIL) ? RSF::APPROXIMATE : RSF::NO_FILE_CHECK;
      if (ResolvePath(Self->Path, flags, &Self->prvResolvedPath) != ERR::Okay) return ERR::ResolvePath;
   }

   *Value = Self->prvResolvedPath.c_str();
   return ERR::Okay;
}

/*********************************************************************************************************************
-FIELD-
Size: The byte size of a file.

The current byte size of a file is indicated by this field.  If the file object represents a folder, the Size value
will be zero.  You can also truncate a file by setting the Size; this will result in the current read/write
position being set to the end of the file.

*********************************************************************************************************************/

static ERR GET_Size(extFile *Self, int64_t *Size)
{
   pf::Log log;

   if ((Self->Flags & FL::FOLDER) != FL::NIL) {
      *Size = 0;
      return ERR::Okay;
   }
   else if (Self->Handle != -1) {
      struct stat64 stats;
      if (!fstat64(Self->Handle, &stats)) {
         *Size = stats.st_size;
         return ERR::Okay;
      }
      else return convert_errno(errno, ERR::SystemCall);
   }
   else if (Self->Buffer) {
      *Size = Self->Size;
      return ERR::Okay;
   }

   CSTRING path;
   if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
      struct stat64 stats;
      if (!stat64(path, &stats)) {
         *Size = stats.st_size;
         log.trace("The file size is %" PF64, (long long)*Size);
         return ERR::Okay;
      }
      else return convert_errno(errno, ERR::SystemCall);
   }
   else return log.warning(ERR::ResolvePath);
}

static ERR SET_Size(extFile *Self, int64_t Size)
{
   pf::Log log;

   if (Size IS Self->Size) return ERR::Okay;
   if (Size < 0) return log.warning(ERR::OutOfRange);

   if (Self->Buffer) {
      if (Self->initialised()) return ERR::NoSupport;
      else Self->Size = Size;

      if (Self->Position > Self->Size) acSeekStart(Self, Size);
      return ERR::Okay;
   }

   if (!Self->initialised()) {
      Self->Size = Size;
      if (Self->Position > Self->Size) acSeekStart(Self, Size);
      return ERR::Okay;
   }

#ifdef _WIN32
   CSTRING path;

   if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
      if (winSetEOF(path, Size)) {
         acSeek(Self, 0.0, SEEK::END);
         Self->Size = Size;
         if (Self->Position > Self->Size) acSeekStart(Self, Size);
         return ERR::Okay;
      }
      else {
         log.warning("Failed to set file size to %" PF64, (long long)Size);
         return ERR::SystemCall;
      }
   }
   else return log.warning(ERR::ResolvePath);

#elif __unix__

   #ifdef __ANDROID__
   #warning Support for ftruncate64() required for Android build.
   if (!ftruncate(Self->Handle, Size)) {
   #else
   if (!ftruncate64(Self->Handle, Size)) {
   #endif
      Self->Size = Size;
      if (Self->Position > Self->Size) acSeekStart(Self, Size);
      return ERR::Okay;
   }
   else {
      // Some filesystem drivers do not support truncation for the purpose of
      // enlarging files.  In this case, we have to write to the end of the file.

      log.warning("%" PF64 " bytes, ftruncate: %s", (long long)Size, strerror(errno));

      if (Size > Self->Size) {
         CSTRING path;

         // Seek past the file boundary and write a single byte to expand the file.  Yes, it's legal and works.

         ERR error;
         if ((error = GET_ResolvedPath(Self, &path)) IS ERR::Okay) {
            struct statfs fstat;
            if (statfs(path, &fstat) != -1) {
               if (Size < (int64_t)fstat.f_bavail * (int64_t)fstat.f_bsize) {
                  log.msg("Attempting to use the write-past-boundary method.");

                  if (lseek64(Self->Handle, Size - 1, SEEK_SET) != -1) {
                     char c = 0;
                     if (write(Self->Handle, &c, 1) IS 1) {
                        lseek64(Self->Handle, Self->Position, SEEK_SET);
                        Self->Size = Size;
                        if (Self->Position > Self->Size) acSeekStart(Self, Size);
                        return ERR::Okay;
                     }
                     else return convert_errno(errno, ERR::SystemCall);
                  }
                  else return convert_errno(errno, ERR::SystemCall);
               }
               else return log.warning(ERR::OutOfSpace);
            }
            else return convert_errno(errno, ERR::SystemCall);
         }
         else return ERR::ResolvePath;
      }
      else return ERR::SystemCall;
   }
#else
   log.trace("No support for truncating file sizes on this platform.");
   return log.warning(ERR::NoSupport);
#endif
}

/*********************************************************************************************************************

-FIELD-
Static: Set to `true` if a file object should be static.

This field applies when a file object has been created in an object script.  By default, a file object will
auto-terminate when a closing tag is received.  If the object must remain live, set this field to `true`.

-FIELD-
Target: Specifies a surface ID to target for user feedback and dialog boxes.

User feedback can be enabled for certain file operations by setting the Target field to a valid surface ID, or zero
for the default target for new windows.  This field is set to `-1` by default, in order to disable this feature.

If set correctly, operations such as file deletion or copying will pop-up a progress box after a certain amount of time
has elapsed during the operation.  The dialog box will also provide the user with a cancel option to terminate the
process early.

-FIELD-
TimeStamp: The last modification time set on a file, represented as a 64-bit integer.

The TimeStamp field is a 64-bit representation of the last modification date/time set on a file.  It is not guaranteed
that the value represents seconds from the epoch, so it should only be used for purposes such as sorting, or
for comparison to the time stamps of other files.  For a parsed time structure, refer to the #Date field.

*********************************************************************************************************************/

static ERR GET_TimeStamp(extFile *Self, int64_t *Value)
{
   pf::Log log;

   *Value = 0;

   if (Self->Handle != -1) {
      struct stat64 stats;
      if (!fstat64(Self->Handle, &stats)) {
         // Timestamp has to match that produced by fs_getinfo()

         if (auto local = localtime(&stats.st_mtime)) {
            DateTime datetime = {
               .Year   = int16_t(1900 + local->tm_year),
               .Month  = int8_t(local->tm_mon + 1),
               .Day    = int8_t(local->tm_mday),
               .Hour   = int8_t(local->tm_hour),
               .Minute = int8_t(local->tm_min),
               .Second = int8_t(local->tm_sec)
            };

            *Value = calc_timestamp(&datetime);
            return ERR::Okay;
         }
         else return convert_errno(errno, ERR::SystemCall);
      }
      else return convert_errno(errno, ERR::SystemCall);
   }
   else {
      CSTRING path;
      if (GET_ResolvedPath(Self, &path) IS ERR::Okay) {
         struct stat64 stats;
         if (!stat64(path, &stats)) {
            if (auto local = localtime(&stats.st_mtime)) {
               DateTime datetime = {
                  .Year   = int16_t(1900 + local->tm_year),
                  .Month  = int8_t(local->tm_mon + 1),
                  .Day    = int8_t(local->tm_mday),
                  .Hour   = int8_t(local->tm_hour),
                  .Minute = int8_t(local->tm_min),
                  .Second = int8_t(local->tm_sec)
               };

               *Value = calc_timestamp(&datetime);
               return ERR::Okay;
            }
            else return convert_errno(errno, ERR::SystemCall);
         }
         else return convert_errno(errno, ERR::SystemCall);
      }
      else return log.warning(ERR::ResolvePath);
   }
}

/*********************************************************************************************************************
-FIELD-
User: Retrieve or change the user ID of a file.

The user ID assigned to a file can be read from this field.  The ID is retrieved from the file system in real time in
case the ID has been changed after initialisation of the file object.

You can also change the user ID of a file by writing an integer value to this field.  This can only be done
post-initialisation or an error code will be returned.

If the filesystem does not support user ID's, `ERR::NoSupport` is returned.
-END-
*********************************************************************************************************************/

static ERR GET_User(extFile *Self, int *Value)
{
#ifdef __unix__
   struct stat64 info;

   if (fstat64(Self->Handle, &info) IS -1) return ERR::FileNotFound;

   *Value = info.st_uid;
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

static ERR SET_User(extFile *Self, int Value)
{
#ifdef __unix__
   pf::Log log;
   if (Self->initialised()) {
      log.msg("Changing user to #%d", Value);
      if (!fchown(Self->Handle, Value, -1)) {
         return ERR::Okay;
      }
      else return log.warning(convert_errno(errno, ERR::SystemCall));
   }
   else return log.warning(ERR::SystemCall);
#else
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************

static const FieldDef PermissionFlags[] = {
   { "Read",         PERMIT::READ },
   { "Write",        PERMIT::WRITE },
   { "Exec",         PERMIT::EXEC },
   { "Executable",   PERMIT::EXEC },
   { "Delete",       PERMIT::DELETE },
   { "Hidden",       PERMIT::HIDDEN },
   { "Archive",      PERMIT::ARCHIVE },
   { "Password",     PERMIT::PASSWORD },
   { "UserID",       PERMIT::USERID },
   { "GroupID",      PERMIT::GROUPID },
   { "OthersRead",   PERMIT::OTHERS_READ },
   { "OthersWrite",  PERMIT::OTHERS_WRITE },
   { "OthersExec",   PERMIT::OTHERS_EXEC },
   { "OthersDelete", PERMIT::OTHERS_DELETE },
   { "GroupRead",    PERMIT::GROUP_READ },
   { "GroupWrite",   PERMIT::GROUP_WRITE },
   { "GroupExec",    PERMIT::GROUP_EXEC },
   { "GroupDelete",  PERMIT::GROUP_DELETE },
   { "AllRead",      PERMIT::ALL_READ },
   { "AllWrite",     PERMIT::ALL_WRITE },
   { "AllExec",      PERMIT::ALL_EXEC },
   { "UserRead",     PERMIT::READ },
   { "UserWrite",    PERMIT::WRITE },
   { "UserExec",     PERMIT::EXEC },
   { nullptr, 0 }
};

#include "class_file_def.c"

static const FieldArray FileFields[] = {
   { "Position",     FDF_INT64|FDF_RW, nullptr, SET_Position },
   { "Flags",        FDF_INTFLAGS|FDF_RW, nullptr, SET_Flags, &clFileFlags },
   { "Static",       FDF_INT|FDF_RI },
   { "Target",       FDF_OBJECTID|FDF_RW, nullptr, nullptr, CLASSID::SURFACE },
   { "Buffer",       FDF_ARRAY|FDF_BYTE|FDF_R, GET_Buffer },
   // Virtual fields
   { "Date",         FDF_POINTER|FDF_STRUCT|FDF_RW, GET_Date, SET_Date, "DateTime" },
   { "Created",      FDF_POINTER|FDF_STRUCT|FDF_RW, GET_Created, nullptr, "DateTime" },
   { "Handle",       FDF_INT64|FDF_R,     GET_Handle },
   { "Icon",         FDF_STRING|FDF_R,    GET_Icon },
   { "Path",         FDF_STRING|FDF_RI,   GET_Path, SET_Path },
   { "Permissions",  FDF_INTFLAGS|FDF_RW, GET_Permissions, SET_Permissions, &PermissionFlags },
   { "ResolvedPath", FDF_STRING|FDF_R,    GET_ResolvedPath },
   { "Size",         FDF_INT64|FDF_RW,    GET_Size, SET_Size },
   { "TimeStamp",    FDF_INT64|FDF_R,     GET_TimeStamp },
   { "Link",         FDF_STRING|FDF_RW,   GET_Link, SET_Link },
   { "User",         FDF_INT|FDF_RW,      GET_User, SET_User },
   { "Group",        FDF_INT|FDF_RW,      GET_Group, SET_Group },
   // Synonyms
   { "Src",      FDF_STRING|FDF_SYNONYM|FDF_RI, GET_Path, SET_Path },
   { "Location", FDF_STRING|FDF_SYNONYM|FDF_RI, GET_Path, SET_Path },
   END_FIELD
};

//********************************************************************************************************************

extern ERR add_file_class(void)
{
   glFileClass = extMetaClass::create::global(
      fl::ClassVersion(VER_FILE),
      fl::Name("File"),
      fl::Category(CCF::SYSTEM),
      fl::Actions(clFileActions),
      fl::Methods(clFileMethods),
      fl::Fields(FileFields),
      fl::Size(sizeof(extFile)),
      fl::Path("modules:core"));

   return glFileClass ? ERR::Okay : ERR::AddClass;
}
