#include "x11_driver.h"

#include <kotuku/modules/module.h>

#include <new>

#ifndef KOTUKU_STATIC
struct CoreBase *CoreBase = nullptr;
#endif

namespace display {

DisplayDriver * create_x11_display_driver(uint32_t InterfaceVersion, struct CoreBase *Core)
{
   if ((InterfaceVersion != DISPLAY_DRIVER_INTERFACE_VERSION) or (not Core)) return nullptr;
#ifndef KOTUKU_STATIC
   CoreBase = Core;
#endif
   return new(std::nothrow) X11Driver;
}

void destroy_x11_display_driver(DisplayDriver *Driver)
{
   delete Driver;
}

}

#ifndef KOTUKU_STATIC
extern "C" DISPLAY_DRIVER_EXPORT DisplayDriver * create_display_driver(uint32_t InterfaceVersion,
   struct CoreBase *Core)
{
   return display::create_x11_display_driver(InterfaceVersion, Core);
}

extern "C" DISPLAY_DRIVER_EXPORT void destroy_display_driver(DisplayDriver *Driver)
{
   display::destroy_x11_display_driver(Driver);
}

static ERR MODInit(OBJECTPTR, struct CoreBase *Core)
{
   if (not Core) return ERR::NullArgs;
   CoreBase = Core;
   return ERR::Okay;
}

__attribute__((visibility("default"))) struct ModHeader ModHeader(MODInit, nullptr, nullptr, nullptr, nullptr, nullptr,
   nullptr, "display-x11", "GfxDriver");
#endif
