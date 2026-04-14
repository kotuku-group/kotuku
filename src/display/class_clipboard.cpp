/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Clipboard: The Clipboard class manages cut, copy and paste between applications.

The Clipboard class manages data transfer between applications on behalf of the user.  Depending on the host system,
behaviour between platforms can vary.

On Windows the clipboard is tightly integrated by default, allowing it to support native Windows applications.  This
reduces the default feature set, but ensures that the clipboard behaves in a way that the user would expect it to.
If historical buffering is enabled with the `CPF::HISTORY_BUFFER` option then the clipboard API will actively monitor
the clipboard and store copied data in the local `clipboard:` file cache.  This results in additional overhead to
clipboard management.

On Linux the clipboard is localised and data is shared between Kōtuku applications only.

Multiple clipboard objects can be created, but they will share the same group of clipped data for the logged-in user.

There is a limit on the number of clipped items that can be stored in the clipboard.  Only 1 grouping of each
datatype is permitted (for example, only one group of image clips may exist at any time).  In historical buffer mode
there is a fixed limit to the clip count and the oldest members are automatically removed.
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

//********************************************************************************************************************
// Remove stale clipboard files that are over 24hrs old

void clean_clipboard(void)
{
   auto time = objTime::create { };
   if (!time.ok()) return;

   time->query();
   int64_t now = time->get<int64_t>(FID_TimeStamp) / 1000000LL;
   int64_t yesterday = now - (24 * 60LL * 60LL);

   DirInfo *dir;
   if (OpenDir("clipboard:", RDF::FILE|RDF::DATE, &dir) IS ERR::Okay) {
      LocalResource free_dir(dir);

      Regex *compiled;
      if (rx::Compile("^\\d+(?:_text|_image|_file|_object)\\d*\\.\\d{3}$", REGEX::NIL, nullptr, &compiled) IS ERR::Okay) {
         while (ScanDir(dir) IS ERR::Okay) {
            if (rx::Match(compiled, dir->Info->Name, RMATCH::NIL, nullptr) IS ERR::Okay) {
               if (dir->Info->TimeStamp < yesterday) {
                  std::string path("clipboard:");
                  path.append(dir->Info->Name);
                  DeleteFile(path.c_str(), nullptr);
               }
            }
         }
         FreeResource(compiled);
      }
   }
}

//********************************************************************************************************************

