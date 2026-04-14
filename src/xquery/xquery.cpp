/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-MODULE-
XQuery: Provides XPath 2.0 and XQuery 1.0+ support for the XML module.

The XQuery module provides comprehensive support for XPath 2.0 and XQuery languages, enabling powerful querying and
navigation of XML documents.  It provides the @XQuery class as its primary interface, and operates in conjunction
with the @XML class to provide a standards-compliant query engine with extensive functionality.

-END-

*********************************************************************************************************************/

#include <kotuku/modules/regex.h>
#include "../link/unicode.h"
#include "../xml/uri_utils.h"
#include "functions/accessor_support.h"

JUMPTABLE_CORE
JUMPTABLE_REGEX

#include "xquery_def.cpp"

static OBJECTPTR glContext = nullptr;
static OBJECTPTR modRegex = nullptr;
static OBJECTPTR clXQuery = nullptr;

static ERR add_xquery_class(void);

//*********************************************************************************************************************
// Dynamic loader for the Regex functionality.  We only load it as needed due to the size of the module.

extern "C" ERR load_regex(void)
{
#ifndef KOTUKU_STATIC
   if (not modRegex) {
      pf::SwitchContext ctx(glContext);
      if (objModule::load("regex", &modRegex, &RegexBase) != ERR::Okay) return ERR::InitModule;
   }
#endif
   return ERR::Okay;
}

//********************************************************************************************************************

#ifdef ENABLE_UNIT_TESTS
#include "unit_tests.cpp"
#endif

//********************************************************************************************************************

static ERR MODInit(OBJECTPTR pModule, struct CoreBase *pCore)
{
   CoreBase = pCore;
   glContext = CurrentContext();
   return add_xquery_class();
}

static ERR MODOpen(OBJECTPTR Module)
{
   return ERR::Okay;
}

static ERR MODExpunge(void)
{
   if (clXQuery) { FreeResource(clXQuery); clXQuery = nullptr; }
   if (modRegex) { FreeResource(modRegex); modRegex = nullptr; }
   return ERR::Okay;
}

static void MODTest(CSTRING Options, int *Passed, int *Total)
{
#ifdef ENABLE_UNIT_TESTS
   run_unit_tests(*Passed, *Total);
#else
   pf::Log log(__FUNCTION__);
   log.warning("Unit tests are disabled in this build.");
#endif
}

//********************************************************************************************************************

#include "xquery_class.cpp"

static STRUCTS glStructures = {
};

KOTUKU_MOD(MODInit, nullptr, MODOpen, MODExpunge, MODTest, MOD_IDL, &glStructures)
extern "C" struct ModHeader * register_xquery_module() { return &ModHeader; }
