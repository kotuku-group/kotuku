
#pragma once

#include "parser_context.h"
#include <functional>
#include <string_view>

using BytecodeLogger = std::function<void(std::string_view, void *)>;

extern void dump_bytecode(FuncState &fs);
extern void trace_proto_bytecode(lua_State *L, GCproto *Proto, BytecodeLogger Logger, void *Meta, bool Verbose = false, int Indent = 0);
