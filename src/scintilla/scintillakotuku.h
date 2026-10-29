
class objScintilla;
class ScintillaKTK;

#include <kotuku/modules/scintilla.h>
#include "../link/unicode.h"

using namespace kt;

class extScintilla : public objScintilla {
   public:
   FUNCTION FileDrop;
   FUNCTION EventCallback;
   FUNCTION MarginClick;
   objFile *FileStream;
   APTR FontHandles[4] = {};         // Regular, bold, italic and bold italic; shared vector module handles
   objVectorScene *Scene;        // The scene that hosts the Viewport
   objVectorRectangle *Canvas;   // Displays the Bitmap within the Page
   objVectorImage *Image;        // Scene definition that wraps the Bitmap for use as the Canvas fill
   objBitmap *Bitmap;            // Scintilla paints into this bitmap; it covers the visible area of the View
   objVectorColour *FillColour;  // Solid fill for shapes drawn with vec::DrawPath()
   objVectorColour *StrokeColour; // Solid stroke for shapes drawn with vec::DrawPath()
   ClipRectangle Dirty;          // Area of the client that is waiting to be repainted
   double CanvasX;               // Sub-pixel offset of the Canvas that aligns the Bitmap with whole device pixels
   double CanvasY;
   double PointerX;              // Client position of the pointer, as of the most recent movement event
   double PointerY;
   int    ScrollX;             // Scroll position reflected by the Page, i.e. Scintilla's xOffset in pixels
   int    ScrollY;             // Scroll position reflected by the Page, i.e. Scintilla's topLine in pixels
   int    PageWidth;           // Extent of the Page, as computed by ModifyScrollBars()
   int    PageHeight;
   ScintillaKTK *API;
   std::string StringBuffer;
   int    LongestLine;         // Longest line in the document
   int    LongestWidth;        // Pixel width of the longest line
   int    TabWidth;
   int    ViewWidth;           // Pixel width of the View, i.e. Scintilla's client area
   int    ViewHeight;          // Pixel height of the View
   int    MarginDigits;        // Number of digits that the line number margin is sized for
   TIMER  TickTimer;           // Calls Tick() while the caret is blinking
   TIMER  IdleTimer;           // Calls Idle() while Scintilla has background work, e.g. word wrapping
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
   uint8_t  ScrollLocked;      // Set while the editor is scrolling in response to a Page change

   extScintilla(objMetaClass *ClassPtr, OBJECTID ObjectID) : objScintilla(ClassPtr, ObjectID) {
      Face = "Source Code Pro";
      FontSize = 14;
      LeftMargin  = 4;
      RightMargin = 30;
      AutoIndent  = TRUE;
      TabWidth    = 8;
      AllowTabs   = FALSE;

      BkgdColour    = RGB8 { 255, 255, 255, 255 };
      LineHighlight = RGB8 { 240, 240, 255, 255 };
      CursorColour  = RGB8 { 0, 0, 0, 255 };
      SelectFore    = RGB8 { 255, 255, 255, 255 };
      SelectBkgd    = RGB8 { 0, 0, 180, 255 };
   }

   ~extScintilla();
};

// This class inherits from ScintillaBase which inherits from Editor.  Responsible for a lot of the editing code.

