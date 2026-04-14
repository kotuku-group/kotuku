
/*********************************************************************************************************************

-FUNCTION-
IdentifyFile: Analyse a file and identify a class that can process it.

This function examines the relationship between file data and Kōtuku classes.  For instance, a JPEG file would be
identified as a datatype of the @Picture class.  An MP3 file would be identified as a datatype of the @Sound
class.

The method involves analysing the `Path`'s file extension and comparing it to the supported extensions of all available
classes.  If a class supports the file extension, the ID of that class will be returned. If the file extension is
not listed in the class dictionary or if it is listed more than once, the first 80 bytes of the file's data will be
loaded and checked against classes that can match against file header information.  If a match is found, the ID of the
matching class will be returned.

The `ERR::Search` code is returned if a suitable class does not match the targeted file.

-INPUT-
cstr Path:     The location of the object data.
cid Filter:    Restrict the search to classes in this subset, or use `CLASSID::NIL` to search all classes.
&cid Class:    Must refer to a `CLASSID` variable that will store the resulting class ID.
&cid SubClass: Optional argument that can refer to a variable that will store the resulting sub-class ID (if the result is a base-class, this variable will receive a value of zero).

-ERRORS-
Okay
Args
Search: A suitable class could not be found for the data source.
FileNotFound
Read
-END-

*********************************************************************************************************************/

