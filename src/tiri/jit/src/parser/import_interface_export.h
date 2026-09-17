#pragma once

#include <string>

struct BlockStmt;
class ParserContext;

// Build and install the portable compile-time interfaces for every cold imported module after semantic analysis has
// completed.  Warm entries already own a decoded interface and are left unchanged.
[[nodiscard]] bool prepare_import_interfaces(ParserContext &, BlockStmt &, std::string &Diagnostic);
