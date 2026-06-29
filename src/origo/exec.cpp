
//********************************************************************************************************************
// Executes the target.

ERR exec_source(std::string_view TargetFile, int ShowTime, const std::string_view Procedure)
{
   kt::Log log(__FUNCTION__);
   ERR error;

   log.msg("Identifying file '%.*s'", int(TargetFile.size()), TargetFile.data());

   FindClass(CLASSID::TIRI); // Pre-load the Tiri module

   CLASSID class_id, derived_id;
   if (TargetFile.starts_with("string:")) { // Presumes Tiri
      derived_id = CLASSID::TIRI;
      class_id = CLASSID::SCRIPT;
   }
   else if ((error = IdentifyFile(TargetFile, CLASSID::NIL, &class_id, &derived_id)) != ERR::Okay) {
      printf("Failed to identify the type of file for path '%.*s', error: %s.  Assuming Tiri script.\n", int(TargetFile.size()), TargetFile.data(), GetErrorMsg(error));
      derived_id = CLASSID::TIRI;
      class_id = CLASSID::SCRIPT;
   }

   if (class_id IS CLASSID::PARC) glSandbox = true;

   if (glSandbox) {
      #ifdef _WIN32
         IntegrityLevel il = get_integrity_level();
         if (il < INTEGRITY_LEVEL_LOW) {
            // If running with an integrity better than 'low', re-run the process with a low integrity.

            if (glRelaunched) return ERR::Security;

            log.msg("Inappropriate integrity level %d (must be %d or higher), re-launching...\n", il, INTEGRITY_LEVEL_LOW);

            std::ostringstream cmdline;

            char exe_buffer[128];
            uint32_t i = get_exe(exe_buffer, sizeof(exe_buffer));
            if ((!i) or (i >= sizeof(exe_buffer)-1)) return ERR::Failed;

            cmdline << '"' << exe_buffer << "\" --relaunch";
            if (GetResource(RES::LOG_LEVEL) >= 5) cmdline << " --log-debug";
            else if (GetResource(RES::LOG_LEVEL) >= 3) cmdline << "--log-info";

            for (int a=0; a < std::ssize(glArgs); a++) {
               if (kt::iequals("--sandbox", glArgs[a])) continue;
               cmdline << " \"";
               if (glArgs[a].find('"') != std::string::npos) {
                  std::string sub = glArgs[a];
                  std::size_t it;
                  while ((it = sub.find('"')) != std::string::npos) {
                     sub.replace(sub.begin() + it, sub.begin() + it + 1, "\\\"");
                  }
                  cmdline << sub;
               }
               else cmdline << glArgs[a];
               cmdline << '"';
            }

            // Temporarily switch off debug messages until the child process returns.

            int log_level = GetResource(RES::LOG_LEVEL);
            SetResource(RES::LOG_LEVEL, 1);

            create_low_process(cmdline.str(), true);

            SetResource(RES::LOG_LEVEL, log_level);

            return ERR::LimitedSuccess;
         }

      #else
/*
         error = init_sandbox(glArgs, glRelaunched ? false : true);
         if (error IS ERR::LimitedSuccess) {
            // Limited success means that the process was re-launched with a lower priority.
            return error;
         }
         else if (error) {
            printf("Sandbox initialisation failed.\n");
            return error;
         }
*/
      #endif
   }
#if 0
   if (class_id IS ID_PARC) {
      objParc::create parc = { fl::Path(TargetFile), fl::Allow(glAllow) };

      if (parc.ok()) {
         // The user can use the --allow parameter to automatically give the PARC program additional access
         // rights as required.  E.g. "--allow storage,maxSize:1M,maxFiles:20,memory:100M"

         if ((error = parc->activate())) {
            STRING msg;
            if (!parc->getMessage(&msg)) {
               printf("Failed to execute the archive, error: %s\n", GetErrorMsg(error));
            }
            else printf("Failed to execute the archive, error: %s\n", GetErrorMsg(error));

            error = ERR::Activate;
         }
         else printf("PARC execution completed successfully.\n");
      }
      else {
         printf("Failed to initialise the PARC archive, error: %s\n", GetErrorMsg(error));
         error = ERR::CreateObject;
      }

      return error;
   }
#endif
   if (!NewObject(derived_id != CLASSID::NIL ? derived_id : class_id, &glScript)) {
      glScript->setTarget(glTarget ? glTarget->UID : CurrentTaskID());
      glScript->setPath(TargetFile);

      if (!Procedure.empty()) glScript->setProcedure(Procedure);

      if (glArgsIndex) {
         for (unsigned i=glArgsIndex; i < glArgs.size(); i++) {
            auto eq = glArgs[i].find('=');
            if (eq IS std::string::npos) acSetKey(glScript, glArgs[i], "true");
            else {
               auto argname = std::string(glArgs[i], 0, eq);
               eq++;
               if (glArgs[i][eq] IS '{') {
                  // Array definition, e.g. files={ file1.txt file2.txt }
                  // This will be converted to files(0)=file.txt files(1)=file2.txt

                  if (glArgs[i][eq+1] > 0x20) acSetKey(glScript, argname, glArgs[i].substr(eq));
                  else {
                     unsigned arg_index = 0;
                     for (++i; (i < glArgs.size()) and (glArgs[i][0] != '}'); i++) {
                        auto argindex = argname + '(' + std::to_string(arg_index) + ')';
                        acSetKey(glScript, argindex, glArgs[i]);
                        arg_index++;
                     }

                     if (i >= glArgs.size()) break;

                     // Note that the last arg in the array will be the "}" that closes it

                     acSetKey(glScript, (argname + ":size"), std::to_string(arg_index));
                  }
               }
               else acSetKey(glScript, argname, glArgs[i].substr(eq));
            }
         }
      }

      int64_t start_time = 0;
      if (ShowTime) start_time = PreciseTime();

      if (auto error = InitObject(glScript); !error) {
         if (auto error = acActivate(glScript); !error) {
            std::string_view msg;
            if (ShowTime) { // Print the execution time of the script
               auto start_seconds = (double)start_time / 1000000.0;
               auto end_seconds   = (double)PreciseTime() / 1000000.0;
               printf("Script executed in %f seconds.\n\n", end_seconds - start_seconds);
            }

            if (glScript->Error != ERR::Okay) {
               if (GetResource(RES::LOG_LEVEL) <= 1) {
                  if ((!glScript->getErrorMessage(msg)) and not msg.empty()) {
                     printf("%.*s\n", int(msg.size()), msg.data());
                  }
                  else if (glScript->Error IS ERR::Exception) {
                     printf("Script threw an exception: %s\n", GetErrorMsg(glScript->Error));
                  }
                  else printf("Script returned an error code of %d: %s\n", int(glScript->Error), GetErrorMsg(glScript->Error));
               }

               return glScript->Error;
            }

            if ((!glScript->getErrorMessage(msg)) and not msg.empty()) {
               if (GetResource(RES::LOG_LEVEL) <= 1) {
                  printf("Script returned error message: %.*s\n", int(msg.size()), msg.data());
               }
               return ERR::Failed;
            }
            else return ERR::Okay;
         }
         else {
            printf("Script failed during processing: %s\nUse --log-warning or --log-api to examine the failure.\n", GetErrorMsg(error));
            return error;
         }
      }
      else {
         printf("Failed to load / initialise the script: %s\n", GetErrorMsg(error));
         return error;
      }
   }
   else {
      printf("Internal Failure: Failed to create a new Script object for file processing.\n");
      return ERR::Failed;
   }
}
