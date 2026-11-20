#pragma once

class ParserContext;
struct BlockStmt;
struct FunctionExprPayload;
struct NameRef;
struct StaticBindingDescriptor;

[[nodiscard]] const FunctionExprPayload * resolve_forward_function(const NameRef &, const StaticBindingDescriptor &);
void discover_static_bindings(ParserContext &, BlockStmt &);
void propagate_native_object_descriptors(ParserContext &, BlockStmt &);
void propagate_static_descriptors(ParserContext &, BlockStmt &);
