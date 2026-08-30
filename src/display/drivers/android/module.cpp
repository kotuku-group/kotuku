#include "android_driver.h"

#include <kotuku/modules/module.h>

#include <new>

#ifndef KOTUKU_STATIC
struct CoreBase *CoreBase = nullptr;
#endif

namespace display {

DisplayDriver * create_android_display_driver(uint32_t InterfaceVersion, struct CoreBase *Core)
{
   if ((InterfaceVersion != DISPLAY_DRIVER_INTERFACE_VERSION) or (not Core)) return nullptr;
#ifndef KOTUKU_STATIC
   CoreBase = Core;
#endif
   auto driver = new(std::nothrow) AndroidDriver;
   if ((not driver) or (not driver->valid())) {
      delete driver;
      return nullptr;
   }
   return driver;
}

void destroy_android_display_driver(DisplayDriver *Driver)
{
   delete Driver;
}

}

#ifndef KOTUKU_STATIC
extern "C" DISPLAY_DRIVER_EXPORT DisplayDriver * create_display_driver(uint32_t InterfaceVersion,
   struct CoreBase *Core)
{
   return display::create_android_display_driver(InterfaceVersion, Core);
}

extern "C" DISPLAY_DRIVER_EXPORT void destroy_display_driver(DisplayDriver *Driver)
{
   display::destroy_android_display_driver(Driver);
}

static ERR MODInit(OBJECTPTR, struct CoreBase *Core)
{
   if (not Core) return ERR::NullArgs;
   CoreBase = Core;
   return ERR::Okay;
}

__attribute__((visibility("default"))) struct ModHeader ModHeader(MODInit, nullptr, nullptr, nullptr, nullptr, nullptr,
   nullptr, "display-android", "GfxDriver");
#endif
