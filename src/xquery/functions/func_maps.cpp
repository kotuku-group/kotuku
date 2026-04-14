#include "func_map_array_common.h"

namespace
{
   enum class MapKeyStatus
   {
      Empty,
      Valid,
      Invalid
   };

   MapKeyStatus extract_map_key(const XPathVal &KeyArg, std::string &OutKey, const XPathContext &Context)
   {
      size_t count = sequence_length(KeyArg);
      if (count IS 0) return MapKeyStatus::Empty;
      if (count > 1) {
         flag_xpath_unsupported(Context);
         return MapKeyStatus::Invalid;
      }

      XPathVal key_value = extract_sequence_item(KeyArg, 0);
      if ((key_value.Type IS XPVT::Map) or (key_value.Type IS XPVT::Array)) {
         flag_xpath_unsupported(Context);
         return MapKeyStatus::Invalid;
      }

      OutKey = sequence_item_string(key_value, 0);
      return MapKeyStatus::Valid;
   }

   const XPathMapEntry * find_entry(const XPathMapStorage *Storage, std::string_view Key)
   {
      if (!Storage) return nullptr;
      for (const auto &entry : Storage->entries) {
         if (entry.key IS Key) return &entry;
      }
      return nullptr;
   }

   XPathMapEntry * find_entry(std::shared_ptr<XPathMapStorage> &Storage, std::string_view Key)
   {
      if (!Storage) return nullptr;
      for (auto &entry : Storage->entries) {
         if (entry.key IS Key) return &entry;
      }
      return nullptr;
   }

   std::shared_ptr<XPathMapStorage> clone_map_storage(const XPathVal &MapValue)
   {
      if ((MapValue.Type != XPVT::Map) or (!MapValue.map_storage)) {
         return std::make_shared<XPathMapStorage>();
      }
      return std::make_shared<XPathMapStorage>(*MapValue.map_storage);
   }

   void copy_entry_value(const XPathValueSequence &Source, XPathValueSequence &Target)
   {
      Target = Source;
   }
}

XPathVal XPathFunctionLibrary::function_map_entry(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return make_map_result(std::make_shared<XPathMapStorage>());

   std::string canonical_key;
   MapKeyStatus key_status = extract_map_key(Args[0], canonical_key, Context);
   if (key_status IS MapKeyStatus::Invalid) return make_map_result(std::make_shared<XPathMapStorage>());
   if (key_status IS MapKeyStatus::Empty) return make_map_result(std::make_shared<XPathMapStorage>());

   auto storage = std::make_shared<XPathMapStorage>();
   XPathMapEntry entry;
   entry.key = std::move(canonical_key);
   sequence_from_xpath_value(Args[1], entry.value);
   storage->entries.push_back(std::move(entry));
   return make_map_result(std::move(storage));
}

XPathVal XPathFunctionLibrary::function_map_put(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 3) return (Args.empty()) ? make_map_result(std::make_shared<XPathMapStorage>()) : Args[0];

   std::string canonical_key;
   MapKeyStatus key_status = extract_map_key(Args[1], canonical_key, Context);
   if (key_status IS MapKeyStatus::Invalid) return Args[0];
   if (key_status IS MapKeyStatus::Empty) return Args[0];

   auto storage = clone_map_storage(Args[0]);
   XPathMapEntry *existing = find_entry(storage, canonical_key);
   if (!existing) {
      XPathMapEntry entry;
      entry.key = canonical_key;
      sequence_from_xpath_value(Args[2], entry.value);
      storage->entries.push_back(std::move(entry));
   }
   else {
      sequence_from_xpath_value(Args[2], existing->value);
   }

   return make_map_result(std::move(storage));
}

XPathVal XPathFunctionLibrary::function_map_get(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal(pf::vector<XTag *>{});

   std::string canonical_key;
   MapKeyStatus key_status = extract_map_key(Args[1], canonical_key, Context);
   if (key_status IS MapKeyStatus::Invalid) return XPathVal(pf::vector<XTag *>{});
   if (key_status IS MapKeyStatus::Empty) return XPathVal(pf::vector<XTag *>{});

   const XPathMapStorage *storage = (Args[0].Type IS XPVT::Map) ? Args[0].map_storage.get() : nullptr;
   const XPathMapEntry *entry = find_entry(storage, canonical_key);
   if (!entry) return XPathVal(pf::vector<XTag *>{});

   return materialise_sequence_with_context(entry->value, Context);
}

XPathVal XPathFunctionLibrary::function_map_contains(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal(false);

   std::string canonical_key;
   MapKeyStatus key_status = extract_map_key(Args[1], canonical_key, Context);
   if (key_status IS MapKeyStatus::Invalid) return XPathVal(false);
   if (key_status IS MapKeyStatus::Empty) return XPathVal(false);

   const XPathMapStorage *storage = (Args[0].Type IS XPVT::Map) ? Args[0].map_storage.get() : nullptr;
   const XPathMapEntry *entry = find_entry(storage, canonical_key);
   return XPathVal(entry != nullptr);
}

XPathVal XPathFunctionLibrary::function_map_size(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(0.0);
   if ((Args[0].Type != XPVT::Map) or (!Args[0].map_storage)) return XPathVal(0.0);
   return XPathVal(double(Args[0].map_storage->size()));
}

XPathVal XPathFunctionLibrary::function_map_keys(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(pf::vector<XTag *>{});
   if ((Args[0].Type != XPVT::Map) or (!Args[0].map_storage)) return XPathVal(pf::vector<XTag *>{});

   SequenceBuilder builder;
   for (const auto &entry : Args[0].map_storage->entries) {
      builder.nodes.push_back(nullptr);
      builder.attributes.push_back(nullptr);
      builder.strings.push_back(entry.key);
   }

   return make_sequence_value(std::move(builder));
}

XPathVal XPathFunctionLibrary::function_map_merge(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   (void)Context;

   auto storage = std::make_shared<XPathMapStorage>();

   auto merge_single = [&](const XPathVal &Value) {
      if ((Value.Type != XPVT::Map) or (!Value.map_storage)) return;
      for (const auto &entry : Value.map_storage->entries) {
         XPathMapEntry *target = find_entry(storage, entry.key);
         if (!target) {
            storage->entries.push_back(entry);
         }
         else {
            copy_entry_value(entry.value, target->value);
         }
      }
   };

   for (const auto &arg : Args) {
      visit_sequence_values(arg, merge_single);
   }

   return make_map_result(std::move(storage));
}
