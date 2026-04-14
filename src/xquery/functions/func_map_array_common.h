#pragma once

#include <memory>

inline void flag_xpath_unsupported(const XPathContext &Context)
{
   if (Context.expression_unsupported) *Context.expression_unsupported = true;
}

inline bool xpath_value_is_empty_sequence(const XPathVal &Value)
{
   if (Value.Type != XPVT::NodeSet) return false;
   bool has_nodes = !Value.node_set.empty();
   bool has_attributes = !Value.node_set_attributes.empty();
   bool has_strings = !Value.node_set_string_values.empty();
   bool has_override = Value.node_set_string_override.has_value();
   bool has_composites = false;
   if (!Value.node_set_composite_values.empty()) {
      for (const auto &stored : Value.node_set_composite_values) {
         if (stored) {
            has_composites = true;
            break;
         }
      }
   }
   return (!has_nodes) and (!has_attributes) and (!has_strings) and (!has_override) and (!has_composites);
}

inline XPathVal clone_composite_value(const XPathValue &Source)
{
   XPathVal clone;
   clone.Type = Source.Type;
   clone.NumberValue = Source.NumberValue;
   clone.StringValue = Source.StringValue;
   clone.node_set = Source.node_set;
   clone.node_set_string_override = Source.node_set_string_override;
   clone.node_set_string_values = Source.node_set_string_values;
   clone.node_set_attributes = Source.node_set_attributes;
   clone.node_set_composite_values = Source.node_set_composite_values;
   clone.preserve_node_order = Source.preserve_node_order;
   clone.map_storage = Source.map_storage;
   clone.array_storage = Source.array_storage;
   return clone;
}

template <typename Callback>
inline void visit_sequence_values(const XPathVal &Value, Callback &&Fn)
{
   if ((Value.Type IS XPVT::NodeSet) and (!Value.node_set_composite_values.empty())) {
      for (const auto &stored : Value.node_set_composite_values) {
         if (!stored) continue;
         XPathVal clone = clone_composite_value(*stored);
         visit_sequence_values<Callback>(clone, Fn);
      }
      return;
   }

   Fn(Value);
}

inline void sequence_from_xpath_value(const XPathVal &Value, XPathValueSequence &Sequence)
{
   Sequence.reset();
   if ((Value.Type IS XPVT::NodeSet) and xpath_value_is_empty_sequence(Value)) return;
   Sequence.items.push_back(Value);
}

inline XPathVal materialise_sequence_with_context(const XPathValueSequence &Sequence, const XPathContext &Context)
{
   if (Context.eval) return Context.eval->materialise_sequence_value(Sequence);
   if (Sequence.items.empty()) return XPathVal(pf::vector<XTag *>{});

   XPathVal clone;
   XPathValue &base = clone;
   base = Sequence.items.front();
   return clone;
}

inline XPathVal make_map_result(std::shared_ptr<XPathMapStorage> Storage)
{
   XPathVal result;
   result.Type = XPVT::Map;
   result.map_storage = std::move(Storage);
   return result;
}

inline XPathVal make_array_result(std::shared_ptr<XPathArrayStorage> Storage)
{
   XPathVal result;
   result.Type = XPVT::Array;
   result.array_storage = std::move(Storage);
   return result;
}
