// Support for reading metadata from Windows executables.

using TaskMetadataFields = ankerl::unordered_dense::map<std::string, std::string, CaseInsensitiveHash,
   CaseInsensitiveEqual>;

struct TaskVersionMetadata {
   TaskMetadataFields Fields;
   bool FixedFileVersion = false;
   bool FixedProductVersion = false;
   uint32_t FileVersionMS = 0;
   uint32_t FileVersionLS = 0;
   uint32_t ProductVersionMS = 0;
   uint32_t ProductVersionLS = 0;
};

struct VersionInfoBlock {
   size_t Start = 0;
   size_t End = 0;
   size_t ValueOffset = 0;
   size_t ValueSize = 0;
   size_t ChildrenOffset = 0;
   uint16_t ValueLength = 0;
   uint16_t Type = 0;
   std::string Key;
};

static size_t align_task_version(size_t Value)
{
   return (Value + 3) & ~size_t(3);
}

static bool read_u16_le(const uint8_t *Data, size_t Size, size_t Offset, uint16_t *Value)
{
   if ((not Value) or (Offset + 2 > Size)) return false;
   *Value = uint16_t(Data[Offset]) | (uint16_t(Data[Offset + 1]) << 8);
   return true;
}

static bool read_u32_le(const uint8_t *Data, size_t Size, size_t Offset, uint32_t *Value)
{
   if ((not Value) or (Offset + 4 > Size)) return false;
   *Value = uint32_t(Data[Offset]) | (uint32_t(Data[Offset + 1]) << 8) |
      (uint32_t(Data[Offset + 2]) << 16) | (uint32_t(Data[Offset + 3]) << 24);
   return true;
}

static void append_utf8(std::string &Output, uint32_t Codepoint)
{
   if (Codepoint <= 0x7f) Output.push_back(char(Codepoint));
   else if (Codepoint <= 0x7ff) {
      Output.push_back(char(0xc0 | (Codepoint >> 6)));
      Output.push_back(char(0x80 | (Codepoint & 0x3f)));
   }
   else if (Codepoint <= 0xffff) {
      Output.push_back(char(0xe0 | (Codepoint >> 12)));
      Output.push_back(char(0x80 | ((Codepoint >> 6) & 0x3f)));
      Output.push_back(char(0x80 | (Codepoint & 0x3f)));
   }
   else {
      Output.push_back(char(0xf0 | (Codepoint >> 18)));
      Output.push_back(char(0x80 | ((Codepoint >> 12) & 0x3f)));
      Output.push_back(char(0x80 | ((Codepoint >> 6) & 0x3f)));
      Output.push_back(char(0x80 | (Codepoint & 0x3f)));
   }
}

static std::string read_utf16le_string(const uint8_t *Data, size_t Size, size_t Offset, size_t MaxChars,
   size_t *NextOffset)
{
   std::string result;
   size_t cursor = Offset;
   size_t count = 0;

   while ((cursor + 2 <= Size) and (count < MaxChars)) {
      uint16_t code;
      if (not read_u16_le(Data, Size, cursor, &code)) break;
      cursor += 2;
      count++;
      if (code IS 0) break;

      if ((code >= 0xd800) and (code <= 0xdbff) and (cursor + 2 <= Size) and (count < MaxChars)) {
         uint16_t next;
         if (read_u16_le(Data, Size, cursor, &next) and (next >= 0xdc00) and (next <= 0xdfff)) {
            cursor += 2;
            count++;
            append_utf8(result, 0x10000 + ((uint32_t(code - 0xd800) << 10) | uint32_t(next - 0xdc00)));
         }
         else append_utf8(result, code);
      }
      else append_utf8(result, code);
   }

   if (NextOffset) *NextOffset = cursor;
   return result;
}

