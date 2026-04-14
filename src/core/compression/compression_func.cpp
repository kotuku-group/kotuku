
template<class T> void wrb(T Value, APTR Target) {
   if constexpr (std::endian::native == std::endian::little) {
      ((T *)Target)[0] = Value;
   }
   else if constexpr (sizeof(T) == 2) {
      ((T *)Target)[0] = __builtin_bswap16(Value);
   }
   else ((T *)Target)[0] = __builtin_bswap32(Value);
}

//*********************************************************************************************************************

static void print(extCompression *Self, CSTRING Buffer)
{
   pf::Log log;

   if (Self->OutputID) {
      pf::ScopedObjectLock output(Self->OutputID);
      if (output.granted()) acDataFeed(*output, Self, DATA::TEXT, Buffer, strlen(Buffer) + 1);
   }
   else log.msg("%s", Buffer);
}

static void print(extCompression *Self, std::string Buffer)
{
   pf::Log log;

   if (Self->OutputID) {
      pf::ScopedObjectLock output(Self->OutputID);
      if (output.granted()) acDataFeed(*output, Self, DATA::TEXT, Buffer.c_str(), Buffer.length() + 1);
   }
   else log.msg("%s", Buffer.c_str());
}

//*********************************************************************************************************************

static ERR compress_folder(extCompression *Self, std::string Location, std::string Path)
{
   pf::Log log(__FUNCTION__);

   log.branch("Compressing folder \"%s\" to \"%s\"", Location.c_str(), Path.c_str());

   auto file = objFile::create { fl::Path(Location) };
   if (!file.ok()) return log.warning(ERR::File);

   if (((file->Flags & FL::LINK) != FL::NIL) and ((Self->Flags & CMF::NO_LINKS) IS CMF::NIL)) {
      log.msg("Folder is a link.");
      return compress_file(Self, Location, Path, true);
   }

   if (Self->OutputID) {
      std::ostringstream out;
      out << "  Compressing folder \"" << Location << "\".";
      print(Self, out.str());
   }

   // Send feedback if requested to do so

   CompressionFeedback feedback(FDB::COMPRESS_FILE, Self->FileIndex, Location.c_str(), Path.c_str());

   ERR error = send_feedback(Self, &feedback);

   Self->FileIndex++;
   if ((error IS ERR::Terminate) or (error IS ERR::Cancelled)) return ERR::Cancelled;
   else if (error IS ERR::Skip) return ERR::Okay;

   if (!Path.empty()) {
      // Seek to the position at which this new directory entry will be added

      uint32_t dataoffset = 0;
      if (!Self->Files.empty()) {
         auto &last = Self->Files.back();
         if (acSeekStart(Self->FileIO, last.Offset + HEAD_NAMELEN) != ERR::Okay) {
            return log.warning(ERR::Seek);
         }

         uint16_t namelen, extralen;
         if (fl::ReadLE(Self->FileIO, &namelen) != ERR::Okay) return ERR::Read;
         if (fl::ReadLE(Self->FileIO, &extralen) != ERR::Okay) return ERR::Read;
         dataoffset = last.Offset + HEAD_LENGTH + namelen + extralen + last.CompressedSize;
      }

      if (acSeekStart(Self->FileIO, dataoffset) != ERR::Okay) return ERR::Seek;

      // If a matching file name already exists in the archive, make a note of its position


      auto replace_file = Self->Files.begin();
      for (; replace_file != Self->Files.end(); replace_file++) {
         if (iequals(replace_file->Name, Location)) break;
      }

      // Allocate the file entry structure and set up some initial variables.

      ZipFile entry(Path);

      entry.Offset = dataoffset;

      // Convert the file date stamp into a DOS time stamp for zip

      DateTime *tm;
      if (file->get(FID_Date, tm) IS ERR::Okay) {
         if (tm->Year < 1980) entry.TimeStamp = 0x00210000;
         else entry.TimeStamp = ((tm->Year-1980)<<25) | (tm->Month<<21) | (tm->Day<<16) | (tm->Hour<<11) | (tm->Minute<<5) | (tm->Second>>1);
      }

      // Write the compression file entry

      if (acSeekStart(Self->FileIO, entry.Offset) != ERR::Okay) return ERR::Seek;

      uint8_t header[sizeof(glHeader)];
      copymem(glHeader, header, sizeof(glHeader));

      wrb<uint16_t>(entry.DeflateMethod, header + HEAD_DEFLATEMETHOD);
      wrb<uint32_t>(entry.TimeStamp, header + HEAD_TIMESTAMP);
      wrb<uint32_t>(entry.CRC, header + HEAD_CRC);
      wrb<uint32_t>(entry.CompressedSize, header + HEAD_COMPRESSEDSIZE);
      wrb<uint32_t>(entry.OriginalSize, header + HEAD_FILESIZE);
      wrb<uint16_t>(entry.Name.size(), header + HEAD_NAMELEN);
      if (acWriteResult(Self->FileIO, header, HEAD_LENGTH) != HEAD_LENGTH) return ERR::Okay;
      if (acWriteResult(Self->FileIO, entry.Name.c_str(), entry.Name.size()) != (int)entry.Name.size()) return ERR::Okay;

      Self->Files.push_back(entry);

      // If this new data replaces an existing directory, remove the old directory now

      if (replace_file != Self->Files.end()) remove_file(Self, replace_file);

      Self->CompressionCount++;
   }

   // Enter the directory and compress its contents

   DirInfo *dir;
   if (OpenDir(Location.c_str(), RDF::FILE|RDF::FOLDER|RDF::QUALIFY, &dir) IS ERR::Okay) {
      while (ScanDir(dir) IS ERR::Okay) { // Recurse for each directory in the list
         FileInfo *scan = dir->Info;
         if (((scan->Flags & RDF::FOLDER) != RDF::NIL) and ((scan->Flags & RDF::LINK) IS RDF::NIL)) {
            std::string location = Location + scan->Name;
            std::string path = Path + scan->Name;
            compress_folder(Self, location, path);
         }
         else if ((scan->Flags & (RDF::FILE|RDF::LINK)) != RDF::NIL) {
            std::string location = Location + scan->Name;
            compress_file(Self, location, Path, ((scan->Flags & RDF::LINK) != RDF::NIL) ? true : false);
         }
      }

      FreeResource(dir);
   }

   return ERR::Okay;
}

