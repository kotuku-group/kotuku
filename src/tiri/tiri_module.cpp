
#define PRV_SCRIPT
#define PRV_TIRI
#define PRV_TIRI_MODULE
#include <kotuku/main.h>
#include <kotuku/modules/module.h>
#include <kotuku/strings.hpp>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_str.h"
#include "lj_struct.h"
#include "lj_tab.h"

#include "defs.h"
#include "lj_proto_registry.h"

#include <ffi.h>
#include <utility>
#include <unordered_map>
#include <algorithm>
#include <mutex>
#include <new>
#include <limits>

struct static_module_function_signature {
   std::string Name;
   std::vector<std::string> FieldNames;
   std::vector<FunctionField> Fields;
};

// Immutable per-module signature.  This is the sole owner of the canonical function names and copied FunctionField
// metadata: compile-time queries and the runtime ModuleCallable records both read these copies rather than the loaded
// module's original Function list, so a name or field descriptor has one representation with one lifetime.
//
// The function-name index removes the linear scan that compile-time queries previously performed; it maps a
// case-insensitive hash to every ordinal sharing that hash, so the canonical name is always revalidated after the
// lookup and a collision cannot resolve to the wrong function.  Because Functions is built in the module's export
// order and ModuleBinding::Callables is built from it, one ordinal addresses both representations.

constexpr uint32_t NO_FUNCTION_ORDINAL = std::numeric_limits<uint32_t>::max();

struct static_module_signature {
   std::string Name;
   std::vector<static_module_function_signature> Functions;
   ankerl::unordered_dense::map<uint32_t, std::vector<uint32_t>> FunctionIndex;

   [[nodiscard]] uint32_t find_ordinal(std::string_view Name) const noexcept {
      if (auto it = FunctionIndex.find(strihash(Name)); it != FunctionIndex.end()) {
         for (auto slot : it->second) {
            if (kt::iequals(Functions[slot].Name, Name)) return slot;
         }
      }
      return NO_FUNCTION_ORDINAL;
   }

   [[nodiscard]] const static_module_function_signature * find(std::string_view Name) const noexcept {
      auto ordinal = find_ordinal(Name);
      return (ordinal IS NO_FUNCTION_ORDINAL) ? nullptr : &Functions[ordinal];
   }
};

// The native argument limit.  A signature is rejected during preparation if it declares more than this many native
// parameters, so every bounded store in the call bridge can be sized from it and can never overflow at call time.
//
// Script callers supply at most MAX_MODULE_ARGS values as well: argument i of the signature reads script index i, so
// the two limits are one contract rather than two.  Result and span arguments consume a signature slot without
// consuming a script value, which makes the script side of the contract an upper bound rather than an exact count.

constexpr int MAX_MODULE_ARGS = 16;
constexpr size_t BUFFER_ELEMENT_SIZE = 16;
constexpr size_t BUFFER_SIZE = MAX_MODULE_ARGS * BUFFER_ELEMENT_SIZE;
constexpr size_t MAX_STRING_PREFIX_LENGTH = 200;

struct ModuleBinding;

// Precomputed counts of the bridge-owned temporaries one signature can require.  Deriving these once per callable lets
// the call bridge construct only the elements a signature actually uses, and lets preparation reject a signature whose
// demands would exceed a bounded store instead of discovering the overflow mid-call.
//
struct marshalling_profile {
   uint8_t Strings = 0;         // Mutable std::string temporaries (FD_STR|FD_CPP with FD_MUTABLE or FD_RESULT)
   uint8_t StringViews = 0;     // std::string_view temporaries (FD_STR|FD_CPP, read-only)
   uint8_t MutableStrings = 0;  // Copy-back records for mutable std::string arguments
   uint8_t Arrays = 0;          // kt::vector<> results
   uint8_t Spans = 0;           // std::span wrappers

   // True when the signature needs no bridge-owned temporary at all.  Scalar-only and read-only C-string calls take
   // this path and perform no heap allocation of their own.
   [[nodiscard]] bool trivial_storage() const noexcept {
      return not (Strings or StringViews or MutableStrings or Arrays or Spans);
   }
};

// One process-wide callable record per exported module function.  Records are allocated individually so that their
// addresses remain stable from publication until expunge_modules(), which is required because Tiri closures retain a
// raw pointer to their record and the cached ffi_cif refers to ArgTypes in place.
//
// Every field is written once during preparation and is immutable afterwards, so concurrent calls from independent
// Tiri states can share one record without synchronisation.

struct ModuleCallable {
   CSTRING Name = nullptr;               // Canonical name, owned by the binding's immutable signature
   APTR Address = nullptr;               // Native function address; leave as null if function wasn't processed
   const FunctionField *Fields = nullptr; // Argument metadata, owned by the binding's immutable signature

   marshalling_profile Profile;          // Upper bounds on the bridge-owned temporaries this signature can require

   ffi_cif Cif = { };
   ffi_type *ArgTypes[MAX_MODULE_ARGS] = { };

   inline bool trivial() { return Fields IS nullptr; }

   inline bool valid() { // Returns false if the function couldn't be processed
      return Address != nullptr;
   }

   inline void invalidate() { Address = nullptr; }
};

// Address-stable bounded storage for the call bridge's temporaries.
//
// Elements live in raw aligned storage and are constructed only as they are used, so a signature that needs none pays
// nothing beyond the (stack-resident) storage itself.  Because the capacity is fixed and validated during preparation,
// an element's address never changes once it has been handed to libffi -- the property the previous reserve(8) vectors
// only provided by accident.
//
// emplace() returns null when the store is full.  Preparation rejects any signature that could reach that point, so a
// null return indicates a profile/marshaller disagreement rather than an expected condition.

template <class T, size_t Capacity> class bounded_store {
   alignas(T) uint8_t Storage[Capacity * sizeof(T)];
   size_t Count = 0;

   [[nodiscard]] T * slot(size_t Index) noexcept { return (T *)(Storage + (Index * sizeof(T))); }

   public:
   bounded_store() = default;
   bounded_store(const bounded_store &) = delete;
   bounded_store & operator=(const bounded_store &) = delete;

   ~bounded_store() {
      while (Count) slot(--Count)->~T();
   }

   template <class... Args> [[nodiscard]] T * emplace(Args &&...Arguments) {
      if (Count >= Capacity) return nullptr;
      auto element = new (slot(Count)) T(std::forward<Args>(Arguments)...);
      Count++;
      return element;
   }

   [[nodiscard]] size_t size() const noexcept { return Count; }

   T & operator[](size_t Index) noexcept { return *slot(Index); }
};

// Records how far the module's IDL definitions (constants and structures) have progressed.  The state belongs to the
// module binding rather than a separate name set, so that a module and the definitions it published cannot disagree.

enum class def_state : uint8_t {
   Unloaded = 0, // No definition processing has been attempted
   Loaded,       // Definitions were published in full
   Failed        // Definition processing failed; the batch was rolled back
};

// Callables is parallel to Signature->Functions: element i describes the module's i'th export in both. Runtime name
// lookup therefore uses the signature's index to obtain an ordinal and then addresses Callables directly, so the
// compiler and the call bridge share one collision-safe, case-insensitive mapping.

struct ModuleBinding {
   std::string Name;
   objModule *Module = nullptr;
   std::unique_ptr<const static_module_signature> Signature;
   std::vector<std::unique_ptr<ModuleCallable>> Callables;

   // Definition-loading state is guarded by glConstantMutex rather than the registry mutex.  DefError retains the
   // failure so that a repeated request reports the original cause instead of silently succeeding.
   def_state DefState = def_state::Unloaded;
   ERR DefError = ERR::Okay;

   ~ModuleBinding() {
      if (Module) FreeResource(Module);
   }
};

// One index entry.  The spelling under which the binding was registered is stored alongside it, because an alias is
// reachable under a name that differs from the binding's canonical name.  Retaining the spelling lets the lookup
// revalidate it after hashing, so a hash collision can never resolve to the wrong module.

struct ModuleIndexEntry {
   std::string Name;
   ModuleBinding *Binding = nullptr;
};

// The process-wide module registry.  See MODULE_REGISTRY.md for its lifecycle and lock contract.  Ownership and lookup
// are deliberately separate: Bindings owns every binding exactly once, while Index maps a case-insensitive name hash
// to non-owning entries.  Because bindings are allocated individually, growth of either container never moves a
// published binding, its signature or its callables.  That stability is required, as Tiri closures and compiler
// symbols retain raw pointers until expunge_modules().
//
// Separating the two also lets one binding be reached through several spellings (the requested name and the module's
// canonical name) without any shared-ownership scheme.

struct ModuleRegistry {
   std::mutex Mutex;
   std::vector<std::unique_ptr<ModuleBinding>> Bindings;
   ankerl::unordered_dense::map<uint32_t, std::vector<ModuleIndexEntry>> Index;
   bool Expunging = false;
};

static ModuleRegistry glModuleRegistry;

// Locate a published binding.  The caller must hold glModuleRegistry.Mutex.

static ModuleBinding * find_module_binding(std::string_view Name)
{
   if (auto it = glModuleRegistry.Index.find(strihash(Name)); it != glModuleRegistry.Index.end()) {
      for (const auto &entry : it->second) {
         if (kt::iequals(entry.Name, Name)) return entry.Binding;
      }
   }
   return nullptr;
}

// Make Binding reachable under Name.  The caller must hold glModuleRegistry.Mutex.  Registering the requested
// spelling in addition to the canonical one lets a later lookup for either spelling hit the index directly.

static void publish_module_alias(std::string_view Name, ModuleBinding *Binding)
{
   auto &bucket = glModuleRegistry.Index[strihash(Name)];
   for (const auto &entry : bucket) {
      if (kt::iequals(entry.Name, Name)) return; // Already reachable under this spelling
   }
   bucket.push_back({ std::string(Name), Binding });
}

template<class... Args> void RMSG(Args...) {
   //log.msg(Args)
}

struct module_span_arg {
   std::span<int8_t> Span;

   APTR set(CPTR Data, size_t Extent) noexcept {
      Span = std::span<int8_t>((int8_t *)Data, Extent);
      return &Span;
   }
};

// Resolve and validate the element metadata for a callable array.

