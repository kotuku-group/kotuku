/*********************************************************************************************************************

-CLASS-
Document: Provides document display and editing facilities.

The Document class offers a complete page layout engine, providing rich text display features for creating complex
documents and text-based interfaces.  Internally, document data is maintained as a serial byte stream and all
object model information from the source is discarded.  This simplification of the data makes it possible to
edit the document in-place, much the same as any word processor.  Alternatively it can be used for presentation
purposes only, similarly to PDF or HTML formats.  Presentation is achieved by building a vector scene graph in
conjunction with the @Vector module.  This means that the output is compatible with SVG and can be manipulated in
detail with our existing vector API.  Consequently, document formatting is closely integrated with SVG concepts
and seamlessly inherits SVG functionality such as filling and stroking commands.

The native document format for Kōtuku is RIPL.  Documentation for RIPL is available in the Kotuku Wiki.  Other
document formats may be supported as derived classes, but bear in mind that document parsing is a one-way trip and
stateful information such as the HTML DOM is not supported.

The Document class does not include a security barrier in its current form.  Documents that include scripted code
should not be processed unless they originate from a trusted source and are confirmed as such.
To mitigate security problems, we recommend that the application is built with some form of sandbox that can prevent
the system being compromised by bad actors.  Utilising a project such as Win32 App Isolation
https://github.com/microsoft/win32-app-isolation is one potential way of doing this.

-END-

*********************************************************************************************************************/

static void notify_disable_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   if (!Result) acDisable(CurrentContext());
}

static void notify_enable_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   if (!Result) acEnable(CurrentContext());
}

static void notify_free_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extDocument *)CurrentContext();
   Self->Scene = nullptr;
   Self->Viewport = nullptr;
   Self->Page = nullptr;
   Self->View = nullptr;

   // If the viewport is being forcibly terminated (e.g. by window closure) then the cleanest way to deal with
   // lingering page resources is to remove them now.

   Self->Resources.clear();
}

// Used by script callbacks for subscribers that disappear without notice.

static void notify_free_script_context(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extDocument *)CurrentContext();
   if ((Self->EventCallback.isScript()) and (Self->EventCallback.Context IS Object)) Self->EventCallback.clear();

   for (int t=0; t < int(DRT::END); t++) {
restart:
      auto &triggers = Self->Triggers[t];
      for (auto cb=triggers.begin(); cb != triggers.end(); cb++) {
         if (cb->Context IS Object) {
            Self->Triggers[t].erase(cb);
            goto restart;
         }
      }
   }
}

//********************************************************************************************************************

static void notify_focus_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extDocument *)CurrentContext();

   if (Result != ERR::Okay) return;

   Self->HasFocus = true;

   if (Self->FocusIndex != -1) set_focus(Self, Self->FocusIndex, "FocusNotify");
}

static void notify_lostfocus_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   if (Result != ERR::Okay) return;

   auto Self = (extDocument *)CurrentContext();
   Self->HasFocus = false;

   // Redraw any selected link so that it is unhighlighted

   if ((Self->FocusIndex >= 0) and (Self->FocusIndex < int(Self->Tabs.size()))) {
      if (Self->Tabs[Self->FocusIndex].type IS TT::LINK) {
         for (auto &link : Self->Links) {
            if (link.origin.uid IS std::get<BYTECODE>(Self->Tabs[Self->FocusIndex].ref)) {
               Self->Page->draw();
               break;
            }
         }
      }
   }
}

//********************************************************************************************************************
// Receiver for events from Self->View, primarily path changes.
//
// Bear in mind that the XOffset and YOffset of the document's View must be zero initially, and will be controlled by
// the scrollbar.  For that reason we don't need to do much here other than update the layout of the page.

