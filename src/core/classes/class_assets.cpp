/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
FileAssets: For Android systems only.  The FileAssets sub-class provides access to the assets folder in the currently running Android project.
-END-

*********************************************************************************************************************/

//#define DEBUG

#define __system__
#define PRV_FILE
#define PRV_FILESYSTEM

#include <android/asset_manager_jni.h>
#include <android/asset_manager.h>
#include <unistd.h>

#define VER_FileAssets 1.0

static OBJECTPTR glAssetClass = nullptr;

extern CoreBase *CoreBase;
static AAssetManager *glAssetManager = nullptr;
static bool glAssetManagerFree = false;

connst int LEN_ASSETS = 7; // "assets:" length

static ERR ASSET_Delete(objFile *, APTR);
static ERR ASSET_Free(objFile *, APTR);
static ERR ASSET_Init(objFile *, APTR);
static ERR ASSET_Move(objFile *, struct mtFileMove *);
static ERR ASSET_Read(objFile *, struct acRead *);
static ERR ASSET_Rename(objFile *, struct acRename *);
static ERR ASSET_Seek(objFile *, struct acSeek *);
static ERR ASSET_Write(objFile *, struct acWrite *);

static ERR GET_Permissions(objFile *, APTR *);
static ERR GET_Size(objFile *, int64_t *);

static ERR SET_Permissions(objFile *, APTR);

static const FieldArray clFields[] = {
   { "Permissions", FDF_INT|FDF_RW, GET_Permissions, SET_Permissions },
   { "Size",        FDF_INT64|FDF_R, GET_Size },
   END_FIELD
};

static const ActionArray clActions[] = {
   { AC::Free,   ASSET_Free },
   { AC::Init,   ASSET_Init },
   { AC::Move,   ASSET_Move },
   { AC::Read,   ASSET_Read },
   { AC::Rename, ASSET_Rename },
   { AC::Seek,   ASSET_Seek },
   { AC::Write,  ASSET_Write },
   { 0, nullptr }
};

struct prvFileAsset {
   AAsset *Asset;
   AAssetDir *Dir;
   AAssetManager *Mgr;
};

static const MethodEntry clMethods[] = {
   { asset::FileDelete::id, ASSET_Delete, "Delete", nullptr, 0 },
   { asset::FileMove::id,   ASSET_Move, "Move", nullptr, 0 },
   { 0, nullptr, nullptr, nullptr, 0 }
};

static ERR close_dir(DirInfo *);
static ERR open_dir(DirInfo *);
static ERR get_info(std::string_view, FileInfo *, int);
static ERR read_dir(CSTRING, DirInfo **, int);
static ERR scan_dir(DirInfo *);
static ERR test_path(std::string &, int *);
static AAssetManager * get_asset_manager(void);

//********************************************************************************************************************

