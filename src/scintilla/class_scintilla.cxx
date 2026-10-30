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

Releasing the right mouse button over the editor opens a context menu with Undo, Redo, Cut, Copy, Paste, Delete and
Select All.  Items are disabled when they cannot be used: edits require `SCIF::EDIT`, Cut, Copy and Delete require a
selection, and Paste requires text on the clipboard.  The menu is displayed with `gui/menu` at the pointer position,
including over a transformed or scrolled host.  Closing the menu with Escape or by choosing an item returns the focus
to the editor, while moving the focus elsewhere closes it without doing so.
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
#include <algorithm>
#include <map>
#include <format>
#include <charconv>
#include <cmath>
#include <memory>

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
#include <kotuku/modules/script.h>
#include <kotuku/modules/filesystem.h>
#include <kotuku/modules/module.h>
#include <kotuku/modules/vector.h>
#include <kotuku/strings.hpp>

#include "scintillakotuku.h"

#include "module_def.c"

JUMPTABLE_CORE
JUMPTABLE_DISPLAY
JUMPTABLE_VECTOR
JUMPTABLE_FONT

static const double IDLE_INTERVAL = 0.01; // Seconds between calls to Idle() while Scintilla has background work
static const int MAX_MARKER = SC_MARKNUM_FOLDEREND - 1; // Markers above this number are reserved for the fold margin

static OBJECTPTR clScintilla = nullptr;
static OBJECTPTR modDisplay = nullptr, modFont = nullptr, modVector = nullptr;
extern OBJECTPTR clScintillaSearch;

static const struct {
   CSTRING File;
   SCLEX Lexer;
} glLexers[] = {
   { "*.asm|*.s",     SCLEX::ASSEMBLER },
   { "*.asp",         SCLEX::ASP },
   { "*.bash|*.sh",   SCLEX::BASH },
   { "*.bat|*.dos",   SCLEX::BATCH },
   { "*.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx", SCLEX::CPP },
   { "*.css",         SCLEX::CSS },
   { "*.diff",        SCLEX::DIFF },
   { "*.errorlist",   SCLEX::ERRORLIST },
   { "*.lua|*.tiri", SCLEX::TIRI },
   { "*.dmd",         SCLEX::HTML },
   { "*.html|*.htm|*.xhtml", SCLEX::HTML },
   { "makefile|*.make", SCLEX::MAKEFILE },
   { "*.pas",          SCLEX::PASCAL },
   { "*.perl|*.pl",    SCLEX::PERL },
   { "*.php",          SCLEX::PHPSCRIPT },
   { "*.reb|*.rebol",   SCLEX::REBOL },
   { "*.prop|*.cfg",   SCLEX::PROPERTIES },
   { "*.py",           SCLEX::PYTHON },
   { "*.ruby|*.rb",    SCLEX::RUBY },
   { "*.sql",          SCLEX::SQL },
   { "*.vb",           SCLEX::VB },
   { "*.vbscript",     SCLEX::VBSCRIPT },
   { "*.xml",          SCLEX::XML }
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
static ERR GET_CursorCol(extScintilla *, int *);
static ERR GET_CursorRow(extScintilla *, int *);
static ERR GET_FileDrop(extScintilla *, FUNCTION * &);
static ERR GET_FoldingMarkers(extScintilla *, int *);
static ERR GET_LineCount(extScintilla *, int *);
static ERR GET_LineNumbers(extScintilla *, int *);
static ERR GET_MarginClick(extScintilla *, FUNCTION * &);
static ERR GET_ShowWhitespace(extScintilla *, int *);
static ERR GET_EventCallback(extScintilla *, FUNCTION * &);
static ERR GET_String(extScintilla *, std::string_view &);
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
static ERR SET_MarginClick(extScintilla *, FUNCTION *);
static ERR SET_Path(extScintilla *, const std::string_view &);
static ERR SET_Modified(extScintilla *, int);
static ERR SET_Origin(extScintilla *, const std::string_view &);
static ERR SET_RightMargin(extScintilla *, int);
static ERR SET_ShowWhitespace(extScintilla *, int);
static ERR SET_EventCallback(extScintilla *, FUNCTION *);
static ERR SET_SelectBkgd(extScintilla *, RGB8 *);
static ERR SET_SelectFore(extScintilla *, RGB8 *);
static ERR SET_String(extScintilla *, const std::string_view &);
static ERR SET_Symbols(extScintilla *, int);
static ERR SET_TabWidth(extScintilla *, int);
static ERR SET_TextColour(extScintilla *, RGB8 *);
static ERR SET_Wordwrap(extScintilla *, int);

//********************************************************************************************************************

static ERR resolve_fonts(extScintilla *, std::string_view, int);
static ERR SET_Face(extScintilla *, const std::string_view &);
static ERR create_scintilla(void);
static ERR create_canvas(extScintilla *);
static ERR feedback_view(objVectorViewport *, FM);
static ERR feedback_page(objVectorViewport *, FM);
static ERR feedback_focus(objVectorViewport *, FM);
static void update_focus(extScintilla *);
static ERR input_canvas(objVectorRectangle *, const InputEvent *);
static void align_canvas(extScintilla *, double, double);
static void sync_page(extScintilla *);
static void clear_bitmap(extScintilla *);
static ERR resize_bitmap(extScintilla *, int, int);
static void invalidate_area(extScintilla *, int, int, int, int);
static void paint_dirty(extScintilla *);
static void update_margin_width(extScintilla *);
static ERR load_file(extScintilla *, std::string_view);
static void calc_longest_line(extScintilla *);
static ERR key_event(objVectorViewport *, KQ, KEY, int);
static int command_key(KEY, bool);
static int shortcut_key(KEY);
static void report_event(extScintilla *, SEF Event);
static void report_margin_click(extScintilla *, SCMG, int, KQ);
static void hide_context_menu(extScintilla *);
static void free_context_menu(extScintilla *);
static ERR tick_timer(extScintilla *, int64_t, int64_t);
static ERR idle_timer(extScintilla *, int64_t, int64_t);
extern ERR init_search(void);

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR argModule, struct CoreBase *argCoreBase)
{
   CoreBase = argCoreBase;

   if (objModule::load("display", &modDisplay, &DisplayBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("font", &modFont, &FontBase) != ERR::Okay) return ERR::InitModule;
   if (objModule::load("vector", &modVector, &VectorBase) != ERR::Okay) return ERR::InitModule;

   if (!init_search()) {
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
// TODO Phase 7: Drag and drop support.  This was previously subscribed to the AC::DragDrop action of the host surface.

[[maybe_unused]] static void notify_dragdrop(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acDragDrop *Args)
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

   if (!acDataFeed(Args->Source, Self, DATA::REQUEST,
         std::span<const int8_t>((const int8_t *)&request, sizeof(request)))) {
      // The source will return a DATA::RECEIPT for the items that we've asked for (see the DataFeed action).
   }
}

//********************************************************************************************************************
// The editor follows the enabled state of its host viewport.

static void notify_disable_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   if (Result IS ERR::Okay) acDisable(CurrentContext());
}

static void notify_enable_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   if (Result IS ERR::Okay) acEnable(CurrentContext());
}

//********************************************************************************************************************
// Receiver for focus changes on the Focus viewport.  The editor has the focus while the Focus viewport or any of its
// children has the focus, which matches the delivery of keyboard input.

static ERR feedback_focus(objVectorViewport *Viewport, FM Event)
{
   auto Self = (extScintilla *)CurrentContext();
   Self->HasFocus = (Event & (FM::HAS_FOCUS|FM::CHILD_HAS_FOCUS)) != FM::NIL;
   update_focus(Self);
   return ERR::Okay;
}

//********************************************************************************************************************
// Scintilla shows the caret only while it has the focus, which is withheld while the editor is disabled.  The tracked
// modifier keys are reset on losing the focus because their release will not be reported to the editor.

static void update_focus(extScintilla *Self)
{
   if (not Self->API) return;

   bool focus = (Self->HasFocus) and ((Self->Flags & SCIF::DISABLED) IS SCIF::NIL);
   if (not focus) {
      Self->KeyShift = false;
      Self->KeyCtrl  = false;
      Self->KeyAlt   = false;
   }

   Self->API->panSetFocus(focus);
}

//********************************************************************************************************************
// The host viewport is being freed, which takes the View, Page and Canvas with it.

static void notify_free_viewport(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extScintilla *)CurrentContext();
   hide_context_menu(Self);
   Self->Viewport = nullptr;
   Self->Focus    = nullptr;
   Self->View     = nullptr;
   Self->Page     = nullptr;
   Self->Canvas   = nullptr;
}

//********************************************************************************************************************
// A Focus viewport that differs from the host Viewport is being freed.

static void notify_free_focus(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extScintilla *)CurrentContext();
   Self->Focus    = nullptr;
   Self->HasFocus = false;
   update_focus(Self);
}

//********************************************************************************************************************
// The image definition belongs to the scene and will be freed with it.

static void notify_free_image(OBJECTPTR Object, ACTIONID ActionID, ERR Result, APTR Args)
{
   auto Self = (extScintilla *)CurrentContext();
   Self->Image = nullptr;
}


//********************************************************************************************************************
// Receiver for path changes on the View, which reflect a change to the size of Scintilla's client area.

static ERR feedback_view(objVectorViewport *View, FM Event)
{
   auto Self = (extScintilla *)CurrentContext();

   Unit w, h;
   View->getWidth(w);
   View->getHeight(h);
   int width  = std::max(1, int(std::lround(double(w))));
   int height = std::max(1, int(std::lround(double(h))));

   align_canvas(Self, double(w), double(h));

   if ((Self->ViewWidth IS width) and (Self->ViewHeight IS height)) return ERR::Okay;

   Self->ViewWidth  = width;
   Self->ViewHeight = height;

   // The Bitmap is one pixel wider and taller than the View to cover the sub-pixel alignment offset.

   if ((Self->Bitmap) and (resize_bitmap(Self, width + 1, height + 1) IS ERR::Okay)) {
      if (Self->Canvas) {
         Self->Canvas->setWidth(Self->Bitmap->Width);
         Self->Canvas->setHeight(Self->Bitmap->Height);
      }
   }

   if (Self->API) Self->API->panResized();
   invalidate_area(Self, 0, 0, Self->ViewWidth, Self->ViewHeight);
   return ERR::Okay;
}

//********************************************************************************************************************
// Receiver for path changes on the Page.  If the Page has been moved by a third party, such as a scrollbar, the editor
// scrolls to match.  Scintilla scrolls by whole lines, so the Page is snapped to the nearest line.

static ERR feedback_page(objVectorViewport *Page, FM Event)
{
   auto Self = (extScintilla *)CurrentContext();

   if ((Self->ScrollLocked) or (not Self->API)) return ERR::Okay;

   Unit x, y;
   Page->getX(x);
   Page->getY(y);
   if ((-double(x) IS Self->ScrollX) and (-double(y) IS Self->ScrollY)) return ERR::Okay;

   Self->API->panScrollTo(-double(x), -double(y));
   return ERR::Okay;
}

