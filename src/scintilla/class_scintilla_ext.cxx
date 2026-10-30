
// Style codes for each lexer are defined in SciLexer.h

#include <format>

#include "lexer_styles.cxx"
#include "lexer_keywords.cxx"

/*********************************************************************************************************************
** This is the main entry point, we're called from the Init action here.
*/

ScintillaKTK::ScintillaKTK(extScintilla *Scintilla)
:  scintilla(Scintilla)
{
   idle_on        = false;
   ticking_on     = false;
   captured_mouse = false;

   // The platform Window is identified by the Kotuku Scintilla object (see pan_window.cxx)

   wMain = Scintilla;

   SendScintilla(SCI_STYLESETFONT, STYLE_DEFAULT, "Clean");
   SendScintilla(SCI_STYLESETSIZE, STYLE_DEFAULT, 10);
   SendScintilla(SCI_STYLECLEARALL);
   SendScintilla(SCI_STYLESETBOLD, STYLE_BRACELIGHT, 1);
   SendScintilla(SCI_STYLESETBOLD, STYLE_BRACEBAD, 1);
   SendScintilla(SCI_STYLESETITALIC, STYLE_BRACEBAD, 1);
   SetStyleColours();

   // SC_MOD_CHANGEFOLD is required by FoldChanged()

   SendScintilla(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT | SC_MOD_CHANGEFOLD |
      SC_PERFORMED_USER | SC_PERFORMED_UNDO | SC_PERFORMED_REDO |
      SC_MULTISTEPUNDOREDO | SC_LASTSTEPINUNDOREDO | SC_MOD_BEFOREINSERT |
      SC_MOD_BEFOREDELETE | SC_MULTILINEUNDOREDO);

   // Margin 1 shows the client's markers and margin 2 shows the fold markers.  Their widths are managed by the Symbols
   // and FoldingMarkers fields, and the symbol margin is made sensitive while a MarginClick callback is defined.

   SendScintilla(SCI_SETMARGINTYPEN, 1UL, long(SC_MARGIN_SYMBOL));
   SendScintilla(SCI_SETMARGINMASKN, 1UL, long(~SC_MASK_FOLDERS));
   SendScintilla(SCI_SETMARGINTYPEN, 2UL, long(SC_MARGIN_SYMBOL));
   SendScintilla(SCI_SETMARGINMASKN, 2UL, long(SC_MASK_FOLDERS));
   SendScintilla(SCI_SETMARGINSENSITIVEN, 2UL, 1L);
   SendScintilla(SCI_SETFOLDFLAGS, (unsigned long)SC_FOLDFLAG_LINEAFTER_CONTRACTED);
   DefineFoldMarkers();

   // We're always UTF-8

   SendScintilla(SCI_SETCODEPAGE, SC_CP_UTF8);

   // Call tips are drawn with STYLE_CALLTIP rather than fixed colours.  Tabs are not expanded.

   SendScintilla(SCI_CALLTIPUSESTYLE, 0UL);
}

ScintillaKTK::~ScintillaKTK()
{

}

//********************************************************************************************************************

void ScintillaKTK::Finalise()
{
   kt::Log(__FUNCTION__).trace("");

   SetTicking(false);
   ScintillaBase::Finalise();
}

/*********************************************************************************************************************
** Code assistance.  The completion list and call tip are displayed by pan_listbox.cxx and pan_popup.cxx, while the
** editing behaviour is ScintillaBase's.
*/