//*********************************************************************************************************************

static ERR compress_file(extCompression *Self, std::string Location, std::string Path, bool Link)
{
   pf::Log log(__FUNCTION__);

   log.branch("Compressing file \"%s\" to \"%s\"", Location.c_str(), Path.c_str());

   CSTRING symlink = nullptr;
   bool deflateend = false;
   uint32_t dataoffset = 0;
   std::string filename;
   std::list<ZipFile>::iterator file_index;
   int i, len;

   int16_t level = Self->CompressionLevel / 10;
   if (level < 0) level = 0;
   else if (level > 9) level = 9;

   auto defer = pf::Defer([Self, deflateend] {
      if (deflateend) deflateEnd(&Self->Zip);
      Self->FileIndex++;
   });

   // Open the source file for reading only

   auto file = objFile::create { fl::Path(Location), fl::Flags(Link ? FL::NIL : FL::READ) };

   if (!file.ok()) {
      if (Self->OutputID) {
         std::ostringstream out;
         out << "  Error opening file \"" << Location << "\".";
         print(Self, out.str());
      }
      return log.warning(ERR::OpenFile);
   }


   if ((Link) and ((file->Flags & FL::LINK) IS FL::NIL)) {
      log.warning("Internal Error: Expected a link, but the file is not.");
      return ERR::SanityCheckFailed;
   }

   // Determine the name that will be used for storing this file

   i = Location.size();
   if ((Location.back() IS '/') or (Location.back() IS '\\')) i--; // Ignore trailing slashes for symbolically linked folders
   while ((i > 0) and (Location[i-1] != ':') and (Location[i-1] != '/') and (Location[i-1] != '\\')) i--;

   if (!Path.empty()) {
      filename.assign(Path);
      filename.append(Location, i);
   }
   else filename.assign(Location, i);

   if ((Link) and (filename.back() IS '/')) filename.pop_back();

   // Send feedback

   CompressionFeedback fb(FDB::COMPRESS_FILE, Self->FileIndex, Location.c_str(), filename.c_str());
   file->get(FID_Size, fb.OriginalSize);

   switch (send_feedback(Self, &fb)) {
      case ERR::Terminate:
      case ERR::Cancelled:
         return ERR::Cancelled;
      case ERR::Skip:
         return ERR::Okay;
      default:
         break;
   }

   // Send informative output to the user

   if (Self->OutputID) {
      std::ostringstream out;
      out << "  Compressing file \"" << Location << "\".";
      print(Self, out.str());
   }

   // Seek to the position at which this new file will be added

   if (!Self->Files.empty()) {
      auto &chain = Self->Files.back();

      if (acSeek(Self->FileIO, chain.Offset + HEAD_NAMELEN, SEEK::START) != ERR::Okay) return log.warning(ERR::Seek);

      uint16_t namelen, extralen;
      if (fl::ReadLE(Self->FileIO, &namelen) != ERR::Okay) return ERR::Read;
      if (fl::ReadLE(Self->FileIO, &extralen) != ERR::Okay) return ERR::Read;
      dataoffset = chain.Offset + HEAD_LENGTH + namelen + extralen + chain.CompressedSize;
   }

   if (acSeek(Self->FileIO, dataoffset, SEEK::START) != ERR::Okay) return ERR::Seek;

   // Initialise the compression algorithm

   Self->CompressionCount++;

   Self->Zip.next_in   = 0;
   Self->Zip.avail_in  = 0;
   Self->Zip.next_out  = 0;
   Self->Zip.avail_out = 0;
   Self->Zip.total_in  = 0;
   Self->Zip.total_out = 0;

   if (deflateInit2(&Self->Zip, level, Z_DEFLATED, -MAX_WBITS, ZLIB_MEM_LEVEL, Z_DEFAULT_STRATEGY) IS Z_OK) {
      deflateend = true;
      Self->Zip.next_out  = Self->Output;
      Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;
   }
   else return ERR::InvalidData;

   // If a matching file name already exists in the archive, make a note of its position

   auto replace_file = Self->Files.begin();
   for (; replace_file != Self->Files.end(); replace_file++) {
      if (iequals(replace_file->Name, filename)) break;
   }

   // Allocate the file entry structure and set up some initial variables.

   ZipFile entry(filename);

   entry.Offset = dataoffset;

   if (((Self->Flags & CMF::NO_LINKS) IS CMF::NIL) and ((file->Flags & FL::LINK) != FL::NIL)) {
      if (file->get(FID_Link, symlink) IS ERR::Okay) {
         log.msg("Note: File \"%s\" is a symbolic link to \"%s\"", filename.c_str(), symlink);
         entry.Flags |= ZIP_LINK;
      }
   }

   // Convert the file date stamp into a DOS time stamp for zip

   DateTime *time;
   if (file->get(FID_Date, time) IS ERR::Okay) {
      if (time->Year < 1980) entry.TimeStamp = 0x00210000;
      else entry.TimeStamp = ((time->Year-1980)<<25) | (time->Month<<21) | (time->Day<<16) | (time->Hour<<11) | (time->Minute<<5) | (time->Second>>1);
   }

   PERMIT permissions;
   if (file->get(FID_Permissions, (int &)permissions) IS ERR::Okay) {
      if ((permissions & PERMIT::USER_READ) != PERMIT::NIL)   entry.Flags |= ZIP_UREAD;
      if ((permissions & PERMIT::GROUP_READ) != PERMIT::NIL)  entry.Flags |= ZIP_GREAD;
      if ((permissions & PERMIT::OTHERS_READ) != PERMIT::NIL) entry.Flags |= ZIP_OREAD;

      if ((permissions & PERMIT::USER_WRITE) != PERMIT::NIL)   entry.Flags |= ZIP_UWRITE;
      if ((permissions & PERMIT::GROUP_WRITE) != PERMIT::NIL)  entry.Flags |= ZIP_GWRITE;
      if ((permissions & PERMIT::OTHERS_WRITE) != PERMIT::NIL) entry.Flags |= ZIP_OWRITE;

      if ((permissions & PERMIT::USER_EXEC) != PERMIT::NIL)   entry.Flags |= ZIP_UEXEC;
      if ((permissions & PERMIT::GROUP_EXEC) != PERMIT::NIL)  entry.Flags |= ZIP_GEXEC;
      if ((permissions & PERMIT::OTHERS_EXEC) != PERMIT::NIL) entry.Flags |= ZIP_OEXEC;
   }

   entry.Flags &= 0xffffff00; // Do not write anything to the low order bits, they have meaning exclusive to MSDOS

   // Skip over the PKZIP header that will be written for this file (we will be updating the header later).

   if (acWriteResult(Self->FileIO, nullptr, HEAD_LENGTH + entry.Name.size() + entry.Comment.size()) != int(HEAD_LENGTH + entry.Name.size() + entry.Comment.size())) return ERR::Write;

   // Specify the limitations of our buffer so that the compression routine doesn't overwrite its boundaries.  Then
   // start the compression of the input file.

   if (entry.Flags & ZIP_LINK) {
      // Compress the symbolic link to the zip file, rather than the data
      len = strlen(symlink);
      Self->Zip.next_in   = (Bytef *)symlink;
      Self->Zip.avail_in  = len;
      Self->Zip.next_out  = Self->Output;
      Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;
      if (deflate(&Self->Zip, Z_NO_FLUSH) != Z_OK) {
         log.warning("Failure during data compression.");
         return ERR::Compression;
      }
      entry.CRC = GenCRC32(entry.CRC, (APTR)symlink, len);
   }
   else {
      struct acRead read = { .Buffer = Self->Input, .Length = SIZE_COMPRESSION_BUFFER };
      while ((Action(AC::Read, *file, &read) IS ERR::Okay) and (read.Result > 0)) {
         Self->Zip.next_in  = Self->Input;
         Self->Zip.avail_in = read.Result;

         while (Self->Zip.avail_in) {
            if (!Self->Zip.avail_out) {
               // Write out the compression buffer because it is at capacity
               struct acWrite write = { .Buffer = Self->Output, .Length = SIZE_COMPRESSION_BUFFER };
               Action(AC::Write, Self->FileIO, &write);

               // Reset the compression buffer
               Self->Zip.next_out  = Self->Output;
               Self->Zip.avail_out = SIZE_COMPRESSION_BUFFER;

               fb.CompressedSize = Self->Zip.total_out;
               fb.Progress       = Self->Zip.total_in; //fb.CompressedSize * 100 / fb.OriginalSize;
               send_feedback(Self, &fb);
            }

            if (deflate(&Self->Zip, Z_NO_FLUSH) != Z_OK) {
               log.warning("Failure during data compression.");
               return ERR::Compression;
            }
         }

         entry.CRC = GenCRC32(entry.CRC, Self->Input, read.Result);
      }
   }

   if (acFlush(Self) != ERR::Okay) return ERR::Compression;
   deflateEnd(&Self->Zip);
   deflateend = false;

   // Finalise entry details

   entry.CompressedSize = Self->Zip.total_out;
   entry.OriginalSize   = Self->Zip.total_in;

   if (entry.OriginalSize > 0) entry.DeflateMethod = 8;
   else {
      entry.DeflateMethod = 0;
      entry.CompressedSize = 0;
   }

   Self->Files.push_back(entry);

   // Update the header that we earlier wrote for our file entry.  Note that the header stores only some of the file's
   // meta information.  The majority is stored in the directory at the end of the zip file.

   if (acSeek(Self->FileIO, (double)entry.Offset, SEEK::START) != ERR::Okay) return ERR::Seek;

   uint8_t header[sizeof(glHeader)];
   copymem(glHeader, header, sizeof(glHeader));
   wrb<uint16_t>(entry.DeflateMethod, header + HEAD_DEFLATEMETHOD);
   wrb<uint32_t>(entry.TimeStamp, header + HEAD_TIMESTAMP);
   wrb<uint32_t>(entry.CRC, header + HEAD_CRC);
   wrb<uint32_t>(entry.CompressedSize, header + HEAD_COMPRESSEDSIZE);
   wrb<uint32_t>(entry.OriginalSize, header + HEAD_FILESIZE);
   wrb<uint16_t>(entry.Name.size(), header + HEAD_NAMELEN);
   if (acWriteResult(Self->FileIO, header, HEAD_LENGTH) != HEAD_LENGTH) return ERR::Write;
   if (acWriteResult(Self->FileIO, entry.Name.c_str(), entry.Name.size()) != (int)entry.Name.size()) return ERR::Write;

   // Send updated feedback if necessary

   if (fb.Progress < fb.OriginalSize) {
      fb.CompressedSize = entry.CompressedSize;
      fb.Progress       = fb.OriginalSize;
      send_feedback(Self, &fb);
   }

   // If this new data replaces an existing file, remove the old file now

   if (replace_file != Self->Files.end()) remove_file(Self, replace_file);

   return ERR::Okay;
}

