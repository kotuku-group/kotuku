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

The editor is hosted by a @VectorViewport, which is referenced by #Viewport and defaults to the Scintilla object's
owner.  On initialisation the editor creates a #View viewport that covers the host and clips the content, and a #Page
viewport within the View that represents the extent of the document.  Scrolling moves the Page within the View, so the
View and Page can be passed to a scrollbar such as `gui/scrollbar` without further integration.  The visible text is
painted into an internal bitmap that is drawn as the fill of a rectangle within the Page, and only the regions affected
by edits, selection changes and scrolling are repainted.  The editor follows changes to the size of the host.  If the
host is freed before the editor, the editor closes its pop-ups and releases its graphics, and operations that display
content, such as #ShowCompletions(), fail with `ERR::NotPossible`.

Keyboard input and the caret require the user focus, which is monitored through the #Focus viewport.  The hosting
@VectorScene must be linked to a @Surface for keyboard input, and should have the `VPF::IGNORE_TAB` flag if the Tab
key is to reach the editor.

Text is drawn by the Vector module with the font selected by #TextStyle.  Scalable and bitmap
faces are supported, and all styles share the same family and size.  Lexer styles, margins, the caret, the line
highlight and the selection are coloured from the per-editor palette, which is configured in one operation through
#ThemeXML.  Applications that use the GUI packages can build the theme from their GUI palette and reassign it when
the palette changes.  The editor has no dependency on the GUI packages.

Sizes and positions, including #TextStyle sizes, #LeftMargin, #RightMargin, #ScrollToPoint() and the extent of the #Page,
are expressed in the units of the #View.  If the host is scaled, the editor renders at the device resolution: the
device scale is taken from all transforms and view boxes that apply to the #View, the bitmap is sized in device
pixels, and fonts, margins and the caret are drawn at that scale.  An editor in a host scaled by 1.5 therefore renders
the same pixels as an unscaled editor with a #TextStyle size that is 1.5 times larger, and pointer input is mapped to the
same positions.  A change of scale, such as zooming the window, reloads the fonts and repaints the editor while
retaining the top line and horizontal scroll position.  The completion list and call tip are closed by a change of
scale.  The display density is not applied automatically; applications that use the GUI packages scale the #TextStyle size by
`gui.interface.dpi / 96` as the other widgets do.

Releasing the right mouse button over the editor opens a context menu with Undo, Redo, Cut, Copy, Paste, Delete and
Select All.  Items are disabled when they cannot be used: edits require `SCIF::EDIT`, Cut, Copy and Delete require a
selection, and Paste requires text on the clipboard.  The menu is displayed with `gui/menu` at the pointer position,
including over a transformed or scrolled host.  Closing the menu with Escape or by choosing an item returns the focus
to the editor, while moving the focus elsewhere closes it without doing so.

Code assistance is provided through a completion list and a call tip, which the client opens with
#ShowCompletions() and #ShowCallTip().  The #CompletionCallback reports the events that typically prompt them, such
as typed characters and Ctrl+Space, as well as accepted and cancelled completions.  Only one of the two pop-ups is
open at a time.  They are drawn above the content of the scene with the editor's font and colours, are positioned
at the text that they refer to, and are kept within the bounds of the scene.  They follow that text when the editor
scrolls, and close if it scrolls out of view.  Pop-ups are drawn at the device scale of the host, but are not
rotated with it, although their position follows it.  Refer to #ShowCompletions() for the
keyboard and pointer interaction.

Drag and drop requires the hosting @VectorScene to have `VPF::DROP_TARGETS` set.  The editor registers its visible
#View as a drop destination.  File paths are reported through #FileDrop.  Text drops insert at the pointer's document
position, preserve existing text, convert line endings to the document mode and require `SCIF::EDIT`.

Dragging selected text within or between editors moves it by default, while Ctrl copies it.  Source text is removed
only after an editor commits the insertion; dropping onto another kind of widget exports a copy.  Native outbound
dragging and X11 XDND inbound transport depend on Display support and are separate from these internal operations.

On X11, selecting text publishes it as the primary selection.  The middle mouse button inserts the primary selection
at that position as one undo action, with document line-ending conversion and read-only enforcement.

The following limitations apply to the current implementation:

<list type="bullet">
<li>Device scaling applies to scalable faces only.  Bitmap faces have fixed sizes, so they are drawn at the size of
the #View and resampled if the host is scaled.</li>
<li>The device scale is limited to the range 0.5 to 4.  Beyond it, and if the host is rotated, skewed or scaled
unequally on each axis, the bitmap is resampled.  The larger axis scale is used in those cases so that text is never
magnified.  Lines, such as indentation guides and fold lines, remain one device pixel wide.</li>
<li>A modifier key that is pressed while the application window does not have the keyboard focus is not known to the
editor until it is pressed again.</li>
<li>Rectangular clipboard content is recognised only within the process that copied it.  Refer to #Clipboard() for
details.</li>
<li>Primary selection is supported only by the X11 display driver.</li>
<li>If the #Page is moved a large distance by a third party, such as a scrollbar, the previous content can be shown
for one frame before the editor repaints.</li>
</list>
-END-

*********************************************************************************************************************/

//#define DEBUG
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

#include "engine_headers.h"
#include "Lexilla.h"

#include <kotuku/main.h>
#include <kotuku/modules/xml.h>
#include <kotuku/modules/config.h>
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
#include "lexer_profiles.h"

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
   { "*.lua|*.tiri",  SCLEX::TIRI },
   { "*.dmd",         SCLEX::HTML },
   { "*.html|*.htm|*.xhtml", SCLEX::HTML },
   { "makefile|*.make", SCLEX::MAKEFILE },
   { "*.pas",           SCLEX::PASCAL },
   { "*.perl|*.pl",     SCLEX::PERL },
   { "*.php",           SCLEX::PHPSCRIPT },
   { "*.reb|*.rebol",   SCLEX::REBOL },
   { "*.prop|*.cfg",    SCLEX::PROPERTIES },
   { "*.py",            SCLEX::PYTHON },
   { "*.ruby|*.rb",     SCLEX::RUBY },
   { "*.sql",           SCLEX::SQL },
   { "*.vb",            SCLEX::VB },
   { "*.vbscript",      SCLEX::VBSCRIPT },
   { "*.xml",           SCLEX::XML },
   { "*.json|*.jsonld",  SCLEX::JSON },
   { "*.md|*.markdown",  SCLEX::MARKDOWN }
};

#define SCICOLOUR(red,green,blue) (((uint8_t)(blue))<<16)|(((uint8_t)(green))<<8)|((uint8_t)(red))
#define SCICALL     Self->API->SendScintilla

//********************************************************************************************************************
// Scintilla class definition.

static ERR GET_ActivePopup(extScintilla *, SCPOP *);
static ERR GET_AllowTabs(extScintilla *, int *);
static ERR GET_AutoIndent(extScintilla *, int *);
static ERR GET_CompletionCallback(extScintilla *, FUNCTION * &);
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
static ERR SET_CompletionCallback(extScintilla *, FUNCTION *);
static ERR SET_FileDrop(extScintilla *, FUNCTION *);
static ERR SET_FoldingMarkers(extScintilla *, int);
static ERR SET_LeftMargin(extScintilla *, int);
static ERR SET_Lexer(extScintilla *, SCLEX);
static ERR SET_LineNumbers(extScintilla *, int);
static ERR SET_MarginClick(extScintilla *, FUNCTION *);
static ERR SET_Path(extScintilla *, const std::string_view &);
static ERR SET_Modified(extScintilla *, int);
static ERR SET_Origin(extScintilla *, const std::string_view &);
static ERR SET_RightMargin(extScintilla *, int);
static ERR SET_ShowWhitespace(extScintilla *, int);
static ERR SET_EventCallback(extScintilla *, FUNCTION *);
static ERR SET_String(extScintilla *, const std::string_view &);
static ERR SET_Symbols(extScintilla *, int);
static ERR SET_TabWidth(extScintilla *, int);
static ERR SET_Wordwrap(extScintilla *, int);

//********************************************************************************************************************

static ERR SET_TextStyle(extScintilla *, const std::string_view &);
static ERR resolve_fonts(extScintilla *, const std::string &, const std::string &, int);
static ERR create_scintilla(void);
static ERR create_canvas(extScintilla *);
static void queue_repaint(extScintilla *);
static void update_popups(extScintilla *);
static bool popup_has_focus(extScintilla *);
static ERR feedback_view(objVectorViewport *, FM);
static ERR feedback_page(objVectorViewport *, FM);
static ERR feedback_focus(objVectorViewport *, FM);
static void update_focus(extScintilla *);
static ERR input_canvas(objVectorRectangle *, const InputEvent *);
static void update_host_transform(extScintilla *, double, double);
static double device_scale(extScintilla *, bool);
static void update_view(extScintilla *);
static void align_canvas(extScintilla *);
static void apply_metrics(extScintilla *);
static bool face_scalable(std::string_view);
static void fonts_changed(extScintilla *);
static void sync_page(extScintilla *);
static void clear_bitmap(extScintilla *);
static void clear_bitmap_border(extScintilla *);
static ERR resize_bitmap(extScintilla *, int, int);
static void invalidate_area(extScintilla *, int, int, int, int);
static void paint_dirty(extScintilla *);
static void update_margin_width(extScintilla *);
static ERR load_file(extScintilla *, std::string_view);
static void calc_longest_line(extScintilla *);

// Convert a length in View units to client pixels.

static int scaled(extScintilla *Self, int Value)
{
   return int(std::lround(Value * Self->Scale));
}

// Pin a copy before releasing the stored callback, including when Value aliases Callback.

static void replace_callback(FUNCTION &Callback, const FUNCTION *Value)
{
   auto replacement = Value ? *Value : FUNCTION();
   if (replacement.defined()) replacement.pin();
   if (Callback.defined()) Callback.unpin();
   Callback = replacement;
}

// Scintilla draws opaque highlights beneath the text, and translucent highlights over it.

static uptr_t scintilla_alpha(uint8_t Alpha)
{
   return (Alpha < 255) ? Alpha : (uptr_t)SC_ALPHA_NOALPHA;
}

//********************************************************************************************************************
// Applies the caret, line highlight and selection colours of the theme to Scintilla.  Style colours are applied
// separately by SetStyleColours().  An alpha of zero disables the line highlight and the selection overrides.

static void apply_theme_elements(extScintilla *Self)
{
   if (not Self->API) return;

   const auto &cursor = Self->CursorColour;
   SCICALL(SCI_SETCARETFORE, (uptr_t)SCICOLOUR(cursor.Red, cursor.Green, cursor.Blue));

   const auto &line = Self->LineHighlight;
   SCICALL(SCI_SETCARETLINEBACK, SCICOLOUR(line.Red, line.Green, line.Blue));
   SCICALL(SCI_SETCARETLINEBACKALPHA, scintilla_alpha(line.Alpha));
   SCICALL(SCI_SETCARETLINEVISIBLE, (uptr_t)(line.Alpha > 0));

   const auto &whitespace = Self->Whitespace;
   SCICALL(SCI_SETELEMENTCOLOUR, uptr_t(Scintilla::Element::WhiteSpace),
      sptr_t(Scintilla::Internal::ColourRGBA(whitespace.Red, whitespace.Green, whitespace.Blue).AsInteger()));

   // A colour with zero alpha resets its element to Scintilla's default.  Translucent background colours retain their
   // alpha, which SCI_SETELEMENTCOLOUR applies after SCI_SETSELALPHA.

   auto element_colour = [Self](Scintilla::Element Element, const RGB8 &Colour, bool Translucent) {
      if (Colour.Alpha) {
         auto alpha = Translucent ? Colour.Alpha : uint8_t(255);
         SCICALL(SCI_SETELEMENTCOLOUR, uptr_t(Element),
            sptr_t(Scintilla::Internal::ColourRGBA(Colour.Red, Colour.Green, Colour.Blue, alpha).AsInteger()));
      }
      else SCICALL(SCI_RESETELEMENTCOLOUR, uptr_t(Element));
   };

   using Scintilla::Element;
   for (auto element : { Element::SelectionText, Element::SelectionAdditionalText, Element::SelectionSecondaryText,
         Element::SelectionInactiveText, Element::SelectionInactiveAdditionalText }) {
      element_colour(element, Self->SelectFore, false);
   }

   // The layer of every selection is decided by selectionBackground: an opaque colour is drawn beneath the text, and a
   // translucent one is blended over it.

   const auto &bkgd = Self->SelectBkgd;
   SCICALL(SCI_SETSELALPHA, bkgd.Alpha ? scintilla_alpha(bkgd.Alpha) : uptr_t(SC_ALPHA_NOALPHA));
   element_colour(Element::SelectionBack, bkgd, true);
   element_colour(Element::SelectionSecondaryBack, bkgd, true);
   element_colour(Element::SelectionAdditionalBack, Self->SelectAdditionalBkgd, true);
   element_colour(Element::SelectionInactiveBack, Self->SelectInactiveBkgd, true);
   element_colour(Element::SelectionInactiveAdditionalBack, Self->SelectInactiveBkgd, true);
}

static void check_modified_widths(extScintilla *);
static void track_modified_lines(extScintilla *, int, int);
static void grow_scroll_width(extScintilla *, int, int);
static ERR key_event(objVectorViewport *, KQ, KEY, int);
static ERR modifier_event(const evKey *, APTR);
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

static ERR MODInit(OBJECTPTR, struct CoreBase *ArgCoreBase)
{
   CoreBase = ArgCoreBase;

   kt::Log log("scintilla");
   log.msg("Backend: v5; Scintilla 5.6.9; Lexilla 5.5.5");
   // Initialise the upstream catalogue during serial module loading, before editor creation can occur on threads.
   if (GetLexerCount() < 1) return ERR::InitModule;

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

static std::string_view lexer_name(const LexerProfile &Profile)
{
   if (Profile.ID IS SCLEX::TIRI) return "tiri";
   if (Profile.ID IS SCLEX::HTML) return "html";
   if (Profile.ID IS SCLEX::ASP) return "asp";
   return Profile.Factory;
}

static bool accepts_editor_drop(extScintilla *Self, std::string_view Types)
{
   if ((not Self->API) or ((Self->Flags & SCIF::DISABLED) != SCIF::NIL)) return false;
   for (auto type : Types) {
      if ((type IS char(DATA::FILE)) and Self->FileDrop.defined() and not Self->FileDrop.stale()) return true;
      if ((type IS char(DATA::TEXT)) and ((Self->Flags & SCIF::EDIT) != SCIF::NIL)) return true;
   }
   return false;
}

static ERR prepare_editor_drop(objVectorViewport *, objConfig *Snapshot, double X, double Y, APTR)
{
   auto Self = (extScintilla *)CurrentContext();
   auto pointer = gfx::AccessPointer();
   if (not pointer) return ERR::Cancelled;
   std::string_view types;
   pointer->getDragDatatypes(types);
   const bool accepted = accepts_editor_drop(Self, types);
   pointer->setDragFeedback(accepted ? 1 : 0);
   ReleaseObject(pointer);
   if (not accepted) return ERR::Cancelled;
   Snapshot->write("Scintilla", "Editor", std::to_string(Self->UID));
   Snapshot->write("Scintilla", "Position", std::to_string(Self->API->panDropPosition(
      (X - Self->CanvasX) * Self->Scale, (Y - Self->CanvasY) * Self->Scale)));
   Snapshot->write("Scintilla", "Revision", std::to_string(Self->DocumentRevision));
   return ERR::Okay;
}

static void notify_dragdrop(OBJECTPTR, ACTIONID, ERR Result, struct acDragDrop *Args)
{
   auto Self = (extScintilla *)CurrentContext();
   if ((not Args) or (not Args->Source) or (Result != ERR::Okay) or (not Self->API) or Self->Drop.active) return;
   auto pointer = gfx::AccessPointer();
   if (not pointer) return;
   pointer->setDragFeedback(0);
   const auto release = kt::Defer([&]() { ReleaseObject(pointer); });
   if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) return;

   double x = 0, y = 0;
   Self->Scene->getDropX(x);
   Self->Scene->getDropY(y);
   int position = Self->API->panDropPosition((x - Self->CanvasX) * Self->Scale, (y - Self->CanvasY) * Self->Scale);
   OBJECTID snapshot_id = 0;
   pointer->getDropSession(snapshot_id);
   if (snapshot_id) {
      kt::ScopedObjectLock<objConfig> snapshot(snapshot_id);
      int editor = 0;
      std::string revision;
      if ((not snapshot.granted()) or
          (snapshot->read("Scintilla", "Editor", editor) != ERR::Okay) or (editor != Self->UID) or
          (snapshot->read("Scintilla", "Position", position) != ERR::Okay) or
          (snapshot->read("Scintilla", "Revision", revision) != ERR::Okay) or
          (revision != std::to_string(Self->DocumentRevision))) return;
   }

   bool files = false, text = false;
   for (auto type : Args->Datatype) {
      if (type IS char(DATA::FILE)) files = Self->FileDrop.defined() and not Self->FileDrop.stale();
      if (type IS char(DATA::TEXT)) text = (Self->Flags & SCIF::EDIT) != SCIF::NIL;
   }
   if (not (files or text)) return;
   Self->Drop = { Args->Source->UID, Args->Item, position, Self->DocumentRevision, true };
   const auto editor_id = Self->UID;
   dcRequest request = {};
   request.Item = Args->Item;
   request.Preference[0] = char(files ? DATA::FILE : DATA::TEXT);
   request.Preference[1] = char(files and text ? DATA::TEXT : DATA::NIL);
   acDataFeed(Args->Source, Self, DATA::REQUEST,
      std::span<const int8_t>((const int8_t *)&request, sizeof(request)));
   // Sources answer synchronously.  An unanswered request cannot redirect a later receipt into this editor.
   if (CheckResourceExists(editor_id) IS ERR::True) Self->Drop.active = false;
}