ERR ScintillaKTK::panShowCompletions(int Length, std::string_view Candidates, SCACF Flags)
{
   kt::Log log(__FUNCTION__);

   if (((scintilla->Flags & SCIF::DISABLED) != SCIF::NIL) or (not scintilla->Viewport)) {
      return log.warning(ERR::NotPossible);
   }
   if ((scintilla->Flags & SCIF::EDIT) IS SCIF::NIL) return log.warning(ERR::ReadOnly);

   const int caret = sel.MainCaret();
   if ((Length < 0) or (caret - Length < pdoc->LineStart(pdoc->LineFromPosition(caret)))) {
      return log.warning(ERR::OutOfRange);
   }

   // AutoComplete finds the typed prefix with a binary search, which requires the order of its own comparison.

   const bool ignore_case = (Flags & SCACF::IGNORE_CASE) != SCACF::NIL;

   std::vector<std::string_view> list;
   while (not Candidates.empty()) {
      auto end = Candidates.find('\n');
      auto item = Candidates.substr(0, end);
      if (item.ends_with('\r')) item.remove_suffix(1);
      if (not item.empty()) list.push_back(item);
      if (end IS std::string_view::npos) break;
      Candidates.remove_prefix(end + 1);
   }

   if (list.empty()) return ERR::NoData;

   if (ignore_case) {
      std::stable_sort(list.begin(), list.end(), [](std::string_view A, std::string_view B) {
         return std::lexicographical_compare(A.begin(), A.end(), B.begin(), B.end(), [](char X, char Y) {
            return Scintilla::MakeUpperCase(X) < Scintilla::MakeUpperCase(Y);
         });
      });
   }
   else std::sort(list.begin(), list.end());

   list.erase(std::unique(list.begin(), list.end()), list.end());

   std::string joined;
   for (auto &item : list) {
      if (not joined.empty()) joined.push_back('\n');
      joined.append(item);
   }

   log.traceBranch("%d candidates, prefix length %d", int(list.size()), Length);

   SendScintilla(SCI_AUTOCSETSEPARATOR, (unsigned long)'\n');
   SendScintilla(SCI_AUTOCSETTYPESEPARATOR, 0UL);
   SendScintilla(SCI_AUTOCSETIGNORECASE, (unsigned long)ignore_case);

   // The engine's chooseSingle shortcut bypasses acceptance notifications and dropRestOfWord, and leaves an
   // existing list open.  Start the normal list and accept its matching candidate before it can be painted.

   SendScintilla(SCI_AUTOCSETCHOOSESINGLE, 0UL);
   SendScintilla(SCI_AUTOCSETDROPRESTOFWORD, (unsigned long)((Flags & SCACF::DROP_REST_OF_WORD) != SCACF::NIL));
   SendScintilla(SCI_AUTOCSHOW, (unsigned long)Length, joined.c_str());
   if ((list.size() IS 1) and ((Flags & SCACF::CHOOSE_SINGLE) != SCACF::NIL) and (ac.Active())) {
      AutoCompleteCompleted();
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// CallTipShow() closes any completion list, which is reported by panCompletionClosed().

ERR ScintillaKTK::panShowCallTip(int Pos, std::string_view Text, int HighlightStart, int HighlightEnd)
{
   kt::Log log(__FUNCTION__);

   if (((scintilla->Flags & SCIF::DISABLED) != SCIF::NIL) or (not scintilla->Viewport)) {
      return log.warning(ERR::NotPossible);
   }

   if (Pos IS -1) Pos = sel.MainCaret();
   else if ((Pos < 0) or (Pos > pdoc->Length())) return log.warning(ERR::OutOfRange);

   std::string text(Text);
   calltip_pos = Pos;
   SendScintilla(SCI_CALLTIPSHOW, (unsigned long)Pos, text.c_str());
   if ((HighlightStart >= 0) and (HighlightEnd > HighlightStart)) {
      SendScintilla(SCI_CALLTIPSETHLT, (unsigned long)HighlightStart, long(HighlightEnd));
   }
   return ERR::Okay;
}

//********************************************************************************************************************

void ScintillaKTK::panCancelPopups()
{
   AutoCompleteCancel();
   ct.CallTipCancel();
}

//********************************************************************************************************************

SCPOP ScintillaKTK::panActivePopup()
{
   if (ac.Active()) return SCPOP::COMPLETIONS;
   if (ct.inCallTipMode) return SCPOP::CALL_TIP;
   return SCPOP::NONE;
}

//********************************************************************************************************************
// The position of the start of the word that is being completed.

int ScintillaKTK::panCompletionStart()
{
   return ac.posStart - ac.startLen;
}

//********************************************************************************************************************
// Point is in the coordinate system of the call tip.  CallTipClick() reports SCN_CALLTIPCLICK.

void ScintillaKTK::panCallTipClick(Scintilla::Point Point)
{
   ct.MouseClick(Point);
   CallTipClick();
}

/*********************************************************************************************************************
** Report an event to the CompletionCallback.  Events that occur part way through an operation of ScintillaBase are
** deferred, because the client may respond by opening or closing a pop-up, which would interfere with the operation.
** Deferred events are reported by the ReportEvent method in the order that they occurred.
*/

void ScintillaKTK::ReportCompletion(SCAC Event, int Position, int Value, CSTRING Text, bool Deferred)
{
   auto &callback = scintilla->CompletionCallback;
   if (not callback.defined()) return;

   if (Deferred) {
      completion_reports.push_back({ Event, Position, Value, Text ? Text : "" });
      QueueAction(sci::ReportEvent::id, scintilla->UID);
      return;
   }

   if (callback.stale()) {
      callback.unpin();
      callback.clear();
      return;
   }

   if (not Text) Text = "";

   if (callback.isC()) {
      kt::SwitchContext ctx(callback.Context);
      auto routine = (void (*)(extScintilla *, SCAC, int, int, CSTRING, APTR))callback.Routine;
      routine(scintilla, Event, Position, Value, Text, callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({
         { "Scintilla", scintilla, FD_OBJECTPTR },
         { "Event", int(Event) },
         { "Position", Position },
         { "Value", Value },
         { "Text", Text }
      }));
   }
}

//********************************************************************************************************************
// Called when the completion list closes.  A closure that does not follow an accepted completion is a cancellation.

void ScintillaKTK::panCompletionClosed()
{
   if (completion_selected) completion_selected = false;
   else ReportCompletion(SCAC::CANCELLED, sel.MainCaret(), 0, nullptr, true);
}

//********************************************************************************************************************
// Called by the ReportEvent method.  The reports are moved out first because the callback can add more.

void ScintillaKTK::panFlushCompletionReports()
{
   if (completion_reports.empty()) return;

   auto reports = std::move(completion_reports);
   completion_reports.clear();
   for (auto &report : reports) {
      ReportCompletion(report.event, report.position, report.value, report.text.c_str(), false);
   }
}

/*********************************************************************************************************************
** Scintilla calls these when topLine or xOffset change.  Scrollbars are external to the editor (see gui.scrollbar), so
** the scroll position is expressed by moving the Page within the View.
*/

void ScintillaKTK::SetVerticalScrollPos()
{
   kt::Log(__FUNCTION__).trace("%d", topLine);
   DwellEnd(true); // Cancel any current mouse hover
   panSyncPage();
}

void ScintillaKTK::SetHorizontalScrollPos()
{
   kt::Log(__FUNCTION__).trace("%d", xOffset);
   DwellEnd(true); // Cancel any current mouse hover
   panSyncPage();
}

/*********************************************************************************************************************
** Scintilla calls this with nMax = MaxScrollPos() + nPage - 1, where nPage is the number of lines that fit in the
** client area.  The Page is sized so that the furthest that it can be moved within the View corresponds to the
** maximum scroll positions of Scintilla, i.e. MaxScrollPos() lines vertically and the scroll width minus the width of
** the text area horizontally.  The result is true if the extent of the Page has changed.
*/

bool ScintillaKTK::ModifyScrollBars(int nMax, int nPage)
{
   kt::Log log(__FUNCTION__);

   int height = (nMax - nPage + 1) * vs.lineHeight + scintilla->ViewHeight;
   int width  = scintilla->ViewWidth;
   if (wrapState IS eWrapNone) width = std::max(width, scrollWidth + vs.fixedColumnWidth + vs.rightMarginWidth);

   if ((width IS scintilla->PageWidth) and (height IS scintilla->PageHeight)) return false;

   log.traceBranch("Lines: %d/%d, Page: %dx%d", nMax, nPage, width, height);

   scintilla->PageWidth  = width;
   scintilla->PageHeight = height;

   if (scintilla->Page) {
      scintilla->Page->setWidth(width);
      scintilla->Page->setHeight(height);
   }

   return true;
}

//********************************************************************************************************************
// Called after SCI_SETWRAPMODE and SCI_SETHSCROLLBAR.  Scrollbars are not managed by the editor, so there is nothing
// to show or hide.

void ScintillaKTK::ReconfigureScrollBars()
{
}

//********************************************************************************************************************
// The clipboard holds plain text, so it cannot mark a clip as a rectangular selection.  The text of the most recent
// rectangular copy is retained instead, and pasting identical text from the clipboard is treated as a rectangular
// paste.  The record is shared by all editors in the process, so a rectangular clip can be pasted from one editor into
// another.  Copying the same text from another application is indistinguishable and also pastes as a rectangle.
// Scintilla objects are only used from the main thread.

static std::string glRectangularClip;

//********************************************************************************************************************
// Report a clipboard failure with a log message and SEF::FAIL_CLIPBOARD.  Scintilla's cut, copy and paste have no
// return value, so the error is also recorded for panClipboard().

ERR ScintillaKTK::clipboard_failed(ERR Error, CSTRING Message)
{
   kt::Log log(__FUNCTION__);
   log.warning("%s: %s", Message, GetErrorMsg(Error));

   scintilla->ReportEventFlags |= SEF::FAIL_CLIPBOARD;
   QueueAction(sci::ReportEvent::id, scintilla->UID);
   return Error;
}

//********************************************************************************************************************
// Copy text to the clipboard.  The text is copied in the document's line ending mode.  Text.len includes the
// terminating null.

ERR ScintillaKTK::copy_to_clipboard(const Scintilla::SelectionText &Text)
{
   kt::Log log(__FUNCTION__);

   if ((not Text.s) or (Text.len <= 1)) return ERR::Okay;

   std::string_view text(Text.s, size_t(Text.len - 1));
   log.traceBranch("%d bytes, rectangular: %c", int(text.size()), Text.rectangular ? 'Y' : 'N');

   objClipboard::create clipboard = { };
   if (not clipboard.ok()) return clipboard_failed(ERR::CreateObject, "Failed to create a Clipboard object");

   if (auto error = clipboard->addText(text); error != ERR::Okay) {
      return clipboard_failed(error, "Failed to copy text to the clipboard");
   }

   if (Text.rectangular) glRectangularClip.assign(text);
   else glRectangularClip.clear();
   return ERR::Okay;
}

//********************************************************************************************************************
// Read the most recent text clip.  ERR::NoData is returned without a failure report if the clipboard holds no text.

ERR ScintillaKTK::read_clipboard(std::string &Content)
{
   objClipboard::create clipboard = { };
   if (not clipboard.ok()) return clipboard_failed(ERR::CreateObject, "Failed to create a Clipboard object");

   kt::vector<std::string> files;
   if (auto error = clipboard->getFiles(CLIPTYPE::TEXT, 0, nullptr, files, nullptr); error != ERR::Okay) {
      if (error IS ERR::NoData) return ERR::NoData;
      return clipboard_failed(error, "Failed to query the clipboard");
   }

   if (files.empty()) return ERR::NoData;

   objFile::create file = { fl::Path(files[0]), fl::Flags(FL::READ) };
   if (not file.ok()) return clipboard_failed(ERR::OpenFile, "Failed to open the clipboard file");

   int64_t size;
   if (auto error = file->getSize(size); error != ERR::Okay) {
      return clipboard_failed(error, "Failed to read the clipboard file size");
   }

   if (size <= 0) return ERR::NoData;

   Content.resize(size_t(size));
   int len;
   if (auto error = file->read(std::span<int8_t>((int8_t *)Content.data(), Content.size()), &len);
       error != ERR::Okay) {
      return clipboard_failed(error, "Failed to read the clipboard file");
   }

   Content.resize(size_t(len));

   // Text clips are not null terminated.  If a null is present, the text ends there.

   if (auto end = Content.find('\0'); end != std::string::npos) Content.resize(end);
   if (Content.empty()) return ERR::NoData;
   return ERR::Okay;
}

//********************************************************************************************************************
// Paste the most recent text clip at the selection, replacing it.  Line endings are converted to the document's mode
// unless SCI_SETPASTECONVERTENDINGS has disabled this.  A clip that came from a rectangular copy is pasted as a
// rectangle.  The paste is a single undo action.

ERR ScintillaKTK::paste_clipboard()
{
   kt::Log log(__FUNCTION__);

   if (not CanPaste()) {
      pdoc->CheckReadOnly(); // Reports SCN_MODIFYATTEMPTRO
      return ERR::ReadOnly;
   }

   std::string content;
   if (auto error = read_clipboard(content); error != ERR::Okay) return error;

   const bool rectangular = (not glRectangularClip.empty()) and (content IS glRectangularClip);

   log.traceBranch("%d bytes, rectangular: %c", int(content.size()), rectangular ? 'Y' : 'N');

   {
      Scintilla::UndoGroup ug(pdoc);
      ClearSelection(multiPasteMode IS SC_MULTIPASTE_EACH);
      auto start = sel.IsRectangular() ? sel.Rectangular().Start() : sel.Range(sel.Main()).Start();

      if (rectangular) PasteRectangular(start, content.data(), int(content.size()));
      else if (convertPastes) {
         int len;
         std::unique_ptr<char[]> text(Scintilla::Document::TransformLineEnds(&len, content.data(), content.size(),
            pdoc->eolMode));
         InsertPaste(start, text.get(), len);
      }
      else InsertPaste(start, content.data(), int(content.size()));
   }

   calc_longest_line(scintilla);
   Redraw();
   return ERR::Okay;
}

//********************************************************************************************************************
// Called by Scintilla for copy operations that do not go through Copy(), e.g. SCI_COPYRANGE and SCI_COPYTEXT.

void ScintillaKTK::CopyToClipboard(const Scintilla::SelectionText &selectedText)
{
   clip_error = copy_to_clipboard(selectedText);
}

/*********************************************************************************************************************
** Cut the selected text to the clipboard.  As for Scintilla's own implementation, a read-only document refuses the
** cut entirely.  The selection is retained if the clipboard does not accept the text.  Editor::Cut() is not virtual,
** so WndProc() routes SCI_CUT and SCI_LINECUT here.
*/

ERR ScintillaKTK::cut_clipboard()
{
   if (SelectionEmpty()) return ERR::Okay;

   pdoc->CheckReadOnly(); // Reports SCN_MODIFYATTEMPTRO
   if ((pdoc->IsReadOnly()) or (SelectionContainsProtected())) return ERR::ReadOnly;

   Scintilla::SelectionText text;
   CopySelectionRange(&text);
   if (auto error = copy_to_clipboard(text); error != ERR::Okay) return error;

   ClearSelection();
   return ERR::Okay;
}

/*********************************************************************************************************************
** Copy the selected text to the clipboard.  A rectangular selection is copied with a line ending after each row.
*/

void ScintillaKTK::Copy()
{
   clip_error = ERR::Okay;
   if (SelectionEmpty()) return;

   Scintilla::SelectionText text;
   CopySelectionRange(&text);
   clip_error = copy_to_clipboard(text);
}

//********************************************************************************************************************

void ScintillaKTK::Paste()
{
   clip_error = paste_clipboard();
}

/*********************************************************************************************************************
** Run a clipboard operation for the Clipboard action.  The operation is sent as a Scintilla message, so it behaves as
** the equivalent key press does.  If SCIF::EDIT is not set, cut and paste are refused and reported as SEF::FAIL_RO.
*/

ERR ScintillaKTK::panClipboard(CLIPMODE Mode)
{
   unsigned int msg;
   if (Mode IS CLIPMODE::CUT) msg = SCI_CUT;
   else if (Mode IS CLIPMODE::COPY) msg = SCI_COPY;
   else if (Mode IS CLIPMODE::PASTE) msg = SCI_PASTE;
   else return ERR::Args;

   const bool read_only = (scintilla->Flags & SCIF::EDIT) IS SCIF::NIL;
   if (read_only) SendScintilla(SCI_SETREADONLY, 1UL);

   clip_error = ERR::Okay;
   SendScintilla(msg);

   if (read_only) SendScintilla(SCI_SETREADONLY, 0UL);
   return clip_error;
}

//********************************************************************************************************************
// This is used for the drag and drop of selected text.

void ScintillaKTK::ClaimSelection()
{
   kt::Log log(__FUNCTION__);
   log.traceBranch();
   if (!SelectionEmpty()) primarySelection = true;
   else primarySelection = false;
}

//********************************************************************************************************************

void ScintillaKTK::NotifyChange()
{
   // This method is useless because Scintilla immediately follows this
   // up with the SCN_MODIFIED message, which carries a lot more detail.
}

//********************************************************************************************************************
// Sometimes Scintilla will report events that have occurred in the text editor.

void ScintillaKTK::NotifyParent(Scintilla::SCNotification scn)
{
   kt::Log log("SciMsg");
   int code;

   if (!(code = scn.nmhdr.code)) return;

   if (code IS SCN_UPDATEUI) {
      // Either the text or styling of the document has changed or the selection range has changed. Now would be a good
      // time to update any container UI elements that depend on document or view state.  This was previously called
      // SCN_CHECKBRACE because a common use is to check whether the caret is next to a brace and set highlights on
      // this brace and its corresponding matching brace.

      log.traceBranch("[UPDATEUI] $%x", scn.updated);

      // Update the cursor-row and cursor-column fields if they've changed.

      int pos = SendScintilla(SCI_GETCURRENTPOS);
      if (pos != oldpos) {
         oldpos = pos;
         scintilla->CursorRow = SendScintilla(SCI_LINEFROMPOSITION, pos);
         scintilla->CursorCol = SendScintilla(SCI_GETCOLUMN, pos);
         if (SendScintilla(SCI_GETLEXER) IS SCLEX_CPP) braceMatch();

         // Event report has to be delayed, as we otherwise get interference in the drawing process.
         scintilla->ReportEventFlags |= SEF::CURSOR_POS;
         QueueAction(sci::ReportEvent::id, scintilla->UID);
      }
   }
   else if (code IS SCN_STYLENEEDED) {
      // If you used SCLEX_CONTAINER to make the container act as the lexer, you will receive this notification when
      // Scintilla is about to display or print text that requires styling. You are required to style the text from the
      // line that contains the position returned by SCI_GETENDSTYLED up to the position passed in
      // SCNotification.position.

      log.trace("[STYLENEEDED]");

      //MyStyleRoutine(start, scn.position);

   }
   else if (code IS SCN_DOUBLECLICK) {
      // Mouse buttons have been interpreted as a double click

      log.trace("[DOUBLECLICK]");
   }
   else if (code IS SCN_MODIFYATTEMPTRO) {
      // An attempt has been made to modify the document when in read-only mode

      log.trace("[MODIFYATTEMPTRO]");

      scintilla->ReportEventFlags |= SEF::FAIL_RO;
      QueueAction(sci::ReportEvent::id, scintilla->UID);
   }
   else if (code IS SCN_CHARADDED) {
      // This is sent when the user types an ordinary text character (as opposed to a command character) that is
      // entered into the text. The container can use this to decide to display a call tip or an auto completion list.
      // The character is in SCNotification.ch.

      log.traceBranch("[CHARADDED]");

      int pos = SendScintilla(SCI_GETSELECTIONSTART);
      if (pos != SendScintilla(SCI_GETSELECTIONEND)) return;

      // Auto-indent management for the enter key

      if ((scintilla->AutoIndent) and ((scn.ch IS '\r') or (scn.ch IS '\n'))) {
         int row, indent, col;

         pos = SendScintilla(SCI_GETCURRENTPOS);
         row = SendScintilla(SCI_LINEFROMPOSITION, pos);
         col = SendScintilla(SCI_GETCOLUMN, pos);

         if (row > 0) {
            indent = SendScintilla(SCI_GETLINEINDENTATION, row - 1); // indent = The indentation of the previous line

            if (indent <= col);
            else {
               // Indent the current line
               SendScintilla(SCI_SETLINEINDENTATION, row, indent);

               // Move the cursor to the start of the line, after following the indentation whitespace
               pos = SendScintilla(SCI_GETLINEINDENTPOSITION, row);

               SendScintilla(SCI_SETSEL, (uint32_t)-1, pos);
            }
         }
      }

      scintilla->ReportEventFlags |= SEF::NEW_CHAR;
      QueueAction(sci::ReportEvent::id, scintilla->UID);

      ReportCompletion(SCAC::CHAR_ADDED, SendScintilla(SCI_GETCURRENTPOS), scn.ch, nullptr, false);
   }
   else if (code IS SCN_SAVEPOINTREACHED) {
      // The document is unmodified (recently saved)

      log.trace("[SAVEPOINTREACHED]");
   }
   else if (code IS SCN_SAVEPOINTLEFT) {
      // The document has just been modified

      log.trace("[SAVEPOINTLEFT]");

      if (!scintilla->HoldModify) {
         scintilla->setModified(TRUE);
      }
      else {
         // 'Hold Modifications' means that we have to tell Scintilla that the document is unmodified.

         SendScintilla(SCI_SETSAVEPOINT);
      }
   }
   else if (code IS SCN_KEY) {
      // Reports all keys pressed but not consumed by Scintilla

      log.trace("[KEY]");
   }
   else if (code IS SCN_MODIFIED) {
      // This notification is sent when the text or styling of the document changes or is about to change. You can set
      // a mask for the notifications that are sent to the container with SCI_SETMODEVENTMASK. The notification
      // structure contains information about what changed, how the change occurred and whether this changed the number
      // of lines in the document. No modifications may be performed while in a SCN_MODIFIED event.
      //
      // See HTML documentation for more information.

      log.trace("[MODIFIED] Type: %d, Length: %d, LinesAdded: %d, Line: %d", scn.modificationType, scn.length, scn.linesAdded, scn.line);

      if (scn.modificationType & SC_MOD_CHANGEFOLD) FoldChanged(scn.line, scn.foldLevelNow, scn.foldLevelPrev);
   }
   else if (code IS SCEN_SETFOCUS) {
      log.trace("[SETFOCUS]");
   }
   else if (code IS SCEN_KILLFOCUS) {
      log.trace("[KILLFOCUS]");
   }
   else if (code IS SCN_MACRORECORD) {
      // The SCI_STARTRECORD and SCI_STOPRECORD messages enable and disable macro recording. When enabled, each time a
      // recordable change occurs, the SCN_MACRORECORD notification is sent to the container. It is up to the container
      // to record the action. To see the complete list of SCI_* messages that are recordable, search the Scintilla
      // source Editor.cxx for Editor::NotifyMacroRecord. The fields of SCNotification set in this notification are:
      // Field    Usage
      // message  The SCI_* message that caused the notification.
      // wParam   The value of wParam in the SCI_* message.
      // lParam   The value of lParam in the SCI_* message.

      log.trace("[MACRORECORD]");
   }
   else if (code IS SCN_MARGINCLICK) {
      // The mouse was clicked inside a sensitive margin.  The position is the start of the clicked line.  A click in
      // the fold margin toggles the fold of a header line, and every click is then reported to the client.

      int line = pdoc->LineFromPosition(scn.position);
      log.traceBranch("[MARGINCLICK] Margin: %d, Line: %d", scn.margin, line);

      if ((scn.margin IS 2) and (pdoc->GetLevel(line) & SC_FOLDLEVELHEADERFLAG)) ToggleContraction(line);

      KQ qualifiers = KQ::NIL;
      if (scn.modifiers & SCI_SHIFT) qualifiers |= KQ::SHIFT;
      if (scn.modifiers & SCI_CTRL)  qualifiers |= KQ::CTRL;
      if (scn.modifiers & SCI_ALT)   qualifiers |= KQ::ALT;
      report_margin_click(scintilla, SCMG(scn.margin), line, qualifiers);
   }
   else if (code IS SCN_NEEDSHOWN) {
      // Scintilla has determined that a range of lines that is currently invisible should be made visible. An example of where this may be
      // needed is if the end of line of a contracted fold point is deleted. This message is sent to the container in case it wants to make the
      // line visible in some unusual way such as making the whole document visible. Most containers will just ensure each line in the range is
      // visible by calling SCI_ENSUREVISIBLE. The position and length fields of SCNotification indicate the range of the document that should be
      // made visible.

      // The range is inclusive of the line at position + length, as an insertion has a length of zero.

      int first = SendScintilla(SCI_LINEFROMPOSITION, scn.position);
      int last = SendScintilla(SCI_LINEFROMPOSITION, scn.position + scn.length);

      log.trace("[NEEDSHOWN] First: %d, Last: %d", first, last);

      for (int line=first; line <= last; line++) SendScintilla(SCI_ENSUREVISIBLE, line);
   }
   else if (code IS SCN_PAINTED) {
      // Painting has just been done. Useful when you want to update some other widgets based on a change in Scintilla,
      // but want to have the paint occur first to appear more responsive. There is no other information in SCNotification.

      log.trace("[PAINTED]");
   }
   else if (code IS SCN_USERLISTSELECTION) {
      // The user has selected an item in a user list. The SCNotification fields used are:
      // Field   Usage
      // wParam  This is set to the listType parameter from the SCI_USERLISTSHOW message that initiated the list.
      // text    The text of the selection.

      log.trace("[USERLISTSELECTION]");
   }
   else if (code IS SCN_DWELLSTART) {
      // Generated when the user keeps the mouse in one position for the dwell period (see SCI_SETMOUSEDWELLTIME).
      //
      // position: This is the nearest position in the document to the position where the mouse pointer was lingering.
      // x, y: Where the pointer lingered. The position field is set to SCI_POSITIONFROMPOINTCLOSE(x, y).

      log.trace("[DWELLSTART]");
   }
   else if (code IS SCN_DWELLEND) {
      // Generated after a SCN_DWELLSTART and the mouse is moved or other activity such as key press indicates the dwell is over.

      log.trace("[DWELLEND]");
   }
   else if (code IS SCN_ZOOM) {
      // Unsupported/Redundant Scintilla feature
   }
   else if (code IS SCN_HOTSPOTCLICK) {
      log.trace("[HOTSPOTCLICK]");

   }
   else if (code IS SCN_HOTSPOTDOUBLECLICK) {
      // Generated when the user clicks or double clicks on text that is in a style with the hotspot attribute set. This notification can be
      // used to link to variable definitions or web pages. The position field is set the text position of the click or double click and
      // the modifiers field set to the key modifiers held down in a similar manner to SCN_KEY.

      log.trace("[HOTSPOTDOUBLECLICK]");
   }
   else if (code IS SCN_CALLTIPCLICK) {
      // Generated when the user clicks on a calltip. This notification can be used to display the next function prototype when a function name
      // is overloaded with different arguments. The position field is set to 1 if the click is in an up arrow, 2 if in a down arrow, and 0 if
      // elsewhere.

      log.trace("[CALLTIPCLICK]");

      ReportCompletion(SCAC::CALL_TIP_CLICK, calltip_pos, scn.position, nullptr, true);
   }
   else if (code IS SCN_AUTOCSELECTION) {
      // The user has selected an item in an autocompletion list. The notification is sent before the selection is inserted. Automatic
      // insertion can be cancelled by sending a SCI_AUTOCCANCEL message before returning from the notification. The SCNotification fields
      // used are:
      //
      // lParam: The start position of the word being completed.
      // text: The text of the selection.

      log.trace("[AUTOCSELECTION]");

      completion_selected = true;
      ReportCompletion(SCAC::SELECTED, int(scn.lParam), 0, scn.text, true);
   }
   else if (code IS SCN_AUTOCCANCELLED) {
      // Not every cancellation is notified, so they are reported by panCompletionClosed() instead.
   }
   else if (code IS SCN_AUTOCCHARDELETED) {
      // A character was deleted while the completion list is open, which updates the selected item.
   }
   else if (code IS 2012) {
      // Deprecated
   }
   else log.traceWarning("Notification code %d unsupported.", code);
}

//********************************************************************************************************************

void ScintillaKTK::ScrollText(int linesToMove)
{
   // TODO Phase 3: Optimise by shifting the bitmap content and invalidating only the exposed band.
   Redraw();
}

/*********************************************************************************************************************
** Scintilla calls SetTicking(true) whenever the caret is shown at a new position.  The caret blink is restarted so that
** the caret remains visible for a full period after it moves.  See UpdateTimers() for the conditions under which the
** tick timer runs.
*/

void ScintillaKTK::SetTicking(bool On)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("State: %d", On);

   ticking_on = On;
   if (On) {
      timer.ticksToWait = caret.period;
      UpdateTimers();
   }
}

/*********************************************************************************************************************
** Scintilla requests idle processing when it has background work to do, such as wrapping lines.  Returning true
** confirms that Idle() will be called until it reports that the work is complete.
*/

bool ScintillaKTK::SetIdle(bool On)
{
   idle_on = On;
   if (On) UpdateTimers();
   return true;
}

/*********************************************************************************************************************
** Start the tick and idle timers if they are required.  Timers are not stopped here because a timer cannot be removed
** while its own callback is running.  Instead, tick_timer() and idle_timer() end their subscriptions when panTick() or
** panIdle() report that there is nothing left to do.  The timers are subscribed for the Scintilla object, so that
** they are removed with it.
*/

void ScintillaKTK::UpdateTimers()
{
   kt::SwitchContext ctx(scintilla);

   if ((TickRequired()) and (not scintilla->TickTimer)) {
      // Tick() counts down the caret period and other delays in steps of tickSize milliseconds.
      SubscribeTimer(double(timer.tickSize) / 1000.0, C_FUNCTION(tick_timer), &scintilla->TickTimer);
   }

   if ((idle_on) and (not scintilla->IdleTimer)) {
      SubscribeTimer(IDLE_INTERVAL, C_FUNCTION(idle_timer), &scintilla->IdleTimer);
   }
}

/*********************************************************************************************************************
** Ticking is only necessary for blinking the caret while the editor has the focus, and for scrolling while the mouse
** is captured.  Tick() repeats the last mouse movement while the mouse is captured, which scrolls the document if the
** pointer is held outside the client area.
*/

bool ScintillaKTK::TickRequired()
{
   return (HaveMouseCapture()) or ((ticking_on) and (hasFocus) and (caret.period > 0));
}

//********************************************************************************************************************
// Called by tick_timer().  Returns false if the timer is no longer required.

bool ScintillaKTK::panTick()
{
   if (not TickRequired()) return false;
   Tick(); // Goes to -> Editor::Tick()
   return TickRequired();
}

//********************************************************************************************************************
// Called by idle_timer().  Returns false if the timer is no longer required.

bool ScintillaKTK::panIdle()
{
   if (idle_on) idle_on = Idle();
   return idle_on;
}

//********************************************************************************************************************
// Scintilla captures the mouse while the user holds a button and drags (e.g. when selecting text).  The scene already
// directs all pointer input to the Canvas while the LMB is held, so the capture state is only recorded here.  The tick
// timer runs while the mouse is captured, so that holding the pointer outside the client keeps scrolling.

void ScintillaKTK::SetMouseCapture(bool On)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("State: %d", On);
   captured_mouse = On;
   if (On) UpdateTimers();
}

