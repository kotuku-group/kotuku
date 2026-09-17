// Copyright © 2025-2026 Paul Manias
// IR emitter implementation: import and namespace statement emission
// This file is #included from ir_emitter.cpp

static bool remap_import_module_references(GCproto *, const std::vector<uint32_t> &);

//********************************************************************************************************************

static int append_import_module_dump(lua_State *, const void *Data, size_t Size, void *Context)
{
   ((std::string *)Context)->append((const char *)Data, Size);
   return 0;
}

//********************************************************************************************************************

static bool install_import_module_table(lua_State *L, GCproto *Root,
   const std::vector<tiri::import_cache::RootModuleRecord> &Records,
   const std::vector<GCproto *> &Initialisers)
{
   if (Records.size() != Initialisers.size()) return false;
   if (Records.empty()) return true;
   size_t dependency_count = 0;
   for (size_t i = 0; i < Records.size(); ++i) {
      if (not Initialisers[i] or Initialisers[i]->sizeuv != 0) return false;
      dependency_count += Records[i].Dependencies.size();
      if (dependency_count > tiri::import_cache::MAX_ROOT_MODULES) return false;
      for (uint32_t dependency : Records[i].Dependencies) if (dependency >= i) return false;
   }

   const size_t byte_size = sizeof(ImportModuleTable) + Records.size() * sizeof(ImportModuleTableEntry) +
      dependency_count * sizeof(uint32_t);
   if (byte_size > std::numeric_limits<uint32_t>::max()) return false;
   auto table = (ImportModuleTable *)lj_mem_new(L, MSize(byte_size));
   table->version = IMPORT_MODULE_TABLE_VERSION;
   memset(table->reserved, 0, sizeof(table->reserved));
   table->entry_count = uint32_t(Records.size());
   table->dependency_count = uint32_t(dependency_count);
   table->byte_size = uint32_t(byte_size);

   auto entries = import_module_table_entries(table);
   auto dependencies = import_module_table_dependencies(table);
   uint32_t next_dependency = 0;
   for (size_t i = 0; i < Records.size(); ++i) {
      const auto &record = Records[i];
      setgcref(entries[i].compiled_identity, obj2gco(&G(L)->strempty));
      setgcref(entries[i].initialiser, obj2gco(Initialisers[i]));
      entries[i].first_dependency = next_dependency;
      entries[i].dependency_count = uint32_t(record.Dependencies.size());
      entries[i].source_index = record.SourceIndex;
      memset(entries[i].reserved, 0, sizeof(entries[i].reserved));
      for (uint32_t dependency : record.Dependencies) dependencies[next_dependency++] = dependency;
   }

   setmref(Root->import_module_table, table);
   for (size_t i = 0; i < Records.size(); ++i) {
      attach_compilation_source_root(Initialisers[i], Root);
      const std::string &identity = Records[i].CompiledIdentity;
      setgcref(entries[i].compiled_identity, obj2gco(lj_str_new(L, identity.data(), identity.size())));
   }
   return true;
}

//********************************************************************************************************************

