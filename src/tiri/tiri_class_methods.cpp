// Support methods for the Script class

//********************************************************************************************************************
// NB: Parameter definitions are managed in the Script base class.

static ERR TIRI_GetProcedureID(objScript *, struct sc::GetProcedureID *);
static ERR TIRI_DerefProcedure(objScript *, struct sc::DerefProcedure *);
static ERR TIRI_DebugLog(objScript *, struct sc::DebugLog *);

static const MethodEntry clMethods[] = {
   { sc::GetProcedureID::id, (APTR)TIRI_GetProcedureID, "GetProcedureID", nullptr, 0 },
   { sc::DerefProcedure::id, (APTR)TIRI_DerefProcedure, "DerefProcedure", nullptr, 0 },
   { sc::DebugLog::id,       (APTR)TIRI_DebugLog,       "DebugLog", nullptr, 0 },
   { AC::NIL, nullptr, nullptr, nullptr, 0 }
};

//********************************************************************************************************************

static std::string_view tiri_type_name(TiriType Type)
{
   switch (Type) {
      case TiriType::Nil:     return "nil";
      case TiriType::Bool:    return "bool";
      case TiriType::Num:     return "number";
      case TiriType::Str:     return "string";
      case TiriType::Table:   return "table";
      case TiriType::Array:   return "array";
      case TiriType::Func:    return "function";
      case TiriType::Thread:  return "thread";
      case TiriType::Object:  return "object";
      case TiriType::Range:   return "range";
      case TiriType::Unknown: return "unknown";
      case TiriType::Any:
      default: return "any";
   }
}

//********************************************************************************************************************

static int append_dump_chunk([[maybe_unused]] lua_State *, const void *Chunk, size_t Size, void *UserData)
{
   auto bytes = (std::vector<uint8_t> *)UserData;
   if ((not bytes) or (not Chunk)) return 1;
   if (not Size) return 0; // End of dump signaled.

   std::span<const uint8_t> data_span(static_cast<const uint8_t *>(Chunk), Size);
   bytes->insert(bytes->end(), data_span.begin(), data_span.end());
   return 0;
}

//********************************************************************************************************************

static void append_hex_dump(std::span<uint8_t> Data, std::ostringstream &Buf, bool Compact)
{
   if (Data.empty()) {
      Buf << "(empty)\n";
      return;
   }

   static constexpr std::string_view hex_digits = "0123456789abcdef";

   if (Compact) {
      for (const auto byte : Data) {
         Buf << hex_digits[(byte >> 4) & 0x0f] << hex_digits[byte & 0x0f];
      }
      Buf << "\n";
      return;
   }

   static constexpr size_t bytes_per_line = 16;

   for (size_t offset = 0; offset < Data.size(); offset += bytes_per_line) {
      const size_t line_end = std::min(offset + bytes_per_line, Data.size());

      // Write offset and hex bytes
      Buf << std::format("{:04x}: ", offset);

      for (size_t index = offset; index < offset + bytes_per_line; ++index) {
         if (index < line_end) {
            const uint8_t byte = Data[index];
            Buf << hex_digits[(byte >> 4) & 0x0f] << hex_digits[byte & 0x0f];
         }
         else Buf << "  ";

         if (index + 1 < offset + bytes_per_line) Buf << ' ';
      }

      // Write ASCII representation
      Buf << "  ";
      for (size_t index = offset; index < line_end; ++index) {
         Buf << (std::isprint(Data[index]) ? char(Data[index]) : '.');
      }

      Buf << "\n";
   }
}