//********************************************************************************************************************
// Pointer input is received by the Canvas because it is the top-most vector in the editor.  This also prevents wheel
// events from reaching a scrollbar's subscription to the Page, which would otherwise scroll the Page a second time.
//
// The Canvas reports positions in the coordinate system of the Page, which are converted to Scintilla's client
// coordinates by removing the Canvas position.  While the left mouse button is held, the scene continues to deliver
// movement to the Canvas when the pointer is outside of it, which Scintilla uses to extend the selection and scroll.
//
// Button events are reported at the position of the most recent movement event, in the coordinate system of the Page
// at that time.  The editor may have scrolled since then (e.g. while autoscrolling), so button events use the client
// position that was recorded for that movement.
//
// The scene can make the Canvas its active vector without delivering a movement event, e.g. when a drag that started
// in another vector is released over the Canvas.  The Canvas then receives only an enter event, and the recorded
// position belongs to an earlier visit.  The next button or wheel event carries the position that the scene computed
// on entry, so it is used to resolve the client position before the editor can scroll.
//
// A release of the RMB opens the context menu at the scene position of the event, which is free of the transforms
// that apply to the editor (see pan_menu.cxx).  Opening on release ensures that the button is no longer held when the
// menu appears under the pointer.
//
// Each wheel event scrolls by a fixed step because wheel values are not consistent between platforms.  Holding Ctrl
// scrolls by a page.  Input events do not carry the modifier state, so the state that key_event() tracks is used.

