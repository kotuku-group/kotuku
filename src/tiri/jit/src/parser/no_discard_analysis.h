#pragma once

class ParserContext;
struct BlockStmt;

void validate_no_discard_results(ParserContext &Context, const BlockStmt &Module);
