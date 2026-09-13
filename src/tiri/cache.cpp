// Cache implementation is included directly by tiri_class.cpp so it can share its private compilation helpers.
//
// The automatic cache directory is created lazily with user-private permissions.  Entries are published from uniquely
// owned sibling files through the Core filesystem's move operation after the file is flushed and closed.  Publication
// is best-effort; unsupported destinations may decline it.  Failed publication does not prevent source execution.
// Cache files for deleted sources, obsolete configurations and older builds may accumulate; users may clear
// `temp:tiri/cache/` at any time.  Size limits and crash-orphan cleanup are not currently provided, and cache
// publication is not crash-durable.

static std::atomic_uint64_t glCacheTemporarySequence = 0;

#ifdef UNIT_TESTS
static std::atomic<CachePublishFailure> glCachePublishFailure = CachePublishFailure::NIL;

void set_cache_publish_failure(CachePublishFailure Failure)
{
   glCachePublishFailure.store(Failure, std::memory_order_relaxed);
}

static bool fail_cache_publication(CachePublishFailure Stage)
{
   auto expected = Stage;
   return glCachePublishFailure.compare_exchange_strong(expected, CachePublishFailure::NIL,
      std::memory_order_relaxed);
}
#else
static bool fail_cache_publication(CachePublishFailure) { return false; }
#endif

//********************************************************************************************************************
// Script flags currently affect runtime behaviour or parser-only metadata, not emitted bytecode.  Keep this helper
// explicit so a future bytecode-affecting flag has one identity boundary to update.

static std::vector<tiri::cache::CompilationOption> cache_compilation_options(const extTiri *)
{
   return {};
}

//********************************************************************************************************************
// Read source bytes without changing cache provenance or other Script state.