//********************************************************************************************************************
// Scintilla shows the arrow over the selection to indicate that the text can be dragged.  Text drag and drop is not
// supported yet (see panMousePress()), so the text cursor is kept.  Margins use the reverse arrow and are unaffected.

void ScintillaKTK::DisplayCursor(Scintilla::Window::Cursor Cursor)
{
   if (Cursor IS Scintilla::Window::cursorArrow) Cursor = Scintilla::Window::cursorText; // TODO Phase 7
   ScintillaBase::DisplayCursor(Cursor);
}

//********************************************************************************************************************

bool ScintillaKTK::HaveMouseCapture()
{
   return captured_mouse;
}

//********************************************************************************************************************

sptr_t ScintillaKTK::WndProc(unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
   switch(iMessage) {

   case SCI_GETDIRECTFUNCTION:
      return reinterpret_cast<sptr_t>(DirectFunction);

   case SCI_GETDIRECTPOINTER:
      return reinterpret_cast<sptr_t>(this);

   case SCI_CUT: // Editor::Cut() is not virtual
      if (recordingMacro) NotifyMacroRecord(iMessage, wParam, lParam);
      clip_error = cut_clipboard();
      SetLastXChosen();
      return 0;

   case SCI_LINECUT: { // Cut the lines that the selection touches, as Editor::KeyCommand() does
      if (recordingMacro) NotifyMacroRecord(iMessage, wParam, lParam);
      int start = pdoc->LineStart(pdoc->LineFromPosition(SelectionStart().Position()));
      int end = pdoc->LineStart(pdoc->LineFromPosition(SelectionEnd().Position()) + 1);
      SetSelection(start, end);
      clip_error = cut_clipboard();
      SetLastXChosen();
      return 0;
   }

   default:
      return ScintillaBase::WndProc(iMessage, wParam, lParam);
   }
   return 0;
}

