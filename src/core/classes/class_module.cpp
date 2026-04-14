/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

-CLASS-
Module: Manages the loading of system libraries.

The Module class is used to load and maintain the modules that are installed on the user's system.  A number of modules
are available in the core platform as standard, which you can use in the development of your programs.  Examples of
existing modules can be found in both the `modules:` folder.

To load a module and interact with its API, create a module object and initialise it.  The following code segment
illustrates in C++:

<pre>
DisplayBase *DisplayBase;
auto modDisplay = objModule::create::global(fl::Name("display"));
if (modDisplay) modDisplay->get(FID_ModBase, DisplayBase);
</pre>

To do the same in Tiri:

<pre>
mGfx = mod.load('display')
</pre>

It is critical that the module object is permanently retained until the program no longer needs its functionality.
-END-

**********************************************************************************************************************/

#ifdef __unix__
#include <dlfcn.h>
#endif

#include "../defs.h"
#include <kotuku/main.h>

static STRUCTS glStructures = {
   { "ActionArray",         sizeof(ActionArray) },
   { "ActionEntry",         sizeof(ActionEntry) },
   //{ "ActionTable",         sizeof(ActionTable) },
   { "CacheFile",           sizeof(CacheFile) },
   { "ChildEntry",          sizeof(ChildEntry) },
   { "ClipRectangle",       sizeof(ClipRectangle) },
   { "ColourFormat",        sizeof(ColourFormat) },
   { "CompressedItem",      sizeof(CompressedItem) },
   { "CompressionFeedback", sizeof(CompressionFeedback) },
   { "DateTime",            sizeof(DateTime) },
   { "DirInfo",             sizeof(DirInfo) },
   { "Edges",               sizeof(Edges) },
   { "FRGB",                sizeof(FRGB) },
   { "Field",               sizeof(Field) },
   { "FieldArray",          sizeof(FieldArray) },
   { "FieldDef",            sizeof(FieldDef) },
   { "FileFeedback",        sizeof(FileFeedback) },
   { "FileInfo",            sizeof(FileInfo) },
   { "Function",            sizeof(Function) },
   { "FunctionField",       sizeof(FunctionField) },
   { "HSV",                 sizeof(HSV) },
   { "InputEvent",          sizeof(InputEvent) },
   { "MemInfo",             sizeof(MemInfo) },
   { "Message",             sizeof(Message) },
   { "MethodEntry",         sizeof(MethodEntry) },
   { "ModHeader",           sizeof(struct ModHeader) },
   { "MsgHandler",          sizeof(MsgHandler) },
   { "ObjectSignal",        sizeof(ObjectSignal) },
   { "RGB16",               sizeof(RGB16) },
   { "RGB32",               sizeof(RGB32) },
   { "RGB8",                sizeof(RGB8) },
   { "RGBPalette",          sizeof(RGBPalette) },
   { "ResourceManager",     sizeof(ResourceManager) },
   { "SystemState",         sizeof(SystemState) },
   { "ThreadActionMessage", sizeof(ThreadActionMessage) },
   { "ThreadMessage",       sizeof(ThreadMessage) },
   { "Unit",                sizeof(Unit) },
   { "dcAudio",             sizeof(dcAudio) },
   { "dcDeviceInput",       sizeof(dcDeviceInput) },
   { "dcKeyEntry",          sizeof(dcKeyEntry) },
   { "dcRequest",           sizeof(dcRequest) }
};

#include "../idl.h"

static RootModule glCoreRoot;
struct ModHeader glCoreHeader(nullptr, nullptr, nullptr, nullptr, nullptr, glIDL, &glStructures, "core", "Sys");

static RootModule * check_resident(extModule *, std::string_view);
static void free_module(MODHANDLE handle);

//********************************************************************************************************************

static ERR GET_Defs(extModule *, CSTRING *);
static ERR GET_Name(extModule *, CSTRING *);

static ERR SET_Header(extModule *, struct ModHeader *);
static ERR SET_Name(extModule *, CSTRING);