static bool prepare_import_module_dump(LexState &State, GCproto *Prototype, std::string &Output)
{
   lua_State *L = State.L;
   attach_compilation_sources(L, Prototype, State.compilation_sources);

   std::vector<uint8_t> structure_manifest;
   std::string detail;
   if (build_declared_struct_manifest(L, State.compilation_struct_roots, State.compilation_structs,
       State.dynamic_struct_reference, structure_manifest, &detail) != ERR::Okay) return false;
   auto structures = (uint8_t *)lj_mem_new(L, MSize(structure_manifest.size()));
   memcpy(structures, structure_manifest.data(), structure_manifest.size());
   setmref(Prototype->struct_manifest, structures);
   Prototype->struct_manifest_size = uint32_t(structure_manifest.size());

   std::vector<tiri::import_cache::RootModuleRecord> records;
   std::vector<GCproto *> initialisers;
   std::vector<uint32_t> remap(State.import_module_records.size(), UINT32_MAX);
   std::function<bool(uint32_t)> append_record = [&](uint32_t Index) {
      if (Index >= State.import_module_records.size()) return false;
      if (remap[Index] != UINT32_MAX) return true;
      const auto &source = State.import_module_records[Index];
      std::vector<uint32_t> dependencies;
      for (uint32_t dependency : source.dependencies) {
         if (not append_record(dependency)) return false;
         dependencies.push_back(remap[dependency]);
      }
      remap[Index] = uint32_t(records.size());
      uint8_t source_index = FILESOURCE_OVERFLOW_INDEX;
      for (size_t i = 0; i < State.compilation_sources.size(); ++i) {
         if (State.compilation_sources[i].runtime_index IS source.source_index) {
            source_index = uint8_t(i);
            break;
         }
      }
      records.push_back({ source.lookup_identity, source.compiled_identity, source.interface_bytes,
         std::move(dependencies), source_index });
      initialisers.push_back(source.initialiser);
      return true;
   };
   if (not State.import_module_stack.empty()) {
      for (uint32_t dependency : State.import_module_stack.back().dependencies) {
         if (not append_record(dependency)) return false;
      }
   }
   tiri::import_cache::RootModuleGraphAssembly assembly;
   if (tiri::import_cache::assemble_root_module_graph(records, assembly) !=
       tiri::cache::FormatError::OKAY or assembly.input_to_canonical().size() != records.size()) return false;
   std::vector<GCproto *> canonical_initialisers(assembly.records().size(), nullptr);
   for (size_t i = 0; i < records.size(); ++i) {
      const uint32_t index = assembly.input_to_canonical()[i];
      if (index >= canonical_initialisers.size()) return false;
      if (not canonical_initialisers[index]) canonical_initialisers[index] = initialisers[i];
   }
   std::vector<uint32_t> wire_mapping(State.import_module_records.size(), UINT32_MAX);
   std::vector<uint32_t> restore_mapping(assembly.records().size(), UINT32_MAX);
   for (size_t i = 0; i < remap.size(); ++i) {
      if (remap[i] IS UINT32_MAX) continue;
      if (remap[i] >= assembly.input_to_canonical().size()) return false;
      wire_mapping[i] = assembly.input_to_canonical()[remap[i]];
      if (wire_mapping[i] >= restore_mapping.size()) return false;
      if (restore_mapping[wire_mapping[i]] IS UINT32_MAX) restore_mapping[wire_mapping[i]] = uint32_t(i);
   }
   for (uint32_t restored : restore_mapping) if (restored IS UINT32_MAX) return false;
   if (not remap_import_module_references(Prototype, wire_mapping)) return false;
   for (GCproto *initialiser : canonical_initialisers) {
      if (not initialiser or not remap_import_module_references(initialiser, wire_mapping)) return false;
   }
   std::string module_bundle;
   if (tiri::import_cache::encode_root_module_bundle(assembly, module_bundle) !=
       tiri::cache::FormatError::OKAY) return false;
   auto modules = (uint8_t *)lj_mem_new(L, MSize(module_bundle.size()));
   memcpy(modules, module_bundle.data(), module_bundle.size());
   setmref(Prototype->import_module_bundle, modules);
   Prototype->import_module_bundle_size = uint32_t(module_bundle.size());
   if (not install_import_module_table(L, Prototype, assembly.records(), canonical_initialisers)) return false;
   const bool written = lj_bcwrite(L, Prototype, append_import_module_dump, &Output, 0) IS 0;
   auto table = Prototype->import_module_table.get<ImportModuleTable>();
   if (table) {
      const uint32_t table_size = table->byte_size;
      setmref(Prototype->import_module_table, nullptr);
      lj_mem_free(G(L), table, table_size);
   }
   bool restored = remap_import_module_references(Prototype, restore_mapping);
   for (GCproto *initialiser : canonical_initialisers) {
      restored = remap_import_module_references(initialiser, restore_mapping) and restored;
   }
   if (not restored) return false;
   return written;
}

