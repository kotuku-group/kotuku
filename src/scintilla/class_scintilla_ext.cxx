
// Style codes for each lexer are defined in SciLexer.h

#define COL_BLACK          0x000000
#define COL_DARKSLATEGREY  0x2F4F4F
#define COL_LIGHTSLATEGREY 0x576889
#define COL_GREY           0x808080
#define COL_LIGHTGREY      0xd3d3d3
#define COL_MIDNIGHTBLUE   0x191970
#define COL_NAVYBLUE       0x000080
#define COL_BLUE           0x0000ff
#define COL_CYAN           0x00ffff
#define COL_TURQUOISE      0x00ced1
#define COL_CADETBLUE      0x5f9ea0
#define COL_OLIVE          0x6b8e23
#define COL_KHAKI          0xBDB76B
#define COL_GOLD           0xFFD700
#define COL_INDIANRED      0xCD5C5C
#define COL_SADDLEBROWN    0x8B4513
#define COL_FIREBRICK      0xB22222
#define COL_BROWN          0xA52A2A
#define COL_FORESTGREEN    0x228B22
#define COL_BRIGHTRED      0xFF0000
#define COL_BRIGHTGREEN    0xFF0000
#define COL_DARKVIOLET     0x9400D3
#define COL_RED            0xb00000

static const struct styledef std_styles[] = {
   { STYLE_DEFAULT,    COL_BLACK, FTF::NIL },
   { STYLE_LINENUMBER, COL_BLACK, FTF::NIL },
   { STYLE_BRACELIGHT, COL_BRIGHTRED, FTF::BOLD },
   { STYLE_BRACEBAD,   COL_BRIGHTRED, FTF::BOLD|FTF::ITALIC }  // Somebody set us up the bomb
};

static const struct styledef c_styles[] = {
   { SCE_C_DEFAULT,                COL_BLACK, FTF::NIL },          // What you say?
   { SCE_C_COMMENT,                COL_GREY, FTF::NIL },           // Standard C comment
   { SCE_C_COMMENTLINE,            COL_GREY, FTF::NIL },           // // style comment
   { SCE_C_COMMENTDOC,             COL_LIGHTSLATEGREY, FTF::NIL }, // Double-star comments
   { SCE_C_NUMBER,                 COL_BLUE, FTF::NIL },           // Any number or float
   { SCE_C_WORD,                   COL_FIREBRICK, FTF::NIL },
   { SCE_C_STRING,                 COL_RED, FTF::NIL },          // Strings "..."
   { SCE_C_CHARACTER,              COL_RED, FTF::NIL },          // Characters ' '
   { SCE_C_UUID,                   COL_BRIGHTRED, FTF::NIL },    // \n, \r
   { SCE_C_PREPROCESSOR,           COL_FORESTGREEN, FTF::NIL },  // #include, #define etc
   { SCE_C_OPERATOR,               COL_BLACK, FTF::NIL },        // + - *
   { SCE_C_IDENTIFIER,             COL_BLACK, FTF::NIL },        // The default colour
   { SCE_C_STRINGEOL,              COL_BRIGHTRED, FTF::NIL },
   { SCE_C_VERBATIM,               COL_BRIGHTRED, FTF::NIL },
   { SCE_C_REGEX,                  COL_BLUE, FTF::NIL },
   { SCE_C_COMMENTLINEDOC,         COL_GREY, FTF::NIL }, // // style comment
   { SCE_C_WORD2,                  COL_BRIGHTRED, FTF::NIL },
   { SCE_C_COMMENTDOCKEYWORD,      COL_GREY, FTF::NIL },
   { SCE_C_COMMENTDOCKEYWORDERROR, COL_GREY, FTF::NIL },
   { SCE_C_GLOBALCLASS,            COL_RED, FTF::NIL }
};

void ScintillaKTK::SetStyles(const struct styledef *Def, int Total)
{
   pf::Log log("SetStyles");
   int i, index;

   log.branch("%d", Total);

   for (i=0; i < Total; i++) {
      index = Def[i].Index;
      WndProc(SCI_STYLESETFONT, index, (MAXINT)"courier"); // Font name, must be set to something
      WndProc(SCI_STYLESETSIZE, index, 10); // Default Size
      WndProc(SCI_STYLESETFORE, index, SCICOLOUR((uint8_t)(Def[i].Colour>>16), (uint8_t)(Def[i].Colour>>8), Def[i].Colour));

      if ((index IS STYLE_BRACELIGHT) or (index IS STYLE_BRACEBAD)) {
         WndProc(SCI_STYLESETBACK, index, SCICOLOUR(255, 255, 200));
      }

      if ((Def[i].FontStyle & FTF::BOLD) != FTF::NIL) WndProc(SCI_STYLESETBOLD, index, 1);
      if ((Def[i].FontStyle & FTF::ITALIC) != FTF::NIL) WndProc(SCI_STYLESETITALIC, index, 1);
   }

   WndProc(SCI_STYLESETBACK, STYLE_DEFAULT, (long int)SCICOLOUR(scintilla->BkgdColour.Red, scintilla->BkgdColour.Green, scintilla->BkgdColour.Blue));
   WndProc(SCI_STYLESETFORE, STYLE_DEFAULT, (long int)SCICOLOUR(scintilla->TextColour.Red, scintilla->TextColour.Green, scintilla->TextColour.Blue));
}

/*********************************************************************************************************************
** This is the main entry point, we're called from the Init action here.
*/

ScintillaKTK::ScintillaKTK(int SurfaceID, extScintilla *Scintilla)
:  scintilla(Scintilla), surfaceid(SurfaceID)
{
   lastkeytrans[0] = 0;
   idle_timer_on   = false;
   lastticktime    = 0;
   captured_mouse  = false;

   // Assign the OBJECTID of the native Pandora Surface to the Platform-wrapper Scintilla object

   wMain = Scintilla;

   SetStyles(c_styles, std::ssize(c_styles));
   SetStyles(std_styles, std::ssize(std_styles));

   SendScintilla(SCI_SETMODEVENTMASK, SC_MOD_INSERTTEXT, SC_MOD_DELETETEXT |
      SC_PERFORMED_USER | SC_PERFORMED_UNDO | SC_PERFORMED_REDO |
      SC_MULTISTEPUNDOREDO | SC_LASTSTEPINUNDOREDO | SC_MOD_BEFOREINSERT |
      SC_MOD_BEFOREDELETE | SC_MULTILINEUNDOREDO);

   // We're always UTF-8

   SendScintilla(SCI_SETCODEPAGE, SC_CP_UTF8);

   SetTicking(true);
}

ScintillaKTK::~ScintillaKTK()
{

}

//********************************************************************************************************************

void ScintillaKTK::Finalise()
{
   pf::Log log(__FUNCTION__);

   log.trace("");

   SetTicking(true);
   ScintillaBase::Finalise();
}

//********************************************************************************************************************

void ScintillaKTK::CreateCallTipWindow(Scintilla::PRectangle rc)
{
   pf::Log log(__FUNCTION__);
   log.trace("");
}

//********************************************************************************************************************

void ScintillaKTK::AddToPopUp(const char *label, int cmD, bool enabled)
{
   pf::Log log(__FUNCTION__);
   log.trace("%s", label);

   // The one and only Menu object is a member of ScintillaBase: Menu popup;

   auto menu = reinterpret_cast<OBJECTPTR>(popup.GetID());

   if (menu) {
      char buffer[200];
      snprintf(buffer, sizeof(buffer), "<item text=\"%s\"></item>", label);
      acDataXML(menu, buffer);
   }
}

//********************************************************************************************************************