static bool read_version_block(const uint8_t *Data, size_t Size, size_t Offset, VersionInfoBlock *Block)
{
   if ((not Block) or (Offset + 6 > Size)) return false;

   uint16_t length;
   if (not read_u16_le(Data, Size, Offset, &length)) return false;
   if ((length < 6) or (Offset + length > Size)) return false;

   Block->Start = Offset;
   Block->End = Offset + length;
   if (not read_u16_le(Data, Size, Offset + 2, &Block->ValueLength)) return false;
   if (not read_u16_le(Data, Size, Offset + 4, &Block->Type)) return false;

   size_t cursor;
   Block->Key = read_utf16le_string(Data, Size, Offset + 6, (Block->End - Offset - 6) / 2, &cursor);
   if (cursor > Block->End) return false;

   Block->ValueOffset = align_task_version(cursor);
   if (Block->ValueOffset > Block->End) return false;

   if (Block->Type IS 1) Block->ValueSize = size_t(Block->ValueLength) * 2;
   else Block->ValueSize = Block->ValueLength;

   auto value_end = Block->ValueOffset + Block->ValueSize;
   if (value_end > Block->End) return false;

   Block->ChildrenOffset = align_task_version(value_end);
   if (Block->ChildrenOffset > Block->End) Block->ChildrenOffset = Block->End;

   return true;
}

static std::string version_to_string(uint32_t VersionMS, uint32_t VersionLS)
{
   std::ostringstream out;
   out << ((VersionMS >> 16) & 0xffff) << '.' << (VersionMS & 0xffff) << '.'
      << ((VersionLS >> 16) & 0xffff) << '.' << (VersionLS & 0xffff);
   return out.str();
}

static void add_metadata_field(TaskVersionMetadata &Metadata, const std::string &Key, std::string Value)
{
   while ((not Value.empty()) and (Value.back() IS 0)) Value.pop_back();
   if (Value.empty()) return;
   if (Metadata.Fields.find(Key) IS Metadata.Fields.end()) Metadata.Fields[Key] = Value;
}

static void parse_string_table(const uint8_t *Data, const VersionInfoBlock &Table, TaskVersionMetadata &Metadata)
{
   for (size_t cursor = Table.ChildrenOffset; cursor + 6 <= Table.End; ) {
      VersionInfoBlock item;
      if (not read_version_block(Data, Table.End, cursor, &item)) break;

      if ((not item.Key.empty()) and (item.Type IS 1) and (item.ValueLength > 0)) {
         auto value = read_utf16le_string(Data, item.ValueOffset + item.ValueSize, item.ValueOffset,
            item.ValueLength, nullptr);
         add_metadata_field(Metadata, item.Key, value);
      }

      auto next = align_task_version(item.End);
      if (next <= cursor) break;
      cursor = next;
   }
}

static void parse_string_file_info(const uint8_t *Data, const VersionInfoBlock &Info, TaskVersionMetadata &Metadata)
{
   for (size_t cursor = Info.ChildrenOffset; cursor + 6 <= Info.End; ) {
      VersionInfoBlock table;
      if (not read_version_block(Data, Info.End, cursor, &table)) break;
      parse_string_table(Data, table, Metadata);

      auto next = align_task_version(table.End);
      if (next <= cursor) break;
      cursor = next;
   }
}