//********************************************************************************************************************
// Emit bytecode for one import entry.
//
// The import entry inlines the content of the referenced file at compile time.
// The inlined_body block is emitted as if its statements were written directly at the import location.
// This creates a new scope for the imported content to provide some isolation.
//
// When namespace_name is set (from 'as alias' or module's default namespace), emits:
//   local <namespace_name> <const> = _LIB['<default_namespace>']

void IrEmitter::emit_namespace_registry_load(std::string_view Name, BCReg Destination)
{
   FuncState *fs = &this->func_state;
   lua_State *L = this->lex_state.L;
   auto str_const = [fs](GCstr *String) -> BCREG {
      return const_gc(fs, obj2gco(String), LJ_TSTR);
   };

   bcemit_AD(fs, BC_GGET, Destination.raw(), str_const(lj_str_newlit(L, "_LIB")));
   GCstr *namespace_name = lj_str_new(L, Name.data(), Name.size());
   bcemit_tgets(fs, Destination.raw(), Destination.raw(), str_const(namespace_name));
}

//********************************************************************************************************************

void IrEmitter::emit_namespace_missing_guard(std::string_view Name, BCReg Value)
{
   FuncState *fs = &this->func_state;
   lua_State *L = this->lex_state.L;
   ExpDesc nil_value(ExpKind::Nil);
   bcemit_INS(fs, BCINS_AD(BC_ISEQP, Value.raw(), const_pri(&nil_value)));
   ControlFlowEdge missing = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));
   ControlFlowEdge present = this->control_flow.make_unconditional(BCPos(bcemit_jmp(fs)));

   missing.patch_here();
   BCReg saved_freereg = fs->free_reg();
   BCReg message_reg = saved_freereg;
   bcreg_reserve(fs, BCReg(2));
   auto str_const = [fs](GCstr *String) -> BCREG {
      return const_gc(fs, obj2gco(String), LJ_TSTR);
   };
   std::string message = std::format("Cannot join namespace '{}': registry entry does not exist", Name);
   bcemit_AD(fs, BC_KSTR, message_reg.raw(), str_const(lj_str_new(L, message.c_str(), message.size())));
   BCReg raise_message_reg = BCReg(message_reg.raw() + 1);
   bcemit_AD(fs, BC_MOV, raise_message_reg, message_reg);
   bcemit_AD(fs, BC_RAISE, message_reg, raise_message_reg);
   fs->freereg = saved_freereg.raw();
   present.patch_here();
}

//********************************************************************************************************************

void IrEmitter::publish_namespace_local(const Identifier &Name, BCReg Slot)
{
   FuncState *fs = &this->func_state;
   this->lex_state.var_new(BCReg(0), Name.symbol, Name.span.line, Name.span.column);
   this->lex_state.var_add(BCReg(1));

   VarInfo *info = &fs->var_get(fs->varmap.size() - 1);
   info->info |= VarInfoFlag::Const;
   info->binding_id = Name.binding_id;
   info->static_value = Name.static_value;
   if (Name.binding_id) {
      const auto &binding = this->ctx.descriptors().binding(Name.binding_id);
      info->static_callable = binding.callable;
      if (binding.callable) info->static_results = this->ctx.descriptors().callable(binding.callable).results;
      this->apply_analysed_local_type(Slot, Name.binding_id);
      this->assert_analysed_local_type(Slot, Name.binding_id);
   }

   this->update_local_binding(Name.symbol, Slot);
   fs->reset_freereg();
}

//********************************************************************************************************************

