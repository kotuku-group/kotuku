
// Style codes for each lexer are defined in SciLexer.h

#include <format>

#include "SciLexer.h"
#include "lexer_styles.cxx"
#include "lexer_keywords.cxx"

/*********************************************************************************************************************
** This is the main entry point, we're called from the Init action here.
*/

ScintillaKTK::ScintillaKTK(extScintilla *Scintilla)
:  scintilla(Scintilla)
{
   idle_on        = false;
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

   SendScintilla(SCI_SETMARGINTYPEN, 1UL, sptr_t(SC_MARGIN_SYMBOL));
   SendScintilla(SCI_SETMARGINMASKN, 1UL, sptr_t(~SC_MASK_FOLDERS));
   SendScintilla(SCI_SETMARGINTYPEN, 2UL, sptr_t(SC_MARGIN_SYMBOL));
   SendScintilla(SCI_SETMARGINMASKN, 2UL, sptr_t(SC_MASK_FOLDERS));
   SendScintilla(SCI_SETMARGINSENSITIVEN, 2UL, 1L);
   SendScintilla(SCI_SETFOLDFLAGS, (uptr_t)SC_FOLDFLAG_LINEAFTER_CONTRACTED);
   DefineFoldMarkers();

   // We're always UTF-8

   SendScintilla(SCI_SETCODEPAGE, SC_CP_UTF8);

   // Call tips are drawn with STYLE_CALLTIP rather than fixed colours.  Tabs are not expanded.

   SendScintilla(SCI_CALLTIPUSESTYLE, 0UL);
}

ScintillaKTK::~ScintillaKTK()
{
   Finalise();
}

void ScintillaKTK::InsertCharacter(std::string_view Text, Scintilla::CharacterSource Source)
{
   if (panCheckLength(Text.size()) IS ERR::Okay) ScintillaBase::InsertCharacter(Text, Source);
   else errorStatus = Scintilla::Status::Failure;
}

//********************************************************************************************************************