//********************************************************************************************************************
// The editor follows the enabled state of its host viewport.

static void notify_disable_viewport(OBJECTPTR, ACTIONID, ERR Result, APTR)
{
   if (Result IS ERR::Okay) acDisable(CurrentContext());
}

static void notify_enable_viewport(OBJECTPTR, ACTIONID, ERR Result, APTR)
{
   if (Result IS ERR::Okay) acEnable(CurrentContext());
}

//********************************************************************************************************************
// Receiver for focus changes on the Focus viewport.  The editor has the focus while the Focus viewport or any of its
// children has the focus, which matches the delivery of keyboard input.

static ERR feedback_focus(objVectorViewport *, FM Event)
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

   // Clicking a code assistance pop-up gives it the focus, which input_popup() returns to the editor.  Scintilla
   // closes its pop-ups when it loses the focus, so the loss is not reported in the meantime.

   if ((not focus) and ((Self->Flags & SCIF::DISABLED) IS SCIF::NIL) and (popup_has_focus(Self))) return;

   if (not focus) {
      Self->KeyShift = false;
      Self->KeyCtrl  = false;
      Self->KeyAlt   = false;
   }

   Self->API->panSetFocus(focus);
}

//********************************************************************************************************************
// The host viewport is being freed, which takes the View, Page and Canvas with it.

static void notify_free_viewport(OBJECTPTR, ACTIONID, ERR, APTR)
{
   auto Self = (extScintilla *)CurrentContext();
   hide_context_menu(Self);
   Self->HasFocus = false;
   if (Self->API) {
      Self->API->panSetFocus(false);
      Self->API->panReleaseMouse();
      Self->API->panCancelPopups();
   }
   Self->Drop.active = false;
   Self->PrimaryDrop.active = false;
   Self->Viewport = nullptr;
   Self->Focus    = nullptr;
   Self->View     = nullptr;
   Self->Page     = nullptr;
   Self->Canvas   = nullptr;
}

//********************************************************************************************************************
// A Focus viewport that differs from the host Viewport is being freed.

static void notify_free_focus(OBJECTPTR, ACTIONID, ERR, APTR)
{
   auto Self = (extScintilla *)CurrentContext();
   Self->Focus    = nullptr;
   Self->HasFocus = false;
   update_focus(Self);
}

//********************************************************************************************************************
// The image definition belongs to the scene and will be freed with it.

static void notify_free_image(OBJECTPTR, ACTIONID, ERR, APTR)
{
   auto Self = (extScintilla *)CurrentContext();
   Self->Image = nullptr;
}

//********************************************************************************************************************
// Receiver for path changes on the View, including changes to the host's position, size and transforms.

static ERR feedback_view(objVectorViewport *, FM)
{
   auto Self = (extScintilla *)CurrentContext();

   if ((Self->API) and (Self->API->panActivePopup() != SCPOP::NONE)) queue_repaint(Self);

   update_view(Self);
   return ERR::Okay;
}

/*********************************************************************************************************************
** Size the client and the Bitmap to match the View at the device scale.  If the scale has changed, the fonts and
** metrics are reloaded, the pop-ups are closed because they were laid out at the former scale, and the editor is
** repainted in full.  The Dirty area is in the former client units, so it is replaced.
*/

static void update_view(extScintilla *Self)
{
   if (not Self->View) return;

   Unit w, h;
   Self->View->getWidth(w);
   Self->View->getHeight(h);
   update_host_transform(Self, double(w), double(h));

   const double old_scale = Self->Scale;
   Self->Scale = device_scale(Self, Self->ScalableFace);
   const bool rescaled = Self->Scale != old_scale;

   int width  = std::max(1, int(std::lround(double(w) * Self->Scale)));
   int height = std::max(1, int(std::lround(double(h) * Self->Scale)));

   if ((not rescaled) and (Self->ViewWidth IS width) and (Self->ViewHeight IS height)) {
      align_canvas(Self);
      return;
   }

   Self->ViewWidth  = width;
   Self->ViewHeight = height;

   // The Bitmap is one pixel wider and taller than the View to cover the sub-pixel alignment offset.  The Canvas maps
   // the Bitmap's device pixels to View units.

   if ((Self->Bitmap) and (resize_bitmap(Self, width + 1, height + 1) IS ERR::Okay)) {
      if (Self->Canvas) {
         Self->Canvas->setWidth(Self->Bitmap->Width / Self->Scale);
         Self->Canvas->setHeight(Self->Bitmap->Height / Self->Scale);
      }
   }

   if (rescaled) {
      Self->Dirty = { 0, 0, 0, 0 };
      Self->PointerX *= Self->Scale / old_scale;
      Self->PointerY *= Self->Scale / old_scale;

      APTR handle;
      const int size = std::max(1, scaled(Self, Self->FontSize));
      if (vec::GetFontHandle(Self->Face, Self->TextStyle, 0, size, &handle) IS ERR::Okay) {
         Self->FontHandle = handle;
      }

      if (Self->API) {
         Self->API->panCancelPopups();
         apply_metrics(Self);
         fonts_changed(Self);
         Self->API->panRescaled(Self->Scale / old_scale);
      }
   }
   else if (Self->API) Self->API->panResized();

   align_canvas(Self);
   invalidate_area(Self, 0, 0, Self->ViewWidth, Self->ViewHeight);
}

//********************************************************************************************************************
// Receiver for path changes on the Page.  If the Page has been moved by a third party, such as a scrollbar, the editor
// scrolls to match.  Scintilla scrolls by whole lines, so the Page is snapped to the nearest line.

static ERR feedback_page(objVectorViewport *Page, FM)
{
   auto Self = (extScintilla *)CurrentContext();

   if ((Self->ScrollLocked) or (not Self->API)) return ERR::Okay;

   Unit x, y;
   Page->getX(x);
   Page->getY(y);
   if ((-double(x) IS Self->ScrollX / Self->Scale) and (-double(y) IS Self->ScrollY / Self->Scale)) return ERR::Okay;

   Self->API->panScrollTo(-double(x) * Self->Scale, -double(y) * Self->Scale);
   return ERR::Okay;
}

//********************************************************************************************************************
// Pointer input is received by the Canvas because it is the top-most vector in the editor.  This also prevents wheel
// events from reaching a scrollbar's subscription to the Page, which would otherwise scroll the Page a second time.
//
// The Canvas reports positions in the coordinate system of the Page, which are converted to Scintilla's client
// coordinates by removing the Canvas position and applying the device scale.  While the left mouse button is held, the
// scene continues to deliver movement to the Canvas when the pointer is outside of it, which Scintilla uses to extend
// the selection and scroll.
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
// that apply to the editor (see kt_menu.cxx).  Opening on release ensures that the button is no longer held when the
// menu appears under the pointer.
//
// Each wheel event scrolls by a fixed step because wheel values are not consistent between platforms.  Holding Ctrl
// scrolls by a page.  Input events do not carry the modifier state, so the state that key_event() tracks is used.