//********************************************************************************************************************

sptr_t ScintillaKTK::DirectFunction(ScintillaKTK *sci, unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
   return sci->WndProc(iMessage, wParam, lParam);
}

/*********************************************************************************************************************
** Do nothing; this is a Win32 support function.
*/

sptr_t ScintillaKTK::DefWndProc(unsigned int iMessage, uptr_t wParam, sptr_t lParam)
{
   return 0;
}

/*********************************************************************************************************************
** Paint an Area of the client into the Scintilla object's Bitmap.  Refer to pan_surface.cxx for the drawing routines.
** If Scintilla abandons the paint because a wider area needs to be redrawn (for instance, styling has spilled over a
** line end) then the entire client is repainted immediately, as the GTK platform does.
*/

void ScintillaKTK::panPaint(Scintilla::PRectangle Area)
{
   kt::Log log(__FUNCTION__);

   if (this->paintState != notPainting) return; // Recursion prevention
   if (not scintilla->Bitmap) return;

   auto rcClient = GetClientRectangle();

   log.traceBranch("Area: %dx%d - %dx%d", Area.left, Area.top, Area.Width(), Area.Height());

   std::unique_ptr<Scintilla::Surface> surface(Scintilla::Surface::Allocate());
   if (not surface) return;
   surface->Init(scintilla->Bitmap, wMain.GetID());

   this->paintState = painting;
   this->rcPaint    = Area;
   this->paintingAllText = rcPaint.Contains(rcClient);
   this->Paint(surface.get(), rcPaint);

   if (this->paintState IS paintAbandoned) {
      this->paintState = painting;
      this->rcPaint    = rcClient;
      this->paintingAllText = true;
      this->Paint(surface.get(), rcPaint);
   }

   this->paintState = notPainting;
}