void ScintillaKTK::SetVerticalScrollPos()
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("%d", topLine);

   DwellEnd(true); // Cancel any current mouse hover
/*
   if (!scintilla->ScrollLocked) {
      scroll.ViewSize = -1;
      scroll.PageSize = -1;
      scroll.Position = topLine * vs.lineHeight;
      scroll.Unit     = vs.lineHeight;
      if (glBitmap) QueueAction(sc::UpdateScroll::id, scintilla->VScrollID, &scroll);
      else ActionMsg(sc::UpdateScroll::id, scintilla->VScrollID, &scroll);
   }
*/
}

//********************************************************************************************************************

void ScintillaKTK::SetHorizontalScrollPos()
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("%d", xOffset);

   DwellEnd(true); // Cancel any current mouse hover
/*
   scroll.ViewSize = -1;
   scroll.PageSize = -1;
   scroll.Position = xOffset;
   scroll.Unit     = vs.lineHeight;
   if (glBitmap) QueueAction(sc::UpdateScroll::id, scintilla->HScrollID, &scroll);
   else ActionMsg(sc::UpdateScroll::id, scintilla->HScrollID, &scroll);
*/
}

/*********************************************************************************************************************
** nMax: Total number of lines in document
** nPage: Number of lines per view.
*/

bool ScintillaKTK::ModifyScrollBars(int nMax, int nPage)
{
   pf::Log log(__FUNCTION__);

   if (scintilla->ScrollLocked) return FALSE;
#if 0
   // Note: Sometimes Scintilla will attempt to change the scrollbars in the middle of surface redrawing.  This can
   // cause problems, so message delays are used in those cases.

   // Horizontal scrollbar

   scroll.ViewSize = -1;
   scroll.PageSize = SendScintilla(SCI_GETSCROLLWIDTH);
   scroll.Position = xOffset;
   scroll.Unit     = vs.lineHeight;

   log.traceBranch("Lines: %d, PageWidth: %d/%d, Delay: %c", nMax, scroll.PageSize, scroll.ViewSize, (glBitmap ? 'Y' : 'N'));

   if (glBitmap) QueueAction(sc::UpdateScroll::id, scintilla->HScrollID, &scroll);
   else ActionMsg(sc::UpdateScroll::id, scintilla->HScrollID, &scroll);

   // Vertical scrollbar

   lines = SendScintilla(SCI_GETLINECOUNT);

   if ((scintilla->Flags & SCIF::EXT_PAGE) != SCIF::NIL) {
      // Scintilla's nMax variable caters for all the lines, plus the height of the viewing area.

      scroll.ViewSize = nPage * vs.lineHeight;
      scroll.PageSize = nMax * vs.lineHeight;
   }
   else {
      scroll.ViewSize = -1;
      scroll.PageSize = (nMax+1) * vs.lineHeight;
      if ((scroll.PageSize > scintilla->Surface.Height) and (vs.lineHeight > 0)) {
         scroll.PageSize -= (scintilla->Surface.Height / vs.lineHeight);
      }
   }

   scroll.Position = topLine * vs.lineHeight;
   scroll.Unit     = vs.lineHeight;

   log.trace("PageLength: %d/%d (lines: %d/%d), Pos: %d", scroll.PageSize, scroll.ViewSize, lines, nMax, scroll.Position);

   if (glBitmap) QueueAction(sc::UpdateScroll::id, scintilla->VScrollID, &scroll);
   else ActionMsg(sc::UpdateScroll::id, scintilla->VScrollID, &scroll);
#endif
   return TRUE;
}

//********************************************************************************************************************
// Called after SCI_SETWRAPMODE and SCI_SETHSCROLLBAR.

void ScintillaKTK::ReconfigureScrollBars()
{
   pf::Log log(__FUNCTION__);
   log.traceBranch();

/*
	 if (horizontalScrollBarVisible) acShowID(scintilla->HScroll);
	 else acHideID(scintilla->HScroll);

	 if (verticalScrollBarVisible) acShowID(scintilla->VScroll);
	 else acHideID(scintilla->VScroll);
*/
}

/****************************************************************************
** Copies the selected text section to the Pandora clipboard.
*/

void ScintillaKTK::CopyToClipboard(const Scintilla::SelectionText &selectedText)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch();

   auto clipboard = objClipboard::create { };
   if (clipboard.ok()) {
      if (clipboard->addText(selectedText.s) IS ERR::Okay) {

      }
   }
}

/*********************************************************************************************************************
** Cut the selected text to the clipboard.
*/

void ScintillaKTK::Cut()
{
   pf::Log log(__FUNCTION__);
   log.traceBranch();

   if (SendScintilla(SCI_GETSELECTIONSTART) != SendScintilla(SCI_GETSELECTIONEND)) {
      Scintilla::SelectionText text;
      CopySelectionRange(&text);
      CopyToClipboard(text);
      ClearSelection();
   }
}

/*********************************************************************************************************************
** Copy the selected text to the clipboard.
*/

void ScintillaKTK::Copy()
{
   pf::Log log(__FUNCTION__);
   log.traceBranch();

   if (SendScintilla(SCI_GETSELECTIONSTART) != SendScintilla(SCI_GETSELECTIONEND)) {
      Scintilla::SelectionText text;
      CopySelectionRange(&text);
      CopyToClipboard(text);
   }
}

//********************************************************************************************************************

void ScintillaKTK::Paste()
{
   pf::Log log(__FUNCTION__);

   log.traceBranch();

   objClipboard::create clipboard = { };
   if (clipboard.ok()) {
      CSTRING *files;
      if (clipboard->getFiles(CLIPTYPE::TEXT, 0, nullptr, &files, nullptr) IS ERR::Okay) {
         objFile::create file = { fl::Path(files[0]), fl::Flags(FL::READ) };
         if (file.ok()) {
            int len, size;
            if ((file->get(FID_Size, size) IS ERR::Okay) and (size > 0)) {
               STRING buffer;
               if (AllocMemory(size, MEM::STRING, &buffer) IS ERR::Okay) {
                  if (file->read(buffer, size, &len) IS ERR::Okay) {
                     pdoc->BeginUndoAction();

                        ClearSelection();
                        pdoc->InsertString(CurrentPosition(), (char *)buffer, len);
                        SetEmptySelection(CurrentPosition() + len);

                     pdoc->EndUndoAction();

                     NotifyChange();
                     Redraw();

                     calc_longest_line(scintilla);
                  }
                  else error_dialog("Paste Error", "Failed to read data from the clipboard file.", ERR::Okay);

                  FreeResource(buffer);
               }
               else error_dialog("Paste Error", nullptr, ERR::AllocMemory);
            }
         }
         else {
            char msg[200];
            snprintf(msg, sizeof(msg), "Failed to load clipboard file \"%s\"", files[0]);
            error_dialog("Paste Error", msg, ERR::Okay);
         }
      }
   }
}

//********************************************************************************************************************
// This is used for the drag and drop of selected text.