//*********************************************************************************************************************

static ERR remove_file(extCompression *Self, std::list<ZipFile>::iterator &File)
{
   pf::Log log(__FUNCTION__);

   log.branch("Deleting \"%s\"", File->Name.c_str());

   // Seek to the end of the compressed file.  We are going to delete the file by shifting all the data after the file
   // to the start of the file's position.

   if (acSeekStart(Self->FileIO, File->Offset + HEAD_NAMELEN) != ERR::Okay) return log.warning(ERR::Seek);

   uint16_t namelen, extralen;
   if (fl::ReadLE(Self->FileIO, &namelen) != ERR::Okay) return ERR::Read;
   if (fl::ReadLE(Self->FileIO, &extralen) != ERR::Okay) return ERR::Read;
   int chunksize  = HEAD_LENGTH + namelen + extralen + File->CompressedSize;
   double currentpos = File->Offset + chunksize;
   if (acSeekStart(Self->FileIO, currentpos) != ERR::Okay) return log.warning(ERR::Seek);

   double writepos = File->Offset;

   struct acRead read = { Self->Input, SIZE_COMPRESSION_BUFFER };
   while ((Action(AC::Read, Self->FileIO, &read) IS ERR::Okay) and (read.Result > 0)) {
      if (acSeekStart(Self->FileIO, writepos) != ERR::Okay) return log.warning(ERR::Seek);
      struct acWrite write = { Self->Input, read.Result };
      if (Action(AC::Write, Self->FileIO, &write) != ERR::Okay) return log.warning(ERR::Write);
      writepos += write.Result;

      currentpos += read.Result;
      if (acSeekStart(Self->FileIO, currentpos) != ERR::Okay) return log.warning(ERR::Seek);
   }

   Self->FileIO->set(FID_Size, writepos);

   // Adjust the offset of files that are ahead of this one

   auto scan = File;
   for (++scan; scan != Self->Files.end(); scan++) scan->Offset -= chunksize;

   File = Self->Files.erase(File); // Remove the file reference from the chain
   return ERR::Okay;
}