static bool remap_import_module_references(GCproto *Prototype, const std::vector<uint32_t> &Mapping)
{
   BCIns *bytecode = proto_bc(Prototype);
   for (MSize i = 1; i < Prototype->sizebc; ++i) {
      if (bc_op(bytecode[i]) != BC_BFUNC or
          bc_d(bytecode[i]) != builtin_callable_index(BuiltinCallableID::ImportModuleActivate)) continue;
      const BCREG base = bc_a(bytecode[i]);
      const BCREG reference_slot = base + 2 + LJ_FR2;
      bool remapped = false;
      for (MSize next = i + 1; next < Prototype->sizebc; ++next) {
         if (bc_op(bytecode[next]) IS BC_KSHORT and bc_a(bytecode[next]) IS reference_slot) {
            const int32_t old_index = int32_t(int16_t(bc_d(bytecode[next])));
            if (old_index < 0 or uint32_t(old_index) >= Mapping.size() or
                Mapping[uint32_t(old_index)] > uint32_t(INT16_MAX)) return false;
            setbc_d(&bytecode[next], uint16_t(Mapping[uint32_t(old_index)]));
            remapped = true;
         }
         if (bc_op(bytecode[next]) IS BC_CALL and bc_a(bytecode[next]) IS base) break;
      }
      if (not remapped) return false;
   }

   if (Prototype->flags & PROTO_CHILD) {
      GCRef *constant = mref<GCRef>(Prototype->k) - 1;
      for (MSize i = 0; i < Prototype->sizekgc; ++i, --constant) {
         GCobj *object = gcref(*constant);
         if (object->gch.gct IS ~LJ_TPROTO and
             not remap_import_module_references(gco_to_proto(object), Mapping)) return false;
      }
   }
   return true;
}

//********************************************************************************************************************

static bool canonicalise_import_module_dependencies(
   const std::vector<ImportModuleCompilationRecord> &Records, std::vector<uint32_t> &Dependencies,
   ImportedModuleCompilationCounters &Counters)
{
   std::vector<uint32_t> roots;
   std::vector<int32_t> root_positions(Records.size(), -1);
   for (uint32_t candidate : Dependencies) {
      if (candidate >= Records.size()) return false;
      if (root_positions[candidate] < 0) {
         root_positions[candidate] = int32_t(roots.size());
         roots.push_back(candidate);
      }
   }

   std::vector<bool> reached(roots.size(), false);
   std::vector<uint32_t> visited(Records.size(), 0);
   std::vector<uint32_t> stack;
   uint32_t generation = 0;
   for (size_t root_position = 0; root_position < roots.size(); ++root_position) {
      Counters.root_normalisation_traversals++;
      if (++generation IS 0) {
         std::ranges::fill(visited, 0);
         generation = 1;
      }
      stack.clear();
      stack.push_back(roots[root_position]);
      visited[roots[root_position]] = generation;
      while (not stack.empty()) {
         const uint32_t current = stack.back();
         stack.pop_back();
         for (uint32_t dependency : Records[current].dependencies) {
            Counters.root_normalisation_edges++;
            if (dependency >= Records.size()) return false;
            const int32_t reached_position = root_positions[dependency];
            if (reached_position >= 0 and size_t(reached_position) != root_position) {
               reached[size_t(reached_position)] = true;
            }
            if (visited[dependency] != generation) {
               visited[dependency] = generation;
               stack.push_back(dependency);
            }
         }
      }
   }

   std::vector<uint32_t> canonical;
   for (size_t i = 0; i < roots.size(); ++i) if (not reached[i]) canonical.push_back(roots[i]);
   std::ranges::sort(canonical, [&](uint32_t Left, uint32_t Right) {
      return Records[Left].compiled_identity < Records[Right].compiled_identity;
   });
   Dependencies = std::move(canonical);
   return true;
}

//********************************************************************************************************************