static void emit_stack_trace(prvTiri *Prv, std::ostringstream &Buf, bool Compact)
{
   if (not Compact) Buf << "-CALL STACK-\n";

   if constexpr (LJ_HASPROFILE) {
      size_t dump_len = 0;
      // Format codes: F=function name, l=source:line, p=preserve full path
      auto dump = luaJIT_profile_dumpstack(Prv->Lua, Compact ? "pF (l)\n" : "l f\n", 50, &dump_len);
      if (dump and dump_len) {
         // Skip the first line (level 0) which is the C function mtDebugLog itself
         auto first_newline = (CSTRING)memchr(dump, '\n', dump_len);
         if (first_newline and size_t(first_newline - dump + 1) < dump_len) {
            CSTRING start = first_newline + 1;
            size_t len = dump_len - (start - dump);
            Buf << std::string_view(start, len);
         }
      }
   }
   else {
      lua_Debug ar;
      int level = 1; // Start at 1 to skip the C function (mtDebugLog) itself

      while (lua_getstack(Prv->Lua, level, &ar)) {
         lua_getinfo(Prv->Lua, "nSl", &ar);

         if (Compact) {
            Buf << "[" << level << "] ";
            if (ar.name) Buf << ar.name;
            else Buf << "?";
            if (ar.source and ar.source[0]) Buf << " (" << ar.short_src << ":" << ar.currentline << ")";
            Buf << "\n";
         }
         else {
            Buf << "[" << level << "] ";
            if (ar.name) Buf << ar.name;
            else Buf << "<anonymous>";

            if (ar.source and ar.source[0]) {
               Buf << " (" << ar.short_src << ":" << ar.currentline << ")";
            }

            Buf << " - ";
            if (ar.what) {
               if (strcmp(ar.what, "Lua") IS 0) Buf << "Lua function";
               else if (strcmp(ar.what, "C") IS 0) Buf << "C function";
               else if (strcmp(ar.what, "main") IS 0) Buf << "main chunk";
               else Buf << ar.what;
            }
            Buf << "\n";
         }
         level++;
      }
   }

   if (not Compact) Buf <<"\n";
}

//********************************************************************************************************************

static void emit_locals_info(prvTiri *Prv, std::ostringstream &Buf, bool Compact)
{
   if (not Compact) Buf << "-LOCALS-\n";

   // In diagnosis mode, variables will be captured during compilation.

   if (not Prv->CapturedVariables.empty()) {
      for (const auto& var : Prv->CapturedVariables) {
         if (not var.is_global) {
            Buf << var.line << ":" << var.column << ":" << (var.scope.empty() ? "_" : var.scope) << ":"
               << var.name << ":" << tiri_type_name(var.type) << "\n";
         }
      }
      return;
   }

   lua_Debug ar;
   if (not lua_getstack(Prv->Lua, 1, &ar)) return; // Level 1 = caller's frame

   int idx = 1;
   CSTRING name;
   while ((name = lua_getlocal(Prv->Lua, &ar, idx))) {
      int type = lua_type(Prv->Lua, -1);
      std::string value;

      if (not Compact) {
         switch (type) {
            case LUA_TBOOLEAN: value = (lua_toboolean(Prv->Lua, -1) ? "true" : "false"); break;
            case LUA_TNUMBER:  value = lua_tostring(Prv->Lua, -1); break;
            case LUA_TSTRING: {
               size_t len;
               CSTRING str = lua_tolstring(Prv->Lua, -1, &len);
               std::string_view sv(str, len);
               if (len > 40) value = "\"" + std::string(sv.substr(0, 40)) + "...\"";
               else value = "\"" + std::string(sv) + "\"";
               break;
            }
         }
      }

      Buf << ":::" << name << ":" << lua_typename(Prv->Lua, type) << ":" << value << "\n";
      lua_pop(Prv->Lua, 1);
      idx++;
   }
}

//********************************************************************************************************************

