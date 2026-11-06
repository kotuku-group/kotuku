#include "kt_classes.h"

std::shared_ptr<Scintilla::Internal::Font> Scintilla::Internal::Font::Allocate(const FontParameters &Parameters)
{
   return std::make_shared<PanFont>(Parameters);
}

Scintilla::Internal::ColourRGBA Scintilla::Internal::Platform::Chrome()
{
   return ColourRGBA(0xe0, 0xe0, 0xe0);
}

Scintilla::Internal::ColourRGBA Scintilla::Internal::Platform::ChromeHighlight()
{
   return ColourRGBA(0xff, 0xff, 0xff);
}

const char *Scintilla::Internal::Platform::DefaultFont() { return "Clean"; }
int Scintilla::Internal::Platform::DefaultFontSize() { return 20; }

unsigned int Scintilla::Internal::Platform::DoubleClickTime()
{
   unsigned int interval = 300;
   if (auto pointer = gfx::AccessPointer()) {
      if (pointer->DoubleClick > 0) interval = (unsigned int)std::lround(pointer->DoubleClick * 1000.0);
      ReleaseObject(pointer);
   }
   return interval;
}

void Scintilla::Internal::Platform::DebugDisplay(const char *Text) noexcept
{
   kt::Log("Scintilla").msg("%s", Text);
}

void Scintilla::Internal::Platform::DebugPrintf(const char *Format, ...) noexcept
{
   va_list arguments;
   va_start(arguments, Format);
   VLogF(VLF::TRACE, "Scintilla", Format, arguments);
   va_end(arguments);
}

static bool glAssertionPopUps = false;

bool Scintilla::Internal::Platform::ShowAssertionPopUps(bool Enable) noexcept
{
   return std::exchange(glAssertionPopUps, Enable);
}

void Scintilla::Internal::Platform::Assert(const char *Condition, const char *File, int Line) noexcept
{
   kt::Log("Scintilla").warning("%s, File %s, Line %d", Condition, File, Line);
   std::abort();
}

void Platform_Initialise() { }
void Platform_Finalise() { }
