
int GetFontHeight(void *TheFont)
{
   //log.trace("GetFontHeight()");
   return static_cast<objFont *>(TheFont)->MaxHeight;
}

int GetFontLeading(void *TheFont)
{
   //log.trace("GetFontLeading()");
   return static_cast<objFont *>(TheFont)->Leading;
}

int GetFontGutter(void *TheFont)
{
   //log.trace("GetFontGutter()");
   return static_cast<objFont *>(TheFont)->Gutter;
}

/*********************************************************************************************************************
** Point class
*/

Scintilla::Point Scintilla::Point::FromLong(long lpoint)
{
   return Scintilla::Point(Scintilla::Platform::LowShortFromLong(lpoint),
      Scintilla::Platform::HighShortFromLong(lpoint));
}

/*********************************************************************************************************************
** Palette class.  Functionality not required as we only use 32 bit colours.
*/

Scintilla::Palette::Palette() { }
Scintilla::Palette::~Palette() { }
void Scintilla::Palette::Release() { }
void Scintilla::Palette::Allocate(Scintilla::Window &w) { }

void Scintilla::Palette::WantFind(Scintilla::ColourPair &cp, bool want)
{
   cp.allocated.Set(cp.desired.AsLong());
}

/*********************************************************************************************************************
** Font class
**
** Not really supported as we only need to allocate 3 main fonts in the
** Scintilla class to serve all of our font needs in an edited document.
** Scintilla will try to create a font for every style allocated, which is
** overkill.
*/

Scintilla::Font::Font()
{
   bold = 0;
   italic = 0;
}

Scintilla::Font::~Font()
{

}

void Scintilla::Font::Create(const char *faceName, int characterSet, int size, bool bold_, bool italic_, int)
{
   kt::Log log(__FUNCTION__);

   bold = bold_;
   italic = italic_;

   log.trace("Face: %s, Style:%s%s", faceName, (bold) ? " Bold" : "", (italic) ? " Italic" : "");
}

void Scintilla::Font::Release()
{
}

/*********************************************************************************************************************
** BitmapClipper class
**
** Utility class used by SurfacePan
*/

class BitmapClipper
{
public:
   BitmapClipper(objBitmap *bitmap_, const Scintilla::PRectangle& cliprect) :  bitmap(bitmap_)
   {
      // Save old clipping rectangle

      saved_cliprect.left   = bitmap->Clip.Left;
      saved_cliprect.top    = bitmap->Clip.Top;
      saved_cliprect.right  = bitmap->Clip.Right;
      saved_cliprect.bottom = bitmap->Clip.Bottom;

      // Apply new clipping rectangle

      bitmap->Clip.Left   = std::max(bitmap->Clip.Left, cliprect.left);
      bitmap->Clip.Top    = std::max(bitmap->Clip.Top, cliprect.top);
      bitmap->Clip.Right  = std::min(bitmap->Clip.Right, cliprect.right);
      bitmap->Clip.Bottom = std::min(bitmap->Clip.Bottom, cliprect.bottom);
   }

   ~BitmapClipper()
   {
      // Restore old clipping rectangle

      bitmap->Clip.Left   = saved_cliprect.left;
      bitmap->Clip.Top    = saved_cliprect.top;
      bitmap->Clip.Right  = saved_cliprect.right;
      bitmap->Clip.Bottom = saved_cliprect.bottom;
   }

private:
   objBitmap *bitmap;
   Scintilla::PRectangle saved_cliprect;
};

/*********************************************************************************************************************
** DynamicLibraryImpl class
*/

class DynamicLibraryImpl : public Scintilla::DynamicLibrary
{
protected:

public:
   DynamicLibraryImpl(const char *modulePath) {
      kt::Log log(__FUNCTION__);
      log.msg("Path: %s", modulePath);
   }

   virtual ~DynamicLibraryImpl() { }

   // Use g_module_symbol to get a pointer to the relevant function.
   virtual void * FindFunction(const char *name) {
      kt::Log log(__FUNCTION__);
      log.msg("Name: %s", name);
      return nullptr;
   }

   virtual bool IsValid() {
      return FALSE;
   }
};

Scintilla::DynamicLibrary * Scintilla::DynamicLibrary::Load(const char *modulePath)
{
   kt::Log log(__FUNCTION__);
   log.msg("Path: %s", modulePath);
   return static_cast<DynamicLibrary *>(new DynamicLibraryImpl(modulePath));
}

/*********************************************************************************************************************
** ElapsedTime class
*/

