#include "../driver/display_driver.h"
#include <kotuku/modules/display.h>

static int check(bool Condition)
{
   return Condition ? 0 : 1;
}

int main()
{
   int failures = 0;
   HeadlessDriver driver;
   DriverCallbacks callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION };
   HOSTWINDOW window = nullptr;
   APTR native_handle = nullptr;
   double pointer_x = 0;
   double pointer_y = 0;

   failures += check(std::string_view(driver.name()) IS "headless");
   failures += check(driver.displayType() IS DT::NATIVE);
   failures += check(driver.capabilities() IS DCAP::NIL);
   failures += check(driver.isAvailable() IS ERR::Okay);
   failures += check(driver.open(callbacks) IS ERR::Okay);
   failures += check(driver.open(callbacks) IS ERR::DoubleInit);
   failures += check(driver.flush() IS ERR::Okay);
   failures += check(driver.createWindow(nullptr, window) IS ERR::NoSupport);
   failures += check(driver.nativeWindowHandle(nullptr, native_handle) IS ERR::NoSupport);
   failures += check(driver.present(nullptr, nullptr, 0, 0, 1, 1, 0, 0) IS ERR::NoSupport);
   failures += check(driver.allocBitmap(nullptr) IS ERR::NoSupport);
   failures += check(driver.pointerPosition(pointer_x, pointer_y) IS ERR::NoSupport);
   failures += check(driver.close() IS ERR::Okay);
   failures += check(driver.close() IS ERR::Okay);

   callbacks.Version++;
   failures += check(driver.open(callbacks) IS ERR::WrongVersion);
   failures += check(driver.close() IS ERR::Okay);
   return failures;
}
