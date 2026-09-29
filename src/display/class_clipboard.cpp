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

On Wayland, plain UTF-8 text and local file references are exchanged with other desktop applications through the
compositor's data device.  Incoming transfers complete asynchronously; GetFiles() may return `ERR::NoData` until a new
selection has finished transferring.  Other non-Windows builds keep clipboard data local unless their display driver
provides host integration.

When history buffering is active, a fixed number of clip groups is retained and the oldest group is removed when the
limit is exceeded.  Cached clipboard files are kept under `clipboard:` and stale generated files are cleaned up during
display initialisation.
-END-

*********************************************************************************************************************/

#include "defs.h"
#include <kotuku/modules/time.h>
#include <fstream>

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
static std::atomic_bool glHostSelectionGone = false;
static CLIPTYPE glDropType = CLIPTYPE::NIL;
static std::vector<std::string> glDropPaths;
static std::string glDropText;
#ifdef _WIN32
static int glLastClipID = -1;
#endif

//********************************************************************************************************************

static std::string get_datatype(CLIPTYPE);
static ERR add_clip(CLIPTYPE, const std::vector<ClipItem> &, CEF = CEF::NIL);
static ERR add_clip(std::string_view);
static ERR CLIPBOARD_AddObjects(objClipboard *, struct clip::AddObjects *);