static ERR module_span_element_metadata(lua_State *Lua, const FunctionField &Field, AET *ElementType,
   size_t *ElementSize, struct_record **StructDef)
{
   *StructDef = nullptr;

   if (Field.Type & FD_STRUCT) {
      if ((Field.Type & FD_PTR) or (not Field.Name) or (not valid_struct_name(Field.Name))) return ERR::InvalidData;
      auto definition = find_struct(Lua, struct_name_prefix(Field.Name));
      if (not definition) return ERR::Search;
      if (definition->Size <= 0) return ERR::InvalidData;
      *ElementType = AET::STRUCT;
      *ElementSize = size_t(definition->Size);
      *StructDef = definition;
      return ERR::Okay;
   }

   const int type_count = bool(Field.Type & FD_DOUBLE) + bool(Field.Type & FD_INT64) +
      bool(Field.Type & FD_FLOAT) + bool(Field.Type & FD_INT) + bool(Field.Type & FD_WORD) +
      bool(Field.Type & FD_BYTE) + bool(Field.Type & FD_PTR);
   if (type_count != 1) return ERR::InvalidData;

   if (Field.Type & FD_DOUBLE) { *ElementType = AET::DOUBLE; *ElementSize = sizeof(double); }
   else if (Field.Type & FD_INT64) { *ElementType = AET::INT64; *ElementSize = sizeof(int64_t); }
   else if (Field.Type & FD_FLOAT) { *ElementType = AET::FLOAT; *ElementSize = sizeof(float); }
   else if (Field.Type & FD_INT) { *ElementType = AET::INT32; *ElementSize = sizeof(int); }
   else if (Field.Type & FD_WORD) { *ElementType = AET::INT16; *ElementSize = sizeof(int16_t); }
   else if (Field.Type & FD_BYTE) { *ElementType = AET::BYTE; *ElementSize = sizeof(int8_t); }
   else { *ElementType = AET::PTR; *ElementSize = sizeof(APTR); }

   return ERR::Okay;
}

// One constant parsed from a module's IDL, retained with its full canonical name.
//
// The registry is keyed by hash alone, so the name is preserved here to make a collision or a conflicting redefinition
// diagnosable: without it, two different constants that hash alike would silently reduce to one value with no way to
// report which module supplied either.

struct staged_constant {
   std::string Name;
   TiriConstant Value;
};

// A module's definitions, parsed but not yet published.  Staging the whole module before committing keeps publication
// atomic, so a malformed definition cannot leave half of a module's constants and structures visible.

struct definition_batch {
   std::string Source;                       // Module supplying the definitions, retained for diagnostics
   std::vector<staged_constant> Constants;
   std::vector<std::pair<std::string, std::string>> Structs; // Name, definition sequence
};

[[nodiscard]] static ERR load_include_struct(CSTRING, std::string_view, CSTRING *, definition_batch &);
[[nodiscard]] static CSTRING load_include_constant(CSTRING, std::string_view, definition_batch &);

static int module_call(lua_State *);
static ERR module_call_inner(lua_State *, std::string &, int &);
static int process_results(extTiri *, APTR, const FunctionField *);

static std::unique_ptr<const static_module_signature> make_module_signature(
   std::string_view Name, const Function *Functions)
{
   if (not Functions) return {};

   auto signature = std::make_unique<static_module_signature>();
   signature->Name = Name;

   size_t function_count = 0;
   while (Functions[function_count].Name) function_count++;
   signature->Functions.reserve(function_count);

   for (size_t i = 0; i < function_count; ++i) {
      auto &entry = signature->Functions.emplace_back();
      entry.Name = Functions[i].Name;
      signature->FunctionIndex[strihash(entry.Name)].push_back(uint32_t(i));

      if (const FunctionField *fields = Functions[i].Args) {
         size_t field_count = 0;
         while (fields[field_count].Name) field_count++;
         entry.FieldNames.reserve(field_count);
         entry.Fields.reserve(field_count + 1);
         for (size_t j = 0; j < field_count; ++j) entry.FieldNames.emplace_back(fields[j].Name);
         for (size_t j = 0; j < field_count; ++j) {
            entry.Fields.push_back({ entry.FieldNames[j].c_str(), fields[j].Type });
         }
         entry.Fields.push_back({ nullptr, 0 });
      }
      else entry.Fields.push_back({ nullptr, 0 });
   }

   return signature;
}

StaticModuleHandle static_module_by_name(std::string_view Name) noexcept
{
   std::lock_guard lock(glModuleRegistry.Mutex);
   if (glModuleRegistry.Expunging) return nullptr;
   auto binding = find_module_binding(Name);
   return binding ? binding->Signature.get() : nullptr;
}

const FunctionField * static_module_function(
   StaticModuleHandle Module, std::string_view Name) noexcept
{
   if (not Module) return nullptr;
   auto function = Module->find(Name);
   return function ? function->Fields.data() : nullptr;
}

std::string_view static_module_name(StaticModuleHandle Module) noexcept
{
   return Module ? std::string_view(Module->Name) : std::string_view();
}

std::string_view static_module_function_name(StaticModuleHandle Module, std::string_view Name) noexcept
{
   if (not Module) return {};
   auto function = Module->find(Name);
   return function ? std::string_view(function->Name) : std::string_view();
}

//********************************************************************************************************************
// Determine the libffi type for one argument.  The mapping mirrors the argument marshalling in module_call_inner();
// both must agree on the number and type of arguments passed to ffi_call().
//
// Signature-dependent validation that does not require a lua_State is performed here so that it happens once per
// process rather than on every call.  State-dependent checks, such as resolving a named structure for a span, remain
// in the call bridge.

static ERR callable_arg_type(const FunctionField &Field, ffi_type *&Type, std::string &ErrorMsg)
{
   const int argtype = Field.Type;

   if ((argtype & FDF_SPAN) IS FDF_SPAN) {
      if (argtype & (FD_RESULT|FD_ALLOC)) {
         ErrorMsg = std::format("Unsupported span result for arg '{}'.", Field.Name);
         return ERR::NoSupport;
      }
      Type = &ffi_type_pointer;
      return ERR::Okay;
   }

   if (argtype & FD_ARRAY) { // Non-span arrays are no longer marshalled
      ErrorMsg = std::format("Unsupported pointer array arg '{}'.", Field.Name);
      return ERR::NoSupport;
   }

   if (argtype & FD_RESULT) {
      // Every result argument is passed as a pointer to storage that the bridge provides.
      if ((((argtype & FDF_VECTOR) IS FDF_VECTOR) and (argtype & FD_MUTABLE)) or
          (argtype & (FD_STR|FD_PTR|FD_INT|FD_DOUBLE|FD_INT64))) {
         Type = &ffi_type_pointer;
         return ERR::Okay;
      }
      ErrorMsg = std::format("Unrecognised result arg '{}', flags ${:08x}.", Field.Name, argtype);
      return ERR::InvalidType;
   }

   if (argtype & FD_FUNCTION) { Type = &ffi_type_pointer; return ERR::Okay; }
   if (argtype & FD_STR) { Type = &ffi_type_pointer; return ERR::Okay; }

   if ((argtype & FDF_VECTOR) IS FDF_VECTOR) {
      ErrorMsg = "C++ array inputs are not supported.";
      return ERR::NoSupport;
   }

   if (argtype & FD_PTR) { Type = &ffi_type_pointer; return ERR::Okay; }
   if (argtype & FD_INT) { Type = &ffi_type_sint32; return ERR::Okay; }
   if (argtype & FD_DOUBLE) { Type = &ffi_type_double; return ERR::Okay; }
   if (argtype & FD_INT64) { Type = &ffi_type_sint64; return ERR::Okay; }

   if (argtype & (FD_TAGS|FD_VARTAGS)) {
      ErrorMsg = "Functions using tags are not supported.";
      return ERR::NoSupport;
   }

   ErrorMsg = std::format("Unsupported arg '{}', flags ${:08x}.", Field.Name, argtype);
   return ERR::NoSupport;
}

//********************************************************************************************************************
// Accumulate the bridge-owned temporaries one argument can require.  The classification must agree with the
// marshalling loop in module_call_inner().  A temporary counted here but not used is merely wasteful, whereas one
// used but not counted would overflow a bounded store.

static void profile_arg(const FunctionField &Field, marshalling_profile &Profile)
{
   const int argtype = Field.Type;

   if ((argtype & FDF_SPAN) IS FDF_SPAN) { Profile.Spans++; return; }

   if (argtype & FD_RESULT) {
      if (((argtype & FDF_VECTOR) IS FDF_VECTOR) and (argtype & FD_MUTABLE)) Profile.Arrays++;
      else if ((argtype & FD_STR) and (argtype & FD_CPP)) {
         // A mutable result is backed by a std::string the bridge owns; a read-only one by a std::string_view.
         if (argtype & FD_MUTABLE) Profile.Strings++;
         else Profile.StringViews++;
      }
      return;
   }

   if (argtype & FD_FUNCTION) return; // Isolated function lives in the frame, not a store

   if (argtype & FD_STR) {
      if (argtype & FD_CPP) {
         if (argtype & FD_MUTABLE) { Profile.Strings++; Profile.MutableStrings++; }
         else Profile.StringViews++;
      }
      return;
   }

}

//********************************************************************************************************************
// Determine the libffi return type from the function's result descriptor, which is stored in element zero of the
// argument list.

static ffi_type * callable_return_type(int ResultType)
{
   if (ResultType & FD_STR) return &ffi_type_pointer;
   if (ResultType & FD_OBJECT) return &ffi_type_pointer;
   if (ResultType & FD_PTR) return &ffi_type_pointer;
   if (ResultType & (FD_INT|FD_ERROR)) {
      return (ResultType & FD_UNSIGNED) ? &ffi_type_uint32 : &ffi_type_sint32;
   }
   if (ResultType & FD_DOUBLE) return &ffi_type_double;
   if (ResultType & FD_INT64) return &ffi_type_sint64;
   return &ffi_type_void;
}

//********************************************************************************************************************
// Build the process-wide callable records for a module.  Called once, while the module is being resolved and before
// it is published to the registry, so no other thread can observe a partially prepared record.
//
// The binding's signature must already be built, because it owns the canonical names and copied FunctionField
// metadata that the callables refer to.  The loaded Function list is consulted only for the native addresses, which
// the signature deliberately does not retain.  One callable is emitted per signature entry, in signature order, so
// that an ordinal from the signature's index addresses the matching callable.
//
// A preparation failure is recorded on the individual callable rather than propagated, because one unsupported
// signature must not prevent the rest of the module from being used.