ERR add_asset_class(void)
{
   pf::Log log(__FUNCTION__);
   OpenInfo *openinfo;
   CSTRING classname;
   int i;

   log.branch();

   if (!(openinfo = GetResourcePtr(RES::OPENINFO))) {
      log.warning("No OpenInfo structure set during Core initialisation.");
      return ERR::InvalidState;
   }

   classname = nullptr;
   if ((openinfo->Flags & OPF_OPTIONS) and (openinfo->Options)) {
      for (i=0; openinfo->Options[i].Tag != TAGEND; i++) {
         switch (openinfo->Options[i].Tag) {
            case TOI::ANDROID_CLASS: {
               classname = openinfo->Options[i].Value.String;
               break;
            }

            case TOI::ANDROID_ASSETMGR: {
               glAssetManager = openinfo->Options[i].Value.Pointer;
               break;
            }
         }
      }
   }

   if (glAssetManager) {
      // Asset manager has been pre-allocated during JNI initialisation.
      glAssetManagerFree = FALSE;
   }
   else {

      JNIEnv *env = GetResourcePtr(RES::JNI_ENV);
      glAssetManagerFree = TRUE;

      if ((!env) or (!classname)) {
         log.warning("Android env and class name must be defined when opening the Core.");
         return ERR::InvalidState;
      }

      jclass glActivityClass = ((*env)->FindClass)(env, classname);
      if (glActivityClass) {
         jfieldID fidAssetManager = (*env)->GetStaticFieldID(env, glActivityClass, "assetManager", "Landroid/content/res/AssetManager;");
         if (fidAssetManager) {
            glAssetManager = (*env)->GetStaticObjectField(env, glActivityClass, fidAssetManager);
            if (glAssetManager) {
               glAssetManager = (*env)->NewGlobalRef(env, glAssetManager); // This call is required to prevent the Java GC from collecting the reference.
            }
            else { log.traceWarning("Failed to get assetManager field."); return ERR::SystemCall; }
         }
         else { log.traceWarning("Failed to get assetManager field ID."); return ERR::SystemCall; }
      }
      else { log.traceWarning("Failed to get Java class %s", classname); return ERR::SystemCall; }
   }

   // Create the assets: control class

   if (!(glAssetClass = extMetaClass::create::global(
      fl::BaseClassID(ID_FILE),
      fl::ClassID(ID_FILEASSETS),
      fl::Name("FileAssets"),
      fl::Actions(clActions),
      fl::Methods(clMethods),
      fl::Fields(clFields),
      fl::Path("modules:core")))) return ERR::CreateObject;

   // Create the 'assets' virtual volume

   VirtualVolume("assets", VAS_OPEN_DIR,  &open_dir,
                           VAS_SCAN_DIR,  &scan_dir,
                           VAS_CLOSE_DIR, &close_dir,
                           VAS_TEST_PATH, &test_path,
                           VAS_GET_INFO,  &get_info,
                           0);

   return ERR::Okay;
}

//********************************************************************************************************************

void free_asset_class(void)
{
   if ((glAssetManager) and (glAssetManagerFree)) {
      JNIEnv *env = GetResourcePtr(RES::JNI_ENV);
      if (env) (*env)->DeleteGlobalRef(env, glAssetManager);
   }

   VirtualAssign("assets", VAS_DEREGISTER, TAGEND);

   if (glAssetClass) { FreeResource(glAssetClass); glAssetClass = nullptr; }
}

//********************************************************************************************************************

static ERR ASSET_Delete(objFile *Self)
{
   return ERR::NoSupport; // Asset files cannot be deleted.
}

//********************************************************************************************************************