#ifdef _WIN32
static std::u16string utf8_to_utf16(std::string_view String, bool SurrogatePairs)
{
   std::u16string result;
   if (String.empty()) return result;

   auto str = String.data();
   int chars, bytes = 0;
   for (chars=0; bytes < std::ssize(String); chars++) {
      for (++bytes; (bytes < std::ssize(String)) and ((str[bytes] & 0xc0) IS 0x80); bytes++);
   }

   result.reserve(size_t(chars));

   int pos = 0;
   while (pos < std::ssize(String)) {
      int len = UTF8CharLength(str);
      if ((len IS 0) or (pos + len > std::ssize(String))) break;

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
   if (not time.ok()) return;

   time->query();
   int64_t timestamp;
   time->getTimestamp(timestamp);
   int64_t now = timestamp / 1000000LL;
   int64_t yesterday = now - (24 * 60LL * 60LL);

   DirInfo *dir;
   if (!OpenDir("clipboard:", RDF::FILE|RDF::DATE, &dir)) {
      LocalResource free_dir(dir);

      Regex *compiled;
      if (!rx::Compile("^\\d+(?:_text|_image|_file|_object)\\d*\\.\\d{3}$", REGEX::NIL, nullptr, &compiled)) {
         while (!ScanDir(dir)) {
            if (!rx::Match(compiled, dir->Info->Name, RMATCH::NIL, nullptr)) {
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

   for (auto &item : Items) if (item.Owned) DeleteFile(item.Path, nullptr);
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
      if (!ResolvePath(item.Path, RSF::NIL, &path)) {
         list << utf8_to_utf16(path, true) << char16_t(0);
      }
   }
   list << char16_t(0); // An extra null byte is required to terminate the list for Windows HDROP

   auto str = list.str();
   auto error = (ERR)winAddFileClip(str.c_str(), str.size() * sizeof(char16_t), Cut);
   if (error != ERR::Okay) log.warning(error);
   return error;
#else
   if (glDriver and (glDriver->capabilities() & DCAP::CLIPBOARD) != DCAP::NIL) {
      std::vector<std::string> paths;
      for (auto &item : Items) paths.push_back(item.Path);
      auto error = glDriver->clipboardAddFiles(CLIPTYPE::FILE, paths, Cut);
      if (error IS ERR::Okay) glHostSelectionGone = false;
      return error;
   }
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************

static ERR add_text_to_host(objClipboard *Self, std::string_view String)
{
   kt::Log log(__FUNCTION__);

   if ((Self->Flags & CPF::DRAG_DROP) != CPF::NIL) return ERR::NoSupport;

#ifdef _WIN32
   // Copy text to the Windows clipboard.  This requires a conversion from UTF-8 to UTF-16.

   auto utf16 = utf8_to_utf16(String, false);
   utf16.push_back(0);

   auto error = (ERR)winAddClip(int(CLIPTYPE::TEXT), utf16.data(), utf16.size() * sizeof(char16_t), false);
   if (error != ERR::Okay) log.warning(error);
   return error;
#else
   if (glDriver and (glDriver->capabilities() & DCAP::CLIPBOARD) != DCAP::NIL) {
      auto error = glDriver->clipboardAddText(std::string(String).c_str());
      if (error IS ERR::Okay) glHostSelectionGone = false;
      return error;
   }
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
strview Path: Path of the file to add.
int(CEF) Flags: Optional flags.

-ERRORS-
Okay: The files were added to the clipboard.
NullArgs
MissingPath: `Path` was not specified.

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddFile(objClipboard *Self, struct clip::AddFile *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if (Args->Path.empty()) return log.warning(ERR::MissingPath);

   std::string path(Args->Path);
   log.branch("Path: %s", path.c_str());

   std::vector<ClipItem> items = { path };
   if (glDriver and (glDriver->displayType() IS DT::WAYLAND) and
         ((Self->Flags & CPF::DRAG_DROP) IS CPF::NIL)) {
      if (auto error = add_clip(Args->Datatype, items, Args->Flags & (CEF::DELETE|CEF::EXTEND));
            error != ERR::Okay) return error;
      if (Args->Datatype IS CLIPTYPE::FILE) {
         std::vector<ClipItem> current;
         {
            const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);
            if (not glClips.empty() and (glClips.front().Datatype IS CLIPTYPE::FILE)) current = glClips.front().Items;
         }
         add_file_to_host(Self, current, ((Args->Flags & CEF::DELETE) != CEF::NIL));
      }
      else if (Args->Datatype IS CLIPTYPE::TEXT) {
         std::string resolved;
         if (ResolvePath(path, RSF::NIL, &resolved) IS ERR::Okay) {
            std::ifstream stream(resolved, std::ios::binary);
            if (stream) {
               std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
               add_text_to_host(Self, content);
            }
         }
      }
      return ERR::Okay;
   }
   if (Args->Datatype IS CLIPTYPE::TEXT and glDriver and
         (glDriver->capabilities() & DCAP::CLIPBOARD) != DCAP::NIL) {
      std::string resolved;
      if (ResolvePath(path, RSF::NIL, &resolved) IS ERR::Okay) {
         std::ifstream stream(resolved, std::ios::binary);
         if (stream) {
            std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            glDriver->clipboardAddText(content.c_str());
            glHostSelectionGone = false;
         }
      }
   }
   else if (!add_file_to_host(Self, items, ((Args->Flags & CEF::DELETE) != CEF::NIL) ? true : false)) {
      if (glHistoryLimit <= 1 and glDriver and (glDriver->displayType() != DT::WAYLAND)) return ERR::Okay;
   }

   return add_clip(Args->Datatype, items, Args->Flags & (CEF::DELETE|CEF::EXTEND));
}

/*********************************************************************************************************************

-METHOD-
AddObjects: Saves objects to clipboard cache files.

Use AddObjects() to copy one or more objects by asking each object to save itself to a generated file in the
`clipboard:` volume.  This avoids requiring the caller to create temporary files before copying object data.

If `Datatype` is `CLIPTYPE::NIL`, the clipboard chooses a datatype from the source object's class where possible.
@Image objects are stored as `CLIPTYPE::IMAGE`, sound objects are stored as `CLIPTYPE::AUDIO`, and unrecognised
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

-TAGS-
blocking, mutates-object, creates-resource
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddObjects(objClipboard *Self, struct clip::AddObjects *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Objects) or (not Args->Objects[0])) return log.warning(ERR::NullArgs);

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
               if (object.obj->classID() IS CLASSID::IMAGE) datatype = CLIPTYPE::IMAGE;
               else if (object.obj->classID() IS CLASSID::SOUND) datatype = CLIPTYPE::AUDIO;
               else datatype = CLIPTYPE::OBJECT;
            }

            char idx[5];
            snprintf(idx, sizeof(idx), ".%.3d", i);
            auto path = std::string("clipboard:") + glProcessID + "_" + get_datatype(datatype) +
               std::to_string(counter) + idx;

            auto file = objFile::create { fl::Path(path), fl::Flags(FL::WRITE|FL::NEW) };
            if (file.ok()) {
               if (auto error = acSaveToObject(*object, *file); error != ERR::Okay) return log.warning(error);
               items.emplace_back(path, true);
            }
            else return ERR::CreateFile;
         }
      }
      else return ERR::Lock;
   }

   if (!add_file_to_host(Self, items, ((Args->Flags & CEF::DELETE) != CEF::NIL) ? true : false)) {
      if (glHistoryLimit <= 1 and glDriver and (glDriver->displayType() != DT::WAYLAND)) return ERR::Okay;
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
strview String: UTF-8 text to add to the clipboard.

-ERRORS-
Okay
NullArgs
CreateFile

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddText(objClipboard *Self, struct clip::AddText *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if (Args->String.empty()) return ERR::Okay;

   add_text_to_host(Self, Args->String);

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
   if (glDriver and (glDriver->capabilities() & DCAP::CLIPBOARD) != DCAP::NIL) glDriver->clipboardClear();
   std::string path;
   if (!ResolvePath("clipboard:", RSF::NO_FILE_CHECK, &path)) {
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

   if (not Args) return log.warning(ERR::NullArgs);

   if (Args->Datatype IS DATA::TEXT) {
      log.msg("Copying text to the clipboard.");

      if ((not Args->Buffer.data()) and (not Args->Buffer.empty())) return log.warning(ERR::NullArgs);
      if (auto error = add_text_to_host(Self,
            std::string_view((const char *)Args->Buffer.data(), Args->Buffer.size()));
            (error != ERR::Okay) and (error != ERR::NoSupport)) {
         return log.warning(error);
      }

      std::vector<ClipItem> items = { ClipItem(std::string("clipboard:") + glProcessID + "_text" +
         std::to_string(glCounter++) + std::string(".000"), true) };
      if (auto error = add_clip(CLIPTYPE::TEXT, items); !error) {
         auto file = objFile::create { fl::Path(items[0].Path), fl::Flags(FL::NEW|FL::WRITE),
            fl::Permissions(PERMIT::READ|PERMIT::WRITE) };
         if (file.ok()) {
            if (file->write(Args->Buffer) != ERR::Okay) {
               return log.warning(ERR::Write);
            }
            return ERR::Okay;
         }
         else return log.warning(ERR::CreateObject);
      }
      else return log.warning(error);
   }
   else if ((Args->Datatype IS DATA::REQUEST) and ((Self->Flags & CPF::DRAG_DROP) != CPF::NIL))  {
      if ((not Args->Buffer.data()) or (not Args->Object)) return log.warning(ERR::NullArgs);
      if (Args->Buffer.size() < sizeof(struct dcRequest)) return log.warning(ERR::Args);

      struct dcRequest request;
      copymem(Args->Buffer.data(), &request, sizeof(request));
      log.branch("Data request from #%d received for item %d, datatype %d", Args->Object->UID, request.Item,
         request.Preference[0]);

      ERR error = ERR::Okay;
      if (Self->RequestHandler.stale()) {
         Self->RequestHandler.unpin();
         Self->RequestHandler.disable();
      }

      if (Self->RequestHandler.isC()) {
         auto routine = (ERR (*)(objClipboard *, OBJECTPTR, int, char *, APTR))Self->RequestHandler.Routine;
         kt::SwitchContext ctx(Self->RequestHandler.Context);
         error = routine(Self, Args->Object, request.Item, request.Preference, Self->RequestHandler.Meta);
      }
      else if (Self->RequestHandler.isScript()) {
         std::span<char> span(request.Preference, sizeof(request.Preference));
         if (sc::Call(Self->RequestHandler, std::to_array<ScriptArg>({
            { "Clipboard", Self, FD_OBJECTPTR },
            { "Requester", Args->Object, FD_OBJECTPTR },
            { "Item",      request.Item },
            { "Datatypes", &span, FDF_SPAN|FD_BYTE }
         }), error) != ERR::Okay) error = ERR::Terminate;
      }
      else error = log.warning(ERR::FieldNotSet);

      if (error IS ERR::Terminate) {
         Self->RequestHandler.unpin();
         Self->RequestHandler.disable();
      }

      return error;
   }
   else log.warning("Unrecognised data type %d.", int(Args->Datatype));

   return ERR::NoSupport;
}

//********************************************************************************************************************

objClipboard::~objClipboard()
{
   if (RequestHandler.defined()) RequestHandler.unpin();
}

/*********************************************************************************************************************

-METHOD-
GetFiles: Retrieve the most recently clipped data as a list of files.

GetFiles() returns clipboard entries as readable file paths.  The caller can request a specific set of datatypes through
`Filter`, or pass `CLIPTYPE::NIL` to accept any datatype.

Without history buffering, only the most recent clip group is available.  With history buffering enabled, pass
`CLIPTYPE::NIL` as `Filter` and increment `Index` to scan retained history from newest to oldest until
`ERR::OutOfRange` is returned.

On success, `Datatype` reports the datatype of the returned clip and `Files` receives the matching file paths.  How the
caller reads each file depends on `Datatype`; ~Core.IdentifyFile() can also be used to find a class that supports the
data.  `Files` must refer to an empty caller-owned array and remains owned by the caller after the method returns.
On Wayland, an incoming selection is copied asynchronously into the local file cache.  This method returns
`ERR::NoData` while that transfer is pending when history buffering is disabled.

If `CEF::DELETE` is returned in `Flags`, the caller must delete the source files after successfully copying the data in
order to complete a cut operation.  When cutting and pasting files within the same file system, ~Core.MoveFile() is
usually the most efficient way to consume those entries.

-INPUT-
int(CLIPTYPE) Filter: Datatype filter.  Set to zero to accept any datatype.
int Index: History index to read when Filter is zero.  Zero is the most recent clip group.
&int(CLIPTYPE) Datatype: Datatype of the returned clip group.
^&vector(string) Files: An empty string array is required to receive the file list.
&int(CEF) Flags: Result flags.  If the delete flag is set, delete the files after use to complete a cut operation.

-ERRORS-
Okay: A matching clip was found and returned.
NullArgs
OutOfRange: The specified `Index` is out of the range of the available clip items.
NoData: No clip was available that matched the requested data type.

-TAGS-
mutates-input
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_GetFiles(objClipboard *Self, struct clip::GetFiles *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Files)) return log.warning(ERR::NullArgs);

   log.branch("Datatype: $%.8x", int(Args->Datatype));

   if ((Self->Flags & CPF::HISTORY_BUFFER) IS CPF::NIL) {
#ifdef _WIN32
      // If the history buffer is disabled then we need to actively retrieve whatever Windows has on the clipboard.
      if (winCurrentClipboardID() != glLastClipID) winCopyClipboard();
#endif
   }

   if (glHostSelectionGone and (Self->Flags & CPF::HISTORY_BUFFER) IS CPF::NIL) return ERR::NoData;

   const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);

   if (glClips.empty()) return ERR::NoData;

   ClipRecord *clip = &glClips.front();

   // Find the first clipboard entry to match what has been requested

   if ((Self->Flags & CPF::HISTORY_BUFFER) != CPF::NIL) {
      if (Args->Filter IS CLIPTYPE::NIL) { // Return the newest clip or the item at Index.
         if ((Args->Index < 0) or (Args->Index >= int(glClips.size()))) return log.warning(ERR::OutOfRange);
         auto selected = glClips.begin();
         std::advance(selected, Args->Index);
         clip = &*selected;
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

         if (not found) {
            log.warning("No clips available for datatype $%x", int(Args->Filter));
            return ERR::NoData;
         }
      }
   }
   else if (Args->Filter != CLIPTYPE::NIL) {
      if ((clip->Datatype & Args->Filter) IS CLIPTYPE::NIL) return ERR::NoData;
   }

   auto &list = *Args->Files;
   Args->Flags    = clip->Flags;
   Args->Datatype = clip->Datatype;

   for (auto &item : clip->Items) {
      list.push_back(item.Path);
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR CLIPBOARD_Init(objClipboard *Self)
{
   if ((Self->Flags & CPF::HISTORY_BUFFER) != CPF::NIL) glHistoryLimit = MAX_CLIPS;

   // Create a directory under temp: to store clipboard data

   CreateFolder("clipboard:", PERMIT::READ|PERMIT::WRITE);

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

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_Remove(objClipboard *Self, struct clip::Remove *Args)
{
   kt::Log log;

   if ((not Args) or (Args->Datatype IS CLIPTYPE::NIL)) return log.warning(ERR::NullArgs);

   log.branch("Datatype: $%x", int(Args->Datatype));

   const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);

   for (auto it=glClips.begin(); it != glClips.end();) {
      if ((it->Datatype & Args->Datatype) != CLIPTYPE::NIL) {
         if (it IS glClips.begin()) {
            #ifdef _WIN32
            winClearClipboard();
            #endif
            if (glDriver and (glDriver->capabilities() & DCAP::CLIPBOARD) != DCAP::NIL)
               glDriver->clipboardClear();
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

static ERR GET_RequestHandler(objClipboard *Self, FUNCTION * &Value)
{
   if (Self->RequestHandler.defined()) {
      Value = &Self->RequestHandler;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_RequestHandler(objClipboard *Self, FUNCTION *Value)
{
   if (Self->RequestHandler.defined()) {
      Self->RequestHandler.unpin();
      Self->RequestHandler.disable();
   }

   if (Value) {
      Self->RequestHandler = *Value;
      if (Self->RequestHandler.defined()) Self->RequestHandler.pin();
   }
   else Self->RequestHandler.disable();

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

            it->Items.insert(it->Items.end(), Items.begin(), Items.end());
            glClips.splice(glClips.begin(), glClips, it);
            return ERR::Okay;
         }
      }
   }

   // Remove any existing clips that match this datatype

   for (auto it = glClips.begin(); it != glClips.end(); ) {
      if (it->Datatype IS Datatype) it = glClips.erase(it);
      else it++;
   }

   if (int(glClips.size()) >= glHistoryLimit) glClips.pop_back(); // Remove oldest clip if history buffer is full.

   glClips.emplace_front(Datatype, Flags & CEF::DELETE, Items);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR add_clip(std::string_view String)
{
   kt::Log log(__FUNCTION__);
   log.branch();

   std::vector<ClipItem> items = { ClipItem(std::string("clipboard:") + glProcessID + "_text" +
      std::to_string(glCounter++) + ".000", true) };
   if (auto error = add_clip(CLIPTYPE::TEXT, items); !error) {
      kt::Create<objFile> file = { fl::Path(items[0].Path), fl::Flags(FL::WRITE|FL::NEW),
         fl::Permissions(PERMIT::READ|PERMIT::WRITE) };
      if (file.ok()) {
         if (auto error = file->write(std::span<const int8_t>((const int8_t *)String.data(), String.size()));
             error != ERR::Okay) return log.warning(error);
         return ERR::Okay;
      }
      else return log.warning(ERR::CreateFile);
   }
   else return log.warning(error);
}

static int hex_digit(char Value)
{
   if ((Value >= '0') and (Value <= '9')) return Value - '0';
   if ((Value >= 'a') and (Value <= 'f')) return Value - 'a' + 10;
   if ((Value >= 'A') and (Value <= 'F')) return Value - 'A' + 10;
   return -1;
}

void lose_host_clipboard()
{
   glHostSelectionGone = true;
}

void receive_host_clipboard(CSTRING Mime, std::string_view Data, bool Dropped, OBJECTID SurfaceID)
{
   if (not Mime) return;
   const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);
   if (Dropped) { glDropType = CLIPTYPE::NIL; glDropPaths.clear(); glDropText.clear(); }
   if (std::string_view(Mime) IS "text/uri-list") {
      std::vector<ClipItem> items;
      size_t start = 0;
      while (start < Data.size()) {
         auto end = Data.find_first_of("\r\n", start);
         if (end IS std::string_view::npos) end = Data.size();
         auto line = Data.substr(start, end - start);
         start = Data.find_first_not_of("\r\n", end);
         if (start IS std::string_view::npos) start = Data.size();
         if (not line.starts_with("file://")) continue;
         line.remove_prefix(7);
         if (line.starts_with("localhost")) line.remove_prefix(9);
         if (line.empty() or (line[0] != '/')) continue;
         std::string path;
         for (size_t i=0; i < line.size(); i++) {
            if ((line[i] IS '%') and (i + 2 < line.size())) {
               auto hi = hex_digit(line[i + 1]);
               auto lo = hex_digit(line[i + 2]);
               if ((hi >= 0) and (lo >= 0)) {
                  auto ch = char((hi << 4) | lo);
                  if (ch IS 0) { path.clear(); break; }
                  path.push_back(ch);
                  i += 2;
                  continue;
               }
            }
            path.push_back(line[i]);
         }
         if (not path.empty()) items.emplace_back(path);
      }
      if (not items.empty()) {
         if (Dropped) {
            glDropPaths.clear();
            for (auto &item : items) glDropPaths.push_back(item.Path);
            glDropType = CLIPTYPE::FILE;
         }
         else if (add_clip(CLIPTYPE::FILE, items) IS ERR::Okay) glHostSelectionGone = false;
      }
   }
   else if ((std::string_view(Mime) IS "text/plain") or
         (std::string_view(Mime) IS "text/plain;charset=utf-8")) {
      if (Dropped) { glDropText = Data; glDropType = CLIPTYPE::TEXT; }
      else if (add_clip(Data) IS ERR::Okay) glHostSelectionGone = false;
   }
   if (Dropped and SurfaceID and glDropType != CLIPTYPE::NIL) {
      const char datatype[] = { char(glDropType IS CLIPTYPE::FILE ? DATA::FILE : DATA::TEXT), 0 };
      display::DriverDragDropped(SurfaceID, datatype);
   }
}

static void append_xml(std::string &Output, std::string_view Text)
{
   for (char ch : Text) {
      if (ch IS '&') Output.append("&amp;");
      else if (ch IS '<') Output.append("&lt;");
      else if (ch IS '>') Output.append("&gt;");
      else if (ch IS '"') Output.append("&quot;");
      else Output.push_back(ch);
   }
}

ERR respond_wayland_drop_request(OBJECTPTR Display, OBJECTPTR Requester, int Item, char Preference)
{
   if ((not Display) or (not Requester)) return ERR::NullArgs;
   const std::lock_guard<std::recursive_mutex> lock(glClipboardLock);
   std::string xml = "<receipt totalitems=\"";
   if ((glDropType IS CLIPTYPE::FILE) and (Preference IS char(DATA::FILE))) {
      xml.append(std::to_string(glDropPaths.size()));
      xml.append("\" id=\"").append(std::to_string(Item)).append("\">");
      for (auto &path : glDropPaths) {
         xml.append("<file path=\"");
         append_xml(xml, path);
         xml.append("\"/>");
      }
   }
   else if ((glDropType IS CLIPTYPE::TEXT) and (Preference IS char(DATA::TEXT))) {
      xml.append("1\" id=\"").append(std::to_string(Item)).append("\"><text>");
      append_xml(xml, glDropText);
      xml.append("</text>");
   }
   else return ERR::NoSupport;
   xml.append("</receipt>");
   return acDataFeed(Requester, Display, DATA::RECEIPT,
      std::span<const int8_t>((const int8_t *)xml.data(), xml.size()));
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

   add_clip(buffer.str());
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
   { "RequestHandler", FDF_FUNCTION|FDF_RW|FDF_PURE, GET_RequestHandler, SET_RequestHandler },
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
      fl::Path("modules:display"));

   int pid;
   if (!CurrentTask()->getProcess(pid)) glProcessID = std::to_string(pid);

   return clClipboard ? ERR::Okay : ERR::AddClass;
}
