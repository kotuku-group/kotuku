/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Clipboard: Manages copied, cut and dragged data for paste operations.

The Clipboard class stores references to copied data so that applications can offer cut, copy, paste and drag-and-drop
workflows.  Clipboard entries are grouped by datatype and are exposed to consumers as readable files, allowing large
items to be pasted without first loading their contents into application memory.

Multiple Clipboard objects can be created, but they share the same clipboard store for the current process and user
session.  Each datatype normally has one active group of items.  Adding a new group for the same datatype replaces the
previous group unless the `CEF::EXTEND` flag is used.

On Windows, text and file references are integrated with the host clipboard when possible, so Kōtuku applications can
exchange those datatypes with native Windows applications.  If `CPF::HISTORY_BUFFER` is enabled, the clipboard actively
monitors host changes and caches copied data in the local `clipboard:` volume.  This enables limited history at the
cost of additional monitoring and storage overhead.

On Linux and other non-Windows builds, clipboard storage is local to Kōtuku applications unless platform-specific host
integration is supplied by the display driver.

When history buffering is active, a fixed number of clip groups is retained and the oldest group is removed when the
limit is exceeded.  Cached clipboard files are kept under `clipboard:` and stale generated files are cleaned up during
display initialisation.
-END-

*********************************************************************************************************************/

#include "defs.h"

#ifdef _WIN32
using namespace display;
#endif

constexpr int MAX_CLIPS = 10; // Maximum number of clips stored in the historical buffer

static const FieldDef glDatatypes[] = {
   { "data",   CLIPTYPE::DATA },
   { "audio",  CLIPTYPE::AUDIO },
   { "image",  CLIPTYPE::IMAGE },
   { "file",   CLIPTYPE::FILE },
   { "object", CLIPTYPE::OBJECT },
   { "text",   CLIPTYPE::TEXT },
   { nullptr, 0 }
};

std::list<ClipRecord> glClips;
std::recursive_mutex glClipboardLock;
static int glCounter = 1;
static int glHistoryLimit = 1;
static std::string glProcessID;
#ifdef _WIN32
static int glLastClipID = -1;
#endif

//********************************************************************************************************************

static std::string get_datatype(CLIPTYPE);
static ERR add_clip(CLIPTYPE, const std::vector<ClipItem> &, CEF = CEF::NIL);
static ERR add_clip(CSTRING);
static ERR CLIPBOARD_AddObjects(objClipboard *, struct clip::AddObjects *);

#ifdef _WIN32
static std::u16string utf8_to_utf16(CSTRING String, int Length, bool SurrogatePairs)
{
   std::u16string result;
   if (!String) return result;

   auto str = String;
   int chars, bytes = 0;
   for (chars=0; (bytes < Length) and (str[bytes]); chars++) {
      for (++bytes; (bytes < Length) and ((str[bytes] & 0xc0) IS 0x80); bytes++);
   }

   result.reserve(size_t(chars));

   int pos = 0;
   while (pos < bytes) {
      int len = UTF8CharLength(str);
      if ((len IS 0) or (pos + len > bytes)) break;

      uint32_t codepoint;
      if (SurrogatePairs and (len IS 1)) codepoint = *str;
      else codepoint = UTF8ReadValue(str, nullptr);
      if (SurrogatePairs and (codepoint >= 0x10000)) {
         codepoint -= 0x10000;
         result.push_back((char16_t)(0xD800 + (codepoint >> 10)));
         result.push_back((char16_t)(0xDC00 + (codepoint & 0x3FF)));
      }
      else result.push_back((char16_t)codepoint);

      str += len;
      pos += len;
   }

   return result;
}
#endif

//********************************************************************************************************************
// Remove stale clipboard files that are over 24hrs old

