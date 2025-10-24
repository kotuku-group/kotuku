
#include <parasol/modules/xml.h>
#include <parasol/modules/xpath.h>
#include <parasol/strings.hpp>
#include <array>
#include <format>
#include <functional>
#include <sstream>
#include <memory>
#include <utility>
#include <string>
#include "../link/unicode.h"
#include "../xml/uri_utils.h"
#include "../xml/xml.h"

JUMPTABLE_CORE

static OBJECTPTR glContext = nullptr;

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR pModule, struct CoreBase *pCore)
{
   CoreBase = pCore;
   glContext = CurrentContext();
   return ERR::Okay;
}

static ERR MODOpen(OBJECTPTR Module)
{
   return ERR::Okay;
}

static ERR MODExpunge(void)
{
   return ERR::Okay;
}

//********************************************************************************************************************

PARASOL_MOD(MODInit, nullptr, MODOpen, MODExpunge, MOD_IDL, nullptr)
extern "C" struct ModHeader * register_xslt_module() { return &ModHeader; }
