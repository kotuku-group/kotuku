#include "func_map_array_common.h"
#include "../api/xquery_errors.h"

namespace
{
   enum class ArrayIndexState
   {
      Valid,
      Invalid,
      OutOfRange
   };

   const XPathArrayStorage * view_array_storage(const XPathVal &Value)
   {
      return (Value.Type IS XPVT::Array) ? Value.array_storage.get() : nullptr;
   }

   std::shared_ptr<XPathArrayStorage> clone_array_storage(const XPathVal &Value)
   {
      if ((Value.Type IS XPVT::Array) and Value.array_storage) {
         return std::make_shared<XPathArrayStorage>(*Value.array_storage);
      }
      return std::make_shared<XPathArrayStorage>();
   }

   ArrayIndexState parse_array_index(const XPathVal &IndexArg, size_t Length, size_t &OutIndex, const XPathContext &Context)
   {
      size_t count = sequence_length(IndexArg);
      if (count IS 0) {
         flag_xpath_unsupported(Context);
         return ArrayIndexState::Invalid;
      }
      if (count > 1) {
         flag_xpath_unsupported(Context);
         return ArrayIndexState::Invalid;
      }

      XPathVal numeric = extract_sequence_item(IndexArg, 0);
      double value = numeric.to_number();
      if (std::isnan(value) or std::isinf(value)) {
         flag_xpath_unsupported(Context);
         return ArrayIndexState::Invalid;
      }

      long long floored = (long long)std::floor(value);
      if (floored < 1) return ArrayIndexState::OutOfRange;

      OutIndex = (size_t)(floored - 1);
      if (OutIndex >= Length) return ArrayIndexState::OutOfRange;
      return ArrayIndexState::Valid;
   }

   size_t compute_insert_index(const XPathVal &IndexArg, size_t Length)
   {
      size_t count = sequence_length(IndexArg);
      if (count IS 0) return 0;

      XPathVal numeric = extract_sequence_item(IndexArg, 0);
      double value = numeric.to_number();
      if (std::isnan(value)) return 0;
      if (std::isinf(value)) return (value > 0.0) ? Length : 0;

      long long floored = (long long)std::floor(value);
      if (floored <= 1) return 0;
      if (floored > (long long)Length) return Length;
      return (size_t)(floored - 1);
   }

   void flatten_member(const XPathValueSequence &Member, const XPathContext &Context, XPathArrayStorage &Target)
   {
      XPathVal runtime = materialise_sequence_with_context(Member, Context);
      if ((runtime.Type IS XPVT::Array) and runtime.array_storage) {
         for (const auto &nested : runtime.array_storage->members) flatten_member(nested, Context, Target);
         return;
      }

      Target.members.push_back(Member);
   }
}

XPathVal XPathFunctionLibrary::function_array_size(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return XPathVal(0.0);
   const XPathArrayStorage *storage = view_array_storage(Args[0]);
   if (!storage) return XPathVal(0.0);
   return XPathVal(double(storage->size()));
}

XPathVal XPathFunctionLibrary::function_array_get(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return XPathVal(pf::vector<XTag *>{});
   const XPathArrayStorage *storage = view_array_storage(Args[0]);
   if (!storage) return XPathVal(pf::vector<XTag *>{});

   size_t index = 0;
   ArrayIndexState state = parse_array_index(Args[1], storage->members.size(), index, Context);
   if (state IS ArrayIndexState::Invalid) return XPathVal(pf::vector<XTag *>{});
   if (state IS ArrayIndexState::OutOfRange) {
      if (Context.eval) {
         auto detail = std::format("Array index {} is outside the available range.", sequence_item_string(Args[1], 0));
         auto message = xquery::errors::array_index_out_of_bounds(detail);
         Context.eval->record_error(message, true);
      }
      flag_xpath_unsupported(Context);
      return XPathVal(pf::vector<XTag *>{});
   }

   return materialise_sequence_with_context(storage->members[index], Context);
}

XPathVal XPathFunctionLibrary::function_array_append(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return (Args.empty()) ? make_array_result(std::make_shared<XPathArrayStorage>()) : Args[0];

   auto storage = clone_array_storage(Args[0]);
   XPathValueSequence member;
   sequence_from_xpath_value(Args[1], member);
   storage->members.push_back(std::move(member));
   return make_array_result(std::move(storage));
}

XPathVal XPathFunctionLibrary::function_array_insert_before(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 3) return (Args.empty()) ? make_array_result(std::make_shared<XPathArrayStorage>()) : Args[0];

   auto storage = clone_array_storage(Args[0]);
   size_t length = storage->members.size();
   size_t insert_index = compute_insert_index(Args[1], length);
   if (insert_index > length) insert_index = length;

   XPathValueSequence member;
   sequence_from_xpath_value(Args[2], member);
   storage->members.insert(storage->members.begin() + insert_index, std::move(member));
   return make_array_result(std::move(storage));
}

XPathVal XPathFunctionLibrary::function_array_remove(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.size() < 2) return (Args.empty()) ? make_array_result(std::make_shared<XPathArrayStorage>()) : Args[0];
   auto storage = clone_array_storage(Args[0]);
   if (storage->members.empty()) return make_array_result(std::move(storage));

   size_t index = 0;
   ArrayIndexState state = parse_array_index(Args[1], storage->members.size(), index, Context);
   if (state IS ArrayIndexState::Invalid) return make_array_result(std::move(storage));
   if (state IS ArrayIndexState::OutOfRange) return make_array_result(std::move(storage));

   storage->members.erase(storage->members.begin() + index);
   return make_array_result(std::move(storage));
}

XPathVal XPathFunctionLibrary::function_array_join(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   (void)Context;

   auto storage = std::make_shared<XPathArrayStorage>();
   auto append_members = [&](const XPathVal &Value) {
      const XPathArrayStorage *source = view_array_storage(Value);
      if (!source) return;
      for (const auto &member : source->members) storage->members.push_back(member);
   };

   for (const auto &arg : Args) visit_sequence_values(arg, append_members);

   return make_array_result(std::move(storage));
}

XPathVal XPathFunctionLibrary::function_array_flatten(const std::vector<XPathVal> &Args, const XPathContext &Context)
{
   if (Args.empty()) return make_array_result(std::make_shared<XPathArrayStorage>());
   const XPathArrayStorage *storage = view_array_storage(Args[0]);
   if (!storage) return make_array_result(std::make_shared<XPathArrayStorage>());

   auto flattened = std::make_shared<XPathArrayStorage>();
   for (const auto &member : storage->members) flatten_member(member, Context, *flattened);
   return make_array_result(std::move(flattened));
}
