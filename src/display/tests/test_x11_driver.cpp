#include "../drivers/x11/x11_driver.h"

#include <kotuku/modules/display.h>

#include <cstdlib>
#include <optional>
#include <string>

static int check(bool Condition)
{
   return Condition ? 0 : 1;
}

static std::optional<std::string> environment_value(CSTRING Name)
{
   if (auto value = std::getenv(Name)) return value;
   return std::nullopt;
}

static void restore_environment(CSTRING Name, const std::optional<std::string> &Value)
{
   if (Value) setenv(Name, Value->c_str(), 1);
   else unsetenv(Name);
}

int main()
{
   const auto original_kotuku_display = environment_value("KOTUKU_XDISPLAY");
   const auto original_display = environment_value("DISPLAY");
   auto core = (struct CoreBase *)(uintptr_t)1;
#ifdef KOTUKU_STATIC
   auto driver = display::create_x11_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#else
   auto driver = create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, core);
#endif
   int failures = 0;

   failures += check(driver != nullptr);
   failures += check(std::string(driver->name()) IS "x11");
   failures += check(driver->displayType() IS DT::X11);
   failures += check(driver->capabilities() IS DCAP::NIL);

   unsetenv("KOTUKU_XDISPLAY");
   unsetenv("DISPLAY");
   failures += check(driver->isAvailable() IS ERR::NoSupport);

   setenv("DISPLAY", ":91", 1);
   failures += check(driver->isAvailable() IS ERR::Okay);

   setenv("KOTUKU_XDISPLAY", ":92", 1);
   unsetenv("DISPLAY");
   failures += check(driver->isAvailable() IS ERR::Okay);

   DriverCallbacks wrong_callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION - 1 };
   failures += check(driver->open(wrong_callbacks) IS ERR::WrongVersion);
   failures += check(driver->close() IS ERR::Okay);

   DriverCallbacks callbacks = { .Version = DISPLAY_DRIVER_INTERFACE_VERSION };
   failures += check(driver->open(callbacks) IS ERR::SystemCall);
   failures += check(driver->close() IS ERR::Okay);
   failures += check(driver->close() IS ERR::Okay);

#ifdef KOTUKU_STATIC
   display::destroy_x11_display_driver(driver);
   failures += check(display::create_x11_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(display::create_x11_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#else
   destroy_display_driver(driver);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION - 1, core) IS nullptr);
   failures += check(create_display_driver(DISPLAY_DRIVER_INTERFACE_VERSION, nullptr) IS nullptr);
#endif

   restore_environment("KOTUKU_XDISPLAY", original_kotuku_display);
   restore_environment("DISPLAY", original_display);
   return failures;
}