static bool parse_version_info(const uint8_t *Data, size_t Size, TaskVersionMetadata &Metadata)
{
   VersionInfoBlock root;
   if (not read_version_block(Data, Size, 0, &root)) return false;
   if (root.Key != "VS_VERSION_INFO") return false;

   uint32_t signature;
   if ((root.ValueSize >= 52) and read_u32_le(Data, Size, root.ValueOffset, &signature) and
      (signature IS 0xfeef04bd)) {
      read_u32_le(Data, Size, root.ValueOffset + 8, &Metadata.FileVersionMS);
      read_u32_le(Data, Size, root.ValueOffset + 12, &Metadata.FileVersionLS);
      read_u32_le(Data, Size, root.ValueOffset + 16, &Metadata.ProductVersionMS);
      read_u32_le(Data, Size, root.ValueOffset + 20, &Metadata.ProductVersionLS);
      Metadata.FixedFileVersion = true;
      Metadata.FixedProductVersion = true;
   }

   for (size_t cursor = root.ChildrenOffset; cursor + 6 <= root.End; ) {
      VersionInfoBlock child;
      if (not read_version_block(Data, root.End, cursor, &child)) break;
      if (child.Key IS "StringFileInfo") parse_string_file_info(Data, child, Metadata);

      auto next = align_task_version(child.End);
      if (next <= cursor) break;
      cursor = next;
   }

   if (Metadata.FixedFileVersion) {
      auto value = version_to_string(Metadata.FileVersionMS, Metadata.FileVersionLS);
      Metadata.Fields["FixedFileVersion"] = value;
      if (Metadata.Fields.find("FileVersion") IS Metadata.Fields.end()) Metadata.Fields["FileVersion"] = value;
   }

   if (Metadata.FixedProductVersion) {
      auto value = version_to_string(Metadata.ProductVersionMS, Metadata.ProductVersionLS);
      Metadata.Fields["FixedProductVersion"] = value;
      if (Metadata.Fields.find("ProductVersion") IS Metadata.Fields.end()) Metadata.Fields["ProductVersion"] = value;
   }

   return (not Metadata.Fields.empty()) or Metadata.FixedFileVersion or Metadata.FixedProductVersion;
}

struct PeSection {
   uint32_t VirtualAddress = 0;
   uint32_t VirtualSize = 0;
   uint32_t RawOffset = 0;
   uint32_t RawSize = 0;
};

static bool pe_rva_to_offset(const std::vector<uint8_t> &Data, const PeSection &ResourceSection, uint32_t Rva,
   size_t NeededSize, size_t *Offset)
{
   auto span = std::max(ResourceSection.VirtualSize, ResourceSection.RawSize);
   if ((span > 0) and (Rva >= ResourceSection.VirtualAddress) and
      ((Rva - ResourceSection.VirtualAddress) < span)) {
      auto offset = size_t(Rva - ResourceSection.VirtualAddress);
      if ((offset <= Data.size()) and (NeededSize <= Data.size() - offset)) {
         *Offset = offset;
         return true;
      }
   }

   // Version resource data normally lives in the resource section.  Entries that point outside it are skipped
   // because this parser only loads that section, not the whole executable.
   return false;
}

static bool read_pe_resource_entry(const std::vector<uint8_t> &Data, size_t BaseOffset, uint32_t DirectoryOffset,
   uint16_t ID, uint32_t *ChildOffset)
{
   auto dir = BaseOffset + DirectoryOffset;
   if (dir + 16 > Data.size()) return false;

   uint16_t named_count;
   uint16_t id_count;
   if (not read_u16_le(Data.data(), Data.size(), dir + 12, &named_count)) return false;
   if (not read_u16_le(Data.data(), Data.size(), dir + 14, &id_count)) return false;

   auto count = uint32_t(named_count) + uint32_t(id_count);
   auto entry = dir + 16;
   for (uint32_t index = 0; index < count; index++, entry += 8) {
      uint32_t name;
      uint32_t offset;
      if (entry + 8 > Data.size()) return false;
      if (not read_u32_le(Data.data(), Data.size(), entry, &name)) return false;
      if (not read_u32_le(Data.data(), Data.size(), entry + 4, &offset)) return false;
      if (((name & 0x80000000) IS 0) and (uint16_t(name) IS ID) and ((offset & 0x80000000) != 0)) {
         *ChildOffset = offset & 0x7fffffff;
         return true;
      }
   }

   return false;
}