ParserResult<IrEmitUnit> IrEmitter::emit_import_entry(const ImportEntryPayload &Entry)
{
   FuncState *fs = &this->func_state;
   ImportedModuleUnit *unit = Entry.module_unit.get();
   if (Entry.module_initialiser and not unit) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(
         ParserErrorCode::InternalInvariant, "Imported module edge has no compilation unit"));
   }
   if (unit and not Entry.module_already_imported and unit->module_identity.empty()) {
      return ParserResult<IrEmitUnit>::failure(this->make_error(
         ParserErrorCode::InternalInvariant, "Imported module has no runtime identity"));
   }

   // Local imports retain their inline block.  Non-local module initialisers are detached from ordinary child
   // traversal and owned exactly once by the root executable directory.

   if ((unit and unit->body) or Entry.inlined_body) {
      // Temporarily switch to the imported file's FileSource index
      // so that prototypes created for functions in the import get the correct file_source_idx

      uint8_t saved_file_index = this->lex_state.current_file_index;
      BCLine saved_lastline = this->lex_state.lastline;
      this->lex_state.current_file_index = unit ? unit->file_source_idx : Entry.file_source_idx;

      ParserResult<IrEmitUnit> result = ParserResult<IrEmitUnit>::success(IrEmitUnit{});
      if (Entry.module_initialiser and not Entry.module_already_imported) {
         auto activate_module = [&](uint32_t ModuleIndex) {
            if (not this->lex_state.import_module_stack.empty()) {
               auto &dependencies = this->lex_state.import_module_stack.back().dependencies;
               if (std::ranges::find(dependencies, ModuleIndex) IS dependencies.end()) {
                  dependencies.push_back(ModuleIndex);
               }
            }

            BCReg base = fs->free_reg();
            constexpr BCREG argument_count = 2;
            bcreg_reserve(fs, BCReg(1 + LJ_FR2 + argument_count));
            bcemit_builtin_callable(fs, BuiltinCallableID::ImportModuleActivate, base.raw());

            lua_State *L = this->lex_state.L;
            GCstr *identity = lj_str_new(L, unit->module_identity.data(), unit->module_identity.size());
            bcemit_AD(fs, BC_KSTR, base.raw() + 1 + LJ_FR2, const_gc(fs, obj2gco(identity), LJ_TSTR));

            bcemit_AD(fs, BC_KSHORT, base.raw() + 2 + LJ_FR2, int32_t(ModuleIndex));
            bcemit_ABC(fs, BC_CALL, base.raw(), 1, argument_count + 1);
            fs->freereg = base.raw();
         };

         if (unit->compilation_record != UINT32_MAX) activate_module(unit->compilation_record);
         else {
            GCproto *warm_root = nullptr;
            const ImportModuleTable *warm_table = nullptr;
            if (unit->module_cache_hit) {
               lua_State *L = this->lex_state.L;
               if (lua_load(L, unit->module_payload, "=import-cache-link") != 0 or
                   not lua_isfunction(L, -1) or lua_iscfunction(L, -1)) {
                  std::string diagnostic = "Warm imported-module executable graph could not be decoded";
                  if (const char *message = lua_tostring(L, -1)) diagnostic += std::string(": ") + message;
                  lua_pop(L, 1);
                  return ParserResult<IrEmitUnit>::failure(this->make_error(
                     ParserErrorCode::InternalInvariant, diagnostic));
               }
               warm_root = funcproto(funcV(L->top - 1));
               warm_table = proto_import_module_table(warm_root);
               const uint32_t warm_count = warm_table ? warm_table->entry_count : 0;
               if (warm_count != unit->embedded_modules.size()) {
                  lua_pop(L, 1);
                  return ParserResult<IrEmitUnit>::failure(this->make_error(
                     ParserErrorCode::InternalInvariant, "Warm imported-module directory does not match its metadata"));
               }
               this->lex_state.import_module_anchors.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
               this->lex_state.linked_import_module_roots.push_back(warm_root);
            }

            std::vector<uint32_t> embedded_mapping(unit->embedded_modules.size(), UINT32_MAX);
            std::vector<bool> embedded_dependency(unit->embedded_modules.size(), false);
            std::vector<bool> embedded_inserted(unit->embedded_modules.size(), false);
            for (size_t i = 0; i < unit->embedded_modules.size(); ++i) {
               const auto &record = unit->embedded_modules[i];
               for (uint32_t dependency : record.Dependencies) {
                  if (dependency >= i or embedded_mapping[dependency] IS UINT32_MAX) {
                     return ParserResult<IrEmitUnit>::failure(this->make_error(
                        ParserErrorCode::InternalInvariant, "Warm imported-module dependency order is invalid"));
                  }
                  embedded_dependency[dependency] = true;
               }

               std::vector<uint32_t> dependencies;
               for (uint32_t dependency : record.Dependencies) {
                  dependencies.push_back(embedded_mapping[dependency]);
               }

               auto found = std::ranges::find(this->lex_state.import_module_records, record.CompiledIdentity,
                  &ImportModuleCompilationRecord::compiled_identity);
               if (found != this->lex_state.import_module_records.end()) {
                  auto dependency_identities = [&](const std::vector<uint32_t> &Dependencies) {
                     std::vector<std::string_view> identities;
                     identities.reserve(Dependencies.size());
                     for (uint32_t dependency : Dependencies) {
                        if (dependency >= this->lex_state.import_module_records.size()) {
                           return std::optional<std::vector<std::string_view>>{};
                        }
                        identities.push_back(this->lex_state.import_module_records[dependency].compiled_identity);
                     }
                     std::ranges::sort(identities);
                     return std::optional<std::vector<std::string_view>>(std::move(identities));
                  };
                  const auto existing_dependencies = dependency_identities(found->dependencies);
                  const auto incoming_dependencies = dependency_identities(dependencies);
                  const bool dependencies_match = existing_dependencies and incoming_dependencies and
                     *existing_dependencies IS *incoming_dependencies;
                  if (found->lookup_identity != record.LookupIdentity or
                      found->interface_bytes != record.InterfaceBytes or not dependencies_match) {
                     auto describe_dependencies = [](const auto &Dependencies) {
                        std::string result;
                        for (std::string_view identity : Dependencies) {
                           if (not result.empty()) result += ", ";
                           result += identity;
                        }
                        return result;
                     };
                     tiri::import_cache::Interface conflicting_interface;
                     std::string source_path = "<unknown>";
                     if (tiri::import_cache::decode_interface(record.InterfaceBytes, conflicting_interface) IS
                         tiri::cache::FormatError::OKAY and not conflicting_interface.Sources.empty()) {
                        source_path = conflicting_interface.Sources.front().ResolvedPath;
                     }
                     return ParserResult<IrEmitUnit>::failure(this->make_error(
                        ParserErrorCode::InternalInvariant,
                        std::format("Conflicting warm imported-module graph record '{}' for '{}' (lookup {}, "
                           "interface {}, dependencies {}; existing [{}], incoming [{}])", record.CompiledIdentity,
                           source_path,
                           found->lookup_identity IS record.LookupIdentity ? "matches" : "differs",
                           found->interface_bytes IS record.InterfaceBytes ? "matches" : "differs",
                           dependencies_match ? "match" : "differ",
                           existing_dependencies ? describe_dependencies(*existing_dependencies) : "<invalid>",
                           incoming_dependencies ? describe_dependencies(*incoming_dependencies) : "<invalid>")));
                  }
                  embedded_mapping[i] = uint32_t(found - this->lex_state.import_module_records.begin());
               }
               else {
                  if (not warm_table or not gcref(import_module_table_entries(warm_table)[i].initialiser)) {
                     return ParserResult<IrEmitUnit>::failure(this->make_error(
                        ParserErrorCode::InternalInvariant, "Warm imported-module executable body is missing"));
                  }
                  embedded_mapping[i] = uint32_t(this->lex_state.import_module_records.size());
                  this->lex_state.import_module_records.push_back({ record.LookupIdentity, record.CompiledIdentity,
                     record.InterfaceBytes, std::move(dependencies),
                     gco_to_proto(gcref(import_module_table_entries(warm_table)[i].initialiser)), record.SourceIndex });
                  embedded_inserted[i] = true;
               }
            }

            for (size_t i = 0; i < embedded_mapping.size(); ++i) {
               if (embedded_inserted[i] and not remap_import_module_references(
                   this->lex_state.import_module_records[embedded_mapping[i]].initialiser, embedded_mapping)) {
                  return ParserResult<IrEmitUnit>::failure(this->make_error(
                     ParserErrorCode::InternalInvariant, "Warm imported-module references could not be relocated"));
               }
            }
            if (warm_root and not remap_import_module_references(warm_root, embedded_mapping)) {
               return ParserResult<IrEmitUnit>::failure(this->make_error(
                  ParserErrorCode::InternalInvariant, "Warm imported-module root references could not be relocated"));
            }

            std::vector<uint32_t> embedded_roots;
            for (size_t i = 0; i < embedded_mapping.size(); ++i) {
               if (not embedded_dependency[i]) embedded_roots.push_back(embedded_mapping[i]);
            }

            auto existing = std::ranges::find_if(this->lex_state.import_module_records, [&](const auto &Record) {
               return Record.compiled_identity IS unit->module_identity;
            });

            const bool new_module = existing IS this->lex_state.import_module_records.end();
            if (new_module) {
               this->lex_state.import_module_stack.push_back({ unit->module_cache_identity.LookupIdentity,
                  unit->module_identity, std::move(embedded_roots) });
            }

            FunctionExprPayload initialiser;
            GCproto *module_prototype = nullptr;
            ParserResult<ExpDesc> function = ParserResult<ExpDesc>::success(ExpDesc{});

            if (unit->module_cache_hit) module_prototype = warm_root;
            else if (new_module or not existing->initialiser) {
               this->lex_state.imported_module_counters.initialiser_emissions++;
               function = this->emit_function_body(
                  initialiser, *unit->body, nullptr, true, &module_prototype, &unit->module_dependencies, true);
               if (function.ok() and module_prototype) {
                  lua_State *L = this->lex_state.L;
                  setfuncV(L, L->top, lj_func_newL_empty(L, module_prototype, tabref(L->env)));
                  incr_top(L);
                  this->lex_state.import_module_anchors.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
               }
            }

            if (not function.ok()) {
               if (new_module) this->lex_state.import_module_stack.pop_back();
               result = ParserResult<IrEmitUnit>::failure(function.error_ref());
            }
            else {
               if (new_module and not unit->module_cache_hit and unit->installed_interface and module_prototype) {
                  std::string payload;
                  if (prepare_import_module_dump(this->lex_state, module_prototype, payload)) {
                     tiri::import_cache::CompilationRequest request;
                     request.ExpectedIdentity = unit->module_cache_identity;
                     tiri::import_cache::CompiledModule published;
                     (void)tiri::import_cache::publish_module(request, unit->module_cache_identity,
                        unit->installed_interface->portable_interface(), payload,
                        this->lex_state.import_cache_counters, published);
                  }
               }

               uint32_t module_index = 0;
               if (new_module) {
                  std::string interface_bytes;
                  tiri::import_cache::Interface empty_interface;
                  const auto &portable = unit->installed_interface ?
                     unit->installed_interface->portable_interface() : empty_interface;
                  auto interface_error = tiri::import_cache::encode_interface(portable, interface_bytes);
                  if (interface_error != tiri::cache::FormatError::OKAY) {
                     this->lex_state.import_module_stack.pop_back();
                     return ParserResult<IrEmitUnit>::failure(this->make_error(
                        ParserErrorCode::InternalInvariant, std::format(
                           "Imported module '{}' has an invalid portable interface: {}", Entry.lib_path,
                           tiri::cache::format_error_name(interface_error))));
                  }

                  ImportModuleCompilationFrame frame = std::move(this->lex_state.import_module_stack.back());
                  this->lex_state.import_module_stack.pop_back();
                  if (not canonicalise_import_module_dependencies(
                      this->lex_state.import_module_records, frame.dependencies,
                      this->lex_state.imported_module_counters)) {
                     return ParserResult<IrEmitUnit>::failure(this->make_error(
                        ParserErrorCode::InternalInvariant, "Imported module dependencies contain an invalid index"));
                  }
                  module_index = uint32_t(this->lex_state.import_module_records.size());
                  this->lex_state.import_module_records.push_back({
                     std::move(frame.lookup_identity), std::move(frame.compiled_identity),
                     std::move(interface_bytes), std::move(frame.dependencies), module_prototype, unit->file_source_idx
                  });
               }
               else module_index = uint32_t(existing - this->lex_state.import_module_records.begin());

               unit->compilation_record = module_index;
               unit->state = ImportedModuleState::Emitted;
               activate_module(module_index);
            }
         }
      }
      else if (not Entry.module_initialiser) {
         result = this->emit_block(*Entry.inlined_body, FuncScopeFlag::None);
      }

      // Restore the parent source location before emitting the namespace binding generated for the import.
      this->lex_state.current_file_index = saved_file_index;
      this->lex_state.lastline = saved_lastline;

      if (not result.ok()) return result;
   }

   // If namespace_name is set, emit: local <name> <const> = _LIB['<default_namespace>']
   if (Entry.namespace_name and not Entry.reuses_namespace_binding) {
      const Identifier &ns_id = *Entry.namespace_name;
      const std::string &default_ns = Entry.default_namespace;

      if (default_ns.empty()) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant,
            "import namespace_name is set but default_namespace is empty"));
      }

      // Allocate a register for the namespace local variable
      BCReg dest = BCReg(fs->freereg);
      bcreg_reserve(fs, BCReg(1));
      this->emit_namespace_registry_load(default_ns, dest);
      this->publish_namespace_local(ns_id, dest);
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit a namespace declaration.  Creation publishes its const local before evaluating the literal so closures can
// capture their owning namespace, then stores the completed value in the registry.  A join loads the registry value
// and rejects a missing entry before publishing the local.  Joins following an import already have the correct
// binding and only validate that its registry value is present.