//********************************************************************************************************************
// Invalidate the entire client area.

void ScintillaKTK::panRedraw()
{
   Redraw();
}

/*********************************************************************************************************************
** Called on initialisation and whenever Face or FontSize changes.  SurfacePan::GetFont() reads the style handles
** from the owning Scintilla object.
*/

void ScintillaKTK::panFontChanged()
{
   kt::Log log(__FUNCTION__);
   log.traceBranch();

   this->InvalidateStyleRedraw();
   this->RefreshStyleData();
}

//********************************************************************************************************************

void ScintillaKTK::panWordwrap(int Value)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("%d", Value);

//   int bytepos = SendScintilla(SCI_POSITIONFROMLINE, topLine);

   SendScintilla(SCI_SETWRAPMODE, Value);

   // Scintilla likes to process wordwrapping in its timer, but this causes
   // issues with the scrollbar (and potentially other problems) so this
   // loop will force the wordwrap to be processed immediately.

   scintilla->ScrollLocked++;
   while (Idle());
   scintilla->ScrollLocked--;

   calc_longest_line(scintilla);
/*
   if (!Value) {
      topLine = SendScintilla(SCI_LINEFROMPOSITION, bytepos);
      SetVerticalScrollPos();
   }
*/
   //SendScintilla(SCI_ENSUREVISIBLE, SendScintilla(SCI_LINEFROMPOSITION, SendScintilla(SCI_GETCURRENTPOS)));
}