static void prepare_module_callables(ModuleBinding *Module, const Function *Functions)
{
   kt::Log log(__FUNCTION__);

   if ((not Functions) or (not Module->Signature)) return;

   const auto &signatures = Module->Signature->Functions;
   Module->Callables.reserve(signatures.size());

   for (size_t index = 0; index < signatures.size(); ++index) {
      const auto &function = Functions[index];
      const auto &signature = signatures[index];
      auto callable = std::make_unique<ModuleCallable>();

      callable->Name    = signature.Name.c_str();
      callable->Address = function.Address;
      callable->Fields  = function.Args ? signature.Fields.data() : nullptr;

      if (not function.Args) {
         // A function with no argument list takes no parameters and returns nothing, so libffi is unnecessary.
         Module->Callables.push_back(std::move(callable));
         continue;
      }

      const FunctionField *args = callable->Fields;
      int total = 0;
      marshalling_profile profile;
      size_t front_bytes = 0; // Argument slots, allocated from the start of the call buffer
      size_t back_bytes = 0;  // Result storage, allocated from the end of the call buffer

      for (int arg = 1; args[arg].Name; ++arg) {
         if (total >= MAX_MODULE_ARGS) {
            callable->invalidate();
            log.msg("Function '%s' exceeds the %d argument limit.", callable->Name, MAX_MODULE_ARGS);
            break;
         }

         ffi_type *type = nullptr;
         std::string message;
         if (auto error = callable_arg_type(args[arg], type, message); error != ERR::Okay) {
            callable->invalidate();
            log.msg("Function '%s': %s", callable->Name, message.c_str());
            break;
         }
         callable->ArgTypes[total++] = type;
         profile_arg(args[arg], profile);

         // Mirror the call buffer's two-ended allocation so that an overlap is rejected here rather than corrupting
         // an argument slot at call time.  The front slot is sized as the marshaller sizes it; the back allocation
         // applies only to the result kinds that write into buffer-tail storage.

         const int argtype = args[arg].Type;
         if ((argtype & FDF_SPAN) IS FDF_SPAN) front_bytes += sizeof(APTR);
         else if (argtype & FD_RESULT) {
            front_bytes += sizeof(APTR);
            if (((argtype & FDF_VECTOR) IS FDF_VECTOR) and (argtype & FD_MUTABLE)) { }
            else if ((argtype & FD_STR) and (argtype & FD_CPP)) { }
            else if (argtype & (FD_STR|FD_PTR)) back_bytes += sizeof(APTR);
            else if (argtype & FD_INT) back_bytes += sizeof(int);
            else if (argtype & (FD_DOUBLE|FD_INT64)) back_bytes += sizeof(int64_t);
         }
         else if (argtype & FD_FUNCTION) front_bytes += sizeof(FUNCTION *);
         else if (argtype & FD_STR) front_bytes += sizeof(APTR);
         else if (argtype & FD_PTR) front_bytes += sizeof(APTR);
         else if (argtype & FD_INT) front_bytes += sizeof(int);
         else if (argtype & FD_DOUBLE) front_bytes += sizeof(double);
         else if (argtype & FD_INT64) front_bytes += sizeof(int64_t);
      }

      if (callable->valid() and (front_bytes + back_bytes > BUFFER_SIZE)) {
         callable->invalidate();
         log.msg("Function '%s' requires %zu call buffer bytes, exceeding the %zu byte limit.",
            callable->Name, front_bytes + back_bytes, BUFFER_SIZE);
      }

      // Every bounded store is sized at MAX_MODULE_ARGS, and each temporary is required by a distinct argument, so a
      // signature within the argument limit cannot exhaust one.  The check is retained because it is the contract the
      // marshaller relies on to treat a full store as unreachable.

      if (callable->valid()) {
         if ((profile.Strings > MAX_MODULE_ARGS) or (profile.StringViews > MAX_MODULE_ARGS) or
             (profile.MutableStrings > MAX_MODULE_ARGS) or (profile.Arrays > MAX_MODULE_ARGS) or
             (profile.Spans > MAX_MODULE_ARGS)) {
            callable->invalidate();
            log.msg("Function '%s' exceeds the bounded temporary storage limit.", callable->Name);
         }
      }

      if (callable->valid()) {
         callable->Profile = profile;
         ffi_type *return_type = callable_return_type(args->Type);

         if (ffi_prep_cif(&callable->Cif, FFI_DEFAULT_ABI, total, return_type, callable->ArgTypes) != FFI_OK) {
            callable->invalidate();
            log.msg("Failed to prepare the call interface for '%s'.", callable->Name);
         }
      }

      Module->Callables.push_back(std::move(callable));
   }
}

//********************************************************************************************************************
// Resolve one process-wide module instance.  Entries remain stable until the Tiri module is expunged.

static ERR resolve_module(std::string_view Name, ModuleBinding *&Result)
{
   {
      std::lock_guard lock(glModuleRegistry.Mutex);
      if (glModuleRegistry.Expunging) return ERR::InvalidState;
      if (auto binding = find_module_binding(Name)) {
         Result = binding;
         return ERR::Okay;
      }
   }

   // Module creation runs external initialisation code that can re-enter the registry, so it is performed without
   // holding the index lock.  Two threads racing on the same name may therefore both construct a module; the loser's
   // instance is discarded below so that exactly one binding is ever published.

   auto loaded_module = objModule::create::global(fl::Name(Name));
   if (not loaded_module) return ERR::CreateObject;

   auto entry = std::make_unique<ModuleBinding>();
   entry->Module = loaded_module;
   const Function *functions = nullptr;
   loaded_module->getFunctionList(functions);

   std::string_view canonical_name;
   if (loaded_module->getName(canonical_name) != ERR::Okay or canonical_name.empty()) canonical_name = Name;
   entry->Name = canonical_name;
   entry->Signature = make_module_signature(entry->Name, functions);
   prepare_module_callables(entry.get(), functions);

   std::lock_guard lock(glModuleRegistry.Mutex);
   if (glModuleRegistry.Expunging) return ERR::InvalidState;

   // Re-check under the lock; another thread may have published this module while it was being prepared.
   if (auto binding = find_module_binding(Name)) {
      Result = binding;
      return ERR::Okay;
   }
   if (auto binding = find_module_binding(entry->Name)) { // Requested an alias of an already published module
      publish_module_alias(Name, binding);
      Result = binding;
      return ERR::Okay;
   }

   Result = entry.get();
   glModuleRegistry.Bindings.push_back(std::move(entry));
   publish_module_alias(Result->Name, Result);
   publish_module_alias(Name, Result);
   return ERR::Okay;
}

// Destroy every registered module.  All Tiri states must be unable to execute before this runs, because published
// ModuleCallable addresses are retained by closures and compiler symbols for the lifetime of the registry.
//
// The index holds only non-owning pointers, so it is discarded before the bindings it refers to.  Binding destruction
// runs after releasing the registry mutex because FreeResource() executes external module teardown code that must be
// free to re-enter registry-facing code without deadlocking.

void expunge_modules()
{
   std::vector<std::unique_ptr<ModuleBinding>> bindings;
   {
      std::lock_guard lock(glModuleRegistry.Mutex);
      glModuleRegistry.Expunging = true;
      glModuleRegistry.Index.clear();
      bindings.swap(glModuleRegistry.Bindings);
   }
}

//********************************************************************************************************************
// Support for mutable array results

struct cpp_array_result {
   APTR Data = nullptr;
   void (*Delete)(APTR) = nullptr;

   cpp_array_result() = default;
   cpp_array_result(const cpp_array_result &) = delete;
   cpp_array_result & operator=(const cpp_array_result &) = delete;

   cpp_array_result(cpp_array_result &&Other) noexcept:
      Data(Other.Data),
      Delete(Other.Delete)
   {
      Other.Data = nullptr;
      Other.Delete = nullptr;
   }

   cpp_array_result & operator=(cpp_array_result &&Other) noexcept
   {
      if (this != &Other) {
         if ((Data) and (Delete)) Delete(Data);
         Data = Other.Data;
         Delete = Other.Delete;
         Other.Data = nullptr;
         Other.Delete = nullptr;
      }
      return *this;
   }

   ~cpp_array_result()
   {
      if ((Data) and (Delete)) Delete(Data);
   }
};

template <class T> static void delete_cpp_array_result(APTR Result)
{
   delete (kt::vector<T> *)Result;
}

template <class T> static ERR make_cpp_array_result(cpp_array_result *Result)
{
   auto vector = new (std::nothrow) kt::vector<T>;
   if (not vector) return ERR::AllocMemory;

   Result->Data = vector;
   Result->Delete = delete_cpp_array_result<T>;
   return ERR::Okay;
}

// Create an empty kt::vector<T> suitable for passing to a module API function.

static ERR make_cpp_array_result(int Type, cpp_array_result *Result)
{
   if (Type & FD_STR) return make_cpp_array_result<std::string>(Result);
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) return make_cpp_array_result<OBJECTPTR>(Result);
   else if (Type & FD_PTR) return make_cpp_array_result<APTR>(Result);
   else if (Type & FD_DOUBLE) return make_cpp_array_result<double>(Result);
   else if (Type & FD_FLOAT) return make_cpp_array_result<float>(Result);
   else if (Type & FD_INT64) return make_cpp_array_result<int64_t>(Result);
   else if (Type & FD_INT) return make_cpp_array_result<int>(Result);
   else if (Type & FD_WORD) return make_cpp_array_result<int16_t>(Result);
   else if (Type & FD_BYTE) return make_cpp_array_result<uint8_t>(Result);
   else return ERR::NoSupport;
}

template <class T> static void push_cpp_array_result(lua_State *Lua, int Type, std::string_view Name, APTR Source)
{
   auto vector = (kt::vector<T> *)Source;
   make_any_array(Lua, Type, Name, int(vector->size()), vector->data());
}

static bool push_cpp_array_result(lua_State *Lua, int Type, std::string_view Name, APTR Source)
{
   if (Type & FD_STR) {
      auto vector = (kt::vector<std::string> *)Source;
      make_array(Lua, AET::STR_CPP, int(vector->size()), vector);
   }
   else if ((Type & FD_OBJECT) and (Type & FD_PTR)) push_cpp_array_result<OBJECTPTR>(Lua, Type, Name, Source);
   else if (Type & FD_PTR) push_cpp_array_result<APTR>(Lua, Type, Name, Source);
   else if (Type & FD_DOUBLE) push_cpp_array_result<double>(Lua, Type, Name, Source);
   else if (Type & FD_FLOAT) push_cpp_array_result<float>(Lua, Type, Name, Source);
   else if (Type & FD_INT64) push_cpp_array_result<int64_t>(Lua, Type, Name, Source);
   else if (Type & FD_INT) push_cpp_array_result<int>(Lua, Type, Name, Source);
   else if (Type & FD_WORD) push_cpp_array_result<int16_t>(Lua, Type, Name, Source);
   else if (Type & FD_BYTE) push_cpp_array_result<uint8_t>(Lua, Type, Name, Source);
   else return false;

   return true;
}

//********************************************************************************************************************

[[nodiscard]] static GCstr * string_arg(lua_State *Lua, int Arg) noexcept
{
   return strV(Lua->base + Arg - 1);
}

//********************************************************************************************************************

[[nodiscard]] static constexpr int8_t datatype(std::string_view String) noexcept
{
   auto it = std::ranges::find_if(String, [](char c) { return c > 0x20; });
   if (it IS String.end()) return 's';

   auto remaining = String.substr(it - String.begin());

   if (remaining.starts_with("0x")) {
      auto hex_digits = remaining.substr(2);
      if (hex_digits.empty()) return 's';
      bool all_hex = std::ranges::all_of(hex_digits, [](char c) { return std::isxdigit(c); });
      return all_hex ? 'h' : 's';
   }

   // Check for numeric content
   bool is_number = true;
   bool is_float  = false;

   for (char c : remaining) {
      if ((not std::isdigit(c)) and (c != '.') and (c != '-')) is_number = false;
      if (c IS '.') is_float = true;
   }

   if (is_float and is_number) return 'f';
   else if (is_number) return 'i';
   else return 's';
}