static void emit_upvalues_info(prvTiri *Prv, std::ostringstream &Buf, bool Compact)
{
   lua_Debug ar;
   if (not lua_getstack(Prv->Lua, 1, &ar)) return; // Level 1 = caller's frame

   lua_getinfo(Prv->Lua, "f", &ar);

   if (not Compact) Buf <<"-UPVALUES-\n";

   int idx = 1;
   CSTRING name;
   while ((name = lua_getupvalue(Prv->Lua, -1, idx))) {
      int type = lua_type(Prv->Lua, -1);

      Buf << name << " = ";

      switch (type) {
         case LUA_TNIL: Buf << "nil"; break;
         case LUA_TBOOLEAN: Buf << (lua_toboolean(Prv->Lua, -1) ? "true" : "false"); break;
         case LUA_TNUMBER: Buf << lua_tonumber(Prv->Lua, -1); break;
         case LUA_TSTRING: {
            size_t len;
            CSTRING str = lua_tolstring(Prv->Lua, -1, &len);
            std::string_view sv(str, len);
            if (len > 40) Buf << "\"" << sv.substr(0, 40) << "...\"";
            else Buf << "\"" << sv << "\"";
            break;
         }
         case LUA_TTABLE: Buf << "{ ... }"; break;
         case LUA_TFUNCTION: Buf << "<function>"; break;
         default: Buf << "<" << lua_typename(Prv->Lua, type) << ">"; break;
      }

      if (not Compact) Buf <<" (" << lua_typename(Prv->Lua, type) << ")";
      Buf << "\n";
      lua_pop(Prv->Lua, 1);
      idx++;
   }

   lua_pop(Prv->Lua, 1); // Pop the function
   if (not Compact) Buf <<"\n";
}

//********************************************************************************************************************

static void emit_globals_info(prvTiri *Prv, std::ostringstream &Buf, bool Compact)
{
   if (not Compact) Buf << "-GLOBALS-\n";

   // In diagnosis mode, variables will be captured during compilation.

   if (not Prv->CapturedVariables.empty()) {
      for (const auto& var : Prv->CapturedVariables) {
         if (var.is_global) {
            Buf << var.line << ":" << var.column << ":" << (var.scope.empty() ? "_" : var.scope) << ":"
               << var.name << ":" << tiri_type_name(var.type) << ":" << "\n";
         }
      }
      // TODO: We need to add globals not in the capture list.
      return;
   }

   // Access the storage table where user-defined globals are stored.
   // The storage table is either:
   //   1. LUA_GLOBALSINDEX directly (no metatable protection)
   //   2. The __index table directly (JIT-compatible mode with metatable)
   //   3. An upvalue in the __index closure (legacy mode with metatable)

   int storage_table_idx;
   int items_to_pop;

   lua_pushvalue(Prv->Lua, LUA_GLOBALSINDEX); // Push global environment

   if (not lua_getmetatable(Prv->Lua, -1)) {
      // No metatable - globals are stored directly in LUA_GLOBALSINDEX
      storage_table_idx = lua_gettop(Prv->Lua);
      items_to_pop = 1; // global env only
   }
   else {
      lua_pushstring(Prv->Lua, "__index");
      lua_rawget(Prv->Lua, -2); // Get __index (could be table or function)

      if (lua_istable(Prv->Lua, -1)) {
         // JIT-compatible mode: __index IS the storage table directly
         storage_table_idx = lua_gettop(Prv->Lua);
         items_to_pop = 3; // storage table, metatable, global env
      }
      else if (lua_isfunction(Prv->Lua, -1)) {
         // Legacy mode: __index is a closure with storage table as upvalue
         auto upvalue_name = lua_getupvalue(Prv->Lua, -1, 1);
         if (not upvalue_name or not lua_istable(Prv->Lua, -1)) {
            lua_pop(Prv->Lua, 3);
            return;
         }
         storage_table_idx = lua_gettop(Prv->Lua);
         items_to_pop = 4; // storage table, __index closure, metatable, global env
      }
      else {
         lua_pop(Prv->Lua, 3);
         return;
      }
   }

   int count = 0;
   lua_pushnil(Prv->Lua);
   while (lua_next(Prv->Lua, storage_table_idx)) {
      CSTRING key = lua_tostring(Prv->Lua, -2);
      int type = lua_type(Prv->Lua, -1);

      std::string value;

      if (not Compact) {
         switch (type) {
            case LUA_TBOOLEAN: value = lua_toboolean(Prv->Lua, -1) ? "true" : "false"; break;
            case LUA_TNUMBER:  value = lua_tostring(Prv->Lua, -1); break;
            case LUA_TSTRING: {
               size_t len;
               CSTRING str = lua_tolstring(Prv->Lua, -1, &len);
               std::string_view sv(str, len);
               if (len > 40) value = "\"" + std::string(sv.substr(0, 40)) + "...\"";
               else value = "\"" + std::string(sv) + "\"";
               break;
            }
         }
      }

      Buf << ":::" << key << ":" << lua_typename(Prv->Lua, type) << ":" << value << "\n";
      count++;
      lua_pop(Prv->Lua, 1);
   }

   if (count IS 0) Buf << "(none)\n";
   lua_pop(Prv->Lua, items_to_pop);

   if (not Compact) Buf << "\n";
}

