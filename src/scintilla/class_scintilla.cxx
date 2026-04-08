/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Scintilla: Provides advanced text display and editing facilities.

The Scintilla class provides advanced text editing capabilities that are suitable for modifying text files of any kind,
as well as simple user input features for text input boxes.  The code is based on the Scintilla project at
http://scintilla.org and it may be useful to study the official Scintilla documentation for further insight into its
capabilities.
-END-

*********************************************************************************************************************/

//#define DEBUG
#define NO_SSTRING
#define INTERNATIONAL_INPUT
#define PRV_SCINTILLA

#define DBGDRAW(...) //FMSG

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <limits.h>
#include <string>
#include <sstream>
#include <vector>
#include <map>

#include "Platform.h"

#include "ILexer.h"
#include "Scintilla.h"

#include "PropSetSimple.h"
#ifdef SCI_LEXER
#include "SciLexer.h"
#include "LexerModule.h"
#include "Catalogue.h"
#endif
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "CallTip.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "XPM.h"
#include "LineMarker.h"
#include "Style.h"
#include "ViewStyle.h"
#include "AutoComplete.h"
#include "CharClassify.h"
#include "Decoration.h"
#include "Document.h"
#include "Selection.h"
#include "PositionCache.h"
#include "Editor.h"
#include "ScintillaBase.h"


#include "Platform.h"
#include "Scintilla.h"
#include "ScintillaWidget.h"
#include "Partitioning.h"
#include "RunStyles.h"
#ifdef SCI_LEXER
#include "SciLexer.h"
#include "PropSetSimple.h"
//#include "Accessor.h"
//#include "KeyWords.h"
#endif
#include "CharClassify.h"
#include "ContractionState.h"
#include "SVector.h"
#include "CellBuffer.h"
#include "CallTip.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "XPM.h"
#include "LineMarker.h"
#include "Style.h"
#include "AutoComplete.h"
#include "ViewStyle.h"
#include "Document.h"
#include "Editor.h"
#include "ScintillaBase.h"
#include "UniConversion.h"

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/display.h>
#include <kotuku/modules/font.h>
#include <kotuku/modules/vector.h>
#include <kotuku/strings.hpp>

#include "scintillakotuku.h"

#include "module_def.c"

JUMPTABLE_CORE
JUMPTABLE_DISPLAY
JUMPTABLE_VECTOR
JUMPTABLE_FONT

static OBJECTPTR clScintilla = nullptr;
static OBJECTPTR modDisplay = nullptr, modFont = nullptr, modVector = nullptr;
//static OBJECTID glInputID = 0;
static RGB8 glHighlight = { 220, 220, 255 };
extern OBJECTPTR clScintillaSearch;

struct styledef {
   uint8_t Index;
   uint32_t Colour;
   FTF FontStyle;
};

// This is bad - the fonts should be stored in the class.

static OBJECTPTR glFont = nullptr, glBoldFont = nullptr, glItalicFont = nullptr, glBIFont = nullptr;

static const struct {
   CSTRING File;
   SCLEX Lexer;
} glLexers[] = {
   { "*.asm|*.s",     SCLEX::ASSEMBLER },
   { "*.asp",         SCLEX::ASP },
   { "*.bash",        SCLEX::BASH },
   { "*.bat|*.dos",   SCLEX::BATCH },
   { "*.c|*.cpp|*.cxx|*.h|*.hpp", SCLEX::CPP },
   { "*.css",         SCLEX::CSS },
   { "*.diff",        SCLEX::DIFF },
   { "*.errorlist",   SCLEX::ERRORLIST },
   { "*.lua|*.tiri", SCLEX::TIRI },
   { "*.dmd",         SCLEX::HTML },
   { "*.html",        SCLEX::HTML },
   { "makefile|*.make", SCLEX::MAKEFILE },
   { "*.pas",          SCLEX::PASCAL },
   { "*.perl|*.pl",    SCLEX::PERL },
   { "*.prop|*.cfg",   SCLEX::PROPERTIES },
   { "*.py",           SCLEX::PYTHON },
   { "*.ruby|*.rb",    SCLEX::RUBY },
   { "*.sql",          SCLEX::SQL },
   { "*.vb",           SCLEX::VB },
   { "*.vbscript",     SCLEX::VBSCRIPT },
   { "*.xml",          SCLEX::XML },
   { nullptr, SCLEX::NIL }
};

#define SCICOLOUR(red,green,blue) (((uint8_t)(blue))<<16)|(((uint8_t)(green))<<8)|((uint8_t)(red))
#define SCIRED(c)   (uint8_t)(c)
#define SCIGREEN(c) (uint8_t)(c>>8)
#define SCIBLUE(c)  (uint8_t)(c>>16)
#define SCICALL     Self->API->SendScintilla

//********************************************************************************************************************
// Scintilla class definition.

static ERR GET_AllowTabs(extScintilla *, int *);
static ERR GET_AutoIndent(extScintilla *, int *);
static ERR GET_FileDrop(extScintilla *, FUNCTION **);
static ERR GET_FoldingMarkers(extScintilla *, int *);
static ERR GET_LineCount(extScintilla *, int *);
static ERR GET_LineNumbers(extScintilla *, int *);
static ERR GET_Path(extScintilla *, CSTRING *);
static ERR GET_ShowWhitespace(extScintilla *, int *);
static ERR GET_EventCallback(extScintilla *, FUNCTION **);
static ERR GET_String(extScintilla *, STRING *);
static ERR GET_Symbols(extScintilla *, int *);
static ERR GET_TabWidth(extScintilla *, int *);
static ERR GET_Wordwrap(extScintilla *, int *);

static ERR SET_AllowTabs(extScintilla *, int);
static ERR SET_AutoIndent(extScintilla *, int);
static ERR SET_BkgdColour(extScintilla *, RGB8 *);
static ERR SET_CursorColour(extScintilla *, RGB8 *);
static ERR SET_FileDrop(extScintilla *, FUNCTION *);
static ERR SET_FoldingMarkers(extScintilla *, int);
static ERR SET_LeftMargin(extScintilla *, int);
static ERR SET_Lexer(extScintilla *, SCLEX);
static ERR SET_LineHighlight(extScintilla *, RGB8 *);
static ERR SET_LineNumbers(extScintilla *, int);
static ERR SET_Path(extScintilla *, CSTRING);
static ERR SET_Modified(extScintilla *, int);
static ERR SET_Origin(extScintilla *, CSTRING);
static ERR SET_RightMargin(extScintilla *, int);
static ERR SET_ShowWhitespace(extScintilla *, int);
static ERR SET_EventCallback(extScintilla *, FUNCTION *);
static ERR SET_SelectBkgd(extScintilla *, RGB8 *);
static ERR SET_SelectFore(extScintilla *, RGB8 *);
static ERR SET_String(extScintilla *, CSTRING);
static ERR SET_Symbols(extScintilla *, int);
static ERR SET_TabWidth(extScintilla *, int);
static ERR SET_TextColour(extScintilla *, RGB8 *);
static ERR SET_Wordwrap(extScintilla *, int);

//********************************************************************************************************************

static ERR consume_input_events(const InputEvent *, int);
static void create_styled_fonts(extScintilla *);
static ERR create_scintilla(void);
static void draw_scintilla(extScintilla *, objSurface *, objBitmap *);
static ERR load_file(extScintilla *, CSTRING);
static void calc_longest_line(extScintilla *);
static void key_event(evKey *, int, extScintilla *);
static void report_event(extScintilla *, SEF Event);
static ERR idle_timer(extScintilla *Self, int64_t Elapsed, int64_t CurrentTime);
extern ERR init_search(void);

//********************************************************************************************************************

static bool read_rgb8(CSTRING Value, RGB8 *RGB)
{
   VectorPainter painter;
   if (vec::ReadPainter(nullptr, Value, &painter, nullptr) IS ERR::Okay) {
      RGB->Red   = int(painter.Colour.Red   * 255.0);
      RGB->Green = int(painter.Colour.Green * 255.0);
      RGB->Blue  = int(painter.Colour.Blue  * 255.0);
      RGB->Alpha = int(painter.Colour.Alpha * 255.0);
      return true;
   }
   else return false;
}

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("font", &modFont, &FontBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("vector", &modVector, &VectorBase) != ERR::Okay) return ERR::InitModule;

   OBJECTID id;
   if (FindObject("glStyle", CLASSID::XML, FOF::NIL, &id) IS ERR::Okay) {
      char buffer[40];
      if (acGetKey(GetObjectPtr(id), "/colours/@texthighlight", buffer, sizeof(buffer)) IS ERR::Okay) {
         read_rgb8(buffer, &glHighlight);
      }
   }

   if (init_search() IS ERR::Okay) {
      return create_scintilla();
   }
   else return ERR::AddClass;
}

//********************************************************************************************************************

static ERR MODExpunge(void)
{
   if (modDisplay)  { FreeResource(modDisplay);  modDisplay = nullptr; }
   if (modFont)     { FreeResource(modFont);     modFont = nullptr; }
   if (modVector)   { FreeResource(modVector);   modVector = nullptr; }
   if (clScintilla) { FreeResource(clScintilla); clScintilla = nullptr; }
   if (clScintillaSearch) { FreeResource(clScintillaSearch); clScintillaSearch = nullptr; }
   return ERR::Okay;
}

//********************************************************************************************************************

static void notify_dragdrop(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acDragDrop *Args)
{
   auto Self = (extScintilla *)CurrentContext();

   // There are two drag-drop cases - DATA::TEXT and DATA::FILE.  DATA::TEXT is something that we can handle ourselves,
   // while DATA::FILE is handled via an external function provided by the user.  Refer to the DataFeed action for
   // further code.

   if (!Args) return;

   // Send the source an item request

   struct dcRequest request;
   request.Item          = Args->Item;
   request.Preference[0] = int8_t(DATA::FILE);
   request.Preference[1] = int8_t(DATA::TEXT);
   request.Preference[2] = 0;

   struct acDataFeed dc;
   dc.Object   = Self;
   dc.Datatype = DATA::REQUEST;
   dc.Buffer   = &request;
   dc.Size     = sizeof(request);
   if (Action(AC::DataFeed, Args->Source, &dc) IS ERR::Okay) {
      // The source will return a DATA::RECEIPT for the items that we've asked for (see the DataFeed action).
   }
}