//********************************************************************************************************************
// Parse one constant group into the staging batch.
//
// Nothing is written to the process-wide registry here.  Definitions are staged so that a malformed entry later in the
// module's IDL cannot leave a partially populated registry behind; publish_definition_batch() commits the batch only
// once the whole module has parsed successfully.

static CSTRING load_include_constant(CSTRING Line, std::string_view Source, definition_batch &Batch)
{
   kt::Log log("load_module_defs");

   int i;
   for (i=0; (unsigned(Line[i]) > 0x20) and (Line[i] != ':'); i++);

   if (Line[i] != ':') {
      log.warning("Malformed const name in %.*s.", int(Source.size()), Source.data());
      return next_line(Line);
   }

   std::string name(Line, i);
   name.reserve(MAX_STRING_PREFIX_LENGTH);

   Line += i + 1;

   if (not name.empty()) name += '_';
   auto append_from = name.size();

   while (*Line > 0x20) {
      int n;
      for (n=0; (Line[n] > 0x20) and (Line[n] != '='); n++);

      if (Line[n] != '=') {
         log.warning("Malformed const definition, expected '=' after name '%s'", name.c_str());
         break;
      }

      name.erase(append_from);
      name.append(Line, n);
      Line += n + 1;

      for (n=0; (Line[n] > 0x20) and (Line[n] != ','); n++);
      std::string value(Line, n);
      Line += n;

      //log.warning("%s = %s", name.c_str(), value.c_str());

      if (n > 0) {
         auto dt = datatype(value);
         TiriConstant constant(int64_t(0));

         if (dt IS 'i') constant = TiriConstant(int64_t(strtoll(value.c_str(), nullptr, 0)));
         else if (dt IS 'f') constant = TiriConstant(strtod(value.c_str(), nullptr));
         else if (dt IS 'h') constant = TiriConstant(int64_t(strtoull(value.c_str(), nullptr, 0)));
         else log.warning("Unsupported constant value: %s", value.c_str());

         Batch.Constants.push_back({ name, constant });
      }

      if (*Line IS ',') Line++;
   }

   return next_line(Line);
}

//********************************************************************************************************************
// Parse a module's IDL into a staging batch.  No global state is modified.

