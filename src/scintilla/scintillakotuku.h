class objScintilla;
class ScintillaKTK;
class SurfacePan;

#include <kotuku/modules/scintilla.h>
#include "../link/unicode.h"

using namespace kt;

#include "adapter_types.h"

class extScintilla : public objScintilla {
   public:
   // The palette is configured and read back through ThemeXML.
   RGB8 BkgdColour;
   RGB8 TextColour;
   RGB8 CursorColour;
   RGB8 LineHighlight;
   RGB8 SelectFore;
   RGB8 SelectBkgd;
   RGB8 CommentColour;
   RGB8 KeywordColour;
   RGB8 StringColour;
   RGB8 NumberColour;
   RGB8 OperatorColour;
   RGB8 PreprocessorColour;
   RGB8 TypeColour;
   RGB8 ErrorColour;
   RGB8 MarginColour;
   RGB8 MarginBkgd;
   // Derived colours follow a source colour of the palette until they are set explicitly (see glThemeColours)
   RGB8 Whitespace;
   RGB8 IndentGuide;
   RGB8 BraceMatch;
   RGB8 BraceMismatch;
   RGB8 BraceBkgd;
   RGB8 SelectInactiveBkgd;
   RGB8 SelectAdditionalBkgd;
   uint32_t ThemeOverrides = 0; // Bit per glThemeColours entry; set when a derived colour no longer inherits
   OBJECTID ThemeCacheID = 0; // XML object returned when ThemeXML is read; rewritten on each read