void clean_clipboard(void)
{
   auto time = objTime::create { };
   if (!time.ok()) return;

   time->query();
   int64_t now = time->get<int64_t>(FID_Timestamp) / 1000000LL;
   int64_t yesterday = now - (24 * 60LL * 60LL);

   DirInfo *dir;
   if (OpenDir("clipboard:", RDF::FILE|RDF::DATE, &dir) IS ERR::Okay) {
      LocalResource free_dir(dir);

      Regex *compiled;
      if (rx::Compile("^\\d+(?:_text|_image|_file|_object)\\d*\\.\\d{3}$", REGEX::NIL, nullptr, &compiled) IS ERR::Okay) {
         while (ScanDir(dir) IS ERR::Okay) {
            if (rx::Match(compiled, dir->Info->Name, RMATCH::NIL, nullptr) IS ERR::Okay) {
               if (dir->Info->Timestamp < yesterday) {
                  std::string path("clipboard:");
                  path.append(dir->Info->Name);
                  DeleteFile(path, nullptr);
               }
            }
         }
         FreeResource(compiled);
      }
   }
}

//********************************************************************************************************************

ClipRecord::~ClipRecord() {
   kt::Log log(__FUNCTION__);

   if (Datatype != CLIPTYPE::FILE) {
      log.branch("Deleting clip files for %s datatype.", get_datatype(Datatype).c_str());
      for (auto &item : Items) DeleteFile(item.Path, nullptr);
   }
   else log.branch("Datatype: File");
}

//********************************************************************************************************************

static std::string get_datatype(CLIPTYPE Datatype)
{
   for (unsigned i=0; glDatatypes[i].Name; i++) {
      if (int(Datatype) IS glDatatypes[i].Value) return std::string(glDatatypes[i].Name);
   }

   return "unknown";
}

//********************************************************************************************************************

static void notify_script_free(OBJECTPTR Object, ACTIONID ActionID, ERR Result, nullptr_t Args)
{
   auto Self = (objClipboard *)CurrentContext();
   Self->RequestHandler.clear();
}

//********************************************************************************************************************

