/*********************************************************************************************************************
-CATEGORY-
Name: Files
-END-
*********************************************************************************************************************/

class extDirInfo : public DirInfo {
public:
   extDirInfo() {
      Info            = &prvInfo;
      Driver          = nullptr;
      prvHandle       = nullptr;
      prvPath         = nullptr;
      prvResolvedPath = nullptr;
      prvFlags        = RDF::OPENDIR;
      prvTotal        = 0;
      prvVirtualID    = DEFAULT_VIRTUALID;
      prvIndex        = 0;
      prvResolveLen   = 0;

      prvInfo      = { };
      prvInfo.Name = prvName;
      prvName[0]   = 0;

      #ifdef _WIN32
         prvHandle = (WINHANDLE)-1;
      #endif
   }

   ~extDirInfo() {
      if (prvDriverStorage) {
         ::operator delete(prvDriverStorage);
         prvDriverStorage = nullptr;
      }

      delete[] prvResolvedPathBuffer;
      delete[] prvPathBuffer;
   }

   ERR initialise(std::string_view Path, std::string_view ResolvedPath, RDF Flags, int DriverSize) {
      auto resolved_len = ResolvedPath.size() + 1;

      auto path_buffer = new (std::nothrow) char[Path.size() + 1];
      if (not path_buffer) return ERR::AllocMemory;

      auto resolved_buffer = new (std::nothrow) char[resolved_len + 1];
      if (not resolved_buffer) {
         delete[] path_buffer;
         return ERR::AllocMemory;
      }

      APTR driver_storage = nullptr;
      if (DriverSize > 0) {
         driver_storage = ::operator new((size_t)DriverSize, std::nothrow);
         if (not driver_storage) {
            delete[] resolved_buffer;
            delete[] path_buffer;
            return ERR::AllocMemory;
         }
      }

      prvPathBuffer = path_buffer;
      prvResolvedPathBuffer = resolved_buffer;
      prvDriverStorage = driver_storage;

      Info = &prvInfo;
      Info->Name = prvName;
      Driver = prvDriverStorage;
      prvPath = prvPathBuffer;
      prvResolvedPath = prvResolvedPathBuffer;
      prvFlags = Flags | RDF::OPENDIR;
      prvVirtualID = DEFAULT_VIRTUALID;
      prvResolveLen = resolved_len;

      prvInfo = { };
      prvInfo.Name = prvName;
      prvName[0] = 0;

      copymem(Path.data(), prvPathBuffer, Path.size() + 1);
      copymem(ResolvedPath.data(), prvResolvedPathBuffer, resolved_len);
      prvResolvedPathBuffer[resolved_len] = 0; // Reserve one extra byte for temporary win32 wildcard expansion.

      return ERR::Okay;
   }

private:
   FileInfo prvInfo;
   char prvName[MAX_FILENAME];
   char *prvPathBuffer = nullptr;
   char *prvResolvedPathBuffer = nullptr;
   APTR prvDriverStorage = nullptr;
};

static ERR folder_free(APTR Address)
{
   pf::Log log("CloseDir");
   auto folder = (extDirInfo *)Address;

   // Note: Virtual file systems should focus on destroying handles as fs_closedir() will take care of memory and list
   // deallocations.

   if ((folder->prvVirtualID) and (folder->prvVirtualID != DEFAULT_VIRTUALID)) {
      auto id = folder->prvVirtualID;
      if (glVirtual.contains(id)) {
         log.trace("Virtual file driver function @ %p", glVirtual[id].CloseDir);
         if (glVirtual[id].CloseDir) glVirtual[id].CloseDir(folder);
      }
   }

   fs_closedir(folder);
   folder->~extDirInfo();
   return ERR::Okay;
}

static ResourceManager glResourceFolder = {
   "Folder",
   &folder_free
};

/*********************************************************************************************************************

-FUNCTION-
OpenDir: Opens a folder for content scanning.

The OpenDir() function is used to open a folder for scanning via the ~ScanDir() function.  If the provided Path can be
accessed, a !DirInfo structure will be returned in the Info parameter, which will need to be passed to ~ScanDir().  Once
the scanning process is complete, call the ~FreeResource() function.

When opening a folder, it is necessary to indicate the type of files that are of interest.  If no flags are defined,
the scanner will return file and folder names only.  Only a subset of the available `RDF` flags may be used, namely
`SIZE`, `DATE`, `PERMISSIONS`, `FILE`, `FOLDER`, `QUALIFY`, `TAGS`.

-INPUT-
cstr Path: The folder location to be scanned.  Using an empty string will scan for volume names.
int(RDF) Flags: Optional flags.
!resource(DirInfo) Info: A !DirInfo structure will be returned in the pointer referenced here.

-ERRORS-
Okay
Args
NullArgs
DirEmpty
AllocMemory

*********************************************************************************************************************/

