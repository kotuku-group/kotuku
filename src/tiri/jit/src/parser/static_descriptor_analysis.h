#pragma once

class ParserContext;
struct BlockStmt;

void discover_static_bindings(ParserContext &, BlockStmt &);
void propagate_static_descriptors(ParserContext &, BlockStmt &);