   struct pending_drop {
      OBJECTID source = 0;
      int item = 0;
      int position = 0;
      int64_t revision = 0;
      bool active = false;
   } Drop, PrimaryDrop;
   int64_t DocumentRevision = 0;
   int ClientUndoDepth = 0;
   int PrimaryItem = 0;
   objClipboard *DragClipboard = nullptr;
   std::string DragText;
   FUNCTION FileDrop;
   FUNCTION EventCallback;
   FUNCTION MarginClick;
   FUNCTION CompletionCallback;
   FUNCTION ChangeCallback;
   objFile *FileStream;
   APTR FontHandle = nullptr;        // Shared vector module handle for the TextStyle
   std::string Face;                 // The family declared by TextStyle
   int FontSize = 14;                // The pixel size declared by TextStyle
   objVectorScene *Scene;        // The scene that hosts the Viewport
   objVectorRectangle *Canvas;   // Displays the Bitmap within the Page
   objVectorImage *Image;        // Scene definition that wraps the Bitmap for use as the Canvas fill
   objBitmap *Bitmap;            // Scintilla paints into this bitmap; it covers the visible area of the View
   objVectorColour *FillColour;  // Solid fill for shapes drawn with vec::DrawPath()
   objVectorColour *StrokeColour; // Solid stroke for shapes drawn with vec::DrawPath()
   objScript *MenuScript;        // Hosts the context menu with gui/menu (see kt_menu.cxx)
   ClipRectangle Dirty;          // Area of the client that is waiting to be repainted
   double Scale = 1.0;           // Device pixels per unit of the View.  Scintilla's client units are device pixels
   double PageScale = 1.0;       // Device scale used when the Page's dimensions were last set
   double HostScale = 1.0;       // Scale of the View in the scene, see update_host_transform()
   double HostX = 0;             // Scene position of the View's origin
   double HostY = 0;
   double CanvasX;               // Sub-pixel offset of the Canvas (in View units) that aligns the Bitmap with whole
   double CanvasY;               //   device pixels
   double PointerX;              // Client position of the pointer, as of the most recent movement event
   double PointerY;
   double MenuX;                 // Scene position at which the context menu was requested
   double MenuY;
   int    ScrollX;             // Scroll position reflected by the Page, i.e. Scintilla's xOffset in client pixels
   int    ScrollY;             // Scroll position reflected by the Page, i.e. Scintilla's topLine in client pixels
   int    PageWidth;           // Extent of the Page in client pixels, as computed by ModifyScrollBars()
   int    PageHeight;
   ScintillaKTK *API;
   std::string StringBuffer;
   std::string SelectionBuffer;
   struct lexer_settings {
      std::map<int, std::string> keywords;
      std::map<std::string, std::string> properties;
   };
   std::map<SCLEX, lexer_settings> LexerSettings;
   std::map<int, std::string> ActiveKeywords;
   struct indicator_definition {
      SCIND style;
      int colour; // RGB, or -1 ErrorColour, -2 KeywordColour, -3 SelectBkgd; see theme_reference()
   };
   std::map<int, indicator_definition> Indicators;
   struct marker_definition {
      int symbol;
      int fore;  // RGB, or a theme reference as for indicator_definition
      int bkgd;
      int alpha; // Applies to bkgd
   };
   std::map<int, marker_definition> Markers;
   int    LongestLine;         // Longest line in the document
   int    LongestWidth;        // Scroll width: the pixel width of the longest line, plus padding
   int    WidthCheckFirst = INT_MAX; // Range of modified lines to measure before the next paint
   int    WidthCheckLast = -1;
   SCEOL EOLMode = SCEOL::LF;
   SCGUIDE IndentationGuides = SCGUIDE::NONE;
   int IndentWidth = 0;
   int    TabWidth;
   int    ViewWidth;           // Width of the View in client pixels, i.e. Scintilla's client area
   int    ViewHeight;          // Height of the View in client pixels
   int    MarginDigits;        // Number of digits that the line number margin is sized for
   TIMER  TickTimer;           // Runs the independent fine-ticker deadlines
   TIMER  IdleTimer;           // Calls Idle() while Scintilla has background work, e.g. word wrapping
   int    KeyHandle = 0; // Subscription to all keyboard events, which track the modifier keys
   SEF    ReportEventFlags;    // For delayed event reporting.
   uint16_t KeyAlt:1;
   uint16_t KeyCtrl:1;
   uint16_t KeyShift:1;
   uint16_t LineNumbers:1;
   uint16_t Wordwrap:1;
   uint16_t Symbols:1;
   uint16_t FoldingMarkers:1;
   uint16_t ShowWhitespace:1;
   uint16_t AutoIndent:1;
   uint16_t HoldModify:1;
   uint16_t AllowTabs:1;
   uint16_t RepaintQueued:1;   // A Repaint message is queued for the Dirty area
   uint16_t HasFocus:1;        // The Focus viewport, or one of its children, has the user focus
   uint16_t PointerStale:1;    // The pointer entered the Canvas without a movement event, so PointerX/Y are outdated
   uint16_t MenuOpen:1;        // The context menu is visible
   uint16_t HostUniform:1;     // The View is unrotated and scaled equally on both axes
   uint16_t ScalableFace:1;    // The Face is scalable, so the fonts follow the device scale
   uint8_t  ScrollLocked;      // Set while the editor is scrolling in response to a Page change

   extScintilla(objMetaClass *ClassPtr, OBJECTID ObjectID) : objScintilla(ClassPtr, ObjectID) {
      Face = "Source Code Pro";
      TextStyle = "face=Source Code Pro; size=14px";
      LeftMargin  = 4;
      RightMargin = 30;
      AutoIndent  = TRUE;
      TabWidth    = 8;
      AllowTabs   = FALSE;

      BkgdColour    = RGB8 { 255, 255, 255, 255 };
      TextColour    = RGB8 { 0, 0, 0, 255 };
      CommentColour = RGB8 { 128, 128, 128, 255 };
      KeywordColour = RGB8 { 178, 34, 34, 255 };
      StringColour  = RGB8 { 176, 0, 0, 255 };
      NumberColour  = RGB8 { 0, 0, 255, 255 };
      OperatorColour = RGB8 { 0, 0, 0, 255 };
      PreprocessorColour = RGB8 { 34, 139, 34, 255 };
      TypeColour    = RGB8 { 148, 0, 211, 255 };
      ErrorColour   = RGB8 { 255, 0, 0, 255 };
      MarginColour  = RGB8 { 128, 128, 128, 255 };
      MarginBkgd    = RGB8 { 224, 224, 224, 255 };
      LineHighlight = RGB8 { 240, 240, 255, 255 };
      CursorColour  = RGB8 { 0, 0, 0, 255 };
      SelectFore    = RGB8 { 255, 255, 255, 255 };
      SelectBkgd    = RGB8 { 0, 0, 180, 255 };
      Whitespace    = MarginColour;
      IndentGuide   = MarginColour;
      BraceMatch    = KeywordColour;
      BraceMismatch = ErrorColour;
      BraceBkgd     = MarginBkgd;
      SelectInactiveBkgd   = SelectBkgd;
      SelectAdditionalBkgd = SelectBkgd;
   }