/*********************************************************************************************************************
** Process a key press.  Command keys are translated to Scintilla key codes and passed through the key map, while
** printable characters are inserted as text.  Letters are always passed to the key map in upper case, because that is
** how its shortcuts are defined (e.g. Ctrl+Z).
**
** Ctrl and Alt combinations are treated as shortcuts, with the exception of AltGr.  AltGr is reported as R_ALT on X11
** layouts that map it to Alt_R, and with an additional Ctrl on Windows.  A printable character that is produced with
** AltGr is text.  On X11 layouts that map AltGr to ISO_Level3_Shift, no qualifier is reported for it.
**
** If SCIF::EDIT is not set, the document is read-only for the duration of the key press.  Navigation and selection
** are unaffected, while edits are refused and reported as SEF::FAIL_RO.  The document is otherwise left writable so
** that it can always be modified through the API.
*/

void ScintillaKTK::panKeyDown(KEY Code, KQ Qualifiers, int Unicode)
{
   const bool shift = (Qualifiers & KQ::SHIFT) != KQ::NIL;
   const bool ctrl  = (Qualifiers & KQ::CTRL) != KQ::NIL;
   const bool alt   = (Qualifiers & KQ::ALT) != KQ::NIL;
   const bool altgr = ((Qualifiers & KQ::R_ALT) != KQ::NIL) and ((Qualifiers & KQ::L_ALT) IS KQ::NIL);
   const bool printable = ((Qualifiers & KQ::NOT_PRINTABLE) IS KQ::NIL) and (Unicode >= 0x20) and (Unicode != 0x7f);

   const bool read_only = (scintilla->Flags & SCIF::EDIT) IS SCIF::NIL;

   // Ctrl+Space requests completion candidates from the client.  It has no binding in Scintilla's key map.

   if ((ctrl) and (not alt) and (not shift) and (Code IS KEY::SPACE)) {
      if (not read_only) ReportCompletion(SCAC::REQUEST, SendScintilla(SCI_GETCURRENTPOS), 0, nullptr, false);
      return;
   }

   if (read_only) SendScintilla(SCI_SETREADONLY, 1UL);

   if (int key = command_key(Code, printable)) KeyDown(key, shift, ctrl, alt);
   else if ((printable) and ((altgr) or ((not ctrl) and (not alt)))) {
      char utf8[8];
      int len = UTF8WriteValue(Unicode, std::span<int8_t>((int8_t *)utf8, sizeof(utf8)));
      if (len > 0) AddCharUTF(utf8, len);
   }
   else if (int key = shortcut_key(Code)) KeyDown(key, shift, ctrl, alt);

   if (read_only) SendScintilla(SCI_SETREADONLY, 0UL);
}

/*********************************************************************************************************************
** Process a mouse button press at a client position.  The Time is in milliseconds and allows Scintilla to detect
** double and triple clicks.  Modifier keys are taken from the state that key_event() tracks: Shift extends the
** selection, and Alt starts a rectangular selection.
**
** Scintilla's internal text drag is not supported yet, so a click inside the selection clears it first.  This turns
** the click into an ordinary click, and a drag from that point into a new selection.  Shift clicks are excluded so
** that they extend the selection from its anchor.
*/

