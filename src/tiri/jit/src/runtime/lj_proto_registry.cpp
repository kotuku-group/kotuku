// Function Prototype Registry
// Copyright (C) 2026 Paul Manias
//
// Stores type signatures for registered C functions and interface methods.
// Arena-style allocation for efficient memory management.

#include "lj_proto_registry.h"
#include "lj_ff.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "../lib/lib_range.h"
#include <kotuku/main.h>
#include <ankerl/unordered_dense.h>
#include <vector>
#include <cstring>
#include <shared_mutex>
#include <mutex>
#include <atomic>

//********************************************************************************************************************
// Arena allocator for fprototype records

class ProtoArena {
   static constexpr size_t BLOCK_SIZE = 4096;

   struct Block {
      std::unique_ptr<uint8_t[]> data;
      size_t used = 0;

      explicit Block() : data(std::make_unique<uint8_t[]>(BLOCK_SIZE)) { }
   };

   std::vector<Block> mBlocks;

public:
   void * allocate(size_t Size, size_t Alignment = alignof(std::max_align_t)) {
      if (mBlocks.empty() or (align_up(mBlocks.back().used, Alignment) + Size > BLOCK_SIZE)) {
         mBlocks.emplace_back();
      }

      Block &blk = mBlocks.back();
      size_t aligned_offset = align_up(blk.used, Alignment);
      void *ptr = blk.data.get() + aligned_offset;
      blk.used = aligned_offset + Size;
      return ptr;
   }

   void clear() { mBlocks.clear(); }

private:
   static constexpr size_t align_up(size_t Value, size_t Alignment) noexcept {
      return (Value + Alignment - 1) & ~(Alignment - 1);
   }
};

//********************************************************************************************************************
// Global registry state

static ProtoArena glArena;
static ankerl::unordered_dense::map<ProtoKey, fprototype*, ProtoKeyHash> glRegistry;

struct MethodKey {
   TiriType receiver_type;
   uint32_t member_hash;

   [[nodiscard]] bool operator==(const MethodKey &) const = default;
};

struct MethodKeyHash {
   [[nodiscard]] size_t operator()(const MethodKey &Key) const noexcept
   {
      return size_t(Key.member_hash) ^ (size_t(uint8_t(Key.receiver_type)) << 32);
   }
};

static ankerl::unordered_dense::map<MethodKey, fprototype*, MethodKeyHash> glMethodRegistry;
static std::shared_mutex glRegistryMutex;

// The registry is built once and never mutated afterwards.  Registration runs from luaL_openlibs(), which executes
// for every Tiri script object rather than once per process, so states 2..N repeat the same registration calls and
// find every entry already present.  Those repeats are content-identical: a given key always resolves to the same
// prototype.
static std::atomic<bool> glRegistrySealed{false};

//********************************************************************************************************************

void init_proto_registry()
{
   static std::once_flag flag;
   std::call_once(flag, []() {
      glArena.clear();
      glRegistry.clear();
      glMethodRegistry.clear();
   });
}

//********************************************************************************************************************

void seal_proto_registry()
{
   std::unique_lock lock(glRegistryMutex);
   glRegistrySealed.store(true, std::memory_order_release);
}

//********************************************************************************************************************
// Internal helper to allocate and initialise a prototype

static fprototype * alloc_prototype(std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags, TiriType ReceiverType = TiriType::Unknown,
   BuiltinCallableID Callable = BuiltinCallableID::Invalid)
{
   size_t struct_size = sizeof(fprototype) + ParamTypes.size() * sizeof(TiriType);
   auto *proto = (fprototype*)glArena.allocate(struct_size, alignof(fprototype));

   proto->result_count = uint8_t(std::min(ResultTypes.size(), PROTO_MAX_RETURN_TYPES));
   proto->param_count = uint8_t(std::min(ParamTypes.size(), FPROTO_MAX_PARAMS));
   proto->flags = Flags;
   proto->receiver_type = ReceiverType;
   proto->builtin_callable_id = Callable;
   proto->reserved = 0;

   // Initialise result types

   size_t idx = 0;
   for (auto rt : ResultTypes) {
      if (idx >= PROTO_MAX_RETURN_TYPES) break;
      proto->result_types[idx++] = rt;
   }
   for (; idx < PROTO_MAX_RETURN_TYPES; ++idx) {
      proto->result_types[idx] = TiriType::Unknown;
   }

   // Copy parameter types

   TiriType *params = proto->param_types();
   idx = 0;
   for (auto pt : ParamTypes) {
      if (idx >= FPROTO_MAX_PARAMS) break;
      params[idx++] = pt;
   }

   return proto;
}