   // Resolves a colour argument of DefineIndicator() or DefineMarker() to 0xRRGGBB.  Negative values refer to the
   // theme: -1 error, -2 keyword, -3 selectionBackground.

   int theme_reference(int Colour) const {
      if (Colour >= 0) return Colour;
      auto &rgb = (Colour IS -1) ? ErrorColour : ((Colour IS -2) ? KeywordColour : SelectBkgd);
      return (rgb.Red << 16) | (rgb.Green << 8) | rgb.Blue;
   }

   ~extScintilla();
};

/*********************************************************************************************************************
** Code assistance pop-ups, i.e. the call tip and the completion list, are Scintilla platform Windows that are displayed
** as separate viewports in the editor's scene.  The WindowID of a pop-up refers to its PanPopup, while the
** WindowID of the editor refers to the extScintilla object.  See kt_popup.cxx.
*/

class PanPopup {
public:
   extScintilla *scintilla = nullptr;
   objVectorViewport *viewport = nullptr; // Positions the pop-up in scene coordinates
   objVectorRectangle *canvas = nullptr;  // Displays the bitmap and receives pointer input
   objVectorImage *image = nullptr;       // Scene definition that wraps the bitmap for use as the canvas fill
   objBitmap *bitmap = nullptr;           // The content of the pop-up is painted into this bitmap
   Scintilla::Internal::PRectangle rect;            // Requested position, in the client coordinates of the editor
   Scintilla::Internal::Point anchor;               // Client position of the anchor when the rect was set
   double scene_x = 0, scene_y = 0;       // Position of the pop-up in the scene
   bool shown = false;
   bool dirty = false;                    // The bitmap needs to be repainted

   PanPopup();
   PanPopup(const PanPopup &) = delete;
   PanPopup & operator=(const PanPopup &) = delete;
   virtual ~PanPopup();

   virtual void paint(SurfacePan &Surface) = 0;
   virtual void input(const InputEvent *Event) = 0;
   virtual int anchor_position() = 0;     // Document position that the pop-up is attached to
   virtual void closed() { }              // Called when Scintilla closes the pop-up

   ERR attach(extScintilla *Scintilla, CSTRING Name);
   void set_position(Scintilla::Internal::PRectangle Rect);
   void show(bool Show);
   void invalidate();
   bool place();
   void redraw();
   void release();

   static PanPopup * find(Scintilla::Internal::WindowID ID);
};

// This class inherits from ScintillaBase which inherits from Editor.  Responsible for a lot of the editing code.