static void collect_pe_resource_data(const std::vector<uint8_t> &Data, const PeSection &ResourceSection,
   size_t BaseOffset, uint32_t DirectoryOffset, std::vector<std::pair<size_t, size_t>> &Resources, int Depth = 0)
{
   // Resource trees are at most three levels deep (type/name/language); the cap guards against
   // corrupt files whose directory entries form a cycle.
   if (Depth > 4) return;

   auto dir = BaseOffset + DirectoryOffset;
   if (dir + 16 > Data.size()) return;

   uint16_t named_count;
   uint16_t id_count;
   if (not read_u16_le(Data.data(), Data.size(), dir + 12, &named_count)) return;
   if (not read_u16_le(Data.data(), Data.size(), dir + 14, &id_count)) return;

   auto count = uint32_t(named_count) + uint32_t(id_count);
   auto entry = dir + 16;
   for (uint32_t index = 0; index < count; index++, entry += 8) {
      uint32_t offset;
      if (entry + 8 > Data.size()) return;
      if (not read_u32_le(Data.data(), Data.size(), entry + 4, &offset)) return;
      if ((offset & 0x80000000) != 0) {
         collect_pe_resource_data(Data, ResourceSection, BaseOffset, offset & 0x7fffffff, Resources, Depth + 1);
      }
      else {
         auto data_entry = BaseOffset + offset;
         uint32_t data_rva;
         uint32_t data_size;
         size_t data_offset;
         if (data_entry + 16 > Data.size()) continue;
         if (not read_u32_le(Data.data(), Data.size(), data_entry, &data_rva)) continue;
         if (not read_u32_le(Data.data(), Data.size(), data_entry + 4, &data_size)) continue;
         if ((data_size > 0) and pe_rva_to_offset(Data, ResourceSection, data_rva, data_size, &data_offset) and
            (data_offset + data_size <= Data.size())) {
            Resources.emplace_back(data_offset, data_size);
         }
      }
   }
}