//********************************************************************************************************************

static void emit_memory_stats(prvTiri *Prv, std::ostringstream &Buf, bool Compact)
{
   if (not Compact) Buf << "-MEMORY-\n";

   const int kb = lua_gc(Prv->Lua, LUA_GCCOUNT, 0);
   const int bytes = lua_gc(Prv->Lua, LUA_GCCOUNTB, 0);
   const double mb = kb / 1024.0 + bytes / (1024.0 * 1024.0);

   Buf << (Compact
      ? std::format("LuaUsage: {} MB\n", mb)
      : std::format("LuaUsage: {} MB ({} KB + {} bytes)\n", mb, kb, bytes));

   if (not Compact) Buf << "\n";
}

//********************************************************************************************************************

static void emit_state_info(prvTiri *Prv, std::ostringstream &Buf, bool Compact)
{
   if (not Compact) Buf << "-STATE-\n";

   Buf << std::format("Stack top: {}\n", lua_gettop(Prv->Lua));

   if (auto hook_mask = lua_gethookmask(Prv->Lua)) {
      std::vector<std::string_view> flags;
      if (hook_mask & LUA_MASKCALL)  flags.emplace_back("CALL");
      if (hook_mask & LUA_MASKRET)   flags.emplace_back("RET");
      if (hook_mask & LUA_MASKLINE)  flags.emplace_back("LINE");
      if (hook_mask & LUA_MASKCOUNT) flags.emplace_back("COUNT");

      Buf << "Hook mask: ";
      for (size_t i = 0; i < flags.size(); ++i) {
         if (i > 0) Buf << "|";
         Buf << flags[i];
      }
      Buf << "\n";
   }
   else Buf << "Hook mask: none\n";

   if (not Compact) Buf << "\n";
}

/*********************************************************************************************************************

-METHOD-
DebugLog: Acquire a debug log from a compiled Script.

Use the DebugLog() method to acquire debug information from a Tiri script.  This method can be called from within
the script itself, or post-compilation to analyse the generated byte code.

The amount of debug information returned is defined by the Options parameter, which is a CSV list supporting the
following options:

<list type="unordered>
<li>stack: Returns the current stack trace. [L]</li>
<li>locals: Returns a list of all local variables and their values. [L]</li>
<li>upvalues: Returns a list of all upvalues. [L]</li>
<li>globals: Returns a list of all global variables and their values.</li>
<li>memory: Returns information about memory allocation and usage.</li>
<li>state: Returns the current state of the Tiri engine.</li>
<li>disasm: Returns disassembled bytecode.</li>
<li>dump: Returns a binary dump of the script.</li>
<li>funcinfo: Returns information about functions in the script.</li>
<li>compact: Returns a compact representation of the log.</li>
</list>

Options marked with [L] are only available when calling DebugLog() from inside the script.

The resulting log information is returned as a string, which needs to be deallocated once no longer required.

*********************************************************************************************************************/