//*********************************************************************************************************************
// Scans a zip file and adds file entries to the Compression object.
//
// This technique goes to the end of the zip file and reads the file entries from a huge table.  This is very fast, but
// if the zip file is damaged or partially downloaded, it will fail.  In the event that the directory is unavailable,
// the function will fallback to scan_zip().

static ERR fast_scan_zip(extCompression *Self)
{
   pf::Log log(__FUNCTION__);
   ziptail tail;

   log.traceBranch();

   if (acSeek(Self->FileIO, TAIL_LENGTH, SEEK::END) != ERR::Okay) return ERR::Seek; // Surface error, fail
   if (acRead(Self->FileIO, &tail, TAIL_LENGTH, nullptr) != ERR::Okay) return ERR::Read; // Surface error, fail

   if (0x06054b50 != ((uint32_t *)&tail)[0]) {
      // Tail not available, use the slow scanner instead
      return scan_zip(Self);
   }

#ifndef REVERSE_BYTEORDER
   tail.filecount  = le16_cpu(tail.filecount);
   tail.listsize   = le32_cpu(tail.listsize);
   tail.listoffset = le32_cpu(tail.listoffset);
#endif

   if (acSeek(Self->FileIO, tail.listoffset, SEEK::START) != ERR::Okay) return ERR::Seek;

   zipentry *list, *scan;
   int total_files = 0;
   if (AllocMemory(tail.listsize, MEM::DATA|MEM::NO_CLEAR, (APTR *)&list, nullptr) IS ERR::Okay) {
      log.trace("Reading end-of-central directory from index %d, %d bytes.", tail.listoffset, tail.listsize);
      if (acRead(Self->FileIO, list, tail.listsize, nullptr) != ERR::Okay) {
         FreeResource(list);
         return scan_zip(Self);
      }

      #pragma GCC diagnostic ignored "-Waddress-of-packed-member"
      auto head = (uint32_t *)list;
      #pragma GCC diagnostic warning "-Waddress-of-packed-member"

      for (int i=0; i < tail.filecount; i++) {
         if (0x02014b50 != head[0]) {
            log.warning("Zip file has corrupt end-of-file signature.");
            Self->Files.clear();
            FreeResource(list);
            return scan_zip(Self);
         }

         scan = (zipentry *)(head + 1);

#ifndef REVERSE_BYTEORDER
         scan->deflatemethod  = le16_cpu(scan->deflatemethod);
         scan->timestamp      = le32_cpu(scan->timestamp);
         scan->crc32          = le32_cpu(scan->crc32);
         scan->compressedsize = le32_cpu(scan->compressedsize);
         scan->originalsize   = le32_cpu(scan->originalsize);
         scan->namelen        = le16_cpu(scan->namelen);
         scan->extralen       = le16_cpu(scan->extralen);
         scan->commentlen     = le16_cpu(scan->commentlen);
         scan->diskno         = le16_cpu(scan->diskno);
         scan->ifile          = le16_cpu(scan->ifile);
         scan->attrib         = le32_cpu(scan->attrib);
         scan->offset         = le32_cpu(scan->offset);
#endif

         total_files++;

         ZipFile zf;

         zf.NameLen        = scan->namelen;
         zf.CommentLen     = scan->commentlen;
         zf.CompressedSize = scan->compressedsize;
         zf.OriginalSize   = scan->originalsize;
         zf.DeflateMethod  = scan->deflatemethod;
         zf.TimeStamp      = scan->timestamp;
         zf.CRC            = scan->crc32;
         zf.Offset         = scan->offset;

         if (scan->ostype IS ZIP_KOTUKU) zf.Flags = scan->attrib;
         else zf.Flags = 0;

         // Read string information

         STRING str = STRING(head) + LIST_LENGTH;
         if ((str[0] IS '.') and (str[1] IS '/')) { // Get rid of any useless './' prefix that sometimes make their way into zip files
            zf.Name.assign(str+2, scan->namelen-2);
         }
         else zf.Name.assign(str, scan->namelen);

         zf.Comment.assign(str + scan->namelen + scan->extralen, scan->commentlen);

         if (zf.Flags & ZIP_LINK);
         else if ((!zf.OriginalSize) and (zf.Name.back() IS '/')) zf.IsFolder = true;

         Self->Files.push_back(zf);

         head = (uint32_t *)(((uint8_t *)head) + LIST_LENGTH + scan->commentlen + scan->namelen + scan->extralen);
      }
   }

   FreeResource(list);
   return ERR::Okay;
}