void ScintillaKTK::Finalise()
{
   if (finalised) return;
   finalised = true;
   idle_on = false;
   for (auto &ticker : tickers) ticker = {};
   if (scintilla->TickTimer) { UpdateTimer(scintilla->TickTimer, 0); scintilla->TickTimer = nullptr; }
   if (scintilla->IdleTimer) { UpdateTimer(scintilla->IdleTimer, 0); scintilla->IdleTimer = nullptr; }
   completion_reports.clear();
   scintilla->Drop.active = false;
   scintilla->PrimaryDrop.active = false;
   ScintillaBase::Finalise();
   calltip_popup.reset();
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
            return std::toupper((unsigned char)X) < std::toupper((unsigned char)Y);
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

   SendScintilla(SCI_AUTOCSETSEPARATOR, (uptr_t)'\n');
   SendScintilla(SCI_AUTOCSETTYPESEPARATOR, 0UL);
   SendScintilla(SCI_AUTOCSETIGNORECASE, (uptr_t)ignore_case);

   // The engine's chooseSingle shortcut bypasses acceptance notifications and dropRestOfWord, and leaves an
   // existing list open.  Start the normal list and accept its matching candidate before it can be painted.

   SendScintilla(SCI_AUTOCSETCHOOSESINGLE, 0UL);
   SendScintilla(SCI_AUTOCSETDROPRESTOFWORD, (uptr_t)((Flags & SCACF::DROP_REST_OF_WORD) != SCACF::NIL));
   SendScintilla(SCI_AUTOCSHOW, (uptr_t)Length, joined.c_str());
   if ((list.size() IS 1) and ((Flags & SCACF::CHOOSE_SINGLE) != SCACF::NIL) and (ac.Active())) {
      AutoCompleteCompleted(0, Scintilla::CompletionMethods::Command);
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
   SendScintilla(SCI_CALLTIPSHOW, (uptr_t)Pos, text.c_str());
   if ((HighlightStart >= 0) and (HighlightEnd > HighlightStart)) {
      SendScintilla(SCI_CALLTIPSETHLT, (uptr_t)HighlightStart, sptr_t(HighlightEnd));
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

void ScintillaKTK::panCallTipClick(Scintilla::Internal::Point Point)
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
   if (scintilla->collecting() or not scintilla->CompletionCallback.defined()) return;

   if (Deferred) {
      completion_reports.push_back({ Event, Position, Value, Text ? Text : "" });
      QueueAction(sci::ReportEvent::id, scintilla->UID);
      return;
   }

   if (scintilla->CompletionCallback.stale()) {
      scintilla->CompletionCallback.unpin();
      scintilla->CompletionCallback.clear();
      return;
   }

   // The client may replace the field while this callback is running.
   auto callback = scintilla->CompletionCallback;
   callback.pin();
   const auto unpin = kt::Defer([&]() { callback.unpin(); });

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
// A callback may pump messages and enqueue more reports.  Finish the original batch before delivering those reports.

bool ScintillaKTK::panFlushCompletionReports()
{
   if (flushing_reports or scintilla->collecting()) return false;
   if (completion_reports.empty()) return true;

   flushing_reports = true;
   const auto reset = kt::Defer([&]() { flushing_reports = false; });

   auto reports = std::move(completion_reports);
   completion_reports.clear();
   for (auto &report : reports) {
      if (scintilla->collecting()) return false;
      ReportCompletion(report.event, report.position, report.value, report.text.c_str(), false);
   }
   if (scintilla->collecting()) return false;
   // A nested ReportEvent may have consumed the action queued by the callback.
   if (not completion_reports.empty()) QueueAction(sci::ReportEvent::id, scintilla->UID);
   return true;
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
** the text area horizontally.
**
** The result is always false.  Scintilla repaints the entire client when the result is true, because a native
** scrollbar can change the client area.  The Page is external to the client area, so a change to its extent does not
** require a repaint: the edits that change the extent invalidate the affected areas themselves.  This avoids a full
** repaint for every character typed at the end of the widest line, and for every line added.
*/

bool ScintillaKTK::ModifyScrollBars(Sci::Line Max, Sci::Line Page)
{
   kt::Log log(__FUNCTION__);

   int height = (Max - Page + 1) * vs.lineHeight + scintilla->ViewHeight;
   int width  = scintilla->ViewWidth;
   if (vs.wrap.state IS Scintilla::Wrap::None) {
      width = std::max(width, scrollWidth + vs.fixedColumnWidth + vs.rightMarginWidth);
   }

   if ((width IS scintilla->PageWidth) and (height IS scintilla->PageHeight) and
       (scintilla->Scale IS scintilla->PageScale)) return false;

   log.traceBranch("Lines: %td/%td, Page: %dx%d", Max, Page, width, height);
   DwellEnd(true);

   scintilla->PageWidth  = width;
   scintilla->PageHeight = height;
   scintilla->PageScale  = scintilla->Scale;

   if (scintilla->Page) { // The Page is sized in View units
      scintilla->Page->setWidth(width / scintilla->Scale);
      scintilla->Page->setHeight(height / scintilla->Scale);
   }

   return false;
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

ERR ScintillaKTK::copy_to_clipboard(const Scintilla::Internal::SelectionText &Text)
{
   kt::Log log(__FUNCTION__);

   if (Text.Empty()) return ERR::Okay;

   auto text = Text.AsView();
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
   if (size > KotukuScintilla::document_limit) return ERR::OutOfRange;

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

   if (convertPastes) content = Scintilla::Internal::Document::TransformLineEnds(content, pdoc->eolMode);
   if (panCheckLength(content.size()) != ERR::Okay) return ERR::OutOfRange;
   log.traceBranch("%d bytes, rectangular: %c", int(content.size()), rectangular ? 'Y' : 'N');

   {
      Scintilla::Internal::UndoGroup ug(pdoc);
      ClearSelection(multiPasteMode IS Scintilla::MultiPaste::Each);
      auto start = sel.IsRectangular() ? sel.Rectangular().Start() : sel.Range(sel.Main()).Start();

      if (rectangular) PasteRectangular(start, content);
      else InsertPaste(content);
   }

   calc_longest_line(scintilla);
   Redraw();
   return ERR::Okay;
}

//********************************************************************************************************************
// Called by Scintilla for copy operations that do not go through Copy(), e.g. SCI_COPYRANGE and SCI_COPYTEXT.

void ScintillaKTK::CopyToClipboard(const Scintilla::Internal::SelectionText &Text)
{
   clip_error = copy_to_clipboard(Text);
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

   Scintilla::Internal::SelectionText text;
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

   Scintilla::Internal::SelectionText text;
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
   primarySelection = not SelectionEmpty();
   if (primarySelection) {
      Scintilla::Internal::SelectionText text;
      CopySelectionRange(&text);
      if (not text.Empty()) gfx::SetPrimarySelection(text.AsView());
   }
}

//********************************************************************************************************************

void ScintillaKTK::NotifyChange()
{
   // This method is useless because Scintilla immediately follows this
   // up with the SCN_MODIFIED message, which carries a lot more detail.
}

//********************************************************************************************************************
// Sometimes Scintilla will report events that have occurred in the text editor.

void ScintillaKTK::NotifyParent(Scintilla::NotificationData Notification)
{
   const auto &scn = Notification;
   kt::Log log("SciMsg");
   int position, length, line, lines_added;
   if (not KotukuScintilla::PublicInteger(scn.position, position) or
       not KotukuScintilla::PublicInteger(scn.length, length) or
       not KotukuScintilla::PublicInteger(scn.line, line) or
       not KotukuScintilla::PublicInteger(scn.linesAdded, lines_added)) {
      log.warning(ERR::OutOfRange);
      return;
   }
   const auto code = scn.nmhdr.code;
   if (int(code) IS 0) return;

   if (code IS Scintilla::Notification::UpdateUI) {
      // Either the text or styling of the document has changed or the selection range has changed. Now would be a good
      // time to update any container UI elements that depend on document or view state.  This was previously called
      // SCN_CHECKBRACE because a common use is to check whether the caret is next to a brace and set highlights on
      // this brace and its corresponding matching brace.

      log.traceBranch("[UPDATEUI] $%x", int(scn.updated));

      // Update the cursor-row and cursor-column fields if they've changed.

      int pos = SendScintilla(SCI_GETCURRENTPOS);
      if (pos != oldpos) {
         oldpos = pos;
         int row, column;
         if (not KotukuScintilla::PublicInteger(SendScintilla(SCI_LINEFROMPOSITION, pos), row) or
             not KotukuScintilla::PublicInteger(SendScintilla(SCI_GETCOLUMN, pos), column)) {
            log.warning(ERR::OutOfRange);
            return;
         }
         scintilla->CursorRow = row;
         scintilla->CursorCol = column;
         if (scintilla->Lexer IS SCLEX::CPP) braceMatch();

         // Event report has to be delayed, as we otherwise get interference in the drawing process.
         scintilla->ReportEventFlags |= SEF::CURSOR_POS;
         QueueAction(sci::ReportEvent::id, scintilla->UID);
      }
   }
   else if (code IS Scintilla::Notification::StyleNeeded) {
      // If you used SCLEX_CONTAINER to make the container act as the lexer, you will receive this notification when
      // Scintilla is about to display or print text that requires styling. You are required to style the text from the
      // line that contains the position returned by SCI_GETENDSTYLED up to the position passed in
      // SCNotification.position.

      log.trace("[STYLENEEDED]");

      //MyStyleRoutine(start, position);

   }
   else if (code IS Scintilla::Notification::DoubleClick) {
      // Mouse buttons have been interpreted as a double click

      log.trace("[DOUBLECLICK]");
   }
   else if (code IS Scintilla::Notification::ModifyAttemptRO) {
      // An attempt has been made to modify the document when in read-only mode

      log.trace("[MODIFYATTEMPTRO]");

      scintilla->ReportEventFlags |= SEF::FAIL_RO;
      QueueAction(sci::ReportEvent::id, scintilla->UID);
   }
   else if (code IS Scintilla::Notification::CharAdded) {
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

               SendScintilla(SCI_SETSEL, uptr_t(-1), pos);
            }
         }
      }

      scintilla->ReportEventFlags |= SEF::NEW_CHAR;
      QueueAction(sci::ReportEvent::id, scintilla->UID);

      ReportCompletion(SCAC::CHAR_ADDED, SendScintilla(SCI_GETCURRENTPOS), scn.ch, nullptr, false);
   }
   else if (code IS Scintilla::Notification::SavePointReached) {
      // The document is unmodified (recently saved)

      log.trace("[SAVEPOINTREACHED]");
   }
   else if (code IS Scintilla::Notification::SavePointLeft) {
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
   else if (code IS Scintilla::Notification::Key) {
      // Reports all keys pressed but not consumed by Scintilla

      log.trace("[KEY]");
   }
   else if (code IS Scintilla::Notification::Modified) {
      // This notification is sent when the text or styling of the document changes or is about to change. You can set
      // a mask for the notifications that are sent to the container with SCI_SETMODEVENTMASK. The notification
      // structure contains information about what changed, how the change occurred and whether this changed the number
      // of lines in the document. No modifications may be performed while in a SCN_MODIFIED event.
      //
      // See HTML documentation for more information.

      log.trace("[MODIFIED] Type: %d, Length: %d, LinesAdded: %d, Line: %d",
         int(scn.modificationType), length, lines_added, line);

      if (int(scn.modificationType) & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
         scintilla->DocumentRevision++;
         track_modified_lines(scintilla, SendScintilla(SCI_LINEFROMPOSITION, position), lines_added);
      }
      if (int(scn.modificationType) & SC_MOD_CHANGEFOLD) {
         FoldChanged(line, int(scn.foldLevelNow), int(scn.foldLevelPrev));
      }
   }
   else if (code IS Scintilla::Notification::FocusIn) {
      log.trace("[SETFOCUS]");
   }
   else if (code IS Scintilla::Notification::FocusOut) {
      log.trace("[KILLFOCUS]");
   }
   else if (code IS Scintilla::Notification::MacroRecord) {
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
   else if (code IS Scintilla::Notification::MarginClick) {
      // The mouse was clicked inside a sensitive margin.  The position is the start of the clicked line.  A click in
      // the fold margin toggles the fold of a header line, and every click is then reported to the client.

      int line = pdoc->LineFromPosition(position);
      log.traceBranch("[MARGINCLICK] Margin: %d, Line: %d", scn.margin, line);

      if ((scn.margin IS 2) and (pdoc->GetLevel(line) & SC_FOLDLEVELHEADERFLAG)) {
         FoldLine(line, Scintilla::FoldAction::Toggle);
      }

      KQ qualifiers = KQ::NIL;
      if (int(scn.modifiers) & int(Scintilla::KeyMod::Shift)) qualifiers |= KQ::SHIFT;
      if (int(scn.modifiers) & int(Scintilla::KeyMod::Ctrl))  qualifiers |= KQ::CTRL;
      if (int(scn.modifiers) & int(Scintilla::KeyMod::Alt))   qualifiers |= KQ::ALT;
      report_margin_click(scintilla, SCMG(scn.margin), line, qualifiers);
   }
   else if (code IS Scintilla::Notification::NeedShown) {
      // Scintilla has determined that a range of lines that is currently invisible should be made visible. An example of where this may be
      // needed is if the end of line of a contracted fold point is deleted. This message is sent to the container in case it wants to make the
      // line visible in some unusual way such as making the whole document visible. Most containers will just ensure each line in the range is
      // visible by calling SCI_ENSUREVISIBLE. The position and length fields of SCNotification indicate the range of the document that should be
      // made visible.

      // The range is inclusive of the line at position + length, as an insertion has a length of zero.

      int first = SendScintilla(SCI_LINEFROMPOSITION, position);
      int last = SendScintilla(SCI_LINEFROMPOSITION, position + length);

      log.trace("[NEEDSHOWN] First: %d, Last: %d", first, last);

      for (int line=first; line <= last; line++) SendScintilla(SCI_ENSUREVISIBLE, line);
   }
   else if (code IS Scintilla::Notification::Painted) {
      // Painting has just been done. Useful when you want to update some other widgets based on a change in Scintilla,
      // but want to have the paint occur first to appear more responsive. There is no other information in SCNotification.

      log.trace("[PAINTED]");
   }
   else if (code IS Scintilla::Notification::UserListSelection) {
      // The user has selected an item in a user list. The SCNotification fields used are:
      // Field   Usage
      // wParam  This is set to the listType parameter from the SCI_USERLISTSHOW message that initiated the list.
      // text    The text of the selection.

      log.trace("[USERLISTSELECTION]");
   }
   else if (code IS Scintilla::Notification::DwellStart) {
      // Generated when the user keeps the mouse in one position for the dwell period (see SCI_SETMOUSEDWELLTIME).
      //
      // position: This is the nearest position in the document to the position where the mouse pointer was lingering.
      // x, y: Where the pointer lingered. The position field is set to SCI_POSITIONFROMPOINTCLOSE(x, y).

      log.trace("[DWELLSTART]");
   }
   else if (code IS Scintilla::Notification::DwellEnd) {
      // Generated after a SCN_DWELLSTART and the mouse is moved or other activity such as key press indicates the dwell is over.

      log.trace("[DWELLEND]");
   }
   else if (code IS Scintilla::Notification::Zoom) {
      // Unsupported/Redundant Scintilla feature
   }
   else if (code IS Scintilla::Notification::HotSpotClick) {
      log.trace("[HOTSPOTCLICK]");

   }
   else if (code IS Scintilla::Notification::HotSpotDoubleClick) {
      // Generated when the user clicks or double clicks on text that is in a style with the hotspot attribute set. This notification can be
      // used to link to variable definitions or web pages. The position field is set the text position of the click or double click and
      // the modifiers field set to the key modifiers held down in a similar manner to SCN_KEY.

      log.trace("[HOTSPOTDOUBLECLICK]");
   }
   else if (code IS Scintilla::Notification::CallTipClick) {
      // Generated when the user clicks on a calltip. This notification can be used to display the next function prototype when a function name
      // is overloaded with different arguments. The position field is set to 1 if the click is in an up arrow, 2 if in a down arrow, and 0 if
      // elsewhere.

      log.trace("[CALLTIPCLICK]");

      ReportCompletion(SCAC::CALL_TIP_CLICK, calltip_pos, position, nullptr, true);
   }
   else if (code IS Scintilla::Notification::AutoCSelection) {
      // The user has selected an item in an autocompletion list. The notification is sent before the selection is inserted. Automatic
      // insertion can be cancelled by sending a SCI_AUTOCCANCEL message before returning from the notification. The SCNotification fields
      // used are:
      //
      // lParam: The start position of the word being completed.
      // text: The text of the selection.

      log.trace("[AUTOCSELECTION]");

      completion_selected = true;
      ReportCompletion(SCAC::SELECTED, position, 0, scn.text, true);
   }
   else if (code IS Scintilla::Notification::AutoCCancelled) {
      // Not every cancellation is notified, so they are reported by panCompletionClosed() instead.
   }
   else if (code IS Scintilla::Notification::AutoCCharDeleted) {
      // A character was deleted while the completion list is open, which updates the selected item.
   }
   else if ((code IS Scintilla::Notification::AutoCCompleted) or
            (code IS Scintilla::Notification::AutoCSelectionChange)) {
      // Selection is reported once from AutoCSelection.  Highlight changes have no public callback equivalent.
   }
   else log.traceWarning("Notification code %d unsupported.", int(code));
}

//********************************************************************************************************************

/*********************************************************************************************************************
** Scintilla calls ScrollText() for vertical scrolls of up to ten lines, after topLine has changed.  The Bitmap content
** is shifted by the scrolled distance and only the exposed band is invalidated.  The caret, margins and selection
** scroll with the text because they are document-relative.
**
** The shift is only safe if the Bitmap reflects the previous topLine, which is tracked by painted_top.  A pending
** Dirty area may have been recorded in either the old or the new client coordinates (Scintilla can invalidate between
** changing topLine and calling ScrollText()), so both the original and the shifted areas remain dirty.  A full repaint
** is used if the Bitmap is out of step, the editor is disabled (it is always painted in full) or most of the View
** would be exposed anyway.
*/

void ScintillaKTK::ScrollText(Sci::Line LinesToMove)
{
   auto bmp = scintilla->Bitmap;
   const int dy = LinesToMove * vs.lineHeight;
   const int height = scintilla->ViewHeight;
   auto &dirty = scintilla->Dirty;
   const bool dirty_empty = (dirty.Right <= dirty.Left) or (dirty.Bottom <= dirty.Top);

   if ((not bmp) or (not bmp->Data) or (dy IS 0) or (std::abs(dy) * 2 >= height) or
       (painted_top != topLine + LinesToMove) or ((scintilla->Flags & SCIF::DISABLED) != SCIF::NIL) or
       ((not dirty_empty) and (dirty.Top <= 0) and (dirty.Bottom >= height))) {
      painted_top = -1;
      Redraw();
      return;
   }

   kt::Log(__FUNCTION__).trace("Lines: %td, Pixels: %d", LinesToMove, dy);

   // The Bitmap is one row taller than the View, and every row is shifted.

   const size_t stride = size_t(bmp->LineWidth);
   const int keep = bmp->Height - std::abs(dy);
   if (dy > 0) memmove(bmp->Data + (size_t(dy) * stride), bmp->Data, size_t(keep) * stride);
   else memmove(bmp->Data, bmp->Data + (size_t(-dy) * stride), size_t(keep) * stride);
   painted_top = topLine;

   // The exposed band is cleared so that a frame drawn before the repaint, e.g. after an external Page move that is
   // detected during scene rendering, shows the background rather than duplicated rows.

   auto &bkgd = scintilla->BkgdColour;
   gfx::DrawRectangle(bmp, 0, (dy > 0) ? 0 : keep, bmp->Width, std::abs(dy),
      bmp->packPixel(bkgd.Red, bkgd.Green, bkgd.Blue, 255), BAF::FILL);
   clear_bitmap_border(scintilla);

   if (not dirty_empty) {
      auto pending = dirty;
      invalidate_area(scintilla, pending.Left, pending.Top + dy, pending.Right, pending.Bottom + dy);
   }

   // The exposed band includes the row below the View, which is only painted as part of a partially visible line.

   if (dy > 0) invalidate_area(scintilla, 0, 0, scintilla->ViewWidth, dy);
   else invalidate_area(scintilla, 0, height + dy - 1, scintilla->ViewWidth, height);
}

// Each engine tick reason has its own deadline; the shared framework subscription polls them every 10 ms.

bool ScintillaKTK::FineTickerRunning(TickReason Reason)
{
   return tickers[size_t(Reason)].milliseconds > 0;
}

void ScintillaKTK::FineTickerStart(TickReason Reason, int Milliseconds, int)
{
   if (finalised) return;
   auto &ticker = tickers[size_t(Reason)];
   ticker.milliseconds = std::max(1, Milliseconds);
   ticker.deadline = PreciseTime() + int64_t(ticker.milliseconds) * 1000;
   UpdateTimers();
}

void ScintillaKTK::FineTickerCancel(TickReason Reason)
{
   tickers[size_t(Reason)] = {};
}

/*********************************************************************************************************************
** Scintilla requests idle processing when it has background work to do, such as wrapping lines.  Returning true
** confirms that Idle() will be called until it reports that the work is complete.
*/

bool ScintillaKTK::SetIdle(bool On)
{
   idle_on = On and not finalised;
   if (idle_on) UpdateTimers();
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
   if (finalised) return;
   kt::SwitchContext ctx(scintilla);

   if ((TickRequired()) and (not scintilla->TickTimer)) {
      // TickFor() dispatches only reasons whose independent deadlines have elapsed.
      SubscribeTimer(0.01, C_FUNCTION(tick_timer), &scintilla->TickTimer);
   }

   if ((idle_on) and (not scintilla->IdleTimer)) {
      SubscribeTimer(IDLE_INTERVAL, C_FUNCTION(idle_timer), &scintilla->IdleTimer);
   }
}

// Stop the shared subscription when no fine-ticker reason remains active.

bool ScintillaKTK::TickRequired()
{
   return not finalised and std::any_of(tickers.begin(), tickers.end(),
      [](const auto &Ticker) { return Ticker.milliseconds > 0; });
}

//********************************************************************************************************************
// Called by tick_timer().  Returns false if the timer is no longer required.

bool ScintillaKTK::panTick()
{
   if (not TickRequired()) return false;
   const auto now = PreciseTime();
   for (size_t i = 0; i < tickers.size(); i++) {
      auto &ticker = tickers[i];
      if ((ticker.milliseconds > 0) and (now >= ticker.deadline)) {
         ticker.deadline = now + int64_t(ticker.milliseconds) * 1000;
         TickFor(TickReason(i));
      }
   }
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
// Scintilla shows the arrow over selected text and the reverse arrow over margins.

void ScintillaKTK::DisplayCursor(Scintilla::Internal::Window::Cursor Cursor)
{
   ScintillaBase::DisplayCursor(Cursor);
}

//********************************************************************************************************************

bool ScintillaKTK::HaveMouseCapture()
{
   return captured_mouse;
}

//********************************************************************************************************************

sptr_t ScintillaKTK::WndProc(Scintilla::Message Message, uptr_t WParam, sptr_t LParam)
{
   // The public framework contract retains int byte offsets.  Validate incoming lengths before the engine
   // can grow a document beyond that contract; pointer payloads and signed sentinels stay pointer-sized.
   size_t added = 0;
   Sci::Position removed = 0;
   bool changes_length = true;
   switch (Message) {
      case Scintilla::Message::SetText:
         removed = pdoc->Length();
         added = LParam ? std::strlen((const char *)LParam) : 0;
         break;
      case Scintilla::Message::InsertText:
      case Scintilla::Message::ReplaceSel:
         added = LParam ? std::strlen((const char *)LParam) : 0;
         if (Message IS Scintilla::Message::ReplaceSel) removed = sel.Length();
         break;
      case Scintilla::Message::AddText:
      case Scintilla::Message::AppendText:
         added = WParam;
         break;
      case Scintilla::Message::ReplaceTarget:
      case Scintilla::Message::ReplaceTargetRE:
         if (not LParam) return -1;
         added = (WParam IS uptr_t(-1)) ? std::strlen((const char *)LParam) : WParam;
         removed = std::abs(targetRange.end.Position() - targetRange.start.Position());
         if (Message IS Scintilla::Message::ReplaceTargetRE) {
            if (added > size_t(KotukuScintilla::document_limit)) return -1;
            Sci::Position length = Sci::Position(added);
            const auto expanded = pdoc->SubstituteByPosition((const char *)LParam, &length);
            if (not expanded) return 0;
            if ((length < 0) or (panCheckLength(size_t(length), removed) != ERR::Okay)) {
               errorStatus = Scintilla::Status::Failure;
               return -1;
            }
            const std::string text(expanded, size_t(length));
            return WndProc(Scintilla::Message::ReplaceTarget, text.size(), (sptr_t)text.data());
         }
         break;
      default: changes_length = false; break;
   }
   if (changes_length and (panCheckLength(added, removed) != ERR::Okay)) {
      errorStatus = Scintilla::Status::Failure;
      return -1;
   }

   switch (Message) {

   case Scintilla::Message::GetDirectFunction:
      return reinterpret_cast<sptr_t>(DirectFunction);

   case Scintilla::Message::GetDirectPointer:
      return reinterpret_cast<sptr_t>(this);

   case Scintilla::Message::Cut: // Editor::Cut() is not virtual
      if (recordingMacro) NotifyMacroRecord(Message, WParam, LParam);
      clip_error = cut_clipboard();
      SetLastXChosen();
      return 0;

   case Scintilla::Message::LineCut: { // Cut the lines that the selection touches, as Editor::KeyCommand() does
      if (recordingMacro) NotifyMacroRecord(Message, WParam, LParam);
      int start = pdoc->LineStart(pdoc->LineFromPosition(SelectionStart().Position()));
      int end = pdoc->LineStart(pdoc->LineFromPosition(SelectionEnd().Position()) + 1);
      SetSelection(start, end);
      clip_error = cut_clipboard();
      SetLastXChosen();
      return 0;
   }

   default:
      return ScintillaBase::WndProc(Message, WParam, LParam);
   }
   return 0;
}

//********************************************************************************************************************

sptr_t ScintillaKTK::DirectFunction(sptr_t Context, unsigned int Message, uptr_t WParam, sptr_t LParam)
{
   auto editor = (ScintillaKTK *)Context;
   // Direct calls bypass Action(), so retain the owner until engine notifications and callbacks have returned.
   kt::ScopedObjectLock owner(editor->scintilla);
   if (not owner.granted()) return 0;
   return editor->WndProc(Scintilla::Message(Message), WParam, LParam);
}

/*********************************************************************************************************************
** Do nothing; this is a Win32 support function.
*/

sptr_t ScintillaKTK::DefWndProc(Scintilla::Message, uptr_t, sptr_t)
{
   return 0;
}

/*********************************************************************************************************************
** Paint an Area of the client into the Scintilla object's Bitmap.  Refer to pan_surface.cxx for the drawing routines.
** If Scintilla abandons the paint because a wider area needs to be redrawn (for instance, styling has spilled over a
** line end) then the entire client is repainted immediately, as the GTK platform does.
*/

void ScintillaKTK::panPaint(Scintilla::Internal::PRectangle Area)
{
   kt::Log log(__FUNCTION__);

   if (this->paintState != PaintState::notPainting) return; // Recursion prevention
   if (not scintilla->Bitmap) return;

   auto rcClient = GetClientRectangle();

   log.traceBranch("Area: %.2fx%.2f - %.2fx%.2f", Area.left, Area.top, Area.Width(), Area.Height());

   auto surface = Scintilla::Internal::Surface::Allocate(Scintilla::Technology::Default);
   if (not surface) return;
   surface->Init(scintilla->Bitmap, wMain.GetID());

   this->paintState = PaintState::painting;
   this->rcPaint    = Area;
   this->paintingAllText = rcPaint.Contains(rcClient);
   this->Paint(surface.get(), rcPaint);

   if (this->paintState IS PaintState::abandoned) {
      this->paintState = PaintState::painting;
      this->rcPaint    = rcClient;
      this->paintingAllText = true;
      this->Paint(surface.get(), rcPaint);
   }

   this->paintState = PaintState::notPainting;
   painted_top = topLine;

   // Painted lines that are wider than the scroll width widen the Page, e.g. a proportional face can rank a line
   // below a narrower one in calc_longest_line().

   if ((vs.wrap.state IS Scintilla::Wrap::None) and (view.lineWidthMaxSeen > 0)) {
      grow_scroll_width(scintilla, view.lineWidthMaxSeen, scintilla->LongestLine);
   }
}

//********************************************************************************************************************
// Invalidate the entire client area.

void ScintillaKTK::panRedraw()
{
   Redraw();
}

/*********************************************************************************************************************
** Called on initialisation and whenever the face, size or device scale changes.  All styles share the public
** family and pixel size; weight and italic attributes remain specific to each style.
*/

void ScintillaKTK::panFontChanged()
{
   kt::Log log(__FUNCTION__);
   log.traceBranch();

   const int size = int(std::lround(scintilla->FontSize * scintilla->Scale * 72.0 * SC_FONT_SIZE_MULTIPLIER / 96.0));
   for (size_t style = 0; style < vs.styles.size(); style++) {
      SendScintilla(SCI_STYLESETFONT, style, scintilla->Face.c_str());
      SendScintilla(SCI_STYLESETSIZEFRACTIONAL, style, size);
   }
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

   const auto modifiers = Scintilla::KeyMod((shift ? 1 : 0) | (ctrl ? 2 : 0) | (alt ? 4 : 0));
   const bool read_only = (scintilla->Flags & SCIF::EDIT) IS SCIF::NIL;

   // Ctrl+Space requests completion candidates from the client.  It has no binding in Scintilla's key map.

   if ((ctrl) and (not alt) and (not shift) and (Code IS KEY::SPACE)) {
      if (not read_only) ReportCompletion(SCAC::REQUEST, SendScintilla(SCI_GETCURRENTPOS), 0, nullptr, false);
      return;
   }

   if (read_only) SendScintilla(SCI_SETREADONLY, 1UL);

   if (int key = command_key(Code, printable)) KeyDownWithModifiers(Scintilla::Keys(key), modifiers, nullptr);
   else if ((printable) and ((altgr) or ((not ctrl) and (not alt)))) {
      char utf8[8];
      int len = UTF8WriteValue(Unicode, std::span<int8_t>((int8_t *)utf8, sizeof(utf8)));
      if (len > 0) InsertCharacter(std::string_view(utf8, size_t(len)), Scintilla::CharacterSource::DirectInput);
   }
   else if (int key = shortcut_key(Code)) KeyDownWithModifiers(Scintilla::Keys(key), modifiers, nullptr);

   if (read_only) SendScintilla(SCI_SETREADONLY, 0UL);
}

/*********************************************************************************************************************
** Process a mouse button press at a client position.  The Time is in milliseconds and allows Scintilla to detect
** double and triple clicks.  Modifier keys are taken from the state that key_event() tracks: Shift extends the
** selection, and Alt starts a rectangular selection.
*/

void ScintillaKTK::panMousePress(JET Button, double X, double Y, unsigned int Time)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("%.0fx%.0f", X, Y);

   if (Button IS JET::LMB) {
      Scintilla::Internal::Point point(int(std::floor(X)), int(std::floor(Y)));

      ButtonDownWithModifiers(point, Time, Scintilla::KeyMod((scintilla->KeyShift ? 1 : 0) |
         (scintilla->KeyCtrl ? 2 : 0) | (scintilla->KeyAlt ? 4 : 0)));
   }
   else if (Button IS JET::MMB) panPrimaryPaste(X, Y);
}

//********************************************************************************************************************

void ScintillaKTK::panMouseMove(double X, double Y)
{
   ButtonMoveWithModifiers(Scintilla::Internal::Point(X, Y), (unsigned int)(PreciseTime() / 1000),
      Scintilla::KeyMod((scintilla->KeyShift ? 1 : 0) | (scintilla->KeyCtrl ? 2 : 0) | (scintilla->KeyAlt ? 4 : 0)));
}

//********************************************************************************************************************

void ScintillaKTK::panMouseRelease(JET Button, double X, double Y, unsigned int Time)
{
   kt::Log log(__FUNCTION__);
   log.trace("%.0fx%.0f", X, Y);

   if ((Button IS JET::LMB) and (not drag_active)) {
      ButtonUpWithModifiers(Scintilla::Internal::Point(X, Y), Time,
         Scintilla::KeyMod((scintilla->KeyShift ? 1 : 0) | (scintilla->KeyCtrl ? 2 : 0) | (scintilla->KeyAlt ? 4 : 0)));
   }
}

//********************************************************************************************************************
// Abandon a mouse drag, e.g. when the editor is disabled while a button is held.  The selection is left as it is.

void ScintillaKTK::panReleaseMouse()
{
   if (drag_active) {
      if (auto pointer = gfx::AccessPointer()) { acDeactivate(pointer); ReleaseObject(pointer); }
      panEndDrag();
   }
   ChangeMouseCapture(false); // Cancel the scroll reason as well as the platform capture state.
}

//********************************************************************************************************************

void ScintillaKTK::panResized()
{
   kt::Log log(__FUNCTION__);
   log.traceBranch();
   ChangeSize();
}

/*********************************************************************************************************************
** Called after the device scale has changed by Ratio, and the fonts and metrics have been reloaded.  The top line is
** retained, and the horizontal scroll position is converted to the new client units.  The content of the Bitmap no
** longer reflects any line, so the next paint is in full.
*/

void ScintillaKTK::panRescaled(double Ratio)
{
   kt::Log log(__FUNCTION__);
   log.traceBranch("Ratio: %.3f", Ratio);

   painted_top = -1;
   ChangeSize();

   scintilla->ScrollLocked++;
   HorizontalScrollTo(int(std::lround(xOffset * Ratio)));
   scintilla->ScrollLocked--;

   panSyncPage();
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
   if (not Focus) {
      // A native drag no longer owns mouse capture and must survive focus moving to its destination.
      if (HaveMouseCapture()) panReleaseMouse();
      DwellEnd(true);
   }
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

   FoldLine(Line, Scintilla::FoldAction::Toggle);
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
   bool changed = pcs->HiddenLines();
   for (int line=0; line < total; line++) {
      if (pcs->SetExpanded(line, true)) changed = true;
   }

   if (changed) {
      pcs->SetVisible(0, total - 1, true);
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
   if (pcs->GetExpanded(Line)) return;

   pcs->SetExpanded(Line, true);
   if (not pcs->GetVisible(Line)) return; // The lines are revealed when the enclosing fold is expanded

   int last = Line;
   for (int line=Line + 1; (line < pdoc->LinesTotal()) and (not pcs->GetVisible(line)); line++) {
      pcs->SetExpanded(line, true);
      last = line;
   }

   if (last > Line) {
      pcs->SetVisible(Line + 1, last, true);
      SetScrollBars();
      Redraw();
   }
}

/*********************************************************************************************************************
** Set the lexer.  To turn off the lexer, use SCLEX_NULL.  The fold levels of the previous lexer no longer apply, so all
** folds are expanded.  Scintilla does not pass its stored properties to a new lexer, so the 'fold' property is set
** again.
*/

ERR ScintillaKTK::SetLexer(SCLEX ID)
{
   auto profile = find_lexer_profile(ID);
   if (not profile) return ERR::NoSupport;

   // Create before touching the active document: factory failure preserves the lexer, folds and public field.
   auto lexer = CreateLexer(profile->Factory);
   if (not lexer) return ERR::Failed;

   ExpandAllFolds();
   // Document::LexInterface owns the instance and calls Release on replacement/destruction.  Even plain text uses
   // Lexilla's null lexer; a null pointer would request container styling instead.
   SendScintilla(SCI_SETILEXER, uptr_t(0), sptr_t(lexer));
   scintilla->Lexer = ID;
   SendScintilla(SCI_SETPROPERTY, "asp.default.language", ID IS SCLEX::ASP ? "2" : "1");
   SendScintilla(SCI_SETPROPERTY, "fold", scintilla->FoldingMarkers ? "1" : "0");
   SetKeywords();
   SetStyleColours();
   SendScintilla(SCI_CLEARDOCUMENTSTYLE);
   SendScintilla(SCI_COLOURISE, 0, -1);
   InvalidateStyleRedraw();
   return ERR::Okay;
}

/*********************************************************************************************************************
** Handle brace matching.
*/

void ScintillaKTK::braceMatch()
{
   sptr_t braceAtCaret, braceOpposite;

   findMatchingBrace(braceAtCaret, braceOpposite, braceMode);

   if (braceAtCaret >= 0 and braceOpposite < 0) {
      SendScintilla(SCI_BRACEBADLIGHT, braceAtCaret);
      SendScintilla(SCI_SETHIGHLIGHTGUIDE,0UL);
   }
   else {
      char chBrace = SendScintilla(SCI_GETCHARAT,braceAtCaret);

      SendScintilla(SCI_BRACEHIGHLIGHT,braceAtCaret,braceOpposite);

      sptr_t columnAtCaret = SendScintilla(SCI_GETCOLUMN,braceAtCaret);
      sptr_t columnOpposite = SendScintilla(SCI_GETCOLUMN,braceOpposite);

      if (chBrace IS ':') {
         sptr_t lineStart = SendScintilla(SCI_LINEFROMPOSITION,braceAtCaret);
         sptr_t indentPos = SendScintilla(SCI_GETLINEINDENTPOSITION,lineStart);
         sptr_t indentPosNext = SendScintilla(SCI_GETLINEINDENTPOSITION,lineStart + 1);
         columnAtCaret = SendScintilla(SCI_GETCOLUMN,indentPos);
         sptr_t columnAtCaretNext = SendScintilla(SCI_GETCOLUMN,indentPosNext);
         sptr_t indentSize = SendScintilla(SCI_GETINDENT);

         if (columnAtCaretNext - indentSize > 1) columnAtCaret = columnAtCaretNext - indentSize;
         if (columnOpposite IS 0) columnOpposite = columnAtCaret;
      }

      sptr_t column = columnAtCaret;

      if (column > columnOpposite) column = columnOpposite;

      SendScintilla(SCI_SETHIGHLIGHTGUIDE,column);
   }
}

/*********************************************************************************************************************
** Check if the character at a position is a brace.
*/

sptr_t ScintillaKTK::checkBrace(sptr_t pos,int brace_style)
{
   sptr_t brace_pos = -1;
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

bool ScintillaKTK::findMatchingBrace(sptr_t &brace,sptr_t &other,sptr_t mode)
{
   int brace_style;

/*   if (lexLanguage IS SCLEX_CPP) brace_style = '{';
   else*/ brace_style = -1;

   brace = -1;
   other = -1;

   sptr_t caretPos = SendScintilla(SCI_GETCURRENTPOS);

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
   sptr_t braceAtCaret;
   sptr_t braceOpposite;

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
