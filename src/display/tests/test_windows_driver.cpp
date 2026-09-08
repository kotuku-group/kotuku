#include "../driver/display_driver.h"

#include <kotuku/modules/display.h>

#include <string>

static int check(bool Condition)
{
   return Condition ? 0 : 1;
}

int main()
{
   auto core = (struct CoreBase *)(uintptr_t)1;
#ifdef KOTUKU_STATIC
   auto driver = display::create_win32_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#else
   auto driver = create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#endif
   int failures = 0;

   failures += check(driver != nullptr);
   if (not driver) return failures;
   failures += check(std::string(driver->name()) IS "windows");
   failures += check(driver->displayType() IS DT::WINGDI);
   failures += check(driver->isAvailable() IS ERR::Okay);

   DriverCallbacks wrong_callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION - 1 };
   failures += check(driver->open(wrong_callbacks) IS ERR::WrongVersion);
   failures += check(driver->close() IS ERR::Okay);

#ifdef KOTUKU_STATIC
   display::destroy_win32_display_driver(driver);
   display::destroy_win32_display_driver(nullptr);
   failures += check(display::create_win32_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(display::create_win32_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#else
   destroy_display_driver(driver);
   destroy_display_driver(nullptr);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#endif
   return failures;
}