//*********************************************************************************************************************
// Scans a zip file and adds file entries to the Compression object.

static ERR scan_zip(extCompression *Self)
{
   pf::Log log(__FUNCTION__);

   log.traceBranch();

   if (acSeek(Self->FileIO, 0.0, SEEK::START) != ERR::Okay) return log.warning(ERR::Seek);

   int type, result;
   int total_files = 0;
   while (fl::ReadLE(Self->FileIO, &type) IS ERR::Okay) {
      if (type IS 0x04034b50) {
         // PKZIP file header entry detected

         if (acSeek(Self->FileIO, (double)HEAD_COMPRESSEDSIZE-4, SEEK::CURRENT) != ERR::Okay) return log.warning(ERR::Seek);

         struct {
            uint32_t compressedsize;
            uint32_t originalsize;
            uint16_t namelen;
            uint16_t extralen;
         } header;

         if (acRead(Self->FileIO, &header, sizeof(header), &result) != ERR::Okay) return log.warning(ERR::Read);

#ifndef REVERSE_BYTEORDER
         header.compressedsize = le32_cpu(header.compressedsize);
         header.originalsize   = le32_cpu(header.originalsize);
         header.namelen        = le16_cpu(header.namelen);
         header.extralen       = le16_cpu(header.extralen);
#endif

         if (acSeekCurrent(Self->FileIO, header.compressedsize + header.namelen + header.extralen) != ERR::Okay) return log.warning(ERR::Seek);
      }
      else if (type IS 0x02014b50) {
         // PKZIP list entry detected

         total_files++;

         zipentry zipentry;
         if (acRead(Self->FileIO, &zipentry, sizeof(zipentry), &result) != ERR::Okay) return log.warning(ERR::Read);

#ifndef REVERSE_BYTEORDER
         zipentry.deflatemethod  = le16_cpu(zipentry.deflatemethod);
         zipentry.timestamp      = le32_cpu(zipentry.timestamp);
         zipentry.crc32          = le32_cpu(zipentry.crc32);
         zipentry.compressedsize = le32_cpu(zipentry.compressedsize);
         zipentry.originalsize   = le32_cpu(zipentry.originalsize);
         zipentry.namelen        = le16_cpu(zipentry.namelen);
         zipentry.extralen       = le16_cpu(zipentry.extralen);
         zipentry.commentlen     = le16_cpu(zipentry.commentlen);
         zipentry.diskno         = le16_cpu(zipentry.diskno);
         zipentry.ifile          = le16_cpu(zipentry.ifile);
         zipentry.attrib         = le32_cpu(zipentry.attrib);
         zipentry.offset         = le32_cpu(zipentry.offset);
#endif

         ZipFile entry;

         entry.Name.resize(zipentry.namelen);
         if (acRead(Self->FileIO, (APTR)entry.Name.c_str(), zipentry.namelen, &result) != ERR::Okay) return log.warning(ERR::Read);

         if (zipentry.extralen > 0) { // Not currently used
            std::string extra(zipentry.extralen, '\0');
            struct acRead read = { (APTR)extra.c_str(), zipentry.extralen };
            if (Action(AC::Read, Self->FileIO, &read) != ERR::Okay) return log.warning(ERR::Read);
         }

         if (zipentry.commentlen > 0) {
            entry.Comment.resize(zipentry.commentlen);
            if (acRead(Self->FileIO, (APTR)entry.Comment.c_str(), zipentry.commentlen, &result) != ERR::Okay) return log.warning(ERR::Read);
         }

         entry.NameLen        = zipentry.namelen;
         entry.CommentLen     = zipentry.commentlen;
         entry.CompressedSize = zipentry.compressedsize;
         entry.OriginalSize   = zipentry.originalsize;
         entry.DeflateMethod  = zipentry.deflatemethod;
         entry.TimeStamp      = zipentry.timestamp;
         entry.CRC            = zipentry.crc32;
         entry.Offset         = zipentry.offset;

         if (zipentry.ostype IS ZIP_KOTUKU) entry.Flags = zipentry.attrib;
         else entry.Flags = 0;

         // Get rid of any useless './' prefix that sometimes make their way into zip files

         if ((entry.Name[0] IS '.') and (entry.Name[1] IS '/')) {
            entry.Name.erase(0, 2);
         }

         if (entry.Flags & ZIP_LINK);
         else if ((!entry.OriginalSize) and (entry.Name.back() IS '/')) entry.IsFolder = true;

         Self->Files.push_back(entry);
      }
      else if (type IS 0x06054b50) {
         // PKZIP end of file directory signature detected
         log.trace("End of central directory signature detected.");
         break; // End the loop
      }
      else {
         // Unrecognised PKZIP data
         log.warning("Unrecognised PKZIP entry $%.8x in the central directory.", type);
         return ERR::InvalidData;
      }
   }

   log.trace("Detected %d files.", total_files);
   return ERR::Okay;
}

