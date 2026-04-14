// Function Prototype Registry
// Copyright (C) 2026 Paul Manias
//
// Stores type signatures for registered C functions and interface methods.
// Arena-style allocation for efficient memory management.

#include "lj_proto_registry.h"
#include <kotuku/main.h>
#include <ankerl/unordered_dense.h>
#include <vector>
#include <cstring>
#include <shared_mutex>
#include <mutex>

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
static std::shared_mutex glRegistryMutex;

//********************************************************************************************************************

void init_proto_registry()
{
   static std::once_flag flag;
   std::call_once(flag, []() {
      glArena.clear();
      glRegistry.clear();
   });
}

//********************************************************************************************************************
// Internal helper to allocate and initialise a prototype

static fprototype * alloc_prototype(std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags)
{
   size_t struct_size = sizeof(fprototype) + ParamTypes.size() * sizeof(TiriType);
   auto *proto = (fprototype*)glArena.allocate(struct_size, alignof(fprototype));

   proto->result_count = uint8_t(std::min(ResultTypes.size(), PROTO_MAX_RETURN_TYPES));
   proto->param_count = uint8_t(std::min(ParamTypes.size(), FPROTO_MAX_PARAMS));
   proto->flags = Flags;
   proto->_pad = 0;

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

ERR reg_func_prototype(std::string_view Name, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags)
{
   ProtoKey key{ 0, pf::strhash(Name) };

   { // Fast path: check under shared lock first (common case after first script init)
      std::shared_lock read_lock(glRegistryMutex);
      if (glRegistry.contains(key)) return ERR::Exists;
   }

   std::unique_lock lock(glRegistryMutex);
   if (glRegistry.contains(key)) return ERR::Exists; // Re-check after acquiring exclusive lock

   auto *proto = alloc_prototype(ResultTypes, ParamTypes, Flags);
   glRegistry[key] = proto;
   return ERR::Okay;
}

//********************************************************************************************************************

ERR reg_iface_prototype(std::string_view Interface, std::string_view Method, std::initializer_list<TiriType> ResultTypes,
   std::initializer_list<TiriType> ParamTypes, FProtoFlags Flags)
{
   ProtoKey key{ pf::strhash(Interface), pf::strhash(Method) };

   {
      std::shared_lock read_lock(glRegistryMutex);
      if (glRegistry.contains(key)) return ERR::Exists;
   }

   std::unique_lock lock(glRegistryMutex);
   if (glRegistry.contains(key)) return ERR::Exists;

   auto *proto = alloc_prototype(ResultTypes, ParamTypes, Flags);
   glRegistry[key] = proto;
   return ERR::Okay;
}

//********************************************************************************************************************

const fprototype * get_prototype(std::string_view Interface, std::string_view Method)
{
   return get_prototype_by_hash(pf::strhash(Interface), pf::strhash(Method));
}

const fprototype * get_func_prototype(std::string_view Name)
{
   return get_func_prototype_by_hash(pf::strhash(Name));
}

//********************************************************************************************************************

const fprototype * get_prototype_by_hash(uint32_t IfaceHash, uint32_t FuncHash)
{
   std::shared_lock lock(glRegistryMutex);
   ProtoKey key{ IfaceHash, FuncHash };
   auto it = glRegistry.find(key);
   return (it != glRegistry.end()) ? it->second : nullptr;
}

const fprototype * get_func_prototype_by_hash(uint32_t FuncHash)
{
   return get_prototype_by_hash(0, FuncHash);
}