ERR IdentifyFile(CSTRING Path, CLASSID Filter, CLASSID *ClassID, CLASSID *SubClassID)
{
   pf::Log log(__FUNCTION__);
   int i, bytes_read;
   constexpr int HEADER_SIZE = 80;

   if ((!Path) or (!ClassID)) return log.warning(ERR::NullArgs);

   log.branch("File: %s", Path);

   // Determine the class type by examining the Path file name.  If the file extension does not tell us the class
   // that supports the data, we then load the first 256 bytes from the file and then compare file headers.

   ERR error = ERR::Okay;
   std::string res_path;
   if (ClassID) *ClassID = CLASSID::NIL;
   if (SubClassID) *SubClassID = CLASSID::NIL;
   uint8_t buffer[400] = { 0 };

   if (auto res_error = ResolvePath(Path, RSF::APPROXIMATE|RSF::PATH|RSF::CHECK_VIRTUAL, &res_path); res_error != ERR::Okay) {
      if (res_error IS ERR::VirtualVolume) {
         // Virtual volumes may support the IdentifyFile() request as a means of speeding up file identification.  This
         // is often useful when probing remote file systems.  If the FS doesn't support this option, we can still
         // fall-back to the standard file reading option.
         //
         // Note: A virtual volume may return ERR::Okay even without identifying the class of the queried file.  This
         // means that the file was analysed but belongs to no known class.

         if (auto vd = get_virtual(res_path)) {
            if (vd->IdentifyFile) {
               if (vd->IdentifyFile(res_path, ClassID, SubClassID) IS ERR::Okay) {
                  log.trace("Virtual volume identified the target file.");
                  goto class_identified;
               }
               else log.trace("Virtual volume reports no support for %d:%d", *ClassID, *SubClassID);
            }
            else log.trace("Virtual volume does not support IdentifyFile()");
         }
      }
      else {
         // Before we assume failure - check for the use of semicolons that split the string into multiple file names.

         log.warning("ResolvePath() failed on '%s', error '%s'", Path, GetErrorMsg(res_error));

         if (startswith("string:", Path)) { // Do not check for '|' when string: is in use
            return ERR::FileNotFound;
         }

         for (i=0; Path[i] and (Path[i] != '|'); i++);

         if (Path[i] IS '|') {
            auto tmp = std::string(Path, i);
            if (ResolvePath(tmp, RSF::APPROXIMATE, &res_path) != ERR::Okay) {
               return ERR::FileNotFound;
            }
         }
         else return ERR::FileNotFound;
      }
   }

   // Check against the class registry to identify what class and sub-class that this data source belongs to.

   if (!glClassDB.empty()) {
      // Check extension

      if (*ClassID IS CLASSID::NIL) {
         if (auto sep = res_path.find_last_of("."); sep != std::string::npos) {
            auto ext = res_path.substr(sep + 1, std::string::npos);
            if (auto ext_class_id = lookup_class_by_ext(Filter, ext); ext_class_id != CLASSID::NIL) {
               auto &rec = glClassDB[ext_class_id];

               if (rec.ParentID != CLASSID::NIL) { // Confirm if ext_class_id is a sub-class
                  *ClassID = rec.ParentID;
                  if (SubClassID) *SubClassID = rec.ClassID;
               }
               else *ClassID = rec.ClassID;
            }
         }
      }

      // Check data

      if (*ClassID IS CLASSID::NIL) {
         log.trace("Loading file header to identify '%s' against class registry", res_path);

         if ((ReadFileToBuffer(res_path.c_str(), buffer, HEADER_SIZE, &bytes_read) IS ERR::Okay) and (bytes_read >= 4)) {
            log.trace("Checking file header data (%d bytes) against %d classes....", bytes_read, glClassDB.size());
            for (auto it = glClassDB.begin(); it != glClassDB.end(); it++) {
               auto &rec = it->second;
               if (rec.Header.empty()) continue;

               auto header = std::string_view(rec.Header); // Compare the header to the Path buffer
               bool match = true;  // Headers use an offset based hex format, for example: [8:$958a9b9f9301][24:$939a9fff]
               while (!header.empty()) {
                  if (!header.starts_with('[')) {
                     if ((header.starts_with('|')) and (match)) break;
                     header.remove_prefix(1);
                     continue;
                  }

                  header.remove_prefix(1);
                  int offset = svtonum<int>(header);
                  if (auto i = header.find(':'); i != std::string::npos) header.remove_prefix(i + 1);
                  else break;

                  if (header.starts_with('$')) { // Find and compare the data, one byte at a time
                     header.remove_prefix(1);
                     while (!header.empty()) {
                        if (((header[0] >= '0') and (header[0] <= '9')) or
                            ((header[0] >= 'A') and (header[0] <= 'F')) or
                            ((header[0] >= 'a') and (header[0] <= 'f'))) break;
                        header.remove_prefix(1);
                     }

                     uint8_t byte;
                     while (!header.empty()) {
                        // Nibble 1
                        if ((header[0] >= '0') and (header[0] <= '9')) byte = (header[0] - '0')<<4;
                        else if ((header[0] >= 'A') and (header[0] <= 'F')) byte = (header[0] - 'A' + 0x0a)<<4;
                        else if ((header[0] >= 'a') and (header[0] <= 'f')) byte = (header[0] - 'a' + 0x0a)<<4;
                        else break;
                        header.remove_prefix(1);

                        // Nibble 2
                        if ((header[0] >= '0') and (header[0] <= '9')) byte |= header[0] - '0';
                        else if ((header[0] >= 'A') and (header[0] <= 'F')) byte |= header[0] - 'A' + 0x0a;
                        else if ((header[0] >= 'a') and (header[0] <= 'f')) byte |= header[0] - 'a' + 0x0a;
                        else break;
                        header.remove_prefix(1);

                        if (offset >= bytes_read) { match = false; break; }
                        if (byte != buffer[offset++]) { match = false; break; }
                     }
                  }
                  else {
                     while ((!header.empty()) and (header[0] != ']')) {
                        if (offset >= bytes_read) { match = false; break; }
                        if (header[0] != buffer[offset++]) { match = false; break; }
                        header.remove_prefix(1);
                     }
                  }

                  if (!match) {
                     if (auto sep = header.find('|'); sep != std::string::npos) { // Look for an or operation
                        match = true; // Continue comparisons
                        header.remove_prefix(sep + 1);
                     }
                     else break;
                  }
               }

               if (match) {
                  if (rec.ParentID != CLASSID::NIL) {
                     *ClassID = rec.ParentID;
                     if (SubClassID) *SubClassID = rec.ClassID;
                  }
                  else *ClassID = rec.ClassID;
                  break;
               }
            } // for all classes
         }
         else error = log.warning(ERR::Read);
      }
   }
   else {
      log.warning("Class database not available.");
      error = ERR::Search;
   }

class_identified:
   if (error IS ERR::Okay) {
      if (*ClassID != CLASSID::NIL) log.detail("File belongs to class $%.8x:$%.8x", (unsigned int)(*ClassID), SubClassID ? (unsigned int)(*SubClassID) : 0);
      else {
         log.detail("Failed to identify file \"%s\"", Path);
         error = ERR::Search;
      }
   }

   if ((*ClassID) IS CLASSID::NIL) return ERR::Search;
   else return error;
}