//*********************************************************************************************************************

static ERR send_feedback(extCompression *Self, CompressionFeedback *Feedback)
{
   pf::Log log(__FUNCTION__);
   ERR error;

   if (!Self->Feedback.defined()) return ERR::Okay;

   if (Self->Feedback.isC()) {
      auto routine = (ERR (*)(extCompression *, CompressionFeedback *, APTR Meta))Self->Feedback.Routine;
      pf::SwitchContext context(Self->Feedback.Context);
      error = routine(Self, Feedback, Self->Feedback.Meta);
   }
   else if (Self->Feedback.isScript()) {
      if (sc::Call(Self->Feedback, std::to_array<ScriptArg>({
         { "Compression", Self, FD_OBJECTPTR },
         { "CompressionFeedback:Feedback", Feedback, FD_POINTER|FD_STRUCT }
      }), error) != ERR::Okay) error = ERR::Function;
   }
   else {
      log.warning("Callback function structure does not specify a recognised Type.");
      error = ERR::Terminate;
   }

   return error;
}

//*********************************************************************************************************************

static void write_eof(extCompression *Self)
{
   if ((Self->FileIO) and (!Self->isSubClass()) and (Self->CompressionCount > 0)) {
      if (!Self->Files.empty()) {
         // Calculate the start of the list offset

         auto &last = Self->Files.back();
         acSeekStart(Self->FileIO, last.Offset + HEAD_NAMELEN);

         uint16_t namelen, extralen;
         if (fl::ReadLE(Self->FileIO, &namelen) != ERR::Okay) return;
         if (fl::ReadLE(Self->FileIO, &extralen) != ERR::Okay) return;
         acSeekCurrent(Self->FileIO, last.CompressedSize + namelen + extralen);
         uint32_t listoffset = last.Offset + last.CompressedSize + namelen + extralen + HEAD_LENGTH;

         // Write out the central directory

         uint32_t listsize  = 0;
         uint16_t filecount = 0;
         for (auto &chain : Self->Files) {
            uint8_t elist[sizeof(glList)];
            copymem(glList, elist, sizeof(glList));

            wrb<uint16_t>(chain.DeflateMethod, elist+LIST_METHOD);
            wrb<uint32_t>(chain.TimeStamp, elist+LIST_TIMESTAMP);
            wrb<uint32_t>(chain.CRC, elist+LIST_CRC);
            wrb<uint32_t>(chain.CompressedSize, elist+LIST_COMPRESSEDSIZE);
            wrb<uint32_t>(chain.OriginalSize, elist+LIST_FILESIZE);
            wrb<uint16_t>(chain.Name.size(), elist+LIST_NAMELEN);
            wrb<uint16_t>(0, elist+LIST_EXTRALEN);
            wrb<uint16_t>(chain.Comment.size(), elist+LIST_COMMENTLEN);
            wrb<uint16_t>(0, elist+LIST_DISKNO);
            wrb<uint16_t>(0, elist+LIST_IFILE);
            wrb<uint32_t>(chain.Flags, elist+LIST_ATTRIB);
            wrb<uint32_t>(chain.Offset, elist+LIST_OFFSET);

            acWriteResult(Self->FileIO, elist, LIST_LENGTH);

            acWriteResult(Self->FileIO, chain.Name.c_str(), chain.Name.size());
            if (!chain.Comment.empty()) acWriteResult(Self->FileIO, chain.Comment.c_str(), chain.Comment.size());

            listsize += LIST_LENGTH + chain.Name.size() + chain.Comment.size();
            filecount++;
         }

         uint8_t tail[sizeof(glTail)];
         copymem(glTail, tail, sizeof(glTail));

         wrb<uint16_t>(filecount,  tail + TAIL_FILECOUNT); // File count for this file
         wrb<uint16_t>(filecount,  tail + TAIL_TOTALFILECOUNT); // File count for all zip files when spanning multiple archives
         wrb<uint32_t>(listsize,   tail + TAIL_FILELISTSIZE);
         wrb<uint32_t>(listoffset, tail + TAIL_FILELISTOFFSET);
         acWriteResult(Self->FileIO, tail, TAIL_LENGTH);
      }
      else Self->FileIO->set(FID_Size, 0);

      Self->CompressionCount = 0;
   }
}