ClipRecord::~ClipRecord() {
   pf::Log log(__FUNCTION__);

   if (Datatype != CLIPTYPE::FILE) {
      log.branch("Deleting clip files for %s datatype.", get_datatype(Datatype).c_str());
      for (auto &item : Items) DeleteFile(item.Path.c_str(), nullptr);
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
   pf::Log log;

   if ((Self->Flags & CPF::DRAG_DROP) != CPF::NIL) return ERR::NoSupport;

#ifdef _WIN32
   // Build a list of resolved path names in a new buffer that is suitable for passing to Windows.

   std::basic_stringstream<char16_t> list;
   for (auto &item : Items) {
      std::string path;
      if (ResolvePath(item.Path, RSF::NIL, &path) IS ERR::Okay) {
         // Convert UTF-8 to UTF-16 manually
         std::u16string dest;
         const char *src = path.c_str();
         while (*src) {
            uint32_t codepoint = 0;
            int len = UTF8CharLength(src);
            if (len IS 1) codepoint = *src;
            else codepoint = UTF8ReadValue(src, nullptr);

            if (codepoint < 0x10000) {
               dest.push_back(static_cast<char16_t>(codepoint));
            }
            else {
               // Surrogate pair for codepoints >= 0x10000
               codepoint -= 0x10000;
               dest.push_back(static_cast<char16_t>(0xD800 + (codepoint >> 10)));
               dest.push_back(static_cast<char16_t>(0xDC00 + (codepoint & 0x3FF)));
            }
            src += len;
         }
         list << dest << '\0';
      }
   }
   list << '\0'; // An extra null byte is required to terminate the list for Windows HDROP

   auto str = list.str();
   winAddFileClip(str.c_str(), str.size() * sizeof(char16_t), Cut);
   return ERR::Okay;
#else
   return ERR::NoSupport;
#endif
}

//********************************************************************************************************************

static ERR add_text_to_host(objClipboard *Self, CSTRING String, int Length = 0x7fffffff)
{
   pf::Log log(__FUNCTION__);

   if ((Self->Flags & CPF::DRAG_DROP) != CPF::NIL) return ERR::NoSupport;

#ifdef _WIN32
   // Copy text to the windows clipboard.  This requires a conversion from UTF-8 to UTF-16.

   auto str = String;
   int chars, bytes = 0;
   for (chars=0; (str[bytes]) and (bytes < Length); chars++) {
      for (++bytes; (bytes < Length) and ((str[bytes] & 0xc0) IS 0x80); bytes++);
   }

   std::vector<uint16_t> utf16(size_t(chars + 1) * sizeof(uint16_t));

   int i = 0;
   while (i < bytes) {
      int len = UTF8CharLength(str);
      if (i + len >= bytes) break; // Avoid corrupt UTF-8 sequences resulting in minor buffer overflow
      utf16[i++] = UTF8ReadValue(str, nullptr);
      str += len;
   }
   utf16[i] = 0;

   auto error = (ERR)winAddClip(int(CLIPTYPE::TEXT), utf16.data(), utf16.size() * sizeof(uint16_t), false);
   if (error != ERR::Okay) log.warning(error);
   return error;
#else
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-METHOD-
AddFile: Add files to the clipboard.

This method is used to add a file to the clipboard.  You are required to specify the type of data that is represented
by the file. This allows the file content to be pasted by other applications that understand the data.  Adding files
to the clipboard with a known datatype can be very efficient compared to other methods, as it saves loading the data
into memory until the user is ready to paste the content.

Recognised data types are:

<types lookup="CLIPTYPE"/>

Optional flags that may be passed to this method are as follows:

<types lookup="CEF"/>

-INPUT-
int(CLIPTYPE) Datatype: Set this argument to indicate the type of data you are copying to the clipboard.
cstr Path: The path of the file to add.
int(CEF) Flags: Optional flags.

-ERRORS-
Okay: The files were added to the clipboard.
NullArgs
MissingPath: The Files argument was not correctly specified.
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddFile(objClipboard *Self, struct clip::AddFile *Args)
{
   pf::Log log;

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
AddObjects: Extract data from objects and add it all to the clipboard.

Data can be saved to the clipboard directly from an object if the object's class supports the SaveToObject() action.  The
clipboard will ask that the object save its data directly to a cache file, completely removing the need for the
client to save the object data to an interim file for the clipboard.

Certain classes are recognised by the clipboard system and will be added to the correct datatype automatically (for
instance, @Picture objects will be put into the `CLIPTYPE::IMAGE` data category).  If an object's class is not recognised by
the clipboard system then the data will be stored in the `CLIPTYPE::OBJECT` category to signify that there is a class in the
system that recognises the data.  If you want to over-ride any aspect of this behaviour, force the `Datatype`
parameter with one of the available `CLIPTYPE` values.

This method supports groups of objects in a single clip, thus requires an array of object ID's terminated
with a zero entry.

Optional flags that may be passed to this method are the same as those specified in the #AddFile() method.  The
`CEF::DELETE` flag has no effect on objects.

-INPUT-
int(CLIPTYPE) Datatype: The type of data representing the objects, or NULL for automatic recognition.
ptr(oid) Objects: Array of object ID's to add to the clipboard.
int(CEF) Flags: Optional flags.

-ERRORS-
Okay: The objects were added to the clipboard.
Args
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddObjects(objClipboard *Self, struct clip::AddObjects *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Objects) or (!Args->Objects[0])) return log.warning(ERR::NullArgs);

   log.branch();

   int counter = glCounter++;
   CLASSID classid = CLASSID::NIL;
   auto datatype = Args->Datatype;

   std::vector<ClipItem> items;
   for (unsigned i=0; Args->Objects[i]; i++) {
      pf::ScopedObjectLock<Object> object(Args->Objects[i], 5000);
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
            if (file.ok()) acSaveToObject(*object, *file);
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

Plain UTF-8 text can be added to the clipboard using the AddText() method.

-INPUT-
cstr String: The text to add to the clipboard.

-ERRORS-
Okay
NullArgs
CreateFile
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_AddText(objClipboard *Self, struct clip::AddText *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->String)) return log.warning(ERR::NullArgs);
   if (!Args->String[0]) return ERR::Okay;

   if (add_text_to_host(Self, Args->String) IS ERR::Okay) {
      if (glHistoryLimit <= 1) return ERR::Okay;
   }

   return add_clip(Args->String);
}

/*********************************************************************************************************************
-ACTION-
Clear: Destroys all cached data that is stored in the clipboard.
-END-
*********************************************************************************************************************/

static ERR CLIPBOARD_Clear(objClipboard *Self)
{
   std::string path;
   if (ResolvePath("clipboard:", RSF::NO_FILE_CHECK, &path) IS ERR::Okay) {
      DeleteFile(path.c_str(), nullptr);
      CreateFolder(path.c_str(), PERMIT::READ|PERMIT::WRITE);
   }

   glClips.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
DataFeed: This action can be used to place data in a clipboard.

Data can be sent to a clipboard object via the DataFeed action. Currently, only the `DATA::TEXT` type is supported.
All data that is sent to a clipboard object through this action will replace any stored information that matches the
given data type.
-END-
*********************************************************************************************************************/

static ERR CLIPBOARD_DataFeed(objClipboard *Self, struct acDataFeed *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (Args->Datatype IS DATA::TEXT) {
      log.msg("Copying text to the clipboard.");

      add_text_to_host(Self, (CSTRING)Args->Buffer, Args->Size);

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
      auto request = (struct dcRequest *)Args->Buffer;
      log.branch("Data request from #%d received for item %d, datatype %d", Args->Object->UID, request->Item, request->Preference[0]);

      ERR error = ERR::Okay;
      if (Self->RequestHandler.isC()) {
         auto routine = (ERR (*)(objClipboard *, OBJECTPTR, int, char *, APTR))Self->RequestHandler.Routine;
         pf::SwitchContext ctx(Self->RequestHandler.Context);
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

      return ERR::Okay;
   }
   else log.warning("Unrecognised data type %d.", int(Args->Datatype));

   return ERR::Okay;
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

This method returns a list of items that are on the clipboard.  The caller must declare the types of data that it
supports (or zero if all datatypes are recognised).

The most recently clipped datatype is always returned.  To scan for all available clip items, set the `Filter`
parameter to zero and repeatedly call this method with incremented Index numbers until the error code `ERR::OutOfRange`
is returned.

On success this method will return a list of files (terminated with a `NULL` entry) in the `Files` parameter.  Each file is
a readable clipboard entry - how the client reads it depends on the resulting `Datatype`.  Additionally, the
~Core.IdentifyFile() function could be used to find a class that supports the data.  The resulting `Files` array is a
memory allocation that must be freed with a call to ~Core.FreeResource().

If this method returns the `CEF::DELETE` flag in the `Flags` parameter, the client must delete the source files after
successfully copying the data.  When cutting and pasting files within the file system, using ~Core.MoveFile() is
recommended as the most efficient method.

-INPUT-
int(CLIPTYPE) Filter: Filter down to the specified data type.  This parameter will be updated to reflect the retrieved data type when the method returns.  Set to zero to disable.
int Index: If the `Filter` parameter is zero and clipboard history is enabled, this parameter refers to a historical clipboard item, with zero being the most recent.
&int(CLIPTYPE) Datatype: The resulting datatype of the requested clip data.
!array(cstr) Files: The resulting location(s) of the requested clip data are returned in this parameter; terminated with a `NULL` entry.  The client must free the returned array with ~Core.FreeResource().
&int(CEF) Flags: Result flags are returned in this parameter.  If `DELETE` is defined, the client must delete the files after use in order to support the 'cut' operation.

-ERRORS-
Okay: A matching clip was found and returned.
Args:
OutOfRange: The specified `Index` is out of the range of the available clip items.
NoData: No clip was available that matched the requested data type.
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_GetFiles(objClipboard *Self, struct clip::GetFiles *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.branch("Datatype: $%.8x", int(Args->Datatype));

   Args->Files = nullptr;

   if ((Self->Flags & CPF::HISTORY_BUFFER) IS CPF::NIL) {
#ifdef _WIN32
      // If the history buffer is disabled then we need to actively retrieve whatever Windows has on the clipboard.
      if (winCurrentClipboardID() != glLastClipID) winCopyClipboard();
#endif
   }

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
Remove: Remove items from the clipboard.

The Remove() method will clear all items that match a specified datatype.  Clear multiple datatypes by combining flags
in the `Datatype` parameter.  To clear all content from the clipboard, use the #Clear() action instead of this method.

-INPUT-
int(CLIPTYPE) Datatype: The datatype(s) that will be deleted (datatypes may be logically-or'd together).

-ERRORS-
Okay
NullArgs
AccessMemory: The clipboard memory data was not accessible.
-END-

*********************************************************************************************************************/

static ERR CLIPBOARD_Remove(objClipboard *Self, struct clip::Remove *Args)
{
   pf::Log log;

   if ((!Args) or (Args->Datatype IS CLIPTYPE::NIL)) return log.warning(ERR::NullArgs);

   log.branch("Datatype: $%x", int(Args->Datatype));

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
Flags: Optional flags.

-FIELD-
RequestHandler: Provides a hook for responding to drag and drop requests.

Applications can request data from a clipboard if it is in drag-and-drop mode by sending a `DATA::REQUEST` to the
Clipboard's DataFeed action.  Doing so will result in a callback to the function that is referenced in the
RequestHandler, which must be defined by the source application.  The RequestHandler function must follow this
template:

`ERR RequestHandler(*Clipboard, OBJECTPTR Requester, int Item, BYTE Datatypes[4])`

The function will be expected to send a `DATA::RECEIPT` to the object referenced in the Requester paramter.  The
receipt must provide coverage for the referenced Item and use one of the indicated Datatypes as the data format.
If this cannot be achieved then `ERR::NoSupport` should be returned by the function.

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
   pf::Log log(__FUNCTION__);

   log.branch("Datatype: $%x, Flags: $%x, Total Items: %d", int(Datatype), int(Flags), int(Items.size()));

   if (Items.empty()) return ERR::Args;

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
   pf::Log log(__FUNCTION__);
   log.branch();

   std::vector<ClipItem> items = { std::string("clipboard:") + glProcessID + "_text" + std::to_string(glCounter++) + ".000" };
   if (auto error = add_clip(CLIPTYPE::TEXT, items); error IS ERR::Okay) {
      pf::Create<objFile> file = { fl::Path(items[0].Path), fl::Flags(FL::WRITE|FL::NEW), fl::Permissions(PERMIT::READ|PERMIT::WRITE) };
      if (file.ok()) {
         file->write(String, strlen(String), 0);
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
   pf::Log log("Clipboard");
   log.branch("Application has detected text on the clipboard.");

   add_clip(String);
   glLastClipID = winCurrentClipboardID();
}

//********************************************************************************************************************
// Called when the Windows clipboard holds new file references.  We store a direct reference to the file path.

extern "C" void report_windows_files(APTR Data, int CutOperation)
{
   pf::Log log("Clipboard");
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
   pf::Log log("Clipboard");
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
               utf8_path.push_back(static_cast<char>(codepoint));
            }
            else if (codepoint < 0x800) {
               utf8_path.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
               utf8_path.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else if (codepoint < 0x10000) {
               utf8_path.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
               utf8_path.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
               utf8_path.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
            }
            else {
               utf8_path.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
               utf8_path.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
               utf8_path.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
               utf8_path.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
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
   pf::Log log("Clipboard");
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
   pf::Log log(__FUNCTION__);
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
