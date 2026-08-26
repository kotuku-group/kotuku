#pragma once

class ParserContext;
struct BlockStmt;

void resolve_assignment_targets(ParserContext &, BlockStmt &);