static ERR TIRI_DebugLog(objScript *Self, struct sc::DebugLog *Args)
{
   pf::Log log;

   if (not Args) return log.warning(ERR::NullArgs);

   auto prv = (prvTiri *)Self->ChildPrivate;
   if (not prv->Lua) return log.warning(ERR::NotInitialised);

   log.branch("Options: %s", Args->Options ? Args->Options : "(none)");

   // Parse options (CSV list)

   struct DebugOptions {
      bool show_stack = false;
      bool show_locals = false;
      bool show_upvalues = false;
      bool show_globals = false;
      bool show_memory = false;
      bool show_state = false;
      bool show_disasm = false;
      bool show_dump = false;
      bool show_funcinfo = false;
      bool compact = false;
   } opts;

   constexpr auto has_option = [](std::string_view haystack, std::string_view needle) {
      return haystack.find(needle) != std::string_view::npos;
   };

   if (Args->Options) {
      const std::string_view options = Args->Options;

      if (has_option(options, "all")) {
         opts.show_stack = opts.show_locals = opts.show_upvalues = opts.show_globals =
         opts.show_memory = opts.show_state = opts.show_disasm = opts.show_funcinfo = true;
      }
      else {
         opts.show_stack    = has_option(options, "stack");
         opts.show_locals   = has_option(options, "locals");
         opts.show_upvalues = has_option(options, "upvalues");
         opts.show_globals  = has_option(options, "globals");
         opts.show_memory   = has_option(options, "memory");
         opts.show_state    = has_option(options, "state");
         opts.show_disasm   = has_option(options, "disasm") or has_option(options, "bytecode");
         opts.show_dump     = has_option(options, "dump");
         opts.show_funcinfo = has_option(options, "funcinfo");
      }
      opts.compact = has_option(options, "compact");
   }
   else opts.show_stack = true; // Default: just the stack trace

   // Build the log message

   std::ostringstream buf;

   if (opts.show_stack) {
      if (prv->Recurse) emit_stack_trace(prv, buf, opts.compact);
      else log.warning("Unable to provide stack trace to an external caller.");
   }

   if (opts.show_upvalues) {
      if (prv->Recurse) emit_upvalues_info(prv, buf, opts.compact);
      else log.warning("Unable to provide upvalues to an external caller.");
   }

   if (opts.show_locals) emit_locals_info(prv, buf, opts.compact);

   // Here we can process options that are meaningful post-execution.

   if (opts.show_funcinfo) {
      if (not opts.compact) buf <<"-FUNCTIONS-\n";

      if (prv->Recurse) {
         int level = 1;
         bool wrote = false;
         lua_Debug ar;

         while (lua_getstack(prv->Lua, level, &ar)) {
            if (not lua_getinfo(prv->Lua, "nSl", &ar)) {
               level++;
               continue;
            }

            CSTRING func_name = ar.name ? ar.name : "<anonymous>";
            bool is_lua_func = false;
            int param_count = 0;
            bool is_vararg = false;
            uint32_t frame_slots = 0;
            uint32_t bytecodes = 0;
            uint32_t numeric_consts = 0;
            uint32_t object_consts = 0;
            int upvalues = 0;

            if (lua_getinfo(prv->Lua, "f", &ar)) {
               GCfunc *fn = funcV(prv->Lua->top - 1);
               upvalues = fn->c.nupvalues;
               if (isluafunc(fn)) {
                  is_lua_func = true;
                  GCproto *pt = funcproto(fn);
                  param_count = pt->numparams;
                  is_vararg = (pt->flags & PROTO_VARARG) != 0;
                  frame_slots = pt->framesize;
                  bytecodes = pt->sizebc;
                  numeric_consts = pt->sizekn;
                  object_consts = pt->sizekgc;
               }
               else is_vararg = true;
               lua_pop(prv->Lua, 1);
            }

            if (opts.compact) {
               buf << "[" << level << "] " << func_name;
               if (is_lua_func) {
                  buf << " (" << ar.short_src << ":" << ar.linedefined << "-" << ar.lastlinedefined << ")";
                  buf << " params=" << param_count;
                  buf << " vararg=" << (is_vararg ? "true" : "false");
                  buf << " slots=" << frame_slots;
                  buf << " bytecode=" << bytecodes;
                  buf << " consts=" << numeric_consts << "+" << object_consts;
               }
               else buf << " <C function>";
               buf << "\n";
            }
            else {
               buf << "Function [" << level << "]: " << func_name;
               if (is_lua_func) {
                  buf << " (" << ar.short_src << ":" << ar.linedefined << "-" << ar.lastlinedefined << ")\n";
                  buf << "   Parameters: " << param_count << "\n";
                  buf << "   Vararg: " << (is_vararg ? "true" : "false") << "\n";
                  buf << "   Stack slots: " << frame_slots << "\n";
                  buf << "   Bytecodes: " << bytecodes << "\n";
                  buf << "   Constants: " << numeric_consts << " numeric, " << object_consts << " objects\n";
                  buf << "   Upvalues: " << upvalues << "\n";
               }
               else {
                  buf << " (<C function>)\n";
                  buf << "   Upvalues: " << upvalues << "\n";
               }
            }

            wrote = true;
            level++;
         }

         if (not wrote) buf << "(no frames)";
         if (not opts.compact) buf <<"\n";
      }
   }

   if (opts.show_disasm) {
      auto buf_logger = [](std::string_view Msg, void *Meta) {
         auto *pbuf = (std::ostringstream *)Meta;
         *pbuf << Msg << "\n";
      };

      if (not opts.compact) buf << "-BYTECODE-\n";

      lua_Debug ar;
      if (lua_getstack(prv->Lua, 1, &ar)) {
         if (lua_getinfo(prv->Lua, "f", &ar)) {
            GCfunc *fn = funcV(prv->Lua->top - 1);
            if (isluafunc(fn)) trace_proto_bytecode(prv->Lua, funcproto(fn), buf_logger, &buf, not opts.compact);
            else buf << "(current frame is a C function; bytecode unavailable)\n";

            lua_pop(prv->Lua, 1);
         }
         else buf << "(unable to inspect current frame)\n";
      }
      else {
         // No active frame - try to disassemble the main chunk if available
         if (prv->MainChunkRef) {
            lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, prv->MainChunkRef);
            if (lua_isfunction(prv->Lua, -1)) {
               GCfunc *fn = funcV(prv->Lua->top - 1);
               if (isluafunc(fn)) {
                  trace_proto_bytecode(prv->Lua, funcproto(fn), buf_logger, &buf, not opts.compact);
               }
               else buf << "(main chunk is a C function; bytecode unavailable)\n";
            }
            else buf << "(main chunk reference is not a function)\n";
            lua_pop(prv->Lua, 1);
         }
         else buf << "(no main chunk reference stored; bytecode disassembly requires calling DebugLog from within a function)\n";
      }
   }

   if (opts.show_dump) {
      lua_Debug ar;
      if (lua_getstack(prv->Lua, 1, &ar)) {
         lua_getinfo(prv->Lua, "Sln", &ar);

         if (not opts.compact) buf <<"-BINARY-\n";

         if (lua_getinfo(prv->Lua, "f", &ar)) {
            GCfunc *fn = funcV(prv->Lua->top - 1);
            if (isluafunc(fn)) {
               std::vector<uint8_t> binary;

               if (lua_dump(prv->Lua, append_dump_chunk, &binary) IS 0) {
                  if (not opts.compact) {
                     CSTRING func_name = ar.name ? ar.name : "<anonymous>";
                     buf << "Function: " << func_name << " (" << ar.short_src << ":" << ar.linedefined
                         << "-" << ar.lastlinedefined << ")\n";
                     buf << "Bytes: " << binary.size() << "\n";
                  }

                  append_hex_dump(binary, buf, opts.compact);
               }
               else buf << "(failed to serialise bytecode)\n";
            }
            else buf << "(current frame is a C function; bytecode unavailable)\n";

            lua_pop(prv->Lua, 1);
         }
         else buf << "(unable to inspect current frame)\n";

         if (not opts.compact) buf <<"\n";
      }
      else {
         // No active frame - try to dump the main chunk if available
         if (not opts.compact) buf <<"-BYTECODE-\n";

         if (prv->MainChunkRef) {
            lua_rawgeti(prv->Lua, LUA_REGISTRYINDEX, prv->MainChunkRef);
            if (lua_isfunction(prv->Lua, -1)) {
               GCfunc *fn = funcV(prv->Lua->top - 1);
               if (isluafunc(fn)) {
                  std::vector<uint8_t> binary;

                  if (lua_dump(prv->Lua, append_dump_chunk, &binary) IS 0) {
                     if (not opts.compact) {
                        buf << "Main chunk\n";
                        buf << "Bytes: " << binary.size() << "\n";
                     }

                     append_hex_dump(binary, buf, opts.compact);
                  }
                  else buf << "(failed to serialise bytecode)\n";
               }
               else buf << "(main chunk is a C function; bytecode unavailable)\n";
            }
            else buf << "(main chunk reference is not a function)\n";
            lua_pop(prv->Lua, 1);
         }
         else buf << "(no main chunk reference stored; bytecode dump requires calling DebugLog from within a function)\n";

         if (not opts.compact) buf <<"\n";
      }
   }

   if (opts.show_globals) emit_globals_info(prv, buf, opts.compact);
   if (opts.show_memory) emit_memory_stats(prv, buf, opts.compact);
   if (opts.show_state) emit_state_info(prv, buf, opts.compact);

   const std::string result = buf.str();
   if ((Args->Result = pf::strclone(result.c_str())) IS nullptr) return ERR::AllocMemory;

   return ERR::Okay;
}

