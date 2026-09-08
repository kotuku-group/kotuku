#include "../../driver/display_driver.h"

#include <kotuku/modules/module.h>

#ifndef KOTUKU_STATIC
struct CoreBase *CoreBase = nullptr;
#endif

#ifndef KOTUKU_STATIC
extern "C" DisplayDriver * create_display_driver(uint32_t InterfaceVersion, struct CoreBase *Core)
{
   if (not Core) return nullptr;
   CoreBase = Core;
   return display::create_win32_display_driver(InterfaceVersion, Core);
}

extern "C" void destroy_display_driver(DisplayDriver *Driver)
{
   display::destroy_win32_display_driver(Driver);
}

static ERR MODInit(OBJECTPTR, struct CoreBase *Core)
{
   if (not Core) return ERR::NullArgs;
   CoreBase = Core;
   return ERR::Okay;
}

struct ModHeader ModHeader(MODInit, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, "display-windows",
   "GfxDriver");
#endif