void ScintillaKTK::panMousePress(JET Button, double X, double Y, unsigned int Time)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("%.0fx%.0f", X, Y);

   if (Button IS JET::LMB) {
      Scintilla::Point point(int(std::floor(X)), int(std::floor(Y)));

      if ((not scintilla->KeyShift) and (not SelectionEmpty()) and (PointInSelection(point))) {
         SetEmptySelection(CurrentPosition()); // TODO Phase 7: Text drag and drop
      }

      ButtonDown(point, Time, scintilla->KeyShift, scintilla->KeyCtrl, scintilla->KeyAlt);
   }
}

//********************************************************************************************************************

void ScintillaKTK::panMouseMove(double X, double Y)
{
   ButtonMove(Scintilla::Point(int(std::floor(X)), int(std::floor(Y))));
}

//********************************************************************************************************************

void ScintillaKTK::panMouseRelease(JET Button, double X, double Y, unsigned int Time)
{
   kt::Log log(__FUNCTION__);
   log.trace("%.0fx%.0f", X, Y);

   if (Button IS JET::LMB) {
      ButtonUp(Scintilla::Point(int(std::floor(X)), int(std::floor(Y))), Time, scintilla->KeyCtrl);
   }
}

//********************************************************************************************************************
// Abandon a mouse drag, e.g. when the editor is disabled while a button is held.  The selection is left as it is.

void ScintillaKTK::panReleaseMouse()
{
   if (HaveMouseCapture()) SetMouseCapture(false);
}

//********************************************************************************************************************

void ScintillaKTK::panResized()
{
   kt::Log log(__FUNCTION__);
   log.traceBranch();
   ChangeSize();
}

/*********************************************************************************************************************
** Scroll to a point in the document, expressed in pixels from the top-left corner.  Scintilla scrolls vertically by
** whole lines, so Y is rounded to the nearest line.  Both axes are clamped to the extent of the Page.  The Page is
** then snapped to the resulting position, which also corrects a Page that was moved to an unreachable position.
*/

void ScintillaKTK::panScrollTo(double X, double Y)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("%.2f, %.2f", X, Y);

   if (vs.lineHeight > 0) {
      int max_x = std::max(0, scintilla->PageWidth - scintilla->ViewWidth);

      scintilla->ScrollLocked++;
      ScrollTo(int(std::lround(Y / vs.lineHeight)));
      HorizontalScrollTo(std::clamp(int(std::lround(X)), 0, max_x));
      scintilla->ScrollLocked--;
   }

   panSyncPage();
}

//********************************************************************************************************************
// Scroll relative to the current position by a number of columns (measured in space widths) and lines.

void ScintillaKTK::panScrollBy(int Columns, int Lines)
{
   panScrollTo(xOffset + (Columns * vs.spaceWidth), (topLine + Lines) * vs.lineHeight);
}

//********************************************************************************************************************
// Move the Page and Canvas to reflect Scintilla's current scroll position.

void ScintillaKTK::panSyncPage()
{
   scintilla->ScrollX = xOffset;
   scintilla->ScrollY = topLine * vs.lineHeight;
   sync_page(scintilla);
}

//********************************************************************************************************************
// Showing the caret on receipt of the focus starts the tick timer.  On losing the focus, the timer ends itself on its
// next call.

void ScintillaKTK::panSetFocus(bool Focus)
{
   if (Focus != hasFocus) SetFocusState(Focus);
}

/*********************************************************************************************************************
** Get the cursor position.
*/

void ScintillaKTK::panGetCursorPosition(int *line, int *index)
{
   int pos, lin, linpos;

   pos = SendScintilla(SCI_GETCURRENTPOS);
   lin = SendScintilla(SCI_LINEFROMPOSITION,pos);
   linpos = SendScintilla(SCI_POSITIONFROMLINE,lin);

   *line = lin;
   *index = pos - linpos;
}

/*********************************************************************************************************************
** Set the cursor position.
*/

void ScintillaKTK::panSetCursorPosition(int line, int index)
{
   kt::Log log(__FUNCTION__);
   int pos, eol;

   log.trace("Line: %d, Index: %d", line, index);

   pos = SendScintilla(SCI_POSITIONFROMLINE,line) + index;
   eol = SendScintilla(SCI_GETLINEENDPOSITION,line);

   if (pos > eol) pos = eol;

   SendScintilla(SCI_GOTOPOS, pos);
}

/*********************************************************************************************************************
** Ensure a line is visible (expands folded zones).
*/

void ScintillaKTK::panEnsureLineVisible(int line)
{
   SendScintilla(SCI_ENSUREVISIBLEENFORCEPOLICY,line);
}

/*********************************************************************************************************************
** Enable or disable folding.  The lexers only compute fold levels while the 'fold' property is set.  Disabling folding
** expands every fold first, because the lines of a contracted fold could not be shown again.
*/

void ScintillaKTK::panSetFolding(bool Enable)
{
   if (not Enable) ExpandAllFolds();
   SendScintilla(SCI_SETPROPERTY, "fold", Enable ? "1" : "0");
}

/*********************************************************************************************************************
** Toggle the fold of a header line, or of the fold that contains the line if it is not a header.  Fold levels are
** computed when a line is styled, so the document is styled up to the end of the line first.  Returns false if
** folding is disabled, or if the line is not within a fold that has content.
*/

bool ScintillaKTK::panToggleFold(int Line)
{
   if (not scintilla->FoldingMarkers) return false; // The fold levels are stale while folding is disabled
   if ((Line < 0) or (Line >= pdoc->LinesTotal())) return false;

   pdoc->EnsureStyledTo(pdoc->LineStart(Line + 1));

   if (not (pdoc->GetLevel(Line) & SC_FOLDLEVELHEADERFLAG)) {
      Line = pdoc->GetFoldParent(Line);
      if (Line < 0) return false;
   }

   if (pdoc->GetLastChild(Line) <= Line) return false;

   ToggleContraction(Line);
   return true;
}

/*********************************************************************************************************************
** The fold margin uses the 'box tree' style.  The markers are drawn in the background colour with a contrasting
** foreground, so they are visible on the margin's chequer pattern.
*/

void ScintillaKTK::DefineFoldMarkers()
{
   static const struct { int Marker, Symbol; } folders[] = {
      { SC_MARKNUM_FOLDEROPEN,    SC_MARK_BOXMINUS },
      { SC_MARKNUM_FOLDER,        SC_MARK_BOXPLUS },
      { SC_MARKNUM_FOLDERSUB,     SC_MARK_VLINE },
      { SC_MARKNUM_FOLDERTAIL,    SC_MARK_LCORNER },
      { SC_MARKNUM_FOLDEREND,     SC_MARK_BOXPLUSCONNECTED },
      { SC_MARKNUM_FOLDEROPENMID, SC_MARK_BOXMINUSCONNECTED },
      { SC_MARKNUM_FOLDERMIDTAIL, SC_MARK_TCORNER }
   };

   for (auto &folder : folders) {
      SendScintilla(SCI_MARKERDEFINE, folder.Marker, folder.Symbol);
      SendScintilla(SCI_MARKERSETFORE, folder.Marker, SCICOLOUR(scintilla->MarginBkgd.Red,
         scintilla->MarginBkgd.Green, scintilla->MarginBkgd.Blue));
      SendScintilla(SCI_MARKERSETBACK, folder.Marker, SCICOLOUR(scintilla->MarginColour.Red,
         scintilla->MarginColour.Green, scintilla->MarginColour.Blue));
   }
}

//********************************************************************************************************************
// Show every line and mark every fold as expanded.