static ERR ASSET_Free(objFile *Self)
{
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR ASSET_Init(objFile *Self)
{
   pf::Log log(__FUNCTION__);
   prvFileAsset *prv;

   if (!Self->Path) return ERR::FieldNotSet;

   log.trace("Path: %s", Self->Path);

   if (!pf::startswith("assets:", Self->Path)) return ERR::NoSupport;

   if (Self->Flags & (FL::NEW|FL::WRITE)) return log.warning(ERR::ReadOnly);

   // Allocate private structure

   if (!AllocMemory(sizeof(prvFileAsset), Self->memflags(), &Self->ChildPrivate, nullptr)) {
      int len;
      for (len=0; Self->Path[len]; len++);

      if (Self->Path[len-1] IS ':') {
         return ERR::Okay;
      }
      else if (Self->Path[len-1] IS '/') {
         // Check that the folder exists.

         uint8_t dirpath[len];

         StrCopy(Self->Path+LEN_ASSETS, dirpath);

         log.trace("Checking that path exists for '%s'", dirpath);

         AAssetDir *dir;
         if ((dir = AAssetManager_openDir(get_asset_manager(), dirpath))) {
            // Folder exists, close it and return OK
            AAssetDir_close(dir);
            return ERR::Okay;
         }
         else {
            FreeResource(Self->ChildPrivate);
            Self->ChildPrivate = nullptr;
            return ERR::DoesNotExist;
         }
      }
      else {
         prv = Self->ChildPrivate;

         // Check that the location exists / open the file.

         AAssetManager *mgr = get_asset_manager();
         if (mgr) {
            if ((prv->Asset = AAssetManager_open(mgr, Self->Path+LEN_ASSETS, AASSET_MODE_RANDOM))) { // AASSET_MODE_UNKNOWN, AASSET_MODE_RANDOM
               return ERR::Okay;
            }
            else log.warning("Failed to open asset file \"%s\"", Self->Path+LEN_ASSETS);
         }

         FreeResource(Self->ChildPrivate);
         Self->ChildPrivate = nullptr;
         return ERR::InvalidState;
      }
   }
   else return log.warning(ERR::AllocMemory);
}

//********************************************************************************************************************

static ERR ASSET_Move(objFile *Self, struct mtFileMove *Args)
{
   return ERR::NoSupport; // Assets cannot be moved
}

//********************************************************************************************************************

static ERR ASSET_Read(objFile *Self, struct acRead *Args)
{
   pf::Log log(__FUNCTION__);
   prvFileAsset *prv;

   if (!(prv = Self->ChildPrivate)) return log.warning(ERR::ObjectCorrupt);
   if (!(Self->Flags & FL::READ)) return log.warning(ERR::FileReadFlag);

   Args->Result = AAsset_read(prv->Asset, Args->Buffer, Args->Length);

   if (Args->Result != Args->Length) {
      if (Args->Result IS -1) {
         log.msg("Failed to read %d bytes from the file.", Args->Length);
         Args->Result = 0;
         return ERR::Read;
      }

      // Return ERR::Okay even though not all data was read, because this was not due to a failure.

      log.msg("%d of the intended %d bytes were read from the file.", Args->Result, Args->Length);
      Self->Position += Args->Result;
      return ERR::Okay;
   }
   else {
      Self->Position += Args->Result;
      return ERR::Okay;
   }
}

//********************************************************************************************************************

static ERR ASSET_Rename(objFile *Self, struct acRename *Args)
{
   return ERR::NoSupport; // Assets cannot be renamed.
}

//********************************************************************************************************************

static ERR ASSET_Seek(objFile *Self, struct acSeek *Args)
{
   prvFileAsset *prv;
   int method;

   if (!(prv = Self->ChildPrivate)) return log.warning(ERR::ObjectCorrupt);

   if (Args->Position IS POS_START) method = SEEK::SET;
   else if (Args->Position IS POS_END) method = SEEK::END;
   else if (Args->Position IS POS_CURRENT) method = SEEK::CUR;
   else return log.warning(ERR::Args);

   off_t offset = AAsset_seek(prv->Asset, Args->Offset, method);
   if (offset != -1) Self->Position = offset;
   else return ERR::InvalidState;

   return ERR::Okay;
}

//********************************************************************************************************************
// Assets are read-only.  If writing to an asset is required, the developer should copy the file to the cache or other
// storage area and modify it there.

static ERR ASSET_Write(objFile *Self, struct acWrite *Args)
{
   return ERR::NoSupport; // Writing to assets is disallowed
}

//********************************************************************************************************************

static ERR GET_Permissions(objFile *Self, APTR *Value)
{
   *Value = NULL;
   return ERR::Okay;
}

static ERR SET_Permissions(objFile *Self, APTR Value)
{
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR GET_Size(objFile *Self, int64_t *Value)
{
   prvFileAsset *prv;

   if (!(prv = Self->ChildPrivate)) return log.warning(ERR::ObjectCorrupt);

   if (prv->Asset) {
      *Value = AAsset_getLength(prv->Asset);
      if (*Value >= 0) return ERR::Okay;
      else return ERR::InvalidState;
   }
   else return ERR::InvalidState; // Either the file is a folder or hasn't been opened.
}

//********************************************************************************************************************
// Open the assets: volume for scanning.

static ERR open_dir(DirInfo *Dir)
{
   pf::Log log(__FUNCTION__);
   AAssetManager *mgr;
   int len;

   log.traceBranch("%s", Dir->prvResolvedPath.c_str());

   if (!(mgr = get_asset_manager())) return log.warning(ERR::SystemCall);

   // openDir() doesn't like trailing slashes, this code will handle such circumstances.

   for (len=0; Dir->prvResolvedPath[len]; len++);
   if (Dir->prvResolvedPath[len-1] != '/') Dir->prvHandle = AAssetManager_openDir(mgr, Dir->prvResolvedPath+LEN_ASSETS);
   else {
      char path[len];
      len = len - LEN_ASSETS - 1;
      copymem(Dir->prvResolvedPath+LEN_ASSETS, path, len);
      path[len] = 0;
      Dir->prvHandle = AAssetManager_openDir(mgr, path);
   }

   if (Dir->prvHandle) {
      return ERR::Okay;
   }
   else return ERR::InvalidPath;
}

//********************************************************************************************************************
// Scan the next entry in the folder.

static ERR scan_dir(DirInfo *Dir)
{
   CSTRING filename;
   AAssetManager *mgr;

   log.traceBranch("Asset file scan on %s", Dir->prvResolvedPath);

   if (!(mgr = get_asset_manager())) {
      return log.warning(ERR::SystemCall);
   }

   while ((filename = AAssetDir_getNextFileName(Dir->prvHandle))) {
      if ((Dir->prvFlags & RDF::FILE) != RDF::NIL) {
         AAsset *asset;
         if ((asset = AAssetManager_open(mgr, Dir->prvResolvedPath+LEN_ASSETS, AASSET_MODE_UNKNOWN))) {
            Dir->Info->Flags |= RDF::FILE;
            if ((Dir->prvFlags & RDF::SIZE) != RDF::NIL) Dir->Info->Size = AAsset_getLength(asset);
            AAsset_close(asset);
            StrCopy(filename, Dir->Info->Name, MAX_FILENAME);

            Dir->prvIndex++;
            Dir->prvTotal++;
            return ERR::Okay;
         }
      }

      if ((Dir->prvFlags & RDF::FOLDER) != RDF::NIL) {
         AAssetDir *dir;
         if ((dir = AAssetManager_openDir(mgr, Dir->prvResolvedPath+LEN_ASSETS))) {
            Dir->Info->Flags |= RDF::FOLDER;
            AAssetDir_close(dir);

            StrCopy(filename, Dir->Info->Name, MAX_FILENAME);

            Dir->prvIndex++;
            Dir->prvTotal++;
            return ERR::Okay;
         }
      }
   }

   return ERR::DirEmpty;
}

//********************************************************************************************************************
// Close the assets: volume.

static ERR close_dir(DirInfo *Dir)
{
   // Note: FreeResource() will take care of memory dealloactions, we only need to be concerned with deallocation of any
   // open handles.

   if (Dir->prvHandle) {
      AAssetDir_close(Dir->prvHandle);
      Dir->prvHandle = nullptr;
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR get_info(std::string_view Path, FileInfo *Info, int InfoSize)
{
   pf::Log log(__FUNCTION__);
   int8_t dir;
   int i, len;

   // We need to open the file in order to retrieve its size.

   AAssetManager *mgr = get_asset_manager();
   dir = FALSE;
   if (mgr) {
      AAsset *asset;
      AAssetDir *assetdir;
      if (pf::startswith("assets:", Path)) { // Just a sanity check - the Path is always meant to be resolved.
         if ((asset = AAssetManager_open(mgr, Path+LEN_ASSETS, AASSET_MODE_UNKNOWN))) {
            Info->Size = AAsset_getLength(asset);
            AAsset_close(asset);
         }
         else if ((assetdir = AAssetManager_openDir(mgr, Path+LEN_ASSETS))) {
            if (AAssetDir_getNextFileName(assetdir)) dir = TRUE;
            AAssetDir_close(assetdir);
         }
      }
      else return ERR::NoSupport;
   }
   else return ERR::SystemCall;

   Info->Flags = 0;
   Info->Time.Year   = 2013;
   Info->Time.Month  = 1;
   Info->Time.Day    = 1;
   Info->Time.Hour   = 0;
   Info->Time.Minute = 0;
   Info->Time.Second = 0;

   for (len=0; Path[len]; len++);

   if ((Path[len-1] IS '/') or (Path[len-1] IS '\\')) Info->Flags |= RDF::FOLDER;
   else if (dir) Info->Flags |= RDF::FOLDER;
   else Info->Flags |= RDF::FILE|RDF::SIZE;

   // Extract the file name

   i = len;
   if ((Path[i-1] IS '/') or (Path[i-1] IS '\\')) i--;
   while ((i > 0) and (Path[i-1] != '/') and (Path[i-1] != '\\') and (Path[i-1] != ':')) i--;
   i = StrCopy(Path + i, Info->Name, MAX_FILENAME-2);

   if ((Info->Flags & RDF::FOLDER) != RDF::NIL) {
      if (Info->Name[i-1] IS '\\') Info->Name[i-1] = '/';
      else if (Info->Name[i-1] != '/') {
         Info->Name[i++] = '/';
         Info->Name[i] = 0;
      }
   }

   Info->Permissions = 0;
   Info->UserID      = 0;
   Info->GroupID     = 0;
   Info->Tags        = nullptr;
   return ERR::Okay;
}

//********************************************************************************************************************
// Test an assets: location.

static ERR test_path(std::string &Path, int Flags, LOC *Type)
{
   pf::Log log(__FUNCTION__);
   AAssetManager *mgr;
   AAsset *asset;
   AAssetDir *dir;
   int len;

   log.traceBranch("%s", Path);

   if (!(mgr = get_asset_manager())) return ERR::SystemCall;

   for (len=0; Path[len]; len++);  // Check if the reference is explicitly defined as a folder.
   if (Path[len-1] != '/') {
      if ((asset = AAssetManager_open(mgr, Path+LEN_ASSETS, AASSET_MODE_UNKNOWN))) {
         log.trace("Path identified as a file.");
         *Type = LOC::FILE;
         AAsset_close(asset);
         return ERR::Okay;
      }

      dir = AAssetManager_openDir(mgr, Path+LEN_ASSETS);
   }
   else {
      // openDir() doesn't like trailing slashes, so we'll have to remove it.
      char path[len];
      len = len - LEN_ASSETS - 1;
      copymem(Path+LEN_ASSETS, path, len);
      path[len] = 0;

      dir = AAssetManager_openDir(mgr, path);
   }

   // Testing a folder for its existance requires that it contains at least one file.
   // This is because openDir() has been observed as succeeding even when the path doesn't exist.

   if (dir) {
      if (AAssetDir_getNextFileName(dir)) {
         log.trace("Path identified as a folder.");
         *Type = LOC::DIRECTORY;
         AAssetDir_close(dir);
         return ERR::Okay;
      }
      else AAssetDir_close(dir);
   }

   log.trace("Path '%s' does not exist.", Path + LEN_ASSETS);
   return ERR::DoesNotExist;
}

//********************************************************************************************************************
// Read the entire folder in one function call.

#if 0
static ERR read_dir(CSTRING Path, DirInfo **Result, int Flags)
{
   DirInfo *dirinfo;
   AAssetDir *dir;
   AAssetManager *mgr;
   int len;

   log.traceBranch("Path: %s, Flags: $%.8x", Path, Flags);

   if (!(mgr = get_asset_manager())) {
      return log.warning(ERR::SystemCall);
   }

   // openDir() doesn't like trailing slashes, this code will handle such circumstances.

   for (len=0; Path[len]; len++);
   if (Path[len-1] != '/') dir = AAssetManager_openDir(mgr, Path+LEN_ASSETS);
   else {
      char path[len];
      len = len - LEN_ASSETS - 1;
      copymem(Path+LEN_ASSETS, path, len);
      path[len] = 0;
      dir = AAssetManager_openDir(mgr, path);
   }

   if (!dir) {
      return ERR::InvalidPath;
   }

   if (AllocMemory(sizeof(DirInfo), MEM::DATA, &dirinfo, nullptr)) {
      AAssetDir_close(dir);
      return ERR::AllocMemory;
   }

   const char *filename;
   FileInfo *entry, *current;
   uint8_t assetpath[300];
   int i;

   // Read folder structure

   current = nullptr;
   dirinfo->Total = 0;
   ERR error = ERR::Okay;
   int insert = StrCopy(Path+LEN_ASSETS, assetpath, sizeof(assetpath)-2);
   if (assetpath[insert-1] != '/') assetpath[insert++] = '/';
   while ((filename = AAssetDir_getNextFileName(dir)) and (!error)) {
      entry = nullptr;

      StrCopy(filename, assetpath+insert, sizeof(assetpath)-insert-1);
      if (insert >= sizeof(assetpath)-1) {
         error = ERR::BufferOverflow;
         break;
      }

      AAsset *asset;
      if ((asset = AAssetManager_open(mgr, assetpath, AASSET_MODE_UNKNOWN))) {
         if ((Flags & RDF::FILE) != RDF::NIL) {
            int size = sizeof(FileInfo) + strlen(filename) + 2;
            if (!AllocMemory(size, MEM::DATA, &entry, nullptr)) {
               entry->Flags = RDF::FILE;

               if (Flags & RDF::PERMISSIONS) {
                  entry->Flags |= RDF::PERMISSIONS;
                  entry->Permissions = PERMIT::READ|PERMIT::GROUP_READ|PERMIT::OTHERS_READ;
               }

               if ((Flags & RDF::SIZE) != RDF::NIL) {
                  entry->Flags |= RDF::SIZE;
                  entry->Size = AAsset_getLength(asset);
               }

               if ((Flags & RDF::DATE) != RDF::NIL) {
                  entry->Time.Year = 2013;
                  entry->Time.Month = 1;
                  entry->Time.Day = 1;
               }

               entry->Name = (STRING)(entry + 1);
               StrCopy(filename, entry->Name);

               dirinfo->Total++;
            }
            else error = ERR::AllocMemory;
         }
         AAsset_close(asset);
      }
      else if ((Flags & RDF::FOLDER) != RDF::NIL) {
         int size = sizeof(FileInfo) + strlen(filename) + 2;
         if (!AllocMemory(size, MEM::DATA, &entry, nullptr)) {
            entry->Flags = RDF::FOLDER;

            if ((Flags & RDF::PERMISSIONS) != RDF::NIL) {
               entry->Flags |= RDF::PERMISSIONS;
               entry->Permissions = PERMIT::READ|PERMIT::GROUP_READ|PERMIT::OTHERS_READ;
            }

            entry->Name = (STRING)(entry + 1);
            i = StrCopy(filename, entry->Name);
            if ((Flags & RDF::QUALIFY) != RDF::NIL) { entry->Name[i++] = '/'; entry->Name[i++] = 0; }

            dirinfo->Total++;
         }
         else error = ERR::AllocMemory;
      }

      // Insert entry into the linked list

      if (entry) {
         if (!dirinfo->Info) dirinfo->Info = entry;
         if (current) current->Next = entry;
         current = entry;
      }
   }

   AAssetDir_close(dir);

   log.trace("Found %d files, error code %d", dirinfo->Total, error);

   if (error) {
      // Remove all allocations.

      FileInfo *list = dirinfo->Info;
      while (list) {
         FileInfo *next = list->Next;
         FreeResource(list);
         list = next;
      }

      if (Result) *Result = nullptr;
      FreeResource(dirinfo);
      return error;
   }
   else {
      if (Result) *Result = dirinfo;
      return ERR::Okay;
   }
}
#endif

//********************************************************************************************************************

static AAssetManager * get_asset_manager(void)
{
   log.trace("Native Access: %d", glAssetManagerFree);

   if (glAssetManagerFree) {
      return AAssetManager_fromJava(GetResourcePtr(RES::JNI_ENV), glAssetManager);
   }
   else return glAssetManager;
}