ParserResult<IrEmitUnit> IrEmitter::emit_namespace_stmt(const NamespaceStmtPayload &Payload)
{
   std::string_view namespace_name(strdata(Payload.name.symbol), Payload.name.symbol->len);
   if (Payload.reuses_import_binding) {
      std::optional<BCReg> existing = this->resolve_local(Payload.name.symbol);
      if (not existing) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant, "Reused namespace import binding is unavailable"));
      }
      this->emit_namespace_missing_guard(namespace_name, *existing);
      return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
   }

   FuncState *fs = &this->func_state;
   lua_State *L = this->lex_state.L;
   BCReg value_reg = fs->free_reg();

   if (Payload.mode IS NamespaceDeclarationMode::Create) {
      if (not Payload.initialiser) {
         return ParserResult<IrEmitUnit>::failure(this->make_error(
            ParserErrorCode::InternalInvariant, "Creating namespace has no initialiser"));
      }

      bcreg_reserve(fs, BCReg(1));
      this->publish_namespace_local(Payload.name, value_reg);

      auto emitted = this->emit_expression(*Payload.initialiser);
      if (not emitted.ok()) return ParserResult<IrEmitUnit>::failure(emitted.error_ref());
      ExpDesc value = emitted.value_ref();
      this->materialise_to_reg(value, value_reg, "namespace initialiser");
      fs->reset_freereg();

      BCReg registry_reg = fs->free_reg();
      bcreg_reserve(fs, BCReg(1));
      auto str_const = [fs](GCstr *String) -> BCREG {
         return const_gc(fs, obj2gco(String), LJ_TSTR);
      };
      bcemit_AD(fs, BC_GGET, registry_reg.raw(), str_const(lj_str_newlit(L, "_LIB")));
      bcemit_tsets(fs, value_reg.raw(), registry_reg.raw(), str_const(Payload.name.symbol));
      fs->reset_freereg();
   }
   else {
      bcreg_reserve(fs, BCReg(1));
      this->emit_namespace_registry_load(namespace_name, value_reg);
      this->emit_namespace_missing_guard(namespace_name, value_reg);
      this->publish_namespace_local(Payload.name, value_reg);
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}

//********************************************************************************************************************
// Emit bytecode for import statement: import 'path' [, 'path'...]

ParserResult<IrEmitUnit> IrEmitter::emit_import_stmt(const ImportStmtPayload &Payload)
{
   for (const ImportEntryPayload &entry : Payload.entries) {
      auto result = this->emit_import_entry(entry);
      if (not result.ok()) return result;
   }

   return ParserResult<IrEmitUnit>::success(IrEmitUnit{});
}