class ScintillaKTK : public Scintilla::ScintillaBase {
public:
   ScintillaKTK(extScintilla *Scintilla);
   virtual ~ScintillaKTK();
   virtual void Initialise() {};
   virtual void Finalise();
//    virtual void RefreshColourPalette(Palette &pal, bool want);
//    virtual void AddCharUTF(char *s, unsigned int len, bool treatAsDBCS=false);
//    virtual void CancelModes();
//    virtual int KeyCommand(unsigned int iMessage);
   virtual void CreateCallTipWindow(Scintilla::PRectangle rc);
   virtual void AddToPopUp(const char *label, int cmd=0, bool enabled=true);
//    virtual void NotifyStyleToNeeded(int endStyleNeeded);
   virtual void SetVerticalScrollPos();
   virtual void SetHorizontalScrollPos();
   virtual bool ModifyScrollBars(int nMax, int nPage);
   virtual void ReconfigureScrollBars();
   virtual void Copy();
   virtual void Paste();
   virtual void ClaimSelection();
   virtual void NotifyChange();
   // Called when Scintilla wants us to send a message to the parent of this widget
   virtual void NotifyParent(Scintilla::SCNotification scn);
   virtual void CopyToClipboard(const Scintilla::SelectionText &selectedText);
   virtual void ScrollText(int linesToMove);
   virtual void SetTicking(bool on);
   virtual bool SetIdle(bool on);
   virtual void SetMouseCapture(bool on);
   virtual void DisplayCursor(Scintilla::Window::Cursor Cursor);
   virtual bool HaveMouseCapture();
   virtual sptr_t DefWndProc(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
   static sptr_t DirectFunction(ScintillaKTK *sci, unsigned int iMessage, uptr_t wParam, sptr_t lParam);

   // Custom interfacing methods

   virtual void SetStyles(const struct styledef *Def, int Total);
   virtual void SetStyleColours();
   virtual void panPaint(Scintilla::PRectangle Area);
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
   virtual void panScrollTo(double X, double Y);
   virtual void panScrollBy(int Columns, int Lines);
   virtual void panSyncPage();
   virtual void panSetFocus(bool Focus);
   virtual void panGetCursorPosition(int *line, int *index);
   virtual void panSetCursorPosition(int line, int index);
   virtual void panEnsureLineVisible(int line);
   virtual void panWordwrap(int);
   virtual void panSetFolding(bool Enable);
   virtual bool panToggleFold(int Line);
   ERR panClipboard(CLIPMODE Mode);

   // Brace match handling
   virtual void braceMatch();
   virtual long checkBrace(long pos,int brace_style);
   virtual bool findMatchingBrace(long &brace,long &other,long mode);
   virtual void gotoMatchingBrace(bool select);
   virtual void moveToMatchingBrace();
   virtual void selectToMatchingBrace();

   // Lexer handling
   virtual void SetLexerLanguage(const char *languageName);
   virtual void SetLexer(uptr_t wParam);

   // Message handling
   long SendScintilla(unsigned int msg,unsigned long wParam = 0, long lParam = 0) {
      return WndProc(msg, wParam, lParam);
   }

   long SendScintilla(unsigned int msg,unsigned long wParam, const char *lParam) {
      return WndProc(msg,wParam,reinterpret_cast<sptr_t>(lParam));
   }

   long SendScintilla(unsigned int msg,const char *lParam) {
      return WndProc(msg,0UL,reinterpret_cast<sptr_t>(lParam));
   }

   long SendScintilla(unsigned int msg,const char *wParam, const char *lParam) {
       return WndProc(msg,reinterpret_cast<uptr_t>(wParam),reinterpret_cast<sptr_t>(lParam));
   }

public:
   // Public so scintilla_send_message can use it
   virtual sptr_t WndProc(unsigned int iMessage, uptr_t wParam, sptr_t lParam);

private:
   void SetupStyles();
   void DefineFoldMarkers();
   void ExpandAllFolds();
   void FoldChanged(int Line, int LevelNow, int LevelPrev);
   void NotifyKey(int key, int modifiers);
   void UpdateTimers();
   bool TickRequired();
   ERR clipboard_failed(ERR Error, CSTRING Message);
   ERR copy_to_clipboard(const Scintilla::SelectionText &Text);
   ERR read_clipboard(std::string &Content);
   ERR cut_clipboard();
   ERR paste_clipboard();

   int braceMode;
   int oldpos = -1;
   ERR clip_error = ERR::Okay; // Result of the most recent cut, copy or paste
   uint8_t idle_on:1;
   uint8_t ticking_on:1;
   uint8_t captured_mouse:1;
   extScintilla *scintilla;
};