static ERR read_source_file(objFile *File, const std::string &Path, std::string &Source)
{
   if (not File) return ERR::NullArgs;

   int64_t size = 0;
   if (auto error = File->getSize(size); error != ERR::Okay) return error;
   if (auto error = read_open_file_to_string(File, size, Source); error != ERR::Okay) return error;

   // Bytecode paths are never treated as text, including malformed wrappers with a leading BOM.

   if (not has_script_extension(Path, ".tbc")) {
      auto content = check_bom(Source);
      if (content.data() != Source.data()) Source.assign(content);
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Confirm that a candidate cache was produced by this build from the current source content.  Source that cannot be
// read leaves only the build identity verifiable, which is the documented source-free deployment contract.  A cache
// written before identity tokens existed carries no build field and is therefore rejected.
//
// Only legacy identity is judged here.  The caller performs isolated structural validation before selection.

static bool legacy_cache_identity_matches(const extTiri *Self, const std::string &Cache, bool SourceAvailable)
{
   kt::Log log(__FUNCTION__);

   std::string_view payload, token;
   if (compiled_payload(Cache, payload, &token) != ERR::Okay) {
      log.trace("Deferring cache '%s' to Query, it is not a well-formed compiled file.",
         Self->EffectiveCacheFile.c_str());
      return true;
   }

   auto reject = [&](CSTRING Reason) {
      if (SourceAvailable) log.detail("Cache miss '%s': %s.", Self->EffectiveCacheFile.c_str(), Reason);
      else log.warning("Rejecting cache '%s' and its source is unreadable, %s.",
         Self->EffectiveCacheFile.c_str(), Reason);
      return false;
   };

   auto build_field = identity_build_field();
   if (not identity_token_matches_build(token, build_field)) {
      return reject("its identity token is invalid or it was produced by a different build");
   }

   if (not SourceAvailable) {
      log.detail("Accepting cache '%s' on its build identity, the source is unavailable.",
         Self->EffectiveCacheFile.c_str());
      return true;
   }

   if (token != make_identity_token(&Self->Statement)) return reject("the source content has changed");

   return true;
}

//********************************************************************************************************************
// Validate and split an original import request using the same name grammar as the parser's import resolver.

static bool valid_import_name(std::string_view Request, bool &Local, std::string &ParentPrefix,
   std::string_view &Name)
{
   Local = false;
   ParentPrefix.clear();
   Name = Request;
   int slash_count = 0;

   if (Name.starts_with("./")) {
      Local = true;
      Name.remove_prefix(2);
   }
   else {
      while (Name.starts_with("../")) {
         Local = true;
         ParentPrefix.append("../");
         Name.remove_prefix(3);
         slash_count++;
      }
   }

   size_t i = 0;
   for (; i < Name.size(); ++i) {
      const char value = Name[i];
      if ((value >= 'a') and (value <= 'z')) continue;
      if ((value >= 'A') and (value <= 'Z')) continue;
      if ((value >= '0') and (value <= '9')) continue;
      if ((value IS '-') or (value IS '_')) continue;
      if (value IS '/') { slash_count++; continue; }
      break;
   }
   return (i IS Name.size()) and (i < 96) and (slash_count <= 2);
}

//********************************************************************************************************************
// Replay an import request from its recorded parent context and return the resolver's current target spelling.

static std::string replay_import_resolution(extTiri *Self, std::string_view Parent,
   std::string_view Request)
{
   bool local = false;
   std::string parent_prefix;
   std::string_view name;
   if (not valid_import_name(Request, local, parent_prefix, name)) return {};

   std::string path(parent_prefix);
   path.append(name);
   if (local) {
      auto separator = Parent.find_last_of("/\\");
      if (separator != std::string_view::npos) path.insert(0, Parent.substr(0, separator + 1));
      else {
         std::string_view working_path;
         Self->getWorkingPath(working_path);
         if (not working_path.empty()) path.insert(0, working_path);
      }
   }
   else path.insert(0, "scripts:");
   path.append(".tiri");

   std::string resolved;
   if (ResolvePath(path, RSF::NO_FILE_CHECK, &resolved) IS ERR::Okay) return resolved;
   return path;
}

//********************************************************************************************************************
// Reconstruct the path tested by an `@if(exists=...)` observation from its original compilation context.

static std::string replay_exists_resolution(extTiri *Self, std::string_view Context,
   std::string_view Request)
{
   auto separator = Context.find_last_of("/\\");
   if (separator != std::string_view::npos) {
      std::string result(Context.substr(0, separator + 1));
      result.append(Request);
      return result;
   }

   std::string_view working_path;
   Self->getWorkingPath(working_path);
   std::string result(working_path);
   result.append(Request);
   return result;
}

//********************************************************************************************************************
// Check the restricted alpha-numeric module-name grammar accepted by compile-time module observations.

static bool module_name_is_valid(std::string_view Name)
{
   if (Name.empty() or Name.size() >= 32) return false;
   for (char value : Name) {
      if ((value >= 'a') and (value <= 'z')) continue;
      if ((value >= 'A') and (value <= 'Z')) continue;
      if ((value >= '0') and (value <= '9')) continue;
      return false;
   }
   return true;
}

//********************************************************************************************************************
// Verify that all recorded compilation inputs still reproduce the manifest without loading candidate bytecode.

static bool validate_cache_manifest(extTiri *Self, const tiri::cache::Manifest &Stored, std::string &Reason)
{
   using namespace tiri::cache;

   tiri::cache::Manifest expected;
   expected.BuildIdentity = TIRI_BUILD_COMMIT;
   expected.MainSource.ResolvedPath = Self->CompilationSourcePath;
   expected.Options = cache_compilation_options(Self);
   if (not lookup_identity_matches(Stored, expected)) {
      Reason = "its build, root identity or compilation options have changed";
      return false;
   }
   if ((Stored.MainSource.Size != Self->Statement.size()) or
       (Stored.MainSource.ContentDigest != content_digest(Self->Statement))) {
      Reason = "the main source content has changed";
      return false;
   }

   for (const auto &dependency : Stored.Imports) {
      auto resolved = replay_import_resolution(Self, dependency.ParentPath, dependency.OriginalRequest);
      if (resolved.empty() or (resolved != dependency.Source.ResolvedPath)) {
         Reason = "an import now resolves to a different source";
         return false;
      }

      objFile::create file = { fl::Path(resolved), fl::Flags(FL::READ) };
      std::string source;
      if (not file.ok() or (read_source_file(*file, resolved, source) != ERR::Okay)) {
         Reason = "an imported source is missing or unreadable";
         return false;
      }
      if ((dependency.Source.Size != source.size()) or
          (dependency.Source.ContentDigest != content_digest(source))) {
         Reason = "an imported source has changed";
         return false;
      }
   }

   for (const auto &resolution : Stored.ResolutionInputs) {
      bool matched = std::ranges::any_of(Stored.Imports, [&](const auto &Import) {
         return (Import.ParentPath IS resolution.Context) and (Import.OriginalRequest IS resolution.Name) and
            (Import.Source.ResolvedPath IS resolution.Value) and
            (replay_import_resolution(Self, Import.ParentPath, Import.OriginalRequest) IS resolution.Value);
      });
      if (not matched) {
         matched = std::ranges::any_of(Stored.ConditionalInputs, [&](const auto &Input) {
            return (Input.Kind IS ConditionalKind::EXISTS) and (Input.Context IS resolution.Context) and
               (Input.Name IS resolution.Name) and (replay_exists_resolution(Self, Input.Context, Input.Name) IS
               resolution.Value);
         });
      }
      if (not matched) {
         Reason = "a recorded path resolution input has changed or is unsupported";
         return false;
      }
   }

   for (const auto &input : Stored.ConditionalInputs) {
      std::string current;
      switch (input.Kind) {
         case ConditionalKind::IMPORTED:
            current = input.Context IS Stored.MainSource.ResolvedPath ? "false" : "true";
            break;
         case ConditionalKind::DEBUG_MODE: current = GetResource(RES::LOG_LEVEL) > 2 ? "true" : "false"; break;
         case ConditionalKind::LOG_LEVEL: current = std::to_string(GetResource(RES::LOG_LEVEL)); break;
         case ConditionalKind::PLATFORM: {
            const SystemState *state = GetSystemState();
            current = state->Platform ? state->Platform : "";
            break;
         }
         case ConditionalKind::EXISTS: {
            auto resolved = replay_exists_resolution(Self, input.Context, input.Name);
            current = AnalysePath(resolved, nullptr) IS ERR::Okay ? "true" : "false";
            break;
         }
         case ConditionalKind::MODULE_AVAILABLE:
            current = module_name_is_valid(input.Name) and (load_module_defs(input.Name) IS ERR::Okay) ?
               "true" : "false";
            break;
         case ConditionalKind::OTHER:
            Reason = "the cache contains an unsupported conditional observation";
            return false;
      }
      if (current != input.Value) {
         Reason = "a compile-time condition has changed";
         return false;
      }
   }
   return true;
}

//********************************************************************************************************************
// Load candidate bytecode in a disposable Lua state to validate its structure and optionally detect imported sources.

static bool validate_cache_bytecode(extTiri *Self, std::string_view Payload, std::string &Reason,
   bool *HasImports = nullptr)
{
   std::unique_ptr<lua_State, decltype(&lua_close)> validation(luaL_newstate(Self), lua_close);
   if (not validation or (initialise_compilation_state(validation.get()) != ERR::Okay)) {
      Reason = "an isolated validation state could not be created";
      return false;
   }

   std::string diagnostic;
   if (load_compilation_input(validation.get(), Self, Payload, CompilationInputOrigin::DIRECT_BYTECODE,
       diagnostic) != ERR::Okay) {
      Reason = diagnostic.empty() ? "its bytecode is structurally invalid" : diagnostic;
      return false;
   }
   if (HasImports) {
      *HasImports = false;
      if (lua_isfunction(validation.get(), -1) and not lua_iscfunction(validation.get(), -1)) {
         GCproto *prototype = funcproto(funcV(validation->top - 1));
         const CompilationSourceMap *sources = proto_compilation_sources(prototype);
         *HasImports = sources and (sources->count > 1);
      }
   }
   return true;
}

//********************************************************************************************************************
// Caching applies only after an explicit or automatic destination has been selected and proved distinct from the
// source.  Document processing is excluded because its parser symbols are never serialised.

static bool cache_destination_permitted(const extTiri *Self)
{
   return (Self->CacheOrigin != CacheDestinationOrigin::NONE) and (not Self->EffectiveCacheFile.empty()) and
      (not has_script_extension(Self->Path, ".tbc")) and ((Self->Flags & SCF::PROCESS_DOC) IS SCF::NIL);
}

constexpr JOF cache_parser_output_options()
{
   return JOF::DIAGNOSE|JOF::DUMP_BYTECODE|JOF::PROFILE|JOF::TOP_TIPS|JOF::TIPS|JOF::ALL_TIPS|JOF::TRACE;
}

//********************************************************************************************************************
// Select one effective destination without mutating the caller's Path or CacheFile.  Explicit destinations have
// precedence.  Automatic identity is available only after the readable source has established its resolved path.

static void select_cache_destination(extTiri *Self, ERR SourceError)
{
   kt::Log log(__FUNCTION__);
   Self->EffectiveCacheFile.clear();
   Self->CacheSelectionValue = Self->CacheFile;
   Self->CacheSelectionParserOutput = (Self->JitOptions & cache_parser_output_options()) != JOF::NIL;
   Self->CacheOrigin = CacheDestinationOrigin::NONE;
   Self->CacheHit = false;

   if (Self->Path.empty() or Self->Path.starts_with("string:") or Self->Path.starts_with("STRING:") or
       has_script_extension(Self->Path, ".tbc") or ((Self->Flags & SCF::PROCESS_DOC) != SCF::NIL)) return;

   if (not Self->CacheFile.empty()) {
      Self->EffectiveCacheFile = Self->CacheFile;
      Self->CacheOrigin = CacheDestinationOrigin::EXPLICIT;
   }
   else if ((SourceError IS ERR::Okay) and ((Self->Flags & SCF::AUTO_CACHE) != SCF::NIL)) {
      tiri::cache::Manifest identity;
      identity.BuildIdentity = TIRI_BUILD_COMMIT;
      identity.MainSource.ResolvedPath = Self->CompilationSourcePath;
      identity.Options = cache_compilation_options(Self);
      auto key = tiri::cache::file_key(identity);
      if (key.empty()) return;
      Self->EffectiveCacheFile = std::format("temp:tiri/cache/{}.tbc", key);
      Self->CacheOrigin = CacheDestinationOrigin::AUTOMATIC;
   }
   else return;

   if (CompareFilePaths(Self->Path, Self->EffectiveCacheFile) IS ERR::Okay) {
      log.warning("Ignoring cache destination '%s' because it resolves to the script source.",
         Self->EffectiveCacheFile.c_str());
      Self->EffectiveCacheFile.clear();
      Self->CacheOrigin = CacheDestinationOrigin::NONE;
      return;
   }

   log.trace("Selected %s cache destination '%s'.",
      Self->CacheOrigin IS CacheDestinationOrigin::EXPLICIT ? "explicit" : "automatic",
      Self->EffectiveCacheFile.c_str());
}

// Parser-output options require a real source compilation so that their requested diagnostics are produced.  This is
// a lookup bypass, not an identity difference: the resulting bytecode remains eligible for the same cache destination.

static bool cache_lookup_permitted(const extTiri *Self)
{
   return cache_destination_permitted(Self) and
      ((Self->JitOptions & cache_parser_output_options()) IS JOF::NIL);
}

//********************************************************************************************************************
// Create a uniquely owned sibling of the destination.

static std::string temporary_cache_path(std::string_view CachePath)
{
   std::string path;
   auto separator = CachePath.find_last_of(":/\\");
   if (separator IS std::string_view::npos) path.push_back('.');
   else {
      path.assign(CachePath, 0, separator + 1);
      path.push_back('.');
      CachePath.remove_prefix(separator + 1);
   }
   path.append(CachePath);

   int process_id = 0;
   if (auto task = CurrentTask()) process_id = task->ProcessID;
   auto sequence = glCacheTemporarySequence.fetch_add(1, std::memory_order_relaxed) + 1;
   path.append(std::format(".tmp.{}.{}.{}", process_id, GetThreadID(), sequence));
   return path;
}

//********************************************************************************************************************
// Cache output is written to an exclusively owned temporary file beside the destination, flushed and closed before
// publication through the Core filesystem abstraction.  Publication failures never prevent the compiled source
// from running and ordinary failures clean up owned staging files.

static ERR publish_cache(extTiri *Self)
{
   kt::Log log(__FUNCTION__);
   constexpr int MAX_TEMP_ATTEMPTS = 16;

   if ((not Self->CompilationManifest) or Self->EffectiveCacheFile.empty()) return ERR::InvalidData;

   if (Self->CacheOrigin IS CacheDestinationOrigin::AUTOMATIC) {
      auto error = CreateFolder("temp:tiri/cache/", PERMIT::USER);
      if ((error != ERR::Okay) and (error != ERR::FileExists)) return error;
   }

   std::string payload;
   auto append_payload = [](lua_State *, const void *Data, size_t Size, void *Context) {
      ((std::string *)Context)->append((const char *)Data, Size);
      return 0;
   };
   const int stack_top = lua_gettop(Self->Lua);
   const int dump_error = lua_dump(Self->Lua, append_payload, &payload);
   lua_settop(Self->Lua, stack_top);
   if (dump_error) return ERR::InvalidData;

   std::string envelope;
   if (tiri::cache::encode_envelope(*Self->CompilationManifest, payload, envelope) !=
       tiri::cache::FormatError::OKAY) return ERR::InvalidData;

   for (int attempt = 0; attempt < MAX_TEMP_ATTEMPTS; ++attempt) {
      auto temporary_path = temporary_cache_path(Self->EffectiveCacheFile);
      bool owns_temporary = false;
      auto cleanup = deferred_call([&] {
         if (owns_temporary and (AnalysePath(temporary_path, nullptr) IS ERR::Okay)) {
            if (auto error = DeleteFile(temporary_path, nullptr); error != ERR::Okay) {
               log.warning("Failed to remove temporary cache '%s': %s", temporary_path.c_str(), GetErrorMsg(error));
            }
         }
      });

      if (fail_cache_publication(CachePublishFailure::CREATE)) return ERR::TestFailed;

      ERR error;
      {
         objFile::create cache = {
            fl::Path(temporary_path), fl::Flags(FL::NEW|FL::WRITE|FL::EXCLUSIVE),
            fl::Permissions(Self->CachePermissions)
         };
         error = cache.error;
         if (error IS ERR::FileExists) continue;
         if (not cache.ok()) return error;
         owns_temporary = true;

         if (fail_cache_publication(CachePublishFailure::WRITE)) error = ERR::TestFailed;
         else {
            size_t total = 0;
            error = ERR::Okay;
            while ((total < envelope.size()) and (error IS ERR::Okay)) {
               const size_t remaining = envelope.size() - total;
               const size_t count = std::min(remaining, size_t(std::numeric_limits<int>::max()));
               int written = 0;
               error = cache->write(std::span((const int8_t *)envelope.data() + total, count), &written);
               if ((error IS ERR::Okay) and (written != int(count))) error = ERR::Write;
               total += size_t(written);
            }
         }

         if (error IS ERR::Okay) {
            if (fail_cache_publication(CachePublishFailure::FLUSH)) error = ERR::TestFailed;
            else error = cache->flush();
         }
      }

      if (error != ERR::Okay) return error;
      if (fail_cache_publication(CachePublishFailure::MOVE)) return ERR::TestFailed;

      error = MoveFile(temporary_path, Self->EffectiveCacheFile, nullptr);
      if (error IS ERR::Okay) owns_temporary = false;
      return error;
   }

   return ERR::FileExists;
}

//********************************************************************************************************************
// Read and validate the selected cache into Statement without compiling it in the live state.

static ERR load_selected_cache(extTiri *Self, ERR SourceError, std::optional<std::string> *SourceFallback = nullptr)
{
   kt::Log log(__FUNCTION__);
   if (not cache_lookup_permitted(Self)) return ERR::Okay;

   objFile::create cache = { fl::Path(Self->EffectiveCacheFile), fl::Flags(FL::READ) };
   int64_t size = 0;
   if (not cache.ok() or (cache->getSize(size) != ERR::Okay)) {
      log.trace("Cache miss '%s': no readable entry.", Self->EffectiveCacheFile.c_str());
      return ERR::Okay;
   }

   std::string content;
   auto error = read_open_file_to_string(*cache, size, content);
   if (error != ERR::Okay) {
      log.warning("Failed to read cache '%s': %s", Self->EffectiveCacheFile.c_str(), GetErrorMsg(error));
      return SourceError IS ERR::Okay ? ERR::Okay : error;
   }

   bool accepted = false;
   std::string payload;
   tiri::cache::EnvelopeView envelope;
   auto format_error = tiri::cache::decode_envelope(content, envelope);
   if (format_error IS tiri::cache::FormatError::OKAY) {
      std::string reason;
      if (SourceError != ERR::Okay) reason = "schema-1 caches require readable source for validation";
      else if (validate_cache_manifest(Self, envelope.Metadata, reason) and
               validate_cache_bytecode(Self, envelope.Payload, reason)) {
         payload.assign(envelope.Payload);
         accepted = true;
      }
      if (not accepted) log.detail("Cache miss '%s': %s.", Self->EffectiveCacheFile.c_str(), reason.c_str());
   }
   else if (format_error IS tiri::cache::FormatError::NOT_CACHE) {
      accepted = Self->CacheOrigin IS CacheDestinationOrigin::EXPLICIT and
         legacy_cache_identity_matches(Self, content, SourceError IS ERR::Okay);
      if (accepted) {
         std::string reason;
         bool has_imports = false;
         if (validate_cache_bytecode(Self, content, reason, &has_imports) and
             ((SourceError IS ERR::Okay) or not has_imports)) payload = std::move(content);
         else {
            accepted = false;
            if (reason.empty()) reason = "legacy imported caches require source validation metadata";
            log.detail("Cache miss '%s': %s.", Self->EffectiveCacheFile.c_str(), reason.c_str());
         }
      }
      else if (Self->CacheOrigin IS CacheDestinationOrigin::AUTOMATIC) {
         log.detail("Cache miss '%s': legacy bytecode is not an automatic cache.",
            Self->EffectiveCacheFile.c_str());
      }
   }
   else {
      log.detail("Cache miss '%s': malformed schema-1 envelope (%s).", Self->EffectiveCacheFile.c_str(),
         tiri::cache::format_error_name(format_error));
   }

   if (accepted) {
      log.detail("Cache hit '%s'.", Self->EffectiveCacheFile.c_str());
      if ((SourceError IS ERR::Okay) and SourceFallback) SourceFallback->emplace(std::move(Self->Statement));
      Self->Statement = std::move(payload);
      Self->LoadedFromCache = true;
      Self->CacheHit = true;
      Self->SaveCompiled = false;
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Read the original path independently of cache selection, including its metadata for a replacement cache.

static ERR load_source(extTiri *Self)
{
   kt::Log log(__FUNCTION__);

   log.branch("Loading script from %s", Self->Path.c_str());

   objFile::create file = { fl::Path(Self->Path), fl::Flags(FL::READ) };
   if (not file.ok()) return file.error;

   std::string source;
   if (auto error = read_source_file(*file, Self->Path, source); error != ERR::Okay) return error;

   std::string resolved_path;
   if (ResolvePath(Self->Path, RSF::NO_FILE_CHECK, &resolved_path) IS ERR::Okay) {
      Self->CompilationSourcePath = std::move(resolved_path);
   }
   else Self->CompilationSourcePath = Self->Path;

   Self->SourceModifiedHint = 0;
   file->getTimestamp(Self->SourceModifiedHint);

   Self->Statement = std::move(source);
   Self->LoadedFromCache = false;
   Self->SaveCompiled = false;
   Self->CachePermissions = PERMIT::NIL;
   if (auto error = file->getPermissions(Self->CachePermissions); error != ERR::Okay) {
      log.warning("Failed to read source permissions for cache file: %s", GetErrorMsg(error));
   }

   return ERR::Okay;
}

//********************************************************************************************************************
// Clear or reselect cache state when mutable Script inputs change between Init and Query.

static ERR refresh_cache_lifecycle(extTiri *Self)
{
   const bool parser_output = (Self->JitOptions & cache_parser_output_options()) != JOF::NIL;
   if ((not Self->Path.empty()) and ((Self->CacheFile != Self->CacheSelectionValue) or
       (parser_output != Self->CacheSelectionParserOutput))) {
      ERR source_error = ERR::Okay;
      if (Self->LoadedFromCache) {
         source_error = load_source(Self);
         Self->LoadedFromCache = false;
         Self->CacheHit = false;
      }
      select_cache_destination(Self, source_error);
      Self->SaveCompiled = (source_error IS ERR::Okay) and cache_destination_permitted(Self);
      if (Self->CacheOrigin IS CacheDestinationOrigin::AUTOMATIC) Self->CachePermissions = PERMIT::USER;
      if (source_error != ERR::Okay) {
         if (auto error = load_selected_cache(Self, source_error); error != ERR::Okay) return error;
         if (not Self->LoadedFromCache) return source_error;
      }
   }

   if (Self->Path.empty()) {
      Self->LoadedFromCache = false;
      Self->CacheHit = false;
      Self->SaveCompiled = false;
      Self->CompilationSourcePath.clear();
      Self->EffectiveCacheFile.clear();
      Self->CacheSelectionValue = Self->CacheFile;
      Self->CacheSelectionParserOutput = false;
      Self->CacheOrigin = CacheDestinationOrigin::NONE;
      Self->SourceModifiedHint = 0;
   }
   return ERR::Okay;
}

//********************************************************************************************************************
// Load source and select a cache destination during Script initialisation.  Source-free explicit caches are loaded
// immediately because Init must still reject an unusable object; readable-source lookup is deferred to Query.

static ERR prepare_cached_input(extTiri *Self)
{
   const ERR source_error = load_source(Self);
   select_cache_destination(Self, source_error);
   Self->SaveCompiled = (source_error IS ERR::Okay) and cache_destination_permitted(Self);
   if (Self->CacheOrigin IS CacheDestinationOrigin::AUTOMATIC) Self->CachePermissions = PERMIT::USER;
   if (source_error != ERR::Okay) {
      if (auto error = load_selected_cache(Self, source_error); error != ERR::Okay) return error;
   }
   if ((not Self->LoadedFromCache) and (source_error != ERR::Okay)) return source_error;
   return ERR::Okay;
}

//********************************************************************************************************************
// Select a readable-source cache immediately before Query compilation so fallback source remains local.

static ERR prepare_query_cache(extTiri *Self, std::optional<std::string> &SourceFallback)
{
   if (Self->LoadedFromCache or not cache_lookup_permitted(Self)) return ERR::Okay;
   return load_selected_cache(Self, ERR::Okay, &SourceFallback);
}

//********************************************************************************************************************
// Parse the selected input and perform the one-shot local-source fallback when cached bytecode is rejected.

static ERR load_statement_with_cache_fallback(extTiri *Self, std::optional<std::string> &SourceFallback)
{
   kt::Log log(__FUNCTION__);
   auto error = load_statement(Self);
   if ((error != ERR::Okay) and Self->LoadedFromCache) {
      log.warning("Failed to load cache '%s': %s", Self->EffectiveCacheFile.c_str(), Self->ErrorMessage.c_str());
      if (SourceFallback) {
         Self->Statement = std::move(*SourceFallback);
         SourceFallback.reset();
         Self->LoadedFromCache = false;
         Self->CacheHit = false;
         Self->SaveCompiled = cache_destination_permitted(Self);
         error = load_statement(Self);
      }
   }
   return error;
}

//********************************************************************************************************************
// Publish a successful source compilation without allowing cache failure to prevent execution.

static void publish_pending_cache(extTiri *Self)
{
   if (not Self->SaveCompiled) return;
   kt::Log log(__FUNCTION__);
   log.msg("Compiling the source into the cache file.");
   Self->SaveCompiled = false;
   if (auto error = publish_cache(Self); error != ERR::Okay) {
      log.warning("Failed to save cache '%s': %s", Self->EffectiveCacheFile.c_str(), GetErrorMsg(error));
   }
}