Scintilla::ElapsedTime::ElapsedTime()
{
   Duration(TRUE); // Reset time
}

double Scintilla::ElapsedTime::Duration(bool reset)
{
   int64_t systime = PreciseTime() / 1000LL;
   int64_t lasttime = ((int64_t)bigBit<<32) + *(uint32_t*)&littleBit;
   double elapsed = systime - lasttime;

   if (reset) {
      bigBit = (long)(systime>>32);
      *(uint32_t*)&littleBit = (systime & 0xFFFFFFFF);
   }

   return elapsed * 0.001;
}

/*********************************************************************************************************************
** Platform class
*/

Scintilla::ColourDesired Scintilla::Platform::Chrome()
{
   return ColourDesired(0xe0, 0xe0, 0xe0);
}

Scintilla::ColourDesired Scintilla::Platform::ChromeHighlight()
{
   return Scintilla::ColourDesired(0xff, 0xff, 0xff);
}

const char * Scintilla::Platform::DefaultFont()
{
   return "Courier";
}

int Scintilla::Platform::DefaultFontSize()
{
   return 20;
}

unsigned int Scintilla::Platform::DoubleClickTime()
{
   return 500; // Half a second
}

bool Scintilla::Platform::MouseButtonBounce()
{
   return true;
}

void Scintilla::Platform::DebugDisplay(const char *string)
{
   kt::Log log("Scintilla");
   log.msg("%s", string);
}

bool Scintilla::Platform::IsKeyDown(int)
{
   kt::Log log(__FUNCTION__);
   log.msg("UNSUPPORTED");

   // TODO: discover state of keys in GTK+/X
   return false;
}

long Scintilla::Platform::SendScintilla(Scintilla::WindowID w, unsigned int msg, unsigned long wParam, long lParam)
{
   kt::Log log(__FUNCTION__);
   log.msg("UNSUPPORTED");
   return 0;
}

long Scintilla::Platform::SendScintillaPointer(Scintilla::WindowID w, unsigned int msg, unsigned long wParam, void *lParam)
{
   kt::Log log(__FUNCTION__);
   log.msg("UNSUPPORTED");

   //return scintilla_send_message(SCINTILLA(w), msg, wParam,
   //   reinterpret_cast<sptr_t>(lParam));
   return 0;
}

bool Scintilla::Platform::IsDBCSLeadByte(int /* codePage */, char /* ch */)
{
   return false;
}

int Scintilla::Platform::DBCSCharLength(int codePage, const char *s)
{
   return 1;/* TEMP HACK */
   /*if (codePage == 999932) {
      // Experimental and disabled code - change 999932 to 932 above to
      // enable locale avoiding but expensive character length determination.
      // Avoid locale with explicit use of iconv
      Converter convMeasure("UCS-2", CharacterSetID(SC_CHARSET_SHIFTJIS));
      size_t lenChar = MultiByteLenFromIconv(convMeasure, s, strlen(s));
      return lenChar;
   }
   else {
      int bytes = mblen(s, MB_CUR_MAX);
      if (bytes >= 1)
         return bytes;
      else
         return 1;
   }*/
}


int Scintilla::Platform::DBCSCharMaxLength()
{
   //return MB_CUR_MAX;
   //return 2;
   return 1; // TEMP HACK
}

// These are utility functions not really tied to a platform

int Scintilla::Platform::Minimum(int a, int b)
{
   if (a < b) return a;
   else return b;
}

int Scintilla::Platform::Maximum(int a, int b)
{
   if (a > b) return a;
   else return b;
}

void Scintilla::Platform::DebugPrintf(const char *format, ...)
{
#ifdef _DEBUG
   va_list pArguments;
   va_start(pArguments, format);
   VLogF(VLF::TRACE, "Scintilla", format, pArguments);
   va_end(pArguments);
#endif
}

// Not supported for GTK+
static bool assertionPopUps = true;

bool Scintilla::Platform::ShowAssertionPopUps(bool assertionPopUps_) {
   bool ret = assertionPopUps;
   assertionPopUps = assertionPopUps_;
   return ret;
}

void Scintilla::Platform::Assert(const char *c, const char *file, int line)
{
   kt::Log log("Assert");
   log.warning("%s, File %s, Line %d", c, file, line);
   exit(0);
}

int Scintilla::Platform::Clamp(int val, int minVal, int maxVal)
{
   if (val > maxVal) val = maxVal;
   if (val < minVal) val = minVal;
   return val;
}

void Platform_Initialise()
{

}

void Platform_Finalise()
{

}