//********************************************************************************************************************

static bool prototype_matches(const fprototype *Prototype, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags, TiriType ReceiverType,
   BuiltinCallableID Callable)
{
   if (not Prototype or Prototype->result_count != ResultTypes.size() or
       Prototype->param_count != ParamTypes.size() or Prototype->flags != Flags or
       Prototype->receiver_type != ReceiverType or Prototype->builtin_callable_id != Callable) return false;

   size_t index = 0;
   for (TiriType type : ResultTypes) {
      if (Prototype->result_types[index++] != type) return false;
   }
   index = 0;
   for (TiriType type : ParamTypes) {
      if (Prototype->param_types()[index++] != type) return false;
   }
   return true;
}

static ERR validate_prototype_limits(
   std::initializer_list<TiriType> ResultTypes, std::initializer_list<TiriType> ParamTypes)
{
   if (ResultTypes.size() > PROTO_MAX_RETURN_TYPES or ParamTypes.size() > FPROTO_MAX_PARAMS) {
      return ERR::BufferOverflow;
   }
   return ERR::Okay;
}

static cTValue * exported_interface_member(
   lua_State *L, std::string_view Interface, std::string_view Method)
{
   GCtab *table = tabref(L->env);
   size_t offset = 0;
   while (offset < Interface.size()) {
      size_t separator = Interface.find('.', offset);
      size_t length = separator IS std::string_view::npos ? Interface.size() - offset : separator - offset;
      GCstr *name = lj_str_new(L, Interface.data() + offset, length);
      cTValue *value = lj_tab_getstr(table, name);
      if (not value or not tvistab(value)) return nullptr;
      table = tabV(value);
      if (separator IS std::string_view::npos) break;
      offset = separator + 1;
   }
   return lj_tab_getstr(table, lj_str_new(L, Method.data(), Method.size()));
}

