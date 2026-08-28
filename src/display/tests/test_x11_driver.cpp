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
   auto driver = display::get_x11_driver();
   int failures = 0;

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

   restore_environment("KOTUKU_XDISPLAY", original_kotuku_display);
   restore_environment("DISPLAY", original_display);
   return failures;
}