void ScintillaKTK::ExpandAllFolds()
{
   int total = pdoc->LinesTotal();
   bool changed = cs.HiddenLines();
   for (int line=0; line < total; line++) {
      if (cs.SetExpanded(line, true)) changed = true;
   }

   if (changed) {
      cs.SetVisible(0, total - 1, true);
      SetScrollBars();
      Redraw();
   }
}

/*********************************************************************************************************************
** Called when the fold level of a line changes.  If a contracted fold loses its header, for instance because its
** opening brace was deleted, its lines would remain hidden with no means of showing them.  The hidden lines that
** follow the line are therefore shown, and any contracted folds among them are expanded so that their markers match.
** This is called while the lexer is folding, so it relies only on the contraction state.
*/

void ScintillaKTK::FoldChanged(int Line, int LevelNow, int LevelPrev)
{
   if ((not (LevelPrev & SC_FOLDLEVELHEADERFLAG)) or (LevelNow & SC_FOLDLEVELHEADERFLAG)) return;
   if (cs.GetExpanded(Line)) return;

   cs.SetExpanded(Line, true);
   if (not cs.GetVisible(Line)) return; // The lines are revealed when the enclosing fold is expanded

   int last = Line;
   for (int line=Line + 1; (line < pdoc->LinesTotal()) and (not cs.GetVisible(line)); line++) {
      cs.SetExpanded(line, true);
      last = line;
   }

   if (last > Line) {
      cs.SetVisible(Line + 1, last, true);
      SetScrollBars();
      Redraw();
   }
}

/*********************************************************************************************************************
** Set the lexer.  To turn off the lexer, use SCLEX_NULL.  The fold levels of the previous lexer no longer apply, so all
** folds are expanded.  Scintilla does not pass its stored properties to a new lexer, so the 'fold' property is set
** again.
*/

void ScintillaKTK::SetLexer(uptr_t LexID)
{
   kt::Log log(__FUNCTION__);
   log.branch("Using lexer %d", (int)LexID);

   ExpandAllFolds();
   // ASP uses the HTML lexer with VBScript in server blocks.  NIL uses the null lexer rather than container styling.
   auto lexer = LexID IS int(SCLEX::ASP) ? SCLEX_HTML : (LexID ? LexID : SCLEX_NULL);
   SendScintilla(SCI_SETLEXER, lexer);
   SendScintilla(SCI_SETSTYLEBITS, SendScintilla(SCI_GETSTYLEBITSNEEDED));
   SendScintilla(SCI_SETPROPERTY, "asp.default.language", LexID IS int(SCLEX::ASP) ? "2" : "1");
   SendScintilla(SCI_SETPROPERTY, "fold", scintilla->FoldingMarkers ? "1" : "0");
   SetKeywords();
   SetStyleColours();
   SendScintilla(SCI_COLOURISE, 0, -1);
   InvalidateStyleRedraw();
}

/*********************************************************************************************************************
** Handle brace matching.
*/

void ScintillaKTK::braceMatch()
{
   long braceAtCaret, braceOpposite;

   findMatchingBrace(braceAtCaret, braceOpposite, braceMode);

   if (braceAtCaret >= 0 and braceOpposite < 0) {
      SendScintilla(SCI_BRACEBADLIGHT, braceAtCaret);
      SendScintilla(SCI_SETHIGHLIGHTGUIDE,0UL);
   }
   else {
      char chBrace = SendScintilla(SCI_GETCHARAT,braceAtCaret);

      SendScintilla(SCI_BRACEHIGHLIGHT,braceAtCaret,braceOpposite);

      long columnAtCaret = SendScintilla(SCI_GETCOLUMN,braceAtCaret);
      long columnOpposite = SendScintilla(SCI_GETCOLUMN,braceOpposite);

      if (chBrace IS ':') {
         long lineStart = SendScintilla(SCI_LINEFROMPOSITION,braceAtCaret);
         long indentPos = SendScintilla(SCI_GETLINEINDENTPOSITION,lineStart);
         long indentPosNext = SendScintilla(SCI_GETLINEINDENTPOSITION,lineStart + 1);
         columnAtCaret = SendScintilla(SCI_GETCOLUMN,indentPos);
         long columnAtCaretNext = SendScintilla(SCI_GETCOLUMN,indentPosNext);
         long indentSize = SendScintilla(SCI_GETINDENT);

         if (columnAtCaretNext - indentSize > 1) columnAtCaret = columnAtCaretNext - indentSize;
         if (columnOpposite IS 0) columnOpposite = columnAtCaret;
      }

      long column = columnAtCaret;

      if (column > columnOpposite) column = columnOpposite;

      SendScintilla(SCI_SETHIGHLIGHTGUIDE,column);
   }
}

/*********************************************************************************************************************
** Check if the character at a position is a brace.
*/

long ScintillaKTK::checkBrace(long pos,int brace_style)
{
   long brace_pos = -1;
   char ch = SendScintilla(SCI_GETCHARAT,pos);

   if ((ch IS '{') or (ch IS '}')) {
      if (brace_style < 0) brace_pos = pos;
      else {
         int style = SendScintilla(SCI_GETSTYLEAT,pos) & 0x1f;

         if (style IS brace_style) brace_pos = pos;
      }
   }

   return brace_pos;
}

/*********************************************************************************************************************
** Find a brace and it's match.  Return TRUE if the current position is inside a pair of braces.
*/

bool ScintillaKTK::findMatchingBrace(long &brace,long &other,long mode)
{
   int brace_style;

/*   if (lexLanguage IS SCLEX_CPP) brace_style = '{';
   else*/ brace_style = -1;

   brace = -1;
   other = -1;

   long caretPos = SendScintilla(SCI_GETCURRENTPOS);

   if (caretPos > 0) brace = checkBrace(caretPos - 1,brace_style);

   bool isInside = FALSE;

   if (brace < 0 and mode) {
      brace = checkBrace(caretPos,brace_style);
      if (brace >= 0) isInside = TRUE;
   }

   if (brace >= 0) {
      other = SendScintilla(SCI_BRACEMATCH,brace);
      if (other > brace) isInside = !isInside;
   }

   return isInside;
}

// Move to the matching brace.
void ScintillaKTK::moveToMatchingBrace()
{
   gotoMatchingBrace(FALSE);
}

// Select to the matching brace.
void ScintillaKTK::selectToMatchingBrace()
{
   gotoMatchingBrace(TRUE);
}

// Move to the matching brace and optionally select the text.
void ScintillaKTK::gotoMatchingBrace(bool select)
{
   long braceAtCaret;
   long braceOpposite;

   bool isInside = findMatchingBrace(braceAtCaret,braceOpposite,TRUE);

   if (braceOpposite >= 0) {
      // Convert the character positions into caret positions based
      // on whether the caret position was inside or outside the
      // braces.
      if (isInside) {
         if (braceOpposite > braceAtCaret) braceAtCaret++;
         else braceOpposite++;
      }
      else {
         if (braceOpposite > braceAtCaret) braceOpposite++;
         else braceAtCaret++;
      }

      //ensureLineVisible(SendScintilla(SCI_LINEFROMPOSITION,braceOpposite));

      if (select) SendScintilla(SCI_SETSEL,braceAtCaret,braceOpposite);
      else SendScintilla(SCI_SETSEL,braceOpposite,braceOpposite);
   }
}
