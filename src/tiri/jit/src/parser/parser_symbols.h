// Parser symbol metadata for validation and LSP tooling.
// Copyright © 2025-2026 Paul Manias

#pragma once

#include <string>
#include <utility>
#include <vector>

#include "lexer.h"

struct BlockStmt;
struct lua_State;

struct ParserAnnotationMetadata {
   std::string name;
   std::vector<std::pair<std::string, std::string>> args;
};

struct ParserDocBlockMetadata {
   std::string summary;
   std::string body;
   std::string raw;
   std::vector<std::string> examples;
};

struct ParserDocParamMetadata {
   std::string name;
   std::string type;
   std::string doc;
   bool inferred = false;
};

struct ParserDocReturnMetadata {
   std::string type;
   std::string doc;
   bool inferred = false;
};

struct ParserDocErrorMetadata {
   std::string code;
   std::string doc;
   bool inferred = false;
};

struct ParserSymbolMetadata {
   std::string name;
   std::string kind;
   std::string signature;
   SourceSpan span{};
   SourceSpan end_span{};
   std::vector<ParserAnnotationMetadata> annotations;
   ParserDocBlockMetadata doc;
   std::vector<ParserDocParamMetadata> params;
   std::vector<ParserDocReturnMetadata> results;
   std::vector<ParserDocErrorMetadata> errors;
};

struct ParserSymbolCollection {
   std::vector<ParserSymbolMetadata> symbols;
};

void collect_parser_symbols(lua_State &, const BlockStmt &);