void ScintillaKTK::ClaimSelection()
{
   pf::Log log(__FUNCTION__);
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
   pf::Log log("SciMsg");
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

         if (row > 1) {
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
   }
   else if (code IS SCN_SAVEPOINTREACHED) {
      // The document is unmodified (recently saved)

      log.trace("[SAVEPOINTREACHED]");
   }
   else if (code IS SCN_SAVEPOINTLEFT) {
      // The document has just been modified

      log.trace("[SAVEPOINTLEFT]");

      if (!scintilla->HoldModify) {
         scintilla->set(FID_Modified, TRUE);
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
      // This notification tells the container that the mouse was clicked inside a margin that was marked as sensitive
      // (see SCI_SETMARGINSENSITIVEN). This can be used to perform folding or to place breakpoints. The following
      // SCNotification fields are used:
      //
      // Field   Usage
      // modifiers The appropriate combination of SCI_SHIFT, SCI_CTRL and SCI_ALT to indicate the keys that were held down at the time of the margin click.
      // position  The position of the start of the line in the document that corresponds to the margin click.
      // margin    The margin number that was clicked.

      log.trace("[MARGINCLICK]");
   }
   else if (code IS SCN_NEEDSHOWN) {
      // Scintilla has determined that a range of lines that is currently invisible should be made visible. An example of where this may be
      // needed is if the end of line of a contracted fold point is deleted. This message is sent to the container in case it wants to make the
      // line visible in some unusual way such as making the whole document visible. Most containers will just ensure each line in the range is
      // visible by calling SCI_ENSUREVISIBLE. The position and length fields of SCNotification indicate the range of the document that should be
      // made visible.

      int i, first, last;

      first = SendScintilla(SCI_LINEFROMPOSITION, scn.position);
      last = SendScintilla(SCI_LINEFROMPOSITION, scn.position + scn.length - 1);

      log.trace("[NEEDSHOWN] First: %d, Last: %d", first, last);

      for (i=first; i < last; ++i) {
         SendScintilla(SCI_ENSUREVISIBLE, i);
      }
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
   }
   else if (code IS SCN_AUTOCSELECTION) {
      // The user has selected an item in an autocompletion list. The notification is sent before the selection is inserted. Automatic
      // insertion can be cancelled by sending a SCI_AUTOCCANCEL message before returning from the notification. The SCNotification fields
      // used are:
      //
      // lParam: The start position of the word being completed.
      // text: The text of the selection.

      log.trace("[AUTOCSELECTION]");
   }
   else if (code IS 2012) {
      // Deprecated
   }
   else log.traceWarning("Notification code %d unsupported.", code);
}

//********************************************************************************************************************

void ScintillaKTK::ScrollText(int linesToMove)
{
   if (!surfaceid) return;
/*
   log.traceBranch("linesToMove: %d", linesToMove);

   Scintilla::PRectangle rect = GetClientRectangle();

   struct mtMoveContent movecontent;
   movecontent.XCoord = rect.left;
   movecontent.YCoord = rect.top - (vs.lineHeight * linesToMove);
   movecontent.Width  = rect.Width();
   movecontent.Height = rect.Height();
   movecontent.XDest  = rect.left;
   movecontent.YDest  = rect.top;
   movecontent.ClipLeft   = rect.left;
   movecontent.ClipTop    = rect.top;
   movecontent.ClipRight  = rect.left + rect.Width();
   movecontent.ClipBottom = rect.top + rect.Height();
   movecontent.Flags = 0;
   ActionMsg(drw::MoveContent, surfaceid, &movecontent);
*/
   Scintilla::PRectangle rect = GetClientRectangle();
   pf::ScopedObjectLock surface(surfaceid);
   if (surface.granted()) acDrawArea(*surface, rect.left, rect.top, rect.Width(), rect.Height());
}

//********************************************************************************************************************

void ScintillaKTK::SetTicking(bool On)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch("State: %d", On);

   if (!On) ticking_on = false;
   else if (!ticking_on) {
      ticking_on = true;
      lastticktime = (PreciseTime() / 1000LL);
   }
}

//********************************************************************************************************************
// Grab or release the mouse and keyboard.  This is usually called when the user clicks a mouse button and holds it
// while dragging the mouse (e.g. when highlighting text).

void ScintillaKTK::SetMouseCapture(bool On)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch("State: %d", On);
   captured_mouse = On;
}

/*********************************************************************************************************************
** Simply returns the capture state.
*/

bool ScintillaKTK::HaveMouseCapture()
{
   //log.trace("HaveMouseCapture()");
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
** Refer to the pan_surface.cxx file for the drawing routines that are used when panDraw() is active.
*/

void ScintillaKTK::panDraw(objSurface *TargetSurface, objBitmap *Bitmap)
{
   pf::Log log(__FUNCTION__);
   Scintilla::PRectangle rcClient;
   Scintilla::Surface *surface;

   if (this->paintState != notPainting) return; // Recursion prevention

   Scintilla::PRectangle paintrect(Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right, Bitmap->Clip.Bottom);

   this->paintState = painting;
   this->rcPaint    = paintrect;
   rcClient         = GetClientRectangle();
   this->paintingAllText = rcPaint.Contains(rcClient);

   log.traceBranch("Area: %dx%d - %dx%d", rcClient.left, rcClient.top, rcClient.Width(), rcClient.Height());

   // Create a new surface object (SurfacePan)

   if ((surface = Scintilla::Surface::Allocate())) {
      surface->Init(Bitmap, nullptr);
      this->Paint(surface, paintrect);      // Get Scintilla to do the painting.  See Editor::paint()
      surface->Release();
      delete surface;
   }

   if (this->paintState IS paintAbandoned) {
      // Painting area was insufficient to cover new styling or brace highlight positions, word wrapping etc.  This
      // means that the clipping area needs to be extended, and we're not able to do that from inside a Draw() call.
      // The simplest solution is to send a new draw message to the parent surface, telling it to redraw the entire area.

      QueueAction(AC::Draw, TargetSurface->UID);
   }

   this->paintState = notPainting;
}

/*********************************************************************************************************************
** Called from the SetFont() method whenever the user opts to change the font.
*/

void ScintillaKTK::panFontChanged(void *Font, void *BoldFont, void *ItalicFont, void *BIFont)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch();

   glFont       = (OBJECTPTR)Font;
   glBoldFont   = (OBJECTPTR)BoldFont;
   glItalicFont = (OBJECTPTR)ItalicFont;
   glBIFont     = (OBJECTPTR)BIFont;

   this->InvalidateStyleRedraw();
   this->RefreshStyleData();
}

//********************************************************************************************************************

void ScintillaKTK::panWordwrap(int Value)
{
   pf::Log log(__FUNCTION__);
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

//********************************************************************************************************************

void ScintillaKTK::panIdleEvent()
{
   if (idle_timer_on) {
      bool keepidling = Idle();

      if (!keepidling) {
         //SetIdle(false);
         idle_timer_on = FALSE;
      }
   }

   if (ticking_on) {
#ifdef _DEBUG
      if ((int)((PreciseTime()/1000LL) - lastticktime) >= caret.period) {
#else
      if ((int)((PreciseTime()/1000LL) - lastticktime) >= caret.period / 5) {
#endif
    	 Tick();  // Goes to -> Editor::Tick()
         lastticktime = (PreciseTime()/1000LL);
      }
   }
}

//********************************************************************************************************************

void ScintillaKTK::panKeyDown(int Key, KQ Flags)
{
   bool consumed;

   // After we call KeyDown(), Scintilla will call KeyDefault()

   KeyDown(Key, (Flags & KQ::SHIFT) != KQ::NIL, (Flags & KQ::CTRL) != KQ::NIL, (Flags & KQ::ALT) != KQ::NIL, &consumed);
}

//********************************************************************************************************************

int ScintillaKTK::KeyDefault(int key, int modifiers)
{
   //log.msg("%d, $%.8x", key, modifiers);
   AddCharUTF(lastkeytrans, strlen(lastkeytrans), false);
   return 1;
}

//********************************************************************************************************************

void ScintillaKTK::panMousePress(JET Button, double x, double y)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch("%.0fx%.0f", x, y);

   if (Button IS JET::LMB) {
      // This disables the current selection (effectively eliminating the potential for drag and drop).

      SetEmptySelection(CurrentPosition());
      Scintilla::Point point((int)x, (int)y);
      ButtonDown(point, (PreciseTime()/1000LL), scintilla->KeyShift, scintilla->KeyCtrl, scintilla->KeyAlt);
   }
   else if (Button IS JET::RMB) {




   }
}

//********************************************************************************************************************

void ScintillaKTK::panMouseMove(double x, double y)
{
   Scintilla::Point point((int)x, (int)y);
   ButtonMove(point);
}

//********************************************************************************************************************

void ScintillaKTK::panMouseRelease(JET Button, double x, double y)
{
   pf::Log log(__FUNCTION__);
   Scintilla::Point point((int)x, (int)y);

   log.trace("%.0fx%.0f", x, y);

   if (Button IS JET::LMB) {
      ButtonUp(point, (PreciseTime()/1000LL), scintilla->KeyCtrl);
   }
}

//********************************************************************************************************************

void ScintillaKTK::panResized()
{
   pf::Log log(__FUNCTION__);
   log.traceBranch();
   ChangeSize();
}

//********************************************************************************************************************

void ScintillaKTK::panScrollToX(double x)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch("%.2f", x);
   HorizontalScrollTo((int)(x));
}