ERR OpenDir(CSTRING Path, RDF Flags, DirInfo **Result)
{
   pf::Log log(__FUNCTION__);

   if ((not Path) or (not Result)) return log.warning(ERR::NullArgs);

   log.traceBranch("Path: '%s'", Path);

   *Result = nullptr;

   if ((Flags & (RDF::FOLDER|RDF::FILE)) IS RDF::NIL) Flags |= RDF::FOLDER|RDF::FILE;

   std::string resolved_path;
   if (not Path[0]) Path = ":"; // A path of ':' will return all known volumes.
   if (auto error = ResolvePath(Path, RSF::NIL, &resolved_path); error IS ERR::Okay) {
      auto vd = get_fs(resolved_path);

      extDirInfo *dir;
      if (AllocMemory(sizeof(extDirInfo), MEM::DATA|MEM::MANAGED, (APTR *)&dir, nullptr) != ERR::Okay) {
         return ERR::AllocMemory;
      }

      new (dir) extDirInfo();
      SetResourceMgr(dir, &glResourceFolder);
      if (auto error = dir->initialise(Path, resolved_path, Flags, vd->DriverSize); error != ERR::Okay) {
         FreeResource(dir);
         return error;
      }

      if ((Path[0] IS ':') or (not Path[0])) {
         if ((Flags & RDF::FOLDER) IS RDF::NIL) {
            FreeResource(dir);
            return ERR::DirEmpty;
         }
         *Result = dir;
         return ERR::Okay;
      }

      if (not vd->OpenDir) {
         FreeResource(dir);
         return ERR::DirEmpty;
      }

      if ((error = vd->OpenDir(dir)) IS ERR::Okay) {
         dir->prvVirtualID = vd->VirtualID;
         *Result = dir;
         return ERR::Okay;
      }

      FreeResource(dir);
      return error;
   }
   else return log.warning(ERR::ResolvePath);
}

/*********************************************************************************************************************

-FUNCTION-
ScanDir: Scans the content of a folder, by item.

The ScanDir() function is used to scan for files and folders in a folder that you have opened using the
~OpenDir() function. The ScanDir() function is intended to be used in a simple loop, returning a single item
for each function call that you make.  The following code sample illustrates typical usage:

<pre>
DirInfo *info;
if (not OpenDir(path, RDF::FILE|RDF::FOLDER, &info)) {
   while (not ScanDir(info)) {
      log.msg("File: %s", info->Name);
   }
   FreeResource(info);
}
</pre>

For each item that you scan, you will be able to read the Info structure for information on that item.  The !DirInfo
structure contains a !FileInfo pointer that consists of the following fields:

<struct lookup="FileInfo"/>

The `RDF` flags that may be returned in the Flags field are `VOLUME`, `FOLDER`, `FILE`, `LINK`.

-INPUT-
resource(DirInfo) Info: Pointer to a !DirInfo structure for storing scan results.

-ERRORS-
Okay: An item was successfully scanned from the folder.
Args
DirEmpty: There are no more items to scan.
-END-

*********************************************************************************************************************/

ERR ScanDir(DirInfo *Dir)
{
   pf::Log log(__FUNCTION__);

   if (not Dir) return log.warning(ERR::NullArgs);

   FileInfo *file;
   if (not (file = Dir->Info)) { log.trace("Missing Dir->Info"); return log.warning(ERR::InvalidData); }
   if (not file->Name) { log.trace("Missing Dir->Info->Name"); return log.warning(ERR::InvalidData); }

   file->Name[0] = 0;
   file->Flags   = RDF::NIL;
   file->Permissions = PERMIT::NIL;
   file->Size    = 0;
   file->UserID  = 0;
   file->GroupID = 0;

   if (file->Tags) { delete file->Tags; file->Tags = nullptr; }

   // Support for scanning of volume names

   if ((Dir->prvPath[0] IS ':') or (not Dir->prvPath[0])) {
      if (auto lock = std::unique_lock{glmVolumes, 4s}) {
         int count = 0;
         for (auto const &pair : glVolumes) {
            if (count IS Dir->prvIndex) {
               Dir->prvIndex++;
               auto &volume = pair.first;
               int j = strcopy(volume, file->Name, MAX_FILENAME-2);
               if ((Dir->prvFlags & RDF::QUALIFY) != RDF::NIL) {
                  file->Name[j++] = ':';
                  file->Name[j] = 0;
               }

               if (glVolumes[volume]["Hidden"] == "Yes") {
                  file->Flags |= RDF::HIDDEN;
               }

               if (glVolumes[volume].contains("Label")) {
                  AddInfoTag(file, "Label", glVolumes[volume]["Label"].c_str());
               }

               file->Flags |= RDF::VOLUME;
               return ERR::Okay;
            }
            else count++;
         }

         return ERR::DirEmpty;
      }
      else return log.warning(ERR::SystemLocked);
   }

   // In all other cases, pass functionality to the filesystem driver.

   ERR error = ERR::NoSupport;
   if (Dir->prvVirtualID IS DEFAULT_VIRTUALID) {
      error = fs_scandir(Dir);
   }
   else if (glVirtual.contains(Dir->prvVirtualID)) {
      if (glVirtual[Dir->prvVirtualID].ScanDir) error = glVirtual[Dir->prvVirtualID].ScanDir(Dir);
   }

   if ((file->Name[0]) and ((Dir->prvFlags & RDF::DATE) != RDF::NIL)) {
      file->TimeStamp = calc_timestamp(&file->Modified);
   }

   return error;
}