static ERR add_file_to_host(objClipboard *Self, const std::vector<ClipItem> &Items, bool Cut)
{
   kt::Log log;

   if ((Self->Flags & CPF::DRAG_DROP) != CPF::NIL) return ERR::NoSupport;

#ifdef _WIN32
   // Build a list of resolved path names in a new buffer that is suitable for passing to Windows.

   std::basic_stringstream<char16_t> list;
   for (auto &item : Items) {
      std::string path;
      if (ResolvePath(item.Path, RSF::NIL, &path) IS ERR::Okay) {
         list << utf8_to_utf16(path.c_str(), 0x7fffffff, true) << char16_t(0);
      }
   }
   list << char16_t(0); // An extra null byte is required to terminate the list for Windows HDROP

   auto str = list.str();
   auto error = (ERR)winAddFileClip(str.c_str(), str.size() * sizeof(char16_t), Cut);
   if (error != ERR::Okay) log.warning(error);
   return error;
#else
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************

static ERR add_text_to_host(objClipboard *Self, CSTRING String, int Length = 0x7fffffff)
{
   kt::Log log(__FUNCTION__);

   if ((Self->Flags & CPF::DRAG_DROP) != CPF::NIL) return ERR::NoSupport;

#ifdef _WIN32
   // Copy text to the Windows clipboard.  This requires a conversion from UTF-8 to UTF-16.

   auto utf16 = utf8_to_utf16(String, Length, false);
   utf16.push_back(0);

   auto error = (ERR)winAddClip(int(CLIPTYPE::TEXT), utf16.data(), utf16.size() * sizeof(char16_t), false);
   if (error != ERR::Okay) log.warning(error);
   return error;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-METHOD-
AddFile: Adds a file reference to the clipboard.

Use AddFile() when the data to copy is already available as a file.  The method stores the file path as a clipboard
entry and associates it with a `CLIPTYPE` value so that paste targets can decide whether they understand the content.
This is efficient for large items because the clipboard does not need to load the file contents into memory.

If the clipboard can publish the file reference to the host platform, and history buffering is disabled, the method may
return after updating the host clipboard.  Otherwise the file reference is recorded in the Kōtuku clipboard store.

Recognised data types are:

<types lookup="CLIPTYPE"/>

Optional flags that may be passed to this method are as follows:

<types lookup="CEF"/>

-INPUT-
int(CLIPTYPE) Datatype: Identifies the type of data represented by the file.
cstr Path: Path of the file to add.
int(CEF) Flags: Optional flags.

-ERRORS-
Okay: The files were added to the clipboard.
NullArgs
MissingPath: `Path` was not specified.
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddFile(objClipboard *Self, struct clip::AddFile *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((!Args->Path) or (!Args->Path[0])) return log.warning(ERR::MissingPath);

   log.branch("Path: %s", Args->Path);

   std::vector<ClipItem> items = { std::string(Args->Path) };
   if (add_file_to_host(Self, items, ((Args->Flags & CEF::DELETE) != CEF::NIL) ? true : false) IS ERR::Okay) {
      if (glHistoryLimit <= 1) return ERR::Okay;
   }

   return add_clip(Args->Datatype, items, Args->Flags & (CEF::DELETE|CEF::EXTEND));
}

/*********************************************************************************************************************

-METHOD-
AddObjects: Saves objects to clipboard cache files.

Use AddObjects() to copy one or more objects by asking each object to save itself to a generated file in the
`clipboard:` volume.  This avoids requiring the caller to create temporary files before copying object data.

If `Datatype` is `CLIPTYPE::NIL`, the clipboard chooses a datatype from the source object's class where possible.
@Picture objects are stored as `CLIPTYPE::IMAGE`, sound objects are stored as `CLIPTYPE::AUDIO`, and unrecognised
classes are stored as `CLIPTYPE::OBJECT`.  Set `Datatype` explicitly to override this automatic selection.

All objects in a single call must belong to the same class.  The `Objects` array must be terminated with a zero entry.

Optional flags that may be passed to this method are the same as those specified in the #AddFile() method.  The
`CEF::DELETE` flag has no effect on objects.

-INPUT-
int(CLIPTYPE) Datatype: Type of data represented by the objects, or zero for automatic recognition.
ptr(oid) Objects: Zero-terminated array of object IDs to add to the clipboard.
int(CEF) Flags: Optional flags.

-ERRORS-
Okay: The objects were added to the clipboard.
NullArgs
Args
Lock
CreateFile
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddObjects(objClipboard *Self, struct clip::AddObjects *Args)
{
   kt::Log log;

   if ((!Args) or (!Args->Objects) or (!Args->Objects[0])) return log.warning(ERR::NullArgs);

   log.branch();

   int counter = glCounter++;
   CLASSID classid = CLASSID::NIL;
   auto datatype = Args->Datatype;

   std::vector<ClipItem> items;
   for (unsigned i=0; Args->Objects[i]; i++) {
      kt::ScopedObjectLock<Object> object(Args->Objects[i], 5000);
      if (object.granted()) {
         if (classid IS CLASSID::NIL) classid = object.obj->classID();

         if (classid IS object.obj->classID()) { // The client may not mix and match classes.
            if (datatype IS CLIPTYPE::NIL) {
               if (object.obj->classID() IS CLASSID::PICTURE) datatype = CLIPTYPE::IMAGE;
               else if (object.obj->classID() IS CLASSID::SOUND) datatype = CLIPTYPE::AUDIO;
               else datatype = CLIPTYPE::OBJECT;
            }

            char idx[5];
            snprintf(idx, sizeof(idx), ".%.3d", i);
            auto path = std::string("clipboard:") + glProcessID + "_" + get_datatype(datatype) + std::to_string(counter) + idx;

            auto file = objFile::create { fl::Path(path), fl::Flags(FL::WRITE|FL::NEW) };
            if (file.ok()) {
               if (auto error = acSaveToObject(*object, *file); error != ERR::Okay) return log.warning(error);
            }
            else return ERR::CreateFile;
         }
      }
      else return ERR::Lock;
   }

   if (add_file_to_host(Self, items, ((Args->Flags & CEF::DELETE) != CEF::NIL) ? true : false) IS ERR::Okay) {
      if (glHistoryLimit <= 1) return ERR::Okay;
   }

   return add_clip(datatype, items, Args->Flags & CEF::EXTEND);
}

/*********************************************************************************************************************

-METHOD-
AddText: Adds a block of text to the clipboard.

Use AddText() to place plain UTF-8 text on the clipboard.  Empty strings are ignored and return `ERR::Okay`.

On Windows, the text is also published to the host clipboard when supported.  If history buffering is disabled and the
host clipboard accepts the text, no local cache file is created.

-INPUT-
cstr String: UTF-8 text to add to the clipboard.

-ERRORS-
Okay
NullArgs
CreateFile
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddText(objClipboard *Self, struct clip::AddText *Args)
{
   kt::Log log;

   if ((!Args) or (!Args->String)) return log.warning(ERR::NullArgs);
   if (!Args->String[0]) return ERR::Okay;

   if (add_text_to_host(Self, Args->String) IS ERR::Okay) {
      if (glHistoryLimit <= 1) return ERR::Okay;
   }

   return add_clip(Args->String);
}

/*********************************************************************************************************************
-ACTION-
Clear: Removes all cached clipboard data.

Clear deletes the generated clipboard cache and removes all clip records tracked by the current process.  Use
#Remove() to delete only selected datatypes.

-END-
*********************************************************************************************************************/

static ERR CLIPBOARD_Clear(objClipboard *Self)
{
   std::string path;
   if (ResolvePath("clipboard:", RSF::NO_FILE_CHECK, &path) IS ERR::Okay) {
      DeleteFile(path, nullptr);
      CreateFolder(path, PERMIT::READ|PERMIT::WRITE);
   }

   {
      const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);
      glClips.clear();
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
DataFeed: Sends data to the clipboard or handles drag-and-drop requests.

For regular clipboard writes, DataFeed currently accepts `DATA::TEXT`.  Text received through this action replaces the
current text clip and is cached as a generated file unless the host clipboard accepts it and no history buffer is
active.

When the clipboard is in drag-and-drop mode, DataFeed also accepts `DATA::REQUEST`.  Requests are forwarded to the
#RequestHandler callback so the source application can provide the requested data to the requester.

-ERRORS-
Okay
NullArgs
Write
CreateObject
FieldNotSet
NoSupport
Terminate
-END-
*********************************************************************************************************************/

static ERR CLIPBOARD_DataFeed(objClipboard *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (Args->Datatype IS DATA::TEXT) {
      log.msg("Copying text to the clipboard.");

      if (!Args->Buffer) return log.warning(ERR::NullArgs);
      if (auto error = add_text_to_host(Self, (CSTRING)Args->Buffer, Args->Size);
            (error != ERR::Okay) and (error != ERR::NoSupport)) {
         return log.warning(error);
      }

      std::vector<ClipItem> items = { std::string("clipboard:") + glProcessID + "_text" + std::to_string(glCounter++) + std::string(".000") };
      if (auto error = add_clip(CLIPTYPE::TEXT, items); error IS ERR::Okay) {
         auto file = objFile::create { fl::Path(items[0].Path), fl::Flags(FL::NEW|FL::WRITE), fl::Permissions(PERMIT::READ|PERMIT::WRITE) };
         if (file.ok()) {
            if (file->write(Args->Buffer, Args->Size, 0) != ERR::Okay) return log.warning(ERR::Write);
            return ERR::Okay;
         }
         else return log.warning(ERR::CreateObject);
      }
      else return log.warning(error);
   }
   else if ((Args->Datatype IS DATA::REQUEST) and ((Self->Flags & CPF::DRAG_DROP) != CPF::NIL))  {
      if ((!Args->Buffer) or (!Args->Object)) return log.warning(ERR::NullArgs);

      auto request = (struct dcRequest *)Args->Buffer;
      log.branch("Data request from #%d received for item %d, datatype %d", Args->Object->UID, request->Item, request->Preference[0]);

      ERR error = ERR::Okay;
      if (Self->RequestHandler.isC()) {
         auto routine = (ERR (*)(objClipboard *, OBJECTPTR, int, char *, APTR))Self->RequestHandler.Routine;
         kt::SwitchContext ctx(Self->RequestHandler.Context);
         error = routine(Self, Args->Object, request->Item, request->Preference, Self->RequestHandler.Meta);
      }
      else if (Self->RequestHandler.isScript()) {
         if (sc::Call(Self->RequestHandler, std::to_array<ScriptArg>({
            { "Clipboard", Self, FD_OBJECTPTR },
            { "Requester", Args->Object, FD_OBJECTPTR },
            { "Item",      request->Item },
            { "Datatypes", request->Preference, FD_ARRAY|FD_BYTE },
            { "Size",      int(std::ssize(request->Preference)), FD_INT|FD_ARRAYSIZE }
         }), error) != ERR::Okay) error = ERR::Terminate;
      }
      else error = log.warning(ERR::FieldNotSet);

      if (error IS ERR::Terminate) Self->RequestHandler.Type = CALL::NIL;

      return error;
   }
   else log.warning("Unrecognised data type %d.", int(Args->Datatype));

   return ERR::NoSupport;
}

//********************************************************************************************************************

static ERR CLIPBOARD_Free(objClipboard *Self)
{
   if (Self->RequestHandler.isScript()) {
      UnsubscribeAction(Self->RequestHandler.Context, AC::Free);
      Self->RequestHandler.clear();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetFiles: Retrieve the most recently clipped data as a list of files.

GetFiles() returns clipboard entries as a `NULL`-terminated list of readable file paths.  The caller can request a
specific set of datatypes through `Filter`, or pass `CLIPTYPE::NIL` to accept any datatype.

Without history buffering, only the most recent clip group is available.  With history buffering enabled, pass
`CLIPTYPE::NIL` as `Filter` and increment `Index` to scan retained history from newest to oldest until
`ERR::OutOfRange` is returned.

On success, `Datatype` reports the datatype of the returned clip and `Files` receives the generated file list.  How the
caller reads each file depends on `Datatype`; ~Core.IdentifyFile() can also be used to find a class that supports the
data.  The returned `Files` array is allocated for the caller and must be released with ~Core.FreeResource().

If `CEF::DELETE` is returned in `Flags`, the caller must delete the source files after successfully copying the data in
order to complete a cut operation.  When cutting and pasting files within the same file system, ~Core.MoveFile() is
usually the most efficient way to consume those entries.

-INPUT-
int(CLIPTYPE) Filter: Datatype filter.  Set to zero to accept any datatype.
int Index: History index to read when Filter is zero.  Zero is the most recent clip group.
&int(CLIPTYPE) Datatype: Datatype of the returned clip group.
!array(cstr) Files: `NULL`-terminated list of returned clip file paths.  Release the array with ~Core.FreeResource().
&int(CEF) Flags: Result flags.  If the delete flag is set, delete the files after use to complete a cut operation.

-ERRORS-
Okay: A matching clip was found and returned.
NullArgs
OutOfRange: The specified `Index` is out of the range of the available clip items.
NoData: No clip was available that matched the requested data type.
AllocMemory
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_GetFiles(objClipboard *Self, struct clip::GetFiles *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Datatype: $%.8x", int(Args->Datatype));

   Args->Files = nullptr;

   if ((Self->Flags & CPF::HISTORY_BUFFER) IS CPF::NIL) {
#ifdef _WIN32
      // If the history buffer is disabled then we need to actively retrieve whatever Windows has on the clipboard.
      if (winCurrentClipboardID() != glLastClipID) winCopyClipboard();
#endif
   }

   const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);

   if (glClips.empty()) return ERR::NoData;

   ClipRecord *clip = &glClips.front();

   // Find the first clipboard entry to match what has been requested

   if ((Self->Flags & CPF::HISTORY_BUFFER) != CPF::NIL) {
      if (Args->Filter IS CLIPTYPE::NIL) { // Retrieve the most recent clip item, or the one indicated in the Index parameter.
         if ((Args->Index < 0) or (Args->Index >= int(glClips.size()))) return log.warning(ERR::OutOfRange);
         std::advance(clip, Args->Index);
      }
      else {
         bool found = false;
         for (auto &scan : glClips) {
            if ((Args->Filter & scan.Datatype) != CLIPTYPE::NIL) {
               found = true;
               clip = &scan;
               break;
            }
         }

         if (!found) {
            log.warning("No clips available for datatype $%x", int(Args->Filter));
            return ERR::NoData;
         }
      }
   }
   else if (Args->Filter != CLIPTYPE::NIL) {
      if ((clip->Datatype & Args->Filter) IS CLIPTYPE::NIL) return ERR::NoData;
   }

   CSTRING *list = nullptr;
   int str_len = 0;
   for (auto &item : clip->Items) str_len += item.Path.size() + 1;
   if (AllocMemory(((clip->Items.size()+1) * sizeof(STRING)) + str_len, MEM::NO_CLEAR|MEM::CALLER, &list) IS ERR::Okay) {
      Args->Files    = list;
      Args->Flags    = clip->Flags;
      Args->Datatype = clip->Datatype;

      auto dest = (char *)list + ((clip->Items.size() + 1) * sizeof(STRING));
      for (auto &item : clip->Items) {
         *list++ = dest;
         copymem(item.Path.c_str(), dest, item.Path.size() + 1);
         dest += item.Path.size() + 1;
      }
      *list = nullptr;

      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

//********************************************************************************************************************

static ERR CLIPBOARD_Init(objClipboard *Self)
{
   if ((Self->Flags & CPF::HISTORY_BUFFER) != CPF::NIL) glHistoryLimit = MAX_CLIPS;

   // Create a directory under temp: to store clipboard data

   CreateFolder("clipboard:", PERMIT::READ|PERMIT::WRITE);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR CLIPBOARD_NewObject(objClipboard *Self)
{
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Remove: Removes selected datatypes from the clipboard.

Remove() clears all active clip groups whose datatype matches `Datatype`.  Multiple datatypes can be removed by
combining `CLIPTYPE` flags.  To clear all content from the clipboard, use #Clear().

-INPUT-
int(CLIPTYPE) Datatype: Datatype flags to remove.  Values may be combined.

-ERRORS-
Okay
NullArgs
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_Remove(objClipboard *Self, struct clip::Remove *Args)
{
   kt::Log log;

   if ((!Args) or (Args->Datatype IS CLIPTYPE::NIL)) return log.warning(ERR::NullArgs);

   log.branch("Datatype: $%x", int(Args->Datatype));

   const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);

   for (auto it=glClips.begin(); it != glClips.end();) {
      if ((it->Datatype & Args->Datatype) != CLIPTYPE::NIL) {
         if (it IS glClips.begin()) {
            #ifdef _WIN32
            winClearClipboard();
            #endif
         }
         it = glClips.erase(it);
      }
      else it++;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional clipboard behaviour flags.
Lookup: CPF

-FIELD-
RequestHandler: Callback for drag-and-drop data requests.

When the clipboard is in drag-and-drop mode, applications can request source data by sending a `DATA::REQUEST` to
#DataFeed().  The request is forwarded to the callback stored in RequestHandler, which must be supplied by the source
application.

The callback uses this signature:

`ERR RequestHandler(*Clipboard, OBJECTPTR Requester, int Item, BYTE Datatypes[4])`

The callback is expected to send a `DATA::RECEIPT` to the object referenced by `Requester`.  The receipt must cover
`Item` and use one of the preferred datatypes supplied in `Datatypes`.  If the request cannot be fulfilled, the
callback should return `ERR::NoSupport`.

*********************************************************************************************************************/

static ERR GET_RequestHandler(objClipboard *Self, FUNCTION **Value)
{
   if (Self->RequestHandler.defined()) {
      *Value = &Self->RequestHandler;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_RequestHandler(objClipboard *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->RequestHandler.isScript()) UnsubscribeAction(Self->RequestHandler.Context, AC::Free);
      Self->RequestHandler = *Value;
      if (Self->RequestHandler.isScript()) {
         SubscribeAction(Self->RequestHandler.Context, AC::Free, C_FUNCTION(notify_script_free));
      }
   }
   else Self->RequestHandler.clear();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR add_clip(CLIPTYPE Datatype, const std::vector<ClipItem> &Items, CEF Flags)
{
   kt::Log log(__FUNCTION__);

   log.branch("Datatype: $%x, Flags: $%x, Total Items: %d", int(Datatype), int(Flags), int(Items.size()));

   if (Items.empty()) return ERR::Args;

   const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);

   if ((Flags & CEF::EXTEND) != CEF::NIL) {
      // Search for an existing clip that matches the requested datatype
      for (auto it = glClips.begin(); it != glClips.end(); it++) {
         if (it->Datatype IS Datatype) {
            log.msg("Extending existing clip record for datatype $%x.", int(Datatype));

            auto clip = *it;
            clip.Items.insert(clip.Items.end(), Items.begin(), Items.end());

            // Move clip to the front of the queue.

            glClips.erase(it);
            glClips.insert(glClips.begin(), clip);
            return ERR::Okay;
         }
      }
   }

   // Remove any existing clips that match this datatype

   for (auto it = glClips.begin(); it != glClips.end(); ) {
      if (it->Datatype IS Datatype) it = glClips.erase(it);
      else it++;
   }

   if (int(glClips.size()) > glHistoryLimit) glClips.pop_back(); // Remove oldest clip if history buffer is full.

   glClips.emplace_front(Datatype, Flags & CEF::DELETE, Items);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR add_clip(CSTRING String)
{
   kt::Log log(__FUNCTION__);
   log.branch();

   std::vector<ClipItem> items = { std::string("clipboard:") + glProcessID + "_text" + std::to_string(glCounter++) + ".000" };
   if (auto error = add_clip(CLIPTYPE::TEXT, items); error IS ERR::Okay) {
      kt::Create<objFile> file = { fl::Path(items[0].Path), fl::Flags(FL::WRITE|FL::NEW), fl::Permissions(PERMIT::READ|PERMIT::WRITE) };
      if (file.ok()) {
         if (auto error = file->write(String, strlen(String), 0); error != ERR::Okay) return log.warning(error);
         return ERR::Okay;
      }
      else return log.warning(ERR::CreateFile);
   }
   else return log.warning(error);
}

//********************************************************************************************************************
// Called when the Windows clipboard holds new text.  We respond by copying this into our internal clipboard system.

#ifdef _WIN32
extern "C" void report_windows_clip_text(CSTRING String)
{
   kt::Log log("Clipboard");
   log.branch("Application has detected text on the clipboard.");

   add_clip(String);
   glLastClipID = winCurrentClipboardID();
}

//********************************************************************************************************************
// Called when the Windows clipboard holds new file references.  We store a direct reference to the file path.

extern "C" void report_windows_files(APTR Data, int CutOperation)
{
   kt::Log log("Clipboard");
   log.branch("Application has detected files on the clipboard.  Cut: %d", CutOperation);

   std::vector<ClipItem> items;
   char buffer[256];
   for (int i=0; winExtractFile(Data, i, buffer, sizeof(buffer)); i++) {
      items.push_back(std::string(buffer));
   }
   add_clip(CLIPTYPE::FILE, items, CutOperation ? CEF::DELETE : CEF::NIL);
   glLastClipID = winCurrentClipboardID();
}

//********************************************************************************************************************

extern "C" void report_windows_hdrop(const char *Data, int CutOperation, char WideChar)
{
   kt::Log log("Clipboard");
   log.branch("Application has detected files on the clipboard.  Cut: %d", CutOperation);

   std::vector<ClipItem> items;
   if (WideChar) { // Widechar -> UTF-8
      auto sdata = reinterpret_cast<const char16_t*>(Data);
      while (*sdata) {
         // Convert UTF-16 to UTF-8 manually
         std::string utf8_path;
         const char16_t *src = sdata;
         while (*src) {
            uint32_t codepoint = *src++;

            // Handle surrogate pairs
            if (codepoint >= 0xD800 and codepoint <= 0xDBFF) {
               if (*src >= 0xDC00 and *src <= 0xDFFF) {
                  codepoint = 0x10000 + ((codepoint & 0x3FF) << 10) + (*src++ & 0x3FF);
               }
            }

            // Convert to UTF-8
            if (codepoint < 0x80) {
               utf8_path.push_back((char)codepoint);
            }
            else if (codepoint < 0x800) {
               utf8_path.push_back((char)(0xC0 | (codepoint >> 6)));
               utf8_path.push_back((char)(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint < 0x10000) {
               utf8_path.push_back((char)(0xE0 | (codepoint >> 12)));
               utf8_path.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
               utf8_path.push_back((char)(0x80 | (codepoint & 0x3F)));
            }
            else {
               utf8_path.push_back((char)(0xF0 | (codepoint >> 18)));
               utf8_path.push_back((char)(0x80 | ((codepoint >> 12) & 0x3F)));
               utf8_path.push_back((char)(0x80 | ((codepoint >> 6) & 0x3F)));
               utf8_path.push_back((char)(0x80 | (codepoint & 0x3F)));
            }
         }
         items.emplace_back(utf8_path);
         sdata += std::char_traits<char16_t>::length(sdata) + 1; // Next file path
      }
   }
   else { // UTF-8
      for (int i=0; *Data; i++) {
         while (*Data) {
            items.emplace_back(std::string(Data));
            Data += strlen(Data) + 1; // Next file path
         }
      }
   }

   add_clip(CLIPTYPE::FILE, items, CutOperation ? CEF::DELETE : CEF::NIL);
   glLastClipID = winCurrentClipboardID();
}

//********************************************************************************************************************
// Called when the Windows clipboard holds new text in UTF-16 format.

extern "C" void report_windows_clip_utf16(uint16_t *String)
{
   kt::Log log("Clipboard");
   log.branch("Application has detected unicode text on the clipboard.");

   std::stringstream buffer;

   for (unsigned chars=0; String[chars]; chars++) {
      auto value = String[chars];
      if (value < 128) buffer << (uint8_t)value;
      else if (value < 0x800) {
         uint8_t b = (value & 0x3f) | 0x80;
         value = value>>6;
         buffer << (value | 0xc0) << b;
      }
      else {
         uint8_t c = (value & 0x3f)|0x80;
         value = value>>6;
         uint8_t b = (value & 0x3f)|0x80;
         value = value>>6;
         buffer << (value | 0xe0) << b << c;
      }
   }

   add_clip(buffer.str().c_str());
   glLastClipID = winCurrentClipboardID();
}

//********************************************************************************************************************
// Intercept changes to the Windows clipboard.  If the history buffer is enabled then we need to pro-actively copy
// content from the clipboard.

extern "C" void win_clipboard_updated()
{
   kt::Log log(__FUNCTION__);
   log.branch();
   if (glHistoryLimit <= 1) return;
   winCopyClipboard();
}
#endif

#include "class_clipboard_def.c"

static const FieldArray clFields[] = {
   { "Flags",          FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clClipboardFlags },
   { "RequestHandler", FDF_FUNCTIONPTR|FDF_RW, GET_RequestHandler, SET_RequestHandler },
   END_FIELD
};

//********************************************************************************************************************

ERR create_clipboard_class(void)
{
   clClipboard = objMetaClass::create::global(
      fl::BaseClassID(CLASSID::CLIPBOARD),
      fl::ClassVersion(VER_CLIPBOARD),
      fl::Name("Clipboard"),
      fl::Category(CCF::IO),
      fl::Actions(clClipboardActions),
      fl::Methods(clClipboardMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(objClipboard)),
      fl::Path(MOD_PATH));

   int pid;
   if (CurrentTask()->get(FID_ProcessID, pid) IS ERR::Okay) glProcessID = std::to_string(pid);

   return clClipboard ? ERR::Okay : ERR::AddClass;
}