//********************************************************************************************************************

void ScintillaKTK::panScrollToY(double y)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch("%.2f", y);
   ScrollTo((int)(y / vs.lineHeight));
}

//********************************************************************************************************************
#if 0
void ScintillaKTK::SetSelectedTextStyle(int style)
{
   pf::Log log(__FUNCTION__);
   log.traceBranch("Style: %d", style);

   WndProc(SCI_STARTSTYLING, SelectionStart(), 0x1f);//mask - only overwrite the 5 style bits
   WndProc(SCI_SETSTYLING, SelectionEnd() - SelectionStart(), style);//style
}
#endif
//********************************************************************************************************************

void ScintillaKTK::panGotFocus()
{
   SetFocusState(TRUE);
}

//********************************************************************************************************************

void ScintillaKTK::panLostFocus()
{
   SetFocusState(FALSE);
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
   pf::Log log(__FUNCTION__);
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
** Set the lexer.  To turn off the lexer, use SCLEX_NULL.
*/

void ScintillaKTK::SetLexer(uptr_t LexID)
{
   pf::Log log(__FUNCTION__);
   log.branch("Using lexer %d", (int)LexID);

//   SendScintilla(SCI_STYLERESETDEFAULT);
   SendScintilla(SCI_SETLEXER, LexID);
   //SendScintilla(SCI_CLEARDOCUMENTSTYLE);

   SendScintilla(SCI_STARTSTYLING, 0, 0x1f);
   QueueAction(AC::Draw, scintilla->SurfaceID);
}

void ScintillaKTK::SetLexerLanguage(const char *languageName)
{
   SendScintilla(SCI_SETLEXERLANGUAGE, 0UL, languageName);
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


























#if 0

// The default fold margin width.
static const int defaultFoldMarginWidth = 14;

// The default set of characters that make up a word.
static const char *defaultWordChars = "_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";

/*********************************************************************************************************************
** Handle a possible change to any current call tip.
*/

#if 0
void ScintillaKTK::panCallTip()
{
   if (!ctAPIs) return;

   long pos = SendScintilla(SCI_GETCURRENTPOS);
   long linenr = SendScintilla(SCI_LINEFROMPOSITION,pos);
   long linelen = SendScintilla(SCI_LINELENGTH,linenr);

   char *lbuf = new char[linelen + 1];

   int loff = SendScintilla(SCI_GETCURLINE,linelen + 1,lbuf);
   int commas = 0, start = -1;

   // Move backwards through the line looking for the start of the current call tip and working out which argument it is.

   while (loff > 0) {
      char ch = lbuf[--loff];

      if (ch IS ',')
         ++commas;
      else if (ch IS ')')
      {
         int depth = 1;

         // Ignore everything back to the start of the corresponding parenthesis.
         while (loff > 0)
         {
            ch = lbuf[--loff];

            if (ch IS ')')
               ++depth;
            else if (ch IS '(' and --depth IS 0)
               break;
         }
      }
      else if (ch IS '(' and loff > 0)
      {
         if (isWordChar(lbuf[loff - 1]))
         {
            // The parenthesis is preceded by a word so
            // find the start of that word.
            lbuf[loff--] = '\0';

            while (loff >= 0 and isWordChar(lbuf[loff]))
               --loff;

            start = loff + 1;
            break;
         }

         // We are between parentheses that do not correspond to
         // a call tip, so reset the argument count.
         commas = 0;
      }
   }

   // Cancel any existing call tip.
   SendScintilla(SCI_CALLTIPCANCEL);

   // Done if there is no new call tip to set.
   if (start < 0)
   {
      delete []lbuf;
      return;
   }

   QString ct = ctAPIs -> callTips(&lbuf[start],maxCallTips,commas);

   delete []lbuf;

   if (ct.isEmpty())
      return;

   ctpos = SendScintilla(SCI_POSITIONFROMLINE,linenr) + start;

   SendScintilla(SCI_CALLTIPSHOW,ctpos,ct.latin1());

   // Done if there is more than one line in the call tip or there isn't a
   // down arrow at the start.
   if (ct[0] IS '\002' or ct.find('\n') >= 0)
      return;

   // Highlight the current argument.
   int astart;

   if (commas IS 0)
      astart = ct.find('(');
   else
   {
      astart = -1;

      do
         astart = ct.find(',',astart + 1);
      while (astart >= 0 and --commas > 0);
   }

   int len = ct.length();

   if (astart < 0 or ++astart IS len)
      return;

   // The end is at the next comma or unmatched closing parenthesis.
   int aend, depth = 0;

   for (aend = astart; aend < len; ++aend)
   {
      QChar ch = ct.at(aend);

      if (ch IS ',' and depth IS 0)
         break;
      else if (ch IS '(')
         ++depth;
      else if (ch IS ')')
      {
         if (depth IS 0)
            break;

         --depth;
      }
   }

   if (astart != aend) SendScintilla(SCI_CALLTIPSETHLT,astart,aend);
}
#endif

#if 0
// Handle a call tip click.
void ScintillaKTK::handleCallTipClick(int dir)
{
   if (!ctAPIs)
      return;

   QString ct = ctAPIs -> callTipsNextPrev(dir);

   if (ct.isNull())
      return;

   SendScintilla(SCI_CALLTIPSHOW,ctpos,ct.latin1());
}
#endif

/*********************************************************************************************************************
** Possibly start auto-completion.
*/

void ScintillaKTK::panStartAutoCompletion(AutoCompletionSource acs, bool checkThresh,bool emptyRoot, bool single)
{
   long wend, wstart;

   if (emptyRoot) wend = wstart = 0;
   else {
      // See how long a word has been entered so far.
      wend = SendScintilla(SCI_GETCURRENTPOS);
      wstart = SendScintilla(SCI_WORDSTARTPOSITION,wend,TRUE);
   }

   int wlen = wend - wstart;

   if (checkThresh and wlen < acThresh) return;

   // Get the word entered so far.
   char *word = new char[wlen + 1];
   char *cp = word;

   for (long i = wstart; i < wend; ++i) {
      *cp++ = SendScintilla(SCI_GETCHARAT,i);
   }

   *cp = '\0';

   // Generate the string representing the valid words to select from.
   QStringList wlist;
   bool cs = !SendScintilla(SCI_AUTOCGETIGNORECASE);

   if (acs IS AcsDocument)
   {
      SendScintilla(SCI_SETSEARCHFLAGS,SCFIND_WORDSTART | (cs ? SCFIND_MATCHCASE : 0));

      long pos = 0;
      long dlen = SendScintilla(SCI_GETLENGTH);
      QString root(word);

      for (;;)
      {
         long fstart;

         SendScintilla(SCI_SETTARGETSTART,pos);
         SendScintilla(SCI_SETTARGETEND,dlen);

         if ((fstart = SendScintilla(SCI_SEARCHINTARGET,wlen,word)) < 0)
            break;

         // Move past the root part.
         pos = fstart + wlen;

         // Skip if this is the word we are auto-completing.
         if (fstart IS wstart)
            continue;

         // Get the rest of this word.
         QString w(root);

         while (pos < dlen)
         {
            char ch = SendScintilla(SCI_GETCHARAT,pos);

            if (!isWordChar(ch))
               break;

            w += ch;

            ++pos;
         }

         // Add the word if it isn't already there.
         if (wlist.findIndex(w) < 0)
            wlist.append(w);
      }

      wlist.sort();
   }
   else if (acAPIs)
      wlist = acAPIs -> autoCompletionList(word,cs);

   delete []word;

   if (wlist.isEmpty())
      return;

   char sep = SendScintilla(SCI_AUTOCGETSEPARATOR);
   acWordList = wlist.join(QChar(sep));

   SendScintilla(SCI_AUTOCSETCHOOSESINGLE,single);
   SendScintilla(SCI_AUTOCSHOW,wlen,acWordList.latin1());
}

/*********************************************************************************************************************
** Maintain the indentation of the previous line.
*/

void ScintillaKTK::panMaintainIndentation(char ch,long pos)
{
   if (ch != '\r' and ch != '\n') return;

   int curr_line = SendScintilla(SCI_LINEFROMPOSITION,pos);

   // Get the indentation of the preceding non-zero length line.
   int ind = 0;

   for (int line = curr_line - 1; line >= 0; --line) {
      if (SendScintilla(SCI_GETLINEENDPOSITION,line) > SendScintilla(SCI_POSITIONFROMLINE,line)) {
         ind = indentation(line);
         break;
      }
   }

   if (ind > 0) autoIndentLine(pos,curr_line,ind);
}

/*********************************************************************************************************************
** Implement auto-indentation.
*/

void ScintillaKTK::panAutoIndentation(char ch,long pos)
{
   int curr_line = SendScintilla(SCI_LINEFROMPOSITION,pos);
   int ind_width = indentationWidth();
   long curr_line_start = SendScintilla(SCI_POSITIONFROMLINE,curr_line);

   const char *block_start = lexer -> blockStart();
   bool start_single = (block_start and strlen(block_start) IS 1);

   const char *block_end = lexer -> blockEnd();
   bool end_single = (block_end and strlen(block_end) IS 1);

   if (end_single and block_end[0] IS ch)
   {
      if (!(lexer -> autoIndentStyle() & AiClosing) and rangeIsWhitespace(curr_line_start,pos - 1))
         autoIndentLine(pos,curr_line,blockIndent(curr_line - 1) - indentationWidth());
   }
   else if (start_single and block_start[0] IS ch)
   {
      // De-indent if we have already indented because the previous
      // line was a start of block keyword.
      if (!(lexer -> autoIndentStyle() & AiOpening) and getIndentState(curr_line - 1) IS isKeywordStart and rangeIsWhitespace(curr_line_start,pos - 1))
         autoIndentLine(pos,curr_line,blockIndent(curr_line - 1) - indentationWidth());
   }
   else if (ch IS '\r' or ch IS '\n')
      autoIndentLine(pos,curr_line,blockIndent(curr_line - 1));
}

/*********************************************************************************************************************
** Set the indentation for a line.
*/

void ScintillaKTK::panAutoIndentLine(long pos,int line,int indent)
{
   if (indent < 0) return;

   long pos_before = SendScintilla(SCI_GETLINEINDENTPOSITION,line);
   SendScintilla(SCI_SETLINEINDENTATION,line,indent);
   long pos_after = SendScintilla(SCI_GETLINEINDENTPOSITION,line);
   long new_pos = -1;

   if (pos_after > pos_before) new_pos = pos + (pos_after - pos_before);
   else if (pos_after < pos_before and pos >= pos_after) {
      if (pos >= pos_before) new_pos = pos + (pos_after - pos_before);
      else new_pos = pos_after;
   }

   if (new_pos >= 0) SendScintilla(SCI_SETSEL,new_pos,new_pos);
}

/*********************************************************************************************************************
** Return the indentation of the block defined by the given line (or something significant before).
*/

int ScintillaKTK::panBlockIndent(int line)
{
   if (line < 0) return 0;

   // Handle the trvial case.
   if (!lexer -> blockStartKeyword() and !lexer -> blockStart() and !lexer -> blockEnd()) return indentation(line);

   int line_limit = line - lexer -> blockLookback();

   if (line_limit < 0) line_limit = 0;

   for (int l = line; l >= line_limit; --l) {
      IndentState istate = getIndentState(l);

      if (istate != isNone) {
         int ind_width = indentationWidth();
         int ind = indentation(l);

         if (istate IS isBlockStart) {
            if (!(lexer -> autoIndentStyle() & AiOpening)) ind += ind_width;
         }
         else if (istate IS isBlockEnd) {
            if (lexer -> autoIndentStyle() & AiOpening) ind -= ind_width;
            if (ind < 0) ind = 0;
         }
         else if (line IS l) ind += ind_width;
         return ind;
      }
   }

   return indentation(line);
}

/*********************************************************************************************************************
** Return TRUE if all characters starting at spos up to, but not including epos, are spaces or tabs.
*/

bool ScintillaKTK::panRangeIsWhitespace(long spos,long epos)
{
   while (spos < epos) {
      char ch = SendScintilla(SCI_GETCHARAT,spos);
      if (ch != ' ' and ch != '\t') return FALSE;
      ++spos;
   }

   return TRUE;
}

/*********************************************************************************************************************
** Returns the indentation state of a line.
*/

ScintillaKTK::IndentState ScintillaKTK::panGetIndentState(int line)
{
   IndentState istate;

   // Get the styled text.
   long spos = SendScintilla(SCI_POSITIONFROMLINE,line);
   long epos = SendScintilla(SCI_POSITIONFROMLINE,line + 1);

   char *text = new char[(epos - spos + 1) * 2];

   SendScintilla(SCI_GETSTYLEDTEXT,spos,epos,text);

   const char *words;
   int style, bstart_off, bend_off;

   // Block start/end takes precedence over keywords.
   words = lexer -> blockStart(&style);
   bstart_off = findStyledWord(text,style,words);

   words = lexer -> blockEnd(&style);
   bend_off = findStyledWord(text,style,words);

   if (bstart_off > bend_off)
      istate = isBlockStart;
   else if (bend_off > bstart_off)
      istate = isBlockEnd;
   else
   {
      words = lexer -> blockStartKeyword(&style);

      istate = (findStyledWord(text,style,words) >= 0) ? isKeywordStart : isNone;
   }

   delete[] text;

   return istate;
}

/*********************************************************************************************************************
** Text is a pointer to some styled text (ie. a character byte followed by a
// style byte).  style is a style number.  words is a space separated list of
// words.  Returns the position in the text of the last one of the words with
// the style.  The reason we are after the last, and not the first, occurance
// is that we are looking for words that start and end a block where the latest
// one is the most significant.
*/

int ScintillaKTK::panFindStyledWord(const char *text, int style, const char *words)
{
   if (!words) return -1;

   // Find the range of text with the style we are looking for.
   const char *stext;

   for (stext = text; stext[1] != style; stext += 2)
      if (stext[0] IS '\0') return -1;

   // Move to the last character.
   const char *etext = text;

   while (etext[2] != '\0') etext += 2;

   // Backtrack until we find the style.  There will be one.
   while (etext[1] != style) etext -= 2;

   // Look for each word in turn.
   while (words[0] != '\0') {
      // Find the end of the word.
      const char *eword = words;

      while (eword[1] != ' ' and eword[1] != '\0') ++eword;

      // Now search the text backwards.
      const char *wp = eword;

      for (const char *tp = etext; tp >= stext; tp -= 2) {
         if (tp[0] != wp[0] or tp[1] != style) {
            // Reset the search.
            wp = eword;
            continue;
         }

         // See if all the word has matched.
         if (wp-- IS words) return (tp - text) / 2;
      }

      // Move to the start of the next word if there is one.
      words = eword + 1;

      if (words[0] IS ' ') ++words;
   }

   return -1;
}

/*********************************************************************************************************************
** Return the end-of-line mode.
*/

// Set the folding style.
void ScintillaKTK::setFolding(FoldStyle folding)
{
   fold = folding;

   if (folding IS NoFoldStyle) {
      SendScintilla(SCI_SETMARGINWIDTHN,2,0L);
      return;
   }

   int mask = SendScintilla(SCI_GETMODEVENTMASK);
   SendScintilla(SCI_SETMODEVENTMASK,mask | SC_MOD_CHANGEFOLD);
   SendScintilla(SCI_SETFOLDFLAGS,SC_FOLDFLAG_LINEAFTER_CONTRACTED);
   SendScintilla(SCI_SETMARGINTYPEN,2,(long)SC_MARGIN_SYMBOL);
   SendScintilla(SCI_SETMARGINMASKN,2,SC_MASK_FOLDERS);
   SendScintilla(SCI_SETMARGINSENSITIVEN,2,1);

   // Set the marker symbols to use.
   switch (folding) {
   case PlainFoldStyle:
      setFoldMarker(SC_MARKNUM_FOLDEROPEN,SC_MARK_MINUS);
      setFoldMarker(SC_MARKNUM_FOLDER,SC_MARK_PLUS);
      setFoldMarker(SC_MARKNUM_FOLDERSUB);
      setFoldMarker(SC_MARKNUM_FOLDERTAIL);
      setFoldMarker(SC_MARKNUM_FOLDEREND);
      setFoldMarker(SC_MARKNUM_FOLDEROPENMID);
      setFoldMarker(SC_MARKNUM_FOLDERMIDTAIL);
      break;

   case CircledFoldStyle:
      setFoldMarker(SC_MARKNUM_FOLDEROPEN,SC_MARK_CIRCLEMINUS);
      setFoldMarker(SC_MARKNUM_FOLDER,SC_MARK_CIRCLEPLUS);
      setFoldMarker(SC_MARKNUM_FOLDERSUB);
      setFoldMarker(SC_MARKNUM_FOLDERTAIL);
      setFoldMarker(SC_MARKNUM_FOLDEREND);
      setFoldMarker(SC_MARKNUM_FOLDEROPENMID);
      setFoldMarker(SC_MARKNUM_FOLDERMIDTAIL);
      break;

   case BoxedFoldStyle:
      setFoldMarker(SC_MARKNUM_FOLDEROPEN,SC_MARK_BOXMINUS);
      setFoldMarker(SC_MARKNUM_FOLDER,SC_MARK_BOXPLUS);
      setFoldMarker(SC_MARKNUM_FOLDERSUB);
      setFoldMarker(SC_MARKNUM_FOLDERTAIL);
      setFoldMarker(SC_MARKNUM_FOLDEREND);
      setFoldMarker(SC_MARKNUM_FOLDEROPENMID);
      setFoldMarker(SC_MARKNUM_FOLDERMIDTAIL);
      break;

   case CircledTreeFoldStyle:
      setFoldMarker(SC_MARKNUM_FOLDEROPEN,SC_MARK_CIRCLEMINUS);
      setFoldMarker(SC_MARKNUM_FOLDER,SC_MARK_CIRCLEPLUS);
      setFoldMarker(SC_MARKNUM_FOLDERSUB,SC_MARK_VLINE);
      setFoldMarker(SC_MARKNUM_FOLDERTAIL,SC_MARK_LCORNERCURVE);
      setFoldMarker(SC_MARKNUM_FOLDEREND,SC_MARK_CIRCLEPLUSCONNECTED);
      setFoldMarker(SC_MARKNUM_FOLDEROPENMID,SC_MARK_CIRCLEMINUSCONNECTED);
      setFoldMarker(SC_MARKNUM_FOLDERMIDTAIL,SC_MARK_TCORNERCURVE);
      break;

   case BoxedTreeFoldStyle:
      setFoldMarker(SC_MARKNUM_FOLDEROPEN,SC_MARK_BOXMINUS);
      setFoldMarker(SC_MARKNUM_FOLDER,SC_MARK_BOXPLUS);
      setFoldMarker(SC_MARKNUM_FOLDERSUB,SC_MARK_VLINE);
      setFoldMarker(SC_MARKNUM_FOLDERTAIL,SC_MARK_LCORNER);
      setFoldMarker(SC_MARKNUM_FOLDEREND,SC_MARK_BOXPLUSCONNECTED);
      setFoldMarker(SC_MARKNUM_FOLDEROPENMID,SC_MARK_BOXMINUSCONNECTED);
      setFoldMarker(SC_MARKNUM_FOLDERMIDTAIL,SC_MARK_TCORNER);
      break;
   }

   SendScintilla(SCI_SETMARGINWIDTHN,2,defaultFoldMarginWidth);
}


// Set up a folder marker.
void ScintillaKTK::setFoldMarker(int marknr,int mark)
{
   SendScintilla(SCI_MARKERDEFINE,marknr,mark);

   if (mark != SC_MARK_EMPTY) {
      SendScintilla(SCI_MARKERSETFORE,marknr,white);
      SendScintilla(SCI_MARKERSETBACK,marknr,black);
   }
}


// Handle a click in the fold margin.  This is mostly taken from SciTE.
void ScintillaKTK::foldClick(int lineClick,int bstate)
{
   if ((bstate & ShiftButton) and (bstate & ControlButton)) {
      foldAll();
      return;
   }

   int levelClick = SendScintilla(SCI_GETFOLDLEVEL,lineClick);

   if (levelClick & SC_FOLDLEVELHEADERFLAG) {
      if (bstate & ShiftButton) {
         // Ensure all children are visible.
         SendScintilla(SCI_SETFOLDEXPANDED,lineClick,1);
         foldExpand(lineClick,TRUE,TRUE,100,levelClick);
      }
      else if (bstate & ControlButton) {
         if (SendScintilla(SCI_GETFOLDEXPANDED,lineClick)) {
            // Contract this line and all its children.
            SendScintilla(SCI_SETFOLDEXPANDED,lineClick,0L);
            foldExpand(lineClick,FALSE,TRUE,0,levelClick);
         }
         else { // Expand this line and all its children.
            SendScintilla(SCI_SETFOLDEXPANDED,lineClick,1);
            foldExpand(lineClick,TRUE,TRUE,100,levelClick);
         }
      }
      else { // Toggle this line.
         SendScintilla(SCI_TOGGLEFOLD,lineClick);
      }
   }
}

// Do the hard work of hiding and showing  lines.  This is mostly taken from SciTE.

void ScintillaKTK::foldExpand(int &line,bool doExpand,bool force, int visLevels,int level)
{
   int lineMaxSubord = SendScintilla(SCI_GETLASTCHILD,line,level & SC_FOLDLEVELNUMBERMASK);

   line++;

   while (line <= lineMaxSubord) {
      if (force) {
         if (visLevels > 0) SendScintilla(SCI_SHOWLINES,line,line);
         else SendScintilla(SCI_HIDELINES,line,line);
      }
      else if (doExpand) SendScintilla(SCI_SHOWLINES,line,line);

      int levelLine = level;

      if (levelLine IS -1) levelLine = SendScintilla(SCI_GETFOLDLEVEL,line);

      if (levelLine & SC_FOLDLEVELHEADERFLAG) {
         if (force) {
            if (visLevels > 1) SendScintilla(SCI_SETFOLDEXPANDED,line,1);
            else SendScintilla(SCI_SETFOLDEXPANDED,line,0L);

            foldExpand(line,doExpand,force,visLevels - 1);
         }
         else if (doExpand) {
            if (!SendScintilla(SCI_GETFOLDEXPANDED,line)) SendScintilla(SCI_SETFOLDEXPANDED,line,1);
            foldExpand(line,TRUE,force,visLevels - 1);
         }
         else foldExpand(line,FALSE,force,visLevels - 1);
      }
      else line++;
   }
}

// Fully expand (if there is any line currently folded) all text.  Otherwise,
// fold all text.  This is mostly taken from SciTE.
void ScintillaKTK::foldAll()
{
   recolor();

   int maxLine = SendScintilla(SCI_GETLINECOUNT);
   bool expanding = TRUE;

   for (int lineSeek=0; lineSeek < maxLine; lineSeek++) {
      if (SendScintilla(SCI_GETFOLDLEVEL,lineSeek) & SC_FOLDLEVELHEADERFLAG) {
         expanding = !SendScintilla(SCI_GETFOLDEXPANDED,lineSeek);
         break;
      }
   }

   for (int line=0; line < maxLine; line++) {
      int level = SendScintilla(SCI_GETFOLDLEVEL,line);

      if ((level & SC_FOLDLEVELHEADERFLAG) and (SC_FOLDLEVELBASE IS (level & SC_FOLDLEVELNUMBERMASK))) {
         if (expanding) {
            SendScintilla(SCI_SETFOLDEXPANDED,line,1);
            foldExpand(line,TRUE,FALSE,0,level);
            line--;
         }
         else {
            int lineMaxSubord = SendScintilla(SCI_GETLASTCHILD,line,-1);
            SendScintilla(SCI_SETFOLDEXPANDED,line,0L);
            if (lineMaxSubord > line) SendScintilla(SCI_HIDELINES,line + 1,lineMaxSubord);
         }
      }
   }
}


// Handle a fold change.  This is mostly taken from SciTE.
void ScintillaKTK::foldChanged(int line,int levelNow,int levelPrev)
{
   if (levelNow & SC_FOLDLEVELHEADERFLAG) {
      if (!(levelPrev & SC_FOLDLEVELHEADERFLAG)) SendScintilla(SCI_SETFOLDEXPANDED,line,1);
   }
   else if (levelPrev & SC_FOLDLEVELHEADERFLAG) {
      if (!SendScintilla(SCI_GETFOLDEXPANDED,line)) {
         // Removing the fold from one that has been contracted
         // so should expand.  Otherwise lines are left
         // invisible with no way to make them visible.
         foldExpand(line,TRUE,FALSE,0,levelPrev);
      }
   }
}


// Toggle the fold for a line if it contains a fold marker.
void ScintillaKTK::foldLine(int line)
{
   SendScintilla(SCI_TOGGLEFOLD,line);
}


// Handle the SCN_MODIFIED notification.
void ScintillaKTK::handleModified(int pos,int mtype,const char *text,int len,
               int added,int line,int foldNow,int foldPrev)
{
   if (mtype & SC_MOD_CHANGEFOLD)
   {
      if (fold)
         foldChanged(line,foldNow,foldPrev);
   }
   else if (mtype & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT))
      emit textChanged();
}


// Query the modified state.
bool ScintillaKTK::isModified()
{
   // We don't use SCI_GETMODIFY as it seems to be buggy in Scintilla
   // v1.61.
   return modified;
}

/*********************************************************************************************************************
** Handle the SCN_MARGINCLICK notification.
*/

void ScintillaKTK::handleMarginClick(int pos,int modifiers,int margin)
{
   int state = 0;

   if (modifiers & SCMOD_SHIFT) state |= ShiftButton;
   if (modifiers & SCMOD_CTRL) state |= ControlButton;
   if (modifiers & SCMOD_ALT) state |= AltButton;

   int line = SendScintilla(SCI_LINEFROMPOSITION,pos);

   if (fold and margin IS 2) foldClick(line,state);
   else emit marginClicked(margin,line,(ButtonState)state);
}

/*********************************************************************************************************************
** Return the state of indentation guides.
*/

bool ScintillaKTK::indentationGuides()
{
   return SendScintilla(SCI_GETINDENTATIONGUIDES);
}

// Enable and disable indentation guides.
void ScintillaKTK::setIndentationGuides(bool enable)
{
   SendScintilla(SCI_SETINDENTATIONGUIDES,enable);
}

/*********************************************************************************************************************
** Define a marker based on a character.
*/

int ScintillaKTK::panMarkerDefine(char ch,int mnr)
{
   checkMarker(mnr);
   if (mnr >= 0) SendScintilla(SCI_MARKERDEFINE,mnr,static_cast<long>(SC_MARK_CHARACTER) + ch);
   return mnr;
}

/*********************************************************************************************************************
** Add a marker to a line.
*/

int ScintillaKTK::panMarkerAdd(int linenr,int mnr)
{
   if (mnr < 0 or mnr > MARKER_MAX or (allocatedMarkers & (1 << mnr)) IS 0) return -1;
   return SendScintilla(SCI_MARKERADD,linenr,mnr);
}

/*********************************************************************************************************************
** Get the marker mask for a line.
*/

unsigned ScintillaKTK::panMarkersAtLine(int linenr)
{
   return SendScintilla(SCI_MARKERGET,linenr);
}

/*********************************************************************************************************************
** Delete a marker from a line.
*/

void ScintillaKTK::panMarkerDelete(int linenr,int mnr)
{
   if (mnr <= MARKER_MAX) {
      if (mnr < 0) {
         unsigned am = allocatedMarkers;

         for (int m = 0; m <= MARKER_MAX; ++m) {
            if (am & 1) SendScintilla(SCI_MARKERDELETE,linenr,m);
            am >>= 1;
         }
      }
      else if (allocatedMarkers & (1 << mnr)) SendScintilla(SCI_MARKERDELETE,linenr,mnr);
   }
}

/*********************************************************************************************************************
** Delete a marker from the text.
*/

void ScintillaKTK::panMarkerDeleteAll(int mnr)
{
   if (mnr <= MARKER_MAX) {
      if (mnr < 0) SendScintilla(SCI_MARKERDELETEALL,-1);
      else if (allocatedMarkers & (1 << mnr)) SendScintilla(SCI_MARKERDELETEALL,mnr);
   }
}

/*********************************************************************************************************************
** Delete a marker handle from the text.
*/

void ScintillaKTK::panMarkerDeleteHandle(int mhandle)
{
   SendScintilla(SCI_MARKERDELETEHANDLE,mhandle);
}

/*********************************************************************************************************************
** Return the line containing a marker instance.
*/

int ScintillaKTK::panMarkerLine(int mhandle)
{
   return SendScintilla(SCI_MARKERLINEFROMHANDLE,mhandle);
}

/*********************************************************************************************************************
** Search forwards for a marker.
*/

int ScintillaKTK::panMarkerFindNext(int linenr,unsigned mask)
{
   return SendScintilla(SCI_MARKERNEXT,linenr,mask);
}

/*********************************************************************************************************************
** Search backwards for a marker.
*/

int ScintillaKTK::panMarkerFindPrevious(int linenr,unsigned mask)
{
   return SendScintilla(SCI_MARKERPREVIOUS,linenr,mask);
}

/*********************************************************************************************************************
** Set the marker background colour.
*/

void ScintillaKTK::panSetMarkerBackgroundColor(const QColor &col,int mnr)
{
   if (mnr <= MARKER_MAX)
   {
      if (mnr < 0)
      {
         unsigned am = allocatedMarkers;

         for (int m = 0; m <= MARKER_MAX; ++m)
         {
            if (am & 1)
               SendScintilla(SCI_MARKERSETBACK,m,col);

            am >>= 1;
         }
      }
      else if (allocatedMarkers & (1 << mnr))
         SendScintilla(SCI_MARKERSETBACK,mnr,col);
   }
}

/*********************************************************************************************************************
** Set the marker foreground colour.
*/

void ScintillaKTK::panSetMarkerForegroundColor(const QColor &col,int mnr)
{
   if (mnr <= MARKER_MAX)
   {
      if (mnr < 0)
      {
         unsigned am = allocatedMarkers;

         for (int m = 0; m <= MARKER_MAX; ++m)
         {
            if (am & 1)
               SendScintilla(SCI_MARKERSETFORE,m,col);

            am >>= 1;
         }
      }
      else if (allocatedMarkers & (1 << mnr))
         SendScintilla(SCI_MARKERSETFORE,mnr,col);
   }
}

/*********************************************************************************************************************
** Check a marker, allocating a marker number if necessary.
*/

void ScintillaKTK::panCheckMarker(int &mnr)
{
   if (mnr >= 0)
   {
      // Check the explicit marker number isn't already allocated.
      if (mnr > MARKER_MAX or allocatedMarkers & (1 << mnr))
         mnr = -1;
   }
   else
   {
      unsigned am = allocatedMarkers;

      // Find the smallest unallocated marker number.
      for (mnr = 0; mnr <= MARKER_MAX; ++mnr)
      {
         if ((am & 1) IS 0)
            break;

         am >>= 1;
      }
   }

   // Define the marker if it is valid.
   if (mnr >= 0)
      allocatedMarkers |= (1 << mnr);
}

/*********************************************************************************************************************
** Reset the fold margin colours.
*/

void ScintillaKTK::resetFoldMarginColors()
{
   SendScintilla(SCI_SETFOLDMARGINHICOLOUR,0,0L);
   SendScintilla(SCI_SETFOLDMARGINCOLOUR,0,0L);
}

/*********************************************************************************************************************
** Set the fold margin colours.
*/

void ScintillaKTK::panSetFoldMarginColors(const QColor &fore,const QColor &back)
{
   SendScintilla(SCI_SETFOLDMARGINHICOLOUR,1,fore);
   SendScintilla(SCI_SETFOLDMARGINCOLOUR,1,back);
}

/*********************************************************************************************************************
** Handle a change to the user visible user interface.
*/

// Return a position from a line number and an index within the line.
long ScintillaKTK::posFromLineIndex(int line,int index)
{
   long pos = SendScintilla(SCI_POSITIONFROMLINE,line) + index;
   long eol = SendScintilla(SCI_GETLINEENDPOSITION,line);

   if (pos > eol)
      pos = eol;

   return pos;
}


// Return a line number and an index within the line from a position.
void ScintillaKTK::lineIndexFromPos(long pos,int *line,int *index)
{
   long lin = SendScintilla(SCI_LINEFROMPOSITION,pos);
   long linpos = SendScintilla(SCI_POSITIONFROMLINE,lin);

   *line = lin;
   *index = pos - linpos;
}


// Convert a Scintilla string to a Qt Unicode string.
QString ScintillaKTK::convertText(const char *s)
{
   if (isUtf8())
      return QString::fromUtf8(s);

   QString qs;

   qs.setLatin1(s);

   return qs;
}


// Set the source of the auto-completion list.
void ScintillaKTK::setAutoCompletionSource(AutoCompletionSource source)
{
   acSource = source;
}


// Set the threshold for automatic auto-completion.
void ScintillaKTK::setAutoCompletionThreshold(int thresh)
{
   acThresh = thresh;
}


// Set the APIs for auto-completion.
void ScintillaKTK::setAutoCompletionAPIs(ScintillaKTKAPIs *apis)
{
   acAPIs = apis;
}


// Explicitly auto-complete from the APIs.
void ScintillaKTK::autoCompleteFromAPIs()
{
   // If we are not in a word then display all APIs.
   startAutoCompletion(AcsAPIs,FALSE,!currentCharInWord(),showSingle);
}


// Explicitly auto-complete from the document.
void ScintillaKTK::autoCompleteFromDocument()
{
   // If we are not in a word then ignore.
   if (currentCharInWord())
      startAutoCompletion(AcsDocument,FALSE,FALSE,showSingle);
}

/*********************************************************************************************************************
** Return TRUE if the current character (ie. the one before the carat) is part
** of a word.
*/

bool ScintillaKTK::currentCharInWord()
{
   long pos = SendScintilla(SCI_GETCURRENTPOS);
   if (pos <= 0) return FALSE;
   return isWordChar(SendScintilla(SCI_GETCHARAT,pos - 1));
}

/*********************************************************************************************************************
** Registered an image.
*/

void ScintillaKTK::registerImage(int id,const QPixmap *pm)
{
   SendScintilla(SCI_REGISTERIMAGE,id,pm);
}

/*********************************************************************************************************************
** Clear all registered images.
*/

void ScintillaKTK::clearRegisteredImages()
{
   SendScintilla(SCI_CLEARREGISTEREDIMAGES);
}

/*********************************************************************************************************************
** Set the fill-up characters for auto-completion.
*/

void ScintillaKTK::setAutoCompletionFillups(const char *fillups)
{
   SendScintilla(SCI_AUTOCSETFILLUPS,fillups);
}

/*********************************************************************************************************************
** Set the case sensitivity for auto-completion.
*/

void ScintillaKTK::setAutoCompletionCaseSensitivity(bool cs)
{
   SendScintilla(SCI_AUTOCSETIGNORECASE,!cs);
}

/*********************************************************************************************************************
** Return the case sensitivity for auto-completion.
*/

bool ScintillaKTK::autoCompletionCaseSensitivity()
{
   return !SendScintilla(SCI_AUTOCGETIGNORECASE);
}

/*********************************************************************************************************************
** Set the replace word mode for auto-completion.
*/

void ScintillaKTK::setAutoCompletionReplaceWord(bool replace)
{
   SendScintilla(SCI_AUTOCSETDROPRESTOFWORD,replace);
}

/*********************************************************************************************************************
** Return the replace word mode for auto-completion.
*/

bool ScintillaKTK::autoCompletionReplaceWord()
{
   return SendScintilla(SCI_AUTOCGETDROPRESTOFWORD);
}

/*********************************************************************************************************************
** Set the single item mode for auto-completion.
*/

void ScintillaKTK::setAutoCompletionShowSingle(bool single)
{
   showSingle = single;
}

/*********************************************************************************************************************
** Return the single item mode for auto-completion.
*/

bool ScintillaKTK::autoCompletionShowSingle()
{
   return showSingle;
}

#endif