class ScintillaKTK : public Scintilla::Internal::ScintillaBase {
public:
   ScintillaKTK(extScintilla *Scintilla);
   ~ScintillaKTK() override;
   void Initialise() override {};
   void Finalise() override;
   void CreateCallTipWindow(Scintilla::Internal::PRectangle Rect) override;
   void AddToPopUp(const char *Label, int Command=0, bool Enabled=true) override;
   void SetVerticalScrollPos() override;
   void SetHorizontalScrollPos() override;
   bool ModifyScrollBars(Sci::Line Max, Sci::Line Page) override;
   void ReconfigureScrollBars() override;
   void Copy() override;
   void Paste() override;
   void ClaimSelection() override;
   void StartDrag() override;
   void InsertCharacter(std::string_view Text, Scintilla::CharacterSource Source) override;
   ERR panCheckLength(size_t Added, Sci::Position Removed = 0) const {
      return KotukuScintilla::DocumentGrowth(pdoc->Length(), Removed, Added) ? ERR::Okay : ERR::OutOfRange;
   }
   bool panPublicDocument() const {
      return KotukuScintilla::DocumentGrowth(pdoc->Length(), 0, 0);
   }
   bool panInNotification() const { return notification_depth > 0; }
   void panRefreshFolds() { pdoc->EnsureStyledTo(pdoc->Length()); }
   ERR panConvertEOLs(SCEOL Mode);
   ERR panReplaceRange(int Start, int End, std::string_view Text);
   void panEndDrag();
   bool panDragging() const { return drag_active; }
   int panDropPosition(double X, double Y);
   ERR panDropText(int Position, std::string_view Text, extScintilla *Source = nullptr);
   void panPrimaryPaste(double X, double Y);
   void NotifyChange() override;
   // Called when Scintilla wants us to send a message to the parent of this widget
   void NotifyParent(Scintilla::NotificationData Notification) override;
   void CopyToClipboard(const Scintilla::Internal::SelectionText &Text) override;
   void ScrollText(Sci::Line LinesToMove) override;
   bool FineTickerRunning(TickReason Reason) override;
   void FineTickerStart(TickReason Reason, int Milliseconds, int Tolerance) override;
   void FineTickerCancel(TickReason Reason) override;
   bool SetIdle(bool On) override;
   void SetMouseCapture(bool On) override;
   void DisplayCursor(Scintilla::Internal::Window::Cursor Cursor) override;
   bool HaveMouseCapture() override;
   sptr_t DefWndProc(Scintilla::Message Message, uptr_t WParam, sptr_t LParam) override;
   static sptr_t DirectFunction(sptr_t Context, unsigned int Message, uptr_t WParam, sptr_t LParam);

   // Custom interfacing methods

   virtual void SetStyleColours();
   virtual void panPaint(Scintilla::Internal::PRectangle Area);
   virtual void panRedraw();
   virtual void panFontChanged();
   virtual bool panTick();
   virtual bool panIdle();
   virtual void panKeyDown(KEY Code, KQ Qualifiers, int Unicode);
   virtual void panMousePress(JET Button, double X, double Y, unsigned int Time);
   virtual void panMouseRelease(JET Button, double X, double Y, unsigned int Time);
   virtual void panMouseMove(double X, double Y);
   virtual void panReleaseMouse();
   virtual void panResized();
   void panRescaled(double Ratio);
   virtual void panScrollTo(double X, double Y);
   virtual void panScrollBy(int Columns, int Lines);
   virtual void panSyncPage();
   virtual void panSetFocus(bool Focus);
   virtual void panWordwrap(int);
   virtual void panSetFolding(bool Enable);
   virtual bool panToggleFold(int Line);
   ERR panClipboard(CLIPMODE Mode);
   void panContextMenu(double X, double Y, double SceneX, double SceneY);
   void panShowPopup();
   void panPopupCommand(int Command);
   ERR panShowCompletions(int Length, std::string_view Candidates, SCACF Flags);
   ERR panShowCallTip(int Pos, std::string_view Text, int HighlightStart, int HighlightEnd);
   void panCancelPopups();
   SCPOP panActivePopup();
   int panCompletionStart();
   int panCallTipStart() { return calltip_pos; }
   int panLineHeight() { return vs.lineHeight; }
   Scintilla::Internal::Point panLocation(int Pos) { return LocationFromPosition(Pos); }
   void panPaintCallTip(Scintilla::Internal::Surface *Surface) { ct.PaintCT(Surface); }
   void panCallTipClick(Scintilla::Internal::Point Point);
   bool panFlushCompletionReports();
   bool panFlushChangeReports();
   void panResetChangeReports();
   void panCompletionClosed();