static ERR input_canvas(objVectorRectangle *, const InputEvent *Events)
{
   static const int WHEEL_LINES = 3;
   static const int WHEEL_COLUMNS = 6;

   auto Self = (extScintilla *)CurrentContext();
   kt::ScopedObjectLock owner(Self);
   if (not owner.granted() or not Self->API or Self->collecting()) return ERR::Okay;

   for (auto event=Events; event and not Self->collecting(); event=event->Next) {
      if ((event->Flags & JTYPE::DRAG_ITEM) != JTYPE::NIL) {
         if (auto pointer = gfx::AccessPointer()) {
            std::string_view types;
            pointer->getDragDatatypes(types);
            if (not types.empty()) pointer->setDragFeedback(accepts_editor_drop(Self, types) ? 1 : 0);
            ReleaseObject(pointer);
         }
      }

      // The pointer position is recorded while disabled, because the scene does not repeat a movement event if the
      // pointer has not moved.

      if ((event->Flags & JTYPE::MOVEMENT) != JTYPE::NIL) {
         Self->PointerX = (event->X - (Self->ScrollX / Self->Scale + Self->CanvasX)) * Self->Scale;
         Self->PointerY = (event->Y - (Self->ScrollY / Self->Scale + Self->CanvasY)) * Self->Scale;
         Self->PointerStale = false;
      }
      else if ((event->Flags & JTYPE::CROSSING) != JTYPE::NIL) {
         // Enter events are not in the coordinate system of the Page, so they only mark the position as outdated.
         if (event->Type IS JET::CROSSED_IN) Self->PointerStale = true;
         continue;
      }
      else if (Self->PointerStale) {
         Self->PointerX = (event->X - (Self->ScrollX / Self->Scale + Self->CanvasX)) * Self->Scale;
         Self->PointerY = (event->Y - (Self->ScrollY / Self->Scale + Self->CanvasY)) * Self->Scale;
         Self->PointerStale = false;
      }

      if ((Self->Flags & SCIF::DISABLED) != SCIF::NIL) continue;

      if ((event->Flags & JTYPE::BUTTON) != JTYPE::NIL) {
         // Scintilla handles held buttons through TickFor(); a repeat is not a new selection or drag gesture.
         if ((event->Flags & JTYPE::REPEATED) != JTYPE::NIL) continue;
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

/*********************************************************************************************************************
** Scintilla's client coordinates are device pixels, so that text and graphics are painted at the resolution at which
** they are displayed.  The device scale is the number of device pixels per unit of the View.  It is derived from the
** corners of the View in the scene, and so includes every transform and view box that applies to the host.
**
** A uniformly scaled and unrotated View maps the Bitmap to whole device pixels.  If the View is scaled unequally,
** rotated or skewed, the larger of its axis scales is used so that the Bitmap is never magnified, and the Bitmap is
** resampled.  The scale is limited to MIN_SCALE and MAX_SCALE, beyond which the Bitmap is also resampled.  Changes of
** less than 0.001 are ignored so that the editor is not reconfigured by rounding errors.
*/

static constexpr double MIN_SCALE = 0.5;
static constexpr double MAX_SCALE = 4.0;

struct view_corners {
   double x[4], y[4];
   int count = 0;
};

static ERR trace_view_corner(objVector *, int, int, double X, double Y, APTR Meta)
{
   auto corners = (view_corners *)Meta;
   if (corners->count >= 4) return ERR::Terminate;
   corners->x[corners->count] = X;
   corners->y[corners->count] = Y;
   corners->count++;
   return ERR::Okay;
}

static void update_host_transform(extScintilla *Self, double Width, double Height)
{
   Self->HostScale = 1.0;
   Self->HostUniform = false;

   view_corners corners;
   if ((not Self->View) or (Width <= 0) or (Height <= 0)) return;
   if (Self->View->trace(C_FUNCTION(trace_view_corner, &corners), 1.0, true) != ERR::Okay) return;
   if (corners.count < 4) return;

   // Corners are traced from the origin, clockwise.  The first and last edges give the transformed axes.

   const double ax = (corners.x[1] - corners.x[0]) / Width, ay = (corners.y[1] - corners.y[0]) / Width;
   const double bx = (corners.x[3] - corners.x[0]) / Height, by = (corners.y[3] - corners.y[0]) / Height;
   const double scale_x = std::hypot(ax, ay), scale_y = std::hypot(bx, by);
   const double scale = std::max(scale_x, scale_y);
   if (not std::isfinite(scale) or (scale <= 0)) return;

   Self->HostX = corners.x[0];
   Self->HostY = corners.y[0];
   Self->HostScale = scale;
   Self->HostUniform = (std::abs(ay) < 1e-6) and (std::abs(bx) < 1e-6) and (ax > 0) and (by > 0) and
      (std::abs(scale_x - scale_y) < 1e-6);
}

//********************************************************************************************************************
// Return the device scale for the host, as used by a scalable face.  Bitmap faces have fixed sizes, so they are
// always painted at a scale of 1 and resampled.

static double device_scale(extScintilla *Self, bool Scalable)
{
   if (not Scalable) return 1.0;
   const double scale = std::clamp(Self->HostScale, MIN_SCALE, MAX_SCALE);
   if (std::abs(scale - Self->Scale) < 0.001) return Self->Scale;
   return scale;
}

//********************************************************************************************************************
// Image sampling blurs the text unless the Bitmap is mapped to whole device pixels.  If the View is aligned to the
// device pixel grid at the editor's scale, the Canvas is offset by the fractional part of the View's device position.
// Other hosts are left unaligned because the Bitmap is resampled regardless.

static void align_canvas(extScintilla *Self)
{
   if (not Self->Canvas) return;

   double ax = 0, ay = 0;
   if ((Self->HostUniform) and (std::abs(Self->HostScale - Self->Scale) < 0.001)) {
      ax = std::floor(Self->HostX + 0.001) - Self->HostX;
      ay = std::floor(Self->HostY + 0.001) - Self->HostY;
      ax = (ax > -0.001) ? 0 : ax / Self->Scale;
      ay = (ay > -0.001) ? 0 : ay / Self->Scale;
   }

   if ((ax != Self->CanvasX) or (ay != Self->CanvasY)) {
      Self->CanvasX = ax;
      Self->CanvasY = ay;
      sync_page(Self);
   }
}

//********************************************************************************************************************
// The Page is positioned at the negated scroll position, and the Canvas is positioned at the scroll position within
// the Page so that it always covers the View.  Scroll positions are converted from client pixels to View units.

// Reading a position can regenerate the scene graph.  That can run a client's feedback callback, in which the host
// viewport can be freed, so the reference is tested after each read.  Returns false if the vector no longer exists.

template <class T> static bool read_position(T * &Vector, Unit &X, Unit &Y)
{
   if (Vector) Vector->getX(X);
   if (Vector) Vector->getY(Y);
   return Vector != nullptr;
}

static void sync_page(extScintilla *Self)
{
   if ((not Self->Page) or (not Self->Canvas)) return;

   // Reading a position can also scroll the editor through feedback_page(), so the scroll position is read afterwards.

   Unit x, y;
   if (not read_position(Self->Page, x, y)) return;
   const double page_x = -Self->ScrollX / Self->Scale;
   const double page_y = -Self->ScrollY / Self->Scale;
   if (double(x) != page_x) Self->Page->setX(page_x);
   if ((Self->Page) and (double(y) != page_y)) Self->Page->setY(page_y);

   if (not read_position(Self->Canvas, x, y)) return;
   const double canvas_x = Self->ScrollX / Self->Scale + Self->CanvasX;
   const double canvas_y = Self->ScrollY / Self->Scale + Self->CanvasY;
   if (double(x) != canvas_x) Self->Canvas->setX(canvas_x);
   if ((Self->Canvas) and (double(y) != canvas_y)) Self->Canvas->setY(canvas_y);
}

//********************************************************************************************************************
// The Bitmap is presented through a VectorImage fill, so the scene cannot detect changes to its pixels.  Refreshing the
// Canvas marks it as dirty, which ensures that a buffered viewport containing the editor redraws its buffer.  Call this
// whenever the content of the Bitmap is modified.

static void bitmap_modified(extScintilla *Self)
{
   if (Self->Canvas) Self->Canvas->refresh();
}

//********************************************************************************************************************

static void clear_bitmap(extScintilla *Self)
{
   auto &bkgd = Self->BkgdColour;
   gfx::DrawRectangle(Self->Bitmap, 0, 0, Self->Bitmap->Width, Self->Bitmap->Height,
      Self->Bitmap->packPixel(bkgd.Red, bkgd.Green, bkgd.Blue, 255), BAF::FILL);
   bitmap_modified(Self);
}

// The extra row and column cover fractional Canvas offsets but lie outside Scintilla's client clip.  Retained
// pixels from a larger client or a scroll must not survive there, including before the deferred paint.

static void clear_bitmap_border(extScintilla *Self)
{
   auto bitmap = Self->Bitmap;
   const auto &background = Self->BkgdColour;
   const auto colour = bitmap->packPixel(background.Red, background.Green, background.Blue, 255);
   gfx::DrawRectangle(bitmap, Self->ViewWidth, 0, bitmap->Width - Self->ViewWidth, bitmap->Height, colour, BAF::FILL);
   gfx::DrawRectangle(bitmap, 0, Self->ViewHeight, bitmap->Width, bitmap->Height - Self->ViewHeight, colour, BAF::FILL);
}

//********************************************************************************************************************
// Shared validation for the public configuration API.  Reads remain safe in notification callbacks.

static ERR config_ready(extScintilla *Self, bool Mutating = false)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   if (Mutating and Self->API->panInNotification()) return ERR::Busy;
   return ERR::Okay;
}

static ERR indicator_range(extScintilla *Self, int Indicator, int Start, int End)
{
   if ((Indicator < 8) or (Indicator > 31)) return ERR::OutOfRange;
   if (not Self->Indicators.contains(Indicator)) return ERR::Args;
   if ((Start < 0) or (End < Start) or (End > SCICALL(SCI_GETLENGTH))) return ERR::OutOfRange;
   return ERR::Okay;
}

void ScintillaKTK::ApplyIndicators()
{
   for (const auto &[id, definition] : scintilla->Indicators) {
      int colour = scintilla->theme_reference(definition.colour);
      SendScintilla(SCI_INDICSETSTYLE, id, int(definition.style));
      SendScintilla(SCI_INDICSETFORE, id, ((colour & 255) << 16) | (colour & 0xff00) | ((colour >> 16) & 255));
      SendScintilla(SCI_INDICSETUNDER, id, 1);
      SendScintilla(SCI_INDICSETALPHA, id, 60);
      SendScintilla(SCI_INDICSETOUTLINEALPHA, id, 180);
   }
   InvalidateStyleRedraw();
}

// Markers are reapplied when the theme changes so that theme references follow the palette.  Scintilla's colours
// are in 0xBBGGRR order.

void ScintillaKTK::ApplyMarkers()
{
   auto bgr = [](int Colour) { return sptr_t(((Colour & 255) << 16) | (Colour & 0xff00) | ((Colour >> 16) & 255)); };
   for (const auto &[id, definition] : scintilla->Markers) {
      SendScintilla(SCI_MARKERDEFINE, id, definition.symbol);
      SendScintilla(SCI_MARKERSETFORE, id, bgr(scintilla->theme_reference(definition.fore)));
      SendScintilla(SCI_MARKERSETBACK, id, bgr(scintilla->theme_reference(definition.bkgd)));
      SendScintilla(SCI_MARKERSETALPHA, id, (definition.alpha < 255) ? sptr_t(definition.alpha) :
         sptr_t(SC_ALPHA_NOALPHA));
   }
}

static std::string lexer_string(extScintilla *Self, unsigned int Message, uptr_t Parameter = 0)
{
   auto length = SCICALL(Message, Parameter);
   std::string text(size_t(length) + 1, '\0');
   SCICALL(Message, Parameter, text.data());
   text.resize(size_t(length));
   return text;
}

static bool has_lexer_property(extScintilla *Self, const std::string &Name)
{
   if (Name.empty() or (Name.find('\0') != std::string::npos) or (Name.find('\n') != std::string::npos)) return false;
   if ((Name IS "fold") or (Name IS "asp.default.language")) return false;
   auto names = "\n" + lexer_string(Self, SCI_PROPERTYNAMES) + "\n";
   return names.find("\n" + Name + "\n") != std::string::npos;
}

static bool has_keyword_set(extScintilla *Self, int Set)
{
   auto sets = lexer_string(Self, SCI_DESCRIBEKEYWORDSETS);
   int count = sets.empty() ? 0 : int(std::count(sets.begin(), sets.end(), '\n')) + 1;
   if (not sets.empty() and sets.back() IS '\n') count--;
   return (Set >= 0) and (Set < count);
}

static ERR copy_config_text(const std::string &Text, std::span<int8_t> Buffer)
{
   if (Buffer.size() <= Text.size()) return ERR::BufferOverflow;
   if (not Buffer.data()) return ERR::NullArgs;
   std::copy(Text.begin(), Text.end(), Buffer.begin());
   Buffer[Text.size()] = 0;
   return ERR::Okay;
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
   clear_bitmap_border(Self);
   bitmap_modified(Self);
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

   queue_repaint(Self);
}

//********************************************************************************************************************
// Schedule a call to the Repaint method, which paints the Dirty region and updates the code assistance pop-ups.

static void queue_repaint(extScintilla *Self)
{
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

   SCICALL(SCI_SETMARGINWIDTHN, 0, (sptr_t)width);
}

//********************************************************************************************************************
// Apply the metrics that are defined in View units, which are converted to client pixels at the device scale.  The
// caret is limited to three pixels by Scintilla.

static void apply_metrics(extScintilla *Self)
{
   static const int SYMBOL_MARGIN = 20;
   static const int FOLD_MARGIN = 20;
   static const int CARET_WIDTH = 2;

   if (not Self->API) return;

   SCICALL(SCI_SETMARGINLEFT, 0, scaled(Self, Self->LeftMargin));

   // The initial RightMargin applies only to the caret policy; preserve a zero text margin.

   if (SCICALL(SCI_GETMARGINRIGHT) > 0) SCICALL(SCI_SETMARGINRIGHT, 0, scaled(Self, Self->RightMargin));

   SCICALL(SCI_SETXCARETPOLICY, CARET_SLOP|CARET_STRICT|CARET_EVEN, scaled(Self, Self->RightMargin));
   SCICALL(SCI_SETCARETWIDTH, std::max(1, scaled(Self, CARET_WIDTH)));
   SCICALL(SCI_SETMARGINWIDTHN, 1, Self->Symbols ? scaled(Self, SYMBOL_MARGIN) : 0L);
   SCICALL(SCI_SETMARGINWIDTHN, 2, Self->FoldingMarkers ? scaled(Self, FOLD_MARGIN) : 0L);
}

//********************************************************************************************************************

static void notify_write(OBJECTPTR, ACTIONID, ERR Result, struct acWrite *Args)
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

   // Changing undo collection resets the backend nesting.  Resume outstanding client groups.
   for (int depth = 0; depth < Self->ClientUndoDepth; depth++) SCICALL(SCI_BEGINUNDOACTION);

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

-METHOD-
BeginUndoAction: Begins a client undo group.

BeginUndoAction() starts a group of edits that #Undo() and #Redo() will treat as a single step.  Every edit made until
the matching call to #EndUndoAction() is added to the group, whether it originates from the client or the user.

Groups can be nested.  Each call must be balanced by a call to #EndUndoAction(), and the group is closed only when
the outermost call is ended.  Open groups are preserved when new content is loaded, and any groups that remain open
when the Scintilla object is freed are closed automatically.

-ERRORS-
Okay
NotInitialised
OutOfRange: The maximum nesting depth has been reached.
Busy: The editor is dispatching a notification and cannot be modified until it returns.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_BeginUndoAction(extScintilla *Self)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   if (Self->API->panInNotification()) return ERR::Busy;
   if (Self->ClientUndoDepth IS INT_MAX) return ERR::OutOfRange;
   SCICALL(SCI_BEGINUNDOACTION);
   Self->ClientUndoDepth++;
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Clear: Clears all content from the editor.
*********************************************************************************************************************/

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

The clipboard carries plain text without a rectangular marker, so the editor recognises a rectangular clip by
comparing the pasted text with the most recent rectangular copy in the process.  Consequently, identical text that is
later copied from another source is also pasted as a rectangle, and a rectangular copy from another process or
application is pasted as ordinary text.

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

/*********************************************************************************************************************

-METHOD-
ClearIndicator: Removes an application indicator from a byte range.

ClearIndicator() removes the marks of an indicator slot from the bytes in the range `Start` to `End`, leaving the
marks of other slots untouched.  Marks that extend beyond either end of the range are trimmed rather than removed, so
clearing the middle of a run leaves two separate runs.  To remove every mark in a slot, clear the range from zero to
#TextLength.

The slot must have been defined with #DefineIndicator().  Clearing a range does not modify the document, its
undo history, #DocumentRevision or the selection.

-INPUT-
int Indicator: An indicator slot previously defined with #DefineIndicator().
int Start: Byte offset of the first byte to clear.
int End: Byte offset immediately after the last byte to clear.  Must be between `Start` and #TextLength.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The editor is dispatching a notification and cannot be modified until it returns.
OutOfRange: `Indicator` is outside the range 8 to 31, or the byte range is invalid.
Args: `Indicator` has not been defined.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_ClearIndicator(extScintilla *Self, struct sci::ClearIndicator *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;
   if (auto error = indicator_range(Self, Args->Indicator, Args->Start, Args->End);
       error != ERR::Okay) return error;
   SCICALL(SCI_SETINDICATORCURRENT, Args->Indicator);
   SCICALL(SCI_INDICATORCLEARRANGE, Args->Start, Args->End - Args->Start);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
DefineIndicator: Defines the appearance of an application indicator slot.

Indicators annotate ranges of document text without altering the text or its lexical styling, and are typically used
to highlight diagnostics such as compiler errors, warnings and search matches.  DefineIndicator() configures the
appearance of a slot so that it can be applied to text with #FillIndicator() and #ClearIndicator().

Slots 8 to 31 are available to applications.  Lower slots are reserved for lexers and higher slots for input method
composition.  Indicators are drawn beneath the text, and each slot is drawn independently so that overlapping ranges
remain visible.  The available styles are:

!SCIND

The `Colour` is either an explicit colour in `0xRRGGBB` format or one of the following theme references, which follow
the palette when it is changed through #ThemeXML:

<list type="bullet">
<li>`-1`: The theme's `error` colour.</li>
<li>`-2`: The theme's `keyword` colour.</li>
<li>`-3`: The theme's `selectionBackground` colour.</li>
</list>

A slot can be redefined at any time.  Redefinition changes only the appearance; existing marks and their values are
preserved.

-INPUT-
int Indicator: The application slot to define, from 8 to 31.
int(SCIND) Style: The visual style of the indicator.
int Colour: An explicit `0xRRGGBB` colour, or a theme reference from `-1` to `-3`.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The editor is dispatching a notification and cannot be modified until it returns.
OutOfRange: `Indicator` is outside the range 8 to 31, or `Colour` is neither a valid colour nor a theme reference.
Args: `Style` is not a supported `SCIND` value.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_DefineIndicator(extScintilla *Self, struct sci::DefineIndicator *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;
   if ((Args->Indicator < 8) or (Args->Indicator > 31) or
       (Args->Colour < -3) or (Args->Colour > 0xffffff)) return ERR::OutOfRange;
   switch (Args->Style) {
      case SCIND::PLAIN: case SCIND::SQUIGGLE: case SCIND::BOX: case SCIND::ROUND_BOX:
      case SCIND::STRAIGHT_BOX: case SCIND::DASH: case SCIND::DOTS: break;
      default: return ERR::Args;
   }
   Self->Indicators[Args->Indicator] = { Args->Style, Args->Colour };
   Self->API->ApplyIndicators();
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
Colourise: Restyles a range of the document with the active lexer.

Colourise() runs the active #Lexer over the range `Start` to `End` immediately.  The lexer may begin styling from an
earlier position if it needs preceding context to resolve the range correctly.

Calls to #SetKeywords() and #SetLexerProperty() invalidate the existing styles, but the affected text is not restyled
until it is next drawn.  Call Colourise() after such changes when the updated styles are needed straight away.  To
restyle the entire document, use a range of zero to #TextLength.  An empty range is accepted and has no effect.

Styling does not modify the document, its undo history or #DocumentRevision.

-INPUT-
int Start: Byte offset at which to start styling.
int End: Byte offset at which to stop styling.  Must not be less than `Start` or greater than #TextLength.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The editor is dispatching a notification and cannot be modified until it returns.
OutOfRange: The byte range is invalid.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_Colourise(extScintilla *Self, struct sci::Colourise *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;
   if ((Args->Start < 0) or (Args->End < Args->Start) or
       (Args->End > SCICALL(SCI_GETLENGTH))) return ERR::OutOfRange;
   if (Args->Start != Args->End) SCICALL(SCI_COLOURISE, Args->Start, Args->End);
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR receive_drop(extScintilla *, struct acDataFeed *);

static ERR SCINTILLA_DataFeed(extScintilla *Self, struct acDataFeed *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   if (not Self->API) return ERR::NotInitialised;

   if (Args->Datatype IS DATA::TEXT) {
      if (Self->API->panCheckLength(Args->Buffer.size()) != ERR::Okay) return ERR::OutOfRange;
      // Incoming text is appended to the end of the document
      SCICALL(SCI_APPENDTEXT, Args->Buffer.size(), (const char *)Args->Buffer.data());
   }
   else if (Args->Datatype IS DATA::RECEIPT) {
      return receive_drop(Self, Args);
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
The `SCMK::BACKGROUND` and `SCMK::UNDERLINE` symbols are drawn in the text area instead, in the `Bkgd` colour.  If
`Alpha` is less than 255, these two symbols are blended over the line, which also tints its text.  Otherwise they are
drawn opaquely beneath the text.  `Alpha` is ignored for all other symbols.

Each colour is either an explicit colour in `0xRRGGBB` format or one of the following theme references, which follow
the palette when it is changed through #ThemeXML:

<list type="bullet">
<li>`-1`: The theme's `error` colour.</li>
<li>`-2`: The theme's `keyword` colour.</li>
<li>`-3`: The theme's `selectionBackground` colour.</li>
</list>

A marker can be redefined at any time.  Each call replaces the complete definition, and lines that carry the marker
retain it.

-INPUT-
int Marker: The marker number, from 0 to 24.
int(SCMK) Symbol: The symbol to draw.
int Fore: The foreground colour: an explicit `0xRRGGBB` colour, or a theme reference from `-1` to `-3`.
int Bkgd: The background colour: an explicit `0xRRGGBB` colour, or a theme reference from `-1` to `-3`.
int Alpha: The opacity of `Bkgd` for the `SCMK::BACKGROUND` and `SCMK::UNDERLINE` symbols, from 0 to 255.

-RESULT-
Okay:
NullArgs:
OutOfRange: The `Marker`, a colour or `Alpha` is out of range.
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

   for (auto colour : { Args->Fore, Args->Bkgd }) {
      if ((colour < -3) or (colour > 0xffffff)) return log.warning(ERR::OutOfRange);
   }
   if ((Args->Alpha < 0) or (Args->Alpha > 255)) return log.warning(ERR::OutOfRange);

   Self->Markers[Args->Marker] = { symbol, Args->Fore, Args->Bkgd, Args->Alpha };
   Self->API->ApplyMarkers();
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
   Self->Drop.active = false;
   Self->PrimaryDrop.active = false;
   if ((Self->Flags & SCIF::DISABLED) IS SCIF::NIL) {
      Self->Flags |= SCIF::DISABLED;
      hide_context_menu(Self);
      if (Self->API) {
         Self->API->panReleaseMouse(); // Button releases are ignored while disabled
         Self->API->panCancelPopups();
      }
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

static ERR SCINTILLA_Draw(extScintilla *Self, struct acDraw *)
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

-METHOD-
FillIndicator: Applies an application indicator to a byte range.

FillIndicator() marks the bytes in the range `Start` to `End` with an indicator slot and associates `Value` with every
marked byte.  The value is not interpreted by the editor; applications can use it to link a mark to their own data,
such as an index into a list of diagnostic messages, and retrieve it later with #GetIndicator().

Filling a range overwrites any existing marks of the same slot within that range.  Marks in other slots are
unaffected, so a byte can carry one value per slot.  Marks move with the text when the document is edited, and are
trimmed when the text they cover is deleted.  Marks are not part of the undo history, so undoing a deletion does not
restore them.  Offsets are not adjusted to UTF-8 character boundaries.

The slot must have been defined with #DefineIndicator().  Filling a range does not modify the document, its
undo history, #DocumentRevision or the selection.

-INPUT-
int Indicator: An indicator slot previously defined with #DefineIndicator().
int Start: Byte offset of the first byte to mark.
int End: Byte offset immediately after the last byte to mark.  Must be between `Start` and #TextLength.
int Value: A positive value to associate with the marked bytes.  Zero is reserved for unmarked bytes.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The editor is dispatching a notification and cannot be modified until it returns.
OutOfRange: `Indicator` is outside the range 8 to 31, or the byte range is invalid.
Args: `Indicator` has not been defined, or `Value` is not positive.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_FillIndicator(extScintilla *Self, struct sci::FillIndicator *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;
   if (auto error = indicator_range(Self, Args->Indicator, Args->Start, Args->End);
       error != ERR::Okay) return error;
   if (Args->Value <= 0) return ERR::Args;
   SCICALL(SCI_SETINDICATORCURRENT, Args->Indicator);
   SCICALL(SCI_SETINDICATORVALUE, Args->Value);
   SCICALL(SCI_INDICATORFILLRANGE, Args->Start, Args->End - Args->Start);
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
GetIndicator: Reads the indicator value at a byte position and the extent of its run.

GetIndicator() returns the value that #FillIndicator() associated with the byte at `Position`, together with the
bounds of the contiguous run of bytes that share that value.  If the byte is not marked, `Value` is zero and `Start`
and `End` describe the surrounding run of unmarked bytes.

`Position` may equal #TextLength, in which case `Value` is zero and both `Start` and `End` are set to `Position`.

This method is safe to call from notification callbacks.

-INPUT-
int Indicator: An indicator slot previously defined with #DefineIndicator().
int Position: Byte offset to inspect, from zero to #TextLength.
&int Value: Receives the value at `Position`, or zero if the byte is not marked.
&int Start: Receives the byte offset of the first byte in the run.
&int End: Receives the byte offset immediately after the last byte in the run.

-ERRORS-
Okay
NullArgs
NotInitialised
OutOfRange: `Indicator` is outside the range 8 to 31, or `Position` is outside the document.
Args: `Indicator` has not been defined.

-TAGS-
pure-query

*********************************************************************************************************************/

static ERR SCINTILLA_GetIndicator(extScintilla *Self, struct sci::GetIndicator *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self); error != ERR::Okay) return error;
   if (auto error = indicator_range(Self, Args->Indicator, Args->Position, Args->Position);
       error != ERR::Okay) return error;
   Args->Value = 0;
   Args->Start = Args->End = Args->Position;
   if (Args->Position IS SCICALL(SCI_GETLENGTH)) return ERR::Okay;
   Args->Value = int(SCICALL(SCI_INDICATORVALUEAT, Args->Indicator, Args->Position));
   Args->Start = int(SCICALL(SCI_INDICATORSTART, Args->Indicator, Args->Position));
   Args->End = int(SCICALL(SCI_INDICATOREND, Args->Indicator, Args->Position));
   // Scintilla discards empty decorations and returns zero boundaries for an absent slot.
   if ((Args->Value IS 0) and (Args->End IS 0)) {
      Args->Start = 0;
      Args->End = int(SCICALL(SCI_GETLENGTH));
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetKeywords: Copies a keyword set of the active lexer to a buffer.

GetKeywords() copies the keyword list that the active #Lexer is currently using for the keyword set `Set`.  The result
is the list assigned with #SetKeywords() if one exists for the active lexer, otherwise it is the lexer's built-in
default list, which may be empty.

The number of keyword sets and their meaning depend on the lexer.  The buffer must have room for the keyword list and
a terminating null byte.  If it is too small, the buffer is left unchanged.

-INPUT-
int Set: Zero-based index of the keyword set to read.
^array(char) Buffer: Receives the keyword list as a null-terminated string.

-ERRORS-
Okay
NullArgs
NotInitialised
NoSupport: The active lexer does not provide keyword set `Set`.
BufferOverflow: `Buffer` is too small for the keyword list and its terminator.

-TAGS-
mutates-input

*********************************************************************************************************************/

static ERR SCINTILLA_GetKeywords(extScintilla *Self, struct sci::GetKeywords *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self); error != ERR::Okay) return error;
   if (not has_keyword_set(Self, Args->Set)) return ERR::NoSupport;
   return copy_config_text(Self->ActiveKeywords[Args->Set], Args->Buffer);
}