static ERR input_canvas(objVectorRectangle *Canvas, const InputEvent *Events)
{
   static const int WHEEL_LINES = 3;
   static const int WHEEL_COLUMNS = 6;

   auto Self = (extScintilla *)CurrentContext();
   if (not Self->API) return ERR::Okay;

   for (auto event=Events; event; event=event->Next) {
      // The pointer position is recorded while disabled, because the scene does not repeat a movement event if the
      // pointer has not moved.

      if ((event->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
         Self->PointerX = event->X - (Self->ScrollX + Self->CanvasX);
         Self->PointerY = event->Y - (Self->ScrollY + Self->CanvasY);
         Self->PointerStale = false;
      }
      else if ((event->Flags & JTYPE::CROSSING) != JTYPE::NIL) {
         // Enter events are not in the coordinate system of the Page, so they only mark the position as outdated.
         if (event->Type IS JET::CROSSED_IN) Self->PointerStale = true;
         continue;
      }
      else if (Self->PointerStale) {
         Self->PointerX = event->X - (Self->ScrollX + Self->CanvasX);
         Self->PointerY = event->Y - (Self->ScrollY + Self->CanvasY);
         Self->PointerStale = false;
      }

      if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) continue;

      if ((event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         const auto time = (unsigned int)(event->Timestamp / 1000LL); // Scintilla measures time in milliseconds
         if (event->Value > 0) {
            // The scene focuses the viewport under the pointer, which is not the Focus viewport if the client has
            // chosen one outside of the editor.

            if ((event->Type IS JET::LMB) and (not Self->HasFocus) and (Self->Focus)) acFocus(Self->Focus);
            Self->API->panMousePress(event->Type, Self->PointerX, Self->PointerY, time);
         }
         else if (event->Type IS JET::RMB) {
            Self->API->panContextMenu(Self->PointerX, Self->PointerY, event->AbsX, event->AbsY);
         }
         else Self->API->panMouseRelease(event->Type, Self->PointerX, Self->PointerY, time);
      }
      else if ((event->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
         Self->API->panMouseMove(Self->PointerX, Self->PointerY);
      }
      else if ((event->Type IS JET::WHEEL) or (event->Type IS JET::WHEEL_X)) {
         if (event->Value IS 0) continue;

         int direction = (event->Value > 0) ? 1 : -1;
         if (event->Type IS JET::WHEEL) {
            int lines = WHEEL_LINES;
            if (Self->KeyCtrl) lines = std::max(1, int(SCICALL(SCI_LINESONSCREEN)) - 1);
            Self->API->panScrollBy(0, direction * lines);
         }
         else Self->API->panScrollBy(direction * WHEEL_COLUMNS, 0);
      }
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Image sampling blurs the text unless the Bitmap is mapped to whole device pixels.  If the View is neither scaled nor
// rotated, the Canvas is offset by the fractional part of the View's device position.  Transformed hosts are left
// unaligned because the Bitmap is resampled regardless.

static void align_canvas(extScintilla *Self, double Width, double Height)
{
   if (not Self->Canvas) return;

   double x = 0, y = 0, ax = 0, ay = 0, w, h;
   if (Self->View->getBoundary(VBF::NIL, &x, &y, &w, &h) IS ERR::Okay) {
      if ((std::abs(w - Width) < 0.01) and (std::abs(h - Height) < 0.01)) {
         ax = std::floor(x + 0.001) - x;
         ay = std::floor(y + 0.001) - y;
         if (ax > -0.001) ax = 0;
         if (ay > -0.001) ay = 0;
      }
   }

   if ((ax != Self->CanvasX) or (ay != Self->CanvasY)) {
      Self->CanvasX = ax;
      Self->CanvasY = ay;
      sync_page(Self);
   }
}

//********************************************************************************************************************
// The Page is positioned at the negated scroll position, and the Canvas is positioned at the scroll position within
// the Page so that it always covers the View.

static void sync_page(extScintilla *Self)
{
   if ((not Self->Page) or (not Self->Canvas)) return;

   Unit x, y;
   Self->Page->getX(x);
   Self->Page->getY(y);
   if (double(x) != -Self->ScrollX) Self->Page->setX(-Self->ScrollX);
   if (double(y) != -Self->ScrollY) Self->Page->setY(-Self->ScrollY);

   Self->Canvas->getX(x);
   Self->Canvas->getY(y);
   if (double(x) != Self->ScrollX + Self->CanvasX) Self->Canvas->setX(Self->ScrollX + Self->CanvasX);
   if (double(y) != Self->ScrollY + Self->CanvasY) Self->Canvas->setY(Self->ScrollY + Self->CanvasY);
}

//********************************************************************************************************************

static void clear_bitmap(extScintilla *Self)
{
   auto &bkgd = Self->BkgdColour;
   gfx::DrawRectangle(Self->Bitmap, 0, 0, Self->Bitmap->Width, Self->Bitmap->Height,
      Self->Bitmap->packPixel(bkgd.Red, bkgd.Green, bkgd.Blue, 255), BAF::FILL);
}

//********************************************************************************************************************
// Resize the Bitmap while retaining its content.  The repaint that follows a resize is deferred, so the scene is drawn
// with the resized Bitmap before Scintilla repaints it.  Retaining the content avoids the editor flashing to a blank
// background on every resize, which is persistent while a window is being resized interactively.  Bitmap resizing
// does not preserve pixels (the buffer is reallocated, or reused with a different row stride) so the rows are copied
// through a temporary buffer.  Only the areas exposed by the resize are cleared.

static ERR resize_bitmap(extScintilla *Self, int Width, int Height)
{
   auto bmp = Self->Bitmap;
   int old_width = bmp->Width, old_height = bmp->Height, old_line_width = bmp->LineWidth;
   int keep_height = std::min(old_height, Height);

   std::vector<uint8_t> saved;
   if (bmp->Data) saved.assign(bmp->Data, bmp->Data + (size_t(old_line_width) * keep_height));

   if (auto error = acResize(bmp, Width, Height, 0); error != ERR::Okay) return error;

   if (saved.empty() or (not bmp->Data)) {
      clear_bitmap(Self);
      return ERR::Okay;
   }

   int keep_width = std::min(old_width, bmp->Width);
   auto row_bytes = size_t(keep_width) * bmp->BytesPerPixel;
   for (int y=0; y < keep_height; y++) {
      memcpy(bmp->Data + (size_t(y) * bmp->LineWidth), saved.data() + (size_t(y) * old_line_width), row_bytes);
   }

   auto &bkgd = Self->BkgdColour;
   auto colour = bmp->packPixel(bkgd.Red, bkgd.Green, bkgd.Blue, 255);
   if (bmp->Width > keep_width) {
      gfx::DrawRectangle(bmp, keep_width, 0, bmp->Width - keep_width, keep_height, colour, BAF::FILL);
   }
   if (bmp->Height > keep_height) {
      gfx::DrawRectangle(bmp, 0, keep_height, bmp->Width, bmp->Height - keep_height, colour, BAF::FILL);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Add an area of Scintilla's client to the Dirty region and schedule a repaint.  Painting is deferred so that it
// never occurs during scene rendering, and so that multiple invalidations are combined into one paint.

static void invalidate_area(extScintilla *Self, int Left, int Top, int Right, int Bottom)
{
   Left   = std::max(Left, 0);
   Top    = std::max(Top, 0);
   Right  = std::min(Right, Self->ViewWidth);
   Bottom = std::min(Bottom, Self->ViewHeight);
   if ((Right <= Left) or (Bottom <= Top)) return;

   auto &dirty = Self->Dirty;
   if ((dirty.Right <= dirty.Left) or (dirty.Bottom <= dirty.Top)) dirty = { Left, Top, Right, Bottom };
   else {
      dirty.Left   = std::min(dirty.Left, Left);
      dirty.Top    = std::min(dirty.Top, Top);
      dirty.Right  = std::max(dirty.Right, Right);
      dirty.Bottom = std::max(dirty.Bottom, Bottom);
   }

   if (not Self->RepaintQueued) {
      Self->RepaintQueued = true;
      QueueAction(sci::Repaint::id, Self->UID);
   }
}

//********************************************************************************************************************
// The line number margin is sized for the digits of the line count, with a minimum of two digits and one character of
// padding.

static void update_margin_width(extScintilla *Self)
{
   if (not Self->API) return;

   int digits = 0;
   if (Self->LineNumbers) {
      digits = 2;
      for (int n = SCICALL(SCI_GETLINECOUNT); n >= 100; n /= 10) digits++;
   }

   if (digits IS Self->MarginDigits) return;
   Self->MarginDigits = digits;

   int width = 0;
   if (digits) {
      std::string sample(digits + 1, '9');
      sample[0] = '_';
      width = SCICALL(SCI_TEXTWIDTH, STYLE_LINENUMBER, sample.c_str());
   }

   SCICALL(SCI_SETMARGINWIDTHN, 0, (long)width);
}

//********************************************************************************************************************

static void notify_write(OBJECTPTR Object, ACTIONID ActionID, ERR Result, struct acWrite *Args)
{
   kt::Log log(__FUNCTION__);
   auto Self = (extScintilla *)CurrentContext();

   if (!Args) return;

   if (Result != ERR::Okay) {
      if (Self->FileStream) { FreeResource(Self->FileStream); Self->FileStream = nullptr; }
      return;
   }

   log.msg("%d bytes incoming from file stream.", Args->Result);

   Self->HoldModify = TRUE; // Prevent the file from being marked as modified due to incoming data

   SCICALL(SCI_SETUNDOCOLLECTION, 0UL); // Turn off undo

   if (not Args->Buffer.empty()) {
      acDataFeed(Self, Self, DATA::TEXT, Args->Buffer.first(std::min(size_t(Args->Result), Args->Buffer.size())));
   }
   else { // We have to read the data from the file stream
   }

   SCICALL(SCI_SETUNDOCOLLECTION, 1UL); // Turn on undo

   Self->HoldModify = FALSE;
}

/*********************************************************************************************************************

-METHOD-
AddMarker: Adds a marker to a line.

A marker is a symbol that is drawn in the symbol margin beside a line, or a highlight of the line's text, as configured
by #DefineMarker().  Markers are typically used to indicate bookmarks, breakpoints and errors.  The symbol margin is
enabled with #Symbols.  While it is hidden, a line that carries markers is highlighted with their background colour
instead.

There are 25 markers, numbered from 0 to 24.  A line can carry any combination of them, and adding a marker that the
line already carries has no effect.  A marker stays with its line as text is inserted and deleted.  If the line is
joined to the previous line, the markers of both lines are combined.

-INPUT-
int Line: The index of the line.
int Marker: The marker number, from 0 to 24.

-RESULT-
Okay:
NullArgs:
OutOfRange: The `Line` or `Marker` is out of range.

*********************************************************************************************************************/

static ERR SCINTILLA_AddMarker(extScintilla *Self, struct sci::AddMarker *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Args->Marker < 0) or (Args->Marker > MAX_MARKER)) return log.warning(ERR::OutOfRange);
   if ((Args->Line < 0) or (Args->Line >= SCICALL(SCI_GETLINECOUNT))) return log.warning(ERR::OutOfRange);

   // Scintilla would record a second instance of the marker, so a marker that is already present is not added again.

   if (not (SCICALL(SCI_MARKERGET, Args->Line) & (1 << Args->Marker))) {
      SCICALL(SCI_MARKERADD, Args->Line, Args->Marker);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clear: Clears all content from the editor.

******************************************************************************/

static ERR SCINTILLA_Clear(extScintilla *Self)
{
   kt::Log log;

   log.branch();

   SCICALL(SCI_BEGINUNDOACTION);
   SCICALL(SCI_CLEARALL);
   SCICALL(SCI_ENDUNDOACTION);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clipboard: Cut, copy and paste text through the system clipboard.

The Clipboard action provides the same cut, copy and paste operations as the editor's key bindings (Ctrl+X, Ctrl+C,
Ctrl+V, Shift+Del, Ctrl+Ins and Shift+Ins).  Text is exchanged as UTF-8 through the @Clipboard class.  The editor
also binds Ctrl+L to cut the lines that the selection touches, and Ctrl+Shift+T to copy the current line.

`CLIPMODE::COPY` copies the selection.  `CLIPMODE::CUT` copies the selection and deletes it, but only once the
clipboard has accepted the text.  Nothing is copied if the selection is empty.  A rectangular selection is copied with
a line ending after each row, and is pasted as a rectangle when it is pasted into a Scintilla editor in the same
process.

`CLIPMODE::PASTE` replaces the selection with the most recent text on the clipboard, as a single undo action.  Line
endings are converted to the document's line ending mode.

If #Flags does not include `SCIF::EDIT`, cut and paste are refused with `ERR::ReadOnly` and reported as
`SEF::FAIL_RO`.  If the clipboard cannot be read or written, the error is logged and reported as
`SEF::FAIL_CLIPBOARD`.

-ERRORS-
Okay: The operation completed, or there was nothing to copy.
NullArgs
Args: The Mode is not supported.
NotInitialised
ReadOnly: The document is not editable.
NoData: The clipboard holds no text to paste.
-END-

*********************************************************************************************************************/

static ERR SCINTILLA_Clipboard(extScintilla *Self, struct acClipboard *Args)
{
   kt::Log log;

   if ((!Args) or (Args->Mode IS CLIPMODE::NIL)) return log.warning(ERR::NullArgs);
   if (!Self->initialised()) return log.warning(ERR::NotInitialised);

   log.traceBranch("Mode: %d", int(Args->Mode));

   if ((Args->Mode IS CLIPMODE::CUT) or (Args->Mode IS CLIPMODE::COPY) or (Args->Mode IS CLIPMODE::PASTE)) {
      return Self->API->panClipboard(Args->Mode);
   }
   else return log.warning(ERR::Args);
}

//*****************************************************************************

static ERR SCINTILLA_DataFeed(extScintilla *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (Args->Datatype IS DATA::TEXT) {
      // Incoming text is appended to the end of the document
      SCICALL(SCI_APPENDTEXT, Args->Buffer.size(), (const char *)Args->Buffer.data());
   }
   else if (Args->Datatype IS DATA::RECEIPT) {
      log.msg("Received item receipt from object %d.", Args->Object ? Args->Object->UID : 0);

      objXML::create xml = { fl::Statement(
         std::string_view((const char *)Args->Buffer.data(), Args->Buffer.size())) };
      if (xml.ok()) {
         for (auto &tag : xml->Tags) {
            if (iequals("file", tag.name())) {
               // If the file is being dragged within the same device, it will be moved instead of copied.

               for (auto &a : tag.Attribs) {
                  if (iequals("path", a.Name)) {
                     if (Self->FileDrop.isC()) {
                        kt::SwitchContext ctx(Self->FileDrop.Context);
                        auto routine = (void (*)(extScintilla *, CSTRING, APTR))Self->FileDrop.Routine;
                        routine(Self, a.Value.c_str(), Self->FileDrop.Meta);
                     }
                     else if (Self->FileDrop.isScript()) {
                        ScriptArg args[] = {
                           ScriptArg("Scintilla", Self, FD_OBJECTPTR),
                           ScriptArg("Path",      a.Value.c_str(), FD_STR)
                        };

                        auto script = (objScript *)Self->FileDrop.Context;
                        script->callback(Self->FileDrop.procedureID(), args, std::ssize(args), nullptr);
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
DefineMarker: Defines the appearance of a marker.

This method sets the symbol and colours of a marker, which can then be added to lines with #AddMarker().  Lines that
already carry the marker are redrawn with the new definition.  Until it is defined, a marker is drawn as a circle with
a black outline and a white fill.

Symbols are drawn in the symbol margin in the `Bkgd` colour, with the outline and any inner sign in the `Fore` colour.
The `SCMK::BACKGROUND` and `SCMK::UNDERLINE` symbols are drawn in the text area instead, in the `Bkgd` colour.  If the
alpha component of `Bkgd` is less than 255, these two symbols are blended over the line, which also tints its text.
Otherwise they are drawn opaquely beneath the text.  The alpha components are ignored for all other symbols.

-INPUT-
int Marker: The marker number, from 0 to 24.
int(SCMK) Symbol: The symbol to draw.
struct(*RGB8) Fore: The foreground colour.  If `NULL`, the current colour is retained.
struct(*RGB8) Bkgd: The background colour.  If `NULL`, the current colour is retained.

-RESULT-
Okay:
NullArgs:
OutOfRange: The `Marker` is out of range.
InvalidValue: The `Symbol` is not supported.

*********************************************************************************************************************/

static ERR SCINTILLA_DefineMarker(extScintilla *Self, struct sci::DefineMarker *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Args->Marker < 0) or (Args->Marker > MAX_MARKER)) return log.warning(ERR::OutOfRange);

   auto symbol = int(Args->Symbol);
   if ((symbol < SC_MARK_CIRCLE) or (symbol > SC_MARK_UNDERLINE) or (symbol IS SC_MARK_PIXMAP) or
       (symbol IS SC_MARK_AVAILABLE)) return log.warning(ERR::InvalidValue);

   SCICALL(SCI_MARKERDEFINE, Args->Marker, symbol);

   if (auto c = Args->Fore) SCICALL(SCI_MARKERSETFORE, Args->Marker, (long)SCICOLOUR(c->Red, c->Green, c->Blue));

   if (auto c = Args->Bkgd) {
      SCICALL(SCI_MARKERSETBACK, Args->Marker, (long)SCICOLOUR(c->Red, c->Green, c->Blue));
      SCICALL(SCI_MARKERSETALPHA, Args->Marker, (c->Alpha < 255) ? long(c->Alpha) : long(SC_ALPHA_NOALPHA));
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
   kt::Log log;
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
Disable: Disables user interaction with the editor.

*********************************************************************************************************************/

static ERR SCINTILLA_Disable(extScintilla *Self)
{
   if ((Self->Flags & SCIF::DISABLED) IS SCIF::NIL) {
      Self->Flags |= SCIF::DISABLED;
      hide_context_menu(Self);
      if (Self->API) Self->API->panReleaseMouse(); // Button releases are ignored while disabled
      update_focus(Self);
      acDraw(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Draw: Redraws the editor content.

The entire content of the editor is marked for redrawing.  Painting is deferred until the next cycle of message
processing, after which the scene is redrawn.

*********************************************************************************************************************/

static ERR SCINTILLA_Draw(extScintilla *Self, struct acDraw *Args)
{
   if (Self->API) Self->API->panRedraw();
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Enable: Enables user interaction with the editor.

*********************************************************************************************************************/

static ERR SCINTILLA_Enable(extScintilla *Self)
{
   if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) {
      Self->Flags &= ~SCIF::DISABLED;
      update_focus(Self);
      acDraw(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Focus: Moves the user focus to the #Focus viewport.
-END-
*********************************************************************************************************************/

static ERR SCINTILLA_Focus(extScintilla *Self)
{
   if (Self->Focus) return acFocus(Self->Focus);
   else return ERR::FieldNotSet;
}

/*********************************************************************************************************************

-METHOD-
GetMarkers: Returns the markers of a line.

This method returns the markers that a line carries, as a bit mask in which bit `N` is set for marker `N`.  For
example, a result of `0x5` indicates that the line carries markers 0 and 2.

-INPUT-
int Line: The index of the line.
&int Markers: The bit mask of the line's markers is returned here.

-RESULT-
Okay:
NullArgs:
OutOfRange: The `Line` is out of range.

*********************************************************************************************************************/

static ERR SCINTILLA_GetMarkers(extScintilla *Self, struct sci::GetMarkers *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   Args->Markers = 0;
   if ((Args->Line < 0) or (Args->Line >= SCICALL(SCI_GETLINECOUNT))) return log.warning(ERR::OutOfRange);

   Args->Markers = int(SCICALL(SCI_MARKERGET, Args->Line) & ~SC_MASK_FOLDERS);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetLine: Copies the text content of any line to a user-supplied buffer.

This method retrieves the string for a `Line` at a given index.  The string is copied to the supplied `Buffer`, which
must have enough space for the line content and a terminating null byte.

-INPUT-
int Line: The index of the line to retrieve.
^array(char) Buffer: The destination buffer.

-RESULT-
Okay:
NullArgs:
OutOfRange: At least one of the parameters is out of range.
BufferOverflow: The supplied `Buffer` is not large enough to contain the result.

*********************************************************************************************************************/

static ERR SCINTILLA_GetLine(extScintilla *Self, struct sci::GetLine *Args)
{
   kt::Log log;

   if ((!Args) or (not Args->Buffer.data())) return log.warning(ERR::NullArgs);
   if ((Args->Line < 0) or Args->Buffer.empty()) return log.warning(ERR::OutOfRange);

   // Includes line-end characters, of which there may be more than one.
   int len = SCICALL(SCI_LINELENGTH, Args->Line);
   if (std::ssize(Args->Buffer) > len) {
      SCICALL(SCI_GETLINE, Args->Line, (const char *)Args->Buffer.data());
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
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if (Args->Line < 0) return ERR::OutOfRange;

   log.branch("Line: %d", Args->Line);
   SCICALL(SCI_ENSUREVISIBLE, std::min(Args->Line, int(SCICALL(SCI_GETLINECOUNT)) - 1)); // Expands contracted folds
   SCICALL(SCI_GOTOLINE, Args->Line);
   return ERR::Okay;
}

//*****************************************************************************

static ERR SCINTILLA_Hide(extScintilla *Self)
{
   hide_context_menu(Self);
   if (Self->View) return acHide(Self->View);
   else return ERR::Okay;
}

//*****************************************************************************

static ERR SCINTILLA_Init(extScintilla *Self, APTR)
{
   kt::Log log;

   if (not Self->Viewport) {
      if ((Self->Owner) and (Self->Owner->classID() IS CLASSID::VECTORVIEWPORT)) {
         Self->Viewport = (objVectorViewport *)Self->Owner;
      }
      else return log.warning(ERR::UnsupportedOwner);
   }

   if (Self->Viewport->classID() != CLASSID::VECTORVIEWPORT) return log.warning(ERR::WrongObjectType);

   if (not Self->Focus) Self->Focus = Self->Viewport;

   if (Self->Focus->classID() != CLASSID::VECTORVIEWPORT) return log.warning(ERR::WrongObjectType);

   Self->Scene = Self->Viewport->Scene;

   if (auto error = resolve_fonts(Self, Self->Face, Self->FontSize); error != ERR::Okay) return error;

   if (auto error = create_canvas(Self); error != ERR::Okay) return error;

   if (Self->Scene->SurfaceID) { // Make UI subscriptions as long as we're not headless
      // Keyboard input is delivered while the Focus viewport or one of its children has the focus.

      Self->Focus->subscribeFeedback(FM::HAS_FOCUS|FM::CHILD_HAS_FOCUS|FM::LOST_FOCUS, C_FUNCTION(feedback_focus));
      Self->Focus->subscribeKeyboard(C_FUNCTION(key_event));

      // Pointer input is received by the Canvas.  The View is unsuitable because input is delivered to the top-most
      // subscriber, and a scrollbar subscribes to the Page.

      Self->Canvas->subscribeInput(JTYPE::BUTTON|JTYPE::MOVEMENT|JTYPE::EXT_MOVEMENT|JTYPE::CROSSING,
         C_FUNCTION(input_canvas));

      // TODO Phase 7: Drag and drop support via notify_dragdrop()
   }

   SubscribeAction(Self->Viewport, AC::Free, C_FUNCTION(notify_free_viewport));
   if (Self->Focus != Self->Viewport) SubscribeAction(Self->Focus, AC::Free, C_FUNCTION(notify_free_focus));
   SubscribeAction(Self->Viewport, AC::Disable, C_FUNCTION(notify_disable_viewport));
   SubscribeAction(Self->Viewport, AC::Enable, C_FUNCTION(notify_enable_viewport));

   if ((Self->Viewport->Flags & VF::DISABLED) != VF::NIL) Self->Flags |= SCIF::DISABLED;

   // Create the Scintilla editor and link it to this object.

   if (!(Self->API = new ScintillaKTK(Self))) {
      return ERR::Failed;
   }

   Self->API->panFontChanged();

   // Load a text file if required

   if (not Self->Path.empty()) {
      if (load_file(Self, Self->Path) != ERR::Okay) {
         return ERR::File;
      }
   }
   else calc_longest_line(Self);

   Self->API->SetLexer(int(Self->Lexer));

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

   auto &sf = Self->SelectFore, &sb = Self->SelectBkgd;
   SCICALL(SCI_SETSELFORE, (unsigned long int)(sf.Alpha > 0), (long int)SCICOLOUR(sf.Red, sf.Green, sf.Blue));
   SCICALL(SCI_SETSELBACK, (unsigned long int)(sb.Alpha > 0), (long int)SCICOLOUR(sb.Red, sb.Green, sb.Blue));
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

   if (Self->AllowTabs) SCICALL(SCI_SETUSETABS, 1UL);
   else  SCICALL(SCI_SETUSETABS, 0UL);

   // Set all special margins to invisible (note that the values indicate the pixel width of the margin)

   SCICALL(SCI_SETMARGINWIDTHN, 0, 0L);
   update_margin_width(Self);

   if (Self->Symbols) SCICALL(SCI_SETMARGINWIDTHN, 1, 20L);
   else SCICALL(SCI_SETMARGINWIDTHN, 1, 0L);

   if (Self->FoldingMarkers) SCICALL(SCI_SETMARGINWIDTHN, 2, 20L);
   else SCICALL(SCI_SETMARGINWIDTHN, 2, 0L);

   SCICALL(SCI_SETMARGINSENSITIVEN, 1, (long)Self->MarginClick.defined());
   Self->API->panSetFolding(Self->FoldingMarkers);

   if (Self->Wordwrap) SCICALL(SCI_SETWRAPMODE, 1UL);
   else SCICALL(SCI_SETWRAPMODE, 0UL);

   Self->HasFocus = (Self->Focus->Flags & VF::HAS_FOCUS) != VF::NIL;
   update_focus(Self);

   Self->API->panRedraw();
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
strview String: A text string to add.
int Pos: -1 inserts at the current cursor position, -2 replaces currently selected text, zero or above inserts at the character index indicated.

-RESULT-
Okay
NullArgs
OutOfRange

*********************************************************************************************************************/

static ERR SCINTILLA_InsertText(extScintilla *Self, struct sci::InsertText *Args)
{
   kt::Log log;
   int pos;

   if ((not Args) or (Args->String.empty())) return log.warning(ERR::NullArgs);

   log.branch("Pos: %d, Text: %.*s", Args->Pos, std::min<int>(Args->String.size(), 10), Args->String.data());

   pos = Args->Pos;
   if (pos IS -1) {
      // Get the current cursor position
      pos = SCICALL(SCI_GETCURRENTPOS);
   }
   else if (pos IS -2) {
      // Replace currently selected text

      SCICALL(SCI_BEGINUNDOACTION);
      SCICALL(SCI_REPLACESEL, 0UL, Args->String.data());
      SCICALL(SCI_ENDUNDOACTION);
      return ERR::Okay;
   }
   else if (pos < -1) {
      return log.warning(ERR::OutOfRange);
   }

   SCICALL(SCI_BEGINUNDOACTION);
   SCICALL(SCI_INSERTTEXT, pos, Args->String.data());
   SCICALL(SCI_ENDUNDOACTION);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Redo: Redo the most recently undone activity.

*********************************************************************************************************************/

static ERR SCINTILLA_Redo(extScintilla *Self, struct acRedo *Args)
{
   kt::Log log;

   log.branch();

   SCICALL(SCI_REDO);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
RemoveMarker: Removes markers from one line or from the whole document.

This method removes a marker that was added with #AddMarker().  A `Line` of -1 removes the marker from every line, and
a `Marker` of -1 removes every marker.  Removing a marker that is not present is not an error.

-INPUT-
int Line: The index of the line, or -1 for every line.
int Marker: The marker number from 0 to 24, or -1 for every marker.

-RESULT-
Okay:
NullArgs:
OutOfRange: The `Line` or `Marker` is out of range.

*********************************************************************************************************************/

static ERR SCINTILLA_RemoveMarker(extScintilla *Self, struct sci::RemoveMarker *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);
   if ((Args->Marker < -1) or (Args->Marker > MAX_MARKER)) return log.warning(ERR::OutOfRange);
   if ((Args->Line < -1) or (Args->Line >= SCICALL(SCI_GETLINECOUNT))) return log.warning(ERR::OutOfRange);

   // Scintilla removes every instance of the marker from a line, including those that joining lines can produce.

   if (Args->Line IS -1) SCICALL(SCI_MARKERDELETEALL, Args->Marker);
   else SCICALL(SCI_MARKERDELETE, Args->Line, Args->Marker);
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
strview String: The new string that will replace the line.
int Length: The number of characters to replace the target with, or -1 for the entire source string.

-RESULT-
Okay:
NullArgs:
OutOfRange: The line index is less than zero or greater than the available number of lines.

*********************************************************************************************************************/

static ERR SCINTILLA_ReplaceLine(extScintilla *Self, struct sci::ReplaceLine *Args)
{
   kt::Log log;

   if (!Args) return ERR::NullArgs;
   if (Args->Line < 0) return log.warning(ERR::OutOfRange);

   // Select the line, then replace the text

   int start, end;
   if ((start = SCICALL(SCI_POSITIONFROMLINE, Args->Line)) < 0) return log.warning(ERR::OutOfRange);
   if ((end = SCICALL(SCI_GETLINEENDPOSITION, Args->Line)) < 0) return log.warning(ERR::OutOfRange);
   SCICALL(SCI_SETTARGETSTART, start);
   SCICALL(SCI_SETTARGETEND, end);

   // Replace the targeted text

   size_t length = Args->String.size();
   if ((Args->Length >= 0) and (size_t(Args->Length) < length)) length = Args->Length;
   SCICALL(SCI_REPLACETARGET, (unsigned long)length, Args->String.data());

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ReplaceText: Replaces all text within an entire document or limited range.

The ReplaceText method will replace all instances of the Find string with the content of the Replace string, between a
given Start and End point.  The `STF::CASE`, `STF::SCAN_SELECTION` and `STF::EXPRESSION` are valid flag options for
this method (see FindText for details).

-INPUT-
strview Find: The keyword string to find.
strview Replace: The string that will replace the keyword.
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
   kt::Log log;
   int start, end;

   if ((not Args) or (Args->Find.empty())) return log.warning(ERR::NullArgs);

   log.branch("Text: '%.*s'... Between: %d - %d, Flags: $%.8x",
      int(std::min(size_t(10), Args->Find.size())), Args->Find.data(), Args->Start, Args->End, int(Args->Flags));

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

   int findlen = Args->Find.size();

   int flags = (((Args->Flags & STF::CASE) != STF::NIL) ? SCFIND_MATCHCASE : 0) |
                (((Args->Flags & STF::EXPRESSION) != STF::NIL) ? SCFIND_REGEXP : 0);

   SCICALL(SCI_SETSEARCHFLAGS, flags);

   SCICALL(SCI_BEGINUNDOACTION);

   // Each search resumes from the end of the previous replacement, so that a replacement containing the keyword
   // is not matched again.  The end position is adjusted by the difference between each match and its replacement.

   int total = 0;
   while (start <= end) {
      log.trace("Search between %d - %d", start, end);

      SCICALL(SCI_SETTARGETSTART, start);
      SCICALL(SCI_SETTARGETEND, end);

      int pos = SCICALL(SCI_SEARCHINTARGET, findlen, Args->Find.data());
      if (pos IS -1) break;

      log.trace("Found keyword at %d", pos);

      int match_len = SCICALL(SCI_GETTARGETEND) - pos;

      int len;
      if ((Args->Flags & STF::EXPRESSION) != STF::NIL) {
         len = SCICALL(SCI_REPLACETARGETRE, (unsigned long)Args->Replace.size(), Args->Replace.data());
      }
      else len = SCICALL(SCI_REPLACETARGET, (unsigned long)Args->Replace.size(), Args->Replace.data());

      end  += len - match_len;
      start = pos + len;
      if (!match_len) start++; // An empty match must not be repeated at the same position
      total++;
   }

   SCICALL(SCI_ENDUNDOACTION);

   if (total) return ERR::Okay;
   else return ERR::Search;
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
-METHOD-
Repaint: Private.  For internal use only.

Private

*********************************************************************************************************************/

static ERR SCINTILLA_Repaint(extScintilla *Self)
{
   Self->RepaintQueued = false;
   paint_dirty(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
SaveToObject: Save content as a text stream to another object.

*********************************************************************************************************************/

static ERR SCINTILLA_SaveToObject(extScintilla *Self, struct acSaveToObject *Args)
{
   kt::Log log;

   if ((!Args) or (!Args->Dest)) return log.warning(ERR::NullArgs);

   int len = SCICALL(SCI_GETLENGTH);

   log.branch("To: %d, Size: %d", Args->Dest->UID, len);

   std::vector<char> buffer(len+1);
   SCICALL(SCI_GETTEXT, buffer.size(), (const char *)buffer.data());
   return acWrite(Args->Dest, std::span<const int8_t>((const int8_t *)buffer.data(), size_t(len)));
}

/*********************************************************************************************************************

-METHOD-
SetFont: Changes the font that is used for text display.

Call SetFont() to change the font face that is used for displaying text.  The `Face` is a family name as accepted by
the #Face field, e.g. `Source Code Pro` or `Clean`.  The current #FontSize in pixels is retained.

If the new face is invalid or fails to load, the current font will remain unchanged.

-INPUT-
strview Face: The name of the new font face.

-ERRORS-
Okay
NullArgs
CreateObject: Failed to resolve the font handle.
Search: No matching font could be resolved.
File: The font file could not be loaded.
ResolvePath
CreateResource
-END-

*********************************************************************************************************************/

static ERR SCINTILLA_SetFont(extScintilla *Self, struct sci::SetFont *Args)
{
   kt::Log log;

   if ((not Args) or Args->Face.empty()) return log.warning(ERR::NullArgs);

   log.branch("%.*s", int(Args->Face.size()), Args->Face.data());

   return SET_Face(Self, Args->Face);
}

/*********************************************************************************************************************

-METHOD-
ScrollToPoint: Scrolls the document to a pixel position.

This method scrolls the document so that the point (`X`, `Y`), measured in pixels from the top-left corner of the
document, appears at the top-left corner of the #View.  The #Page is moved to `(-X, -Y)` to reflect the change.

The editor scrolls vertically by whole lines, so `Y` is rounded to the nearest line.  Both coordinates are clamped to
the scrollable range of the document.

-INPUT-
int X: Horizontal scroll position in pixels.
int Y: Vertical scroll position in pixels.

-RESULT-
Okay:
NullArgs:

*********************************************************************************************************************/

static ERR SCINTILLA_ScrollToPoint(extScintilla *Self, struct sci::ScrollToPoint *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.traceBranch("%dx%d", Args->X, Args->Y);

   Self->API->panScrollTo(Args->X, Args->Y);
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
   kt::Log log;

   if ((!Args) or ((!Args->Start) and (!Args->End))) { // Deselect all text
      int pos = SCICALL(SCI_GETCURRENTPOS);
      SCICALL(SCI_SETANCHOR, pos);
      return ERR::Okay;
   }

   log.branch("Selecting area %d to %d", Args->Start, Args->End);

   if (Args->End < 0) SCICALL(SCI_SETSEL, Args->Start, SCICALL(SCI_GETLENGTH));
   else SCICALL(SCI_SETSEL, Args->Start, Args->End);

   SCICALL(SCI_ENSUREVISIBLE, SCICALL(SCI_LINEFROMPOSITION, SCICALL(SCI_GETCURRENTPOS))); // Expands contracted folds
   SCICALL(SCI_SCROLLCARET);

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SCINTILLA_Show(extScintilla *Self)
{
   if (Self->View) return acShow(Self->View);
   else return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ToggleFold: Contracts or expands a fold.

This method toggles the fold that starts at a line, which hides or shows the lines within the fold.  If the line does
not start a fold, the innermost fold that contains it is toggled.  Folding must be enabled with #FoldingMarkers, and
the document needs a #Lexer that supports folding.

-INPUT-
int Line: The index of the line, or -1 for the line of the cursor.

-RESULT-
Okay:
NullArgs:
OutOfRange: The `Line` is out of range.
NothingDone: The line is not within a fold.

*********************************************************************************************************************/

static ERR SCINTILLA_ToggleFold(extScintilla *Self, struct sci::ToggleFold *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   int line = Args->Line;
   if (line IS -1) line = SCICALL(SCI_LINEFROMPOSITION, SCICALL(SCI_GETCURRENTPOS));
   else if ((line < 0) or (line >= SCICALL(SCI_GETLINECOUNT))) return log.warning(ERR::OutOfRange);

   log.branch("Line: %d", line);
   return Self->API->panToggleFold(line) ? ERR::Okay : ERR::NothingDone;
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
   kt::Log log;

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
   kt::Log log;

   log.branch();

   SCICALL(SCI_UNDO);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
AllowTabs: If enabled, use of the tab key produces real tabs and not spaces.

The Tab key is only received by the editor if the `IGNORE_TAB` flag is set on its @VectorScene.  Refer to #Focus for
details.

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

The BkgdColour applies to the text area and to all lexer styles.  The margins use their own colours.

*********************************************************************************************************************/

static ERR SET_BkgdColour(extScintilla *Self, RGB8 *Value)
{
   Self->BkgdColour = *Value;

   if (Self->initialised()) {
      clear_bitmap(Self); // The Bitmap extends beyond the client area that Scintilla paints
      Self->API->SetStyleColours();
   }

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CursorCol: The current column of the text cursor.

The CursorCol and #CursorRow fields reflect the current row and column of the user's text cursor.  Use
#EventCallback and listen for the event `SEF::CURSOR_POS` to receive updates on changes to CursorCol and #CursorRow.

*********************************************************************************************************************/

static ERR GET_CursorCol(extScintilla *Self, int *Value)
{
   if (Self->initialised()) *Value = SCICALL(SCI_GETCOLUMN, SCICALL(SCI_GETCURRENTPOS));
   else *Value = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CursorRow: The current row of the text cursor.

The #CursorCol and CursorRow fields reflect the current row and column of the user's text cursor.  Use
#EventCallback and listen for the event `SEF::CURSOR_POS` to receive updates on changes to #CursorCol and CursorRow.

*********************************************************************************************************************/

static ERR GET_CursorRow(extScintilla *Self, int *Value)
{
   if (Self->initialised()) *Value = SCICALL(SCI_LINEFROMPOSITION, SCICALL(SCI_GETCURRENTPOS));
   else *Value = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CursorColour: Defines the colour of the text cursor.  Alpha blending is not supported.

*********************************************************************************************************************/

static ERR SET_CursorColour(extScintilla *Self, RGB8 *Value)
{
   Self->CursorColour = *Value;

   if (Self->initialised()) {
      auto &c = Self->CursorColour;
      SCICALL(SCI_SETCARETFORE, (unsigned long int)SCICOLOUR(c.Red, c.Green, c.Blue));
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

static ERR GET_FileDrop(extScintilla *Self, FUNCTION * &Value)
{
   if (Self->FileDrop.defined()) {
      Value = &Self->FileDrop;
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
Focus: Defines the viewport that is monitored for user focus changes.

The editor has the user focus while the Focus viewport or any of its children has the focus.  Keyboard input is
received from the Focus viewport during that time, and the caret is shown and blinks.  If not set prior to
initialisation, the #Viewport is used.  Clicking within the editor gives the focus to one of its internal viewports,
which are children of the #Viewport.

Keyboard input requires a @VectorScene that is linked to a @Surface.  The scene uses the Tab key to move the focus
between keyboard subscribers unless its `IGNORE_TAB` flag is set, in which case the Tab key reaches the editor.

-FIELD-
FoldingMarkers: Enables code folding and shows the fold margin.

Set this field to `true` to enable code folding.  Folding is computed by the active #Lexer, so a document without a
lexer, or with a lexer that does not support folding, has no folds.  The fold margin is shown to the left of the text
and marks each fold with a box that contains a plus (contracted) or minus (expanded) sign.  Clicking the box of a fold
toggles it.  Folds can also be toggled with #ToggleFold().

The lines of a contracted fold are hidden.  They are shown again if the fold is expanded, if the cursor is moved into
them with #GotoLine() or #SelectRange(), or if they are edited.  Setting FoldingMarkers to `false` expands every fold.

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
      if (Self->initialised()) {
         SCICALL(SCI_SETMARGINWIDTHN, 2, 20L);
         Self->API->panSetFolding(true);
      }
   }
   else {
      Self->FoldingMarkers = FALSE;
      if (Self->initialised()) {
         SCICALL(SCI_SETMARGINWIDTHN, 2, 0L);
         Self->API->panSetFolding(false);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Face: The font family used for drawing and measuring document text.

Accepts a font family name supported by the vector module's GetFontHandle() function, including bitmap faces such
as `Clean`.
The default is `Source Code Pro`.  Regular, bold, italic and bold italic styles share this family and #FontSize.
Set this field before or after initialisation.  Changing it reloads the styles and updates layout and margins.
#SetFont() provides the same operation.  A failed resolution preserves the current face and handles.

This field and #FontSize replace the former Font object field; scripts must use these fields to customise fonts.

*********************************************************************************************************************/

static ERR SET_Face(extScintilla *Self, const std::string_view &Value)
{
   if (Value.empty()) return ERR::NullArgs;
   if (Self->initialised()) return resolve_fonts(Self, Value, Self->FontSize);
   Self->Face = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FontSize: The font em size in pixels.

Sets a positive integer pixel size for every text style.  The default is 14 pixels.  This is not a point size and
is independent of display density.  Bitmap faces select their nearest supported point size.
Set this field before or after initialisation.  Changing it reloads the font handles and updates layout and margins.

*********************************************************************************************************************/

static ERR SET_FontSize(extScintilla *Self, int Value)
{
   if (Value <= 0) return ERR::OutOfRange;
   if (Self->initialised()) return resolve_fonts(Self, Self->Face, Value);
   Self->FontSize = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

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
MarginClick: A callback for receiving clicks in the symbol and fold margins.

Set this field with a function reference to receive notifications when the user clicks in the symbol margin or the
fold margin.  This is typically used to toggle breakpoints or bookmarks with #AddMarker() and #RemoveMarker().  The
prototype for the callback function is `Function(*Scintilla, SCMG Margin, INT Line, KQ Qualifiers)`.

The `Margin` is `SCMG::SYMBOLS` or `SCMG::FOLDING`, and the `Line` is the index of the line that was clicked.  The
`Qualifiers` indicate the modifier keys that were held, as a combination of `KQ::SHIFT`, `KQ::CTRL` and `KQ::ALT`.
Modifier keys are only known while the editor has the user focus.

A click in the fold margin has already toggled the fold of the line (if it is a fold header) when the callback is
made.  While MarginClick is defined, a click in the symbol margin is reported instead of selecting the line.  Clicks in
the line number margin always select the line and are not reported.

*********************************************************************************************************************/

static ERR GET_MarginClick(extScintilla *Self, FUNCTION * &Value)
{
   if (Self->MarginClick.defined()) {
      Value = &Self->MarginClick;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_MarginClick(extScintilla *Self, FUNCTION *Value)
{
   if (Self->MarginClick.defined()) Self->MarginClick.unpin();
   if (Value) {
      Self->MarginClick = *Value;
      if (Self->MarginClick.defined()) Self->MarginClick.pin();
   }
   else Self->MarginClick.clear();

   if (Self->API) SCICALL(SCI_SETMARGINSENSITIVEN, 1, (long)Self->MarginClick.defined());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Lexer: The lexer for document styling is defined here.

The lexer can be changed at any time.  Its keyword lists and semantic style colours are refreshed immediately.
Existing folds are expanded and folding remains enabled if #FoldingMarkers is set.  Set the lexer to zero to display
plain text.  ASP uses the HTML lexer with VBScript server blocks; Tiri keywords follow the parser's reserved words.

If the `DETECT_LEXER` flag is set, the lexer is chosen from the file name whenever a file is loaded, and this
overrides any existing value.  The `Lexer` is left unchanged if the file name does not match a known type.

*********************************************************************************************************************/

static ERR SET_Lexer(extScintilla *Self, SCLEX Value)
{
   Self->Lexer = Value;
   if (Self->initialised()) {
      kt::Log log;
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
   Self->LineNumbers = Value ? TRUE : FALSE;
   update_margin_width(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Origin: Sets the #Path field without loading the document.

Setting the Origin will update #Path without updating the content of the Scintilla object.  Use for situations such as
changing the document location or name.

*********************************************************************************************************************/

static ERR SET_Origin(extScintilla *Self, const std::string_view &Value)
{
   Self->Path = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Page: An internally created viewport that represents the extent of the document.

The Page is created on initialisation as a child of the #View.  It represents the virtual extent of the document, and
scrolling is achieved by moving the Page within the View.  The Page can be referenced as the `page` of a scrollbar.

The editor manages the size of the Page.  Its height allows the last line of the document to be scrolled to the
bottom of the View, and its width allows the longest line to be scrolled into view.  If the Page is moved by a third
party, the editor scrolls to match and then snaps the Page to the nearest whole line.

-FIELD-
Path: Identifies the location of a text file to load.

To load data from a text file into a scintilla object, set the Path field.

If the Path is set after initialisation, the existing content will be cleared and data loaded from the location
that you specify.  To change the path without automatically loading from the source file, set the
#Origin field instead.

*********************************************************************************************************************/

static ERR SET_Path(extScintilla *Self, const std::string_view &Value)
{
   kt::Log log;

   log.branch("%.*s", int(Value.size()), Value.data());

   Self->Path = Value;
   if (Self->initialised()) {
      if (load_file(Self, Self->Path) != ERR::Okay) {
         return ERR::File;
      }
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

static ERR GET_EventCallback(extScintilla *Self, FUNCTION * &Value)
{
   if (Self->EventCallback.defined()) {
      Value = &Self->EventCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_EventCallback(extScintilla *Self, FUNCTION *Value)
{
   if (Self->EventCallback.defined()) Self->EventCallback.unpin();
   if (Value) {
      Self->EventCallback = *Value;
      if (Self->EventCallback.defined()) Self->EventCallback.pin();
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
      if (Self->initialised()) {
         auto &c = Self->SelectBkgd;
         SCICALL(SCI_SETSELBACK, (unsigned long int)true, (long int)SCICOLOUR(c.Red, c.Green, c.Blue));
         //SCICALL(SCI_SETSELALPHA, Self->SelectBkgd.Alpha);
      }
   }
   else {
      Self->SelectBkgd.Alpha = 0;
      if (Self->initialised()) {
         SCICALL(SCI_SETSELBACK, (unsigned long int)false, 0L);
         //SCICALL(SCI_SETSELALPHA, 0UL);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
SelectFore: Defines the colour of selected text.  Supports alpha blending.

*********************************************************************************************************************/

static ERR SET_SelectFore(extScintilla *Self, RGB8 *Value)
{
   if ((Value) and (Value->Alpha)) {
      Self->SelectFore = *Value;
      if (Self->initialised()) {
         auto &c = Self->SelectFore;
         SCICALL(SCI_SETSELFORE, (unsigned long int)true, (long int)SCICOLOUR(c.Red, c.Green, c.Blue));
      }
   }
   else {
      Self->SelectFore.Alpha = 0;
      if (Self->initialised()) SCICALL(SCI_SETSELFORE, (unsigned long int)false, 0L);
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

static ERR GET_String(extScintilla *Self, std::string_view &Value)
{
   auto len = SCICALL(SCI_GETLENGTH);

   Self->StringBuffer.clear();
   Self->StringBuffer.resize(len);
   SCICALL(SCI_GETTEXT, len+1, Self->StringBuffer.c_str());
   Value = std::string_view(Self->StringBuffer);
   return ERR::Okay;
}

static ERR SET_String(extScintilla *Self, const std::string_view &Value)
{
   if (Self->initialised()) {
      if (not Value.empty()) SCICALL(SCI_SETTEXT, 0UL, (const char *)Value.data());
      else acClear(Self);
      calc_longest_line(Self); // Sets the scroll width, which determines the width of the Page
   }
   else return ERR::NotInitialised;

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Symbols: Shows the symbol margin, in which markers are displayed.

Set this field to `true` to show the symbol margin to the left of the text.  The margin displays the markers that are
added to lines with #AddMarker().  While the margin is hidden, a line that carries markers is highlighted with their
background colour instead.  Markers that highlight the text itself, such as `SCMK::BACKGROUND`, are displayed
regardless of this setting.

If #MarginClick is defined, clicks in the symbol margin are reported to it.  Otherwise a click in the margin selects
the line, as it does in the line number margin.

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
TextColour: Defines the default colour of foreground text.  Alpha blending is not supported.

TextColour applies to plain text, identifiers and any unclassified styles.  Configure lexer and margin colours
through #ThemeXML.  Theme colours are stored independently for each editor and are retained across lexer changes.

The defaults are a white background, black text and operators, grey comments, firebrick keywords, dark red strings,
blue numbers, green directives and violet types.

*********************************************************************************************************************/

static ERR SET_TextColour(extScintilla *Self, RGB8 *Value)
{
   Self->TextColour = *Value;
   if (Self->initialised()) Self->API->SetStyleColours();

   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ThemeXML: Applies colours from a preconfigured XML object.  This field is write-only.

Assign an initialised @XML object containing one `theme` element with one `palette` child.  The palette's attributes
specify SVG colour strings:

<pre>
&lt;theme&gt;
   &lt;palette background="#232a37" text="white" keyword="#70b8ff" comment="#99aabb"
       string="#b5d99c" number="#d4a5ff" operator="white" preprocessor="#b5d99c"
       type="#ffc080" error="#ff8899" margin="#99aabb" marginBackground="#303745"
       cursor="white" lineHighlight="transparent" selectionForeground="white"
       selectionBackground="#335588"/&gt;
&lt;/theme&gt;
</pre>

The theme element accepts no attributes, and the palette accepts no child elements or non-whitespace text.
All palette attributes are optional and case-sensitive.  Omitted colours retain their current values.  Hex colours, SVG
colour names and colour functions such as `rgb()` and `oklch()` are accepted.  Gradients, painter lists and unknown
attributes are rejected.  Alpha is ignored except for lineHighlight and selection colours, where zero disables the
corresponding highlight or selection override.  Other alpha values currently produce opaque colours.

The colours are copied during assignment and the XML object is not retained or modified.  Assigning before
initialisation configures the initial colours; assigning afterwards repaints without changing text, selection or
contracted folds.  Invalid themes leave all colours unchanged.

*********************************************************************************************************************/

static ERR SET_ThemeXML(extScintilla *Self, objXML *Value)
{
   if (not Value) return ERR::NullArgs;
   if ((Value->classID() != CLASSID::XML) and (Value->baseClassID() != CLASSID::XML)) {
      return ERR::WrongObjectType;
   }
   ScopedObjectLock<objXML> xml(Value);
   if (not xml.granted()) return xml.error;
   if (not Value->initialised()) return ERR::NotInitialised;

   auto ignorable = [](const XTag &Tag) {
      if ((Tag.Flags & (XTF::COMMENT|XTF::INSTRUCTION)) != XTF::NIL) return true;
      return (not Tag.Attribs.empty()) and Tag.isContent() and
         (Tag.Attribs[0].Value.find_first_not_of(" \t\r\n") IS std::string::npos);
   };

   const XTag *theme = nullptr;
   for (const auto &tag : Value->Tags) {
      if (ignorable(tag)) continue;
      if (theme or tag.Attribs.empty() or (tag.Attribs[0].Name != "theme")) return ERR::Syntax;
      theme = &tag;
   }
   if (not theme or (theme->Attribs.size() != 1)) return ERR::Syntax;
   const XTag *palette = nullptr;
   for (const auto &tag : theme->Children) {
      if (ignorable(tag)) continue;
      if (palette or tag.Attribs.empty() or (tag.Attribs[0].Name != "palette")) return ERR::Syntax;
      palette = &tag;
   }
   if (not palette) return ERR::Syntax;
   for (const auto &tag : palette->Children) {
      if (not ignorable(tag)) return ERR::Syntax;
   }

   static constexpr struct {
      std::string_view Name;
      RGB8 extScintilla::*Field;
   } colours[] = {
      { "comment", &extScintilla::CommentColour },
      { "keyword", &extScintilla::KeywordColour },
      { "string", &extScintilla::StringColour },
      { "number", &extScintilla::NumberColour },
      { "operator", &extScintilla::OperatorColour },
      { "preprocessor", &extScintilla::PreprocessorColour },
      { "type", &extScintilla::TypeColour },
      { "error", &extScintilla::ErrorColour },
      { "margin", &extScintilla::MarginColour },
      { "marginBackground", &extScintilla::MarginBkgd },
      { "text", &extScintilla::TextColour },
      { "background", &extScintilla::BkgdColour },
      { "cursor", &extScintilla::CursorColour },
      { "lineHighlight", &extScintilla::LineHighlight },
      { "selectionForeground", &extScintilla::SelectFore },
      { "selectionBackground", &extScintilla::SelectBkgd }
   };

   std::vector<std::pair<RGB8 extScintilla::*, RGB8>> updates;
   for (size_t i=1; i < palette->Attribs.size(); i++) {
      const auto &attribute = palette->Attribs[i];
      auto colour = std::find_if(std::begin(colours), std::end(colours), [&attribute](const auto &Colour) {
         return Colour.Name IS attribute.Name;
      });
      if (colour IS std::end(colours)) return ERR::Syntax;

      VectorPainter painter = {};
      std::string_view remainder;
      if ((vec::ReadPainter(nullptr, attribute.Value, &painter, &remainder) != ERR::Okay) or
          painter.Gradient or painter.Pattern or painter.Image or
          (remainder.find_first_not_of(" \t\r\n") != std::string_view::npos)) return ERR::Syntax;
      const auto &rgb = painter.Colour;
      if (not std::isfinite(rgb.Red) or not std::isfinite(rgb.Green) or not std::isfinite(rgb.Blue) or
          not std::isfinite(rgb.Alpha)) return ERR::Syntax;
      auto channel = [](float Value) { return uint8_t(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255)); };
      updates.emplace_back(colour->Field, RGB8 { channel(rgb.Red), channel(rgb.Green), channel(rgb.Blue),
         channel(rgb.Alpha) });
   }

   for (const auto &[field, colour] : updates) Self->*field = colour;
   if (Self->initialised()) {
      clear_bitmap(Self);
      Self->API->SetStyleColours();
      SET_CursorColour(Self, &Self->CursorColour);
      SET_LineHighlight(Self, &Self->LineHighlight);
      SET_SelectFore(Self, &Self->SelectFore);
      SET_SelectBkgd(Self, &Self->SelectBkgd);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
View: An internally created viewport that clips the visible content of the editor.

The View is created on initialisation as a child of the #Viewport, and covers its entire area.  It represents the
visible window into the document and clips the #Page.  The View can be referenced as the `view` of a scrollbar, which
may reduce the size of the View by changing its `XOffset` and `YOffset` values.

-FIELD-
Viewport: The viewport that will host the editor.

This compulsory field refers to the @VectorViewport that will host the editor's graphics.  If not set prior to
initialisation, the Scintilla object's owner will be used if it is a @VectorViewport.

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

static ERR resolve_fonts(extScintilla *Self, std::string_view Face, int Size)
{
   kt::Log log(__FUNCTION__);
   APTR handles[4] = {};
   constexpr CSTRING styles[] = { "Regular", "Bold", "Italic", "Bold Italic" };

   if (auto error = vec::GetFontHandle(Face, styles[0], 0, Size, &handles[0]); error != ERR::Okay) return error;

   for (int i = 0; i < 4; i++) {
      if (i and (vec::GetFontHandle(Face, styles[i], 0, Size, &handles[i]) != ERR::Okay)) {
         handles[i] = handles[0];
         log.warning("Unable to resolve %.*s %s; using Regular.", int(Face.size()), Face.data(), styles[i]);
      }
   }

   Self->Face = Face;
   Self->FontSize = Size;
   std::copy(std::begin(handles), std::end(handles), Self->FontHandles);

   if (Self->initialised()) {
      Self->API->panFontChanged();
      calc_longest_line(Self);
      Self->MarginDigits = -1;
      update_margin_width(Self);
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Create the View and Page viewports within the host Viewport, and the Canvas rectangle that presents the Bitmap that
// Scintilla paints into.  The Bitmap covers the visible area of the View and is wrapped by a VectorImage definition
// that serves as the Canvas fill.  The Page represents the extent of the document, and is sized by ModifyScrollBars().
//
// The View is sized with offsets so that a scrollbar can reserve space within the host by changing them.

static ERR create_canvas(extScintilla *Self)
{
   kt::Log log(__FUNCTION__);

   if (!(Self->View = objVectorViewport::create::global(
         fl::Name("sciView"),
         fl::Owner(Self->Viewport->UID),
         fl::Overflow(VOF::HIDDEN),
         fl::X(0), fl::Y(0), fl::XOffset(0), fl::YOffset(0)))) return log.warning(ERR::CreateObject);

   Unit w, h;
   Self->View->getWidth(w);
   Self->View->getHeight(h);
   Self->ViewWidth  = std::max(1, int(std::lround(double(w))));
   Self->ViewHeight = std::max(1, int(std::lround(double(h))));
   Self->PageWidth  = Self->ViewWidth;
   Self->PageHeight = Self->ViewHeight;

   if (!(Self->Page = objVectorViewport::create::global(
         fl::Name("sciPage"),
         fl::Owner(Self->View->UID),
         fl::X(0), fl::Y(0),
         fl::Width(Self->PageWidth), fl::Height(Self->PageHeight)))) return log.warning(ERR::CreateObject);

   // The Bitmap is one pixel wider and taller than the View to cover the sub-pixel alignment offset (see
   // align_canvas()).

   if (!(Self->Bitmap = objBitmap::create::local(
         fl::Width(Self->ViewWidth + 1),
         fl::Height(Self->ViewHeight + 1),
         fl::BitsPerPixel(32),
         fl::Flags(BMF::ALPHA_CHANNEL)))) return log.warning(ERR::CreateObject);

   clear_bitmap(Self);

   if (!(Self->FillColour = objVectorColour::create::local())) return log.warning(ERR::CreateObject);
   if (!(Self->StrokeColour = objVectorColour::create::local())) return log.warning(ERR::CreateObject);

   // Scene definitions must belong to the scene.  The scene removes the definition automatically when the image is
   // freed.

   if (!(Self->Image = objVectorImage::create::global(
         fl::Owner(Self->Scene->UID),
         fl::Bitmap(Self->Bitmap),
         fl::Units(VUNIT::BOUNDING_BOX),
         fl::SpreadMethod(VSPREAD::CLIP),
         fl::AspectRatio(ARF::X_MIN|ARF::Y_MIN)))) return log.warning(ERR::CreateObject);

   SubscribeAction(Self->Image, AC::Free, C_FUNCTION(notify_free_image));

   auto def_name = std::format("sci{}", Self->UID);
   if (auto error = Self->Scene->addDef(def_name, Self->Image); error != ERR::Okay) return log.warning(error);

   if (!(Self->Canvas = objVectorRectangle::create::global(
         fl::Name("sciCanvas"),
         fl::Owner(Self->Page->UID),
         fl::X(0), fl::Y(0),
         fl::Width(Self->Bitmap->Width), fl::Height(Self->Bitmap->Height),
         fl::Cursor(PTC::TEXT),
         fl::Fill(std::format("url(#{})", def_name))))) return log.warning(ERR::CreateObject);

   // The View's path has already been generated in order to read its size, so the first path change is not reported
   // to feedback_view().

   align_canvas(Self, double(w), double(h));

   Self->View->subscribeFeedback(FM::PATH_CHANGED, C_FUNCTION(feedback_view));
   Self->Page->subscribeFeedback(FM::PATH_CHANGED, C_FUNCTION(feedback_page));
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR load_file(extScintilla *Self, std::string_view Path)
{
   kt::Log log(__FUNCTION__);
   int64_t size, len;
   ERR error = ERR::Okay;

   if (auto file = objFile::create::local(fl::Flags(FL::READ), fl::Path(Path))) {
      if ((file->Flags & FL::STREAM) != FL::NIL) {
         if (!file->startStream(Self->UID, FL::READ, 0)) {
            acClear(Self);

            SubscribeAction(file, AC::Write, C_FUNCTION(notify_write));
            Self->FileStream = file;
            file = nullptr;
         }
         else error = ERR::Failed;
      }
      else if (!file->getSize(size)) {
         if (size > 0) {
            if (size < 1024 * 1024 * 10) {
               std::vector<char> str(size + 1);
               if (!file->read(std::span<int8_t>((int8_t *)str.data(), size_t(size)), &len)) {
                  str[len] = 0;
                  SCICALL(SCI_SETTEXT, str.data());
                  SCICALL(SCI_EMPTYUNDOBUFFER);
                  error = ERR::Okay;

                  calc_longest_line(Self);
               }
               else error = ERR::Read;
            }
            else error = ERR::BufferOverflow;
         }
         else error = ERR::Okay; // File is empty
      }
      else error = ERR::File;

      if (file) FreeResource(file);
   }
   else error = ERR::File;

   if ((!error) and ((Self->Flags & SCIF::DETECT_LEXER) != SCIF::NIL)) {
      auto i = Path.find_last_of("/\\:");
      if (i != std::string::npos) Path.remove_prefix(i+1);

      for (i=0; i < std::size(glLexers); i++) {
         if (wildcmp(glLexers[i].File, Path)) {
            kt::Log log;
            Self->Lexer = glLexers[i].Lexer;
            log.branch("Lexer for the loaded file is %d.", int(Self->Lexer));
            Self->API->SetLexer(int(Self->Lexer));
            break;
         }
      }
      if (i >= std::size(glLexers)) log.msg("Failed to choose a lexer for %.*s", int(Path.size()), Path.data());
   }

   return error;
}

//********************************************************************************************************************
// Receiver for keyboard input from the Focus viewport.  The modifier state is recorded for mouse input, which does not
// report it.

static ERR key_event(objVectorViewport *Viewport, KQ Qualifiers, KEY Code, int Unicode)
{
   auto Self = (extScintilla *)CurrentContext();

   Self->KeyShift = (Qualifiers & KQ::SHIFT) != KQ::NIL;
   Self->KeyCtrl  = (Qualifiers & KQ::CTRL) != KQ::NIL;
   Self->KeyAlt   = (Qualifiers & KQ::ALT) != KQ::NIL;

   if ((Qualifiers & KQ::PRESSED) IS KQ::NIL) return ERR::Okay;
   if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) return ERR::Okay;
   if (not Self->API) return ERR::Okay;

   kt::Log log(__FUNCTION__);
   log.traceBranch("Code: %d, Qualifiers: $%.8x, Unicode: %d", int(Code), int(Qualifiers), Unicode);

   Self->API->panKeyDown(Code, Qualifiers, Unicode);
   return ERR::Okay;
}

//********************************************************************************************************************
// Translate the keys that always act as commands to Scintilla key codes.  Keypad keys that do not produce a
// character, i.e. while Num Lock is off, are navigation keys.  Returns zero for all other keys.

static int command_key(KEY Code, bool Printable)
{
   switch (Code) {
      case KEY::TAB:       return SCK_TAB;
      case KEY::DOWN:      return SCK_DOWN;
      case KEY::UP:        return SCK_UP;
      case KEY::LEFT:      return SCK_LEFT;
      case KEY::RIGHT:     return SCK_RIGHT;
      case KEY::HOME:      return SCK_HOME;
      case KEY::END:       return SCK_END;
      case KEY::PAGE_UP:   return SCK_PRIOR;
      case KEY::PAGE_DOWN: return SCK_NEXT;
      case KEY::DELETE:    return SCK_DELETE;
      case KEY::INSERT:    return SCK_INSERT;
      case KEY::ENTER:
      case KEY::NP_ENTER:  return SCK_RETURN;
      case KEY::ESCAPE:    return SCK_ESCAPE;
      case KEY::BACKSPACE: return SCK_BACK;
      default: break;
   }

   if (Printable) return 0;

   switch (Code) {
      case KEY::NP_0:   return SCK_INSERT;
      case KEY::NP_1:   return SCK_END;
      case KEY::NP_2:   return SCK_DOWN;
      case KEY::NP_3:   return SCK_NEXT;
      case KEY::NP_4:   return SCK_LEFT;
      case KEY::NP_6:   return SCK_RIGHT;
      case KEY::NP_7:   return SCK_HOME;
      case KEY::NP_8:   return SCK_UP;
      case KEY::NP_9:   return SCK_PRIOR;
      case KEY::NP_DOT: return SCK_DELETE;
      default: return 0;
   }
}

//********************************************************************************************************************
// Translate a character key to the unshifted character that Scintilla's key map uses for shortcuts.  Letters are
// upper case, e.g. Ctrl+Z is mapped from 'Z'.  Returns zero for keys that have no shortcut representation.

static int shortcut_key(KEY Code)
{
   if ((int(Code) >= int(KEY::A)) and (int(Code) <= int(KEY::Z))) return int(Code) - int(KEY::A) + 'A';
   if ((int(Code) >= int(KEY::ONE)) and (int(Code) <= int(KEY::NINE))) return int(Code) - int(KEY::ONE) + '1';

   switch (Code) {
      case KEY::ZERO:          return '0';
      case KEY::REVERSE_QUOTE: return '`';
      case KEY::MINUS:         return '-';
      case KEY::EQUALS:        return '=';
      case KEY::L_SQUARE:      return '[';
      case KEY::R_SQUARE:      return ']';
      case KEY::SEMI_COLON:    return ';';
      case KEY::APOSTROPHE:    return '\'';
      case KEY::COMMA:         return ',';
      case KEY::DOT:           return '.';
      case KEY::SLASH:         return '/';
      case KEY::BACK_SLASH:    return '\\';
      case KEY::SPACE:         return ' ';
      default: return 0;
   }
}

//********************************************************************************************************************

static void report_event(extScintilla *Self, SEF Event)
{
   Event &= Self->EventFlags; // Only the events that the client subscribed to are reported
   if (Event != SEF::NIL) {
      if (Self->EventCallback.stale()) {
         Self->EventCallback.unpin();
         Self->EventCallback.clear();
      }

      if (Self->EventCallback.isC()) {
         kt::SwitchContext ctx(Self->EventCallback.Context);
         auto routine = (void (*)(extScintilla *, SEF, APTR)) Self->EventCallback.Routine;
         routine(Self, Event, Self->EventCallback.Meta);
      }
      else if (Self->EventCallback.isScript()) {
         sc::Call(Self->EventCallback, std::to_array<ScriptArg>({ { "Scintilla", Self, FD_OBJECTPTR }, { "EventFlags", int64_t(Event) } }));
      }
   }
}

//********************************************************************************************************************
// Report a click in a sensitive margin to the MarginClick callback.

static void report_margin_click(extScintilla *Self, SCMG Margin, int Line, KQ Qualifiers)
{
   if (Self->MarginClick.stale()) {
      Self->MarginClick.unpin();
      Self->MarginClick.clear();
   }

   if (Self->MarginClick.isC()) {
      kt::SwitchContext ctx(Self->MarginClick.Context);
      auto routine = (void (*)(extScintilla *, SCMG, int, KQ, APTR))Self->MarginClick.Routine;
      routine(Self, Margin, Line, Qualifiers, Self->MarginClick.Meta);
   }
   else if (Self->MarginClick.isScript()) {
      sc::Call(Self->MarginClick, std::to_array<ScriptArg>({
         { "Scintilla", Self, FD_OBJECTPTR },
         { "Margin", int(Margin) },
         { "Line", Line },
         { "Qualifiers", int(Qualifiers) }
      }));
   }
}

//********************************************************************************************************************

static void calc_longest_line(extScintilla *Self)
{
   int linewidth;
   static const int LINE_COUNT_LIMIT = 2000;

   if (not Self->FontHandles[0]) return;

   kt::Log log(__FUNCTION__);
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
         int start = SCICALL(SCI_POSITIONFROMLINE, i);
         int end = SCICALL(SCI_GETLINEENDPOSITION, i);
         // Subtract the line start so margins and horizontal scrolling do not affect the measured width.
         linewidth = SCICALL(SCI_POINTXFROMPOSITION, 0, end) - SCICALL(SCI_POINTXFROMPOSITION, 0, start);

         if (linewidth > cwidth) {
            cline = i;
            cwidth = linewidth;
         }
      }

      Self->LongestLine  = cline;
      Self->LongestWidth = cwidth;

      if (lines IS LINE_COUNT_LIMIT) {
         Self->LongestWidth += 1024; // Add lots of extra space in case there are much longer lines further on in the document
      }
      else Self->LongestWidth += 30;
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
// The tick and idle timers end their own subscriptions when they are no longer required.  See UpdateTimers().

static ERR tick_timer(extScintilla *Self, int64_t Elapsed, int64_t CurrentTime)
{
   if ((Self->API) and (Self->API->panTick())) return ERR::Okay;
   Self->TickTimer = nullptr;
   return ERR::Terminate;
}

static ERR idle_timer(extScintilla *Self, int64_t Elapsed, int64_t CurrentTime)
{
   if ((Self->API) and (Self->API->panIdle())) return ERR::Okay;
   Self->IdleTimer = nullptr;
   return ERR::Terminate;
}

//********************************************************************************************************************

#include "class_scintilla_ext.cxx"
#include "pan_classes.cxx"
#include "pan_window.cxx"
#include "pan_menu.cxx"
#include "pan_surface.cxx"
#include "pan_listbox.cxx"

//********************************************************************************************************************
// Paint the Dirty region of the client into the Bitmap, then ask the scene to redraw the Canvas.  This is called from
// the Repaint method, which is queued by invalidate_area().

static void paint_dirty(extScintilla *Self)
{
   if ((not Self->API) or (not Self->Bitmap)) return;

   update_margin_width(Self); // A change to the margin width extends the Dirty region
   Self->API->panSyncPage();  // The Canvas must be positioned for the topLine that is about to be painted

   auto dirty = Self->Dirty;
   Self->Dirty = { 0, 0, 0, 0 };
   if ((dirty.Right <= dirty.Left) or (dirty.Bottom <= dirty.Top)) return;

   // Scintilla paints whole lines, which can extend beyond the Dirty region.  A disabled editor is always painted in
   // full so that the dimming overlay covers every repainted pixel exactly once.

   bool disabled = (Self->Flags & SCIF::DISABLED) != SCIF::NIL;
   if (disabled) dirty = { 0, 0, Self->ViewWidth, Self->ViewHeight };

   Self->API->panPaint(Scintilla::PRectangle(dirty.Left, dirty.Top, dirty.Right, dirty.Bottom));

   if (disabled) {
      SurfacePan surface;
      surface.Init(Self->Bitmap, Self);
      auto &bkgd = Self->BkgdColour;
      surface.BlendRectangle(Scintilla::PRectangle(0, 0, Self->Bitmap->Width, Self->Bitmap->Height),
         Scintilla::ColourAllocated(SCICOLOUR(bkgd.Red, bkgd.Green, bkgd.Blue)), 160);
   }

   if ((Self->Canvas) and (Self->Scene) and (Self->Scene->SurfaceID)) acDraw(Self->Canvas);
}

//********************************************************************************************************************

extScintilla::~extScintilla() {
   free_context_menu(this); // Before the API is deleted, because the script reports the menu closing
   delete API;

   if (EventCallback.defined()) {
      EventCallback.unpin();
      EventCallback.clear();
   }

   if (MarginClick.defined()) {
      MarginClick.unpin();
      MarginClick.clear();
   }

   if (TickTimer) UpdateTimer(TickTimer, 0);
   if (IdleTimer) UpdateTimer(IdleTimer, 0);

   if (Focus) {
      kt::SwitchContext ctx(this);
      Focus->subscribeFeedback(FM::NIL, C_FUNCTION(feedback_focus));
      if (Focus != Viewport) UnsubscribeAction(Focus, AC::NIL);
   }
   if (Viewport) UnsubscribeAction(Viewport, AC::NIL);

   if (Image) { // Freeing the image also removes its definition from the scene
      UnsubscribeAction(Image, AC::NIL);
      FreeResource(Image);
   }

   if (View) FreeResource(View); // The Page and Canvas are children of the View

   if (FileStream)  FreeResource(FileStream);
   if (Bitmap)      FreeResource(Bitmap);
   if (FillColour)  FreeResource(FillColour);
   if (StrokeColour) FreeResource(StrokeColour);
}

//********************************************************************************************************************

#include "class_scintilla_def.cxx"

static const FieldArray clFields[] = {
   { "Face",           FDF_CPPSTRING|FDF_RW, nullptr, SET_Face },
   { "Path",           FDF_CPPSTRING|FDF_RW, nullptr, SET_Path },
   { "EventFlags",     FDF_INT|FDF_FLAGS|FDF_RW, nullptr, nullptr, &clScintillaEventFlags },
   { "Flags",          FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clScintillaFlags },
   { "Viewport",       FDF_OBJECT|FDF_RI, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Focus",          FDF_OBJECT|FDF_RI, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "View",           FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Page",           FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "LeftMargin",     FDF_INT|FDF_RW, nullptr, SET_LeftMargin },
   { "RightMargin",    FDF_INT|FDF_RW, nullptr, SET_RightMargin },
   { "LineHighlight",  FDF_STRUCT|FDF_RW, nullptr, SET_LineHighlight, "RGB8" },
   { "SelectFore",     FDF_STRUCT|FDF_RW, nullptr, SET_SelectFore, "RGB8" },
   { "SelectBkgd",     FDF_STRUCT|FDF_RW, nullptr, SET_SelectBkgd, "RGB8" },
   { "BkgdColour",     FDF_STRUCT|FDF_RW, nullptr, SET_BkgdColour, "RGB8" },
   { "CursorColour",   FDF_STRUCT|FDF_RW, nullptr, SET_CursorColour, "RGB8" },
   { "TextColour",     FDF_STRUCT|FDF_RW, nullptr, SET_TextColour, "RGB8" },
   { "CursorRow",      FDF_INT|FDF_R, GET_CursorRow },
   { "CursorCol",      FDF_INT|FDF_R, GET_CursorCol },
   { "Lexer",          FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_Lexer, &clScintillaLexer },
   { "Modified",       FDF_INT|FDF_RW, nullptr, SET_Modified },
   { "FontSize",       FDF_INT|FDF_RW, nullptr, SET_FontSize },

   // Virtual fields
   { "ThemeXML",       FDF_OBJECT|FDF_W|FDF_PURE, nullptr, SET_ThemeXML, CLASSID::XML },
   { "AllowTabs",      FDF_INT|FDF_RW|FDF_PURE,   GET_AllowTabs, SET_AllowTabs },
   { "AutoIndent",     FDF_INT|FDF_RW|FDF_PURE,   GET_AutoIndent, SET_AutoIndent },
   { "FileDrop",       FDF_FUNCTION|FDF_RW|FDF_PURE, GET_FileDrop, SET_FileDrop },
   { "FoldingMarkers", FDF_INT|FDF_RW|FDF_PURE,   GET_FoldingMarkers, SET_FoldingMarkers },
   { "LineCount",      FDF_INT|FDF_R,             GET_LineCount },
   { "LineNumbers",    FDF_INT|FDF_RW|FDF_PURE,   GET_LineNumbers, SET_LineNumbers },
   { "MarginClick",    FDF_FUNCTION|FDF_RW|FDF_PURE, GET_MarginClick, SET_MarginClick },
   { "Origin",         FDF_CPPSTRING|FDF_W, nullptr, SET_Origin },
   { "ShowWhitespace", FDF_INT|FDF_RW|FDF_PURE,      GET_ShowWhitespace, SET_ShowWhitespace },
   { "EventCallback",  FDF_FUNCTION|FDF_RW|FDF_PURE, GET_EventCallback, SET_EventCallback },
   { "String",         FDF_CPPSTRING|FDF_RW,      GET_String, SET_String },
   { "Symbols",        FDF_INT|FDF_RW|FDF_PURE,   GET_Symbols, SET_Symbols },
   { "TabWidth",       FDF_INT|FDF_RW|FDF_PURE,   GET_TabWidth, SET_TabWidth },
   { "Wordwrap",       FDF_INT|FDF_RW|FDF_PURE,   GET_Wordwrap, SET_Wordwrap },
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
      fl::FileExtension("txt|text"),
      fl::Icon("filetypes/text"),
      fl::Path("modules:scintilla"));

   return clScintilla ? ERR::Okay : ERR::AddClass;
}

//********************************************************************************************************************

KOTUKU_MOD(MODInit, nullptr, nullptr, MODExpunge, nullptr, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_scintilla_module() { return &ModHeader; }