   // Brace match handling
   virtual void braceMatch();
   virtual sptr_t checkBrace(sptr_t pos,int brace_style);
   virtual bool findMatchingBrace(sptr_t &brace,sptr_t &other,sptr_t mode);

   void ApplyIndicators();
   void ApplyMarkers();

   // Lexer handling
   virtual ERR SetLexer(SCLEX ID);

   // Message handling
   sptr_t SendScintilla(unsigned int Message, uptr_t WParam = 0, sptr_t LParam = 0) {
      return WndProc(Scintilla::Message(Message), WParam, LParam);
   }

   sptr_t SendScintilla(unsigned int Message, uptr_t WParam, const char *LParam) {
      return SendScintilla(Message, WParam, (sptr_t)LParam);
   }

   sptr_t SendScintilla(unsigned int Message, const char *LParam) {
      return SendScintilla(Message, 0, (sptr_t)LParam);
   }

   sptr_t SendScintilla(unsigned int Message, const char *WParam, const char *LParam) {
      return SendScintilla(Message, (uptr_t)WParam, (sptr_t)LParam);
   }

   sptr_t WndProc(Scintilla::Message Message, uptr_t WParam, sptr_t LParam) override;
   bool ValidCodePage(int CodePage) const override { return CodePage IS SC_CP_UTF8; }
   std::string UTF8FromEncoded(std::string_view Text) const override { return std::string(Text); }
   std::string EncodedFromUTF8(std::string_view Text) const override { return std::string(Text); }

private:
   void QueueChangeDelivery();
   void SetKeywords();
   void DefineFoldMarkers();
   void ExpandAllFolds();
   void FoldChanged(int Line, int LevelNow, int LevelPrev);
   void ReportCompletion(SCAC Event, int Position, int Value, CSTRING Text, bool Deferred);
   void UpdateTimers();
   bool TickRequired();
   ERR clipboard_failed(ERR Error, CSTRING Message);
   ERR copy_to_clipboard(const Scintilla::Internal::SelectionText &Text);
   ERR read_clipboard(std::string &Content);
   ERR cut_clipboard();
   ERR paste_clipboard();
   bool PopupCommandAllowed(int Command);

   struct popup_item {
      std::string label;
      int command;
      bool enabled;
      std::string qualifier;
      std::string key;
   };

   std::vector<popup_item> popup_items; // Context menu items, as added by ScintillaBase::ContextMenu()
   std::unique_ptr<PanPopup> calltip_popup; // Created by CreateCallTipWindow() on first use
   int calltip_pos = 0;                 // Document position that the call tip is attached to
   bool completion_selected = false;    // A completion has been accepted and the list is about to close

   struct completion_report {
      SCAC event;
      int position;
      int value;
      std::string text;
   };

   std::vector<completion_report> completion_reports; // Deferred CompletionCallback reports
   bool flushing_reports = false;

   std::vector<ScintillaChange> change_reports;
   uint64_t change_generation = 0;
   bool flushing_changes = false;
   bool change_report_queued = false;

   Scintilla::Internal::Selection drag_selection;
   int64_t drag_revision = 0;
   bool drag_active = false;
   int painted_top = -1;       // The topLine that the content of the Bitmap reflects, see ScrollText()
   int braceMode;
   Sci::Position oldpos = -1;
   bool finalised = false;
   int notification_depth = 0;
   struct ticker {
      int milliseconds = 0;
      int64_t deadline = 0;
   };
   std::array<ticker, 5> tickers;
   ERR clip_error = ERR::Okay; // Result of the most recent cut, copy or paste
   uint8_t idle_on:1;
   uint8_t captured_mouse:1;
   extScintilla *scintilla;
};

// Match Scintilla's exported C direct-call ABI, including on platforms with 32-bit long.
static_assert(std::is_same_v<decltype(&ScintillaKTK::DirectFunction), SciFnDirect>);
static_assert(sizeof(sptr_t) IS sizeof(void *));
static_assert(sizeof(uptr_t) IS sizeof(void *));
