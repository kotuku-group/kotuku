
class objScintilla;
class ScintillaKTK;

#include <kotuku/modules/scintilla.h>
#include "../link/unicode.h"

using namespace pf;

class extScintilla : public objScintilla {
   public:
   struct  SurfaceCoords Surface;
   FUNCTION FileDrop;
   FUNCTION EventCallback;
   objFile *FileStream;
   objFont *BoldFont;        // Bold version of the current font
   objFont *ItalicFont;      // Italic version of the current font
   objFont *BIFont;          // Bold-Italic version of the current font
   ScintillaKTK *API;
   APTR   prvKeyEvent;
   STRING StringBuffer;
   int    LongestLine;         // Longest line in the document
   int    LongestWidth;        // Pixel width of the longest line
   int    TabWidth;
   int    InputHandle;
   TIMER  TimerID;
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
   uint8_t  ScrollLocked;
};

// This class inherits from ScintillaBase which inherits from Editor.  Responsible for a lot of the editing code.

class ScintillaKTK : public Scintilla::ScintillaBase {
public:
   ScintillaKTK(int SurfaceID, extScintilla *Scintilla);
   virtual ~ScintillaKTK();
   virtual void Initialise() {};
   virtual void Finalise();
//    virtual void RefreshColourPalette(Palette &pal, bool want);
//    virtual void AddCharUTF(char *s, unsigned int len, bool treatAsDBCS=false);
//    virtual void CancelModes();
//    virtual int KeyCommand(unsigned int iMessage);
   virtual void CreateCallTipWindow(Scintilla::PRectangle rc);
   virtual void AddToPopUp(const char *label, int cmd=0, bool enabled=true);
//    virtual void ButtonDown(Point pt, unsigned int curTime, bool shift, bool ctrl, bool alt);
//    virtual void NotifyStyleToNeeded(int endStyleNeeded);
   virtual void SetVerticalScrollPos();
   virtual void SetHorizontalScrollPos();
   virtual bool ModifyScrollBars(int nMax, int nPage);
   virtual void ReconfigureScrollBars();
   virtual void Cut();
   virtual void Copy();
   virtual void Paste();
   virtual void ClaimSelection();
   virtual void NotifyChange();
   // Called when Scintilla wants us to send a message to the parent of this widget
   virtual void NotifyParent(Scintilla::SCNotification scn);
   virtual void CopyToClipboard(const Scintilla::SelectionText &selectedText);
   virtual void ScrollText(int linesToMove);
   virtual void SetTicking(bool on);
   virtual void SetMouseCapture(bool on);
   virtual bool HaveMouseCapture();
   virtual sptr_t DefWndProc(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
   static sptr_t DirectFunction(ScintillaKTK *sci, unsigned int iMessage, uptr_t wParam, sptr_t lParam);
   // Non-abstract virtual methods from Editor
   virtual int KeyDefault(int key, int modifiers);

   // Custom interfacing methods

   virtual void SetStyles(const struct styledef *Def, int Total);
   virtual void panDraw(objSurface *, objBitmap *Bitmap);
   virtual void panFontChanged(void *Font, void *BoldFont, void *ItalicFont, void *BIFont);
   virtual void panIdleEvent();
   virtual void panKeyDown(int Key, KQ);
   virtual void panMousePress(JET ButtonFlags, double x, double y);
   virtual void panMouseRelease(JET ButtonFlags, double x, double y);
   virtual void panMouseMove(double x, double y);
   virtual void panResized();
   virtual void panScrollToX(double x);
   virtual void panScrollToY(double y);
   virtual void panGotFocus();
   virtual void panLostFocus();
   virtual void panGetCursorPosition(int *line, int *index);
   virtual void panSetCursorPosition(int line, int index);
   virtual void panEnsureLineVisible(int line);
   virtual void panWordwrap(int);

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

   char   lastkeytrans[7];

private:
   void SetupStyles();
   //void SetSelectedTextStyle(int style);
   void NotifyKey(int key, int modifiers);

   int braceMode;
   int oldpos = -1;
   Scintilla::ElapsedTime timer;
   int8_t idle_timer_on:1;
   int8_t ticking_on:1;
   int8_t captured_mouse:1;
   double lastticktime;
   extScintilla *scintilla;
   OBJECTID surfaceid;
};