static ERR validate_method_state(lua_State *L, std::string_view Interface, std::string_view Method,
   BuiltinCallableID Callable)
{
   if (not L or not builtin_callable_valid(Callable)) return ERR::InvalidValue;
   const char *canonical_name = builtin_callable_name(Callable);
   if (not canonical_name) return ERR::InvalidValue;
   std::string expected_name(Interface);
   expected_name.push_back('.');
   expected_name.append(Method);
   if (expected_name != canonical_name) {
      std::string object_alias("object.");
      object_alias.append(Method);
      if (Interface != "obj" or object_alias != canonical_name) return ERR::Mismatch;
   }

   GCfunc *canonical = lj_builtin_callable(L, Callable);
   cTValue *exported = exported_interface_member(L, Interface, Method);
   if (not canonical or not exported or not tvisfunc(exported) or funcV(exported) != canonical) return ERR::Mismatch;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR reg_func_prototype(std::string_view Name, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags)
{
   if (validate_prototype_limits(ResultTypes, ParamTypes) != ERR::Okay) return ERR::BufferOverflow;
   ProtoKey key{ 0, kt::strhash(Name) };

   { // Fast path: check under shared lock first (common case after first script init)
      std::shared_lock read_lock(glRegistryMutex);
      if (auto existing = glRegistry.find(key); existing != glRegistry.end()) {
         return prototype_matches(existing->second, ResultTypes, ParamTypes, Flags, TiriType::Unknown,
            BuiltinCallableID::Invalid) ? ERR::Exists : ERR::Mismatch;
      }
   }

   std::unique_lock lock(glRegistryMutex);
   if (auto existing = glRegistry.find(key); existing != glRegistry.end()) {
      return prototype_matches(existing->second, ResultTypes, ParamTypes, Flags, TiriType::Unknown,
         BuiltinCallableID::Invalid) ? ERR::Exists : ERR::Mismatch;
   }

   lj_assertX(not glRegistrySealed.load(std::memory_order_acquire), // See reg_iface_method() for the rationale.
      "function prototype '%.*s' registered after the prototype registry was sealed",
      int(Name.size()), Name.data());

   auto *proto = alloc_prototype(ResultTypes, ParamTypes, Flags);
   glRegistry[key] = proto;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR reg_iface_prototype(std::string_view Interface, std::string_view Method, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags)
{
   if (validate_prototype_limits(ResultTypes, ParamTypes) != ERR::Okay) return ERR::BufferOverflow;
   ProtoKey key{ kt::strhash(Interface), kt::strhash(Method) };

   {
      std::shared_lock read_lock(glRegistryMutex);
      if (auto existing = glRegistry.find(key); existing != glRegistry.end()) {
         return prototype_matches(existing->second, ResultTypes, ParamTypes, Flags, TiriType::Unknown,
            BuiltinCallableID::Invalid) ? ERR::Exists : ERR::Mismatch;
      }
   }

   std::unique_lock lock(glRegistryMutex);
   if (auto existing = glRegistry.find(key); existing != glRegistry.end()) {
      return prototype_matches(existing->second, ResultTypes, ParamTypes, Flags, TiriType::Unknown,
         BuiltinCallableID::Invalid) ? ERR::Exists : ERR::Mismatch;
   }

   lj_assertX(not glRegistrySealed.load(std::memory_order_acquire), // See reg_iface_method() for the rationale.
      "interface prototype '%.*s' registered after the prototype registry was sealed",
      int(Method.size()), Method.data());

   auto *proto = alloc_prototype(ResultTypes, ParamTypes, Flags);
   glRegistry[key] = proto;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR reg_iface_method(lua_State *L, std::string_view Interface, std::string_view Method, TiriType ReceiverType,
   BuiltinCallableID Callable, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags)
{
   if (Interface.empty() or Method.empty() or ReceiverType IS TiriType::Any or
       ReceiverType IS TiriType::Unknown or ReceiverType IS TiriType::Nil or
       ParamTypes.size() IS 0 or *ParamTypes.begin() != ReceiverType) return ERR::InvalidValue;
   ERR limits = validate_prototype_limits(ResultTypes, ParamTypes);
   if (limits != ERR::Okay) return limits;
   ERR state_validation = validate_method_state(L, Interface, Method, Callable);
   if (state_validation != ERR::Okay) return state_validation;

   ProtoKey prototype_key{ kt::strhash(Interface), kt::strhash(Method) };
   MethodKey method_key{ ReceiverType, kt::strhash(Method) };

   { // Repeated state initialisation only needs shared access after its state-local callable validation.
      std::shared_lock read_lock(glRegistryMutex);
      auto existing_prototype = glRegistry.find(prototype_key);
      if (existing_prototype != glRegistry.end()) {
         if (not prototype_matches(existing_prototype->second, ResultTypes, ParamTypes, Flags, ReceiverType,
               Callable)) return ERR::Mismatch;
         auto existing_method = glMethodRegistry.find(method_key);
         return existing_method != glMethodRegistry.end() and
            existing_method->second IS existing_prototype->second ? ERR::Exists : ERR::Mismatch;
      }
      if (glMethodRegistry.contains(method_key)) return ERR::Mismatch;
   }

   std::unique_lock lock(glRegistryMutex);

   auto existing_prototype = glRegistry.find(prototype_key);
   if (existing_prototype != glRegistry.end()) {
      if (not prototype_matches(existing_prototype->second, ResultTypes, ParamTypes, Flags, ReceiverType, Callable)) {
         return ERR::Mismatch;
      }
      auto existing_method = glMethodRegistry.find(method_key);
      if (existing_method IS glMethodRegistry.end() or existing_method->second != existing_prototype->second) {
         return ERR::Mismatch;
      }
      return ERR::Exists;
   }

   if (auto existing_method = glMethodRegistry.find(method_key); existing_method != glMethodRegistry.end()) {
      return ERR::Mismatch;
   }

   // A new entry after sealing would mutate the maps while lock-free readers are running, which the writer lock does
   // not exclude.  Registration is expected to be complete before the seal, so reaching here indicates a library that
   // registers prototypes outside the startup pass - that must be fixed at the call site rather than tolerated.
   lj_assertX(not glRegistrySealed.load(std::memory_order_acquire),
      "interface method '%.*s' registered after the prototype registry was sealed",
      int(Method.size()), Method.data());

   auto *prototype = alloc_prototype(ResultTypes, ParamTypes, Flags, ReceiverType, Callable);
   glRegistry[prototype_key] = prototype;
   glMethodRegistry[method_key] = prototype;
   return ERR::Okay;
}

//********************************************************************************************************************

const fprototype * get_prototype(std::string_view Interface, std::string_view Method)
{
   return get_prototype_by_hash(kt::strhash(Interface), kt::strhash(Method));
}

const fprototype * get_func_prototype(std::string_view Name)
{
   return get_func_prototype_by_hash(kt::strhash(Name));
}

const fprototype * get_method_prototype(TiriType ReceiverType, std::string_view Method)
{
   return get_method_prototype_by_hash(ReceiverType, kt::strhash(Method));
}

//********************************************************************************************************************

const fprototype * get_prototype_by_hash(uint32_t IfaceHash, uint32_t FuncHash)
{
   ProtoKey key{ IfaceHash, FuncHash };

   if (glRegistrySealed.load(std::memory_order_acquire)) { // Sealed: no writer can run, so no lock is required.
      auto it = glRegistry.find(key);
      return (it != glRegistry.end()) ? it->second : nullptr;
   }

   std::shared_lock lock(glRegistryMutex);
   auto it = glRegistry.find(key);
   return (it != glRegistry.end()) ? it->second : nullptr;
}

const fprototype * get_func_prototype_by_hash(uint32_t FuncHash)
{
   return get_prototype_by_hash(0, FuncHash);
}

const fprototype * get_method_prototype_by_hash(TiriType ReceiverType, uint32_t MethodHash)
{
   const MethodKey key{ ReceiverType, MethodHash };

   if (glRegistrySealed.load(std::memory_order_acquire)) { // Sealed: no writer can run, so no lock is required.
      auto it = glMethodRegistry.find(key);
      return it != glMethodRegistry.end() ? it->second : nullptr;
   }

   std::shared_lock lock(glRegistryMutex);
   auto it = glMethodRegistry.find(key);
   return it != glMethodRegistry.end() ? it->second : nullptr;
}

//********************************************************************************************************************

bool is_range_userdata(lua_State *L, GCudata *Userdata)
{
   GCtab *metatable = tabref(Userdata->metatable);
   if (not metatable) return false;

   cTValue *registered = lj_tab_getstr(tabV(registry(L)), lj_str_newz(L, RANGE_METATABLE));
   return registered and tvistab(registered) and tabV(registered) IS metatable;
}

//********************************************************************************************************************

TiriType runtime_receiver_type(lua_State *L, cTValue *Receiver)
{
   if (tvistab(Receiver)) return TiriType::Table;
   if (tvisstr(Receiver)) return TiriType::Str;
   if (tvisarray(Receiver)) return TiriType::Array;
   if (tvisobject(Receiver)) return TiriType::Object;
   if (tvisstruct(Receiver)) return TiriType::Struct;
   if (tvisudata(Receiver)) {
      return is_range_userdata(L, udataV(Receiver)) ? TiriType::Range : TiriType::Userdata;
   }
   if (tvislightud(Receiver)) return TiriType::Userdata;
   return TiriType::Unknown;
}

//********************************************************************************************************************

void lj_bmeth_mark_method_compatible(GCtab *Metatable)
{
   if (Metatable) Metatable->flags |= TAB_METHOD_COMPATIBLE;
}

//********************************************************************************************************************

bool lj_bmeth_is_method_compatible(cTValue *Receiver)
{
   if (not Receiver or not tvisudata(Receiver)) return false;
   GCtab *metatable = tabref(udataV(Receiver)->metatable);
   return metatable and (metatable->flags & TAB_METHOD_COMPATIBLE) != 0;
}

//********************************************************************************************************************

extern "C" int32_t lj_bmeth_lookup(lua_State *L, cTValue *Receiver, GCstr *Method)
{
   if (not Receiver or not Method) return LJ_BMETH_FIELD_CALL;
   const fprototype *prototype = get_method_prototype_by_hash(runtime_receiver_type(L, Receiver), Method->hash);
   if (prototype and builtin_callable_valid(prototype->builtin_callable_id)) {
      return int32_t(builtin_callable_index(prototype->builtin_callable_id));
   }
   return lj_bmeth_is_method_compatible(Receiver) ? LJ_BMETH_COMPATIBLE_CALL : LJ_BMETH_FIELD_CALL;
}