static ERR load_pe_version_metadata(const std::string &Path, TaskVersionMetadata &Metadata)
{
   std::ifstream file(Path, std::ios::binary);
   if (not file) return ERR::File;

   file.seekg(0, std::ios::end);
   auto file_size = file.tellg();
   if (file_size <= 0) return ERR::Query;
   file.seekg(0, std::ios::beg);

   if (file_size < std::streampos(0x40)) return ERR::Query;

   std::vector<uint8_t> dos_header(0x40);
   if (not file.read((char *)dos_header.data(), std::streamsize(dos_header.size()))) return ERR::File;
   if ((dos_header[0] != 'M') or (dos_header[1] != 'Z')) return ERR::Query;

   uint64_t file_size_bytes = uint64_t(file_size);
   uint32_t pe_offset;
   if (not read_u32_le(dos_header.data(), dos_header.size(), 0x3c, &pe_offset)) return ERR::Query;
   if (uint64_t(pe_offset) + 24 > file_size_bytes) return ERR::Query;

   std::vector<uint8_t> pe_header(24);
   file.seekg(pe_offset, std::ios::beg);
   if (not file.read((char *)pe_header.data(), std::streamsize(pe_header.size()))) return ERR::File;
   if ((pe_header[0] != 'P') or (pe_header[1] != 'E') or (pe_header[2] != 0) or (pe_header[3] != 0)) {
      return ERR::Query;
   }

   uint16_t section_count;
   uint16_t optional_size;
   if (not read_u16_le(pe_header.data(), pe_header.size(), 6, &section_count)) return ERR::Query;
   if (not read_u16_le(pe_header.data(), pe_header.size(), 20, &optional_size)) return ERR::Query;
   if (section_count > 96) return ERR::Query;

   auto pe_data_size = size_t(24) + size_t(optional_size) + (size_t(section_count) * 40);
   if (uint64_t(pe_offset) + uint64_t(pe_data_size) > file_size_bytes) return ERR::Query;

   std::vector<uint8_t> pe_data(pe_data_size);
   file.seekg(pe_offset, std::ios::beg);
   if (not file.read((char *)pe_data.data(), std::streamsize(pe_data.size()))) return ERR::File;

   auto optional_offset = size_t(24);

   uint16_t magic;
   if (not read_u16_le(pe_data.data(), pe_data.size(), optional_offset, &magic)) return ERR::Query;

   size_t data_directory_offset;
   if (magic IS 0x10b) data_directory_offset = optional_offset + 96;
   else if (magic IS 0x20b) data_directory_offset = optional_offset + 112;
   else return ERR::Query;

   if (data_directory_offset + 24 > optional_offset + optional_size) return ERR::Query;

   uint32_t resource_rva;
   uint32_t resource_size;
   if (not read_u32_le(pe_data.data(), pe_data.size(), data_directory_offset + 16, &resource_rva)) return ERR::Query;
   if (not read_u32_le(pe_data.data(), pe_data.size(), data_directory_offset + 20, &resource_size)) return ERR::Query;
   if ((resource_rva IS 0) or (resource_size IS 0)) return ERR::Query;

   auto section_offset = optional_offset + optional_size;
   if (section_offset + (size_t(section_count) * 40) > pe_data.size()) return ERR::Query;

   std::vector<PeSection> sections;
   sections.reserve(section_count);
   for (uint16_t i = 0; i < section_count; i++) {
      auto offset = section_offset + (size_t(i) * 40);
      PeSection section;
      if (not read_u32_le(pe_data.data(), pe_data.size(), offset + 8, &section.VirtualSize)) return ERR::Query;
      if (not read_u32_le(pe_data.data(), pe_data.size(), offset + 12, &section.VirtualAddress)) return ERR::Query;
      if (not read_u32_le(pe_data.data(), pe_data.size(), offset + 16, &section.RawSize)) return ERR::Query;
      if (not read_u32_le(pe_data.data(), pe_data.size(), offset + 20, &section.RawOffset)) return ERR::Query;
      sections.push_back(section);
   }

   PeSection resource_section;
   bool found_resource_section = false;
   for (auto &section : sections) {
      auto span = std::max(section.VirtualSize, section.RawSize);
      if ((span > 0) and (resource_rva >= section.VirtualAddress) and
         ((resource_rva - section.VirtualAddress) < span)) {
         resource_section = section;
         found_resource_section = true;
         break;
      }
   }

   if (not found_resource_section) return ERR::Query;
   if (resource_section.RawSize IS 0) return ERR::Query;
   if (uint64_t(resource_section.RawOffset) + uint64_t(resource_section.RawSize) > file_size_bytes) return ERR::Query;

   std::vector<uint8_t> section_data(size_t(resource_section.RawSize));
   file.seekg(resource_section.RawOffset, std::ios::beg);
   if (not file.read((char *)section_data.data(), std::streamsize(section_data.size()))) return ERR::File;

   size_t resource_offset;
   if (not pe_rva_to_offset(section_data, resource_section, resource_rva, 16, &resource_offset)) return ERR::Query;

   uint32_t version_directory;
   if (not read_pe_resource_entry(section_data, resource_offset, 0, 16, &version_directory)) return ERR::Query;

   std::vector<std::pair<size_t, size_t>> resources;
   collect_pe_resource_data(section_data, resource_section, resource_offset, version_directory, resources);
   if (resources.empty()) return ERR::Query;

   bool found = false;
   for (auto &resource : resources) {
      if (parse_version_info(section_data.data() + resource.first, resource.second, Metadata)) found = true;
   }

   return found ? ERR::Okay : ERR::Query;
}

static ERR load_task_version_metadata(const std::string &Path, TaskVersionMetadata &Metadata)
{
#ifdef _WIN32
   int handle = 0;
   int size = GetFileVersionInfoSizeA(Path.c_str(), &handle);
   if (size > 0) {
      std::vector<uint8_t> buffer((size_t(size)));
      if (GetFileVersionInfoA(Path.c_str(), 0, size, buffer.data())) {
         if (parse_version_info(buffer.data(), buffer.size(), Metadata)) return ERR::Okay;
         return ERR::Query;
      }
   }
#endif

   return load_pe_version_metadata(Path, Metadata);
}