static const FieldDef clFlags[] = {
   { "LinkLibrary", MOF::LINK_LIBRARY },
   { "Static",      MOF::STATIC },
   { nullptr, 0 }
};

static const FieldArray glModuleFields[] = {
   { "FunctionList", FDF_POINTER|FDF_RW },
   { "ModBase",      FDF_POINTER|FDF_R },
   { "Root",         FDF_OBJECT|FDF_R, nullptr, nullptr, "RootModule" }, // Not intended for client use
   { "Header",       FDF_POINTER|FDF_STRUCT|FDF_RI, nullptr, SET_Header, "ModHeader" }, // For creating virtual modules only
   { "Flags",        FDF_INT|FDF_RI, nullptr, nullptr, &clFlags },
   // Virtual fields
   { "Defs",         FDF_STRING|FDF_R, GET_Defs },
   { "Name",         FDF_STRING|FDF_RI, GET_Name, SET_Name },
   END_FIELD
};

static ERR MODULE_Init(extModule *);
static ERR MODULE_Free(extModule *);
static ERR MODULE_NewPlacement(extModule *);

static const ActionArray glModuleActions[] = {
   { AC::Free, MODULE_Free },
   { AC::Init, MODULE_Init },
   { AC::NewPlacement, MODULE_NewPlacement },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

#ifndef KOTUKU_STATIC
static ERR load_mod(extModule *Self, RootModule *Root, struct ModHeader **Table)
{
   pf::Log log(__FUNCTION__);
   std::string path;

   if ((Self->Name.starts_with('/')) or (Self->Name.find(':') != std::string::npos)) {
      log.trace("Module location is absolute.");
      path.assign(Self->Name);

      std::string volume;
      if (ResolvePath(path, RSF::APPROXIMATE, &volume) IS ERR::Okay) {
         path.assign(volume);
      }
      else {
         log.warning("Failed to resolve the path of module '%s'", Self->Name.c_str());
         return ERR::ResolvePath;
      }
   }

   if (path.empty()) {
      #ifdef __unix__
         if (!glModulePath.empty()) { // If no specific module path is defined, default to the system path and tack on the modules/ suffix.
            path.assign(glModulePath);
            if (path.back() != '/') path.push_back('/');
         }
         else path = glRootPath + "lib/kotuku/";

         if ((Self->Flags & MOF::LINK_LIBRARY) != MOF::NIL) path += "lib/";

         #ifdef __ANDROID__
            if ((Self->Name.starts_with("lib"));
            else path += "lib"; // Packaged Android modules have to begin with 'lib'
         #endif

         path.append(Self->Name);

      #elif _WIN32
         if (!glModulePath.empty()) {
            path = glModulePath;
            if ((path.back() != '\\') and (path.back() != '/')) path.push_back('\\');
         }
         else if (!glSystemPath.empty()) {
            path = glSystemPath;
            if ((path.back() != '\\') and (path.back() != '/')) path.push_back('\\');
            path += "lib\\";
         }
         else {
            path = glRootPath;
            if ((path.back() != '\\') and (path.back() != '/')) path.push_back('\\');
            path += "lib\\";
         }

         if ((Self->Flags & MOF::LINK_LIBRARY) != MOF::NIL) path += "lib\\";
         path.append(Self->Name);
      #endif
   }

   // Deal with the file extensions

   if (path.ends_with(".dll"));
   else if (path.ends_with(".so"));
   else {
      #ifdef __unix__
         path.append(".so");
      #elif _WIN32
         path.append(".dll");
      #elif __APPLE__ // OSX uses .dylib but is compatible with .so
         path.append(".so");
      #else
         #error What is the module extension for this machine type (.so/.mod/...)?
      #endif
   }

   log.trace("Loading module \"%s\".", path.c_str());

   // Open the module file.  Note that we will dlclose() the module in the expunge sequence of the Core (see core.c).

   #ifdef __unix__

      // RTLD_LAZY needs to be used in case the module wants to have the ability to link to
      // symbolically linked libraries (e.g. the Network module does this to dynamically load
      // SSL support).
      //
      // RTLD_GLOBAL is needed only for symbolically linked libraries in case one is dependent on
      // other libraries.  SSL is an example of this as the libssl library is dependent
      // on symbols found in libcrypto, therefore libcrypto needs RTLD_GLOBAL.

      if ((Root->LibraryBase = dlopen(path.c_str(), ((Self->Flags & MOF::LINK_LIBRARY) != MOF::NIL) ? (RTLD_LAZY|RTLD_GLOBAL) : RTLD_LAZY))) {
         if ((Self->Flags & MOF::LINK_LIBRARY) IS MOF::NIL) {
            if (!(*Table = (struct ModHeader *)dlsym(Root->LibraryBase, "ModHeader"))) {
               log.warning("The 'ModHeader' structure is missing from module %s.", path.c_str());
               return ERR::NotFound;
            }
         }
      }
      else {
         log.warning("%s: %s", Self->Name.c_str(), (CSTRING)dlerror());
         return ERR::NoSupport;
      }

   #elif _WIN32

      if ((Root->LibraryBase = winLoadLibrary(path.c_str()))) {
         if ((Self->Flags & MOF::LINK_LIBRARY) IS MOF::NIL) {
            if (!(*Table = (struct ModHeader *)winGetProcAddress(Root->LibraryBase, "ModHeader"))) {
               if (!(*Table = (struct ModHeader *)winGetProcAddress(Root->LibraryBase, "_ModHeader"))) {
                  log.warning("The 'ModHeader' structure is missing from module %s.", path.c_str());
                  return ERR::NotFound;
               }
            }
         }
      }
      else {
         auto msg = winFormatMessage(0);
         log.error("Failed to load DLL '%s' (call: winLoadLibrary(): %s).", path.c_str(), msg.c_str());
         return ERR::Read;
      }

   #else
      #error This system needs support for the loading of module/exe files.
   #endif

   return ERR::Okay;
}
#endif

//********************************************************************************************************************

ERR ROOTMODULE_Free(RootModule *Self)
{
   if (Self->Table) Self->Table->Root = nullptr; // Remove the DLL's reference to the master.

   // Note that the order in which we perform the following actions is very important.

   if (Self->CoreBase) { FreeResource(Self->CoreBase); Self->CoreBase = nullptr; }

   // Free the module's segment/code area

   if ((!Self->NoUnload) and ((Self->Flags & MHF::STATIC) IS MHF::NIL)) {
      free_module(Self->LibraryBase);
      Self->LibraryBase = nullptr;
   }

   if (auto lock = std::unique_lock{glmGeneric, 200ms}) {
      // Patch the gap
      if (Self->Prev) Self->Prev->Next = Self->Next;
      else glModuleList = Self->Next;

      if (Self->Next) Self->Next->Prev = Self->Prev;
   }

   Self->~RootModule();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR ROOTMODULE_NewPlacement(RootModule *Self)
{
   new (Self) RootModule;
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR ROOTMODULE_GET_Header(RootModule *Self, struct ModHeader **Value)
{
   *Value = Self->Header;
   return ERR::Okay;
}

//********************************************************************************************************************
// This action sends a CLOSE command to the module, then frees the personally assigned module structure.  Note that the
// module code will be left resident in memory as it belongs to the RootModule, not the Module.  See Expunge()
// in the Core for further details.

static ERR MODULE_Free(extModule *Self)
{
   // Call the Module's Close procedure

   if (Self->Root) {
      if (Self->Root->OpenCount > 0) Self->Root->OpenCount--;
      if (Self->Root->Close)         Self->Root->Close(Self);
      Self->Root = nullptr;
   }

   if (Self->prvMBMemory) { FreeResource(Self->prvMBMemory); Self->prvMBMemory = nullptr; }
   Self->~extModule();
   return ERR::Okay;
}

//********************************************************************************************************************

static ERR MODULE_Init(extModule *Self)
{
   pf::Log log;
   ERR error = ERR::ModuleValidation;
   bool root_mod = false;

   if (!Self->Name[0]) return log.warning(ERR::FieldNotSet);

   // Check if the module is resident.  If not, we need to load and prepare the module for a shared environment.

   std::string_view name = std::string_view(Self->Name);
   if (auto i = name.find_last_of(":/\\"); i != std::string::npos) {
      name.remove_prefix(i+1);
   }

   if (auto sep = name.find_last_of("."); sep != std::string::npos) {
      name.remove_suffix(name.size() - sep);
   }

   log.trace("Finding module %s (%s)", Self->Name.c_str(), name.data());

   RootModule *master;
   struct ModHeader *table = nullptr;
   if ((master = check_resident(Self, name))) {
      Self->Root = master;
   }
   else if (NewObject(CLASSID::ROOTMODULE, NF::UNTRACKED, (OBJECTPTR *)&master) IS ERR::Okay) {
      master->Next = glModuleList; // Insert the RootModule at the start of the chain.
      if (glModuleList) glModuleList->Prev = master;
      glModuleList = master;

      root_mod = true;

      pf::SwitchContext ctx(master);

      master->LibraryName.assign(name);

      if (Self->Header) {
         // If the developer has specified a module header, then the module code is memory-resident and not to be
         // loaded from storage.
         table = Self->Header;
      }
      else {
         #ifdef KOTUKU_STATIC
         auto it = glStaticModules.find(Self->Name);
         if (it != glStaticModules.end()) table = it->second;
         else {
            log.warning("Unable to find module '%s' from %d static modules.", Self->Name.c_str(), int(glStaticModules.size()));
            error = ERR::NotFound;
            goto exit;
         }
         #else
         if ((error = load_mod(Self, master, &table)) != ERR::Okay) goto exit;
         #endif
      }

      master->OpenCount  = 0;
      master->Version    = 1;
      Self->Root = master;

      if (table) {
         if (!table->Init) { log.warning(ERR::ModuleMissingInit); goto exit; }
         if (!table->Name) { log.warning(ERR::ModuleMissingName); goto exit; }

         master->Header  = table;
         master->Table   = table;
         master->Name    = table->Name;
         master->Init    = table->Init;
         master->Open    = table->Open;
         master->Expunge = table->Expunge;
         master->Test    = table->Test;
         master->Flags   = table->Flags;
      }

      // INIT

      if (master->Init) {
         #ifdef KOTUKU_STATIC
            error = master->Init(Self, nullptr);
         #else
            // Build a Core base for the module to use
            if (auto modkb = (struct CoreBase *)build_jump_table(glFunctions)) {
               master->CoreBase = modkb;
               log.traceBranch("Initialising the module.");
               error = master->Init(Self, modkb);
            }
         #endif
         if (error != ERR::Okay) goto exit;
      }
      else if ((Self->Flags & MOF::LINK_LIBRARY) != MOF::NIL) {
         log.msg("Loaded link library '%s'", Self->Name.c_str());
      }
      else {
         log.warning(ERR::ModuleMissingInit);
         goto exit;
      }
   }
   else {
      error = log.warning(ERR::NewObject);
      goto exit;
   }

   // If the STATIC option is set then the loaded module must not be removed when the Module object is freed.  This is
   // typically used for symbolic linked libraries.

   if ((Self->Flags & MOF::STATIC) != MOF::NIL) master->Flags |= MHF::STATIC;

   // At this stage the module is 100% resident and it is not possible to reverse the process.  Because of this, if an
   // error occurs we must not try to free any resident allocations from memory.

   root_mod = false;

   if (master->Open) {
      log.trace("Opening %s module.", Self->Name.c_str());
      if (master->Open(Self) != ERR::Okay) {
         log.warning(ERR::ModuleOpenFailed);
         goto exit;
      }
   }

   if (master->Table) master->Close = master->Table->Close;
   master->OpenCount++;

   // Build the jump table for the program

   #ifndef KOTUKU_STATIC
   if (Self->FunctionList) {
      if (!(Self->ModBase = build_jump_table(Self->FunctionList))) {
         goto exit;
      }
      Self->prvMBMemory = Self->ModBase;
   }
   #endif

   // Some DLL's like wsock2 can change the exception handler - we don't want that, so reset our exception handler
   // just in case.

   #ifdef _WIN32
      winSetUnhandledExceptionFilter(nullptr);
   #endif

   log.trace("Module has been successfully initialised.");
   error = ERR::Okay;

exit:
   if (error != ERR::Okay) { // Free allocations if an error occurred
      if ((error & ERR::Notified) IS ERR::Okay) log.msg("\"%s\" failed: %s", Self->Name.c_str(), GetErrorMsg(error));
      error &= ~(ERR::Notified);

      if (root_mod) {
         if (master->Expunge) master->Expunge();
         FreeResource(master);
         Self->Root = nullptr;
      }
   }

   return error;
}

//********************************************************************************************************************

static ERR MODULE_NewPlacement(extModule *Self)
{
   new (Self) extModule;
   return ERR::Okay;
}

/*********************************************************************************************************************

-METHOD-
ResolveSymbol: Resolves library symbol names to their address pointers.

This method will convert symbol names to their respective address pointers.  The module code must have been successfully
loaded into memory or an `ERR::FieldNotSet` error will be returned.  If the symbol was not found then `ERR::NotFound` is
returned.

-INPUT-
cstr Name: The name of the symbol to resolve.
&ptr Address: The address of the symbol will be returned in this parameter.

-ERRORS-
Okay
NullArgs
FieldNotSet: The module has not been successfully initialised.
NotFound: The symbol was not found.
NoSupport: The host platform does not support this method.

**********************************************************************************************************************/

static ERR MODULE_ResolveSymbol(extModule *Self, struct mod::ResolveSymbol *Args)
{
   pf::Log log;

   if ((!Args) or (!Args->Name)) return log.warning(ERR::NullArgs);

#ifdef _WIN32
   #ifdef KOTUKU_STATIC
   if ((Args->Address = winGetProcAddress(nullptr, Args->Name))) {
   #else
   if ((!Self->Root) or (!Self->Root->LibraryBase)) return ERR::FieldNotSet;
   if ((Args->Address = winGetProcAddress(Self->Root->LibraryBase, Args->Name))) {
   #endif
      return ERR::Okay;
   }
   else {
      log.msg("Failed to resolve '%s' in %s module.", Args->Name, Self->Root->Name.c_str());
      return ERR::NotFound;
   }
#elif __unix__
   #ifdef KOTUKU_STATIC
   if ((Args->Address = dlsym(RTLD_DEFAULT, Args->Name))) {
   #else
   if ((!Self->Root) or (!Self->Root->LibraryBase)) return ERR::FieldNotSet;
   if ((Args->Address = dlsym(Self->Root->LibraryBase, Args->Name))) {
   #endif
      return ERR::Okay;
   }
   else {
      log.msg("Failed to resolve '%s' in %s module.", Args->Name, Self->Root->Name.c_str());
      return ERR::NotFound;
   }
#else
   #warning Platform not supported.
   return ERR::NoSupport;
#endif
}

/*********************************************************************************************************************

-METHOD-
Test: Run unit tests for the module, if present.

If a module is compiled with unit tests, calling the Test() method will execute them.  There is no explicit contract
for what the unit tests should do, but it is typically expected that test results will be printed to the log.

Unit tests should never be compiled into production releases of the code.

-INPUT-
cstr Options: Optional CSV list of testing options.
&int Passed: The number of tests that passed will be returned in this parameter.
&int Total: The total number of tests that were executed will be returned in this parameter.

-ERRORS-
Okay
NoSupport: Unit tests are not defined for the module.

**********************************************************************************************************************/

static ERR MODULE_Test(extModule *Self, struct mod::Test *Args)
{
   if (Self->Root->Test) {
      Self->Root->Test(Args->Options, &Args->Passed, &Args->Total);
      return ERR::Okay;
   }
   else return ERR::NoSupport;
}

/*********************************************************************************************************************

-FIELD-
Defs: Returns the compiled IDL definition for the module.

Returns the IDL definition string that was compiled from the module's TDL file.  Useful for introspection purposes.

**********************************************************************************************************************/

static ERR GET_Defs(extModule *Self, CSTRING *Value)
{
   if ((Self->Root) and (Self->Root->Header)) *Value = Self->Root->Header->Definitions;
   else *Value = nullptr;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Flags: Optional flags.
Lookup: MOF

-FIELD-
FunctionList: Refers to a list of public functions exported by the module.

After initialisation, the FunctionList will refer to an array of public functions that are exported by the module.  The
FunctionList array consists of !Function structs in the following format:

<struct lookup="Function"/>

-FIELD-
Header: For internal usage only.
Status: private

Setting the module Table prior to initialisation allows virtual modules to be created that reside in memory rather
than on solid media.

**********************************************************************************************************************/

static ERR SET_Header(extModule *Self, struct ModHeader *Value)
{
   if (!Value) return ERR::NullArgs;
   Self->Header = Value;
   return ERR::Okay;
}

/*********************************************************************************************************************

-FIELD-
Root: For internal use only.
Status: private

A loaded module can only reside once in memory, irrespective of how many module objects are used to represent it.  The
Root field refers to a RootModule object that reflects this single instance of the module.

-FIELD-
ModBase: The Module's function base (jump table) must be read from this field.

Initialising a module will create a jump table that is referenced in the ModBase field.  The jump table contains
vectors that point to all functions that are published by the module.  This is considered an internal feature that
is hidden by system headers.

If the module is unloaded at any time then the jump table becomes invalid.

-FIELD-
Name: The name of the module.

This string pointer specifies the name of the module.  This name will be used to load the module from the `modules:`
folder, so this field actually reflects part of the module file name.  It is also possible to specify
sub-directories before the module name itself - this could become more common in module loading in future.

It is critical that file extensions do not appear in the Name string, e.g. `display.dll` as not all systems
may use a `.dll` extension.

**********************************************************************************************************************/

static ERR GET_Name(extModule *Self, CSTRING *Value)
{
   *Value = Self->Name.c_str();
   return ERR::Okay;
}

static ERR SET_Name(extModule *Self, CSTRING Name)
{
   if (!Name) return ERR::Okay;

   Self->Name.assign(Name);
   std::transform(Self->Name.begin(), Self->Name.end(), Self->Name.begin(),
      [](unsigned char c){ return std::tolower(c); });
   return ERR::Okay;
}

//********************************************************************************************************************
// Builds jump tables that link programs to modules.

#ifndef KOTUKU_STATIC
APTR build_jump_table(const Function *FList)
{
   if (!FList) return nullptr;

   pf::Log log(__FUNCTION__);

   int size;
   for (size=0; FList[size].Address; size++);

   log.trace("%d functions have been detected in the function list.", size);

   void **functions;
   if (AllocMemory((size+1) * sizeof(APTR), MEM::NO_CLEAR|MEM::UNTRACKED, (APTR *)&functions, nullptr) IS ERR::Okay) {
      for (int i=0; i < size; i++) functions[i] = FList[i].Address;
      functions[size] = nullptr;
      return functions;
   }
   else log.warning(ERR::AllocMemory);
   return nullptr;
}
#endif

//********************************************************************************************************************
// Searches the system for a RootModule header that matches the Module details.  The module must have been
// loaded into memory in order for this function to return successfully.

static RootModule * check_resident(extModule *Self, const std::string_view ModuleName)
{
   static bool kminit = false;

   if (iequals("core", ModuleName)) {
      if (!kminit) {
         kminit = true;
         // NB: The Object constructor clears all values initially.
         glCoreRoot.Class         = glRootModuleClass;
         glCoreRoot.UID           = 1;
         glCoreRoot.setFlag(NF::INITIALISED|NF::NAME|NF::UNIQUE);
         glCoreRoot.Name          = "Core";
         glCoreRoot.OpenCount     = 1;
         glCoreRoot.Table         = &glCoreHeader;
         glCoreRoot.Header        = &glCoreHeader;
      }
      Self->FunctionList = glFunctions;
      return &glCoreRoot;
   }
   else if (auto master = glModuleList) {
      while (master) {
         auto record_name = std::string_view(master->Name);

         auto sep = record_name.find_last_of(":/");
         if (sep != std::string::npos) record_name.remove_prefix(sep+1);

         sep = record_name.find_last_of(".");
         if (sep != std::string::npos) record_name.remove_suffix(record_name.size() - sep);

         if (iequals(record_name, ModuleName)) return master;
         master = master->Next;
      }
   }

   return nullptr;
}

//********************************************************************************************************************

static void free_module(MODHANDLE handle)
{
   pf::Log log(__FUNCTION__);

   if (!handle) return;

   log.traceBranch("%p", handle);

   #ifdef ANALYSIS_ENABLED
      // Library closure is disabled when code analysis is turned on so that code
      // addresses can be looked up correctly.
   #else
      #ifdef __unix__
         dlclose(handle);
      #elif _WIN32
         winFreeLibrary(handle);
      #else
         #error You need to write machine specific code to expunge modules.
      #endif
   #endif
}

//********************************************************************************************************************

static const FunctionField argsResolveSymbol[] = { { "Name", FD_STR }, { "Address", FD_PTR|FD_RESULT }, { nullptr, 0 } };
static const FunctionField argsTest[] = {
   { "Options", FD_STR },
   { "Passed", FD_INT|FD_RESULT },
   { "Total", FD_INT|FD_RESULT },
   { nullptr, 0 }
};

static const MethodEntry glModuleMethods[] = {
   { mod::ResolveSymbol::id, (APTR)MODULE_ResolveSymbol, "ResolveSymbol", argsResolveSymbol, sizeof(struct mod::ResolveSymbol) },
   { mod::Test::id, (APTR)MODULE_Test, "Test", argsTest, sizeof(mod::Test) },
   { AC::NIL, nullptr, nullptr, nullptr, 0 }
};

//********************************************************************************************************************

static const FieldArray glRootModuleFields[] = {
   { "Header", FDF_POINTER|FDF_RI, ROOTMODULE_GET_Header },
   END_FIELD
};

static const ActionArray glRootModuleActions[] = {
   { AC::Free, ROOTMODULE_Free },
   { AC::NewPlacement, ROOTMODULE_NewPlacement },
   { AC::NIL, nullptr }
};

//********************************************************************************************************************

extern ERR add_module_class(void)
{
   if (!(glModuleClass = extMetaClass::create::global(
      fl::BaseClassID(CLASSID::MODULE),
      fl::ClassVersion(VER_MODULE),
      fl::Name("Module"),
      fl::Category(CCF::SYSTEM),
      fl::FileExtension("*.mod|*.so|*.dll"),
      fl::FileDescription("System Module"),
      fl::Icon("tools/cog"),
      fl::Actions(glModuleActions),
      fl::Methods(glModuleMethods),
      fl::Fields(glModuleFields),
      fl::Size(sizeof(extModule)),
      fl::Path("modules:core")))) return ERR::AddClass;

   if (!(glRootModuleClass = extMetaClass::create::global(
      fl::BaseClassID(CLASSID::ROOTMODULE),
      fl::ClassVersion(1.0),
      fl::Name("RootModule"),
      fl::Flags(CLF::NO_OWNERSHIP),
      fl::Category(CCF::SYSTEM),
      fl::Actions(glRootModuleActions),
      fl::Fields(glRootModuleFields),
      fl::Size(sizeof(RootModule)),
      fl::Path("modules:core")))) return ERR::AddClass;

   return ERR::Okay;
}