//********************************************************************************************************************

static void notify_focus(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   pf::Log log(__FUNCTION__);
   auto Self = (extScintilla *)CurrentContext();

   if (Result != ERR::Okay) return;

   if (!Self->prvKeyEvent) {
      SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(key_event, Self), &Self->prvKeyEvent);
   }

   if ((Self->Visible) and ((Self->Flags & SCIF::DISABLED) IS SCIF::NIL)) {
      Self->API->panGotFocus();
   }
   else log.msg("(Focus) Cannot receive focus, surface not visible or disabled.");
}

//********************************************************************************************************************

static void notify_free_event(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   ((extScintilla *)CurrentContext())->EventCallback.clear();
}

//********************************************************************************************************************

static void notify_hide(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   // Parent surface has been hidden
   acHide(CurrentContext());
}

//********************************************************************************************************************

static void notify_lostfocus(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   pf::Log log(__FUNCTION__);
   log.branch();

   auto Self = (extScintilla *)CurrentContext();
   if (Self->prvKeyEvent) { UnsubscribeEvent(Self->prvKeyEvent); Self->prvKeyEvent = nullptr; }

   Self->API->panLostFocus();
}

//********************************************************************************************************************

static void notify_show(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   // Parent surface now visible
   acShow(CurrentContext());
}

//********************************************************************************************************************

static void notify_redimension(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acRedimension *Args)
{
   if ((!Args) or (Result != ERR::Okay)) return;

   auto Self = (extScintilla *)CurrentContext();
   bool resized;
   if ((Self->Surface.Width != int(Args->Width)) or (Self->Surface.Height != int(Args->Height))) resized = true;
   else resized = false;

   Self->Surface.X = int(Args->X);
   Self->Surface.Y = int(Args->Y);
   Self->Surface.Width  = int(Args->Width);
   Self->Surface.Height = int(Args->Height);

   if (resized) Self->API->panResized();
}

//********************************************************************************************************************

static void notify_write(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acWrite *Args)
{
   pf::Log log(__FUNCTION__);
   auto Self = (extScintilla *)CurrentContext();

   if (!Args) return;

   if (Result != ERR::Okay) {
      if (Self->FileStream) { FreeResource(Self->FileStream); Self->FileStream = nullptr; }
      return;
   }

   log.msg("%d bytes incoming from file stream.", Args->Result);

   Self->HoldModify = TRUE; // Prevent the file from being marked as modified due to incoming data

   SCICALL(SCI_SETUNDOCOLLECTION, 0UL); // Turn off undo

   if (Args->Buffer) {
      acDataFeed(Self, Self, DATA::TEXT, Args->Buffer, Args->Result);
   }
   else { // We have to read the data from the file stream
   }

   SCICALL(SCI_SETUNDOCOLLECTION, 1UL); // Turn on undo

   Self->HoldModify = FALSE;
}

/*********************************************************************************************************************
-ACTION-
Clear: Clears all content from the editor.

******************************************************************************/