static ERR stage_module_defs(objModule *Module, std::string_view Name, definition_batch &Batch)
{
   auto root = (OBJECTPTR)Module->Root;
   if (not root) return ERR::FieldNotSet;

   struct ModHeader *header;
   if (auto error = root->get(strhash("header"), header); error != ERR::Okay) return error;
   if (not header) return ERR::NoData;

   Batch.Source = Name;

   if (auto idl = header->Definitions) {
      while ((idl) and (*idl)) {
         if ((idl[0] IS 's') and (idl[1] IS '.')) {
            if (auto error = load_include_struct(idl+2, Name, &idl, Batch); error != ERR::Okay) return error;
         }
         else if ((idl[0] IS 'c') and (idl[1] IS '.')) idl = load_include_constant(idl+2, Name, Batch);
         else idl = next_line(idl);
      }
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Commit a staged batch to the process-wide registries, rolling back completely on failure.
//
// A lock on glConstantMutex must be held before calling this function.  glStructMutex remains locked across structure
// publication and rollback so that readers cannot observe definitions that a later failure withdraws.  Structures are
// registered with a null state so that a state-local declaration cannot influence the published global layout.

static ERR publish_definition_batch(const definition_batch &Batch)
{
   kt::Log log(__FUNCTION__);
   const std::lock_guard struct_lock(glStructMutex);

   std::vector<uint32_t> published_constants;
   std::vector<std::string> published_structs;
   published_constants.reserve(Batch.Constants.size());
   published_structs.reserve(Batch.Structs.size());

   auto rollback = [&]() {
      for (auto key : published_constants) glConstantRegistry.erase(key);
      for (const auto &name : published_structs) remove_struct(name);
   };

   for (const auto &constant : Batch.Constants) {
      const auto key = kt::strhash(constant.Name);

      // A repeated definition of the same constant is normal, because several modules may publish the same IDL.  A
      // differing value for the same key is not: it indicates either a genuine conflict or a hash collision, and
      // either way silently keeping the first value would hide the problem.

      if (auto existing = glConstantRegistry.find(key); existing != glConstantRegistry.end()) {
         if (not (existing->second IS constant.Value)) {
            log.warning("Constant '%s' from module '%s' conflicts with an existing definition.",
               constant.Name.c_str(), Batch.Source.c_str());
            rollback();
            return ERR::Exists;
         }
         continue;
      }

      glConstantRegistry.emplace(key, constant.Value);
      published_constants.push_back(key);
   }

   for (const auto &[name, sequence] : Batch.Structs) {
      auto error = make_struct(nullptr, name, sequence.c_str());

      // ERR::Exists means the structure is already registered, which is expected when modules share definitions.
      if (error IS ERR::Exists) continue;

      if (error != ERR::Okay) {
         log.warning("Failed to register structure '%s' from module '%s': %s",
            name.c_str(), Batch.Source.c_str(), GetErrorMsg(error));
         rollback();
         return error;
      }
      published_structs.push_back(name);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Process a module's definitions once, independently of any Lua state.
//
// The definitions a module publishes are process-wide, so neither the parsing nor the publication may depend on which
// state requested them first.  The result is recorded on the binding itself, which keeps a module and the definitions
// it published from disagreeing.

static ERR ensure_module_defs(ModuleBinding *Module)
{
   std::unique_lock lock(glConstantMutex);

   if (Module->DefState IS def_state::Loaded) return ERR::Okay;
   if (Module->DefState IS def_state::Failed) return Module->DefError;

   definition_batch batch;
   ERR error = stage_module_defs(Module->Module, Module->Name, batch);
   if (error IS ERR::Okay) error = publish_definition_batch(batch);

   Module->DefState = (error IS ERR::Okay) ? def_state::Loaded : def_state::Failed;
   Module->DefError = error;
   return error;
}

//********************************************************************************************************************
// For the 'include' statement.  Resolves the process-wide module and publishes its definitions.

[[nodiscard]] ERR load_module_defs(std::string_view Module)
{
   ModuleBinding *resolved_module = nullptr;
   if (auto error = resolve_module(Module, resolved_module); error != ERR::Okay) return error;

   AdjustLogLevel(1);
   ERR error = ensure_module_defs(resolved_module);
   AdjustLogLevel(-1);
   return error;
}

//********************************************************************************************************************
// Format: s.Name:typeField,...
// TODO: This parses the struct definitions in advance - ideally we'd record the definition string and parse on first-use.

[[nodiscard]] static ERR load_include_struct(CSTRING Line, std::string_view Source, CSTRING *NextLine,
   definition_batch &Batch)
{
   if (not NextLine) return ERR::NullArgs;
   *NextLine = next_line(Line);

   int i;
   for (i=0; (Line[i] >= 0x20) and (Line[i] != ':'); i++);

   if (Line[i] IS ':') {
      std::string name(Line, i);
      Line += i + 1;

      int j;
      for (j=0; (Line[j] != '\n') and (Line[j] != '\r') and (Line[j]); j++);

      std::string sequence(Line, j);
      if ((Line[j] IS '\n') or (Line[j] IS '\r')) {
         while ((Line[j] IS '\n') or (Line[j] IS '\r')) j++;
      }
      *NextLine = Line + j;

      Batch.Structs.emplace_back(std::move(name), std::move(sequence));
      return ERR::Okay;
   }
   else {
      kt::Log(__FUNCTION__).warning("Malformed struct name in %.*s.", int(Source.size()), Source.data());
      return ERR::Syntax;
   }
}

//********************************************************************************************************************
// Usage: passed, total = mod.test(ModuleName, Options)
// Resolves the named module and runs its unit tests, if any.

static int module_test(lua_State *Lua)
{
   CSTRING module_name = luaL_checkstring(Lua, 1);

   ModuleBinding *mod = nullptr;
   if (auto error = resolve_module(module_name, mod); error != ERR::Okay) {
      luaL_error(Lua, error, "Failed to load the %s module.", module_name);
   }

   auto options = lua_tostringview(Lua, 2);
   int passed = 0, total = 0;
   if (mod->Module->test(options, &passed, &total) IS ERR::Okay) {
      lua_pushinteger(Lua, passed);
      lua_pushinteger(Lua, total);
      return 2;
   }
   luaL_error(Lua, ERR::Failed, "Module test failed.");
   return 0;
}

//********************************************************************************************************************
// Locate one exported function by canonical name.  Case-insensitive, matching the compiler's source-level resolution
// because it is literally the same index: the signature resolves the name to an ordinal, and Callables is parallel to
// the signature's function list.  A module whose signature failed to build exports nothing resolvable.

static ModuleCallable * find_module_callable(ModuleBinding *Module, std::string_view Name)
{
   if (not Module->Signature) return nullptr;
   auto ordinal = Module->Signature->find_ordinal(Name);
   if (ordinal IS NO_FUNCTION_ORDINAL) return nullptr;
   if (ordinal >= Module->Callables.size()) return nullptr;
   return Module->Callables[ordinal].get();
}

//********************************************************************************************************************
// Ensure that a prototype has state-owned storage for resolved callables and per-descriptor activation state.

static void ensure_proto_dependency_sidecar(lua_State *Lua, GCproto *Proto, const ProtoDependencyTable *Table)
{
   if (not Proto->resolved_dependencies) {
      Proto->resolved_count = Table->function_count ? Table->function_count : 1;
      Proto->resolved_dependencies = (void **)lj_mem_new(Lua, Proto->resolved_count * sizeof(void *));
      memset(Proto->resolved_dependencies, 0, Proto->resolved_count * sizeof(void *));
   }
   if (not Proto->resolved_dependency_states) {
      Proto->resolved_dependency_count = Table->dependency_count;
      Proto->resolved_dependency_states = (uint8_t *)lj_mem_new(Lua, Table->dependency_count * sizeof(uint8_t));
      memset(Proto->resolved_dependency_states, 0, Table->dependency_count * sizeof(uint8_t));
   }
}

//********************************************************************************************************************
// Resolve one portable dependency descriptor into the prototype's state-owned sidecar.
//
// The descriptors persist canonical names only, so every load resolves them afresh against the registry's current
// contents.  That is what makes a serialised chunk portable: the module's export list may have been reordered or
// extended since the chunk was written, and a stored index would then select the wrong function.
//
// The sidecar holds non-owning pointers.  Registry records are stable until expunge_modules(), which runs only once
// no Tiri state can execute, so a resolved pointer cannot outlive its record.
//
// Resolution is idempotent and descriptor-local.  A later declaration is not resolved by an earlier BC_MODACT, so a
// missing module or function is reported at that declaration's execution position.

static ERR resolve_proto_dependency(lua_State *Lua, GCproto *Proto, uint32_t Dependency, std::string &ErrorMsg)
{
   auto table = proto_dependencies(Proto);
   if (not table) return ERR::Okay;
   if (Dependency >= table->dependency_count) return ERR::InvalidData;

   ensure_proto_dependency_sidecar(Lua, Proto, table);
   if (Proto->resolved_dependency_states[Dependency]) return ERR::Okay;

   auto dependencies = proto_dependency_list(table);
   auto functions = proto_dependency_functions(table);
   const ProtoDependency &dependency = dependencies[Dependency];

   GCstr *module_name = gco_to_string(gcref(dependency.name));
   std::string_view name(strdata(module_name), module_name->len);

   ModuleBinding *binding = nullptr;
   if (auto error = resolve_module(name, binding); error != ERR::Okay) {
      ErrorMsg = std::format("Failed to load the {} module.", name);
      return error;
   }
   if (auto error = ensure_module_defs(binding); error != ERR::Okay) {
      ErrorMsg = std::format("Failed to process definitions for the {} module.", name);
      return error;
   }

   // Resolve into temporary storage first, so failure cannot mark this descriptor as activated or expose a partial
   // callable range.

   std::vector<void *> resolved(dependency.function_count, nullptr);
   for (uint32_t f = 0; f < dependency.function_count; ++f) {
      uint32_t slot = dependency.first_function + f;
      GCstr *function_name = gco_to_string(gcref(functions[slot].name));
      std::string_view function(strdata(function_name), function_name->len);

      // The name is revalidated here even though the reader accepted the descriptor.  Structural validity says
      // nothing about whether the module still exports the function under that name.

      auto callable = find_module_callable(binding, function);
      if (not callable) {
         ErrorMsg = std::format("Function {}() is not exported by the {} module.", function, name);
         return ERR::Search;
      }
      resolved[f] = callable;
   }

   if (not resolved.empty()) {
      memcpy(Proto->resolved_dependencies + dependency.first_function, resolved.data(),
         resolved.size() * sizeof(void *));
   }
   Proto->resolved_dependency_states[Dependency] = 1;
   return ERR::Okay;
}

//********************************************************************************************************************
// Return the callable resolved for one descriptor slot, or null if the prototype has not been activated or the slot
// is out of range.  Callers must treat the result as non-owning.

APTR proto_dependency_callable(GCproto *Proto, uint32_t Slot)
{
   if (not Proto->resolved_dependencies or Slot >= Proto->resolved_count) return nullptr;
   return Proto->resolved_dependencies[Slot];
}

//********************************************************************************************************************
// Activate one compiler-declared module dependency from BC_MODACT.
//
// The VM positions Lua->top at the opcode's destination register before entering this helper.  Descriptor names are
// resolved once into the prototype-owned sidecar, then the state-owned closures required by ordinary local-call
// bytecode are materialised directly into consecutive stack slots.  A dependency-only declaration resolves its
// module but creates no artificial value.

extern "C" void tiri_module_activate(lua_State *Lua, uint32_t Dependency)
{
   if (not curr_funcisL(Lua)) {
      luaL_error(Lua, ERR::InvalidState, "Module activation requires a Lua prototype.");
   }

   GCproto *proto = curr_proto(Lua);
   auto table = proto_dependencies(proto);
   if (not table or Dependency >= table->dependency_count) {
      luaL_error(Lua, ERR::InvalidData, "Invalid module dependency descriptor %u.", Dependency);
   }

   auto dependencies = proto_dependency_list(table);
   const ProtoDependency &dependency = dependencies[Dependency];
   ptrdiff_t destination = Lua->top - Lua->base;
   if (destination < 0 or destination + dependency.function_count > proto->framesize) {
      luaL_error(Lua, ERR::InvalidData, "Module dependency %u exceeds the prototype register frame.", Dependency);
   }

   std::string error_msg;
   if (auto error = resolve_proto_dependency(Lua, proto, Dependency, error_msg); error != ERR::Okay) {
      luaL_error(Lua, error, std::move(error_msg));
   }

   for (uint32_t slot = 0; slot < dependency.function_count; ++slot) {
      auto callable = proto_dependency_callable(proto, dependency.first_function + slot);
      if (not callable) {
         luaL_error(Lua, ERR::InvalidData, "Module dependency %u has an unresolved callable slot.", Dependency);
      }
      lua_pushlightuserdata(Lua, callable);
      lua_pushcclosure(Lua, module_call, 1);
   }
}

//********************************************************************************************************************
// Makes a call to a Kotuku module function.  module_call() sets the benchmark for processing FD parameters.
//
// A breakdown of possible FD configurations follows.  Input values:
//
// STR          = CSTRING.  Read-only string pointer.  Tiri string values are accepted; NULL is permitted.
// STR|CPP      = const std::string_view &.  Read-only string view.  Tiri nil becomes a zero-length view with
//                null data.
// PTR          = APTR.  Raw pointer input.  Tiri strings, arrays, structs, objects and userdata can provide
//                backing storage depending on the call.
// PTR|STRUCT   = Struct *.  Requires an existing Tiri struct.
// OBJECT|PTR   = OBJECTPTR.  Accepts a Tiri object and resolves it to an object pointer.
// INT/DOUBLE/INT64 = Numeric values.  Tiri nil or a missing argument becomes zero.
// ARRAY        = Unsupported.  Raw pointer-array marshalling has been removed.
// ARRAY|CPP    = Unsupported as input.  kt::vector<> input marshalling is not implemented for module calls.
// SPAN         = const std::span<T> &.  Accepts matching Tiri array or structure storage, byte strings or an empty value.
// SPAN|MUTABLE = const std::span<T> &.  Requires writable array, structure or mutable byte-string storage.
// FUNCTION     = FUNCTION *.  Accepts a Tiri function or global function name.
//
// Mutable combinations.  User is expected to supply a reference to a suitable storage area, as defined by the type
// The storage area may contain existing data for consumption prior to mutation.  Failure to provide storage would be
// considered an error.  Often used for reading data into buffers, like the Read() action.
//
// PTR|MUTABLE     = APTR.  Mutable raw pointer/buffer input.  Tiri strings must be mutable.
// STR|MUTABLE     = STRING.  A mutable Tiri string buffer is required, normally from string.alloc().
//                   The function mutates the buffer in-place.  A BUFSIZE is expected for the next parameter.
// STR|MUTABLE|CPP = std::string *.  A mutable Tiri string buffer is required.  Tiri content is copied into
//                   a temporary std::string before the call, then copied back after the call.  If the final
//                   string is larger than the Tiri buffer, the call fails.
//
// Result combintations.  The function will return a reference to its own storage area, or allocate a new one.  Result
// values will be returned as true Tiri results, unlike mutable types which are manipulated in-place.
//
// RESULT|STR        = CSTRING *.  Function writes a non-allocated C string pointer; returned to Tiri as a string.
// RESULT|STR|ALLOC  = CSTRING *.  Function writes an allocated C string pointer; returned to Tiri then freed.
// RESULT|STR|CPP    = std::string *.  Tiri provides a temporary std::string; returned to Tiri as a string.
//
// RESULT|PTR        = APTR *.  Function writes a pointer; returned to Tiri as light userdata unless specialised.
// RESULT|PTR|ALLOC  = APTR *.  Function writes an allocated block; returned as a byte array when BUFSIZE is
//                     available, then freed.
// RESULT|PTR|STRUCT = Struct **.  Function writes a struct pointer; returned as a Tiri table, or as a managed
//                     struct when RESOURCE is set.
// RESULT|ARRAY      = Unsupported.  Raw pointer-array marshalling has been removed.
// RESULT|ARRAY|CPP  = kt::vector<> *.  Tiri provides an empty kt::vector<>; returned to Tiri as an array.
// Mixed direction:
//
// MUTABLE|RESULT|CPP = An empty C++ buffer is supplied as input (e.g. std::string or kt::vector<>) and it will be
//                      used to store the result.

static int module_call(lua_State *Lua)
{
   std::string error_msg;
   int results = 0;

   auto err = module_call_inner(Lua, error_msg, results);
   if (err != ERR::Okay) {
      if (error_msg.empty()) error_msg = GetErrorMsg(err);
      luaL_error(Lua, err, std::move(error_msg));
   }
   return results;
}

static ERR module_call_inner(lua_State *Lua, std::string &ErrorMsg, int &Results)
{
   kt::Log log("module_call");

   uint8_t buffer[BUFFER_SIZE]; // 16 bytes per argument accommodates output metadata such as sizes.
   int i;

   struct mutable_cpp_string_ref {
      std::string *Source;
      GCstr *Target;
      int Arg;

      mutable_cpp_string_ref(std::string *InitSource, GCstr *InitTarget, int InitArg):
         Source(InitSource), Target(InitTarget), Arg(InitArg) { }
   };

   // Bridge-owned temporaries.  Each store has a fixed capacity that preparation has already proved sufficient for
   // this signature, so an element's address stays valid once written into arg_values, and a signature that needs no
   // temporaries constructs nothing.

   bounded_store<std::string, MAX_MODULE_ARGS> strings;
   bounded_store<std::string_view, MAX_MODULE_ARGS> string_views;
   bounded_store<mutable_cpp_string_ref, MAX_MODULE_ARGS> mutable_cpp_strings;
   bounded_store<cpp_array_result, MAX_MODULE_ARGS> cpp_arrays;
   std::array<module_span_arg, MAX_MODULE_ARGS> span_args;
   size_t span_count = 0;

   auto copy_mutable_cpp_strings = [&]() {
      for (size_t entry_index = 0; entry_index < mutable_cpp_strings.size(); ++entry_index) {
         auto &entry = mutable_cpp_strings[entry_index];
         auto capacity = size_t(entry.Target->len);
         auto length = entry.Source->size();
         if (length > capacity) ErrorMsg = "Mutable buffer too small.";
         else {
            auto target = strdatawr(entry.Target);
            if (length) copymem(entry.Source->data(), target, length);
            if (length < capacity) clearmem(target + length, capacity - length);
         }
      }
   };

   auto tiri = Lua->script;
   if (not tiri) return ERR::ObjectCorrupt;

   auto value_string = [](lua_State *Lua, int Index) -> CSTRING {
      if (auto string = lua_tostring(Lua, Index)) return string;
      else return "";
   };

   // One light-userdata upvalue holds the process-wide callable record, which carries the native address, the argument
   // metadata and the libffi call interface that was prepared when the module was resolved.

   auto callable = (ModuleCallable *)lua_touserdata(Lua, lua_upvalueindex(1));
   if (not callable) {
      ErrorMsg = "module_call() expected a callable record in its upvalue.";
      return ERR::Args;
   }

   // Signature problems are detected during preparation and reported here, so that an unsupported function fails
   // consistently whether it is called directly or through an extracted callable.
   if (not callable->valid()) {
      ErrorMsg = "Function not compatible with Tiri";
      return ERR::NoSupport;
   }

   // The script and native argument limits are one contract: the marshaller reads script index i for signature
   // argument i, so a value beyond MAX_MODULE_ARGS could never be marshalled.  Previously this clamped a local that
   // nothing else consumed, which silently discarded the diagnostic; the surplus is now rejected outright.

   int nargs = lua_gettop(Lua);
   if (nargs > MAX_MODULE_ARGS) {
      ErrorMsg = std::format("Function '{}' received {} arguments, exceeding the limit of {}.",
         callable->Name, nargs, MAX_MODULE_ARGS);
      return ERR::Args;
   }

   uint8_t *end = buffer + sizeof(buffer);

   log.trace("%s() Args: %d", callable->Name, nargs);

   if (callable->trivial()) {
      auto function = (void (*)(void))callable->Address;
      function();
      return ERR::Okay;
   }

   const FunctionField *args = callable->Fields;
   APTR function = callable->Address;
   FUNCTION func;

   // This guard owns the Lua registry reference for an FD_FUNCTION argument until the call is handed to the module.
   // After that point, the module function is responsible for calling DerefProcedure() when it no longer needs it,
   // or alternatively marking the function as consumed.

   struct func_ref_guard {
      lua_State *Lua;
      FUNCTION  &Func;
      bool OwnsReference = true;
      ~func_ref_guard() {
         if (OwnsReference or Func.consumed()) release_tiri_function(Lua, &Func);
      }
   } func_guard{ Lua, func };

   union {
      ffi_arg Arg;
      double  Double;
      int64_t Int64;
   } rc = { };
   void * arg_values[MAX_MODULE_ARGS];
   int in = 0;

   int j = 0;

   for (i=1; args[i].Name; i++) {
      int argtype = args[i].Type;

      if ((argtype & FDF_SPAN) IS FDF_SPAN) {
         if (span_count >= span_args.size()) {
            ErrorMsg = "Too many span arguments.";
            return ERR::Args;
         }

         if (argtype & (FD_RESULT|FD_ALLOC)) {
            ErrorMsg = std::format("Function '{}' uses an unsupported span result.", callable->Name);
            return ERR::NoSupport;
         }

         bool mutable_span = argtype & FD_MUTABLE;

         AET element_type;
         size_t element_size;
         struct_record *struct_def;
         if (auto error = module_span_element_metadata(Lua, args[i], &element_type, &element_size, &struct_def);
             error != ERR::Okay) {
            ErrorMsg = (error IS ERR::Search) ?
               std::format("Function '{}' references an unknown span structure.", callable->Name) :
               std::format("Function '{}' uses invalid span element metadata.", callable->Name);
            return error;
         }

         CPTR data = nullptr;
         size_t extent = 0;
         auto &span_arg = span_args[span_count++];
         auto value_type = lua_type(Lua, i);

         if (value_type IS LUA_TARRAY) {
            auto array = arrayV(Lua, i);
            if (mutable_span and array->is_readonly()) {
               ErrorMsg = std::format("Arg #{} ({}) requires a writable array.", i, args[i].Name);
               return ERR::InvalidType;
            }

            if ((array->elemtype != element_type) or (size_t(array->elemsize) != element_size)) {
               ErrorMsg = std::format("Arg #{} ({}) array element type does not match the span.", i, args[i].Name);
               return ERR::InvalidType;
            }
            if ((element_type IS AET::STRUCT) and (array->structdef != struct_def)) {
               ErrorMsg = std::format("Arg #{} ({}) structure array type does not match the span.", i, args[i].Name);
               return ERR::InvalidType;
            }
            data = array->arraydata();
            extent = array->len;
         }
         else if (value_type IS LUA_TSTRING) {
            if (element_type != AET::BYTE) {
               ErrorMsg = std::format("Arg #{} ({}) only accepts string storage for a byte span.", i, args[i].Name);
               return ERR::InvalidType;
            }

            auto string = string_arg(Lua, i);
            if (mutable_span and not lj_str_ismutable(string)) {
               ErrorMsg = std::format("Arg #{} ({}) requires a mutable string buffer.", i, args[i].Name);
               return ERR::InvalidType;
            }
            data = mutable_span ? CPTR(strdatawr(string)) : CPTR(strdata(string));
            extent = string->len;
         }
         else if ((value_type IS LUA_TNIL) or (value_type IS LUA_TNONE)) {
            // The canonical empty span is constructed below.
         }
         else if (auto native_struct = lua_isstruct(Lua, i) ? lua_tostruct(Lua, i) : nullptr) {
            if (element_type != AET::STRUCT) {
               ErrorMsg = std::format("Arg #{} ({}) requires matching array storage.", i, args[i].Name);
               return ERR::InvalidType;
            }
            if (lj_struct_stale(native_struct)) {
               ErrorMsg = std::format("Arg #{} ({}) structure storage is stale.", i, args[i].Name);
               return ERR::DoesNotExist;
            }
            if ((native_struct->def != struct_def) or (size_t(native_struct->structsize) != element_size)) {
               ErrorMsg = std::format("Arg #{} ({}) structure type does not match the span.", i, args[i].Name);
               return ERR::InvalidType;
            }
            if (not native_struct->data) {
               ErrorMsg = std::format("Arg #{} ({}) structure storage is unavailable.", i, args[i].Name);
               return ERR::InvalidData;
            }
            data = native_struct->data;
            extent = 1;
         }
         else {
            ErrorMsg = std::format("Arg #{} ({}) expected an array, string, structure or nil for a span, got {}.", i,
               args[i].Name, lua_typename(Lua, value_type));
            return ERR::InvalidType;
         }

         if (extent and not data) {
            ErrorMsg = std::format("Arg #{} ({}) span storage is unavailable.", i, args[i].Name);
            return ERR::InvalidData;
         }

         auto span_address = span_arg.set(data, extent);
         ((APTR *)(buffer + j))[0] = span_address;
         arg_values[in] = buffer + j;
         in++;
         j += sizeof(APTR);
      }
      else if (argtype & FD_RESULT) {
         // Result arguments are stored in the buffer with a pointer to an empty variable space (stored at the end of
         // the buffer)

         RMSG("Result for arg %d stored at %p", i, end);

         if (((argtype & FDF_VECTOR) IS FDF_VECTOR) and (argtype & FD_MUTABLE)) {
            // Applicable to RESULT|MUTABLE only, which requires the client to provide an empty kt::vector<> to the function.
            // We set this up here, then convert the resulting values to a Tiri array when the function returns.
            cpp_array_result result_ref = { };
            if ((argtype & FD_STRUCT) and (!(argtype & FD_PTR))) {
               // TODO: args[i].Name will include the name of the struct that we need to use to build the array, e.g. "FontList:Result"
               // where FontList is the struct name.  The kt::vector<T> data() will contain a serialised list of structs and the total
               // count.  For this to work, the STRUCT type would need to host a kt::vector<> and the call site would need to
               // make a TrackResource() call that associates its destruction process with the kt::vector<> address.
               ErrorMsg = "C++ struct arrays are not supported.";
               return ERR::NoSupport;
            }
            else if (auto error = make_cpp_array_result(argtype, &result_ref); !error) {
               ((APTR *)(buffer + j))[0] = result_ref.Data; // kt::vector<>
               if (not cpp_arrays.emplace(std::move(result_ref))) {
                  ErrorMsg = std::format("Function '{}' exceeded its C++ array result storage.", callable->Name);
                  return ERR::BufferOverflow;
               }
               arg_values[in]  = buffer + j;
               in++;
               j += sizeof(APTR);
            }
            else {
               ErrorMsg = std::format("Function '{}' uses an unsupported C++ array result type.", callable->Name);
               return error;
            }
         }
         else if (argtype & FD_STR) { // FD_RESULT
            if (argtype & FD_CPP) {
               if (argtype & FD_MUTABLE) {
                  // Use of MUTABLE enables support for std::string buffering.  We provide our own std::string that will be used as
                  // the buffer, it gets converted to a Tiri string when the function returns.

                  auto slot = strings.emplace();
                  if (not slot) {
                     ErrorMsg = std::format("Function '{}' exceeded its string storage.", callable->Name);
                     return ERR::BufferOverflow;
                  }
                  ((std::string **)(buffer + j))[0] = slot;
                  arg_values[in]  = buffer + j;
                  in++;
                  j += sizeof(APTR);
               }
               else {
                  // Non-mutable string reference, supported as a std::string_view
                  auto slot = string_views.emplace();
                  if (not slot) {
                     ErrorMsg = std::format("Function '{}' exceeded its string view storage.", callable->Name);
                     return ERR::BufferOverflow;
                  }
                  ((std::string_view **)(buffer + j))[0] = slot;
                  arg_values[in]  = buffer + j;
                  in++;
                  j += sizeof(APTR);
               }
            }
            else { // C-style string reference, this parameter style is being deprecated.
               end -= sizeof(APTR);
               ((APTR *)(buffer + j))[0] = end;
               ((APTR *)end)[0] = nullptr;
               arg_values[in]  = buffer + j;
               in++;
               j += sizeof(APTR);
            }
         }
         else if (argtype & FD_PTR) { // FD_RESULT
            end -= sizeof(APTR);
            ((APTR *)(buffer + j))[0] = end;
            ((APTR *)end)[0] = nullptr;
            arg_values[in]  = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if (argtype & FD_INT) { // FD_RESULT
            end -= sizeof(int);
            ((APTR *)(buffer + j))[0] = end;
            ((int *)end)[0] = 0;
            arg_values[in]  = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if (argtype & (FD_DOUBLE|FD_INT64)) { // FD_RESULT
            end -= sizeof(int64_t);
            ((APTR *)(buffer + j))[0] = end;
            ((int64_t *)end)[0] = 0;
            arg_values[in]  = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else {
            ErrorMsg = std::format("Unrecognised arg {} type {}", i, argtype);
            return ERR::InvalidType;
         }
      }
      else if (argtype & FD_FUNCTION) {
         if (func.defined()) { // Is the function reserve already used?
            ErrorMsg = "Multiple function arguments are not supported.";
            return ERR::Args;
         }

         // NOTE: The client is responsible for calling DerefProcedure() on the function reference when it is no longer needed,
         // or it can mark the function as consumed.

         switch(lua_type(Lua, i)) {
            case LUA_TSTRING:
            case LUA_TFUNCTION: {
               if (auto error = capture_tiri_function(Lua, i, func); error != ERR::Okay) {
                  ErrorMsg = std::format("Function argument #{} ({}) must resolve to a function.", i, args[i].Name);
                  return error;
               }
               ((FUNCTION **)(buffer + j))[0] = &func;
               break;
            }

            case LUA_TNIL:
            case LUA_TNONE:
               ((FUNCTION **)(buffer + j))[0] = nullptr;
               break;

            default:
               ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected function, got {} '{}'.", i, args[i].Name,
                  lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
               return ERR::InvalidType;
         }

         arg_values[in]  = buffer + j;
         in++;
         j += sizeof(FUNCTION *);
      }
      else if (argtype & FD_STR) {
         auto type = lua_type(Lua, i);
         int type_size = sizeof(APTR);

         if (argtype & FD_MUTABLE) {
            // The client is expected to provide a mutable string with preallocated size, which can be acquired from string.alloc()
            if (type != LUA_TSTRING) {
               ErrorMsg = std::format("Arg #{} requires a mutable buffer.", i);
               return ERR::InvalidType;
            }

            GCstr *string = string_arg(Lua, i);
            if (not lj_str_ismutable(string)) {
               ErrorMsg = std::format("Arg #{} requires a mutable buffer.", i);
               return ERR::InvalidType;
            }

            if (argtype & FD_CPP) { // Function expects a pointer to std::string that it can mutate
               size_t len;
               auto str = lua_tolstring(Lua, i, &len);
               auto cppstr = str ? strings.emplace(str, len) : strings.emplace();
               if (not cppstr) {
                  ErrorMsg = std::format("Function '{}' exceeded its string storage.", callable->Name);
                  return ERR::BufferOverflow;
               }
               if (not mutable_cpp_strings.emplace(cppstr, string, i)) {
                  ErrorMsg = std::format("Function '{}' exceeded its mutable string storage.", callable->Name);
                  return ERR::BufferOverflow;
               }
               ((std::string **)(buffer + j))[0] = cppstr;
            }
            else ((CSTRING *)(buffer + j))[0] = strdatawr(string);
         }
         else if (argtype & FD_CPP) { // Read-only std::string_view; nil has null data and zero length.
            if ((type != LUA_TSTRING) and (type != LUA_TNIL) and (type != LUA_TNONE)) {
               ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected string, got {} '{}'.", i, args[i].Name,
                  lua_typename(Lua, type), value_string(Lua, i));
               return ERR::InvalidType;
            }
            size_t len;
            auto str = lua_tolstring(Lua, i, &len);
            auto view = str ? string_views.emplace(str, len) : string_views.emplace();
            if (not view) {
               ErrorMsg = std::format("Function '{}' exceeded its string view storage.", callable->Name);
               return ERR::BufferOverflow;
            }
            ((std::string_view **)(buffer + j))[0] = view;
         }
         else if (type IS LUA_TSTRING) {
            ((CSTRING *)(buffer + j))[0] = lua_tostring(Lua, i);
         }
         else if (type <= 0) {
            ((CSTRING *)(buffer + j))[0] = nullptr;
         }
         else if ((type IS LUA_TUSERDATA) or (type IS LUA_TLIGHTUSERDATA)) {
            ErrorMsg = std::format("Arg #{} ({}) requires a string and not untyped pointer.", i, args[i].Name);
            return ERR::InvalidType;
         }
         else {
            ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected string, got {} '{}'.", i, args[i].Name,
               lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
            return ERR::InvalidType;
         }

         arg_values[in] = buffer + j;
         in++;
         j += type_size;
      }
      else if ((argtype & FDF_VECTOR) IS FDF_VECTOR) {
         ErrorMsg = "C++ array inputs are not supported.";
         return ERR::NoSupport;
      }
      else if (argtype & FD_PTR) {
         auto arg_type = lua_type(Lua, i);
         if (arg_type IS LUA_TSTRING) {
            // Lua strings need to be converted to C strings
            auto string = string_arg(Lua, i);
            if ((argtype & FD_MUTABLE) and (not lj_str_ismutable(string))) {
               ErrorMsg = std::format("Arg #{} requires a mutable buffer.", i);
               return ERR::InvalidType;
            }

            ((CSTRING *)(buffer + j))[0] = (argtype & FD_MUTABLE) ? strdatawr(string) : strdata(string);
            arg_values[in] = buffer + j;
            in++;
            j += sizeof(CSTRING);
         }
         else if (auto native_struct = lua_isstruct(Lua, i) ? lua_tostruct(Lua, i) : nullptr) {
            // Guard specific to lifecycle-bound struct views; structs without an object dependency skip it.
            if (lj_struct_stale(native_struct)) {
               ErrorMsg = "A struct argument's providing object has been destroyed.";
               return ERR::DoesNotExist;
            }
            ((APTR *)(buffer + j))[0] = native_struct->data;
            arg_values[in] = buffer + j;
            in++;
            j += sizeof(APTR);

            log.trace("Struct address %p inserted to arg offset %d", native_struct->data, j);
         }
         else if (arg_type IS LUA_TOBJECT) {
            auto obj = lua_toobject(Lua, i);
            if (auto direct = direct_object_ptr(obj)) {
               ((OBJECTPTR *)(buffer + j))[0] = direct;
            }
            else {
               OBJECTPTR ptr_obj;
               if (auto error = access_object(obj, ptr_obj); error IS ERR::Okay) {
                  ((OBJECTPTR *)(buffer + j))[0] = ptr_obj;
                  release_object(obj);
               }
               else {
                  // Referenced object probably does not exist
                  ErrorMsg = std::format("Unable to resolve object #{}: {}", obj->uid, GetErrorMsg(error));
                  return error;
               }
            }

            arg_values[in] = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if (arg_type IS LUA_TARRAY) {
            GCarray *array = lua_toarray(Lua, i);

            ((APTR *)(buffer + j))[0] = array->arraydata();
            arg_values[in] = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else if ((arg_type IS LUA_TUSERDATA) or (arg_type IS LUA_TLIGHTUSERDATA) or (arg_type IS LUA_TNIL) or
                  (arg_type IS LUA_TNONE)) {
            ((APTR *)(buffer + j))[0] = lua_touserdata(Lua, i); //lua_topointer?
            arg_values[in] = buffer + j;
            in++;
            j += sizeof(APTR);
         }
         else {
            ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected pointer-compatible storage, got {} '{}'.", i,
               args[i].Name, lua_typename(Lua, arg_type), value_string(Lua, i));
            return ERR::InvalidType;
         }
      }
      else if (argtype & FD_INT) {
         auto tv = resolve_index(Lua, i);

         if (argtype & FD_OBJECT) {
            if (tvisobject(tv)) ((int *)(buffer + j))[0] = objectV(tv)->uid;
            else if (lua_type(Lua, i) <= LUA_TNIL) ((int *)(buffer + j))[0] = 0;
            else if (lua_type(Lua, i) IS LUA_TNUMBER) ((int *)(buffer + j))[0] = lua_tointeger(Lua, i);
            else {
               ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected object or number, got {} '{}'.", i,
                  args[i].Name, lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
               return ERR::InvalidType;
            }
         }
         else {
            if (lua_type(Lua, i) <= LUA_TNIL) {
               ((int *)(buffer + j))[0] = 0;
            }
            else if (lua_type(Lua, i) != LUA_TNUMBER) {
               ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected number, got {} '{}'.", i, args[i].Name,
                  lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
               return ERR::InvalidType;
            }
            else if (argtype & FD_UNSIGNED) ((uint32_t *)(buffer + j))[0] = lua_tointeger(Lua, i);
            else ((int *)(buffer + j))[0] = lua_tointeger(Lua, i);
         }
         arg_values[in] = buffer + j;
         in++;
         j += sizeof(int);
      }
      else if (argtype & FD_DOUBLE) {
         if (lua_type(Lua, i) <= LUA_TNIL) ((double *)(buffer + j))[0] = 0.0;
         else if (lua_type(Lua, i) != LUA_TNUMBER) {
            ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected number, got {} '{}'.", i, args[i].Name,
               lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
            return ERR::InvalidType;
         }
         else ((double *)(buffer + j))[0] = lua_tonumber(Lua, i);
         arg_values[in] = buffer + j;
         in++;
         j += sizeof(double);
      }
      else if (argtype & FD_INT64) {
         if (lua_type(Lua, i) <= LUA_TNIL) ((int64_t *)(buffer + j))[0] = 0;
         else if (lua_type(Lua, i) != LUA_TNUMBER) {
            ErrorMsg = std::format("Type mismatch, arg #{} ({}) expected number, got {} '{}'.", i, args[i].Name,
               lua_typename(Lua, lua_type(Lua, i)), value_string(Lua, i));
            return ERR::InvalidType;
         }
         else ((int64_t *)(buffer + j))[0] = lua_tointeger(Lua, i);
         arg_values[in] = buffer + j;
         in++;
         j += sizeof(int64_t);
      }
      else if (argtype & (FD_TAGS|FD_VARTAGS)) {
         ErrorMsg = "Functions using tags are not supported.";
         return ERR::NoSupport;
      }
      else {
         ErrorMsg = std::format("{}() unsupported arg '{}', flags ${:08x}.", callable->Name, args[i].Name, argtype);
         return ERR::NoSupport;
      }
   }

   // Call the function.  The call interface and return type were prepared when the module was resolved, so the hot
   // path only has to verify that the marshaller produced the argument count the interface was built for.

   int restype = args->Type;
   int result = (callable->Cif.rtype IS &ffi_type_void) ? 0 : 1;

   // The buffer is allocated from both ends: argument slots grow forwards from 'buffer' and result storage grows
   // backwards from 'end'.  Preparation proves the two cannot meet for an accepted signature, so this check confirms
   // the marshaller honoured the sizes preparation measured rather than guarding an expected condition.

   if (buffer + j > end) {
      ErrorMsg = std::format("Function '{}' overran its call buffer.", callable->Name);
      return ERR::BufferOverflow;
   }

   if (in != int(callable->Cif.nargs)) {
      ErrorMsg = std::format("Function '{}' marshalled {} args but its call interface expects {}.",
         callable->Name, in, callable->Cif.nargs);
      return ERR::Mismatch;
   }

   {
      func_guard.OwnsReference = false;
      ffi_call(&callable->Cif, (void (*)())function, &rc, arg_values);
      copy_mutable_cpp_strings();
      if (not ErrorMsg.empty()) return ERR::BufferOverflow;

      // Process the result based on the return type
      if (restype & FD_STR) {
         lua_pushstring(Lua, (CSTRING)rc.Arg);
         if ((restype & FD_ALLOC) and ((CSTRING)rc.Arg)) FreeResource((APTR)rc.Arg);
      }
      else if (restype & FD_OBJECT) {
         if ((OBJECTPTR)rc.Arg) {
            push_object(Lua, (OBJECTPTR)rc.Arg,  (restype & FD_ALLOC) ? false : true);
         }
         else lua_pushnil(Lua);
      }
      else if (restype & FD_PTR) {
         if (restype & FD_STRUCT) {
            if (auto structptr = (APTR)rc.Arg) {
               ERR error;
               // A structure marked as a resource will be returned as an accessible struct pointer.  This is typically
               // needed when a struct's use is beyond informational and can be passed to other functions.
               //
               // Otherwise, the default behaviour is to convert the struct's content to a regular Lua table.
               if (restype & FD_RESOURCE) push_struct(Lua->script, structptr, args->Name, (restype & FD_ALLOC) ? TRUE : FALSE, TRUE);
               else if ((error = named_struct_to_table(Lua, args->Name, structptr)) != ERR::Okay) {
                  if (error IS ERR::Search) {
                     // Unknown structs are returned as pointers - this is mainly to indicate that there is a value
                     // and not a nil.
                     lua_pushlightuserdata(Lua, (APTR)structptr);
                  }
                  else {
                     ErrorMsg = std::format("Failed to resolve struct {}, error: {}", args->Name, GetErrorMsg(error));
                     return ERR::Search;
                  }
               }
            }
            else lua_pushnil(Lua);
         }
         else {
            if ((APTR)rc.Arg) lua_pushlightuserdata(Lua, (APTR)rc.Arg);
            else lua_pushnil(Lua);
         }
      }
      else if (restype & (FD_INT|FD_ERROR)) {
         if (restype & FD_UNSIGNED) {
            lua_pushnumber(Lua, (uint32_t)rc.Arg);
         }
         else {
            lua_pushinteger(Lua, (int)rc.Arg);
            if ((restype & FD_ERROR) and (rc.Arg >= int(ERR::ExceptionThreshold)) and in_try_immediate_scope(Lua)) {
               // Scope isolation: Only throw exceptions for direct calls within the try block.
               auto error = ERR(rc.Arg);
               ErrorMsg = std::format("{}() failed: {}", callable->Name, GetErrorMsg(error));
               return error;
            }
         }
      }
      else if (restype & FD_DOUBLE) {
         lua_pushnumber(Lua, rc.Double);
      }
      else if (restype & FD_INT64) {
         lua_pushnumber(Lua, rc.Int64);
      }
      // Void functions don't push anything to the stack
   }

   Results = process_results(tiri, buffer, args) + result;
   return ERR::Okay;
}

//********************************************************************************************************************
// Convert FD_RESULT parameters to the equivalent Tiri result value.
// Also takes care of any cleanup code for dynamically allocated values.

static int process_results(extTiri *Tiri, APTR resultsidx, const FunctionField *args)
{
   kt::Log log(__FUNCTION__);

   auto lua = Tiri->Lua;
   auto scan = (uint8_t *)resultsidx;
   int results = 0;
   for (int i=1; args[i].Name; i++) {
      const auto argtype = args[i].Type;

      if ((argtype & FDF_SPAN) IS FDF_SPAN) {
         scan += sizeof(APTR);
      }
      else if ((argtype & FDF_VECTOR) IS FDF_VECTOR) {
         if (argtype & FD_RESULT) {
            auto var = ((APTR *)scan)[0];
            scan += sizeof(APTR);
            if (var) {
               std::string_view argname(args[i].Name);
               if (not push_cpp_array_result(lua, argtype, argname, var)) {
                  log.warning("Unsupported C++ array result arg %s, flags $%.8x", args[i].Name, argtype);
                  lua_pushnil(lua);
               }
            }
            else lua_pushnil(lua);
            results++;
         }
         else scan += sizeof(APTR);
      }
      else if (argtype & FD_STR) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) {
               if (argtype & FD_CPP) {
                  if (argtype & FD_MUTABLE) { // std::string variant
                     lua_pushstring(lua, *((std::string *)var));
                  }
                  else { // std::string_view
                     lua_pushstring(lua, *((std::string_view *)var));
                  }
               }
               else {
                  lua_pushstring(lua, ((STRING *)var)[0]);
                  if ((argtype & FD_ALLOC) and (((STRING *)var)[0])) FreeResource(((STRING *)var)[0]);
               }
            }
            else lua_pushnil(lua);
            results++;
         }
         scan += sizeof(APTR);
      }
      else if (argtype & (FD_PTR|FD_STRUCT)) {
         if (argtype & FD_RESULT) {
            auto var = ((APTR *)scan)[0];
            scan += sizeof(APTR);
            if (var) {
               if (argtype & FD_OBJECT) {
                  if (((APTR *)var)[0]) {
                     push_object(lua, ((OBJECTPTR *)var)[0], (argtype & FD_ALLOC) ? false : true);
                  }
                  else lua_pushnil(lua);
               }
               else if (argtype & FD_STRUCT) {
                  if (((APTR *)var)[0]) {
                     if (argtype & FD_RESOURCE) {
                        // Resource structures are managed with direct data addresses.
                        push_struct(Tiri, ((APTR *)var)[0], args[i].Name, (argtype & FD_ALLOC) ? TRUE : FALSE, TRUE);
                     }
                     else {
                        if (named_struct_to_table(lua, args[i].Name, ((APTR *)var)[0]) != ERR::Okay) lua_pushnil(lua);
                        if (argtype & FD_ALLOC) FreeResource(((APTR *)var)[0]);
                     }
                  }
                  else lua_pushnil(lua);
               }
               else if (argtype & FD_ALLOC) {
                  // Misconfigured or unsupported parameter
                  lua_pushnil(lua);
                  if (((APTR *)var)[0]) FreeResource(((APTR *)var)[0]);
               }
               else lua_pushlightuserdata(lua, ((APTR *)var)[0]);
            }
            else lua_pushnil(lua);
            results++;
         }
         else scan += sizeof(APTR);
      }
      else if (argtype & FD_INT) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushinteger(lua, ((int *)var)[0]);
            else lua_pushnil(lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(int);
      }
      else if (argtype & FD_DOUBLE) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushnumber(lua, ((double *)var)[0]);
            else lua_pushnil(lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(double);
      }
      else if (argtype & FD_INT64) {
         if (argtype & FD_RESULT) {
            if (auto var = ((APTR *)scan)[0]) lua_pushnumber(lua, ((int64_t *)var)[0]);
            else lua_pushnil(lua);
            scan += sizeof(APTR);
            results++;
         }
         else scan += sizeof(int64_t);
      }
      else {
         log.warning("Unsupported arg '%s', flags $%x, aborting now.", args[i].Name, argtype);
         return results;
      }
   }

   return results;
}

#ifdef UNIT_TESTS
//********************************************************************************************************************
// Test-only support for driving a synthetic string-view signature through the real module call bridge.

namespace {

struct synthetic_callable {
   std::vector<FunctionField> Fields;
   std::vector<std::string> Names;
   ModuleCallable Callable;
};

static bool build_synthetic_view_callable(int Count, APTR Address, synthetic_callable &Result)
{
   Result.Names.reserve(Count + 1);
   Result.Names.emplace_back("Result");
   for (int arg = 0; arg < Count; ++arg) Result.Names.emplace_back(std::format("Arg{}", arg));

   Result.Fields.reserve(Count + 2);
   Result.Fields.push_back({ Result.Names[0].c_str(), FD_VOID });
   for (int arg = 0; arg < Count; ++arg) {
      Result.Fields.push_back({ Result.Names[arg + 1].c_str(), FD_STR|FD_CPP });
   }
   Result.Fields.push_back({ nullptr, 0 });

   Result.Callable.Name    = "SyntheticViews";
   Result.Callable.Address = Address;
   Result.Callable.Fields  = Result.Fields.data();

   marshalling_profile profile;
   for (int arg = 1; arg <= Count; ++arg) {
      std::string message;
      ffi_type *type = nullptr;
      if (callable_arg_type(Result.Fields[arg], type, message) != ERR::Okay) return false;
      Result.Callable.ArgTypes[arg - 1] = type;
      profile_arg(Result.Fields[arg], profile);
   }

   Result.Callable.Profile = profile;
   ffi_type *return_type = callable_return_type(Result.Fields[0].Type);
   return ffi_prep_cif(&Result.Callable.Cif, FFI_DEFAULT_ABI, Count, return_type, Result.Callable.ArgTypes) IS FFI_OK;
}

} // namespace

std::string test_module_string_view_call(lua_State *Lua, APTR Address, std::span<const std::string> Inputs)
{
   int count = int(Inputs.size());
   if ((count < 1) or (count > MAX_MODULE_ARGS)) return std::format("Unsupported synthetic arity {}.", count);
   if (not Address) return "The synthetic entry point is null.";

   auto synthetic = std::make_unique<synthetic_callable>();
   if (not build_synthetic_view_callable(count, Address, *synthetic)) {
      return std::format("Failed to prepare the synthetic callable for arity {}.", count);
   }

   if (synthetic->Callable.Profile.StringViews != count) {
      return std::format("Profile counted {} string views for arity {}.",
         int(synthetic->Callable.Profile.StringViews), count);
   }

   int base = lua_gettop(Lua);
   lua_pushlightuserdata(Lua, &synthetic->Callable);
   lua_pushcclosure(Lua, module_call, 1);
   for (const auto &input : Inputs) lua_pushstring(Lua, input);

   if (lua_pcall(Lua, count, LUA_MULTRET, 0) != 0) {
      std::string message = lua_tostring(Lua, -1) ? lua_tostring(Lua, -1) : "unknown error";
      lua_settop(Lua, base);
      return std::format("Synthetic call of arity {} failed: {}", count, message);
   }
   lua_settop(Lua, base);
   return { };
}

// Report what the compiler's signature index and the runtime's callable lookup each resolved for one function name.
// Both paths run through static_module_signature::find_ordinal(), so the test can assert that they agree on the
// ordinal and that the names and field descriptors they expose are the signature's single owned copy.

test_module_resolution test_module_resolve(std::string_view Module, std::string_view Function)
{
   test_module_resolution result;

   ModuleBinding *binding = nullptr;
   if (resolve_module(Module, binding) != ERR::Okay) return result;

   std::lock_guard lock(glModuleRegistry.Mutex);
   if (not binding->Signature) return result;

   if (auto ordinal = binding->Signature->find_ordinal(Function); ordinal != NO_FUNCTION_ORDINAL) {
      const auto &signature = binding->Signature->Functions[ordinal];
      result.Found = true;
      result.Ordinal = ordinal;
      result.SignatureName = signature.Name.c_str();
      result.SignatureFields = signature.Fields.data();
   }

   if (auto callable = find_module_callable(binding, Function)) {
      result.CallableFound = true;
      result.CallableName = callable->Name;
      result.CallableFields = callable->Fields;
      for (size_t index = 0; index < binding->Callables.size(); ++index) {
         if (binding->Callables[index].get() IS callable) {
            result.CallableOrdinal = uint32_t(index);
            break;
         }
      }
   }

   return result;
}
#endif

//********************************************************************************************************************
// Register the module interface.

void register_module_class(lua_State *Lua)
{
   kt::Log log;

   static const struct luaL_Reg modlib_functions[] = {
      { "test", module_test },
      { nullptr, nullptr}
   };

   luaL_openlib(Lua, "mod", modlib_functions, 0);
   lua_pop(Lua, 1); // Drop the mod library table

   // Register mod interface prototypes for compile-time type inference
   reg_iface_prototype("mod", "test", { TiriType::Num, TiriType::Num }, { TiriType::Str, TiriType::Str });
}
