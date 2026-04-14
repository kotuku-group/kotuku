
//********************************************************************************************************************
// Executes the target.

ERR exec_source(std::string TargetFile, int ShowTime, const std::string Procedure)
{
   pf::Log log(__FUNCTION__);
   ERR error;

   log.msg("Identifying file '%s'", TargetFile.c_str());

   FindClass(CLASSID::TIRI);

   CLASSID class_id, subclass;
   if (pf::startswith("STRING:", TargetFile)) {
      subclass = CLASSID::SCRIPT;
      class_id = CLASSID::SCRIPT;
   }
   else if ((error = IdentifyFile(TargetFile.c_str(), CLASSID::NIL, &class_id, &subclass)) != ERR::Okay) {
      printf("Failed to identify the type of file for path '%s', error: %s.  Assuming CLASSID::SCRIPT.\n", TargetFile.c_str(), GetErrorMsg(error));
      subclass = CLASSID::SCRIPT;
      class_id = CLASSID::SCRIPT;
   }

   if (class_id IS CLASSID::PARC) glSandbox = true;

   if (glSandbox) {
      pf::vector<std::string> *params = nullptr;
      glTask->get(FID_Parameters, params);

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

            pf::vector<std::string> &args = *params;
            for (int a=0; a < std::ssize(args); a++) {
               if (pf::iequals("--sandbox", args[a])) continue;
               cmdline << " \"";
               if (args[a].find('"') != std::string::npos) {
                  std::string sub = args[a];
                  std::size_t it;
                  while ((it = sub.find('"')) != std::string::npos) {
                     sub.replace(sub.begin() + it, sub.begin() + it + 1, "\\\"");
                  }
                  cmdline << sub;
               }
               else cmdline << args[a];
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
         error = init_sandbox(args, glRelaunched ? false : true);
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
            if (!parc->get(FID_Message, &msg)) {
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
   if (NewObject(subclass != CLASSID::NIL ? subclass : class_id, &glScript) IS ERR::Okay) {
      glScript->setTarget(glTarget ? glTarget->UID : CurrentTaskID());
      glScript->setPath(TargetFile);

      if (!Procedure.empty()) glScript->setProcedure(Procedure);

      if (glArgsIndex) {
         pf::vector<std::string> &args = *glArgs;

         for (unsigned i=glArgsIndex; i < args.size(); i++) {
            auto eq = args[i].find('=');
            if (eq IS std::string::npos) acSetKey(glScript, args[i].c_str(), "true");
            else {
               auto argname = std::string(args[i], 0, eq);
               eq++;
               if (args[i][eq] IS '{') {
                  // Array definition, e.g. files={ file1.txt file2.txt }
                  // This will be converted to files(0)=file.txt files(1)=file2.txt

                  if (args[i][eq+1] > 0x20) acSetKey(glScript, argname.c_str(), args[i].c_str() + eq);
                  else {
                     unsigned arg_index = 0;
                     for (++i; (i < args.size()) and (args[i][0] != '}'); i++) {
                        auto argindex = argname + '(' + std::to_string(arg_index) + ')';
                        acSetKey(glScript, argindex.c_str(), args[i].c_str());
                        arg_index++;
                     }

                     if (i >= args.size()) break;

                     // Note that the last arg in the array will be the "}" that closes it

                     acSetKey(glScript, (argname + ":size").c_str(), std::to_string(arg_index).c_str());
                  }
               }
               else acSetKey(glScript, argname.c_str(), args[i].c_str() + eq);
            }
         }
      }

      int64_t start_time = 0;
      if (ShowTime) start_time = PreciseTime();

      if (auto error = InitObject(glScript); error IS ERR::Okay) {
         if (auto error = acActivate(glScript); error IS ERR::Okay) {
            if (ShowTime) { // Print the execution time of the script
               auto start_seconds = (double)start_time / 1000000.0;
               auto end_seconds   = (double)PreciseTime() / 1000000.0;
               printf("Script executed in %f seconds.\n\n", end_seconds - start_seconds);
            }

            if (glScript->Error != ERR::Okay) {
               log.msg("Script returned an error code of %d: %s", int(glScript->Error), GetErrorMsg(glScript->Error));
               return glScript->Error;
            }

            CSTRING msg;
            if ((glScript->get(FID_ErrorMessage, msg) IS ERR::Okay) and (msg)) {
               log.msg("Script returned error message: %s", msg);
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