static ERR SCINTILLA_Clear(extScintilla *Self)
{
   pf::Log log;

   log.branch();

   SCICALL(SCI_BEGINUNDOACTION);
   SCICALL(SCI_CLEARALL);
   SCICALL(SCI_ENDUNDOACTION);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clipboard: Full support for clipboard activity is provided through this action.

*********************************************************************************************************************/

static ERR SCINTILLA_Clipboard(extScintilla *Self, struct acClipboard *Args)
{
   pf::Log log;

   if ((!Args) or (Args->Mode IS CLIPMODE::NIL)) return log.warning(ERR::NullArgs);

   if (Args->Mode IS CLIPMODE::CUT) {
      Self->API->Cut();
      return ERR::Okay;
   }
   else if (Args->Mode IS CLIPMODE::COPY) {
      Self->API->Copy();
      return ERR::Okay;
   }
   else if (Args->Mode IS CLIPMODE::PASTE) {
      Self->API->Paste();
      return ERR::Okay;
   }
   else return log.warning(ERR::Args);
}

//*****************************************************************************

static ERR SCINTILLA_DataFeed(extScintilla *Self, struct acDataFeed *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (Args->Datatype IS DATA::TEXT) {
      CSTRING str;

      // Incoming text is appended to the end of the document

      if (!Args->Buffer) str = "";
      else str = (CSTRING)Args->Buffer;

      SCICALL(SCI_APPENDTEXT, strlen(str), str);
   }
   else if (Args->Datatype IS DATA::RECEIPT) {
      log.msg("Received item receipt from object %d.", Args->Object ? Args->Object->UID : 0);

      objXML::create xml = { fl::Statement((CSTRING)Args->Buffer) };
      if (xml.ok()) {
         for (auto &tag : xml->Tags) {
            if (iequals("file", tag.name())) {
               // If the file is being dragged within the same device, it will be moved instead of copied.

               for (auto &a : tag.Attribs) {
                  if (iequals("path", a.Name)) {
                     if (Self->FileDrop.isC()) {
                        pf::SwitchContext ctx(Self->FileDrop.Context);
                        auto routine = (void (*)(extScintilla *, CSTRING, APTR))Self->FileDrop.Routine;
                        routine(Self, a.Value.c_str(), Self->FileDrop.Meta);
                     }
                     else if (Self->FileDrop.isScript()) {
                        ScriptArg args[] = {
                           ScriptArg("Scintilla", Self, FD_OBJECTPTR),
                           ScriptArg("Path",      a.Value.c_str(), FD_STR)
                        };

                        auto script = (objScript *)Self->FileDrop.Context;
                        script->callback(Self->FileDrop.ProcedureID, args, std::ssize(args), nullptr);
                     }
                     break;
                  }
               }
            }
            else if (iequals("text", tag.name())) {
               if ((!tag.Children.empty()) and (tag.Children[0].isContent())) {
                  Self->insertText(tag.Children[0].Attribs[0].Value.c_str(), -1);
               }
            }
         }

         return ERR::Okay;
      }
      else return log.warning(ERR::CreateObject);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DeleteLine: Deletes a line.

This method will delete a single line at the position indicated by the Line parameter.  If no parameters are provided
or the Line parameter is less than zero, the line at the current cursor position will be deleted.  If the index
exceeds the total number of available lines, the last available line will be targeted.

-INPUT-
int Line: The number of the line to delete.  Indexing starts from 0.

-RESULT-
Okay

*********************************************************************************************************************/

static ERR SCINTILLA_DeleteLine(extScintilla *Self, struct sci::DeleteLine *Args)
{
   pf::Log log;
   int line, pos, start, end, linecount;

   linecount = SCICALL(SCI_GETLINECOUNT);

   if ((!Args) or (Args->Line < 0)) {
      pos = SCICALL(SCI_GETCURRENTPOS);
      line = SCICALL(SCI_LINEFROMPOSITION, pos);
   }
   else line = Args->Line;

   log.traceBranch("Line: %d", line);

   // Set the start and end markers.  Some adjustments may be necessary if this is the last line in the document.

   start = SCICALL(SCI_POSITIONFROMLINE, line);
   end = start + SCICALL(SCI_LINELENGTH, line);

   if (line+1 IS linecount) {
      if (line > 0) {
         start = SCICALL(SCI_POSITIONFROMLINE, line-1) + SCICALL(SCI_LINELENGTH, line-1) - 1;
      }

      SCICALL(SCI_GOTOLINE, line-1);
   }

   SCICALL(SCI_SETTARGETSTART, start);
   SCICALL(SCI_SETTARGETEND, end);

   // Delete the targeted text

   SCICALL(SCI_REPLACETARGET, 0UL, "");

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Disable: Disables the target #Surface.

*********************************************************************************************************************/

static ERR SCINTILLA_Disable(extScintilla *Self)
{
   Self->Flags |= SCIF::DISABLED;
   QueueAction(AC::Draw, Self->SurfaceID);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Draws the Scintilla object's graphics.

*********************************************************************************************************************/

static ERR SCINTILLA_Draw(extScintilla *Self, struct acDraw *Args)
{
   pf::ScopedObjectLock surface(Self->SurfaceID);
   if (surface.granted()) Action(AC::Draw, *surface, Args);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Enable: Enables the target #Surface.

*********************************************************************************************************************/

static ERR SCINTILLA_Enable(extScintilla *Self)
{
   Self->Flags &= ~SCIF::DISABLED;
   QueueAction(AC::Draw, Self->SurfaceID);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Focus: Focus on the Scintilla surface.
-END-
*********************************************************************************************************************/

static ERR SCINTILLA_Focus(extScintilla *Self)
{
   pf::ScopedObjectLock surface(Self->SurfaceID);
   if (surface.granted()) return acFocus(*surface);
   else return ERR::AccessObject;
}

//********************************************************************************************************************

static ERR SCINTILLA_Free(extScintilla *Self, APTR)
{
   pf::Log log;

   delete Self->API;
   Self->API = nullptr;

   if (Self->TimerID) { UpdateTimer(Self->TimerID, 0); Self->TimerID = 0; }

   if ((Self->FocusID) and (Self->FocusID != Self->SurfaceID)) {

      if (pf::ScopedObjectLock object(Self->FocusID, 500); object.granted()) {
         UnsubscribeAction(*object, AC::NIL);
      }
   }

   if (Self->SurfaceID) {
      if (pf::ScopedObjectLock<objSurface> object(Self->SurfaceID, 500); object.granted()) {
         object->removeCallback(C_FUNCTION(&draw_scintilla));
         UnsubscribeAction(*object, AC::NIL);
      }
   }

   /*if (Self->PointerLocked) {
      RestoreCursor(PTR_DEFAULT, Self->UID);
      Self->PointerLocked = FALSE;
   }*/

   if (Self->prvKeyEvent)  { UnsubscribeEvent(Self->prvKeyEvent); Self->prvKeyEvent = nullptr; }
   if (Self->FileStream)   { FreeResource(Self->FileStream); Self->FileStream = nullptr; }
   if (Self->Path)         { FreeResource(Self->Path);  Self->Path = nullptr; }
   if (Self->StringBuffer) { FreeResource(Self->StringBuffer); Self->StringBuffer = nullptr; }
   if (Self->Font)         { FreeResource(Self->Font);       Self->Font = nullptr; }
   if (Self->BoldFont)     { FreeResource(Self->BoldFont);   Self->BoldFont = nullptr; }
   if (Self->ItalicFont)   { FreeResource(Self->ItalicFont); Self->ItalicFont = nullptr; }
   if (Self->BIFont)       { FreeResource(Self->BIFont);     Self->BIFont = nullptr; }

   gfx::UnsubscribeInput(Self->InputHandle);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetLine: Copies the text content of any line to a user-supplied buffer.

This method will retrieve the string for a `Line` at a given index.  The string is copied to a user supplied
`Buffer` of the indicated `Length` (in bytes).

-INPUT-
int Line: The index of the line to retrieve.
buf(str) Buffer: The destination buffer.
bufsize Length: The byte size of the `Buffer`.

-RESULT-
Okay:
NullArgs:
OutOfRange: At least one of the parameters is out of range.
BufferOverflow: The supplied `Buffer` is not large enough to contain the result.

*********************************************************************************************************************/

static ERR SCINTILLA_GetLine(extScintilla *Self, struct sci::GetLine *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Buffer)) return log.warning(ERR::NullArgs);
   if ((Args->Line < 0) or (Args->Length < 1)) return log.warning(ERR::OutOfRange);

   int len = SCICALL(SCI_LINELENGTH, Args->Line); // Returns the length of the line (in bytes) including line-end characters (NB: there could be more than one line-end character!)
   if (Args->Length > len) {
      SCICALL(SCI_GETLINE, Args->Line, Args->Buffer);
      Args->Buffer[len] = 0;
      return ERR::Okay;
   }
   else return ERR::BufferOverflow;
}

/*********************************************************************************************************************

-METHOD-
GetPos: Returns the byte position of a given line and column number.

This method converts a `Line` and `Column` index to the equivalent byte position within the text document.

-INPUT-
int Line: Line index
int Column: Column index
&int Pos: The byte position is returned in this parameter.

-RESULT-
Okay:
NullArgs:

*********************************************************************************************************************/

static ERR SCINTILLA_GetPos(extScintilla *Self, struct sci::GetPos *Args)
{
   if (!Args) return ERR::NullArgs;

   Args->Pos = SCICALL(SCI_FINDCOLUMN, Args->Line, Args->Column);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GotoLine: Moves the cursor to any line in the document.

This method moves the cursor to a given line index.  If the index is greater than the total number of available lines,
the cursor is moved to the last line in the document.  A line index of less than zero is invalid.

-INPUT-
int Line: The line index to move to.

-RESULT-
Okay:
NullArgs:
OutOfRange: The Line is less than zero.

*********************************************************************************************************************/

static ERR SCINTILLA_GotoLine(extScintilla *Self, struct sci::GotoLine *Args)
{
   pf::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if (Args->Line < 0) return ERR::OutOfRange;

   log.branch("Line: %d", Args->Line);
   SCICALL(SCI_GOTOLINE, Args->Line);
   return ERR::Okay;
}

//*****************************************************************************

static ERR SCINTILLA_Hide(extScintilla *Self)
{
   if (Self->Visible) {
      pf::Log log;

      log.branch();

      Self->Visible = FALSE;
      acDraw(Self);
   }

   return ERR::Okay;
}

//*****************************************************************************

static ERR SCINTILLA_Init(extScintilla *Self, APTR)
{
   pf::Log log;

   if (!Self->SurfaceID) return log.warning(ERR::UnsupportedOwner);

   if (!Self->FocusID) Self->FocusID = Self->SurfaceID;

   // Subscribe to the object responsible for the user focus

   if (pf::ScopedObjectLock object(Self->FocusID, 5000); object.granted()) {
      SubscribeAction(*object, AC::Focus, C_FUNCTION(notify_focus));
      SubscribeAction(*object, AC::LostFocus, C_FUNCTION(notify_lostfocus));
   }

   // Set up the target surface

   log.trace("Configure target surface #%d", Self->SurfaceID);

   if (pf::ScopedObjectLock<objSurface> surface(Self->SurfaceID, 3000); surface.granted()) {
      surface->setFlags(surface->Flags|RNF::GRAB_FOCUS);

      Self->Surface.X = surface->X;
      Self->Surface.Y = surface->Y;
      Self->Surface.Width  = surface->Width;
      Self->Surface.Height = surface->Height;

      surface->addCallback(C_FUNCTION(draw_scintilla));

      //SubscribeFeed(surface); TODO: Deprecated

      SubscribeAction(*surface, AC::DragDrop, C_FUNCTION(notify_dragdrop));
      SubscribeAction(*surface, AC::Hide, C_FUNCTION(notify_hide));
      SubscribeAction(*surface, AC::Redimension, C_FUNCTION(notify_redimension));
      SubscribeAction(*surface, AC::Show, C_FUNCTION(notify_show));

      if (surface->hasFocus()) {
         SubscribeEvent(EVID_IO_KEYBOARD_KEYPRESS, C_FUNCTION(key_event, Self), &Self->prvKeyEvent);
      }
   }
   else return log.warning(ERR::AccessObject);

   {
      auto callback = C_FUNCTION(consume_input_events);
      gfx::SubscribeInput(&callback, Self->SurfaceID, JTYPE::MOVEMENT|JTYPE::BUTTON, 0, &Self->InputHandle);
   }

   // TODO: Run the scrollbar script here

   if (InitObject(Self->Font) != ERR::Okay) return ERR::Init;

   create_styled_fonts(Self);

   // Create a Scintilla class object, passing it the target surface and a pointer to our own structure to link us
   // together.

   if (!(Self->API = new ScintillaKTK(Self->SurfaceID, Self))) {
      return ERR::Failed;
   }

   Self->API->panFontChanged(Self->Font, Self->BoldFont, Self->ItalicFont, Self->BIFont);

   // Load a text file if required

   if (Self->Path) {
      if (load_file(Self, Self->Path) != ERR::Okay) {
         return ERR::File;
      }
   }
   else calc_longest_line(Self);

   SubscribeTimer(0.03, C_FUNCTION(idle_timer), &Self->TimerID);

   if (Self->Visible IS -1) Self->Visible = TRUE;

   if (((Self->Flags & SCIF::DETECT_LEXER) IS SCIF::NIL) and (Self->Lexer != SCLEX::NIL)) {
      Self->API->SetLexer(int(Self->Lexer));
   }

   QueueAction(AC::Draw, Self->SurfaceID);

   if (Self->LongestWidth) SCICALL(SCI_SETSCROLLWIDTH, Self->LongestWidth);
   else SCICALL(SCI_SETSCROLLWIDTH, 1UL);

   if ((Self->Flags & SCIF::EXT_PAGE) != SCIF::NIL) {
      log.msg("Extended page mode.");
      SCICALL(SCI_SETENDATLASTLINE, 0UL); // Allow scrolling by an extra page at the end of the document
   }
   else SCICALL(SCI_SETENDATLASTLINE, 1UL);

   SCICALL(SCI_SETMARGINLEFT, 0, Self->LeftMargin);
   SCICALL(SCI_SETMARGINRIGHT, 0, 0L);

   SCICALL(SCI_SETTABWIDTH, Self->TabWidth);

   // Selected text will be inversed with these colours

   SCICALL(SCI_SETSELFORE, (unsigned long int)true, (long int)SCICOLOUR(Self->SelectFore.Red, Self->SelectFore.Green, Self->SelectFore.Blue));
   SCICALL(SCI_SETSELBACK, (unsigned long int)true, (long int)SCICOLOUR(Self->SelectBkgd.Red, Self->SelectBkgd.Green, Self->SelectBkgd.Blue));
   //SCICALL(SCI_SETSELALPHA, Self->SelectBkgd.Alpha); // Currently doesn't work as expected

   // Enable line colour for the line that contains the text cursor

   SCICALL(SCI_SETCARETLINEBACK, SCICOLOUR(Self->LineHighlight.Red, Self->LineHighlight.Green, Self->LineHighlight.Blue));
   if (Self->LineHighlight.Alpha > 0) {
      SCICALL(SCI_SETCARETLINEVISIBLE, 1UL);
      //SCICALL(SCI_SETCARETLINEBACKALPHA, Self->LineHighlight.Alpha); // Not working currently - maybe a drawing issue?
   }
   else SCICALL(SCI_SETCARETLINEVISIBLE, 0UL);

   SCICALL(SCI_SETCARETFORE, SCICOLOUR(Self->CursorColour.Red, Self->CursorColour.Green, Self->CursorColour.Blue));
   SCICALL(SCI_SETCARETWIDTH, 2);

#ifdef _DEBUG
   SCICALL(SCI_SETCARETPERIOD, 0UL);
#endif

   // Show whitespace characters like tabs

   if (Self->ShowWhitespace) SCICALL(SCI_SETVIEWWS, (long unsigned int)SCWS_VISIBLEALWAYS);
   else SCICALL(SCI_SETVIEWWS, (long unsigned int)SCWS_INVISIBLE);

   // Turn off buffered drawing because Kotuku surfaces already include buffer support

   SCICALL(SCI_SETBUFFEREDDRAW, 0UL);

   // Caret visibility policy

   SCICALL(SCI_SETYCARETPOLICY, CARET_SLOP|CARET_STRICT|CARET_EVEN, 3);
   SCICALL(SCI_SETXCARETPOLICY, CARET_SLOP|CARET_STRICT|CARET_EVEN, Self->RightMargin);

   // Caret visibility policy (folding margins)

   SCICALL(SCI_SETVISIBLEPOLICY, VISIBLE_STRICT|VISIBLE_SLOP, 4);

   // Miscellaneous options

   SCICALL(SCI_SETEOLMODE, SC_EOL_LF);
   SCICALL(SCI_SETPROPERTY, "fold","0");

   if (Self->AllowTabs) SCICALL(SCI_SETUSETABS, 1UL);
   else  SCICALL(SCI_SETUSETABS, 0UL);

   // Set all special margins to invisible (note that the values indicate the pixel width of the margin)

   if (Self->LineNumbers) SCICALL(SCI_SETMARGINWIDTHN, 0, 50L);
   else SCICALL(SCI_SETMARGINWIDTHN, 0, 0L);

   if (Self->Symbols) SCICALL(SCI_SETMARGINWIDTHN, 1, 20L);
   else SCICALL(SCI_SETMARGINWIDTHN, 1, 0L);

   if (Self->FoldingMarkers) SCICALL(SCI_SETMARGINWIDTHN, 2, 20L);
   else SCICALL(SCI_SETMARGINWIDTHN, 2, 0L);

   if (Self->Wordwrap) SCICALL(SCI_SETWRAPMODE, 1UL);
   else SCICALL(SCI_SETWRAPMODE, 0UL);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
InsertText: Inserts text into a document.

Use InsertText to insert a string at any point in the document (if adding text to the end of the document, we recommend
using data channels instead).

You will need to specify the character position at which the provided String should be inserted.  Two special character
positions are also supported as an alternative - a value of -1 inserts the text at the current cursor position and a
value of -2 replaces currently selected text.

-INPUT-
cstr String: A text string to add.
int Pos: -1 inserts at the current cursor position, -2 replaces currently selected text, zero or above inserts at the character index indicated.

-RESULT-
Okay
NullArgs
OutOfRange

*********************************************************************************************************************/

static ERR SCINTILLA_InsertText(extScintilla *Self, struct sci::InsertText *Args)
{
   pf::Log log;
   int pos;

   if ((!Args) or (!Args->String)) return log.warning(ERR::NullArgs);

   log.branch("Pos: %d, Text: %.10s", Args->Pos, Args->String);

   pos = Args->Pos;
   if (pos IS -1) {
      // Get the current cursor position
      pos = SCICALL(SCI_GETCURRENTPOS);
   }
   else if (pos IS -2) {
      // Replace currently selected text

      SCICALL(SCI_BEGINUNDOACTION);
      SCICALL(SCI_REPLACESEL, 0UL, Args->String);
      SCICALL(SCI_ENDUNDOACTION);
      return ERR::Okay;
   }
   else if (pos < -1) {
      return log.warning(ERR::OutOfRange);
   }

   SCICALL(SCI_BEGINUNDOACTION);
   SCICALL(SCI_INSERTTEXT, pos, Args->String);
   SCICALL(SCI_ENDUNDOACTION);
   return ERR::Okay;
}

//*****************************************************************************

static ERR SCINTILLA_NewObject(extScintilla *Self, APTR)
{
   if (NewLocalObject(CLASSID::FONT, (OBJECTPTR *)&Self->Font) IS ERR::Okay) {
      Self->Font->setFace("courier:10");
      Self->LeftMargin  = 4;
      Self->RightMargin = 30;
      Self->AutoIndent  = TRUE;
      Self->TabWidth    = 8;
      Self->AllowTabs   = FALSE;

      Self->BkgdColour.Red   = 255;
      Self->BkgdColour.Green = 255;
      Self->BkgdColour.Blue  = 255;
      Self->BkgdColour.Alpha = 255;

      Self->LineHighlight.Red   = 240;
      Self->LineHighlight.Green = 240;
      Self->LineHighlight.Blue  = 255;
      Self->LineHighlight.Alpha = 255;

      Self->CursorColour.Red   = 0;
      Self->CursorColour.Green = 0;
      Self->CursorColour.Blue  = 0;
      Self->CursorColour.Alpha = 255;

      Self->SelectFore.Red   = 255;
      Self->SelectFore.Green = 255;
      Self->SelectFore.Blue  = 255;
      Self->SelectFore.Alpha = 255;

      Self->SelectBkgd.Red   = 0;
      Self->SelectBkgd.Green = 0;
      Self->SelectBkgd.Blue  = 180;
      Self->SelectBkgd.Alpha = 255;
   }
   else return ERR::NewObject;

   return ERR::Okay;
}

//*****************************************************************************

static ERR SCINTILLA_NewOwner(extScintilla *Self, struct acNewOwner *Args)
{
   if (!Self->initialised()) {
      auto obj = Args->NewOwner;
      while ((obj) and (obj->classID() != CLASSID::SURFACE)) {
         obj = obj->Owner;
      }
      if (obj) Self->SurfaceID = obj->UID;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Redo: Redo the most recently undone activity.

*********************************************************************************************************************/

static ERR SCINTILLA_Redo(extScintilla *Self, struct acRedo *Args)
{
   pf::Log log;

   log.branch();

   SCICALL(SCI_REDO);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ReplaceLine: Replaces a line with new text content.

This method will replace an entire line with a new text string.  If the new string contains line feeds, then multiple
lines will be inserted at the target position.  It is possible to limit the number of characters inserted from the
source string by setting the Length parameter.  To insert all characters from the source string, set a Length of -1.

-INPUT-
int Line: Index of the line being targeted.
cstr String: The new string that will replace the line.
int Length: The number of characters to replace the target with, or -1 for the entire source string.

-RESULT-
Okay:
NullArgs:
OutOfRange: The line index is less than zero or greater than the available number of lines.

*********************************************************************************************************************/

static ERR SCINTILLA_ReplaceLine(extScintilla *Self, struct sci::ReplaceLine *Args)
{
   pf::Log log;

   if (!Args) return ERR::NullArgs;
   if (Args->Line < 0) return log.warning(ERR::OutOfRange);

   // Select the line, then replace the text

   int start, end;
   if ((start = SCICALL(SCI_POSITIONFROMLINE, Args->Line)) < 0) return log.warning(ERR::OutOfRange);
   if ((end = SCICALL(SCI_GETLINEENDPOSITION, Args->Line)) < 0) return log.warning(ERR::OutOfRange);
   SCICALL(SCI_SETTARGETSTART, start);
   SCICALL(SCI_SETTARGETEND, end);

   // Replace the targeted text

   SCICALL(SCI_REPLACETARGET, Args->Length, Args->String);

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ReplaceText: Replaces all text within an entire document or limited range.

The ReplaceText method will replace all instances of the Find string with the content of the Replace string, between a
given Start and End point.  The `STF::CASE`, `STF::SCAN_SELECTION` and `STF::EXPRESSION` are valid flag options for
this method (see FindText for details).

-INPUT-
cstr Find: The keyword string to find.
cstr Replace: The string that will replace the keyword.
int(STF) Flags: Optional flags.
int Start: The start of the search - set to zero if covering the entire document.  If -1, starts from the current cursor position.
int End: The end of the search - set to -1 if covering the entire document.

-RESULT-
Okay: At least one keyword was successfully replaced.
Args:
Search: The keyword could not be found.

*********************************************************************************************************************/

static ERR SCINTILLA_ReplaceText(extScintilla *Self, struct sci::ReplaceText *Args)
{
   pf::Log log;
   int start, end;

   if ((!Args) or (!Args->Find) or (!*Args->Find)) return log.warning(ERR::NullArgs);

   log.branch("Text: '%.10s'... Between: %d - %d, Flags: $%.8x", Args->Find, Args->Start, Args->End, int(Args->Flags));

   // Calculate the start and end positions

   if ((Args->Flags & STF::SCAN_SELECTION) != STF::NIL) {
      start = SCICALL(SCI_GETSELECTIONSTART);
      end   = SCICALL(SCI_GETSELECTIONEND);
   }
   else {
      if (Args->Start < 0) start = SCICALL(SCI_GETCURRENTPOS);
      else start = Args->Start;

      if (Args->End < 0) end = SCICALL(SCI_GETLENGTH);
      else end = Args->End;

      if (start IS end) return ERR::Search;
   }

   CSTRING replace;
   if (!Args->Replace) replace = "";
   else replace = Args->Replace;

   SCICALL(SCI_SETTARGETSTART, start);
   SCICALL(SCI_SETTARGETEND, end);

   int findlen = strlen(Args->Find);
   int replacelen = strlen(replace);

   int flags = (((Args->Flags & STF::CASE) != STF::NIL) ? SCFIND_MATCHCASE : 0) |
                (((Args->Flags & STF::EXPRESSION) != STF::NIL) ? SCFIND_REGEXP : 0);

   SCICALL(SCI_SETSEARCHFLAGS, flags);

   SCICALL(SCI_BEGINUNDOACTION);

   int pos = 0;
   while (pos != -1) {
      log.trace("Search between %d - %d", start, end);

      SCICALL(SCI_SETTARGETSTART, start);
      SCICALL(SCI_SETTARGETEND, end);

      pos = SCICALL(SCI_SEARCHINTARGET, findlen, (char *)Args->Find);

      if (pos != -1) {
         log.trace("Found keyword at %d", pos);
         //targstart  = SCICALL(SCI_GETTARGETSTART);
         //targend    = SCICALL(SCI_GETTARGETEND);

         // Do the replace

         if ((Args->Flags & STF::EXPRESSION) != STF::NIL) {
            int len = SCICALL(SCI_REPLACETARGETRE, (long unsigned int)-1, replace);
            end = end + (len - findlen);
         }
         else {
            SCICALL(SCI_REPLACETARGET, (uint32_t)-1, replace);
            end = end + (replacelen - findlen);
         }
      }
      else log.trace("Keyword not found.");
   }

   SCICALL(SCI_ENDUNDOACTION);
   return ERR::Okay;
}

/*********************************************************************************************************************
-METHOD-
ReportEvent: Private.  For internal use only.

Private

*********************************************************************************************************************/

static ERR SCINTILLA_ReportEvent(extScintilla *Self)
{
   if (Self->ReportEventFlags IS SEF::NIL) return ERR::Okay;

   auto flags = Self->ReportEventFlags;
   Self->ReportEventFlags = SEF::NIL;
   report_event(Self, flags);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Save content as a text stream to another object.

*********************************************************************************************************************/

static ERR SCINTILLA_SaveToObject(extScintilla *Self, struct acSaveToObject *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Dest)) return log.warning(ERR::NullArgs);

   int len = SCICALL(SCI_GETLENGTH);

   log.branch("To: %d, Size: %d", Args->Dest->UID, len);

   ERR error;
   APTR buffer;
   if (AllocMemory(len+1, MEM::STRING|MEM::NO_CLEAR, &buffer) IS ERR::Okay) {
      SCICALL(SCI_GETTEXT, len+1, (const char *)buffer);
      error = acWrite(Args->Dest, buffer, len, nullptr);
      FreeResource(buffer);
   }
   else error = ERR::AllocMemory;

   return error;
}

/*********************************************************************************************************************

-METHOD-
SetFont: Changes the font that is used for text display.

Call SetFont() to change the font face that is used for displaying text.  The string format follows the standard for
font requests, e.g. `Helvete:12:Bold Italic:#ff0000`.  Refer to the Face field in the @Font class for more
details.

If the new face is invalid or fails to load, the current font will remain unchanged.

-INPUT-
cstr Face: The name of the new font face.

-RESULT-
Okay:
NullArgs:
CreateObject: Failed to create a Font object.
-END-

*********************************************************************************************************************/

static ERR SCINTILLA_SetFont(extScintilla *Self, struct sci::SetFont *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Face)) return log.warning(ERR::NullArgs);

   log.branch("%s", Args->Face);

   if ((Self->Font = objFont::create::local(fl::Face(Args->Face)))) {
      create_styled_fonts(Self);
      Self->API->panFontChanged(Self->Font, Self->BoldFont, Self->ItalicFont, Self->BIFont);
      calc_longest_line(Self);
      return ERR::Okay;
   }
   else return ERR::CreateObject;
}

/*********************************************************************************************************************

-METHOD-
ScrollToPoint: Scrolls text by moving the Page.

This method will scroll text in the Scintilla document by moving the page position.

-INPUT-
int X: New horizontal position.
int Y: New vertical position.

-RESULT-
Okay:

*********************************************************************************************************************/

static ERR SCINTILLA_ScrollToPoint(extScintilla *Self, struct sci::ScrollToPoint *Args)
{
   pf::Log log;

   log.traceBranch("Sending Scroll requests to Scintilla: %dx%d.", Args->X, Args->Y);

   Self->ScrollLocked++;

   Self->API->panScrollToX(Args->X);
   Self->API->panScrollToY(Args->Y);

   Self->ScrollLocked--;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SelectRange: Selects a range of text, can also deselect all text.

This method will select an area of text between a start and end point, measured in characters.  It can also deselect
all text if no arguments are provided.

-INPUT-
int Start: The character at which the selection will start.
int End: The character at which the selection will end.  If negative, the last character in the document will be targeted.

-RESULT-
Okay:

*********************************************************************************************************************/

static ERR SCINTILLA_SelectRange(extScintilla *Self, struct sci::SelectRange *Args)
{
   pf::Log log;

   if ((!Args) or ((!Args->Start) and (!Args->End))) { // Deselect all text
      int pos = SCICALL(SCI_GETCURRENTPOS);
      SCICALL(SCI_SETANCHOR, pos);
      return ERR::Okay;
   }

   log.branch("Selecting area %d to %d", Args->Start, Args->End);

   if (Args->End < 0) {
      int linecount = SCICALL(SCI_GETLINECOUNT);
      int end = SCICALL(SCI_FINDCOLUMN, linecount, 0L);
      SCICALL(SCI_SETSEL, Args->Start, end);
      SCICALL(SCI_SCROLLCARET);
   }
   else {
      SCICALL(SCI_SETSEL, Args->Start, Args->End);
      SCICALL(SCI_SCROLLCARET);
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SCINTILLA_Show(extScintilla *Self)
{
   if (!Self->Visible) {
      pf::Log log;

      log.branch();

      Self->Visible = TRUE;

      acDraw(Self);

      return ERR::Okay;
   }
   else return ERR::Okay|ERR::Notified;
}

/*********************************************************************************************************************

-METHOD-
TrimWhitespace: Strips trailing white-space from the document.

The TrimWhitespace method will remove trailing white-space from every line in the document.  Both tabs and spaces are
considered white-space - all other characters shall be treated as content.

The position of the cursor is reset to the left margin as a result of calling this method.

*********************************************************************************************************************/

static ERR SCINTILLA_TrimWhitespace(extScintilla *Self)
{
   pf::Log log;

   log.traceBranch();

   int cursorpos = SCICALL(SCI_GETCURRENTPOS);
   int cursorline = SCICALL(SCI_LINEFROMPOSITION, cursorpos);

   SCICALL(SCI_BEGINUNDOACTION);

   int maxLines = SCICALL(SCI_GETLINECOUNT);
   for (int line=0; line < maxLines; line++) {
      int lineStart = SCICALL(SCI_POSITIONFROMLINE, line);
      int lineEnd = SCICALL(SCI_GETLINEENDPOSITION, line);
      int i = lineEnd - 1;
      uint8_t ch = SCICALL(SCI_GETCHARAT, i);
      while ((i >= lineStart) and ((ch IS ' ') or (ch IS '\t'))) {
         i--;
         ch = SCICALL(SCI_GETCHARAT, i);
      }

      if (i < (lineEnd-1)) {
         SCICALL(SCI_SETTARGETSTART, i + 1);
         SCICALL(SCI_SETTARGETEND, lineEnd);
         SCICALL(SCI_REPLACETARGET, 0UL, "");
      }
   }

   SCICALL(SCI_ENDUNDOACTION);
   SCICALL(SCI_GOTOLINE, cursorline);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Undo: Undo the last user action.

*********************************************************************************************************************/

static ERR SCINTILLA_Undo(extScintilla *Self, struct acUndo *Args)
{
   pf::Log log;

   log.branch();

   SCICALL(SCI_UNDO);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AllowTabs: If enabled, use of the tab key produces real tabs and not spaces.

*********************************************************************************************************************/

static ERR GET_AllowTabs(extScintilla *Self, int *Value)
{
   *Value = Self->AllowTabs;
   return ERR::Okay;
}

static ERR SET_AllowTabs(extScintilla *Self, int Value)
{
   if (Value) {
      Self->AllowTabs = TRUE;
      if (Self->initialised()) SCICALL(SCI_SETUSETABS, 1UL);
   }
   else {
      Self->AllowTabs = FALSE;
      if (Self->initialised()) SCICALL(SCI_SETUSETABS, 0UL);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AutoIndent: If TRUE, enables auto-indenting when the user presses the enter key.

*********************************************************************************************************************/

static ERR GET_AutoIndent(extScintilla *Self, int *Value)
{
   *Value = Self->AutoIndent;
   return ERR::Okay;
}

static ERR SET_AutoIndent(extScintilla *Self, int Value)
{
   if (Value) Self->AutoIndent = 1;
   else Self->AutoIndent = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
BkgdColour: Defines the background colour.  Alpha blending is not supported.

*********************************************************************************************************************/

static ERR SET_BkgdColour(extScintilla *Self, RGB8 *Value)
{
   Self->BkgdColour = *Value;

   if (Self->initialised()) {
      SCICALL(SCI_STYLESETBACK, STYLE_DEFAULT, (long int)SCICOLOUR(Self->BkgdColour.Red, Self->BkgdColour.Green, Self->BkgdColour.Blue));
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CursorCol: The current column of the text cursor.

The CursorCol and #CursorRow fields reflect the current row and column of the user's text cursor.  The values
are updated every time that the cursor is moved.  Use #EventCallback and listen for the event SEF_CURSOR_POS to
receive updates on changes to CursorCol and #CursorRow.

-FIELD-
CursorRow: The current row of the text cursor.

The #CursorCol and CursorRow fields reflect the current row and column of the user's text cursor.  The values
are updated every time that the cursor is moved.  Use #EventCallback and listen for the event SEF_CURSOR_POS to
receive updates on changes to #CursorCol and CursorRow.

-FIELD-
CursorColour: Defines the colour of the text cursor.  Alpha blending is not supported.

*********************************************************************************************************************/

static ERR SET_CursorColour(extScintilla *Self, RGB8 *Value)
{
   Self->CursorColour = *Value;

   if (Self->initialised()) {
      SCICALL(SCI_SETCARETFORE, STYLE_DEFAULT, (long int)SCICOLOUR(Self->CursorColour.Red, Self->CursorColour.Green, Self->CursorColour.Blue));
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FileDrop: A callback for receiving drag and drop file notifications.

Set this field with a reference to a callback function to receive notifications when the user drops a file onto the
Scintilla object's surface.  The prototype for the callback function is `ERR Function(*Scintilla, CSTRING Path)`

If multiple files are dropped, the callback will be repeatedly called until all of the file paths have been reported.

*********************************************************************************************************************/

static ERR GET_FileDrop(extScintilla *Self, FUNCTION **Value)
{
   if (Self->FileDrop.defined()) {
      *Value = &Self->FileDrop;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_FileDrop(extScintilla *Self, FUNCTION *Value)
{
   if (Value) Self->FileDrop = *Value;
   else Self->FileDrop.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.

-FIELD-
Focus: Defines the object that is monitored for user focus changes.

By default, the user focus is managed by monitoring the target #Surface for changes (for instance, clicking
on or away from the surface will result in a focus state change).  If another object should be monitored for focus
state changes, it can be defined here prior to initialisation.

-FIELD-
FoldingMarkers: Folding markers in the left margin will be visible when this value is TRUE.

*********************************************************************************************************************/

static ERR GET_FoldingMarkers(extScintilla *Self, int *Value)
{
   *Value = Self->FoldingMarkers;
   return ERR::Okay;
}

static ERR SET_FoldingMarkers(extScintilla *Self, int Value)
{
   if (Value) {
      Self->FoldingMarkers = TRUE;
      if (Self->initialised()) SCICALL(SCI_SETMARGINWIDTHN, 2, 20L);
   }
   else {
      Self->FoldingMarkers = FALSE;
      if (Self->initialised()) SCICALL(SCI_SETMARGINWIDTHN, 2, 0L);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Font: Refers to the font that is used for drawing text in the document.

This field refers to the font object that is used for drawing text in the document.  It is recommended that all
font customisation takes place prior to initialisation of the Scintilla object.  Directly altering the font object
after initialisation may result in clashes with the Scintilla class that produce unpredictable results.

To change the font post-initialisation, please use the #SetFont() method.

-FIELD-
LeftMargin: The amount of white-space at the left side of the page.

*********************************************************************************************************************/

static ERR SET_LeftMargin(extScintilla *Self, int Value)
{
   if ((Value >= 0) and (Value <= 100)) {
      Self->LeftMargin = Value;
      if (Self->initialised()) {
         SCICALL(SCI_SETMARGINLEFT, 0, Self->LeftMargin);
      }
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
Lexer: The lexer for document styling is defined here.

The lexer for document styling is defined here.

*********************************************************************************************************************/

static ERR SET_Lexer(extScintilla *Self, SCLEX Value)
{
   Self->Lexer = Value;
   if (Self->initialised()) {
      pf::Log log;
      log.branch("Changing lexer to %d", int(Value));
      Self->API->SetLexer(int(Self->Lexer));
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LineCount: The total number of lines in the document.

*********************************************************************************************************************/

static ERR GET_LineCount(extScintilla *Self, int *Value)
{
   if (Self->initialised()) {
      *Value = SCICALL(SCI_GETLINECOUNT);
      return ERR::Okay;
   }
   else return ERR::NotInitialised;
}

/*********************************************************************************************************************

-FIELD-
LineHighlight: The colour to use when highlighting the line that contains the user's cursor.

*********************************************************************************************************************/

static ERR SET_LineHighlight(extScintilla *Self, RGB8 *Value)
{
   Self->LineHighlight = *Value;

   if (Self->initialised()) {
      SCICALL(SCI_SETCARETLINEBACK, SCICOLOUR(Self->LineHighlight.Red, Self->LineHighlight.Green, Self->LineHighlight.Blue));
      if (Self->LineHighlight.Alpha > 0) {
         SCICALL(SCI_SETCARETLINEVISIBLE, 1UL);
         //SCICALL(SCI_SETCARETLINEBACKALPHA, Self->LineHighlight.Alpha);
      }
      else SCICALL(SCI_SETCARETLINEVISIBLE, 0UL);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LineNumbers: Line numbers will appear on the left when this value is TRUE.

*********************************************************************************************************************/

static ERR GET_LineNumbers(extScintilla *Self, int *Value)
{
   *Value = Self->LineNumbers;
   return ERR::Okay;
}

static ERR SET_LineNumbers(extScintilla *Self, int Value)
{
   if (Value) {
      Self->LineNumbers = TRUE;
      if (Self->initialised()) SCICALL(SCI_SETMARGINWIDTHN, 0, 50L);
   }
   else {
      Self->LineNumbers = FALSE;
      if (Self->initialised()) SCICALL(SCI_SETMARGINWIDTHN, 0, 0L);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Path: Identifies the location of a text file to load.

To load data from a text file into a scintilla object, set the Path field.

If the Path is set after initialisation, the existing content will be cleared and data loaded from the location
that you specify.  To change the path without automatically loading from the source file, set the
#Origin field instead.

*********************************************************************************************************************/

static ERR GET_Path(extScintilla *Self, CSTRING *Value)
{
   *Value = Self->Path;
   return ERR::Okay;
}

static ERR SET_Path(extScintilla *Self, CSTRING Value)
{
   pf::Log log;

   log.branch("%s", Value);

   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }

   if ((Value) and (*Value)) {
      if ((Self->Path = strclone(Value))) {
         if (Self->initialised()) {
            if (load_file(Self, Self->Path) != ERR::Okay) {
               return ERR::File;
            }
         }
      }
      else return ERR::AllocMemory;
   }

   return ERR::Okay;
}

/****************************************************************************

-FIELD-
Origin: Similar to the Path field, but does not automatically load content if set.

This field is identical to the #Path field, with the exception that it does not update the content of a
scintilla object if it is set after initialisation.  This may be useful if the origin of the currently loaded content
needs to be changed without causing a load operation.

****************************************************************************/

static ERR SET_Origin(extScintilla *Self, CSTRING Value)
{
   if (Self->Path) { FreeResource(Self->Path); Self->Path = nullptr; }

   if ((Value) and (*Value)) {
      if (!(Self->Path = strclone(Value))) return ERR::AllocMemory;
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Modified:  Returns `true` if the document has been modified and not saved.

The Modified field controls the modification state of the document.  It is automatically changed to a value of TRUE
when the user edits the document.  To receive notification of changes to the document state, you should subscribe to
the Modified field.

It is recommended that you manually set this field to `false` if the document is saved to disk.  The Scintilla class will
not make this change for you automatically.

*********************************************************************************************************************/

static ERR SET_Modified(extScintilla *Self, int Value)
{
   if (Self->initialised()) {
      if (Value) {
         Self->Modified = TRUE;
      }
      else {
         Self->Modified = FALSE;
         SCICALL(SCI_SETSAVEPOINT); // Tell Scintilla that the document is unmodified
      }

      report_event(Self, SEF::MODIFIED);
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
RightMargin: Defines the amount of white-space at the right side of the page.

*********************************************************************************************************************/

static ERR SET_RightMargin(extScintilla *Self, int Value)
{
   if ((Value >= 0) and (Value <= 100)) {
      Self->RightMargin = Value;
      if (Self->initialised()) {
         SCICALL(SCI_SETMARGINRIGHT, 0, Self->RightMargin);
      }
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
ShowWhitespace: White-space characters will be visible to the user when this field is TRUE.

*********************************************************************************************************************/

static ERR GET_ShowWhitespace(extScintilla *Self, int *Value)
{
   *Value = Self->ShowWhitespace;
   return ERR::Okay;
}

static ERR SET_ShowWhitespace(extScintilla *Self, int Value)
{
   if (Value) {
      Self->ShowWhitespace = 1;
      if (Self->initialised()) SCICALL(SCI_SETVIEWWS, (long unsigned int)SCWS_VISIBLEALWAYS);
   }
   else {
      Self->ShowWhitespace = 0;
      if (Self->initialised()) SCICALL(SCI_SETVIEWWS, (long unsigned int)SCWS_INVISIBLE);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EventCallback: Provides callbacks for global state changes.

Set this field with a function reference to receive event notifications.  It must be set in conjunction with
#EventFlags so that you can select the type of notifications that will be received.

The callback function must be in the format `Function(*Scintilla, INT64 EventFlag)`.

The EventFlag value will indicate the event that occurred.  Please see the #EventFlags field for a list of
supported events and additional details.

*********************************************************************************************************************/

static ERR GET_EventCallback(extScintilla *Self, FUNCTION **Value)
{
   if (Self->EventCallback.defined()) {
      *Value = &Self->EventCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_EventCallback(extScintilla *Self, FUNCTION *Value)
{
   if (Value) {
      if (Self->EventCallback.isScript()) UnsubscribeAction(Self->EventCallback.Context, AC::Free);
      Self->EventCallback = *Value;
      if (Self->EventCallback.isScript()) {
         SubscribeAction(Self->EventCallback.Context, AC::Free, C_FUNCTION(notify_free_event));
      }
   }
   else Self->EventCallback.clear();
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EventFlags: Specifies events that need to be reported from the Scintilla object.

To receive event notifications, set #EventCallback with a function reference and the EventFlags field with a mask that
indicates the events that need to be received.

-FIELD-
SelectBkgd: Defines the background colour of selected text.  Supports alpha blending.

*********************************************************************************************************************/

static ERR SET_SelectBkgd(extScintilla *Self, RGB8 *Value)
{
   if ((Value) and (Value->Alpha)) {
      Self->SelectBkgd = *Value;
      SCICALL(SCI_SETSELBACK, (unsigned long int)true, (long int)SCICOLOUR(Self->SelectBkgd.Red, Self->SelectBkgd.Green, Self->SelectBkgd.Blue));
      //SCICALL(SCI_SETSELALPHA, Self->SelectBkgd.Alpha);
   }
   else {
      Self->SelectBkgd.Alpha = 0;
      SCICALL(SCI_SETSELBACK, (unsigned long int)false, 0L);
      //SCICALL(SCI_SETSELALPHA, 0UL);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SelectFore: Defines the colour of selected text.  Supports alpha blending.

*********************************************************************************************************************/

static ERR SET_SelectFore(extScintilla *Self, RGB8 *Value)
{
   pf::Log log;

   log.msg("New SelectFore colour: %d,%d,%d,%d", Value->Red, Value->Green, Value->Blue, Value->Alpha);
   if ((Value) and (Value->Alpha)) {
      Self->SelectFore = *Value;
      SCICALL(SCI_SETSELFORE, (unsigned long int)true, (long int)SCICOLOUR(Self->SelectFore.Red, Self->SelectFore.Green, Self->SelectFore.Blue));
   }
   else {
      Self->SelectFore.Alpha = 0;
      SCICALL(SCI_SETSELFORE, (unsigned long int)false, 0L);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
String: Manages the text data as a complete string.

A Scintilla document can be completely updated by setting the String field.  Equally, the entire document can be
retrieved by getting the String field.  Please be aware that retrieving the document in string format can be very
inefficient, as the document text is normally stored on a per-line basis.  Consider using the #GetLine()
method as the preferred alternative, as it is much more efficient with memory usage.

*********************************************************************************************************************/

static ERR GET_String(extScintilla *Self, STRING *Value)
{
   int len = SCICALL(SCI_GETLENGTH);

   if (Self->StringBuffer) { FreeResource(Self->StringBuffer); Self->StringBuffer = nullptr; }

   if (AllocMemory(len+1, MEM::STRING|MEM::NO_CLEAR, &Self->StringBuffer) IS ERR::Okay) {
      SCICALL(SCI_GETTEXT, len+1, (const char *)Self->StringBuffer);
      *Value = Self->StringBuffer;
      return ERR::Okay;
   }
   else return ERR::AllocMemory;
}

static ERR SET_String(extScintilla *Self, CSTRING Value)
{
   if (Self->initialised()) {
      if ((Value) and (*Value)) SCICALL(SCI_SETTEXT, 0UL, (const char *)Value);
      else acClear(Self);
   }
   else return ERR::NotInitialised;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Surface: Refers to the @Surface targeted by the Scintilla object.

This compulsory field refers to the @Surface that the Scintilla object is targeting for graphics operations.  If not
set prior to initialisation, the Scintilla object will search for the nearest @Surface object based on its ownership
hierarchy.

-FIELD-
Symbols: Symbols can be displayed in the left margin when this value is TRUE.

*********************************************************************************************************************/

static ERR GET_Symbols(extScintilla *Self, int *Value)
{
   *Value = Self->Symbols;
   return ERR::Okay;
}

static ERR SET_Symbols(extScintilla *Self, int Value)
{
   if (Value) {
      Self->Symbols = TRUE;
      if (Self->initialised()) SCICALL(SCI_SETMARGINWIDTHN, 1, 20L);
   }
   else {
      Self->Symbols = FALSE;
      if (Self->initialised()) SCICALL(SCI_SETMARGINWIDTHN, 1, 0L);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TabWidth: The width of tab stops in the document, measured as fixed-width characters.

*********************************************************************************************************************/

static ERR GET_TabWidth(extScintilla *Self, int *Value)
{
   *Value = Self->TabWidth;
   return ERR::Okay;
}

static ERR SET_TabWidth(extScintilla *Self, int Value)
{
   if (Value > 0) {
      if (Value > 200) Value = 200;
      Self->TabWidth = Value;
      if (Self->initialised()) SCICALL(SCI_SETTABWIDTH, Value);
      return ERR::Okay;
   }
   else return ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
TextColour: Defines the default colour of foreground text.  Supports alpha blending.

*********************************************************************************************************************/

static ERR SET_TextColour(extScintilla *Self, RGB8 *Value)
{
   Self->TextColour = *Value;

   if (Self->initialised()) {
      SCICALL(SCI_STYLESETFORE, STYLE_DEFAULT, (long int)SCICOLOUR(Self->TextColour.Red, Self->TextColour.Green, Self->TextColour.Blue));
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Visible: If TRUE, indicates the Scintilla object is visible in the target #Surface.

This field is set to TRUE if the Scintilla object is visible in the target #Surface.  To control visibility, please use
the #Show() and #Hide() actions appropriately.  This field can be set to FALSE prior to initialisation if the Scintilla
object should start in a hidden state.

-FIELD-
Wordwrap: Enables automatic word wrapping when TRUE.
-END-

*********************************************************************************************************************/

static ERR GET_Wordwrap(extScintilla *Self, int *Value)
{
   *Value = Self->Wordwrap;
   return ERR::Okay;
}

static ERR SET_Wordwrap(extScintilla *Self, int Value)
{
   if (Value) Self->Wordwrap = TRUE;
   else Self->Wordwrap = FALSE;

   if (Self->initialised()) {
      Self->API->panWordwrap(Self->Wordwrap);
   }
   return ERR::Okay;
}

//********************************************************************************************************************

static void create_styled_fonts(extScintilla *Self)
{
   pf::Log log;

   log.msg("create_styled_fonts(%s,%.2f,$%.8x)", Self->Font->Face, Self->Font->Point, int(Self->Font->Flags));

   if (!Self->Font) return;

   if (Self->BoldFont)   { FreeResource(Self->BoldFont); Self->BoldFont = nullptr; }
   if (Self->ItalicFont) { FreeResource(Self->ItalicFont); Self->ItalicFont = nullptr; }
   if (Self->BIFont)     { FreeResource(Self->BIFont); Self->BIFont = nullptr; }

   if ((Self->BoldFont = objFont::create::local(
         fl::Face(Self->Font->Face),
         fl::Point(Self->Font->Point),
         fl::Flags(Self->Font->Flags),
         fl::Style("bold")))) {
   }

   if ((Self->ItalicFont = objFont::create::local(
         fl::Face(Self->Font->Face),
         fl::Point(Self->Font->Point),
         fl::Flags(Self->Font->Flags),
         fl::Style("italics")))) {
   }

   if ((Self->BIFont = objFont::create::local(
         fl::Face(Self->Font->Face),
         fl::Point(Self->Font->Point),
         fl::Flags(Self->Font->Flags),
         fl::Style("bold italics")))) {
   }
}

//********************************************************************************************************************
// Scintilla initiates drawing instructions through window::InvalidateRectangle()

static thread_local objBitmap *glBitmap = nullptr;

static void draw_scintilla(extScintilla *Self, objSurface *Surface, objBitmap *Bitmap)
{
   pf::Log log;

   if (!Self->Visible) return;
   if (!Self->initialised()) return;

   log.traceBranch("Surface: %d, Bitmap: %d. Clip: %dx%d,%dx%d", Surface->UID, Bitmap->UID, Bitmap->Clip.Left, Bitmap->Clip.Top, Bitmap->Clip.Right - Bitmap->Clip.Left, Bitmap->Clip.Bottom - Bitmap->Clip.Top);

   glBitmap = Bitmap;

   glFont       = (OBJECTPTR)Self->Font;
   glBoldFont   = (OBJECTPTR)Self->BoldFont;
   glItalicFont = (OBJECTPTR)Self->ItalicFont;
   glBIFont     = (OBJECTPTR)Self->BIFont;

   Self->API->panDraw(Surface, Bitmap);

   glBitmap = nullptr;

   if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) {
      gfx::DrawRectangle(Bitmap, 0, 0, Bitmap->Width, Bitmap->Height, Bitmap->packPixel(0, 0, 0, 64), BAF::FILL|BAF::BLEND);
   }
}

//********************************************************************************************************************

static void error_dialog(CSTRING Title, CSTRING Message, ERR Error)
{
   pf::Log log;
   static OBJECTID dialog_id = 0;

   log.warning("%s", Message);

   if (dialog_id) {
      if (CheckObjectExists(dialog_id) IS ERR::True) return;
   }

   OBJECTPTR dialog;
   if (NewObject(CLASSID::SCRIPT, &dialog) IS ERR::Okay) {
      dialog->setFields(fl::Name("scDialog"), fl::Owner(CurrentTaskID()), fl::Path("system:scripts/gui/dialog.tiri"));

      acSetKey(dialog, "modal", "1");
      acSetKey(dialog, "title", Title);
      acSetKey(dialog, "options", "okay");
      acSetKey(dialog, "type", "error");

      CSTRING errstr;
      if ((Error != ERR::Okay) and (errstr = GetErrorMsg(Error))) {
         std::ostringstream buffer;
         if (Message) buffer << Message << "\n\nDetails: " << errstr;
         else buffer << "Error: " << errstr;

         acSetKey(dialog, "message", buffer.str().c_str());
      }
      else acSetKey(dialog, "message", Message);

      if ((InitObject(dialog) IS ERR::Okay) and (acActivate(dialog) IS ERR::Okay)) {
         CSTRING *results;
         int size;
         if ((dialog->get(FID_Results, results, size) IS ERR::Okay) and (size > 0)) {
            dialog_id = strtol(results[0], nullptr, 0);
         }
      }
   }
}

//********************************************************************************************************************

static ERR load_file(extScintilla *Self, CSTRING Path)
{
   pf::Log log(__FUNCTION__);
   STRING str;
   int size, len;
   ERR error = ERR::Okay;

   if (auto file = objFile::create::local(fl::Flags(FL::READ), fl::Path(Path))) {
      if ((file->Flags & FL::STREAM) != FL::NIL) {
         if (file->startStream(Self->UID, FL::READ, 0) IS ERR::Okay) {
            acClear(Self);

            SubscribeAction(file, AC::Write, C_FUNCTION(notify_write));
            Self->FileStream = file;
            file = nullptr;
         }
         else error = ERR::Failed;
      }
      else if (file->get(FID_Size, size) IS ERR::Okay) {
         if (size > 0) {
            if (size < 1024 * 1024 * 10) {
               if (AllocMemory(size+1, MEM::STRING|MEM::NO_CLEAR, &str) IS ERR::Okay) {
                  if (acRead(file, str, size, &len) IS ERR::Okay) {
                     str[len] = 0;
                     SCICALL(SCI_SETTEXT, str);
                     SCICALL(SCI_EMPTYUNDOBUFFER);
                     error = ERR::Okay;

                     calc_longest_line(Self);
                  }
                  else error = ERR::Read;

                  FreeResource(str);
               }
               else error = ERR::AllocMemory;
            }
            else error = ERR::BufferOverflow;
         }
         else error = ERR::Okay; // File is empty
      }
      else error = ERR::File;

      if (file) FreeResource(file);
   }
   else error = ERR::File;

   if ((error IS ERR::Okay) and ((Self->Flags & SCIF::DETECT_LEXER) != SCIF::NIL)) {
      int i = strlen(Path);
      while ((i > 0) and (Path[i-1] != '/') and (Path[i-1] != '\\') and (Path[i-1] != ':')) i--;
      Path = Path + i;

      for (i=0; i < std::ssize(glLexers); i++) {
         if (wildcmp(glLexers[i].File, Path)) {
            pf::Log log;
            Self->Lexer = glLexers[i].Lexer;
            log.branch("Lexer for the loaded file is %d.", int(Self->Lexer));
            Self->API->SetLexer(int(Self->Lexer));
            break;
         }
      }
      if (i >= std::ssize(glLexers)) log.msg("Failed to choose a lexer for %s", Path);
   }

   return error;
}

//********************************************************************************************************************

static void key_event(evKey *Event, int Size, extScintilla *Self)
{
   pf::Log log;

   if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) return;
   if ((Self->Flags & SCIF::EDIT) IS SCIF::NIL) return;

   if ((Event->Qualifiers & KQ::PRESSED) != KQ::NIL) {
      if ((Event->Code IS KEY::L_SHIFT) or (Event->Code IS KEY::R_SHIFT)) Self->KeyShift = TRUE;
      else if ((Event->Code IS KEY::L_ALT) or (Event->Code IS KEY::R_ALT)) Self->KeyAlt = TRUE;
      else if ((Event->Code IS KEY::L_CONTROL) or (Event->Code IS KEY::R_CONTROL)) Self->KeyCtrl = TRUE;

      char string[8];
      string[0] = 0;

      if ((Event->Qualifiers & KQ::NOT_PRINTABLE) IS KQ::NIL) {
         int16_t out = UTF8WriteValue(Event->Unicode, string, sizeof(string)-1);
         if (out >= 0) string[out] = 0;
      }

      strcopy(string, (STRING)Self->API->lastkeytrans, sizeof(Self->API->lastkeytrans));

      int keyval;
      switch (Event->Code) {
         // Handle known non-printable character keys first
         case KEY::TAB:       keyval = SCK_TAB; break;
         case KEY::DOWN:      keyval = SCK_DOWN; break;
         case KEY::UP:        keyval = SCK_UP; break;
         case KEY::LEFT:      keyval = SCK_LEFT; break;
         case KEY::RIGHT:     keyval = SCK_RIGHT; break;
         case KEY::HOME:      keyval = SCK_HOME; break;
         case KEY::END:       keyval = SCK_END; break;
         case KEY::PAGE_UP:   keyval = SCK_PRIOR; break;
         case KEY::PAGE_DOWN: keyval = SCK_NEXT; break;
         case KEY::DELETE:    keyval = SCK_DELETE; break;
         case KEY::INSERT:    keyval = SCK_INSERT; break;
         case KEY::ENTER:
         case KEY::NP_ENTER:  keyval = SCK_RETURN; break;
         case KEY::ESCAPE:    keyval = SCK_ESCAPE; break;
         case KEY::BACKSPACE: keyval = SCK_BACK; break;
         default:
            if ((Event->Qualifiers & KQ::NOT_PRINTABLE) != KQ::NIL) {
               // Unhandled non-printable characters are ignored
               keyval = 0;
            }
            else if ((int(Event->Code) >= int(KEY::A)) and (int(Event->Code) <= int(KEY::Z))) {
               keyval = int(Event->Code) - int(KEY::A) + int('a');
            }
            else if ((int(Event->Code) >= int(KEY::ZERO)) and (int(Event->Code) <= int(KEY::NINE))) {
               keyval = int(Event->Code) - int(KEY::ZERO) + int('0');
            }
            else {
               // Call KeyDefault(), which will pull the key value from the lastkeytrans buffer
               if (string[0]) Self->API->KeyDefault(0,0);
               keyval = 0;
            }
            break;
      }

      if (keyval) {
         log.traceBranch("Keypress: %d", int(keyval));
         Self->API->panKeyDown(keyval, Event->Qualifiers);
      }
   }
   else if ((Event->Qualifiers & KQ::RELEASED) != KQ::NIL) {
      if ((Event->Code IS KEY::L_SHIFT) or (Event->Code IS KEY::R_SHIFT)) Self->KeyShift = FALSE;
      else if ((Event->Code IS KEY::L_ALT) or (Event->Code IS KEY::R_ALT)) Self->KeyAlt = FALSE;
      else if ((Event->Code IS KEY::L_CONTROL) or (Event->Code IS KEY::R_CONTROL)) Self->KeyCtrl = FALSE;
   }
}

//********************************************************************************************************************

static ERR consume_input_events(const InputEvent *Events, int TotalEvents)
{
   auto Self = (extScintilla *)CurrentContext();

   for (auto event=Events; event; event=event->Next) {
      if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) continue;

      if ((event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         if (event->Value > 0) {
            Self->API->panMousePress(event->Type, event->X, event->Y);
         }
         else Self->API->panMouseRelease(event->Type, event->X, event->Y);
      }
      else if ((event->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
         Self->API->panMouseMove(event->X, event->Y);
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************

static void report_event(extScintilla *Self, SEF Event)
{
   if ((Event & Self->EventFlags) != SEF::NIL) {
       if (Self->EventCallback.isC()) {
         pf::SwitchContext ctx(Self->EventCallback.Context);
         auto routine = (void (*)(extScintilla *, SEF, APTR)) Self->EventCallback.Routine;
         routine(Self, Event, Self->EventCallback.Meta);
      }
      else if (Self->EventCallback.isScript()) {
         sc::Call(Self->EventCallback, std::to_array<ScriptArg>({ { "Scintilla", Self, FD_OBJECTPTR }, { "EventFlags", int64_t(Event) } }));
      }
   }
}

//********************************************************************************************************************

static void calc_longest_line(extScintilla *Self)
{
   int linewidth;
   static const int LINE_COUNT_LIMIT = 2000;

   if (!Self->Font) return;

   pf::Log log(__FUNCTION__);
   log.traceBranch("Wrap: %d", Self->Wordwrap);

   int lines = SCICALL(SCI_GETLINECOUNT);
   if (lines > LINE_COUNT_LIMIT) lines = LINE_COUNT_LIMIT;

   int cwidth = 0;
   int cline  = 0;

   if (Self->Wordwrap) {
      Self->LongestLine = 0;
      Self->LongestWidth = 0;
   }
   else { // Find the line with the longest width
      for (int i=0; i < lines; i++) {
         int end = SCICALL(SCI_GETLINEENDPOSITION, i);
         if (Self->Font->FixedWidth) {
            int col = SCICALL(SCI_GETCOLUMN, end);
            linewidth = col * Self->Font->FixedWidth;
         }
         else linewidth = SCICALL(SCI_POINTXFROMPOSITION, 0, end);

         if (linewidth > cwidth) {
            cline = i;
            cwidth = linewidth;
         }
      }

      if (lines IS LINE_COUNT_LIMIT) {
         Self->LongestWidth += 1024; // Add lots of extra space in case there are much longer lines further on in the document
      }
      else Self->LongestWidth += 30;

      Self->LongestLine  = cline;
      Self->LongestWidth = cwidth;
   }

   log.trace("Longest line: %d", Self->LongestWidth);

   if (Self->initialised()) {
      if (Self->LongestWidth >= 60) {
         SCICALL(SCI_SETSCROLLWIDTH, Self->LongestWidth);
      }
      else SCICALL(SCI_SETSCROLLWIDTH, 1UL);
   }
}

//********************************************************************************************************************

static ERR idle_timer(extScintilla *Self, int64_t Elapsed, int64_t CurrentTime)
{
   AdjustLogLevel(3);
   Self->API->panIdleEvent();
   AdjustLogLevel(-3);
   return ERR::Okay;
}

//********************************************************************************************************************

#include "class_scintilla_ext.cxx"
#include "pan_classes.cxx"
#include "pan_window.cxx"
#include "pan_menu.cxx"
#include "pan_surface.cxx"
#include "pan_listbox.cxx"

//********************************************************************************************************************

#include "class_scintilla_def.cxx"

static const FieldArray clFields[] = {
   { "Font",           FDF_LOCAL|FDF_R, nullptr, nullptr, CLASSID::FONT },
   { "Path",           FDF_STRING|FDF_RW, nullptr, SET_Path },
   { "EventFlags",     FDF_INT|FDF_FLAGS|FDF_RW, nullptr, nullptr, &clScintillaEventFlags },
   { "Surface",        FDF_OBJECTID|FDF_RI, nullptr, nullptr, CLASSID::SURFACE },
   { "Flags",          FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clScintillaFlags },
   { "Focus",          FDF_OBJECTID|FDF_RI },
   { "Visible",        FDF_INT|FDF_RI },
   { "LeftMargin",     FDF_INT|FDF_RW, nullptr, SET_LeftMargin },
   { "RightMargin",    FDF_INT|FDF_RW, nullptr, SET_RightMargin },
   { "LineHighlight",  FDF_RGB|FDF_RW, nullptr, SET_LineHighlight },
   { "SelectFore",     FDF_RGB|FDF_RI, nullptr, SET_SelectFore },
   { "SelectBkgd",     FDF_RGB|FDF_RI, nullptr, SET_SelectBkgd },
   { "BkgdColour",     FDF_RGB|FDF_RW, nullptr, SET_BkgdColour },
   { "CursorColour",   FDF_RGB|FDF_RW, nullptr, SET_CursorColour },
   { "TextColour",     FDF_RGB|FDF_RW, nullptr, SET_TextColour },
   { "CursorRow",      FDF_INT|FDF_RW },
   { "CursorCol",      FDF_INT|FDF_RW },
   { "Lexer",          FDF_INT|FDF_LOOKUP|FDF_RI, nullptr, SET_Lexer, &clScintillaLexer },
   { "Modified",       FDF_INT|FDF_RW, nullptr, SET_Modified },

   // Virtual fields
   { "AllowTabs",      FDF_INT|FDF_RW,   GET_AllowTabs, SET_AllowTabs },
   { "AutoIndent",     FDF_INT|FDF_RW,   GET_AutoIndent, SET_AutoIndent },
   { "FileDrop",       FDF_FUNCTIONPTR|FDF_RW, GET_FileDrop, SET_FileDrop },
   { "FoldingMarkers", FDF_INT|FDF_RW,   GET_FoldingMarkers, SET_FoldingMarkers },
   { "LineCount",      FDF_INT|FDF_R,    GET_LineCount },
   { "LineNumbers",    FDF_INT|FDF_RW,   GET_LineNumbers, SET_LineNumbers },
   { "Origin",         FDF_STRING|FDF_RW, GET_Path, SET_Origin },
   { "ShowWhitespace", FDF_INT|FDF_RW,   GET_ShowWhitespace, SET_ShowWhitespace },
   { "EventCallback",  FDF_FUNCTIONPTR|FDF_RW, GET_EventCallback, SET_EventCallback },
   { "String",         FDF_STRING|FDF_RW, GET_String, SET_String },
   { "Symbols",        FDF_INT|FDF_RW,   GET_Symbols, SET_Symbols },
   { "TabWidth",       FDF_INT|FDF_RW,   GET_TabWidth, SET_TabWidth },
   { "Wordwrap",       FDF_INT|FDF_RW,   GET_Wordwrap, SET_Wordwrap },
   END_FIELD
};

//********************************************************************************************************************

static ERR create_scintilla(void)
{
   clScintilla = objMetaClass::create::global(
      fl::ClassVersion(VER_SCINTILLA),
      fl::Name("Scintilla"),
      fl::Category(CCF::TOOL),
      fl::Flags(CLF::INHERIT_LOCAL),
      fl::Actions(clScintillaActions),
      fl::Methods(clScintillaMethods),
      fl::Fields(clFields),
      fl::Size(sizeof(extScintilla)),
      fl::FileExtension("*.txt|*.text"),
      fl::Icon("filetypes/text"),
      fl::Path("modules:scintilla"));

   return clScintilla ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_scintilla_module() { return &ModHeader; }