/*********************************************************************************************************************

-METHOD-
GetLexerProperty: Copies a lexer property assigned by the client to a buffer.

GetLexerProperty() copies the value that #SetLexerProperty() assigned to the property `Name` for the active #Lexer.  If
the property is supported but has not been assigned for this lexer, an empty string is returned; the lexer's internal
default value is not reported.

The property must be one advertised by the active lexer.  The `fold` and `asp.default.language` properties are managed
by the Scintilla class and cannot be read with this method.  The buffer must have room for the value and a terminating
null byte.  If it is too small, the buffer is left unchanged.

-INPUT-
strview Name: The exact name of the property, as advertised by the active lexer.
^array(char) Buffer: Receives the property value as a null-terminated string.

-ERRORS-
Okay
NullArgs
NotInitialised
NoSupport: `Name` is not a property advertised by the active lexer, or is reserved.
Args: `Name` contains a null byte.
BufferOverflow: `Buffer` is too small for the value and its terminator.

-TAGS-
mutates-input

*********************************************************************************************************************/

static ERR SCINTILLA_GetLexerProperty(extScintilla *Self, struct sci::GetLexerProperty *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self); error != ERR::Okay) return error;
   std::string name(Args->Name);
   if ((name.find('\0') != std::string::npos)) return ERR::Args;
   if (not has_lexer_property(Self, name)) return ERR::NoSupport;
   auto &properties = Self->LexerSettings[Self->Lexer].properties;
   auto entry = properties.find(name);
   return copy_config_text(entry IS properties.end() ? std::string() : entry->second, Args->Buffer);
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
   int len;
   if (not KotukuScintilla::PublicInteger(SCICALL(SCI_LINELENGTH, Args->Line), len)) return ERR::OutOfRange;
   if (std::ssize(Args->Buffer) > len) {
      SCICALL(SCI_GETLINE, Args->Line, (const char *)Args->Buffer.data());
      Args->Buffer[len] = 0;
      return ERR::Okay;
   }
   else return ERR::BufferOverflow;
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
GetTextRange: Copies an exact byte range to a caller-supplied buffer.

GetTextRange() copies the document bytes from `Start` up to, but not including, `End` into `Buffer` and appends a
terminating null byte.  Offsets are byte positions, so a range that splits a multi-byte UTF-8 character is copied
as-is.  Embedded null bytes in the document are copied verbatim, so use `End - Start` rather than the null terminator
to determine the length of the result.

Unlike #GetLine(), the range is not limited to a single line and line-end characters are copied without
modification.  The selection and cursor position are not affected.

-INPUT-
int Start: Inclusive byte offset, between zero and #TextLength.
int End: Exclusive byte offset, between `Start` and #TextLength.  Negative sentinels such as `-1` are not supported.
^array(char) Buffer: Destination with at least `End - Start + 1` bytes of capacity, allowing for the null terminator.

-ERRORS-
Okay
NullArgs
NotInitialised
OutOfRange: `Start` is negative, `End` is less than `Start`, or `End` exceeds #TextLength.
BufferOverflow: `Buffer` is smaller than `End - Start + 1` bytes.

-TAGS-
mutates-input

*********************************************************************************************************************/