//*********************************************************************************************************************

void zipfile_to_item(ZipFile &ZF, CompressedItem &Item)
{
   clearmem(&Item, sizeof(Item));

   Item.Modified.Year   = 1980 + ((ZF.TimeStamp>>25) & 0x3f);
   Item.Modified.Month  = (ZF.TimeStamp>>21) & 0x0f;
   Item.Modified.Day    = (ZF.TimeStamp>>16) & 0x1f;
   Item.Modified.Hour   = (ZF.TimeStamp>>11) & 0x1f;
   Item.Modified.Minute = (ZF.TimeStamp>>5)  & 0x3f;
   Item.Modified.Second = (ZF.TimeStamp>>1)  & 0x0f;
   Item.Path            = ZF.Name.c_str();
   Item.OriginalSize    = ZF.OriginalSize;
   Item.CompressedSize  = ZF.CompressedSize;

   if (ZF.Flags & ZIP_LINK) Item.Flags |= FL::LINK;
   else {
      if (!Item.OriginalSize) {
         if (ZF.Name.back() IS '/') Item.Flags |= FL::FOLDER;
         else Item.Flags |= FL::FOLDER;
      }
      else Item.Flags |= FL::FILE;
   }

   if (ZF.Flags & ZIP_SECURITY) {
      auto permissions = PERMIT::NIL;
      if (ZF.Flags & ZIP_UEXEC) permissions |= PERMIT::USER_EXEC;
      if (ZF.Flags & ZIP_GEXEC) permissions |= PERMIT::GROUP_EXEC;
      if (ZF.Flags & ZIP_OEXEC) permissions |= PERMIT::OTHERS_EXEC;

      if (ZF.Flags & ZIP_UREAD) permissions |= PERMIT::USER_READ;
      if (ZF.Flags & ZIP_GREAD) permissions |= PERMIT::GROUP_READ;
      if (ZF.Flags & ZIP_OREAD) permissions |= PERMIT::OTHERS_READ;

      if (ZF.Flags & ZIP_UWRITE) permissions |= PERMIT::USER_WRITE;
      if (ZF.Flags & ZIP_GWRITE) permissions |= PERMIT::GROUP_WRITE;
      if (ZF.Flags & ZIP_OWRITE) permissions |= PERMIT::OTHERS_WRITE;

      Item.Permissions = permissions;
   }
}