static ERR feedback_view(objVectorViewport *View, FM Event)
{
   kt::Log log(__FUNCTION__);
   auto Self = (extDocument *)CurrentContext();

   auto width  = View->get<double>(FID_ViewWidth);
   auto height = View->get<double>(FID_ViewHeight);
   if (not width) width = View->get<double>(FID_Width);
   if (not height) height = View->get<double>(FID_Height);

   if ((Self->VPWidth IS width) and (Self->VPHeight IS height)) return ERR::Okay;

   log.traceBranch("Redimension: %gx%g -> %gx%g", Self->VPWidth, Self->VPHeight, width, height);

   Self->VPWidth = width;
   Self->VPHeight = height;

   // The resize event is triggered just prior to the layout of the document.  The recipient
   // function can resize elements on the page in advance of the new layout.

   for (auto &trigger : copy_triggers(Self, DRT::BEFORE_LAYOUT)) {
      if (trigger.isScript()) {
         sc::Call(trigger, std::to_array<ScriptArg>({ { "ViewWidth",  Self->VPWidth }, { "ViewHeight", Self->VPHeight } }));
      }
      else if (trigger.isC()) {
         auto routine = (void (*)(APTR, extDocument *, int, int, APTR))trigger.Routine;
         kt::SwitchContext context(trigger.Context);
         routine(trigger.Context, Self, Self->VPWidth, Self->VPHeight, trigger.Meta);
      }
   }

   Self->UpdatingLayout = true;

#ifndef RETAIN_LOG_LEVEL
   kt::LogLevel level(2);
#endif

   layout_doc(Self);

   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Activate: Activates all child objects of the document.

Calling the Activate() action on a document object will forward Activate() calls to its child objects.

*********************************************************************************************************************/

static ERR DOCUMENT_Activate(extDocument *Self)
{
   kt::Log log;
   log.branch();

   kt::vector<ChildEntry> list;
   if (!ListChildren(Self->UID, &list)) {
      for (unsigned i=0; i < list.size(); i++) {
         kt::ScopedObjectLock obj(list[i].ObjectID);
         if (obj.granted()) acActivate(*obj);
      }
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
AddListener: Adds a listener to a document trigger for receiving special callbacks.

Use the AddListener() method to receive feedback whenever a document event is triggered.  Triggers are a fundamental part
of document page development, accessible through the `&lt;trigger/&gt;` tag.  Triggers are normally configured within the
document's page code, however if you need to monitor triggers from outside the loaded document's code, then AddListener()
will give you that option.

The following triggers are supported:

<types lookup="DRT">
<type name="BEFORE_LAYOUT">Document layout is about to be processed.  C/C++: `void BeforeLayout(*Caller, *Document, LONG ViewWidth, LONG ViewHeight)`</>
<type name="AFTER_LAYOUT">Document layout has been processed.  C/C++: `void AfterLayout(*Caller, *Document, LONG ViewWidth, LONG ViewHeight, LONG PageWidth, LONG PageHeight)`</>
<type name="USER_CLICK">User has clicked the document.</>
<type name="USER_CLICK_RELEASE">User click has been released.</>
<type name="USER_MOVEMENT">User is moving the pointer over the document.</>
<type name="REFRESH">Page has been refreshed.  C/C++: `void Refresh(*Caller, *Document)`</>
<type name="GOT_FOCUS">The document has received the focus.  C/C++: `void GotFocus(*Caller, *Document)`</>
<type name="LOST_FOCUS">The document has lost the focus.  C/C++: `void LostFocus(*Caller, *Document)`</>
<type name="LEAVING_PAGE">The currently loaded page is closing (either a new page is being loaded, or the document object is being freed).  C/C++: `void LeavingPage(*Caller, *Document)`</>
</type>

A listener can be removed by calling #RemoveListener(), however this is normally unnecessary. Listeners are removed
automatically if a new document source is loaded, or the document object is terminated.

Note that a trigger can have multiple listeners attached to it, so a new subscription will not replace any prior
subscriptions, nor is there any handling for multiple copies of a subscription to a trigger.

-INPUT-
int(DRT) Trigger: The unique identifier for the trigger.
ptr(func) Function: The function to call when the trigger activates.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object, callback-held
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_AddListener(extDocument *Self, doc::AddListener *Args)
{
   if ((not Args) or (Args->Trigger IS DRT::NIL) or (not Args->Function)) return ERR::NullArgs;
   if (not valid_trigger(Args->Trigger)) return ERR::OutOfRange;

   bool subscribe_context = false;
   if (Args->Function->isScript()) {
      subscribe_context = not has_script_free_callback(Self, Args->Function->Context);
   }

   Self->Triggers[int(Args->Trigger)].push_back(*Args->Function);

   // Scripts can't auto-remove listeners, so a Free subscription is necessary.  Functional
   // subscribers are expected to self-manage however.

   if (subscribe_context) SubscribeAction(Args->Function->Context, AC::Free, C_FUNCTION(notify_free_script_context));
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
CallFunction: Executes any registered function in the currently open document.

This method will execute any registered function in the currently open document.  The name of the function must be
specified in the first parameter and that function must exist in the document's default script.  If the document
contains multiple scripts, then a specific script can be referenced by using the name format `script.function` where
`script` is the name of the script that contains the function.

Arguments can be passed to the function by setting the `Args` and `TotalArgs` parameters.  These need to be specially
formatted - please refer to the @Script class' Exec method for more information on how to configure these
parameters.

-INPUT-
strview Function: The name of the function that will be called.
struct(*ScriptArg) Args: Pointer to an optional list of parameters to pass to the procedure.
int TotalArgs: The total number of entries in the `Args` array.

-ERRORS-
Okay
NullArgs

-TAGS-
callback-inlines

*********************************************************************************************************************/

static ERR DOCUMENT_CallFunction(extDocument *Self, doc::CallFunction *Args)
{
   kt::Log log;

   if ((not Args) or (Args->Function.empty())) return log.warning(ERR::NullArgs);

   // Function is in the format 'function()' or 'script.function()'

   objScript *script;
   std::string function_name, args;
   if (auto error = extract_script(Self, Args->Function, &script, function_name, args); !error) {
      return script->exec(function_name.c_str(), Args->Args, Args->TotalArgs);
   }
   else return error;
}

/*********************************************************************************************************************

-ACTION-
Clear: Clears all content from the object.

Using the Clear() action will delete all of the document's content.  The UI will be updated to reflect a clear
document.

*********************************************************************************************************************/

static ERR DOCUMENT_Clear(extDocument *Self)
{
   kt::Log log;

   log.branch();
   unload_doc(Self);
   redraw(Self, false);
   return ERR::Okay;
}

/*********************************************************************************************************************

-ACTION-
Clipboard: Full support for clipboard activity is provided through this action.
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_Clipboard(extDocument *Self, struct acClipboard *Args)
{
   kt::Log log;

   if ((not Args) or (Args->Mode IS CLIPMODE::NIL)) return log.warning(ERR::NullArgs);

   if ((Args->Mode IS CLIPMODE::CUT) or (Args->Mode IS CLIPMODE::COPY)) {
      if (Args->Mode IS CLIPMODE::CUT) log.branch("Operation: Cut");
      else log.branch("Operation: Copy");

      // Calculate the length of the highlighted document

      if (Self->SelectEnd != Self->SelectStart) {
         auto buffer = stream_to_string(Self->Stream, Self->SelectStart, Self->SelectEnd);

         // Send the document to the clipboard object

         objClipboard::create clipboard = { };
         if (clipboard.ok()) {
            if (auto error = clipboard->addText(buffer); !error) {
               // Delete the highlighted document if the CUT mode was used
               if (Args->Mode IS CLIPMODE::CUT) {
                  //delete_selection(Self);
               }
            }
            else {
               error_dialog("Clipboard Error", "Failed to add document to the system clipboard.");
               return error;
            }
         }
         else return log.warning(ERR::CreateObject);
      }

      return ERR::Okay;
   }
   else if (Args->Mode IS CLIPMODE::PASTE) {
      log.branch("Operation: Paste");

      if ((Self->Flags & DCF::EDIT) IS DCF::NIL) {
         log.warning("Edit mode is not enabled, paste operation aborted.");
         return ERR::Failed;
      }

      objClipboard::create clipboard = { };
      if (clipboard.ok()) {
         kt::vector<std::string> files;
         if (auto error = clipboard->getFiles(CLIPTYPE::TEXT, 0, nullptr, files, nullptr); !error) {
            if (files.empty()) return ERR::NoData;

            objFile::create file = { fl::Path(files[0]), fl::Flags(FL::READ) };
            if (file.ok()) {
               int size;
               if (!(error = file->get(FID_Size, size))) {
                  if (size <= 0) return ERR::NoData;

                  if (auto buffer = new (std::nothrow) char[size+1]) {
                     int result;
                     if (!(error = file->read(buffer, size, &result))) {
                        buffer[result] = 0;
                        error = Self->dataFeed(Self, DATA::TEXT, buffer, result);
                     }
                     else error_dialog("Clipboard Paste Error", ERR::Read);
                     delete[] buffer;
                     if (error != ERR::Okay) return error;
                  }
                  else {
                     error_dialog("Clipboard Paste Error", ERR::AllocMemory);
                     return ERR::AllocMemory;
                  }
               }
               else return error;
            }
            else {
               error_dialog("Paste Error", "Failed to load clipboard file \"" + std::string(files[0]) + "\"");
               return ERR::OpenFile;
            }
         }
         else return error;
      }
      else return log.warning(ERR::CreateObject);

      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************

-ACTION-
DataFeed: Document data can be sent and consumed via feeds.

Appending content to an active document can be achieved via the data feed feature.  The Document class currently
supports the `DATA::TEXT` and `DATA::XML` types for this purpose.

-ERRORS-
Okay
NullArgs
AllocMemory: The Document's memory buffer could not be expanded.
Mismatch:    The data type that was passed to the action is not supported by the Document class.
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_DataFeed(extDocument *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Buffer)) return log.warning(ERR::NullArgs);

   if ((Args->Datatype IS DATA::TEXT) or (Args->Datatype IS DATA::XML)) {
      // Incoming data is translated on the fly and added to the end of the current document page.  The original XML
      // information is retained in case of refresh.
      //
      // NOTE: Content identified by DATA::TEXT is assumed to be in a serialised XML format.

      if (not Self->initialised()) return log.warning(ERR::NotInitialised);
      if (Self->Processing) return log.warning(ERR::Recursion);

      objXML::create xml = {
         fl::Flags(XMF::INCLUDE_WHITESPACE|XMF::PARSE_HTML|XMF::STRIP_HEADERS|XMF::WELL_FORMED|XMF::READ_ONLY),
         fl::Statement(CSTRING(Args->Buffer))
      };

      if (xml.ok()) {
         if (Self->Stream.data.empty()) {
            // If the document is empty then we use the same process as load_doc()
            parser parse(Self, &Self->Stream);
            parse.process_page(*xml);
         }
         else {
            Self->invalidate_text_width_cache();
            Self->Error = insert_xml(Self, &Self->Stream, *xml, xml->Tags, Self->Stream.size(), STYLE::NIL);
         }

         auto process_error = Self->Error;
         Self->UpdatingLayout = true;
         if ((Self->initialised()) and (process_error IS ERR::Okay)) redraw(Self, true);

         #ifdef DBG_STREAM
            print_stream(Self->Stream);
         #endif
         if (process_error != ERR::Okay) return process_error;
         else return Self->Error;
      }
      else return log.warning(ERR::CreateObject);
   }
   else return log.warning(ERR::Mismatch);
}

/*********************************************************************************************************************
-ACTION-
Disable: Disables user interactivity.
-END-
*********************************************************************************************************************/

static ERR DOCUMENT_Disable(extDocument *Self)
{
   Self->Flags |= DCF::DISABLED;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Force a page layout update (if changes are pending) and redraw to the display.
-END-
*********************************************************************************************************************/

static ERR DOCUMENT_Draw(extDocument *Self)
{
   if (Self->Viewport) {
      if (Self->Processing) Self->Viewport->draw();
      else redraw(Self, false);
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

/*********************************************************************************************************************

-METHOD-
Edit: Activates a user editing section within a document.

The Edit() method will manually activate an editable section in the document.  This results in the text cursor being
placed at the start of the editable section, where the user may immediately begin editing the section via the keyboard.

If the editable section is associated with an `OnEnter` trigger, the trigger will be called when the Edit method is
invoked.

-INPUT-
strview Name: The name of the edit cell that will be activated.  If empty, the current edit cell is deactivated.
int Flags: Optional flags.

-ERRORS-
Okay
NullArgs
Search: The cell was not found.

-TAGS-
mutates-object, callback-inlines
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_Edit(extDocument *Self, doc::Edit *Args)
{
   if (not Args) return ERR::NullArgs;

   if (Args->Name.empty()) {
      if ((not Self->CursorIndex.valid()) or (not Self->ActiveEditDef)) return ERR::Okay;
      deactivate_edit(Self, true);
      return ERR::Okay;
   }
   else if (auto cellindex = Self->Stream.find_editable_cell(Args->Name); cellindex >= 0) {
      return activate_cell_edit(Self, cellindex, stream_char(0,0));
   }
   else return ERR::Search;
}

/*********************************************************************************************************************
-ACTION-
Enable: Enables object functionality.
-END-
*********************************************************************************************************************/

static ERR DOCUMENT_Enable(extDocument *Self)
{
   Self->Flags &= ~DCF::DISABLED;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
FeedParser: Private. Inserts content into a document during the parsing stage.

Private

-INPUT-
strview String: Content to insert

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object, private

*********************************************************************************************************************/

static ERR DOCUMENT_FeedParser(extDocument *Self, doc::FeedParser *Args)
{
   kt::Log log;

   if ((not Args) or (Args->String.empty())) return ERR::NullArgs;

   if (not Self->Processing) return log.warning(ERR::Failed);





   return ERR::NoSupport;
}

/*********************************************************************************************************************

-METHOD-
FindIndex: Searches the document stream for an index, returning the start and end points if found.

Use the FindIndex() method to search for indexes that have been declared in a loaded document.  Indexes are declared
using the `&lt;index/&gt;` tag and must be given a unique name.  They are useful for marking areas of interest - such as
a section of content that may change during run-time viewing, or as place-markers for rapid scrolling to an exact
document position.

If the named index exists, then the start and end points (as determined by the opening and closing of the index tag)
will be returned as byte indexes in the document stream.  The starting byte will refer to an `SCODE::INDEX_START` code and
the end byte will refer to an `SCODE::INDEX_END` code.

-INPUT-
strview Name: The name of the index to search for.
&int Start: The byte position of the index is returned in this parameter.
&int End:   The byte position at which the index ends is returned in this parameter.

-ERRORS-
Okay: The index was found and the `Start` and `End` parameters reflect its position.
NullArgs:
Search: The index was not found.

-TAGS-
pure-query

*********************************************************************************************************************/

static ERR DOCUMENT_FindIndex(extDocument *Self, doc::FindIndex *Args)
{
   kt::Log log;

   if ((not Args) or (Args->Name.empty())) return log.warning(ERR::NullArgs);

   log.trace("Name: %.*s", int(Args->Name.size()), Args->Name.data());

   auto name_hash = strihash(Args->Name);
   for (INDEX i=0; i < INDEX(Self->Stream.size()); i++) {
      if (Self->Stream[i].code IS SCODE::INDEX_START) {
         auto &index = Self->Stream.lookup<bc_index>(i);
         if (name_hash IS index.name_hash) {
            auto end_id = index.id;
            Args->Start = i;

            // Search for the end (ID match)

            for (++i; i < INDEX(Self->Stream.size()); i++) {
               if (Self->Stream[i].code IS SCODE::INDEX_END) {
                  if (end_id IS Self->Stream.lookup<bc_index_end>(i).id) {
                     Args->End = i;
                     log.trace("Found index at range %d - %d", Args->Start, Args->End);
                     return ERR::Okay;
                  }
               }
            }
         }
      }
   }

   log.detail("Failed to find index '%.*s'", int(Args->Name.size()), Args->Name.data());
   return ERR::Search;
}

/*********************************************************************************************************************
-ACTION-
Focus: Sets the user focus on the document page.
-END-
*********************************************************************************************************************/

static ERR DOCUMENT_Focus(extDocument *Self, APTR Args)
{
   acFocus(Self->Page);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR DOCUMENT_Free(extDocument *Self)
{
   if (Self->FlashTimer)  { UpdateTimer(Self->FlashTimer, 0); Self->FlashTimer = 0; }

   if ((Self->Focus) and (Self->Focus != Self->Viewport)) UnsubscribeAction(Self->Focus, AC::NIL);

   if (Self->PretextXML) { FreeResource(Self->PretextXML); Self->PretextXML = nullptr; }

   if (Self->Viewport) UnsubscribeAction(Self->Viewport, AC::NIL);

   if (Self->EventCallback.isScript()) {
      UnsubscribeAction(Self->EventCallback.Context, AC::Free);
      Self->EventCallback.clear();
   }

   unload_doc(Self, ULD::NIL);

   if (Self->Query) { FreeResource(Self->Query); Self->Query = nullptr; }
   if (Self->Page) { FreeResource(Self->Page); Self->Page = nullptr; }
   if (Self->View) { FreeResource(Self->View); Self->View = nullptr; }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
GetKey: Retrieves global variables and URI parameters.

Use GetKey() to access the global variables and URI parameters of a document.  Priority is given to global
variables if there is a name clash.

The current value of each document widget is also available as a global variable accessible from GetKey().  The
key-value will be given the same name as that specified in the widget's element.

-END-
*********************************************************************************************************************/

static ERR DOCUMENT_GetKey(extDocument *Self, struct acGetKey *Args)
{
   if ((not Args) or (not Args->Value)) return ERR::Args;

   if (auto key_it = Self->Vars.find(Args->Key); key_it != Self->Vars.end()) {
      Args->Value->assign(key_it->second);
      return ERR::Okay;
   }
   else if (auto key_it = Self->Params.find(Args->Key); key_it != Self->Params.end()) {
      Args->Value->assign(key_it->second);
      return ERR::Okay;
   }

   Args->Value->clear();
   return ERR::UnsupportedField;
}

/*********************************************************************************************************************

-METHOD-
HideIndex: Hides the content held within a named index.

The HideIndex() and #ShowIndex() methods allow the display of document content to be controlled at code level.  To control
content visibility, start by encapsulating the content in the source document with an `&lt;index&gt;` tag and ensure that
it is named.  Then make calls to HideIndex() and #ShowIndex() with the index name to manipulate visibility.

The document layout is automatically updated and pushed to the display when this method is called.

-INPUT-
strview Name: The name of the index.

-ERRORS-
Okay
NullArgs
Search

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_HideIndex(extDocument *Self, doc::HideIndex *Args)
{
   kt::Log log(__FUNCTION__);
   int tab;

   if ((not Args) or (Args->Name.empty())) return log.warning(ERR::NullArgs);

   log.msg("Index: %.*s", int(Args->Name.size()), Args->Name.data());

   auto &stream = Self->Stream;
   auto name_hash = strihash(Args->Name);
   for (INDEX i=0; i < INDEX(stream.size()); i++) {
      if (stream[i].code IS SCODE::INDEX_START) {
         auto &index = Self->Stream.lookup<bc_index>(i);
         if (name_hash IS index.name_hash) {
            if (not index.visible) return ERR::Okay; // It's already invisible!

            index.visible = false;

            {
               #ifndef RETAIN_LOG_LEVEL
               kt::LogLevel level(2);
               #endif
               Self->UpdatingLayout = true;
               layout_doc(Self);
            }

            // Any objects within the index will need to be hidden.  Also, set ParentVisible markers to false.

            for (++i; i < INDEX(stream.size()); i++) {
               auto code = stream[i].code;
               if (code IS SCODE::INDEX_END) {
                  auto &end = Self->Stream.lookup<bc_index_end>(i);
                  if (index.id IS end.id) break;
               }
               else if (code IS SCODE::IMAGE) {
                  auto &vec = Self->Stream.lookup<bc_image>(i);
                  if (not vec.rect.empty()) {
                     kt::ScopedObjectLock obj(vec.rect->UID);
                     if (obj.granted()) acHide(*obj);
                  }

                  if (auto tab = find_tabfocus(Self, TT::VECTOR, vec.rect->UID); tab >= 0) {
                     Self->Tabs[tab].active = false;
                  }
               }
               else if (code IS SCODE::LINK) {
                  auto &esclink = Self->Stream.lookup<bc_link>(i);
                  if ((tab = find_tabfocus(Self, TT::LINK, esclink.uid)) >= 0) {
                     Self->Tabs[tab].active = false;
                  }
               }
               else if (code IS SCODE::INDEX_START) {
                  auto &index = Self->Stream.lookup<bc_index>(i);
                  index.parent_visible = false;
               }
            }

            Self->Viewport->draw();
            return ERR::Okay;
         }
      }
   }

   return ERR::Search;
}

//********************************************************************************************************************

static ERR DOCUMENT_Init(extDocument *Self)
{
   kt::Log log;

   if (not Self->Viewport) {
      if ((Self->Owner) and (Self->Owner->classID() IS CLASSID::VECTORVIEWPORT)) {
         Self->Viewport = (objVectorViewport *)Self->Owner;
      }
      else return log.warning(ERR::UnsupportedOwner);
   }

   if (not Self->Focus) Self->Focus = Self->Viewport;

   if (Self->Focus->classID() != CLASSID::VECTORVIEWPORT) {
      return log.warning(ERR::WrongObjectType);
   }

   if ((Self->Focus->Flags & VF::HAS_FOCUS) != VF::NIL) Self->HasFocus = true;

   if (Self->Viewport->Scene->SurfaceID) { // Make UI subscriptions as long as we're not headless
      Self->Viewport->subscribeKeyboard(C_FUNCTION(key_event));
      SubscribeAction(Self->Focus, AC::Focus, C_FUNCTION(notify_focus_viewport));
      SubscribeAction(Self->Focus, AC::LostFocus, C_FUNCTION(notify_lostfocus_viewport));
      SubscribeAction(Self->Viewport, AC::Disable, C_FUNCTION(notify_disable_viewport));
      SubscribeAction(Self->Viewport, AC::Enable, C_FUNCTION(notify_enable_viewport));
   }

   SubscribeAction(Self->Viewport, AC::Free, C_FUNCTION(notify_free_viewport));

   Self->VPWidth  = Self->Viewport->get<double>(FID_ViewWidth);
   Self->VPHeight = Self->Viewport->get<double>(FID_ViewHeight);
   if (not Self->VPWidth) Self->VPWidth = Self->Viewport->get<double>(FID_Width);
   if (not Self->VPHeight) Self->VPHeight = Self->Viewport->get<double>(FID_Height);

   float bkgd[4] = { 1.0, 1.0, 1.0, 1.0 };
   Self->Viewport->setFillColour(bkgd, 4);

   // Allocate the view and page areas.  NB: If the parent Viewport is terminated then the
   // Page and View references will be nullified automatically.

   //if ((Self->Scene = objVectorScene::create::local(
   //      fl::Name("docScene"),
   //      fl::Owner(Self->Viewport->UID)))) {
   //}
   //else return ERR::CreateObject;

   Self->Scene = Self->Viewport->Scene;

   // Note: Initially the view is set to match the size of its container and the document will automatically
   // adjust the page width if the container is resized.  If the client wants to maintain a fixed size
   // document, e.g. for scaling, the the Width and Height of the View can be overridden at any time -
   // this is considered a legitimate approach to enforcing a fixed size document for scaling.

   if ((Self->View = objVectorViewport::create::global(
         fl::Name("docView"),
         fl::Owner(Self->Viewport->UID),
         fl::Overflow(VOF::HIDDEN),
         fl::X(0), fl::Y(0),
         fl::Width(SCALE(1.0)), fl::Height(SCALE(1.0))))) {
   }
   else return ERR::CreateObject;

   if ((Self->Page = objVectorViewport::create::global(
         fl::Name("docPage"),
         fl::Owner(Self->View->UID),
         fl::X(0), fl::Y(0),
         fl::Width(MAX_PAGE_WIDTH), fl::Height(MAX_PAGE_HEIGHT)))) {

      // Recent changes mean that page input handling could be merged with inputevent_cell()
      // if necessary (VectorScene already manages existing use-cases).
      //if (Self->Page->Scene->SurfaceID) {
      //   vecSubscribeInput(Self->Page,  JTYPE::MOVEMENT|JTYPE::BUTTON, C_FUNCTION(consume_input_events));
      //}
   }
   else return ERR::CreateObject;

   Self->View->subscribeFeedback(FM::PATH_CHANGED, C_FUNCTION(feedback_view));

   // Flash the cursor via the timer

   if ((Self->Flags & DCF::EDIT) != DCF::NIL) {
      SubscribeTimer(0.5, C_FUNCTION(flash_cursor), &Self->FlashTimer);
   }

   // Load a document file into the line array if required

   Self->UpdatingLayout = true;
   if (not Self->Path.empty()) {
      if ((Self->Path[0] != '#') and (Self->Path[0] != '?')) {
         if (auto error = load_doc(Self, Self->Path, false); error != ERR::Okay) {
            return error;
         }
      }
      else {
         // XML data is probably forthcoming and the location just contains the page name and/or parameters to use.

         process_parameters(Self, Self->Path);
      }
   }

   redraw(Self, true);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
InsertXML: Inserts new content into a loaded document (XML format).

Use the InsertXML() method to insert new content into an initialised document.

Caution must be exercised when inserting document content.  Inserting an image in-between a set of table rows for
instance, would cause unknown results.  Corruption of the document data may lead to a program crash when the document
is refreshed.

The document view will not be automatically redrawn by this method.  This must be done manually once all modifications
to the document are complete.

-INPUT-
strview XML: An XML string in RIPL format.
int Index: The byte position at which to insert the new content.

-ERRORS-
Okay
NullArgs
NoData
CreateObject
OutOfRange

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_InsertXML(extDocument *Self, doc::InsertXML *Args)
{
   kt::Log log;

   if ((not Args) or (Args->XML.empty())) return log.warning(ERR::NullArgs);
   if ((Args->Index < -1) or (Args->Index > int(Self->Stream.size()))) return log.warning(ERR::OutOfRange);

   if (Self->Stream.data.empty()) return ERR::NoData;

   objXML::create xml = {
      fl::Flags(XMF::INCLUDE_WHITESPACE|XMF::PARSE_HTML|XMF::STRIP_HEADERS),
      fl::Statement(Args->XML)
   };

   if (not xml.ok()) {
      Self->UpdatingLayout = true;
      Self->invalidate_text_width_cache();

      ERR error = insert_xml(Self, &Self->Stream, *xml, xml->Tags, (Args->Index IS -1) ? Self->Stream.size() : Args->Index, STYLE::NIL);
      if (error != ERR::Okay) log.warning("Insert failed for: %.*s", int(Args->XML.size()), Args->XML.data());

      return error;
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-METHOD-
InsertText: Inserts new content into a loaded document (raw text format).

Use the InsertText() method to insert new content into an initialised document.

Caution must be exercised when inserting document content.  Inserting an image in-between a set of table rows for
instance, would cause unknown results.  Corruption of the document data may lead to a program crash when the document
is refreshed.

The document view will not be automatically redrawn by this method.  This must be done manually once all modifications
to the document are complete.

-INPUT-
strview Text: A UTF-8 text string.
int Index: Reference to a `TEXT` control code that will receive the content.  If `-1`, the text will be inserted at the end of the document stream.
int Char: A character offset within the `TEXT` control code that will be injected with content.  If `-1`, the text will be injected at the end of the target string.
int Preformat: If `true`, the text will be treated as pre-formatted (all whitespace, including consecutive whitespace will be recognised).

-ERRORS-
Okay
NullArgs
OutOfRange
Failed

-TAGS-
mutates-object, copies-input
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_InsertText(extDocument *Self, doc::InsertText *Args)
{
   kt::Log log(__FUNCTION__);

   if ((not Args) or (Args->Text.empty())) return log.warning(ERR::NullArgs);
   if ((Args->Index < -1) or (Args->Index > std::ssize(Self->Stream))) return log.warning(ERR::OutOfRange);

   log.traceBranch("Index: %d, Preformat: %d", Args->Index, Args->Preformat);

   Self->UpdatingLayout = true;
   Self->invalidate_text_width_cache();

   INDEX index = Args->Index;
   if (index < 0) index = Self->Stream.size();

   stream_char sc(index, 0);
   ERR error = insert_text(Self, &Self->Stream, sc, Args->Text, Args->Preformat);

   #ifdef DBG_STREAM
      print_stream(Self->Stream);
   #endif

   return error;
}

//********************************************************************************************************************

static ERR DOCUMENT_NewObject(extDocument *Self)
{
   unload_doc(Self);
   return ERR::Okay;
}

//********************************************************************************************************************
// XML-safe element names for every SCODE entry.  Kept parallel to strCodes in document.cpp, but uses lowercase,
// hyphenated tokens that are valid XML identifiers.  Used by the DATA::XML branch of ReadContent().

static constexpr std::array<std::string_view, size_t(SCODE::END)> xmlCodes = {
   "nil", "text", "font", "font-end", "link", "tab-def", "p-end", "p", "link-end", "advance", "list", "list-end",
   "table", "table-end", "row", "cell", "row-end", "index", "index-end", "xml", "image", "use", "button", "checkbox",
   "combobox", "input"
};

static_assert(xmlCodes.size() IS size_t(SCODE::END),
   "xmlCodes must contain one entry per SCODE value");

//********************************************************************************************************************
// Append Text to Out with XML escaping applied to the characters that are unsafe in element content or
// double-quoted attribute values.  The document module cannot share the xml module's private escape table without
// breaking module layering, so a small local helper is used instead.

static void append_xml_escaped(std::ostringstream &Out, std::string_view Text)
{
   size_t last_pos = 0;
   for (size_t i = 0; i < Text.size(); i++) {
      std::string_view escape;
      switch (Text[i]) {
         case '&':  escape = "&amp;"; break;
         case '<':  escape = "&lt;"; break;
         case '>':  escape = "&gt;"; break;
         case '"':  escape = "&quot;"; break;
         default: continue;
      }

      if (i > last_pos) Out.write(Text.data() + last_pos, i - last_pos);
      Out << escape;
      last_pos = i + 1;
   }

   if (last_pos < Text.size()) Out.write(Text.data() + last_pos, Text.size() - last_pos);
}

//********************************************************************************************************************
// Attribute emitters for the DATA::XML serialisation.  Each overload writes ` name="value"` to Out; string values are
// XML-escaped on the way through.  Overloads are kept tiny to keep the per-code switch in write_stream_xml tidy.

static inline void emit_attr(std::ostringstream &Out, std::string_view Name, std::string_view Value)
{
   Out << ' ' << Name << "=\"";
   append_xml_escaped(Out, Value);
   Out << '"';
}

static inline void emit_attr(std::ostringstream &Out, std::string_view Name, int Value)
{
   Out << ' ' << Name << "=\"" << Value << '"';
}

static inline void emit_attr(std::ostringstream &Out, std::string_view Name, double Value)
{
   Out << ' ' << Name << "=\"" << Value << '"';
}

static inline void emit_attr(std::ostringstream &Out, std::string_view Name, bool Value)
{
   Out << ' ' << Name << "=\"" << (Value ? "true" : "false") << '"';
}

//********************************************************************************************************************
// Enum-to-string helpers for the attribute emitters.

static constexpr std::string_view du_to_sv(DU Type)
{
   switch (Type) {
      case DU::PIXEL:            return "px";
      case DU::SCALED:           return "%";
      case DU::FONT_SIZE:        return "em";
      case DU::CHAR:             return "ch";
      case DU::LINE_HEIGHT:      return "lh";
      case DU::TRUE_LINE_HEIGHT: return "true-lh";
      case DU::ROOT_FONT_SIZE:   return "rem";
      case DU::ROOT_LINE_HEIGHT: return "rlh";
      case DU::VP_WIDTH:         return "vw";
      case DU::VP_HEIGHT:        return "vh";
      case DU::VP_MIN:           return "vmin";
      case DU::VP_MAX:           return "vmax";
      default:                   return "nil";
   }
}

static constexpr std::string_view align_to_sv(ALIGN Value)
{
   if ((Value & ALIGN::LEFT) != ALIGN::NIL) return "left";
   if ((Value & ALIGN::RIGHT) != ALIGN::NIL) return "right";
   if ((Value & ALIGN::HORIZONTAL) != ALIGN::NIL) return "center";
   if ((Value & ALIGN::TOP) != ALIGN::NIL) return "top";
   if ((Value & ALIGN::BOTTOM) != ALIGN::NIL) return "bottom";
   if ((Value & ALIGN::VERTICAL) != ALIGN::NIL) return "middle";
   return {};
}

static constexpr std::string_view valign_to_sv(ALIGN Value)
{
   if ((Value & ALIGN::TOP) != ALIGN::NIL) return "top";
   if ((Value & ALIGN::BOTTOM) != ALIGN::NIL) return "bottom";
   if ((Value & ALIGN::VERTICAL) != ALIGN::NIL) return "middle";
   return {};
}

static constexpr std::string_view link_type_to_sv(LINK Type)
{
   switch (Type) {
      case LINK::HREF:     return "href";
      case LINK::FUNCTION: return "function";
      default:             return "nil";
   }
}

static constexpr std::string_view list_type_to_sv(uint8_t Type)
{
   switch (Type) {
      case bc_list::ORDERED: return "ordered";
      case bc_list::CUSTOM:  return "custom";
      default:               return "bullet";
   }
}

static constexpr std::string_view fso_align_to_sv(FSO Options)
{
   if ((Options & FSO::ALIGN_CENTER) != FSO::NIL) return "center";
   if ((Options & FSO::ALIGN_RIGHT) != FSO::NIL) return "right";
   return {};
}

static std::string border_to_string(CB Border)
{
   if (Border IS CB::NIL) return {};
   if (Border IS CB::ALL) return "all";

   std::string out;
   if ((Border & CB::TOP) != CB::NIL) out += "top,";
   if ((Border & CB::BOTTOM) != CB::NIL) out += "bottom,";
   if ((Border & CB::LEFT) != CB::NIL) out += "left,";
   if ((Border & CB::RIGHT) != CB::NIL) out += "right,";
   if (not out.empty()) out.pop_back();
   return out;
}

//********************************************************************************************************************
// Compound attribute emitters for layout-related data structures.

static void emit_dunit_attrs(std::ostringstream &Out, std::string_view Name, const DUNIT &Value)
{
   if (Value.type IS DU::NIL) return;

   double value = Value.value;
   if ((value > 0) and (value <= std::numeric_limits<double>::min())) value = 0;

   std::string unit_name(Name);
   emit_attr(Out, unit_name, value);
   unit_name += "-unit";
   emit_attr(Out, unit_name, du_to_sv(Value.type));
}

static void emit_padding_attrs(std::ostringstream &Out, std::string_view Prefix, const padding &Padding)
{
   if (not Padding.configured) return;

   std::string base(Prefix);
   emit_attr(Out, base + "-left", Padding.left);
   emit_attr(Out, base + "-top", Padding.top);
   emit_attr(Out, base + "-right", Padding.right);
   emit_attr(Out, base + "-bottom", Padding.bottom);
   if (Padding.left_scl) emit_attr(Out, base + "-left-scaled", true);
   if (Padding.top_scl) emit_attr(Out, base + "-top-scaled", true);
   if (Padding.right_scl) emit_attr(Out, base + "-right-scaled", true);
   if (Padding.bottom_scl) emit_attr(Out, base + "-bottom-scaled", true);
}

static void emit_stream_identity(std::ostringstream &Out, INDEX Index)
{
   emit_attr(Out, "stream-index", int(Index));
}

static void emit_nested_stream_identity(std::ostringstream &Out, INDEX ParentIndex, INDEX Index)
{
   emit_attr(Out, "parent-stream-index", int(ParentIndex));
   emit_attr(Out, "nested-stream-index", int(Index));
}

static std::string segment_codes_to_string(const doc_segment &Segment)
{
   std::ostringstream out;

   for (auto i = Segment.start; i < Segment.stop; i.next_code()) {
      auto code = Segment.stream[0][i.index].code;
      if (code IS SCODE::FONT) {
         auto &style = Segment.stream->lookup<bc_font>(i.index);
         out << "[E:Font:#" << style.index() << ']';
      }
      else if (code IS SCODE::PARAGRAPH_START) {
         auto &para = Segment.stream->lookup<bc_paragraph>(i.index);
         if (para.list_item) out << "[E:LI]";
         else out << "[E:PS]";
      }
      else if (code IS SCODE::PARAGRAPH_END) out << "[E:PE]";
      else out << "[E:" << strCodes[int(code)] << ']';
   }

   return out.str();
}

static bool segment_in_range(const doc_segment &Segment, INDEX Start, INDEX End)
{
   stream_char range_start(Start, 0), range_end(End, 0);
   return (Segment.stop > range_start) and (Segment.start < range_end);
}

static void write_segments_xml(const std::vector<doc_segment> &Segments, INDEX Start, INDEX End,
   std::ostringstream &Out, INDEX ParentIndex = -1)
{
   bool has_segments = false;

   for (SEGINDEX si = 0; si < SEGINDEX(Segments.size()); si++) {
      auto &segment = Segments[si];
      if (not segment_in_range(segment, Start, End)) continue;

      if (not has_segments) {
         Out << "\n<segments>\n";
         has_segments = true;
      }

      Out << "<segment";
      if (ParentIndex >= 0) emit_attr(Out, "parent-stream-index", int(ParentIndex));
      emit_attr(Out, "index", int(si));
      emit_attr(Out, "start-index", int(segment.start.index));
      emit_attr(Out, "start-offset", int(segment.start.offset));
      emit_attr(Out, "stop-index", int(segment.stop.index));
      emit_attr(Out, "stop-offset", int(segment.stop.offset));
      emit_attr(Out, "trim-stop-index", int(segment.trim_stop.index));
      emit_attr(Out, "trim-stop-offset", int(segment.trim_stop.offset));
      emit_attr(Out, "x", segment.area.X);
      emit_attr(Out, "y", segment.area.Y);
      emit_attr(Out, "width", segment.area.Width);
      emit_attr(Out, "height", segment.area.Height);
      emit_attr(Out, "descent", segment.descent);
      emit_attr(Out, "align-width", segment.align_width);
      emit_attr(Out, "edit", segment.edit);
      emit_attr(Out, "allow-merge", segment.allow_merge);
      emit_attr(Out, "codes", segment_codes_to_string(segment));
      Out << "/>\n";
   }

   if (has_segments) Out << "</segments>\n";
}

//********************************************************************************************************************
static void write_layout_metrics_xml(extDocument *Self, std::ostringstream &Out)
{
   if (!Self) return;

   auto &metrics = Self->LayoutMetrics;

   Out << "<layout-metrics";
   emit_attr(Out, "root-passes", int(metrics.root_passes));
   emit_attr(Out, "do-layout-calls", int(metrics.do_layout_calls));
   emit_attr(Out, "page-extend-restarts", int(metrics.page_extend_restarts));
   emit_attr(Out, "page-extend-requests", int(metrics.page_extend_requests));
   emit_attr(Out, "vertical-repasses", int(metrics.vertical_repasses));
   emit_attr(Out, "table-wrap-restarts", int(metrics.table_wrap_restarts));
   emit_attr(Out, "row-repasses", int(metrics.row_repasses));
   emit_attr(Out, "cell-layouts", int(metrics.cell_layouts));
   emit_attr(Out, "check-wordwrap-calls", int(metrics.check_wordwrap_calls));
   emit_attr(Out, "wrap-through-clips-calls", int(metrics.wrap_through_clips_calls));
   emit_attr(Out, "new-segment-calls", int(metrics.new_segment_calls));
   emit_attr(Out, "segment-count-peak", int(metrics.segment_count_peak));
   emit_attr(Out, "clip-count-peak", int(metrics.clip_count_peak));
   Out << "/>\n";
}

//********************************************************************************************************************
// Emit the shared font cache after the stream fragment so inspection tools can correlate requested font runs with the
// resolved cached fonts and metrics actually used by layout.

static void write_fonts_xml(std::ostringstream &Out)
{
   std::deque<font_entry> fonts;

   {
      std::lock_guard lk(glFontsMutex);
      fonts = glFonts;
   }

   Out << "<fonts>\n";
   for (size_t i = 0; i < fonts.size(); i++) {
      auto &font = fonts[i];

      Out << "<font";
      emit_attr(Out, "cache-index", int(i));
      if (not font.face.empty())  emit_attr(Out, "face", font.face);
      if (not font.style.empty()) emit_attr(Out, "style", font.style);
      if (font.font_size > 0)  emit_attr(Out, "pixel-size", font.font_size);
      if (font.align != ALIGN::NIL) emit_attr(Out, "align", align_to_sv(font.align));
      emit_attr(Out, "height", font.metrics.Height);
      emit_attr(Out, "line-spacing", font.metrics.LineSpacing);
      emit_attr(Out, "ascent", font.metrics.Ascent);
      emit_attr(Out, "descent", font.metrics.Descent);
      emit_attr(Out, "baseline-descent", font.descent());
      Out << "/>\n";
   }
   Out << "</fonts>\n";
}

//********************************************************************************************************************
// Emit attributes for widget_mgr-derived bytecodes (bc_image, bc_button, bc_checkbox, bc_combobox, bc_input).

static void emit_widget_attrs(std::ostringstream &Out, const widget_mgr &Widget)
{
   if (not Widget.name.empty())  emit_attr(Out, "name", Widget.name);
   if (not Widget.label.empty()) emit_attr(Out, "label", Widget.label);
   if (not Widget.fill.empty())  emit_attr(Out, "fill", Widget.fill);
   if (not Widget.alt_fill.empty()) emit_attr(Out, "alt-fill", Widget.alt_fill);
   if (not Widget.font_fill.empty()) emit_attr(Out, "font-fill", Widget.font_fill);
   emit_dunit_attrs(Out, "width", Widget.width);
   emit_dunit_attrs(Out, "height", Widget.height);
   emit_dunit_attrs(Out, "default-size", Widget.def_size);
   emit_dunit_attrs(Out, "label-pad", Widget.label_pad);
   emit_padding_attrs(Out, "padding", Widget.pad);
   if (auto align = align_to_sv(Widget.align); not align.empty()) emit_attr(Out, "align", align);
   if (auto valign = valign_to_sv(Widget.align); not valign.empty()) emit_attr(Out, "v-align", valign);
   if (Widget.align_to_text) emit_attr(Out, "align-to-text", true);
   if (Widget.alt_state) emit_attr(Out, "alt-state", true);
   if (Widget.internal_page) emit_attr(Out, "internal-page", true);
   if (Widget.label_pos != 1) emit_attr(Out, "label-pos", int(Widget.label_pos));
}

//********************************************************************************************************************
// Walk a stream range and emit one XML element per byte code to Out.  SCODE::TEXT codes are emitted as <text>...</text>
// with escaped content; SCODE::CELL codes recurse into their nested stream; all other codes are emitted as
// self-closing elements with selected attributes from the backing bc_* struct.  HasText is set to true if at
// least one SCODE::TEXT code was encountered, so the caller can decide whether to return ERR::NoData.

static void write_stream_xml(
   RSTREAM &Stream,
   INDEX Start,
   INDEX End,
   std::ostringstream &Out,
   bool &HasContent,
   bool ExpandNested,
   INDEX ParentIndex = -1)
{
   for (INDEX i = Start; i < End; i++) {
      auto code = Stream[i].code;
      int idx = int(code);
      std::string_view name = ((idx >= 0) and (idx < int(SCODE::END))) ? xmlCodes[idx] : std::string_view("unknown");
      HasContent = true;

      auto emit_identity = [&Out, ParentIndex, i]() {
         if (ParentIndex >= 0) emit_nested_stream_identity(Out, ParentIndex, i);
         else emit_stream_identity(Out, i);
      };

      switch (code) {
         case SCODE::TEXT: {
            auto &text = Stream.lookup<bc_text>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "length", int(text.text.size()));
            if (text.formatted) emit_attr(Out, "formatted", true);
            if (text.segment >= 0) emit_attr(Out, "segment", int(text.segment));
            Out << '>';
            append_xml_escaped(Out, text.text);
            Out << "</" << name << '>';
            break;
         }

         case SCODE::FONT: {
            auto &font = Stream.lookup<bc_font>(i);
            Out << '<' << name;
            emit_identity();
            if (font.index() >= 0) emit_attr(Out, "index", int(font.index()));
            if (not font.face.empty())  emit_attr(Out, "face", font.face);
            if (not font.style.empty()) emit_attr(Out, "style", font.style);
            if (not font.fill.empty())  emit_attr(Out, "fill", font.fill);
            emit_dunit_attrs(Out, "size", font.req_size);
            if (font.pixel_size > 0) emit_attr(Out, "pixel-size", font.pixel_size);
            auto align = fso_align_to_sv(font.options);
            if (not align.empty()) emit_attr(Out, "align", align);
            Out << "/>";
            break;
         }

         case SCODE::LINK: {
            auto &link = Stream.lookup<bc_link>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "type", link_type_to_sv(link.type));
            if (not link.ref.empty())  emit_attr(Out, "ref", link.ref);
            if (not link.hint.empty()) emit_attr(Out, "hint", link.hint);
            if (not link.fill.empty()) emit_attr(Out, "fill", link.fill);
            if (not link.hooks.on_click.empty()) emit_attr(Out, "on-click", link.hooks.on_click);
            if (not link.hooks.on_motion.empty()) emit_attr(Out, "on-motion", link.hooks.on_motion);
            if (not link.hooks.on_crossing.empty()) emit_attr(Out, "on-crossing", link.hooks.on_crossing);
            Out << "/>";
            break;
         }

         case SCODE::PARAGRAPH_START: {
            auto &para = Stream.lookup<bc_paragraph>(i);
            Out << '<' << name;
            emit_identity();
            if (para.list_item)            emit_attr(Out, "list-item", true);
            if (not para.font.face.empty())  emit_attr(Out, "face", para.font.face);
            if (not para.font.style.empty()) emit_attr(Out, "style", para.font.style);
            if (not para.font.fill.empty())  emit_attr(Out, "fill", para.font.fill);
            emit_dunit_attrs(Out, "size", para.font.req_size);
            if (auto align = fso_align_to_sv(para.font.options); not align.empty()) emit_attr(Out, "align", align);
            if (auto valign = valign_to_sv(para.font.valign); not valign.empty()) emit_attr(Out, "v-align", valign);
            emit_dunit_attrs(Out, "block-indent", para.block_indent);
            emit_dunit_attrs(Out, "item-indent", para.item_indent);
            emit_dunit_attrs(Out, "indent", para.indent);
            emit_dunit_attrs(Out, "line-height", para.line_height);
            emit_dunit_attrs(Out, "leading", para.leading);
            if (para.trim)      emit_attr(Out, "trim", true);
            if (para.aggregate) emit_attr(Out, "aggregate", true);
            Out << "/>";
            break;
         }

         case SCODE::ADVANCE: {
            auto &adv = Stream.lookup<bc_advance>(i);
            Out << '<' << name;
            emit_identity();
            emit_dunit_attrs(Out, "x", adv.x);
            emit_dunit_attrs(Out, "y", adv.y);
            Out << "/>";
            break;
         }

         case SCODE::LIST_START: {
            auto &list = Stream.lookup<bc_list>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "type", list_type_to_sv(list.type));
            if (list.start != 1)     emit_attr(Out, "start", list.start);
            if (not list.fill.empty())  emit_attr(Out, "fill", list.fill);
            emit_dunit_attrs(Out, "item-indent", list.item_indent);
            emit_dunit_attrs(Out, "block-indent", list.block_indent);
            emit_dunit_attrs(Out, "v-spacing", list.v_spacing);
            Out << "/>";
            break;
         }

         case SCODE::TABLE_START: {
            auto &table = Stream.lookup<bc_table>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "columns", int(table.columns.size()));
            if (table.rows > 0)        emit_attr(Out, "rows", table.rows);
            if (not table.fill.empty())   emit_attr(Out, "fill", table.fill);
            if (not table.stroke.empty()) emit_attr(Out, "stroke", table.stroke);
            emit_dunit_attrs(Out, "stroke-width", table.stroke_width);
            emit_dunit_attrs(Out, "min-width", table.min_width);
            emit_dunit_attrs(Out, "min-height", table.min_height);
            emit_dunit_attrs(Out, "cell-h-spacing", table.cell_h_spacing);
            emit_dunit_attrs(Out, "cell-v-spacing", table.cell_v_spacing);
            emit_padding_attrs(Out, "cell-padding", table.cell_padding);
            if (table.align != ALIGN::NIL) emit_attr(Out, "align", align_to_sv(table.align));
            if (table.collapsed) emit_attr(Out, "collapsed", true);
            if (table.wrap)      emit_attr(Out, "wrap", true);
            Out << "/>";
            break;
         }

         case SCODE::ROW: {
            auto &row = Stream.lookup<bc_row>(i);
            Out << '<' << name;
            emit_identity();
            if (not row.fill.empty())   emit_attr(Out, "fill", row.fill);
            if (not row.stroke.empty()) emit_attr(Out, "stroke", row.stroke);
            if (row.min_height != 0) emit_attr(Out, "min-height", row.min_height);
            if (row.vertical_repass) emit_attr(Out, "vertical-repass", true);
            Out << "/>";
            break;
         }

         case SCODE::CELL: {
            auto &cell = Stream.lookup<bc_cell>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "cell-id", int(cell.cell_id));
            emit_attr(Out, "column", cell.column);
            if (cell.col_span != 1) emit_attr(Out, "col-span", cell.col_span);
            if (cell.row_span != 1) emit_attr(Out, "row-span", cell.row_span);
            if (not cell.edit_def.empty()) emit_attr(Out, "edit-def", cell.edit_def);
            if (not cell.fill.empty())     emit_attr(Out, "fill", cell.fill);
            if (not cell.stroke.empty())   emit_attr(Out, "stroke", cell.stroke);
            emit_dunit_attrs(Out, "stroke-width", cell.stroke_width);
            auto border = border_to_string(cell.border);
            if (not border.empty()) emit_attr(Out, "border", border);
            if (cell.modified) emit_attr(Out, "modified", true);
            if (not cell.hooks.on_click.empty()) emit_attr(Out, "on-click", cell.hooks.on_click);
            if (not cell.hooks.on_motion.empty()) emit_attr(Out, "on-motion", cell.hooks.on_motion);
            if (not cell.hooks.on_crossing.empty()) emit_attr(Out, "on-crossing", cell.hooks.on_crossing);

            if (ExpandNested and (cell.stream) and (not cell.stream->data.empty())) {
               Out << '>';
               write_stream_xml(*cell.stream, 0, INDEX(cell.stream->size()), Out, HasContent, false, i);
               write_segments_xml(cell.segments, 0, INDEX(cell.stream->size()), Out, i);
               Out << "</" << name << ">\n";
            }
            else Out << "/>";
            break;
         }

         case SCODE::INDEX_START: {
            auto &index = Stream.lookup<bc_index>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "name-hash", int(index.name_hash));
            emit_attr(Out, "id", index.id);
            emit_attr(Out, "visible", index.visible);
            emit_attr(Out, "parent-visible", index.parent_visible);
            Out << "/>";
            break;
         }

         case SCODE::INDEX_END: {
            auto &index_end = Stream.lookup<bc_index_end>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "id", index_end.id);
            Out << "/>";
            break;
         }

         case SCODE::XML: {
            auto &xml = Stream.lookup<bc_xml>(i);
            Out << '<' << name;
            emit_identity();
            emit_attr(Out, "object-id", int(xml.object_id));
            if (xml.owned) emit_attr(Out, "owned", true);
            Out << "/>";
            break;
         }

         case SCODE::IMAGE: {
            auto &image = Stream.lookup<bc_image>(i);
            Out << '<' << name;
            emit_identity();
            emit_widget_attrs(Out, image);
            Out << "/>";
            break;
         }

         case SCODE::USE: {
            auto &use = Stream.lookup<bc_use>(i);
            Out << '<' << name;
            emit_identity();
            if (not use.id.empty()) emit_attr(Out, "id", use.id);
            if (use.processed) emit_attr(Out, "processed", true);
            Out << "/>";
            break;
         }

         case SCODE::BUTTON: {
            auto &button = Stream.lookup<bc_button>(i);
            Out << '<' << name;
            emit_identity();
            emit_widget_attrs(Out, button);
            emit_padding_attrs(Out, "inner-padding", button.inner_padding);
            if (ExpandNested and (button.stream) and (not button.stream->data.empty())) {
               Out << '>';
               write_stream_xml(*button.stream, 0, INDEX(button.stream->size()), Out, HasContent, false, i);
               write_segments_xml(button.segments, 0, INDEX(button.stream->size()), Out, i);
               Out << "</" << name << '>';
            }
            else Out << "/>";
            break;
         }

         case SCODE::CHECKBOX: {
            auto &checkbox = Stream.lookup<bc_checkbox>(i);
            Out << '<' << name;
            emit_identity();
            emit_widget_attrs(Out, checkbox);
            if (checkbox.processed) emit_attr(Out, "processed", true);
            Out << "/>";
            break;
         }

         case SCODE::COMBOBOX: {
            auto &combobox = Stream.lookup<bc_combobox>(i);
            Out << '<' << name;
            emit_identity();
            emit_widget_attrs(Out, combobox);
            if (not combobox.style.empty()) emit_attr(Out, "style", combobox.style);
            if (not combobox.value.empty()) emit_attr(Out, "value", combobox.value);
            Out << "/>";
            break;
         }

         case SCODE::INPUT: {
            auto &input = Stream.lookup<bc_input>(i);
            Out << '<' << name;
            emit_identity();
            emit_widget_attrs(Out, input);
            if (not input.value.empty()) emit_attr(Out, "value", input.value);
            if (input.secret) emit_attr(Out, "secret", true);
            Out << "/>";
            break;
         }

         default:
            Out << '<' << name;
            emit_identity();
            Out << "/>";
            break;
      }
   }
}

/*********************************************************************************************************************

-METHOD-
ReadContent: Returns selected content from the document, either as plain text, original byte code or XML.

The ReadContent() method extracts content from the document stream, covering a specific area.  It can return the data
as a RIPL binary stream, translated into plain-text (control codes are removed), or serialised as an XML document
describing the byte stream.

The XML format is a linear, non-nested serialisation of the byte stream intended for inspection, diffing and tooling.
Each byte code becomes one XML element wrapped in a `&lt;extract&gt;` root, followed by the requested content in a `&lt;stream&gt;`
element, and text content appears inside `&lt;text&gt;` elements with the usual XML escaping applied.  Start/end markers
such as paragraphs and font runs are emitted as sibling self-closing elements rather than nested, reflecting the
underlying linear storage model.  A trailing `&lt;fonts&gt;` section lists information for the shared cached fonts.

No post-processing is performed to fix validity errors that may arise from an invalid data range.  For instance, if
an opening paragraph code is not closed with a matching paragraph end point, this will remain the case in the
resulting data.

-INPUT-
int(DATA) Format: Set to `TEXT` to receive plain-text, `RAW` to receive the original byte-code, or `XML` to receive a textual XML serialisation of the stream.
int Start:  An index in the document stream from which data will be extracted.
int End:    An index in the document stream at which extraction will stop.
!str Result: The data is returned in this parameter as an allocated string.

-ERRORS-
Okay
NullArgs
OutOfRange: The Start index is not within the stream.
Args
NoData: Operation successful, but no data was present for extraction.

-TAGS-
pure-query, caller-owns-result, null-terminated-result

*********************************************************************************************************************/

static ERR DOCUMENT_ReadContent(extDocument *Self, doc::ReadContent *Args)
{
   kt::Log log(__FUNCTION__);

   if (not Args) return log.warning(ERR::NullArgs);

   Args->Result = nullptr;

   if ((Args->Start < 0) or (Args->Start >= std::ssize(Self->Stream))) return log.warning(ERR::OutOfRange);
   if (Args->End <= Args->Start) return log.warning(ERR::Args);

   // End is a soft upper bound - callers may pass a large sentinel value to mean "to end of stream".

   INDEX end = Args->End;
   if (end > INDEX(std::ssize(Self->Stream))) end = INDEX(std::ssize(Self->Stream));

   if (Args->Format IS DATA::TEXT) {
      std::ostringstream buffer;

      for (INDEX i=Args->Start; i < end; i++) {
         if (Self->Stream[i].code IS SCODE::TEXT) {
            buffer << Self->Stream.lookup<bc_text>(i).text;
         }
      }

      auto str = buffer.str();
      if (str.empty()) return ERR::NoData;
      if ((Args->Result = strclone(str))) return ERR::Okay;
      else return log.warning(ERR::AllocMemory);
   }
   else if (Args->Format IS DATA::RAW) {
      STRING output;
      auto size = (end - Args->Start) * INDEX(sizeof(stream_code));
      if (!AllocMemory(size + 1, MEM::NO_CLEAR, (APTR *)&output)) {
         copymem(Self->Stream.data.data() + Args->Start, output, size);
         output[size] = 0;
         Args->Result = output;
         return ERR::Okay;
      }
      else return log.warning(ERR::AllocMemory);
   }
   else if (Args->Format IS DATA::XML) {
      std::ostringstream buffer;
      bool has_content = false;
      bool expand_nested = (Args->Start IS 0) and (end IS INDEX(std::ssize(Self->Stream)));

      buffer << "<extract>\n<stream>\n";
      write_stream_xml(Self->Stream, Args->Start, end, buffer, has_content, expand_nested);
      write_segments_xml(Self->Segments, Args->Start, end, buffer);
      write_layout_metrics_xml(Self, buffer);
      buffer << "</stream>\n";
      write_fonts_xml(buffer);
      buffer << "</extract>\n";

      if (not has_content) return ERR::NoData;

      auto str = buffer.str();
      if ((Args->Result = strclone(str))) return ERR::Okay;
      else return log.warning(ERR::AllocMemory);
   }
   else return log.warning(ERR::Args);
}

/*********************************************************************************************************************
-ACTION-
Refresh: Reloads the document data from the original source location.
-END-
*********************************************************************************************************************/

static ERR DOCUMENT_Refresh(extDocument *Self)
{
   kt::Log log;

   if (Self->Processing) {
      log.msg("Recursion detected - refresh will be delayed.");
      QueueAction(AC::Refresh, Self->UID);
      return ERR::Okay;
   }

   Self->Processing++;

   for (auto &trigger : copy_triggers(Self, DRT::REFRESH)) {
      if (trigger.isScript()) {
         // The refresh trigger can return ERR::Skip to prevent a complete reload of the document.

         ERR error;
         if (!sc::Call(trigger, error)) {
            if (error IS ERR::Skip) {
               log.msg("The refresh request has been handled by an event trigger.");
               Self->Processing--;
               return ERR::Okay;
            }
         }
      }
      else if (trigger.isC()) {
         auto routine = (void (*)(APTR, extDocument *))trigger.Routine;
         kt::SwitchContext context(trigger.Context);
         routine(trigger.Context, Self);
      }
   }

   ERR error = ERR::Okay;
   if ((not Self->Path.empty()) and (Self->Path[0] != '#') and (Self->Path[0] != '?')) {
      log.branch("Refreshing from path '%s'", Self->Path.c_str());
      error = load_doc(Self, Self->Path, true, ULD::REFRESH);
   }
   else log.msg("No source Path defined in the document.");

   Self->Processing--;

   return error;
}

/*********************************************************************************************************************

-METHOD-
RemoveContent: Removes content from a loaded document.

This method will remove all document content between the `Start` and `End` indexes provided as parameters.  The document
layout will also be marked for an update for the next redraw.

-INPUT-
int Start: The byte position at which to start the removal.
int End: The byte position at which the removal ends.

-ERRORS-
Okay
NullArgs
OutOfRange: The area to be removed is outside the bounds of the document's data stream.
Args

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR DOCUMENT_RemoveContent(extDocument *Self, doc::RemoveContent *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if ((Args->Start < 0) or (Args->Start >= std::ssize(Self->Stream))) return log.warning(ERR::OutOfRange);
   if (Args->End < 0) return log.warning(ERR::OutOfRange);
   if (Args->End <= Args->Start) return log.warning(ERR::Args);

   // End is a soft upper bound - callers may pass a large sentinel value to mean "to end of stream".

   INDEX end = Args->End;
   if (end > INDEX(std::ssize(Self->Stream))) end = INDEX(std::ssize(Self->Stream));

   copymem(Self->Stream.data.data() + end, Self->Stream.data.data() + Args->Start, Self->Stream.data.size() - end);
   Self->Stream.data.resize(Self->Stream.data.size() - (end - Args->Start));

   Self->invalidate_text_width_cache();
   Self->UpdatingLayout = true;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveListener: Removes a previously configured listener from the document.

This method removes a previously configured listener from the document.  The original parameters that were passed to
#AddListener() must be provided.

-INPUT-
int Trigger: The unique identifier for the trigger.
ptr(func) Function: The function that is called when the trigger activates.

-ERRORS-
Okay
NullArgs

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR DOCUMENT_RemoveListener(extDocument *Self, doc::RemoveListener *Args)
{
   if ((not Args) or (not Args->Trigger) or (not Args->Function)) return ERR::NullArgs;
   if (not valid_trigger(Args->Trigger)) return ERR::OutOfRange;

   if (Self->Unloading) return ERR::Okay; // Do nothing, termination will take care of cleaning up.

   if (Args->Function->isC()) {
      for (auto it = Self->Triggers[Args->Trigger].begin(); it != Self->Triggers[Args->Trigger].end(); it++) {
         if ((it->isC()) and (it->Routine IS Args->Function->Routine)) {
            Self->Triggers[Args->Trigger].erase(it);
            return ERR::Okay;
         }
      }
   }
   else if (Args->Function->isScript()) {
      auto context = Args->Function->Context;
      bool removed_listener = false;

      for (auto it = Self->Triggers[Args->Trigger].begin(); it != Self->Triggers[Args->Trigger].end(); it++) {
         if ((it->isScript()) and
               (it->Context IS Args->Function->Context) and
               (it->ProcedureID IS Args->Function->ProcedureID)) {
            Self->Triggers[Args->Trigger].erase(it);
            removed_listener = true;
            break;
         }
      }

      if (removed_listener) unsubscribe_script_context(Self, context);
      return ERR::Okay;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Use this action to save edited information as an XML document file.

*********************************************************************************************************************/

static ERR DOCUMENT_SaveToObject(extDocument *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);
   if (not Args->Dest) return log.warning(ERR::NullArgs);

   log.branch("Destination: %d", Args->Dest->UID);

   doc::ReadContent read = { DATA::XML, 0, int(Self->Stream.size()), nullptr };
   if (auto error = DOCUMENT_ReadContent(Self, &read); !error) {
      error = acWrite(Args->Dest, read.Result, strlen(read.Result), nullptr);
      FreeResource(read.Result);
      if (!error) return ERR::Okay;
      else return log.warning(ERR::Write);
   }
   else return error;
}

/*********************************************************************************************************************

-METHOD-
SelectLink: Selects links in the document.

This method will select a link in the document.  Selecting a link will mean that the link in question will take on a
different appearance (e.g. if a text link, the text will change colour).  If the user presses the enter key when a
hyperlink is selected, that link will be activated.

Selecting a link may also enable drag and drop functionality for that link.

Links are referenced either by their `Index` in the links array, or by name for links that have named references.  It
should be noted that objects that can receive the focus - such as input boxes and buttons - are also treated as
selectable links due to the nature of their functionality.

-INPUT-
int Index: Index to a link (links are in the order in which they are created in the document, zero being the first link).  Ignored if the `Name` parameter is set.
strview Name: The name of the link to select.  Leave empty if an `Index` is defined.

-ERRORS-
Okay
NullArgs
OutOfRange

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_SelectLink(extDocument *Self, doc::SelectLink *Args)
{
   kt::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   if (not Args->Name.empty()) {
/*
      LONG i;
      for (i=0; i < Self->Tabs.size(); i++) {
         if (Self->Tabs[i].Type IS TT::OBJECT) {
            name = GetObjectName(?)
            if (iequals(args->name, name)) {

            }
         }
         else if (Self->Tabs[i].Type IS TT::LINK) {

         }
      }
*/

      return log.warning(ERR::NoSupport);
   }
   else if ((Args->Index >= 0) and (Args->Index < std::ssize(Self->Tabs))) {
      Self->FocusIndex = Args->Index;
      set_focus(Self, Args->Index, "SelectLink");
      return ERR::Okay;
   }
   else return log.warning(ERR::OutOfRange);
}

/*********************************************************************************************************************
-ACTION-
SetKey: Set a global key-value in the document.
-END-
*********************************************************************************************************************/

static ERR DOCUMENT_SetKey(extDocument *Self, struct acSetKey *Args)
{
   // Note: Zero-length parameter values are permitted.

   if ((not Args) or (Args->Key.empty())) return ERR::NullArgs;

   auto key_it = Self->Vars.lower_bound(Args->Key);
   if ((key_it != Self->Vars.end()) and (not Self->Vars.key_comp()(Args->Key, key_it->first))) {
      key_it->second.assign(Args->Value);
   }
   else Self->Vars.emplace_hint(key_it, std::string(Args->Key), std::string(Args->Value));

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ShowIndex: Shows the content held within a named index.

The #HideIndex() and ShowIndex() methods allow the display of document content to be controlled at code level.  To control
content visibility, start by encapsulating the content in the source document with an `&lt;index&gt;` tag and ensure that
it is named.  Then make calls to #HideIndex() and ShowIndex() with the index name to manipulate visibility.

The document layout is automatically updated and pushed to the display when this method is called.

-INPUT-
strview Name: The name of the index.

-ERRORS-
Okay
NullArgs
Search: The index could not be found.

-TAGS-
mutates-object
-END-

*********************************************************************************************************************/

static ERR DOCUMENT_ShowIndex(extDocument *Self, doc::ShowIndex *Args)
{
   kt::Log log;

   if ((not Args) or (Args->Name.empty())) return log.warning(ERR::NullArgs);

   log.branch("Index: %.*s", int(Args->Name.size()), Args->Name.data());

   auto &stream = Self->Stream;
   auto name_hash = strihash(Args->Name);
   for (INDEX i=0; i < INDEX(stream.size()); i++) {
      if (stream[i].code IS SCODE::INDEX_START) {
         auto &index = Self->Stream.lookup<bc_index>(i);
         if (name_hash != index.name_hash) continue;
         if (index.visible) return ERR::Okay; // It's already visible!

         index.visible = true;
         if (index.parent_visible) { // We are visible, but parents must also be visible to show content
            // Show all objects and manage the ParentVisible status of any child indexes

            {
               #ifndef RETAIN_LOG_LEVEL
               kt::LogLevel level(2);
               #endif
               Self->UpdatingLayout = true;
               layout_doc(Self);
            }

            for (++i; i < INDEX(stream.size()); i++) {
               auto code = stream[i].code;
               if (code IS SCODE::INDEX_END) {
                  if (index.id IS Self->Stream.lookup<bc_index_end>(i).id) break;
               }
               else if (code IS SCODE::IMAGE) {
                  auto &img = Self->Stream.lookup<bc_image>(i);
                  if (not img.rect.empty()) acShow(*img.rect);

                  if (auto tab = find_tabfocus(Self, TT::VECTOR, img.rect->UID); tab >= 0) {
                     Self->Tabs[tab].active = true;
                  }
               }
               else if (code IS SCODE::LINK) {
                  if (auto tab = find_tabfocus(Self, TT::LINK, Self->Stream.lookup<bc_link>(i).uid); tab >= 0) {
                     Self->Tabs[tab].active = true;
                  }
               }
               else if (code IS SCODE::INDEX_START) {
                  auto &index = Self->Stream.lookup<bc_index>(i);
                  index.parent_visible = true;

                  if (not index.visible) {
                     for (++i; i < INDEX(stream.size()); i++) {
                        if (stream[i].code IS SCODE::INDEX_END) {
                           if (index.id IS Self->Stream.lookup<bc_index_end>(i).id) break;
                        }
                     }
                  }
               }
            }

            Self->Viewport->draw();
         }

         return ERR::Okay;
      }
   }

   return ERR::Search;
}

//********************************************************************************************************************

#include "document_def.c"

static const FieldArray clFields[] = {
   { "Description",  FDF_CPPSTRING|FDF_R },
   { "Title",        FDF_CPPSTRING|FDF_R },
   { "Author",       FDF_CPPSTRING|FDF_R },
   { "Copyright",    FDF_CPPSTRING|FDF_R },
   { "Keywords",     FDF_CPPSTRING|FDF_R },
   { "Viewport",     FDF_OBJECT|FDF_RW, nullptr, SET_Viewport, CLASSID::VECTORVIEWPORT },
   { "Focus",        FDF_OBJECT|FDF_RI, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "View",         FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Page",         FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "TabFocus",     FDF_OBJECTID|FDF_RW },
   { "EventMask",    FDF_INTFLAGS|FDF_FLAGS|FDF_RW, nullptr, nullptr, &clDocumentEventMask },
   { "Flags",        FDF_INTFLAGS|FDF_RI, nullptr, SET_Flags, &clDocumentFlags },
   { "PageHeight",   FDF_INT|FDF_R },
   { "Error",        FDF_INT|FDF_R },
   // Virtual fields
   { "ClientScript",  FDF_OBJECT|FDF_I,        nullptr, SET_ClientScript },
   { "EventCallback", FDF_FUNCTION|FDF_RW,     GET_EventCallback, SET_EventCallback },
   { "Path",          FDF_CPPSTRING|FDF_RW,    GET_Path, SET_Path },
   { "Origin",        FDF_CPPSTRING|FDF_RW,    GET_Path, SET_Origin },
   { "PageWidth",     FDF_UNIT|FDF_INT|FDF_SCALED|FDF_RW, GET_PageWidth, SET_PageWidth },
   { "Pretext",       FDF_CPPSTRING|FDF_W,     nullptr, SET_Pretext },
   { "Src",           FDF_SYNONYM|FDF_CPPSTRING|FDF_RW, GET_Path, SET_Path },
   { "WorkingPath",   FDF_CPPSTRING|FDF_R,     GET_WorkingPath, nullptr },
   END_FIELD
};