//********************************************************************************************************************

static ERR TIRI_DerefProcedure(objScript *Self, struct sc::DerefProcedure *Args)
{
   pf::Log log;

   if (not Args) return ERR::NullArgs;

   if ((Args->Procedure) and (Args->Procedure->isScript())) {
      if (Args->Procedure->Context IS Self) { // Verification of ownership
         auto prv = (prvTiri *)Self->ChildPrivate;
         if (not prv) {
            if (Self->terminating()) return ERR::Okay;
            else return log.warning(ERR::ObjectCorrupt);
         }

         log.trace("Dereferencing procedure #%" PF64, (long long)Args->Procedure->ProcedureID);

         if (Args->Procedure->ProcedureID) {
            luaL_unref(prv->Lua, LUA_REGISTRYINDEX, Args->Procedure->ProcedureID);
            Args->Procedure->ProcedureID = 0;
         }
         return ERR::Okay;
      }
      else return log.warning(ERR::Args);
   }
   else return log.warning(ERR::Args);
}

//********************************************************************************************************************

static ERR TIRI_GetProcedureID(objScript *Self, struct sc::GetProcedureID *Args)
{
   pf::Log log;

   if ((not Args) or (not Args->Procedure) or (not Args->Procedure[0])) return log.warning(ERR::NullArgs);

   auto prv = (prvTiri *)Self->ChildPrivate;
   if (not prv) return log.warning(ERR::ObjectCorrupt);

   if ((not prv->Lua) or (not Self->ActivationCount)) {
      log.warning("Cannot resolve function '%s'.  Script requires activation.", Args->Procedure);
      return ERR::NotFound;
   }

   lua_getglobal(prv->Lua, Args->Procedure);
   auto id = luaL_ref(prv->Lua, LUA_REGISTRYINDEX);
   if ((id != LUA_REFNIL) and (id != LUA_NOREF)) {
      Args->ProcedureID = id;
      return ERR::Okay;
   }
   else {
      log.warning("Failed to resolve function name '%s' to an ID.", Args->Procedure);
      return ERR::NotFound;
   }
}
