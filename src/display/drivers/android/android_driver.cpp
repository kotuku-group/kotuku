#define GL_GLEXT_PROTOTYPES 1

#include "android_driver.h"
#include "../../defs.h"

#include <kotuku/modules/android.h>

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>
#include <android/configuration.h>
#include <android/native_window.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <new>
#include <unordered_set>

struct AndroidBase *AndroidBase = nullptr;

namespace display {

void android_install_bitmap_routines(extBitmap *Bitmap);

struct AndroidBitmapRecord {
   GLuint Texture = 0;
   GLenum Pixel = GL_RGBA;
   GLenum Format = GL_UNSIGNED_BYTE;
   bool WriteBack = false;
};

struct AndroidDriver::State {
   std::recursive_mutex GraphicsLock;
   std::unordered_set<extBitmap *> Bitmaps;
   const DriverCallbacks *Callbacks = nullptr;
   OBJECTPTR AndroidModule = nullptr;
   ANativeWindow *Window = nullptr;
   EGLDisplay EGLDisplayHandle = EGL_NO_DISPLAY;
   EGLSurface EGLSurfaceHandle = EGL_NO_SURFACE;
   EGLContext EGLContextHandle = EGL_NO_CONTEXT;
   EGLint Width = 0;
   EGLint Height = 0;
   EGLint Depth = 16;
   OBJECTID ActiveDisplay = 0;
   FUNCTION InitWindow;
   FUNCTION TermWindow;
   bool Open = false;
   bool Closing = false;
   bool RequiresInitialisation = true;
};

static AndroidDriver *glAndroidDriver = nullptr;
static std::mutex glAndroidCallbackLock;

static AndroidBitmapRecord * bitmap_record(extBitmap *Bitmap)
{
   return Bitmap ? (AndroidBitmapRecord *)Bitmap->DriverData : nullptr;
}

static void init_window_callback(int)
{
   const std::lock_guard lock(glAndroidCallbackLock);
   if (glAndroidDriver) glAndroidDriver->nativeWindowInitialised();
}

static void term_window_callback(int)
{
   const std::lock_guard lock(glAndroidCallbackLock);
   if (glAndroidDriver) glAndroidDriver->nativeWindowTerminated();
}

static void set_colour_format(ColourFormat &Format, int BitsPerPixel)
{
   clearmem(&Format, sizeof(Format));
   Format.BitsPerPixel = BitsPerPixel;
   if (BitsPerPixel >= 24) {
      Format.RedMask = Format.GreenMask = Format.BlueMask = Format.AlphaMask = 0xff;
      Format.RedPos = 0;
      Format.GreenPos = 8;
      Format.BluePos = 16;
      Format.AlphaPos = 24;
   }
   else {
      Format.RedMask = 0x1f;
      Format.GreenMask = 0x3f;
      Format.BlueMask = 0x1f;
      Format.RedPos = 11;
      Format.GreenPos = 5;
      Format.RedShift = 3;
      Format.GreenShift = 2;
      Format.BlueShift = 3;
   }
}

static void release_egl(AndroidDriver::State *State)
{
   if (State->EGLDisplayHandle != EGL_NO_DISPLAY) {
      eglMakeCurrent(State->EGLDisplayHandle, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
      for (auto bitmap : State->Bitmaps) {
         if (auto record = bitmap_record(bitmap)) record->Texture = 0;
      }
      if (State->EGLContextHandle != EGL_NO_CONTEXT) {
         eglDestroyContext(State->EGLDisplayHandle, State->EGLContextHandle);
      }
      if (State->EGLSurfaceHandle != EGL_NO_SURFACE) {
         eglDestroySurface(State->EGLDisplayHandle, State->EGLSurfaceHandle);
      }
      eglTerminate(State->EGLDisplayHandle);
   }
   State->EGLDisplayHandle = EGL_NO_DISPLAY;
   State->EGLSurfaceHandle = EGL_NO_SURFACE;
   State->EGLContextHandle = EGL_NO_CONTEXT;
   State->RequiresInitialisation = true;
}

static ERR initialise_egl(AndroidDriver::State *State)
{
   if ((not State->RequiresInitialisation) and (State->EGLDisplayHandle != EGL_NO_DISPLAY)) return ERR::Okay;
   if (adGetWindow(&State->Window) != ERR::Okay) return ERR::NotInitialised;

   EGLint attributes[] = { EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_BLUE_SIZE, 5, EGL_GREEN_SIZE, 6,
      EGL_RED_SIZE, 5, EGL_DEPTH_SIZE, 0, EGL_NONE };
   EGLConfig config;
   EGLint total = 0;
   EGLint format = 0;
   State->EGLDisplayHandle = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if ((State->EGLDisplayHandle IS EGL_NO_DISPLAY) or
         (not eglInitialize(State->EGLDisplayHandle, nullptr, nullptr)) or
         (not eglChooseConfig(State->EGLDisplayHandle, attributes, &config, 1, &total)) or (not total)) {
      release_egl(State);
      return ERR::SystemCall;
   }
   eglGetConfigAttrib(State->EGLDisplayHandle, config, EGL_NATIVE_VISUAL_ID, &format);
   eglGetConfigAttrib(State->EGLDisplayHandle, config, EGL_BUFFER_SIZE, &State->Depth);
   ANativeWindow_setBuffersGeometry(State->Window, 0, 0, format);
   State->EGLSurfaceHandle = eglCreateWindowSurface(State->EGLDisplayHandle, config, State->Window, nullptr);
   State->EGLContextHandle = eglCreateContext(State->EGLDisplayHandle, config, EGL_NO_CONTEXT, nullptr);
   if ((State->EGLSurfaceHandle IS EGL_NO_SURFACE) or (State->EGLContextHandle IS EGL_NO_CONTEXT) or
         (not eglMakeCurrent(State->EGLDisplayHandle, State->EGLSurfaceHandle, State->EGLSurfaceHandle,
            State->EGLContextHandle))) {
      release_egl(State);
      return ERR::SystemCall;
   }
   eglQuerySurface(State->EGLDisplayHandle, State->EGLSurfaceHandle, EGL_WIDTH, &State->Width);
   eglQuerySurface(State->EGLDisplayHandle, State->EGLSurfaceHandle, EGL_HEIGHT, &State->Height);
   glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
   glShadeModel(GL_SMOOTH);
   glEnable(GL_BLEND);
   glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
   glDisable(GL_DEPTH_TEST);
   glEnable(GL_TEXTURE_2D);
   glDisable(GL_LIGHTING);
   State->RequiresInitialisation = false;
   return ERR::Okay;
}

static ERR begin_graphics(AndroidDriver::State *State)
{
   if ((not State->Open) or State->Closing) return ERR::NotInitialised;
   if (auto error = initialise_egl(State); error != ERR::Okay) return error;
   return eglMakeCurrent(State->EGLDisplayHandle, State->EGLSurfaceHandle, State->EGLSurfaceHandle,
      State->EGLContextHandle) ? ERR::Okay : ERR::NotInitialised;
}

static GLuint alloc_texture(int Width, int Height)
{
   GLuint texture = 0;
   glGenTextures(1, &texture);
   glBindTexture(GL_TEXTURE_2D, texture);
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
   glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
   GLint crop[] = { 0, Height, Width, -Height };
   glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_CROP_RECT_OES, crop);
   return glGetError() IS GL_NO_ERROR ? texture : 0;
}

AndroidDriver::AndroidDriver() : Data(new(std::nothrow) State) { }
AndroidDriver::~AndroidDriver() { if (Data) close(); delete Data; }
bool AndroidDriver::valid() const { return Data != nullptr; }
CSTRING AndroidDriver::name() const { return "android"; }
DT AndroidDriver::displayType() const { return DT::GLES; }
DCAP AndroidDriver::capabilities() const { return DCAP::VIDEO_BITMAPS; }
ERR AndroidDriver::isAvailable() const { return ERR::Okay; }

ERR AndroidDriver::open(const DriverCallbacks &Callbacks)
{
   if (Data->Open) return ERR::DoubleInit;
   if (Callbacks.Version != DISPLAY_DRIVER_INTERFACE_VERSION) return ERR::WrongVersion;
   Data->Callbacks = &Callbacks;
   Data->Closing = false;
   if (GetResource(RES::SYSTEM_STATE) >= 0) {
      if (objModule::load("android", &Data->AndroidModule, &AndroidBase) != ERR::Okay) return ERR::InitModule;
      SET_CALLBACK_STDC(Data->InitWindow, &init_window_callback);
      SET_CALLBACK_STDC(Data->TermWindow, &term_window_callback);
      if (adAddCallbacks(ACB_INIT_WINDOW, &Data->InitWindow, ACB_TERM_WINDOW, &Data->TermWindow,
            TAGEND) != ERR::Okay) {
         FreeResource(Data->AndroidModule);
         Data->AndroidModule = nullptr;
         return ERR::SystemCall;
      }
   }
   Data->Open = true;
   {
      const std::lock_guard lock(glAndroidCallbackLock);
      glAndroidDriver = this;
   }
   return ERR::Okay;
}

ERR AndroidDriver::close()
{
   if (not Data->Open) return ERR::Okay;
   {
      const std::lock_guard callback_lock(glAndroidCallbackLock);
      const std::lock_guard graphics_lock(Data->GraphicsLock);
      Data->Closing = true;
      if (glAndroidDriver IS this) glAndroidDriver = nullptr;
   }
   if (Data->AndroidModule) {
      adRemoveCallbacks(ACB_INIT_WINDOW, &Data->InitWindow, ACB_TERM_WINDOW, &Data->TermWindow, TAGEND);
   }
   const std::lock_guard lock(Data->GraphicsLock);
   release_egl(Data);
   if (Data->AndroidModule) FreeResource(Data->AndroidModule);
   Data->AndroidModule = nullptr;
   Data->Callbacks = nullptr;
   Data->Open = false;
   return ERR::Okay;
}

ERR AndroidDriver::createWindow(extDisplay *Display, HOSTWINDOW &Handle)
{
   const std::lock_guard lock(Data->GraphicsLock);
   if (adGetWindow(&Data->Window) != ERR::Okay) return ERR::NotInitialised;
   Handle = Data->Window;
   Data->ActiveDisplay = Display ? Display->UID : 0;
   return ERR::Okay;
}

ERR AndroidDriver::adoptWindow(extDisplay *Display, APTR NativeHandle, HOSTWINDOW &Handle)
{
   if (not NativeHandle) return ERR::NullArgs;
   const std::lock_guard lock(Data->GraphicsLock);
   Data->Window = (ANativeWindow *)NativeHandle;
   Handle = NativeHandle;
   Data->ActiveDisplay = Display ? Display->UID : 0;
   return ERR::Okay;
}

ERR AndroidDriver::nativeWindowHandle(HOSTWINDOW Window, APTR &NativeHandle)
{
   NativeHandle = Window;
   return Window ? ERR::Okay : ERR::NotInitialised;
}

ERR AndroidDriver::destroyWindow(HOSTWINDOW)
{
   const std::lock_guard lock(Data->GraphicsLock);
   Data->ActiveDisplay = 0;
   return ERR::Okay;
}
ERR AndroidDriver::showWindow(HOSTWINDOW, bool) { return ERR::Okay; }
ERR AndroidDriver::hideWindow(HOSTWINDOW) { return ERR::Okay; }

ERR AndroidDriver::windowCoords(HOSTWINDOW Window, int &X, int &Y, int &Width, int &Height)
{
   auto window = (ANativeWindow *)Window;
   if (not window) return ERR::NotInitialised;
   X = Y = 0;
   Width = ANativeWindow_getWidth(window);
   Height = ANativeWindow_getHeight(window);
   return ERR::Okay;
}

ERR AndroidDriver::displayInfo(DisplayInfo &Info)
{
   ANativeWindow *window = nullptr;
   if (adLockAndroid(3000) != ERR::Okay) return ERR::TimeOut;
   if (adGetWindow(&window) != ERR::Okay) {
      adUnlockAndroid();
      return ERR::NotInitialised;
   }
   Info.DisplayID = 0;
   Info.Width = ANativeWindow_getWidth(window);
   Info.Height = ANativeWindow_getHeight(window);
   Info.BitsPerPixel = ANativeWindow_getFormat(window) IS WINDOW_FORMAT_RGBA_8888 ? 32 : 16;
   Info.BytesPerPixel = Info.BitsPerPixel / 8;
   Info.AmtColours = 1 << std::min(Info.BitsPerPixel, 24);
   Info.AccelFlags = ACF::VIDEO_BLIT;
   Info.Flags = SCR::MAXSIZE;
   density(nullptr, Info.HDensity, Info.VDensity);
   set_colour_format(Info.PixelFormat, Info.BitsPerPixel);
   adUnlockAndroid();
   return ERR::Okay;
}

ERR AndroidDriver::density(HOSTWINDOW, int &Horizontal, int &Vertical)
{
   AConfiguration *config = nullptr;
   int value = adGetConfig(&config) IS ERR::Okay ? AConfiguration_getDensity(config) : 160;
   if (value < 60) value = 160;
   Horizontal = Vertical = value;
   return ERR::Okay;
}

ERR AndroidDriver::pixelFormat(ColourFormat &Format) { set_colour_format(Format, Data->Depth); return ERR::Okay; }

ERR AndroidDriver::present(HOSTWINDOW, extBitmap *Source, int, int, int, int, int XDest, int YDest)
{
   if ((not Source) or (not Source->Data)) return ERR::NoData;
   const std::lock_guard lock(Data->GraphicsLock);
   if (auto error = begin_graphics(Data); error != ERR::Okay) return error;
   auto record = bitmap_record(Source);
   GLenum pixel = record ? record->Pixel : (Source->BitsPerPixel > 24 ? GL_RGBA : GL_RGB);
   GLenum format = record ? record->Format : (Source->BitsPerPixel <= 16 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE);
   GLuint texture = alloc_texture(Source->Width, Source->Height);
   if (not texture) return ERR::OpenGL;
   glTexImage2D(GL_TEXTURE_2D, 0, pixel, Source->Width, Source->Height, 0, pixel, format, Source->Data);
   glDrawTexiOES(XDest, -YDest, 1, Source->Width, Source->Height);
   glDeleteTextures(1, &texture);
   return eglSwapBuffers(Data->EGLDisplayHandle, Data->EGLSurfaceHandle) ? ERR::Okay : ERR::SystemCall;
}

ERR AndroidDriver::blitBitmap(extBitmap *Destination, extBitmap *Source, BAF, int X, int Y, int Width,
   int Height, int XDest, int YDest)
{
   if ((not Destination) or (Destination->MemType != BMT::VIDEO)) return ERR::NoSupport;
   return present(nullptr, Source, X, Y, Width, Height, XDest, YDest);
}

ERR AndroidDriver::fillBitmap(extBitmap *Destination, int X, int Y, int Width, int Height, uint32_t Colour)
{
   if ((not Destination) or (Destination->MemType != BMT::VIDEO)) return ERR::NoSupport;
   const std::lock_guard lock(Data->GraphicsLock);
   if (auto error = begin_graphics(Data); error != ERR::Okay) return error;
   glEnable(GL_SCISSOR_TEST);
   glScissor(X, Data->Height - Y - Height, Width, Height);
   glClearColor(Destination->unpackRed(Colour) / 255.0f, Destination->unpackGreen(Colour) / 255.0f,
      Destination->unpackBlue(Colour) / 255.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glDisable(GL_SCISSOR_TEST);
   return ERR::Okay;
}

ERR AndroidDriver::flush()
{
   const std::lock_guard lock(Data->GraphicsLock);
   if (auto error = begin_graphics(Data); error != ERR::Okay) return error;
   glFlush();
   return ERR::Okay;
}

ERR AndroidDriver::allocBitmap(extBitmap *Bitmap)
{
   if ((not Bitmap) or (Bitmap->MemType IS BMT::DATA)) return ERR::NoSupport;
   const std::lock_guard lock(Data->GraphicsLock);
   if (Bitmap->DriverData) return ERR::Okay;
   auto record = new(std::nothrow) AndroidBitmapRecord;
   if (not record) return ERR::AllocMemory;
   record->Pixel = ((Bitmap->BitsPerPixel IS 8) and ((Bitmap->Flags & BMF::MASK) != BMF::NIL)) ? GL_ALPHA :
      (Bitmap->BitsPerPixel <= 24 ? GL_RGB : GL_RGBA);
   record->Format = Bitmap->BitsPerPixel <= 16 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE;
   Bitmap->DriverData = record;
   Bitmap->prvAFlags |= BF_DRIVER_DATA;
   Bitmap->Flags |= BMF::ACCELERATED_2D;
   Data->Bitmaps.insert(Bitmap);
   return ERR::Okay;
}

ERR AndroidDriver::freeBitmap(extBitmap *Bitmap)
{
   if (not Bitmap) return ERR::NullArgs;
   const std::lock_guard lock(Data->GraphicsLock);
   if (auto record = bitmap_record(Bitmap)) {
      if (record->Texture and (begin_graphics(Data) IS ERR::Okay)) glDeleteTextures(1, &record->Texture);
      delete record;
   }
   Data->Bitmaps.erase(Bitmap);
   Bitmap->DriverData = nullptr;
   Bitmap->prvAFlags &= ~BF_DRIVER_DATA;
   return ERR::Okay;
}

ERR AndroidDriver::resizeBitmap(extBitmap *Bitmap, int, int)
{
   if (auto record = bitmap_record(Bitmap)) {
      const std::lock_guard lock(Data->GraphicsLock);
      if (record->Texture and (begin_graphics(Data) IS ERR::Okay)) glDeleteTextures(1, &record->Texture);
      record->Texture = 0;
      return ERR::Okay;
   }
   return ERR::NoSupport;
}

ERR AndroidDriver::lockBitmap(extBitmap *Bitmap, int16_t Access)
{
   auto record = bitmap_record(Bitmap);
   if (not record) return ERR::NoSupport;
   if (Bitmap->MemType IS BMT::TEXTURE) return ERR::NoSupport;
   if (not Bitmap->Data) {
      Bitmap->Data = (uint8_t *)std::malloc(Bitmap->Size);
      if (not Bitmap->Data) return ERR::AllocMemory;
      Bitmap->prvAFlags |= BF_DATA;
   }
   const std::lock_guard lock(Data->GraphicsLock);
   if (auto error = begin_graphics(Data); error != ERR::Okay) return error;
   if (Access & SURFACE_READ) {
      glReadPixels(0, 0, Bitmap->Width, Bitmap->Height, record->Pixel, record->Format, Bitmap->Data);
   }
   record->WriteBack = Access & SURFACE_WRITE;
   return ERR::Okay;
}

ERR AndroidDriver::unlockBitmap(extBitmap *Bitmap)
{
   auto record = bitmap_record(Bitmap);
   if ((not record) or (not record->WriteBack)) return ERR::Okay;
   record->WriteBack = false;
   return present(nullptr, Bitmap, 0, 0, Bitmap->Width, Bitmap->Height, 0, 0);
}

ERR AndroidDriver::bitmapRoutines(extBitmap *Bitmap)
{
   if (not bitmap_record(Bitmap)) return ERR::NoSupport;
   android_install_bitmap_routines(Bitmap);
   return ERR::Okay;
}

void AndroidDriver::nativeWindowInitialised()
{
   const std::lock_guard lock(Data->GraphicsLock);
   if (Data->Closing) return;
   Data->RequiresInitialisation = true;
   if (Data->ActiveDisplay) {
      QueueAction(AC::Show, Data->ActiveDisplay);
      QueueAction(AC::Draw, Data->ActiveDisplay);
   }
}

void AndroidDriver::nativeWindowTerminated()
{
   const std::lock_guard lock(Data->GraphicsLock);
   if (not Data->Closing) release_egl(Data);
}

}