static ERR SCINTILLA_GetTextRange(extScintilla *Self, struct sci::GetTextRange *Args)
{
   if (not Args) return ERR::NullArgs;
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   if ((Args->Start < 0) or (Args->End < Args->Start) or
       (Args->End > SCICALL(SCI_GETLENGTH))) return ERR::OutOfRange;

   const size_t length = size_t(Args->End - Args->Start);
   if (Args->Buffer.size() <= length) return ERR::BufferOverflow;
   if (not Args->Buffer.data()) return ERR::NullArgs;

   Scintilla::TextRangeFull range { { Args->Start, Args->End }, (char *)Args->Buffer.data() };
   SCICALL(SCI_GETTEXTRANGEFULL, 0, sptr_t(&range));
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
EndUndoAction: Ends the innermost client undo group.

EndUndoAction() closes the group most recently opened by #BeginUndoAction().  When the outermost group is closed,
the edits made within it are committed to the undo history as a single step.

-ERRORS-
Okay
NotInitialised
Args: There is no open undo group to end.
Busy: The editor is dispatching a notification and cannot be modified until it returns.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_EndUndoAction(extScintilla *Self)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   if (Self->API->panInNotification()) return ERR::Busy;
   if (not Self->ClientUndoDepth) return ERR::Args;
   SCICALL(SCI_ENDUNDOACTION);
   Self->ClientUndoDepth--;
   return ERR::Okay;
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

   if (not KotukuScintilla::PublicInteger(SCICALL(SCI_FINDCOLUMN, Args->Line, Args->Column), Args->Pos))
      return ERR::OutOfRange;
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

/*********************************************************************************************************************

-METHOD-
HidePopups: Closes the completion list and the call tip.

HidePopups() closes any completion list or call tip that is open.  Closing a completion list is reported to the
#CompletionCallback as `SCAC::CANCELLED`.

-RESULT-
Okay:

*********************************************************************************************************************/

static ERR SCINTILLA_HidePopups(extScintilla *Self)
{
   if (Self->API) Self->API->panCancelPopups();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR SCINTILLA_Hide(extScintilla *Self)
{
   hide_context_menu(Self);
   if (Self->API) Self->API->panCancelPopups();
   if (Self->View) return acHide(Self->View);
   else return ERR::Okay;
}

//********************************************************************************************************************

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

   // The canvas determines the device scale at which the fonts are loaded.  Only scalable faces follow the scale.

   Self->ScalableFace = face_scalable(Self->Face);
   if (auto error = create_canvas(Self); error != ERR::Okay) return error;

   if (auto error = resolve_fonts(Self, Self->TextStyle, Self->Face, Self->FontSize); error != ERR::Okay) return error;

   if (Self->Scene->SurfaceID) { // Make UI subscriptions as long as we're not headless
      // Keyboard input is delivered while the Focus viewport or one of its children has the focus.

      Self->Focus->subscribeFeedback(FM::HAS_FOCUS|FM::CHILD_HAS_FOCUS|FM::LOST_FOCUS, C_FUNCTION(feedback_focus));
      Self->Focus->subscribeKeyboard(C_FUNCTION(key_event));

      // Pointer input is received by the Canvas.  The View is unsuitable because input is delivered to the top-most
      // subscriber, and a scrollbar subscribes to the Page.

      Self->Canvas->subscribeInput(JTYPE::BUTTON|JTYPE::MOVEMENT|JTYPE::EXT_MOVEMENT|JTYPE::CROSSING,
         C_FUNCTION(input_canvas));

      // Register the visible client, rather than the scrolling Page or the application's host viewport.
      Self->View->setDropTarget(true);
      Self->View->setDropPrepare(C_FUNCTION(prepare_editor_drop));
      SubscribeAction(Self->View, AC::DragDrop, C_FUNCTION(notify_dragdrop));
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

   // Modifier keys are tracked from all keyboard events, because they can be pressed while another vector has the
   // focus and still apply to a subsequent click.

   auto modifier_callback = C_FUNCTION(modifier_event);
   gfx::SubscribeKeyboard(0, 0, KBF::OBSERVER, &modifier_callback, &Self->KeyHandle);

   // Load a text file if required

   if (not Self->Path.empty()) {
      if (load_file(Self, Self->Path) != ERR::Okay) {
         return ERR::File;
      }
   }
   else calc_longest_line(Self);

   if (auto error = Self->API->SetLexer(Self->Lexer); error != ERR::Okay) return error;

   if (Self->LongestWidth >= scaled(Self, 60)) SCICALL(SCI_SETSCROLLWIDTH, Self->LongestWidth);
   else SCICALL(SCI_SETSCROLLWIDTH, 1UL);

   if ((Self->Flags & SCIF::EXT_PAGE) != SCIF::NIL) {
      log.msg("Extended page mode.");
      SCICALL(SCI_SETENDATLASTLINE, 0UL); // Allow scrolling by an extra page at the end of the document
   }
   else SCICALL(SCI_SETENDATLASTLINE, 1UL);

   SCICALL(SCI_SETMARGINRIGHT, 0, 0L);

   SCICALL(SCI_SETTABWIDTH, Self->TabWidth);
   SCICALL(SCI_SETINDENT, Self->IndentWidth);
   SCICALL(SCI_SETINDENTATIONGUIDES, int(Self->IndentationGuides));

   // Caret, current line and selection colours

   apply_theme_elements(Self);

   // Show whitespace characters like tabs

   if (Self->ShowWhitespace) SCICALL(SCI_SETVIEWWS, (uptr_t)SCWS_VISIBLEALWAYS);
   else SCICALL(SCI_SETVIEWWS, (uptr_t)SCWS_INVISIBLE);

   // Turn off buffered drawing because Kotuku surfaces already include buffer support

   SCICALL(SCI_SETBUFFEREDDRAW, 0UL);

   // Caret visibility policy

   SCICALL(SCI_SETYCARETPOLICY, CARET_SLOP|CARET_STRICT|CARET_EVEN, 3);

   // Caret visibility policy (folding margins)

   SCICALL(SCI_SETVISIBLEPOLICY, VISIBLE_STRICT|VISIBLE_SLOP, 4);

   // Miscellaneous options

   SCICALL(SCI_SETEOLMODE, int(Self->EOLMode));

   if (Self->AllowTabs) SCICALL(SCI_SETUSETABS, 1UL);
   else  SCICALL(SCI_SETUSETABS, 0UL);

   // Set all special margins to invisible (note that the values indicate the pixel width of the margin)

   SCICALL(SCI_SETMARGINWIDTHN, 0, 0L);
   update_margin_width(Self);

   // The text margins, the caret policy and width, and the symbol and folding margins scale with the display.

   apply_metrics(Self);

   SCICALL(SCI_SETMARGINSENSITIVEN, 1, (sptr_t)Self->MarginClick.defined());
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

Specify the zero-based UTF-8 byte position at which String should be inserted.  Two special positions are supported:
a value of -1 inserts the text at the current cursor position and a
value of -2 replaces currently selected text.

-INPUT-
strview String: A text string to add.
int Pos: Byte offset; -1 inserts at the caret and -2 replaces the selection.

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
   const auto removed = Args->Pos IS -2 ?
      SCICALL(SCI_GETSELECTIONEND) - SCICALL(SCI_GETSELECTIONSTART) : 0;
   if (Self->API->panCheckLength(Args->String.size(), removed) != ERR::Okay) return ERR::OutOfRange;

   log.branch("Pos: %d, Text: %.*s", Args->Pos, int(std::min(Args->String.size(), size_t(10))), Args->String.data());

   const std::string text(Args->String);
   pos = Args->Pos;
   if (pos IS -1) {
      // Get the current cursor position
      pos = SCICALL(SCI_GETCURRENTPOS);
   }
   else if (pos IS -2) {
      // Replace currently selected text

      SCICALL(SCI_BEGINUNDOACTION);
      SCICALL(SCI_REPLACESEL, 0UL, text.c_str());
      SCICALL(SCI_ENDUNDOACTION);
      return ERR::Okay;
   }
   else if (pos < -1) {
      return log.warning(ERR::OutOfRange);
   }

   SCICALL(SCI_BEGINUNDOACTION);
   SCICALL(SCI_INSERTTEXT, pos, text.c_str());
   SCICALL(SCI_ENDUNDOACTION);
   return ERR::Okay;
}

/*********************************************************************************************************************
-ACTION-
Redo: Redo the most recently undone activity.

*********************************************************************************************************************/

static ERR SCINTILLA_Redo(extScintilla *Self, struct acRedo *)
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
   else if (Args->Marker < 0) SCICALL(SCI_MARKERDELETE, Args->Line, Args->Marker);
   else {
      const auto mask = uptr_t(1) << Args->Marker;
      while (SCICALL(SCI_MARKERGET, Args->Line) & mask) {
         SCICALL(SCI_MARKERDELETE, Args->Line, Args->Marker);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ReplaceLine: Replaces a line with new text content.

This method will replace an entire line with a new text string.  If the new string contains line feeds, then multiple
lines will be inserted at the target position.  It is possible to limit the number of bytes inserted from the
source string by setting the Length parameter.  To insert all bytes from the source string, set a Length of -1.

-INPUT-
int Line: Index of the line being targeted.
strview String: The new string that will replace the line.
int Length: The number of bytes to replace the target with, or -1 for the entire source string.

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
   if (Self->API->panCheckLength(length, end - start) != ERR::Okay) return ERR::OutOfRange;
   SCICALL(SCI_REPLACETARGET, (uptr_t)length, Args->String.data());

   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ReplaceRange: Replaces an exact byte range without searching or assigning a selection.

ReplaceRange() replaces the document bytes from `Start` up to, but not including, `End` with `Text`.  Setting `Start`
and `End` to the same offset inserts `Text` at that position, and an empty `Text` deletes the range.  A call with an
empty range and an empty `Text` succeeds without modifying the document.

The replacement is recorded as a single undoable edit, increments #DocumentRevision and is reported to
#ChangeCallback like any other edit.  Use #BeginUndoAction() and #EndUndoAction() to combine several replacements into
one undo step.  The current selection is not replaced by `Text`, although selection and cursor positions after the
range are shifted to account for the change in length.

This method is intended for programmatic edits and is not restricted by the `SCIF::EDIT` flag.

-INPUT-
int Start: Inclusive byte offset, between zero and #TextLength.
int End: Exclusive byte offset, between `Start` and #TextLength.  Negative sentinels such as `-1` are not supported.
strview Text: Replacement bytes.  An empty string deletes the range.  Embedded null bytes are supported.

-ERRORS-
Okay
NullArgs
NotInitialised
OutOfRange: The range is invalid, or the replacement would exceed the maximum document length.
ReadOnly: The document is temporarily read-only.
Busy: The editor is dispatching a notification and cannot be modified until it returns.

-TAGS-
copies-input, mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_ReplaceRange(extScintilla *Self, struct sci::ReplaceRange *Args)
{
   if (not Args) return ERR::NullArgs;
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   return Self->API->panReplaceRange(Args->Start, Args->End, Args->Text);
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

   if ((Args->Find.size() > size_t(KotukuScintilla::document_limit)) or
       (Args->Replace.size() > size_t(KotukuScintilla::document_limit))) return ERR::OutOfRange;
   const int findlen = int(Args->Find.size());

   int flags = (((Args->Flags & STF::CASE) != STF::NIL) ? SCFIND_MATCHCASE : 0) |
                (((Args->Flags & STF::EXPRESSION) != STF::NIL) ? SCFIND_REGEXP : 0);

   SCICALL(SCI_SETSEARCHFLAGS, flags);

   SCICALL(SCI_BEGINUNDOACTION);

   // Each search resumes from the end of the previous replacement, so that a replacement containing the keyword
   // is not matched again.  The end position is adjusted by the difference between each match and its replacement.

   int total = 0;
   ERR result = ERR::Okay;
   while (start <= end) {
      log.trace("Search between %d - %d", start, end);

      SCICALL(SCI_SETTARGETSTART, start);
      SCICALL(SCI_SETTARGETEND, end);

      SCICALL(SCI_SETSTATUS, uptr_t(SC_STATUS_OK));
      int pos;
      if (not KotukuScintilla::PublicInteger(SCICALL(SCI_SEARCHINTARGET, findlen, Args->Find.data()), pos)) {
         result = ERR::OutOfRange;
         break;
      }
      if (SCICALL(SCI_GETSTATUS) IS SC_STATUS_WARN_REGEX) { result = ERR::Syntax; break; }
      if (pos IS -1) break;

      log.trace("Found keyword at %d", pos);

      int match_len = SCICALL(SCI_GETTARGETEND) - pos;

      sptr_t len;
      if ((Args->Flags & STF::EXPRESSION) != STF::NIL) {
         len = SCICALL(SCI_REPLACETARGETRE, (uptr_t)Args->Replace.size(), Args->Replace.data());
      }
      else len = SCICALL(SCI_REPLACETARGET, (uptr_t)Args->Replace.size(), Args->Replace.data());

      if (len < 0) { result = ERR::OutOfRange; break; }
      int next_end, next_start;
      if (not KotukuScintilla::PublicInteger(sptr_t(end) + len - match_len, next_end) or
          not KotukuScintilla::PublicInteger(sptr_t(pos) + len, next_start)) {
         result = ERR::OutOfRange;
         break;
      }
      end = next_end;
      start = next_start;
      total++;
      if (not match_len) {
         if (start >= end) break;
         start = int(SCICALL(SCI_POSITIONAFTER, start));
      }
   }

   SCICALL(SCI_ENDUNDOACTION);

   if (result != ERR::Okay) return result;
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
   if (Self->API and not Self->API->panFlushChangeReports()) return ERR::Okay;
   if (Self->API and not Self->API->panFlushCompletionReports()) return ERR::Okay;

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
   update_popups(Self);
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

   int len;
   if (not KotukuScintilla::PublicInteger(SCICALL(SCI_GETLENGTH), len)) return ERR::OutOfRange;

   log.branch("To: %d, Size: %d", Args->Dest->UID, len);

   std::vector<char> buffer(len+1);
   SCICALL(SCI_GETTEXT, buffer.size(), (const char *)buffer.data());
   return acWrite(Args->Dest, std::span<const int8_t>((const int8_t *)buffer.data(), size_t(len)));
}

/*********************************************************************************************************************

-METHOD-
ScrollToPoint: Scrolls the document to a position in the units of the View.

This method scrolls the document so that the point (`X`, `Y`), measured in the units of the #View from the top-left
corner of the document, appears at the top-left corner of the #View.  The #Page is moved to `(-X, -Y)` to reflect the
change.

The editor scrolls vertically by whole lines, so `Y` is rounded to the nearest line.  Both coordinates are clamped to
the scrollable range of the document.

-INPUT-
int X: Horizontal scroll position in the units of the View.
int Y: Vertical scroll position in the units of the View.

-RESULT-
Okay:
NullArgs:

*********************************************************************************************************************/

static ERR SCINTILLA_ScrollToPoint(extScintilla *Self, struct sci::ScrollToPoint *Args)
{
   kt::Log log;

   if (!Args) return log.warning(ERR::NullArgs);

   log.traceBranch("%dx%d", Args->X, Args->Y);

   Self->API->panScrollTo(Args->X * Self->Scale, Args->Y * Self->Scale);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SelectRange: Selects a range of text, can also deselect all text.

Selects a half-open range using zero-based UTF-8 byte offsets.  Start is the anchor and End is the caret, so reversed
bounds preserve selection direction.  Supplying no arguments or two zeroes collapses the selection at the current caret.
Existing backend clamping and negative sentinels are preserved.

-INPUT-
int Start: The anchor byte offset.
int End: The caret byte offset (exclusive for forward selections).  A negative value targets the document end.

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
ShowCallTip: Displays a call tip, such as a function signature, below a position in the document.

ShowCallTip() opens a call tip that is attached to `Pos`.  It is typically used to show the parameters of a function
while the user types its arguments.  Calling ShowCallTip() while a call tip is open replaces it, which is also how the
highlighted parameter is updated.  Showing a call tip closes the completion list.

The call tip is displayed on the line below `Pos`, or above it if it would otherwise extend beyond the bottom of the
scene.  The `Text` may contain multiple lines that are separated by `\n`.  The characters `\001` and `\002` are
drawn as up and down arrows, which a client can use to let the user cycle through overloads by clicking them.  Clicks
are reported to the #CompletionCallback as `SCAC::CALL_TIP_CLICK`.

Typing text, moving the cursor left and right, and deleting with Backspace keep the call tip open.  It is closed by
other cursor movement, Escape, a click in the editor, the loss of the focus, Backspace beyond the cursor position at
which it opened, or #HidePopups().

-INPUT-
int Pos: The document position that the call tip refers to, or -1 for the cursor position.
cstr Text: The text to display.
int HighlightStart: The byte offset within `Text` at which highlighting starts.
int HighlightEnd: The byte offset within `Text` at which highlighting ends.  Highlighting is disabled if this does not exceed `HighlightStart`.

-RESULT-
Okay:
NullArgs: The `Text` is empty.
OutOfRange: The `Pos` is outside of the document.
NotPossible: The editor is disabled, or its host viewport has been freed.

*********************************************************************************************************************/

static ERR SCINTILLA_ShowCallTip(extScintilla *Self, struct sci::ShowCallTip *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Text) or (not Args->Text[0])) return log.warning(ERR::NullArgs);

   log.branch("Pos: %d, Highlight: %d-%d", Args->Pos, Args->HighlightStart, Args->HighlightEnd);
   return Self->API->panShowCallTip(Args->Pos, Args->Text, Args->HighlightStart, Args->HighlightEnd);
}

/*********************************************************************************************************************

-METHOD-
ShowCompletions: Displays a list of completion candidates for the word at the cursor.

ShowCompletions() opens the completion list below the word that is being typed, or above it if there is more room
there.  The `Candidates` are separated by line feeds and may be given in any order.  They are sorted for display and
duplicates are removed.  The client normally calls ShowCompletions() from the #CompletionCallback in response to
`SCAC::REQUEST` or `SCAC::CHAR_ADDED`.

`Length` is the number of bytes of the word that precede the cursor and have already been typed.  The candidate that
begins with this prefix is selected, and the selection follows further typing.  If no candidate matches the typed
prefix, the list closes.  Accepting a candidate replaces the prefix with it.  Showing the list closes any call tip.

While the list is open, the Up and Down arrow keys, Page Up, Page Down, Home and End move the selection.  Enter and
Tab accept the selected candidate, and Escape closes the list.  Tab reaches the editor only if the scene does not use it
to cycle the focus, i.e. the scene has the `VPF::IGNORE_TAB` flag.  Other cursor movement, a click in the editor and the
loss of the focus also close it.  Clicking a candidate selects it, double-clicking accepts it, and the mouse wheel
scrolls the list.  Acceptance is reported to the #CompletionCallback as `SCAC::SELECTED` and any other closure as
`SCAC::CANCELLED`.

Completions edit the document on the user's behalf, so they require the `SCIF::EDIT` flag.

-INPUT-
int Length: The number of bytes of the typed prefix that precede the cursor.
cstr Candidates: Completion candidates, separated by line feeds.
int(SCACF) Flags: Optional flags.

-RESULT-
Okay:
NullArgs:
NoData: `Candidates` does not contain any candidates.
OutOfRange: `Length` is negative or exceeds the start of the cursor's line.
ReadOnly: The editor does not have the `SCIF::EDIT` flag.
NotPossible: The editor is disabled, or its host viewport has been freed.

*********************************************************************************************************************/

static ERR SCINTILLA_ShowCompletions(extScintilla *Self, struct sci::ShowCompletions *Args)
{
   kt::Log log;

   if ((not Args) or (not Args->Candidates)) return log.warning(ERR::NullArgs);

   log.branch("Length: %d, Flags: $%.8x", Args->Length, int(Args->Flags));
   return Self->API->panShowCompletions(Args->Length, Args->Candidates, Args->Flags);
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

//********************************************************************************************************************
// Resolve explicit fold operations against current lexer levels.  New methods retain ToggleFold's enclosing-fold rule.

static ERR fold_line(extScintilla *Self, int Line, int &Header, int &Resolved)
{
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;
   Resolved = Line IS -1 ? int(SCICALL(SCI_LINEFROMPOSITION, SCICALL(SCI_GETCURRENTPOS))) : Line;
   if ((Resolved < 0) or (Resolved >= SCICALL(SCI_GETLINECOUNT))) return ERR::OutOfRange;
   Header = -1;
   if (Self->FoldingMarkers) {
      Self->API->panRefreshFolds();
      Header = (SCICALL(SCI_GETFOLDLEVEL, Resolved) & SC_FOLDLEVELHEADERFLAG) ? Resolved :
         int(SCICALL(SCI_GETFOLDPARENT, Resolved));
      if ((Header >= 0) and (SCICALL(SCI_GETLASTCHILD, Header, -1) <= Header)) Header = -1;
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ConvertEOLs: Converts existing line endings as one undo operation.

Converts every CR, LF and CRLF sequence in the document to the representation given by `Mode`, including documents
with mixed line endings.  The conversion is recorded as a single undo action, so one #Undo() restores the original
bytes.  It can also be nested inside a #BeginUndoAction() group.  A document that is empty or already uses `Mode`
throughout is left unchanged and no undo step is created.

!SCEOL

This method does not alter #EOLMode, which controls the line endings of subsequently entered text.  Set both to
normalise a document and keep it consistent.  Conversion is a programmatic edit, so it does not require the `EDIT`
flag in #Flags.  It is reported through #ChangeCallback and advances #DocumentRevision like any other edit.

The document length is checked before any text is modified, so a failed conversion leaves the content unchanged.

-INPUT-
int(SCEOL) Mode: The line-ending representation to apply to existing text.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The method was called from within a synchronous editor notification.
OutOfRange: `Mode` is invalid, or the converted document would exceed the maximum document length.

-TAGS-
mutates-object

*********************************************************************************************************************/

static ERR SCINTILLA_ConvertEOLs(extScintilla *Self, struct sci::ConvertEOLs *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;
   if ((int(Args->Mode) < 0) or (int(Args->Mode) > 2)) return ERR::OutOfRange;
   return Self->API->panConvertEOLs(Args->Mode);
}

/*********************************************************************************************************************

-METHOD-
ExpandFold: Expands a fold without toggling its current state.

Expands the fold that starts at `Line`.  If the line does not start a fold, the innermost fold that contains it is
expanded.  Nested folds keep their own state, so a collapsed child fold remains collapsed.  If the fold is hidden
within a collapsed ancestor, the ancestors are also expanded so that it becomes visible, and the caret may move to the
fold's first line.  Expanding a fold that is already expanded and visible succeeds without change.

Unlike #ToggleFold(), the result does not depend on the fold's current state.  Folding must be enabled with
#FoldingMarkers, and the document needs a #Lexer that supports folding.

-INPUT-
int Line: Zero-based document line, or -1 for the caret line.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy
OutOfRange
NothingDone: Folding is disabled, or the line has no fold containing child lines.

*********************************************************************************************************************/

static ERR SCINTILLA_ExpandFold(extScintilla *Self, struct sci::ExpandFold *Args)
{
   if (not Args) return ERR::NullArgs;
   int header, line;
   if (auto error = fold_line(Self, Args->Line, header, line); error != ERR::Okay) return error;
   if (header < 0) return ERR::NothingDone;
   SCICALL(SCI_FOLDLINE, header, SC_FOLDACTION_EXPAND);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
CollapseFold: Collapses a fold without toggling its current state.

Collapses the fold that starts at `Line`, hiding its child lines.  If the line does not start a fold, the innermost fold
that contains it is collapsed.  The states of nested folds are retained and apply again when the fold is expanded with
#ExpandFold().  Collapsing a fold that is hidden within a collapsed ancestor records its state without expanding the
ancestors.  Collapsing a fold that is already collapsed succeeds without change.

Unlike #ToggleFold(), the result does not depend on the fold's current state.  Folding must be enabled with
#FoldingMarkers, and the document needs a #Lexer that supports folding.

-INPUT-
int Line: Zero-based document line, or -1 for the caret line.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy
OutOfRange
NothingDone: Folding is disabled, or the line has no fold containing child lines.

*********************************************************************************************************************/

static ERR SCINTILLA_CollapseFold(extScintilla *Self, struct sci::CollapseFold *Args)
{
   if (not Args) return ERR::NullArgs;
   int header, line;
   if (auto error = fold_line(Self, Args->Line, header, line); error != ERR::Okay) return error;
   if (header < 0) return ERR::NothingDone;
   SCICALL(SCI_FOLDLINE, header, sptr_t(SC_FOLDACTION_CONTRACT));
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
FoldAll: Expands or collapses every fold, including nested folds.

Applies the same state to every fold in the document.  Collapsing marks every fold as collapsed, including folds nested
within other folds, so expanding an outer fold afterwards with #ExpandFold() leaves its children collapsed.  Expanding
clears all nested collapsed state.  Document text, undo history and #DocumentRevision are not affected.

Folding must be enabled with #FoldingMarkers.  If the document contains no folds, the call succeeds and has no effect.

-INPUT-
int Expanded: TRUE to expand every fold, FALSE to collapse every fold.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy
NothingDone: Folding is disabled.

*********************************************************************************************************************/

static ERR SCINTILLA_FoldAll(extScintilla *Self, struct sci::FoldAll *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;
   if (not Self->FoldingMarkers) return ERR::NothingDone;
   Self->API->panRefreshFolds();
   SCICALL(SCI_FOLDALL, Args->Expanded ? SC_FOLDACTION_EXPAND :
      SC_FOLDACTION_CONTRACT | SC_FOLDACTION_CONTRACT_EVERY_LEVEL);
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
GetFold: Queries a line's containing fold and visibility using current lexer levels.

Reports the fold that #ExpandFold() and #CollapseFold() would act on for `Line`: the fold that starts at the line, or
otherwise the innermost fold that contains it.  Fold levels are brought up to date before the query, so the result
reflects recent edits.  Document text, undo history and #DocumentRevision are not affected.

If #FoldingMarkers is disabled or no fold applies, `Header` is -1 and `Expanded` is TRUE.  `Visible` is independent
of `Expanded`: a line within an expanded fold is still hidden if any enclosing fold is collapsed.

The result is a snapshot.  Edits move fold state with the lines of the document and can expand folds around edited
text, while changing the #Lexer or disabling #FoldingMarkers expands every fold.  Fold state is not restored
automatically after such changes.  A client that saves and restores folds must re-evaluate the headers against the
current text, then collapse child folds before their parents.

-INPUT-
int Line: Zero-based document line, or -1 for the caret line.
&int Header: Receives the line that starts the fold, or -1 if no fold applies.
&int Expanded: Receives TRUE if the fold is expanded, or if no fold applies.
&int Visible: Receives TRUE if the line is visible through all of its enclosing folds.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The method was called from within a synchronous editor notification, where fold levels cannot be refreshed.
OutOfRange

*********************************************************************************************************************/

static ERR SCINTILLA_GetFold(extScintilla *Self, struct sci::GetFold *Args)
{
   if (not Args) return ERR::NullArgs;
   int header, line;
   if (auto error = fold_line(Self, Args->Line, header, line); error != ERR::Okay) return error;
   Args->Header = header;
   Args->Expanded = header < 0 ? TRUE : int(SCICALL(SCI_GETFOLDEXPANDED, header));
   Args->Visible = SCICALL(SCI_GETLINEVISIBLE, line);
   return ERR::Okay;
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

static ERR SCINTILLA_Undo(extScintilla *Self, struct acUndo *)
{
   kt::Log log;

   log.branch();

   SCICALL(SCI_UNDO);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
ActivePopup: Indicates the code assistance pop-up that is open.

This read-only field reports `SCPOP::COMPLETIONS` while the completion list of #ShowCompletions() is open,
`SCPOP::CALL_TIP` while the call tip of #ShowCallTip() is open, and `SCPOP::NONE` otherwise.

*********************************************************************************************************************/

static ERR GET_ActivePopup(extScintilla *Self, SCPOP *Value)
{
   *Value = Self->API ? Self->API->panActivePopup() : SCPOP::NONE;
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
ChangeCallback: Receives an ordered stream of document byte changes after editing completes.

Set this field with a function reference to receive a record of every insertion and deletion of text.  The
prototype for the callback function is `Function(*Scintilla, struct(*ScintillaChange) Change)`.  Each
!ScintillaChange structure describes one change:

!ScintillaChange

Every change is reported, whether it originates from user input, the clipboard, programmatic methods such as
#ReplaceRange(), file loading, undo or redo.  Each record corresponds to exactly one increment of #DocumentRevision,
and records are delivered in the order that the changes occurred without being coalesced.  A client that starts with
a copy of the document can therefore replay the stream to keep an exact mirror of the text.  An edit that replaces
existing text is reported as a deletion followed by an insertion.  Edits made inside an undo group are reported
individually.

Delivery is deferred to the next cycle of message processing, after the editing call has returned.  Edits made from
within the callback are queued and delivered after the current batch completes.

Changes made while no callback is defined are not recorded.  Setting, replacing or clearing the callback discards
any records that have not yet been delivered, including the remainder of a batch in progress.  If a script callback
raises an error, the callback is cleared and its undelivered records are discarded.

The `Change` structure is only valid for the duration of the callback, so C++ clients must copy any values that need
to be retained.  Script clients receive their own copy of the record.

*********************************************************************************************************************/

static ERR GET_ChangeCallback(extScintilla *Self, FUNCTION * &Value)
{
   if (not Self->ChangeCallback.defined()) return ERR::FieldNotSet;
   Value = &Self->ChangeCallback;
   return ERR::Okay;
}

static ERR SET_ChangeCallback(extScintilla *Self, FUNCTION *Value)
{
   if (Self->API) Self->API->panResetChangeReports();
   replace_callback(Self->ChangeCallback, Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
DocumentRevision: Monotonically increasing revision of document text.

The DocumentRevision value starts at zero and is incremented for every insertion or deletion of text, including
edits made by the user, programmatic changes, undo, redo and the loading of new content.  Styling, folding and
selection changes do not affect the revision.  The value is never decremented, so undoing an edit does not restore
an earlier revision.

Clients can record the revision when caching information about the document, then compare it later to determine
whether the text may have changed in the interim.  Each change reported to #ChangeCallback carries the revision
reached after that change in its `Revision` value.

*********************************************************************************************************************/

static ERR GET_DocumentRevision(extScintilla *Self, int64_t *Value)
{
   *Value = Self->DocumentRevision;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CompletionCallback: Reports the events that relate to code completion and call tips.

Set this field with a function reference to provide completion candidates and call tips.  The prototype for the
callback function is `Function(*Scintilla, SCAC Event, INT Position, INT Value, CSTRING Text)`.

The `Event` is one of the following:

<types lookup="SCAC"/>

`SCAC::REQUEST` and `SCAC::CHAR_ADDED` are reported as they occur, so the client can respond with
#ShowCompletions() or #ShowCallTip().  `Position` is the cursor position, and for `SCAC::CHAR_ADDED` the `Value` is
the Unicode code point of the character that was typed.  The client usually examines the text that precedes the
cursor to choose its candidates.  Ctrl+Space is reported only while the editor has the `SCIF::EDIT` flag.

`SCAC::SELECTED`, `SCAC::CANCELLED` and `SCAC::CALL_TIP_CLICK` are reported on the next cycle of message processing,
after the editor has completed the operation.  For `SCAC::SELECTED`, the `Text` is the accepted candidate and
`Position` is the start of the replaced text, and the candidate has already been inserted.  For
`SCAC::CALL_TIP_CLICK`, `Position` is the position that the call tip refers to.  The `Text` is empty for every event
other than `SCAC::SELECTED`.

*********************************************************************************************************************/

static ERR GET_CompletionCallback(extScintilla *Self, FUNCTION * &Value)
{
   if (Self->CompletionCallback.defined()) {
      Value = &Self->CompletionCallback;
      return ERR::Okay;
   }
   else return ERR::FieldNotSet;
}

static ERR SET_CompletionCallback(extScintilla *Self, FUNCTION *Value)
{
   replace_callback(Self->CompletionCallback, Value);
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
   if (Self->initialised()) {
      if (not KotukuScintilla::PublicInteger(SCICALL(SCI_GETCOLUMN, SCICALL(SCI_GETCURRENTPOS)), *Value))
         return ERR::OutOfRange;
   }
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
   if (Self->initialised()) {
      if (not KotukuScintilla::PublicInteger(SCICALL(SCI_LINEFROMPOSITION, SCICALL(SCI_GETCURRENTPOS)), *Value))
         return ERR::OutOfRange;
   }
   else *Value = 0;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
FileDrop: A callback for receiving drag and drop file notifications.

Set this field with a function reference to receive the paths of files that the user drops onto the editor.  The
prototype for the callback function is `Function(*Scintilla, CSTRING Path)`.  The callback is called once for each
dropped file, in the order provided by the drag source.

The editor accepts file drops only while this field is set, so leave it undefined if files are not to be dropped
onto the editor.  The editor does not act on the dropped files; it is the responsibility of the client to open, load
or insert them as required.  File drops do not require the `SCIF::EDIT` flag, but are refused while the editor is
disabled.  When a drag source offers both files and text, the files are preferred and the text is not inserted.

Delivery is cancelled if the document is modified between the drop and the arrival of the data.  If the callback
destroys the editor, the remaining paths are not reported.

Drag and drop requires the hosting @VectorScene to have `VPF::DROP_TARGETS` set.

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
   replace_callback(Self->FileDrop, Value);
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
         apply_metrics(Self);
         Self->API->panSetFolding(true);
      }
   }
   else {
      Self->FoldingMarkers = FALSE;
      if (Self->initialised()) {
         apply_metrics(Self);
         Self->API->panSetFolding(false);
      }
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
TextStyle: A font definition that selects the font used for drawing and measuring document text.

TextStyle is the exclusive interface for selecting the editor's font, e.g. `face=Source Code Pro; size=14px;
weight=500`.  Its syntax is shared with @VectorText.TextStyle, and the default is
`face=Source Code Pro; size=14px`.

Assignment overwrites the declared keys only, and reading the field returns the canonical form of the accumulated
definition.  The `face`, `size`, style and axis keys are honoured.  Other keys are retained but have no effect, as the
theme palette owns colours.  Each style's bold and italic attributes are applied over the definition.

The size is in the units of the #View, so fonts are loaded at the size multiplied by the device scale of the host,
rounded to whole pixels.  Bitmap faces select their nearest supported point size and are not scaled.  A value of
`size` in points is converted at the fixed ratio of 96 / 72.

Set this field before or after initialisation.  A change reloads the styles and updates layout and margins.  A
malformed definition returns `ERR::Syntax`, and a failed font resolution retains the existing settings.

*********************************************************************************************************************/

static ERR SET_TextStyle(extScintilla *Self, const std::string_view &Value)
{
   kt::Log log;

   if (Value.empty()) return ERR::Okay;

   // Later keys override earlier ones, so appending the value merges it into the current definition.

   std::string canonical;
   const auto merged = Self->TextStyle + ";" + std::string(Value);
   if (auto error = vec::NormaliseTextStyle(merged, &canonical); error != ERR::Okay) return log.warning(error);

   KeyValue *result;
   if (auto error = vec::ReadTextStyle(canonical, &result); error != ERR::Okay) return log.warning(error);
   auto keys = (const KEYVALUE *)result;

   std::string face(keys->at("face"));
   std::string size(keys->at("size"));
   const double number = std::stod(size);
   const int pixels = std::max(1, int(std::lround(size.ends_with("pt") ? number * 96.0 / 72.0 : number)));

   if (Self->initialised()) return resolve_fonts(Self, canonical, face, pixels);

   Self->TextStyle = canonical;
   Self->Face = face;
   Self->FontSize = pixels;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LeftMargin: The amount of white-space at the left side of the page.

The margin is expressed in the units of the #View, and is scaled with the content if the host is scaled.

*********************************************************************************************************************/

static ERR SET_LeftMargin(extScintilla *Self, int Value)
{
   if ((Value >= 0) and (Value <= 100)) {
      Self->LeftMargin = Value;
      if (Self->initialised()) apply_metrics(Self);
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
   replace_callback(Self->MarginClick, Value);

   if (Self->API) SCICALL(SCI_SETMARGINSENSITIVEN, 1, (sptr_t)Self->MarginClick.defined());
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
   if (Self->API and Self->API->panInNotification()) return ERR::Busy;
   if (not find_lexer_profile(Value)) return ERR::NoSupport;
   if (Self->API) return Self->API->SetLexer(Value);
   Self->Lexer = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LineCount: The total number of lines in the document.

*********************************************************************************************************************/

static ERR GET_LineCount(extScintilla *Self, int *Value)
{
   if (Self->initialised()) {
      if (not KotukuScintilla::PublicInteger(SCICALL(SCI_GETLINECOUNT), *Value)) return ERR::OutOfRange;
      return ERR::Okay;
   }
   else return ERR::NotInitialised;
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
   replace_callback(Self->EventCallback, Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EventFlags: Specifies events that need to be reported from the Scintilla object.

To receive event notifications, set #EventCallback with a function reference and the EventFlags field with a mask that
indicates the events that need to be received.

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
RightMargin: Defines the amount of white-space at the right side of the page.

The margin is expressed in the units of the #View, and is scaled with the content if the host is scaled.

*********************************************************************************************************************/

static ERR SET_RightMargin(extScintilla *Self, int Value)
{
   if ((Value >= 0) and (Value <= 100)) {
      Self->RightMargin = Value;
      if (Self->initialised()) SCICALL(SCI_SETMARGINRIGHT, 0, scaled(Self, Self->RightMargin));
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
      if (Self->initialised()) SCICALL(SCI_SETVIEWWS, (uptr_t)SCWS_VISIBLEALWAYS);
   }
   else {
      Self->ShowWhitespace = 0;
      if (Self->initialised()) SCICALL(SCI_SETVIEWWS, (uptr_t)SCWS_INVISIBLE);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
CurrentPos: The main selection caret as a zero-based UTF-8 byte offset.

*********************************************************************************************************************/

static ERR GET_CurrentPos(extScintilla *Self, int *Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   const auto position = SCICALL(SCI_GETSELECTIONNCARET, SCICALL(SCI_GETMAINSELECTION));
   return KotukuScintilla::PublicInteger(position, *Value) ? ERR::Okay : ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
Anchor: The main selection anchor as a zero-based UTF-8 byte offset.  It may follow CurrentPos.

*********************************************************************************************************************/

static ERR GET_Anchor(extScintilla *Self, int *Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   const auto position = SCICALL(SCI_GETSELECTIONNANCHOR, SCICALL(SCI_GETMAINSELECTION));
   return KotukuScintilla::PublicInteger(position, *Value) ? ERR::Okay : ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
SelectionStart: The lower byte offset of the main selection, independent of selection direction.

*********************************************************************************************************************/

static ERR GET_SelectionStart(extScintilla *Self, int *Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   const auto position = SCICALL(SCI_GETSELECTIONNSTART, SCICALL(SCI_GETMAINSELECTION));
   return KotukuScintilla::PublicInteger(position, *Value) ? ERR::Okay : ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
SelectionEnd: The exclusive upper byte offset of the main selection, independent of direction.

*********************************************************************************************************************/

static ERR GET_SelectionEnd(extScintilla *Self, int *Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   const auto position = SCICALL(SCI_GETSELECTIONNEND, SCICALL(SCI_GETMAINSELECTION));
   return KotukuScintilla::PublicInteger(position, *Value) ? ERR::Okay : ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
TextLength: The document length in bytes, including tabs, CR and LF bytes.  An empty document has length zero.

*********************************************************************************************************************/

static ERR GET_TextLength(extScintilla *Self, int *Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   return KotukuScintilla::PublicInteger(SCICALL(SCI_GETLENGTH), *Value) ? ERR::Okay : ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
CanUndo: Reports whether the backend has an edit available to undo.

*********************************************************************************************************************/

static ERR GET_CanUndo(extScintilla *Self, int *Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   return KotukuScintilla::PublicInteger(SCICALL(SCI_CANUNDO), *Value) ? ERR::Okay : ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
CanRedo: Reports whether the backend has an undone edit available to redo.

*********************************************************************************************************************/

static ERR GET_CanRedo(extScintilla *Self, int *Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   return KotukuScintilla::PublicInteger(SCICALL(SCI_CANREDO), *Value) ? ERR::Okay : ERR::OutOfRange;
}

/*********************************************************************************************************************

-FIELD-
SelectedText: Retrieves the contiguous byte span of the main selection.

Returns the document bytes between the start and end of the main selection.  If the selection is empty, an empty
string is returned.  When multiple or rectangular selections are active, only the main selection range is returned
and additional ranges are ignored.  Line-end characters within the selection are returned without modification.

-TAGS-
api-owns-result

*********************************************************************************************************************/

static ERR GET_SelectedText(extScintilla *Self, std::string_view &Value)
{
   if (not Self->initialised() or not Self->API) return ERR::NotInitialised;
   const auto main = SCICALL(SCI_GETMAINSELECTION);
   const auto start = SCICALL(SCI_GETSELECTIONNSTART, main);
   const auto end = SCICALL(SCI_GETSELECTIONNEND, main);
   if ((start < 0) or (end < start) or (end > KotukuScintilla::document_limit)) return ERR::OutOfRange;
   Self->SelectionBuffer.resize(size_t(end - start));
   Scintilla::TextRangeFull range { { start, end }, Self->SelectionBuffer.data() };
   SCICALL(SCI_GETTEXTRANGEFULL, 0, sptr_t(&range));
   Value = Self->SelectionBuffer;
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
   if ((len < 0) or (len > KotukuScintilla::document_limit)) return ERR::OutOfRange;

   Self->StringBuffer.clear();
   Self->StringBuffer.resize(len);
   SCICALL(SCI_GETTEXT, len+1, Self->StringBuffer.c_str());
   Value = std::string_view(Self->StringBuffer);
   return ERR::Okay;
}

static ERR SET_String(extScintilla *Self, const std::string_view &Value)
{
   if (Value.size() > size_t(KotukuScintilla::document_limit)) return ERR::OutOfRange;
   if (Self->initialised()) {
      if (not Value.empty()) {
         const std::string text(Value);
         SCICALL(SCI_SETTEXT, 0UL, text.c_str());
      }
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
      if (Self->initialised()) apply_metrics(Self);
   }
   else {
      Self->Symbols = FALSE;
      if (Self->initialised()) apply_metrics(Self);
   }
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
EOLMode: Line-ending policy for newly entered keyboard and clipboard text.
Lookup: SCEOL

Determines the line ending inserted when the user presses Enter, and the line endings applied to pasted and dropped
text.  Rectangular copies also end each row with this line ending.  The default is `LF` on every platform.

Changing this field does not modify existing text; use #ConvertEOLs() to normalise it.  Text supplied through #String,
#ReplaceText() and other programmatic operations keeps its original line endings, as does text loaded from a file.
Loading does not detect or change this setting, so applications that want to match a file's line endings must inspect
the text and set EOLMode explicitly.  #SaveToObject() writes the document bytes without conversion.

*********************************************************************************************************************/

static ERR GET_EOLMode(extScintilla *Self, SCEOL *Value)
{
   *Value = Self->EOLMode;
   return ERR::Okay;
}

static ERR SET_EOLMode(extScintilla *Self, SCEOL Value)
{
   if ((int(Value) < 0) or (int(Value) > 2)) return ERR::OutOfRange;
   if (Self->API and Self->API->panInNotification()) return ERR::Busy;
   Self->EOLMode = Value;
   if (Self->API) SCICALL(SCI_SETEOLMODE, int(Value));
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
IndentWidth: Indentation width in fixed-width character columns, independently of tab stops.

Sets the width of one indentation level, which determines how far keyboard indentation commands indent and outdent
lines and the spacing of #IndentationGuides.  This allows indentation to differ from #TabWidth, which continues to
control tab stops; for example, a width of 4 with tab stops at 8.  #AllowTabs determines whether indentation is written
with tab characters.  The default of 0 uses the #TabWidth value.  Valid values range from 0 to 200.

#AutoIndent is not affected, and continues to copy the leading whitespace of the previous line.  Changing this field
does not modify existing whitespace.

*********************************************************************************************************************/

static ERR GET_IndentWidth(extScintilla *Self, int *Value)
{
   *Value = Self->IndentWidth;
   return ERR::Okay;
}

static ERR SET_IndentWidth(extScintilla *Self, int Value)
{
   if ((Value < 0) or (Value > 200)) return ERR::OutOfRange;
   if (Self->API and Self->API->panInNotification()) return ERR::Busy;
   Self->IndentWidth = Value;
   if (Self->API) SCICALL(SCI_SETINDENT, Value);
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
IndentationGuides: Controls the display of indentation guides without changing document text.
Lookup: SCGUIDE

Indentation guides are vertical lines drawn at each indentation level, spaced by #IndentWidth or by #TabWidth if
IndentWidth is zero.  Modes that infer the indentation of blank lines also use the indentation behaviour of the active
#Lexer.  The setting is retained when the lexer or theme changes.  The default is `NONE`.

*********************************************************************************************************************/

static ERR GET_IndentationGuides(extScintilla *Self, SCGUIDE *Value)
{
   *Value = Self->IndentationGuides;
   return ERR::Okay;
}

static ERR SET_IndentationGuides(extScintilla *Self, SCGUIDE Value)
{
   if ((int(Value) < 0) or (int(Value) > 3)) return ERR::OutOfRange;
   if (Self->API and Self->API->panInNotification()) return ERR::Busy;
   Self->IndentationGuides = Value;
   if (Self->API) SCICALL(SCI_SETINDENTATIONGUIDES, int(Value));
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
ThemeXML: Configures the colour palette of the editor through an XML object.

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
All palette attributes are optional and case-sensitive.  Omitted colours retain their current values, so a single
colour can be changed by assigning a palette with one attribute.  Hex colours, SVG colour names and colour functions
such as `rgb()` and `oklch()` are accepted.  Gradients, painter lists and unknown attributes are rejected.

The attributes are applied as follows:

<list type="bullet">
<li>`background` fills the text area and applies to all lexer styles.  The margins use `marginBackground`.</li>
<li>`text` applies to plain text, identifiers and any unclassified styles.</li>
<li>`comment`, `keyword`, `string`, `number`, `operator`, `preprocessor`, `type` and `error` colour the semantic
styles of the active lexer.</li>
<li>`margin` and `marginBackground` colour the line number, symbol and fold margins.</li>
<li>`cursor` colours the caret.</li>
<li>`lineHighlight` colours the line that contains the caret while the editor has the focus.</li>
<li>`selectionForeground` is drawn as the colour of selected text, overriding the colours of its style.</li>
<li>`selectionBackground` is drawn behind the main selection.</li>
</list>

The following attributes are derived.  Each one follows the source colour shown in brackets until it is assigned, so
themes that do not mention them remain consistent.  Assigning the value `inherit` restores the link to the source.

<list type="bullet">
<li>`whitespace` (`margin`) colours visible whitespace, as enabled by #ShowWhitespace.</li>
<li>`indentGuide` (`margin`) colours the indentation guides, as enabled by #IndentationGuides.</li>
<li>`braceMatch` (`keyword`) colours a brace and its matching partner while the caret is next to it.</li>
<li>`braceMismatch` (`error`) colours a brace that has no partner while the caret is next to it.</li>
<li>`braceBackground` (`marginBackground`) is drawn behind highlighted braces.</li>
<li>`selectionInactiveBackground` (`selectionBackground`) is drawn behind the selection while the editor does not have
the focus.</li>
<li>`selectionAdditionalBackground` (`selectionBackground`) is drawn behind the additional rows of a rectangular
selection.</li>
</list>

Alpha is ignored except for `lineHighlight` and the selection colours, where zero disables the highlight or override.
A disabled `selectionInactiveBackground` uses the active colour, and a disabled `selectionAdditionalBackground` uses
Scintilla's default colour for additional selections.  Other alpha values produce opaque colours, except that a
translucent `lineHighlight` or `selectionBackground` is blended over the text after it is drawn, which also tints the
text.  An opaque highlight is drawn beneath the text.  The `selectionBackground` decides the layer of all selections,
so when it is translucent the inactive and additional selections are also blended over the text.  Translucent colours
blend in the same way as a translucent vector fill, i.e. with the gamma-correct blending of the Vector module, so a 50%
colour appears lighter than an average of the sRGB values.  Consider disabling `selectionForeground` with a
translucent `selectionBackground` so that selected text retains its styled colours.

The default palette is a white background, black text and operators, grey comments, firebrick keywords, dark red
strings, blue numbers, green directives, violet types, a lavender line highlight, a black cursor, and white
selected text on a dark blue background.  Theme colours are stored independently for each editor and are retained
across lexer changes.

The colours are copied during assignment and the assigned XML object is not retained or modified.  Assigning before
initialisation configures the initial colours; assigning afterwards repaints without changing text, selection or
contracted folds.  Invalid themes leave all colours unchanged.

Reading ThemeXML returns an @XML object that describes the full palette, with every attribute present.  Colours are
expressed in `#RRGGBB` format, or `#RRGGBBAA` for translucent `lineHighlight` and selection colours, and derived
colours that follow their source read as `inherit`.  The result can therefore be assigned back to this or another
editor without loss.  The object belongs to the editor and is rewritten on each
read, so copy any values that need to survive a later read.  Do not free it; it is released with the editor.

*********************************************************************************************************************/

// Derived colours name the colour that they inherit; sources are never derived themselves.  The position of each
// entry is its bit in ThemeOverrides.

static constexpr struct {
   std::string_view Name;
   RGB8 extScintilla::*Field;
   bool Alpha; // The alpha channel is significant and is preserved on readback
   RGB8 extScintilla::*Inherit = nullptr;
} glThemeColours[] = {
   { "comment", &extScintilla::CommentColour, false },
   { "keyword", &extScintilla::KeywordColour, false },
   { "string", &extScintilla::StringColour, false },
   { "number", &extScintilla::NumberColour, false },
   { "operator", &extScintilla::OperatorColour, false },
   { "preprocessor", &extScintilla::PreprocessorColour, false },
   { "type", &extScintilla::TypeColour, false },
   { "error", &extScintilla::ErrorColour, false },
   { "margin", &extScintilla::MarginColour, false },
   { "marginBackground", &extScintilla::MarginBkgd, false },
   { "text", &extScintilla::TextColour, false },
   { "background", &extScintilla::BkgdColour, false },
   { "cursor", &extScintilla::CursorColour, false },
   { "lineHighlight", &extScintilla::LineHighlight, true },
   { "selectionForeground", &extScintilla::SelectFore, true },
   { "selectionBackground", &extScintilla::SelectBkgd, true },
   { "whitespace", &extScintilla::Whitespace, false, &extScintilla::MarginColour },
   { "indentGuide", &extScintilla::IndentGuide, false, &extScintilla::MarginColour },
   { "braceMatch", &extScintilla::BraceMatch, false, &extScintilla::KeywordColour },
   { "braceMismatch", &extScintilla::BraceMismatch, false, &extScintilla::ErrorColour },
   { "braceBackground", &extScintilla::BraceBkgd, false, &extScintilla::MarginBkgd },
   { "selectionInactiveBackground", &extScintilla::SelectInactiveBkgd, true, &extScintilla::SelectBkgd },
   { "selectionAdditionalBackground", &extScintilla::SelectAdditionalBkgd, true, &extScintilla::SelectBkgd }
};

static_assert(std::size(glThemeColours) <= 32, "ThemeOverrides has a bit for each theme colour");

static bool theme_inherits(const extScintilla *Self, size_t Index)
{
   return glThemeColours[Index].Inherit and not (Self->ThemeOverrides & (1u << Index));
}

static ERR GET_ThemeXML(extScintilla *Self, objXML **Value)
{
   std::string statement("<theme><palette");
   for (size_t i=0; i < std::size(glThemeColours); i++) {
      const auto &colour = glThemeColours[i];
      if (theme_inherits(Self, i)) {
         statement += std::format(" {}=\"inherit\"", colour.Name);
         continue;
      }
      const auto &rgb = Self->*colour.Field;
      statement += std::format(" {}=\"#{:02x}{:02x}{:02x}", colour.Name, int(rgb.Red), int(rgb.Green),
         int(rgb.Blue));
      if (colour.Alpha and (rgb.Alpha != 255)) statement += std::format("{:02x}", int(rgb.Alpha));
      statement += '"';
   }
   statement += "/></theme>";

   // The cache is referenced by ID because the client may free it despite being advised otherwise.

   auto cache = Self->ThemeCacheID ? (objXML *)GetObjectPtr(Self->ThemeCacheID) : nullptr;
   if (cache) {
      if (auto error = cache->setStatement(statement); error != ERR::Okay) return error;
   }
   else {
      kt::SwitchContext context(Self);
      if (not (cache = objXML::create::local(fl::Statement(statement)))) return ERR::CreateObject;
      Self->ThemeCacheID = cache->UID;
   }

   *Value = cache;
   return ERR::Okay;
}

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

   std::vector<std::pair<size_t, std::optional<RGB8>>> updates; // An empty colour restores inheritance
   for (size_t i=1; i < palette->Attribs.size(); i++) {
      const auto &attribute = palette->Attribs[i];
      auto colour = std::find_if(std::begin(glThemeColours), std::end(glThemeColours),
         [&attribute](const auto &Colour) { return Colour.Name IS attribute.Name; });
      if (colour IS std::end(glThemeColours)) return ERR::Syntax;
      auto index = size_t(colour - std::begin(glThemeColours));

      std::string_view value(attribute.Value);
      auto first = value.find_first_not_of(" \t\r\n");
      if (first != std::string_view::npos) value = value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
      if (value IS "inherit") {
         if (not colour->Inherit) return ERR::Syntax;
         updates.emplace_back(index, std::nullopt);
         continue;
      }

      VectorPainter painter = {};
      std::string_view remainder;
      if ((vec::ReadPainter(nullptr, attribute.Value, &painter, &remainder) != ERR::Okay) or
          painter.Gradient or painter.Pattern or painter.Image or
          (remainder.find_first_not_of(" \t\r\n") != std::string_view::npos)) return ERR::Syntax;
      const auto &rgb = painter.Colour;
      if (not std::isfinite(rgb.Red) or not std::isfinite(rgb.Green) or not std::isfinite(rgb.Blue) or
          not std::isfinite(rgb.Alpha)) return ERR::Syntax;
      auto channel = [](float Value) { return uint8_t(std::lround(std::clamp(Value, 0.0f, 1.0f) * 255)); };
      updates.emplace_back(index, RGB8 { channel(rgb.Red), channel(rgb.Green), channel(rgb.Blue),
         channel(rgb.Alpha) });
   }

   for (const auto &[index, colour] : updates) {
      if (colour) {
         Self->*glThemeColours[index].Field = *colour;
         Self->ThemeOverrides |= 1u << index;
      }
      else Self->ThemeOverrides &= ~(1u << index);
   }

   for (size_t i=0; i < std::size(glThemeColours); i++) {
      if (theme_inherits(Self, i)) Self->*glThemeColours[i].Field = Self->*glThemeColours[i].Inherit;
   }
   if (Self->initialised()) {
      clear_bitmap(Self);
      Self->API->SetStyleColours();
      apply_theme_elements(Self);
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

// Fonts are resolved through the font database, which reports whether a face is scalable.

static bool face_scalable(std::string_view Face)
{
   std::string family(Face.empty() ? "*" : Face);
   if (not family.ends_with("*")) family.append(",*");
   std::string_view resolved;
   if (fnt::ResolveFamilyName(family, &resolved, nullptr) != ERR::Okay) return false;

   std::string path;
   FMETA meta = FMETA::NIL;
   if (fnt::SelectFont(resolved, "Regular", &path, &meta) != ERR::Okay) return false;
   return (meta & FMETA::SCALED) != FMETA::NIL;
}

// Update the styles and the measurements that depend on the fonts.

static void fonts_changed(extScintilla *Self)
{
   Self->API->panFontChanged();
   calc_longest_line(Self);
   Self->MarginDigits = -1;
   update_margin_width(Self);
}

// Fonts are loaded at the device scale, because Scintilla's client units are device pixels.  A change of face can
// change the device scale, in which case the View is reconfigured for it.

static ERR resolve_fonts(extScintilla *Self, const std::string &Definition, const std::string &Face, int Size)
{
   const bool scalable = face_scalable(Face);
   const double scale = device_scale(Self, scalable);

   APTR handle;
   if (auto error = vec::GetFontHandle(Face, Definition, 0, std::max(1, int(std::lround(Size * scale))), &handle);
         error != ERR::Okay) return error;

   Self->TextStyle = Definition;
   Self->Face = Face;
   Self->FontSize = Size;
   Self->ScalableFace = scalable;
   Self->FontHandle = handle;

   if (Self->initialised()) {
      if (scale != Self->Scale) update_view(Self);
      else fonts_changed(Self);
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
   update_host_transform(Self, double(w), double(h));
   Self->Scale      = device_scale(Self, Self->ScalableFace);
   Self->ViewWidth  = std::max(1, int(std::lround(double(w) * Self->Scale)));
   Self->ViewHeight = std::max(1, int(std::lround(double(h) * Self->Scale)));
   Self->PageWidth  = Self->ViewWidth;
   Self->PageHeight = Self->ViewHeight;
   Self->PageScale  = Self->Scale;

   if (!(Self->Page = objVectorViewport::create::global(
         fl::Name("sciPage"),
         fl::Owner(Self->View->UID),
         fl::X(0), fl::Y(0),
         fl::Width(Self->PageWidth / Self->Scale), fl::Height(Self->PageHeight / Self->Scale)))) {
      return log.warning(ERR::CreateObject);
   }

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
         fl::Width(Self->Bitmap->Width / Self->Scale), fl::Height(Self->Bitmap->Height / Self->Scale),
         fl::Cursor(PTC::TEXT),
         fl::Fill(std::format("url(#{})", def_name))))) return log.warning(ERR::CreateObject);

   // The View's path has already been generated in order to read its size, so the first path change is not reported
   // to feedback_view().

   align_canvas(Self);

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
         else { acClear(Self); error = ERR::Okay; }
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
            error = SET_Lexer(Self, glLexers[i].Lexer);
            if (error != ERR::Okay) break;
            log.branch("Lexer for the loaded file is %d.", int(Self->Lexer));
            break;
         }
      }
      if (i >= std::size(glLexers)) log.msg("Failed to choose a lexer for %.*s", int(Path.size()), Path.data());
   }

   return error;
}

//********************************************************************************************************************
// Receiver for all keyboard events in the process.  The modifier state is recorded for pointer input, which does not
// report it.  Keys pressed while the application window lacks the keyboard focus are not reported, so update_focus()
// resets the state when the editor loses the focus.

static ERR modifier_event(const evKey *Event, APTR)
{
   auto Self = (extScintilla *)CurrentContext();
   Self->KeyShift = (Event->Qualifiers & KQ::SHIFT) != KQ::NIL;
   Self->KeyCtrl  = (Event->Qualifiers & KQ::CTRL) != KQ::NIL;
   Self->KeyAlt   = (Event->Qualifiers & KQ::ALT) != KQ::NIL;
   return ERR::Okay;
}

//********************************************************************************************************************
// Receiver for keyboard input from the Focus viewport.  The modifier state is also recorded here (see modifier_event())
// in case the key was delivered to the viewport directly rather than broadcast.

static ERR key_event(objVectorViewport *, KQ Qualifiers, KEY Code, int Unicode)
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

      auto callback = Self->EventCallback;
      if (not callback.defined() or Self->collecting()) return;
      callback.pin();
      const auto unpin = kt::Defer([&]() { callback.unpin(); });
      if (callback.isC()) {
         kt::SwitchContext ctx(callback.Context);
         auto routine = (void (*)(extScintilla *, SEF, APTR)) callback.Routine;
         routine(Self, Event, callback.Meta);
      }
      else if (callback.isScript()) {
         sc::Call(callback, std::to_array<ScriptArg>({
            { "Scintilla", Self, FD_OBJECTPTR }, { "EventFlags", int64_t(Event) }
         }));
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

   auto callback = Self->MarginClick;
   if (not callback.defined() or Self->collecting()) return;
   callback.pin();
   const auto unpin = kt::Defer([&]() { callback.unpin(); });
   if (callback.isC()) {
      kt::SwitchContext ctx(callback.Context);
      auto routine = (void (*)(extScintilla *, SCMG, int, KQ, APTR))callback.Routine;
      routine(Self, Margin, Line, Qualifiers, callback.Meta);
   }
   else if (callback.isScript()) {
      sc::Call(callback, std::to_array<ScriptArg>({
         { "Scintilla", Self, FD_OBJECTPTR },
         { "Margin", int(Margin) },
         { "Line", Line },
         { "Qualifiers", int(Qualifiers) }
      }));
   }
}

/*********************************************************************************************************************
** The scroll width determines the width of the Page, and is the pixel width of the widest line plus padding.  Exact
** widths require a line layout, which is too expensive to compute for every line of a large document.  Lines are
** ranked by their column count instead (tabs are expanded and UTF-8 continuation bytes are not counted) and only the
** highest ranked candidates are measured.  This is exact for monospaced text.  A proportional face can rank a line
** below a narrower one; such a line widens the Page when it is painted (see grow_scroll_width()).
*/

static const int SCROLL_WIDTH_PADDING = 30;

static int measure_widest_line(extScintilla *Self, int First, int Last, int &Line)
{
   static const int MAX_MEASURED = 64;

   Line = First;
   if (Last < First) return 0;

   const int tab_width = std::max(1, int(SCICALL(SCI_GETTABWIDTH)));

   std::vector<char> text;
   std::vector<std::pair<int, int>> candidates; // Columns, line
   candidates.reserve(Last - First + 1);
   int max_columns = 0;
   for (int line=First; line <= Last; line++) {
      const int start = SCICALL(SCI_POSITIONFROMLINE, line);
      const int end = SCICALL(SCI_GETLINEENDPOSITION, line);
      // Copy each line without moving the document's gap buffer away from the edit position.
      text.resize(size_t(SCICALL(SCI_LINELENGTH, line)) + 1);
      SCICALL(SCI_GETLINE, line, text.data());
      int columns = 0;
      for (int i=0; i < end - start; i++) {
         const auto c = uint8_t(text[i]);
         if (c IS '\t') columns += tab_width - (columns % tab_width);
         else if ((c & 0xc0) != 0x80) columns++;
      }
      max_columns = std::max(max_columns, columns);
      candidates.emplace_back(columns, line);
   }

   // Lines with fewer than two thirds of the maximum column count are not considered.

   const int threshold = (max_columns * 2) / 3;
   std::erase_if(candidates, [threshold](const auto &C) { return C.first < threshold; });
   const auto count = std::min(candidates.size(), size_t(MAX_MEASURED));
   std::partial_sort(candidates.begin(), candidates.begin() + count, candidates.end(),
      [](const auto &A, const auto &B) { return A.first > B.first; });

   int widest = 0;
   for (size_t i=0; i < count; i++) {
      const int line = candidates[i].second;
      const int start = SCICALL(SCI_POSITIONFROMLINE, line);
      const int end = SCICALL(SCI_GETLINEENDPOSITION, line);
      // Subtract the line start so margins and horizontal scrolling do not affect the measured width.
      const int width = SCICALL(SCI_POINTXFROMPOSITION, 0, end) - SCICALL(SCI_POINTXFROMPOSITION, 0, start);
      if (width > widest) {
         widest = width;
         Line = line;
      }
   }
   return widest;
}

//********************************************************************************************************************
// Apply LongestWidth to Scintilla's scroll width.

static void set_scroll_width(extScintilla *Self)
{
   if (not Self->initialised()) return;
   if (Self->LongestWidth >= scaled(Self, 60)) SCICALL(SCI_SETSCROLLWIDTH, Self->LongestWidth);
   else SCICALL(SCI_SETSCROLLWIDTH, 1UL);
}

//********************************************************************************************************************
// Compute the scroll width from the entire document.  Unlike edits, which only widen the Page, this can narrow it.

static void calc_longest_line(extScintilla *Self)
{
   if (not Self->FontHandle) return;

   kt::Log log(__FUNCTION__);
   log.traceBranch("Wrap: %d", Self->Wordwrap);

   Self->WidthCheckFirst = INT_MAX; // A full measurement supersedes the lines pending from edits
   Self->WidthCheckLast  = -1;

   if (Self->Wordwrap) {
      Self->LongestLine = 0;
      Self->LongestWidth = 0;
   }
   else {
      int line;
      Self->LongestWidth = measure_widest_line(Self, 0, SCICALL(SCI_GETLINECOUNT) - 1, line) +
         scaled(Self, SCROLL_WIDTH_PADDING);
      Self->LongestLine  = line;
   }

   log.trace("Longest line: %d, width: %d", Self->LongestLine, Self->LongestWidth);
   set_scroll_width(Self);
}

/*********************************************************************************************************************
** Widen the scroll width for the lines that have been modified since the last paint, and for any painted line that is
** wider than the scroll width (Scintilla records the widest line that it lays out for painting).  Edits never narrow
** the Page, which would otherwise shift the view while typing.
*/

static void grow_scroll_width(extScintilla *Self, int Width, int Line)
{
   const int padding = scaled(Self, SCROLL_WIDTH_PADDING);
   if ((Self->Wordwrap) or (Width + padding <= Self->LongestWidth)) return;
   Self->LongestWidth = Width + padding;
   Self->LongestLine  = Line;
   set_scroll_width(Self);
}

// Record the lines of an insertion or deletion for check_modified_widths(), which runs in the queued Repaint.  The
// recorded range follows the lines that are added or removed by subsequent edits.

static void track_modified_lines(extScintilla *Self, int Line, int LinesAdded)
{
   if (Self->WidthCheckLast >= Self->WidthCheckFirst) {
      if (Line < Self->WidthCheckFirst) Self->WidthCheckFirst = std::max(Line, Self->WidthCheckFirst + LinesAdded);
      if (Line <= Self->WidthCheckLast) Self->WidthCheckLast = std::max(Line, Self->WidthCheckLast + LinesAdded);
   }
   Self->WidthCheckFirst = std::min(Self->WidthCheckFirst, Line);
   Self->WidthCheckLast  = std::max(Self->WidthCheckLast, Line + std::max(0, LinesAdded));
   queue_repaint(Self); // Edits outside of the View do not invalidate it
}

static void check_modified_widths(extScintilla *Self)
{
   if (Self->WidthCheckLast < Self->WidthCheckFirst) return;

   const int last_line = SCICALL(SCI_GETLINECOUNT) - 1;
   const int first = std::clamp(Self->WidthCheckFirst, 0, last_line);
   const int last  = std::clamp(Self->WidthCheckLast, 0, last_line);
   Self->WidthCheckFirst = INT_MAX;
   Self->WidthCheckLast  = -1;
   if ((Self->Wordwrap) or (not Self->FontHandle)) return;

   int line;
   int width = measure_widest_line(Self, first, last, line);
   grow_scroll_width(Self, width, line);
}

//********************************************************************************************************************
// The tick and idle timers end their own subscriptions when they are no longer required.  See UpdateTimers().

static ERR tick_timer(extScintilla *Self, int64_t, int64_t)
{
   if ((Self->API) and (Self->API->panTick())) return ERR::Okay;
   Self->TickTimer = nullptr;
   return ERR::Terminate;
}

static ERR idle_timer(extScintilla *Self, int64_t, int64_t)
{
   if ((Self->API) and (Self->API->panIdle())) return ERR::Okay;
   Self->IdleTimer = nullptr;
   return ERR::Terminate;
}

//********************************************************************************************************************

#include "kt_drag.cxx"
#include "class_scintilla_ext.cxx"
#include "kt_classes.cxx"
#include "kt_window.cxx"
#include "kt_menu.cxx"
#include "kt_surface.cxx"
#include "kt_popup.cxx"
#include "kt_listbox.cxx"

//********************************************************************************************************************
// Paint the Dirty region of the client into the Bitmap, then ask the scene to redraw the Canvas.  This is called from
// the Repaint method, which is queued by invalidate_area().

static void paint_dirty(extScintilla *Self)
{
   if ((not Self->API) or (not Self->Bitmap)) return;

   check_modified_widths(Self); // Can widen the Page before it is synchronised
   update_margin_width(Self); // A change to the margin width extends the Dirty region
   Self->API->panSyncPage();  // The Canvas must be positioned for the topLine that is about to be painted
   if (not Self->Canvas) return; // The host viewport was freed by a client's feedback callback

   auto dirty = Self->Dirty;
   Self->Dirty = { 0, 0, 0, 0 };
   if ((dirty.Right <= dirty.Left) or (dirty.Bottom <= dirty.Top)) return;

   // Scintilla paints whole lines, which can extend beyond the Dirty region.  A disabled editor is always painted in
   // full so that the dimming overlay covers every repainted pixel exactly once.

   bool disabled = (Self->Flags & SCIF::DISABLED) != SCIF::NIL;
   if (disabled) dirty = { 0, 0, Self->ViewWidth, Self->ViewHeight };

   Self->API->panPaint(Scintilla::Internal::PRectangle(dirty.Left, dirty.Top, dirty.Right, dirty.Bottom));
   clear_bitmap_border(Self);

   if (disabled) {
      SurfacePan surface;
      surface.Init(Self->Bitmap, Self);
      auto &bkgd = Self->BkgdColour;
      surface.BlendRectangle(Scintilla::Internal::PRectangle(0, 0, Self->Bitmap->Width, Self->Bitmap->Height),
         Scintilla::Internal::ColourRGBA(SCICOLOUR(bkgd.Red, bkgd.Green, bkgd.Blue)), 160);
   }

   bitmap_modified(Self);
   if ((Self->Canvas) and (Self->Scene) and (Self->Scene->SurfaceID)) acDraw(Self->Canvas);
}

//********************************************************************************************************************

extScintilla::~extScintilla() {
   if (KeyHandle) { gfx::UnsubscribeKeyboard(KeyHandle); KeyHandle = 0; }
   if (TickTimer) { UpdateTimer(TickTimer, 0); TickTimer = nullptr; }
   if (IdleTimer) { UpdateTimer(IdleTimer, 0); IdleTimer = nullptr; }

   Drop.active = false;
   PrimaryDrop.active = false;

   if (Focus) {
      kt::SwitchContext context(this);
      Focus->subscribeFeedback(FM::NIL, C_FUNCTION(feedback_focus));
      // Keyboard callbacks are weak-pinned by Vector and retired when this context terminates.
      if (Focus != Viewport) UnsubscribeAction(Focus, AC::NIL);
   }

   if (Viewport) UnsubscribeAction(Viewport, AC::NIL);

   // Input callbacks are weak-pinned by Vector too.  Retire them through its dispatcher rather than invalidating
   // the active subscription iterator when a margin callback destroys the editor.

   free_context_menu(this);

   // Callbacks consult API; clear it before engine cancellation can close auxiliary windows.

   auto api = std::exchange(API, nullptr);
   if (DragClipboard) { FreeResource(DragClipboard); DragClipboard = nullptr; }

   replace_callback(FileDrop, nullptr);

   if (api) {
      while (ClientUndoDepth > 0) {
         api->SendScintilla(SCI_ENDUNDOACTION);
         ClientUndoDepth--;
      }
      api->Finalise();
   }
   delete api;

   replace_callback(EventCallback, nullptr);

   replace_callback(MarginClick, nullptr);

   replace_callback(CompletionCallback, nullptr);

   replace_callback(ChangeCallback, nullptr);

   if (Image) { // Freeing the image also removes its definition from the scene
      UnsubscribeAction(Image, AC::NIL);
      FreeResource(Image);
   }

   if (View) FreeResource(View); // The Page and Canvas are children of the View

   if (FileStream)  FreeResource(FileStream);
   if (Bitmap)      FreeResource(Bitmap);
   if (FillColour)  FreeResource(FillColour);
   if (StrokeColour) FreeResource(StrokeColour);
   if (ThemeCacheID) FreeResource(ThemeCacheID);
}

/*********************************************************************************************************************

-METHOD-
SetKeywords: Replaces a keyword set of the active lexer.

SetKeywords() replaces the keyword list that the active #Lexer uses for the keyword set `Set`.  Lexers use keyword sets
to classify words, for instance as language keywords or type names, and the number of sets and their meaning depend
on the lexer.  Some structural lexers, such as Markdown and Diff, have no keyword sets.

`Keywords` is a complete list of words separated by spaces, and replaces the lexer's built-in default list.  An empty
string leaves the set empty; it does not restore the default list.

The replacement is stored for the active lexer only, and is reapplied whenever that lexer is selected again.  The
existing styles are invalidated and recalculated when the text is next drawn; call #Colourise() to apply the change
immediately.  The document, its undo history and #DocumentRevision are not affected.

-INPUT-
int Set: Zero-based index of the keyword set to replace.
strview Keywords: Space-separated list of words to use for the set.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The editor is dispatching a notification and cannot be modified until it returns.
NoSupport: The active lexer does not provide keyword set `Set`.
Args: `Keywords` contains a null byte.

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR SCINTILLA_SetKeywords(extScintilla *Self, struct sci::SetKeywords *Args)
{
   if (not Args) return ERR::NullArgs;

   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;

   if (not has_keyword_set(Self, Args->Set)) return ERR::NoSupport;

   if (Args->Keywords.find('\0') != std::string_view::npos) return ERR::Args;

   std::string text(Args->Keywords);
   Self->LexerSettings[Self->Lexer].keywords[Args->Set] = text;
   Self->ActiveKeywords[Args->Set] = text;
   SCICALL(SCI_SETKEYWORDS, Args->Set, text.c_str());
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
SetLexerProperty: Sets a configuration property of the active lexer.

SetLexerProperty() assigns a value to a property advertised by the active #Lexer.  Lexer properties adjust how text
is classified, for example `lexer.json.allow.comments` enables comment recognition in the JSON lexer.  The available
properties depend on the lexer.  The `fold` and `asp.default.language` properties are managed by the Scintilla class
and cannot be set with this method.

Values are validated against the property's type.  Boolean properties accept only `0` or `1`, and integer properties
accept only a decimal integer.  String properties accept any value that does not contain a null byte.

The value is stored for the active lexer only.  It is removed from the editor when a different lexer is selected, and
is reapplied whenever the lexer is selected again.  The current value can be read with #GetLexerProperty().  The
existing styles are invalidated and recalculated when the text is next drawn; call #Colourise() to apply the change
immediately.

-INPUT-
strview Name: The exact name of the property, as advertised by the active lexer.
strview Value: The value to assign.

-ERRORS-
Okay
NullArgs
NotInitialised
Busy: The editor is dispatching a notification and cannot be modified until it returns.
NoSupport: `Name` is not a property advertised by the active lexer, or is reserved.
Args: `Name` or `Value` contains a null byte, or `Value` is not valid for the property's type.

-TAGS-
mutates-object, copies-input

*********************************************************************************************************************/

static ERR SCINTILLA_SetLexerProperty(extScintilla *Self, struct sci::SetLexerProperty *Args)
{
   if (not Args) return ERR::NullArgs;
   if (auto error = config_ready(Self, true); error != ERR::Okay) return error;

   std::string name(Args->Name), value(Args->Value);
   if ((name.find('\0') != std::string::npos) or (value.find('\0') != std::string::npos)) return ERR::Args;
   if (not has_lexer_property(Self, name)) return ERR::NoSupport;

   auto type = SCICALL(SCI_PROPERTYTYPE, uptr_t(name.c_str()));
   if (type IS SC_TYPE_BOOLEAN) {
      if ((value != "0") and (value != "1")) return ERR::Args;
   }
   else if (type IS SC_TYPE_INTEGER) {
      int number;
      auto result = std::from_chars(value.data(), value.data() + value.size(), number);
      if ((result.ec != std::errc()) or (result.ptr != value.data() + value.size())) return ERR::Args;
   }

   Self->LexerSettings[Self->Lexer].properties[name] = value;
   SCICALL(SCI_SETPROPERTY, name.c_str(), value.c_str());
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
LexerName: Selects the lexer by name.

LexerName is a name-based alternative to the #Lexer field.  Reading it returns the canonical name of the active lexer,
and writing it selects the lexer with the matching name, with the same effect as setting #Lexer.  The list of valid
names can be read from #SupportedLexers.  Names are case-sensitive.

The aliases `lua`, `hypertext` and `none` are accepted for `tiri`, `html` and `null` respectively, but the field always
reports the canonical name.  Writing an unknown name fails with `ERR::NoSupport` and leaves the active lexer unchanged.
The lexer cannot be changed while the editor is dispatching a notification.

-TAGS-
static-result, null-terminated-result

*********************************************************************************************************************/

static ERR GET_LexerName(extScintilla *Self, std::string_view &Value)
{
   auto profile = find_lexer_profile(Self->Lexer);
   if (not profile) return ERR::NoSupport;
   Value = lexer_name(*profile);
   return ERR::Okay;
}

static ERR SET_LexerName(extScintilla *Self, const std::string_view &Value)
{
   auto name = Value;

   if (name IS "lua") name = "tiri";
   else if (name IS "hypertext") name = "html";
   else if (name IS "none") name = "null";

   if (Self->API and Self->API->panInNotification()) return ERR::Busy;

   for (const auto &profile : glLexerProfiles) {
      if (name IS lexer_name(profile)) return SET_Lexer(Self, profile.ID);
   }
   return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
SupportedLexers: Lists the names of all supported lexers.  This field is read-only.

Returns the canonical name of every lexer that can be assigned to #LexerName, separated by newline characters.  The
list includes `null`, which selects plain text.  The content of the list is fixed for the lifetime of the process.

-TAGS-
static-result, null-terminated-result

*********************************************************************************************************************/

static ERR GET_SupportedLexers(extScintilla *, std::string_view &Value)
{
   static const std::string names = []() {
      std::string result;
      for (const auto &profile : glLexerProfiles) {
         if (not result.empty()) result += '\n';
         result += lexer_name(profile);
      }
      return result;
   }();
   Value = names;
   return ERR::Okay;
}

#include "class_scintilla_def.cxx"

static const FieldArray clFields[] = {
   { "TextStyle",      FDF_CPPSTRING|FDF_RW, nullptr, SET_TextStyle },
   { "Path",           FDF_CPPSTRING|FDF_RW, nullptr, SET_Path },
   { "EventFlags",     FDF_INT|FDF_FLAGS|FDF_RW, nullptr, nullptr, &clScintillaEventFlags },
   { "Flags",          FDF_INTFLAGS|FDF_RI, nullptr, nullptr, &clScintillaFlags },
   { "Viewport",       FDF_OBJECT|FDF_RI, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Focus",          FDF_OBJECT|FDF_RI, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "View",           FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "Page",           FDF_OBJECT|FDF_R, nullptr, nullptr, CLASSID::VECTORVIEWPORT },
   { "LeftMargin",     FDF_INT|FDF_RW, nullptr, SET_LeftMargin },
   { "RightMargin",    FDF_INT|FDF_RW, nullptr, SET_RightMargin },
   { "CursorRow",      FDF_INT|FDF_R, GET_CursorRow },
   { "CursorCol",      FDF_INT|FDF_R, GET_CursorCol },
   { "Lexer",          FDF_INT|FDF_LOOKUP|FDF_RW, nullptr, SET_Lexer, &clScintillaLexer },
   { "Modified",       FDF_INT|FDF_RW, nullptr, SET_Modified },
   // Virtual fields
   { "ThemeXML",           FDF_OBJECT|FDF_RW|FDF_PURE, GET_ThemeXML, SET_ThemeXML, CLASSID::XML },
   { "ActivePopup",        FDF_INT|FDF_LOOKUP|FDF_R,  GET_ActivePopup, nullptr, &clScintillaSCPOP },
   { "AllowTabs",          FDF_INT|FDF_RW|FDF_PURE,   GET_AllowTabs, SET_AllowTabs },
   { "AutoIndent",         FDF_INT|FDF_RW|FDF_PURE,   GET_AutoIndent, SET_AutoIndent },
   { "CompletionCallback", FDF_FUNCTION|FDF_RW|FDF_PURE, GET_CompletionCallback, SET_CompletionCallback },
   { "FileDrop",           FDF_FUNCTION|FDF_RW|FDF_PURE, GET_FileDrop, SET_FileDrop },
   { "FoldingMarkers",     FDF_INT|FDF_RW|FDF_PURE,   GET_FoldingMarkers, SET_FoldingMarkers },
   { "LineCount",          FDF_INT|FDF_R,             GET_LineCount },
   { "LineNumbers",        FDF_INT|FDF_RW|FDF_PURE,   GET_LineNumbers, SET_LineNumbers },
   { "MarginClick",        FDF_FUNCTION|FDF_RW|FDF_PURE, GET_MarginClick, SET_MarginClick },
   { "Origin",             FDF_CPPSTRING|FDF_W, nullptr, SET_Origin },
   { "ShowWhitespace",     FDF_INT|FDF_RW|FDF_PURE,      GET_ShowWhitespace, SET_ShowWhitespace },
   { "EventCallback",      FDF_FUNCTION|FDF_RW|FDF_PURE, GET_EventCallback, SET_EventCallback },
   { "String",             FDF_CPPSTRING|FDF_RW,      GET_String, SET_String },
   { "Symbols",            FDF_INT|FDF_RW|FDF_PURE,   GET_Symbols, SET_Symbols },
   { "TabWidth",           FDF_INT|FDF_RW|FDF_PURE,   GET_TabWidth, SET_TabWidth },
   { "Wordwrap",           FDF_INT|FDF_RW|FDF_PURE,   GET_Wordwrap, SET_Wordwrap },
   { "CurrentPos",         FDF_INT|FDF_R, GET_CurrentPos },
   { "Anchor",             FDF_INT|FDF_R, GET_Anchor },
   { "SelectionStart",     FDF_INT|FDF_R, GET_SelectionStart },
   { "SelectionEnd",       FDF_INT|FDF_R, GET_SelectionEnd },
   { "TextLength",         FDF_INT|FDF_R, GET_TextLength },
   { "CanUndo",            FDF_INT|FDF_R, GET_CanUndo },
   { "CanRedo",            FDF_INT|FDF_R, GET_CanRedo },
   { "SelectedText",       FDF_CPPSTRING|FDF_R, GET_SelectedText },
   { "ChangeCallback",     FDF_FUNCTION|FDF_RW|FDF_PURE, GET_ChangeCallback, SET_ChangeCallback },
   { "DocumentRevision",   FDF_INT64|FDF_R, GET_DocumentRevision },
   { "LexerName",          FDF_CPPSTRING|FDF_RW, GET_LexerName, SET_LexerName },
   { "SupportedLexers",    FDF_CPPSTRING|FDF_R, GET_SupportedLexers },
   { "EOLMode",            FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, GET_EOLMode, SET_EOLMode, &clScintillaSCEOL },
   { "IndentWidth",        FDF_INT|FDF_RW|FDF_PURE, GET_IndentWidth, SET_IndentWidth },
   { "IndentationGuides",  FDF_INT|FDF_LOOKUP|FDF_RW|FDF_PURE, GET_IndentationGuides, SET_IndentationGuides, &clScintillaSCGUIDE },
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
