
//********************************************************************************************************************

static void clear_server_certificate(SSL_HANDLE SSL)
{
   if (SSL->server_certificate) {
      CertFreeCertificateContext(SSL->server_certificate);
      SSL->server_certificate = nullptr;
   }

   if (SSL->server_certificate_store) {
      CertCloseStore(SSL->server_certificate_store, 0);
      SSL->server_certificate_store = nullptr;
   }

   if (SSL->imported_private_key) {
      if (SSL->imported_private_key_persistent) {
         auto status = NCryptDeleteKey(SSL->imported_private_key, NCRYPT_SILENT_FLAG);
         if ((status != ERROR_SUCCESS) and SSL->imported_private_key_owned) NCryptFreeObject(SSL->imported_private_key);
      }
      else if (SSL->imported_private_key_owned) NCryptFreeObject(SSL->imported_private_key);
      SSL->imported_private_key = 0;
      SSL->imported_private_key_owned = false;
      SSL->imported_private_key_persistent = false;
   }
}

//********************************************************************************************************************

static bool read_binary_file(const std::string &Path, std::vector<BYTE> &Data)
{
   HANDLE file = CreateFileA(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
   if (file IS INVALID_HANDLE_VALUE) return false;

   DWORD file_size = GetFileSize(file, nullptr);
   if ((file_size IS INVALID_FILE_SIZE) or (!file_size)) {
      CloseHandle(file);
      return false;
   }

   Data.resize(file_size);
   DWORD bytes_read;
   if ((!ReadFile(file, Data.data(), file_size, &bytes_read, nullptr)) or (bytes_read != file_size)) {
      CloseHandle(file);
      return false;
   }

   CloseHandle(file);
   return true;
}

//********************************************************************************************************************

static bool read_text_file(const std::string &Path, std::string &Data)
{
   std::vector<BYTE> file_data;
   if (!read_binary_file(Path, file_data)) return false;

   Data.assign((const char *)file_data.data(), file_data.size());
   return true;
}

//********************************************************************************************************************

static bool decode_pem_section(const std::string &PemData, const char *Label, std::vector<BYTE> &DerData)
{
   std::string begin = "-----BEGIN ";
   begin.append(Label);
   begin.append("-----");

   std::string end = "-----END ";
   end.append(Label);
   end.append("-----");

   auto begin_pos = PemData.find(begin);
   if (begin_pos IS std::string::npos) return false;

   auto end_pos = PemData.find(end, begin_pos);
   if (end_pos IS std::string::npos) return false;

   end_pos += end.size();
   auto pem_block = PemData.substr(begin_pos, end_pos - begin_pos);

   DWORD der_size = 0;
   if (!CryptStringToBinaryA(pem_block.c_str(), 0, CRYPT_STRING_BASE64HEADER, nullptr, &der_size, nullptr,
       nullptr)) {
      return false;
   }

   DerData.resize(der_size);
   return CryptStringToBinaryA(pem_block.c_str(), 0, CRYPT_STRING_BASE64HEADER, DerData.data(), &der_size,
      nullptr, nullptr);
}

//********************************************************************************************************************

struct PemBlock {
   std::vector<BYTE> DerData;
   std::string CipherName;
   std::vector<BYTE> InitialisationVector;
};

static std::string trim_pem_header(const std::string &Text)
{
   auto first = Text.find_first_not_of(" \t\r\n");
   if (first IS std::string::npos) return {};

   auto last = Text.find_last_not_of(" \t\r\n");
   return Text.substr(first, last - first + 1);
}

//********************************************************************************************************************

static bool hex_value(char Char, BYTE &Value)
{
   if ((Char >= '0') and (Char <= '9')) {
      Value = BYTE(Char - '0');
      return true;
   }

   if ((Char >= 'A') and (Char <= 'F')) {
      Value = BYTE(Char - 'A' + 10);
      return true;
   }

   if ((Char >= 'a') and (Char <= 'f')) {
      Value = BYTE(Char - 'a' + 10);
      return true;
   }

   return false;
}

//********************************************************************************************************************

static bool decode_hex_string(const std::string &Text, std::vector<BYTE> &Data)
{
   if (Text.size() & 1) return false;

   Data.clear();
   Data.reserve(Text.size() >> 1);

   for (size_t i = 0; i < Text.size(); i += 2) {
      BYTE high;
      BYTE low;
      if ((!hex_value(Text[i], high)) or (!hex_value(Text[i + 1], low))) return false;
      Data.push_back(BYTE((high << 4) | low));
   }

   return true;
}

//********************************************************************************************************************

static bool decode_pem_block(const std::string &PemData, const char *Label, PemBlock &Block)
{
   std::string begin = "-----BEGIN ";
   begin.append(Label);
   begin.append("-----");

   std::string end = "-----END ";
   end.append(Label);
   end.append("-----");

   auto begin_pos = PemData.find(begin);
   if (begin_pos IS std::string::npos) return false;

   auto end_pos = PemData.find(end, begin_pos);
   if (end_pos IS std::string::npos) return false;

   auto block_end = end_pos + end.size();
   auto pem_block = PemData.substr(begin_pos, block_end - begin_pos);

   Block = {};

   std::istringstream stream(pem_block);
   std::string line;
   bool in_headers = false;
   while (std::getline(stream, line)) {
      if ((!line.empty()) and (line.back() IS '\r')) line.pop_back();

      if (line.find(begin) IS 0) {
         in_headers = true;
         continue;
      }

      if (line.empty()) break;
      if (!in_headers) continue;

      auto colon = line.find(':');
      if (colon IS std::string::npos) continue;

      auto header_name = trim_pem_header(line.substr(0, colon));
      auto header_value = trim_pem_header(line.substr(colon + 1));
      if (_stricmp(header_name.c_str(), "DEK-Info") IS 0) {
         auto comma = header_value.find(',');
         if (comma IS std::string::npos) return false;

         Block.CipherName = trim_pem_header(header_value.substr(0, comma));
         auto iv_text = trim_pem_header(header_value.substr(comma + 1));
         if (!decode_hex_string(iv_text, Block.InitialisationVector)) return false;
      }
   }

   DWORD der_size = 0;
   if (!CryptStringToBinaryA(pem_block.c_str(), 0, CRYPT_STRING_BASE64HEADER, nullptr, &der_size, nullptr,
       nullptr)) {
      return false;
   }

   Block.DerData.resize(der_size);
   return CryptStringToBinaryA(pem_block.c_str(), 0, CRYPT_STRING_BASE64HEADER, Block.DerData.data(), &der_size,
      nullptr, nullptr);
}

//********************************************************************************************************************

static std::wstring utf8_to_wide(const std::string &Text)
{
   if (Text.empty()) return {};

   auto wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.c_str(), int(Text.size()), nullptr, 0);
   if (wide_size <= 0) return {};

   std::wstring result(wide_size, L'\0');
   MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, Text.c_str(), int(Text.size()), result.data(), wide_size);
   return result;
}

//********************************************************************************************************************

static std::wstring password_to_wide(std::optional<const std::string> &Password)
{
   if (!Password.has_value()) return {};
   return utf8_to_wide(Password.value());
}

//********************************************************************************************************************

static bool md5_digest(const std::vector<BYTE> &Previous, const std::string &Password,
   const std::vector<BYTE> &Salt, std::vector<BYTE> &Digest)
{
   BCRYPT_ALG_HANDLE algorithm = 0;
   BCRYPT_HASH_HANDLE hash = 0;

   auto status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_MD5_ALGORITHM, nullptr, 0);
   if (status < 0) return false;

   DWORD result_size = 0;
   DWORD object_size = 0;
   status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size, sizeof(object_size),
      &result_size, 0);
   if (status < 0) {
      BCryptCloseAlgorithmProvider(algorithm, 0);
      return false;
   }

   DWORD hash_size = 0;
   status = BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_size, sizeof(hash_size),
      &result_size, 0);
   if (status < 0) {
      BCryptCloseAlgorithmProvider(algorithm, 0);
      return false;
   }

   std::vector<BYTE> hash_object(object_size);
   status = BCryptCreateHash(algorithm, &hash, hash_object.data(), object_size, nullptr, 0, 0);
   if (status >= 0 and (!Previous.empty())) {
      status = BCryptHashData(hash, (PUCHAR)Previous.data(), ULONG(Previous.size()), 0);
   }
   if (status >= 0 and (!Password.empty())) {
      status = BCryptHashData(hash, (PUCHAR)Password.data(), ULONG(Password.size()), 0);
   }
   if (status >= 0) status = BCryptHashData(hash, (PUCHAR)Salt.data(), 8, 0);

   Digest.resize(hash_size);
   if (status >= 0) status = BCryptFinishHash(hash, Digest.data(), hash_size, 0);

   if (hash) BCryptDestroyHash(hash);
   BCryptCloseAlgorithmProvider(algorithm, 0);

   return status >= 0;
}

//********************************************************************************************************************

static bool derive_openssl_pem_key(const std::string &Password, const std::vector<BYTE> &Salt, size_t KeySize,
   std::vector<BYTE> &Key)
{
   if (Salt.size() < 8) return false;

   Key.clear();
   Key.reserve(KeySize);

   std::vector<BYTE> previous;
   while (Key.size() < KeySize) {
      std::vector<BYTE> digest;
      if (!md5_digest(previous, Password, Salt, digest)) return false;

      auto copy_size = std::min(digest.size(), KeySize - Key.size());
      Key.insert(Key.end(), digest.begin(), digest.begin() + copy_size);
      previous = std::move(digest);
   }

   return true;
}

//********************************************************************************************************************

static bool decrypt_cbc_private_key(const std::vector<BYTE> &CipherText, const std::vector<BYTE> &KeyData,
   const std::vector<BYTE> &InitialisationVector, const wchar_t *Algorithm, size_t InitialisationVectorSize,
   std::vector<BYTE> &PlainText)
{
   if (InitialisationVector.size() != InitialisationVectorSize) return false;

   BCRYPT_ALG_HANDLE algorithm = 0;
   BCRYPT_KEY_HANDLE key = 0;

   auto status = BCryptOpenAlgorithmProvider(&algorithm, Algorithm, nullptr, 0);
   if (status < 0) return false;

   status = BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_CBC,
      sizeof(BCRYPT_CHAIN_MODE_CBC), 0);
   if (status < 0) {
      BCryptCloseAlgorithmProvider(algorithm, 0);
      return false;
   }

   status = BCryptGenerateSymmetricKey(algorithm, &key, nullptr, 0, (PUCHAR)KeyData.data(),
      ULONG(KeyData.size()), 0);
   if (status < 0) {
      BCryptCloseAlgorithmProvider(algorithm, 0);
      return false;
   }

   ULONG plain_size = 0;
   auto iv = InitialisationVector;
   status = BCryptDecrypt(key, (PUCHAR)CipherText.data(), ULONG(CipherText.size()), nullptr, iv.data(),
      ULONG(iv.size()), nullptr, 0, &plain_size, BCRYPT_BLOCK_PADDING);
   if (status >= 0) {
      PlainText.resize(plain_size);
      iv = InitialisationVector;
      status = BCryptDecrypt(key, (PUCHAR)CipherText.data(), ULONG(CipherText.size()), nullptr, iv.data(),
         ULONG(iv.size()), PlainText.data(), plain_size, &plain_size, BCRYPT_BLOCK_PADDING);
      if (status >= 0) PlainText.resize(plain_size);
   }

   BCryptDestroyKey(key);
   BCryptCloseAlgorithmProvider(algorithm, 0);

   return status >= 0;
}

//********************************************************************************************************************

static bool decrypt_traditional_pem_key(const PemBlock &Block, std::optional<const std::string> &Password,
   std::vector<BYTE> &PlainText)
{
   if (Block.CipherName.empty()) {
      PlainText = Block.DerData;
      return true;
   }

   if (!Password.has_value()) return false;

   size_t key_size = 0;
   size_t iv_size = 16;
   const wchar_t *algorithm = BCRYPT_AES_ALGORITHM;

   if (_stricmp(Block.CipherName.c_str(), "AES-128-CBC") IS 0) key_size = 16;
   else if (_stricmp(Block.CipherName.c_str(), "AES-192-CBC") IS 0) key_size = 24;
   else if (_stricmp(Block.CipherName.c_str(), "AES-256-CBC") IS 0) key_size = 32;
   else if (_stricmp(Block.CipherName.c_str(), "DES-EDE3-CBC") IS 0) {
      algorithm = BCRYPT_3DES_ALGORITHM;
      key_size = 24;
      iv_size = 8;
   }
   else {
      ssl_debug_log(SSL_DEBUG_INFO, "Unsupported encrypted PEM cipher: %s", Block.CipherName.c_str());
      return false;
   }

   std::vector<BYTE> key;
   if (!derive_openssl_pem_key(Password.value(), Block.InitialisationVector, key_size, key)) return false;
   return decrypt_cbc_private_key(Block.DerData, key, Block.InitialisationVector, algorithm, iv_size, PlainText);
}

//********************************************************************************************************************

static void append_der_length(std::vector<BYTE> &Data, size_t Length)
{
   if (Length < 0x80) Data.push_back(BYTE(Length));
   else {
      std::array<BYTE, sizeof(size_t)> length_bytes {};
      size_t length_count = 0;

      while (Length) {
         length_bytes[length_count++] = BYTE(Length & 0xff);
         Length >>= 8;
      }

      Data.push_back(BYTE(0x80 | length_count));
      while (length_count) Data.push_back(length_bytes[--length_count]);
   }
}

//********************************************************************************************************************

static void append_der_tlv(std::vector<BYTE> &Data, BYTE Tag, const std::vector<BYTE> &Value)
{
   Data.push_back(Tag);
   append_der_length(Data, Value.size());
   Data.insert(Data.end(), Value.begin(), Value.end());
}

//********************************************************************************************************************

static void append_der_oid(std::vector<BYTE> &Data, const std::vector<BYTE> &OidValue)
{
   append_der_tlv(Data, 0x06, OidValue);
}

//********************************************************************************************************************

static void append_der_sequence(std::vector<BYTE> &Data, const std::vector<BYTE> &Value)
{
   append_der_tlv(Data, 0x30, Value);
}

//********************************************************************************************************************

static void append_pkcs8_private_key(std::vector<BYTE> &Data, const std::vector<BYTE> &Algorithm,
   const std::vector<BYTE> &PrivateKey)
{
   std::vector<BYTE> version { 0x02, 0x01, 0x00 };
   std::vector<BYTE> private_key;
   append_der_tlv(private_key, 0x04, PrivateKey);

   std::vector<BYTE> body;
   body.insert(body.end(), version.begin(), version.end());
   body.insert(body.end(), Algorithm.begin(), Algorithm.end());
   body.insert(body.end(), private_key.begin(), private_key.end());
   append_der_sequence(Data, body);
}

//********************************************************************************************************************

static std::vector<BYTE> wrap_rsa_private_key_as_pkcs8(const std::vector<BYTE> &PrivateKey)
{
   static const std::vector<BYTE> rsa_encryption_oid { 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x01, 0x01 };

   std::vector<BYTE> algorithm_body;
   append_der_oid(algorithm_body, rsa_encryption_oid);
   algorithm_body.push_back(0x05);
   algorithm_body.push_back(0x00);

   std::vector<BYTE> algorithm;
   append_der_sequence(algorithm, algorithm_body);

   std::vector<BYTE> pkcs8;
   append_pkcs8_private_key(pkcs8, algorithm, PrivateKey);
   return pkcs8;
}

//********************************************************************************************************************

struct DerItem {
   BYTE Tag = 0;
   size_t Value = 0;
   size_t Length = 0;
   size_t Next = 0;
};

static bool read_der_length(const std::vector<BYTE> &Data, size_t &Offset, size_t Limit, size_t &Length)
{
   if (Offset >= Limit) return false;

   BYTE first = Data[Offset++];
   if (first < 0x80) {
      Length = first;
      return true;
   }

   auto length_octets = size_t(first & 0x7f);
   if ((length_octets IS 0) or (length_octets > sizeof(size_t)) or (Offset + length_octets > Limit)) return false;

   Length = 0;
   while (length_octets--) {
      Length = (Length << 8) | Data[Offset++];
   }

   return true;
}

//********************************************************************************************************************

static bool read_der_item(const std::vector<BYTE> &Data, size_t Offset, size_t Limit, DerItem &Item)
{
   if (Offset >= Limit) return false;

   Item.Tag = Data[Offset++];
   if (!read_der_length(Data, Offset, Limit, Item.Length)) return false;

   Item.Value = Offset;
   Item.Next = Offset + Item.Length;
   return Item.Next <= Limit;
}

//********************************************************************************************************************

static bool extract_sec1_ec_key(const std::vector<BYTE> &PrivateKey, std::vector<BYTE> &CurveOid,
   std::vector<BYTE> &PrivateScalar, std::vector<BYTE> &PublicX, std::vector<BYTE> &PublicY)
{
   DerItem root;
   if ((!read_der_item(PrivateKey, 0, PrivateKey.size(), root)) or (root.Tag != 0x30)) return false;

   size_t offset = root.Value;
   while (offset < root.Next) {
      DerItem item;
      if (!read_der_item(PrivateKey, offset, root.Next, item)) return false;

      if (item.Tag IS 0x04) {
         PrivateScalar.assign(PrivateKey.begin() + item.Value, PrivateKey.begin() + item.Next);
      }
      else if (item.Tag IS 0xa0) {
         DerItem oid;
         if ((!read_der_item(PrivateKey, item.Value, item.Next, oid)) or (oid.Tag != 0x06)) return false;

         CurveOid.assign(PrivateKey.begin() + oid.Value, PrivateKey.begin() + oid.Next);
      }
      else if (item.Tag IS 0xa1) {
         DerItem public_key;
         if ((!read_der_item(PrivateKey, item.Value, item.Next, public_key)) or (public_key.Tag != 0x03)) {
            return false;
         }

         auto public_key_len = public_key.Length;
         auto public_key_offset = public_key.Value;
         if ((public_key_len < 3) or (PrivateKey[public_key_offset] != 0) or (PrivateKey[public_key_offset + 1] != 4)) {
            return false;
         }

         public_key_offset += 2;
         public_key_len -= 2;
         if (public_key_len & 1) return false;

         auto field_size = public_key_len >> 1;
         PublicX.assign(PrivateKey.begin() + public_key_offset, PrivateKey.begin() + public_key_offset + field_size);
         PublicY.assign(PrivateKey.begin() + public_key_offset + field_size,
            PrivateKey.begin() + public_key_offset + public_key_len);
      }

      offset = item.Next;
   }

   return (!CurveOid.empty()) and (!PrivateScalar.empty()) and (!PublicX.empty()) and (!PublicY.empty());
}

//********************************************************************************************************************

static bool bytes_match(const std::vector<BYTE> &Data, std::initializer_list<BYTE> Expected)
{
   if (Data.size() != Expected.size()) return false;
   return std::equal(Data.begin(), Data.end(), Expected.begin());
}

//********************************************************************************************************************

static bool ec_curve_info(const std::vector<BYTE> &CurveOid, DWORD &Magic, DWORD &KeySize)
{
   if (bytes_match(CurveOid, { 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07 })) {
      Magic = BCRYPT_ECDSA_PRIVATE_P256_MAGIC;
      KeySize = 32;
      return true;
   }

   if (bytes_match(CurveOid, { 0x2b, 0x81, 0x04, 0x00, 0x22 })) {
      Magic = BCRYPT_ECDSA_PRIVATE_P384_MAGIC;
      KeySize = 48;
      return true;
   }

   if (bytes_match(CurveOid, { 0x2b, 0x81, 0x04, 0x00, 0x23 })) {
      Magic = BCRYPT_ECDSA_PRIVATE_P521_MAGIC;
      KeySize = 66;
      return true;
   }

   return false;
}

//********************************************************************************************************************

static bool append_fixed_ec_component(std::vector<BYTE> &Blob, const std::vector<BYTE> &Component, DWORD KeySize)
{
   if (Component.size() > KeySize) return false;

   Blob.insert(Blob.end(), KeySize - Component.size(), 0);
   Blob.insert(Blob.end(), Component.begin(), Component.end());
   return true;
}

//********************************************************************************************************************

static void free_imported_private_key(NCRYPT_KEY_HANDLE KeyHandle)
{
   if (!KeyHandle) return;
   NCryptFreeObject(KeyHandle);
}

//********************************************************************************************************************

static void allow_private_key_use(NCRYPT_KEY_HANDLE KeyHandle)
{
   DWORD key_usage = NCRYPT_ALLOW_ALL_USAGES;
   NCryptSetProperty(KeyHandle, NCRYPT_KEY_USAGE_PROPERTY, (PBYTE)&key_usage, sizeof(key_usage), 0);

   DWORD export_policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
   NCryptSetProperty(KeyHandle, NCRYPT_EXPORT_POLICY_PROPERTY, (PBYTE)&export_policy, sizeof(export_policy), 0);
}

//********************************************************************************************************************

static bool add_certificate_to_memory_store(PCCERT_CONTEXT Cert, HCERTSTORE &Store, PCCERT_CONTEXT &StoreCert)
{
   Store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
   if (!Store) return false;

   if (!CertAddCertificateContextToStore(Store, Cert, CERT_STORE_ADD_REPLACE_EXISTING, &StoreCert)) {
      CertCloseStore(Store, 0);
      Store = nullptr;
      return false;
   }

   return true;
}

//********************************************************************************************************************

static bool certificate_has_private_key(PCCERT_CONTEXT);

//********************************************************************************************************************

static bool import_sec1_ec_private_key(const std::vector<BYTE> &PrivateKey, NCRYPT_KEY_HANDLE &KeyHandle)
{
   std::vector<BYTE> curve_oid;
   std::vector<BYTE> private_scalar;
   std::vector<BYTE> public_x;
   std::vector<BYTE> public_y;
   if (!extract_sec1_ec_key(PrivateKey, curve_oid, private_scalar, public_x, public_y)) {
      ssl_debug_log(SSL_DEBUG_INFO, "Failed to parse SEC1 EC private key.");
      return false;
   }

   DWORD magic;
   DWORD key_size;
   if (!ec_curve_info(curve_oid, magic, key_size)) {
      ssl_debug_log(SSL_DEBUG_INFO, "Unsupported SEC1 EC private key curve.");
      return false;
   }

   BCRYPT_ECCKEY_BLOB blob_header {};
   blob_header.dwMagic = magic;
   blob_header.cbKey = key_size;

   std::vector<BYTE> blob(sizeof(blob_header));
   memcpy(blob.data(), &blob_header, sizeof(blob_header));

   if (!append_fixed_ec_component(blob, public_x, key_size)) return false;
   if (!append_fixed_ec_component(blob, public_y, key_size)) return false;
   if (!append_fixed_ec_component(blob, private_scalar, key_size)) return false;

   NCRYPT_PROV_HANDLE provider = 0;
   auto status = NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0);
   if (status != ERROR_SUCCESS) {
      ssl_debug_log(SSL_DEBUG_INFO, "NCryptOpenStorageProvider() failed for EC private key: 0x%08x", status);
      return false;
   }

   status = NCryptImportKey(provider, 0, BCRYPT_ECCPRIVATE_BLOB, nullptr, &KeyHandle, blob.data(),
      DWORD(blob.size()), NCRYPT_SILENT_FLAG);
   NCryptFreeObject(provider);

   if (status != ERROR_SUCCESS) {
      ssl_debug_log(SSL_DEBUG_INFO, "NCryptImportKey() failed for EC private key: 0x%08x", status);
      return false;
   }

   allow_private_key_use(KeyHandle);
   return true;
}

//********************************************************************************************************************

static bool import_pkcs8_private_key(const std::vector<BYTE> &, std::optional<const std::string> &,
   NCRYPT_KEY_HANDLE &);

//********************************************************************************************************************

static bool import_private_key_pem(const std::string &PEMData, std::optional<const std::string> &Password,
   NCRYPT_KEY_HANDLE &KeyHandle)
{
   std::vector<BYTE> key_der;

   if (decode_pem_section(PEMData, "PRIVATE KEY", key_der)) {
      return import_pkcs8_private_key(key_der, Password, KeyHandle);
   }

   if (decode_pem_section(PEMData, "ENCRYPTED PRIVATE KEY", key_der)) {
      return import_pkcs8_private_key(key_der, Password, KeyHandle);
   }

   PemBlock key_block;
   if (decode_pem_block(PEMData, "RSA PRIVATE KEY", key_block)) {
      if (!decrypt_traditional_pem_key(key_block, Password, key_der)) return false;
      key_der = wrap_rsa_private_key_as_pkcs8(key_der);
      return import_pkcs8_private_key(key_der, Password, KeyHandle);
   }

   if (decode_pem_block(PEMData, "EC PRIVATE KEY", key_block)) {
      if (!decrypt_traditional_pem_key(key_block, Password, key_der)) return false;
      return import_sec1_ec_private_key(key_der, KeyHandle);
   }

   return false;
}

//********************************************************************************************************************

static bool try_import_pkcs8_private_key(NCRYPT_PROV_HANDLE Provider, const std::vector<BYTE> &KeyDer,
   NCryptBufferDesc *Params, NCRYPT_KEY_HANDLE &KeyHandle)
{
   auto status = NCryptImportKey(Provider, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB, Params, &KeyHandle,
      (PBYTE)KeyDer.data(), DWORD(KeyDer.size()), NCRYPT_SILENT_FLAG);
   return status IS ERROR_SUCCESS;
}

//********************************************************************************************************************

static bool import_pkcs8_private_key(const std::vector<BYTE> &KeyDer, std::optional<const std::string> &Password,
   NCRYPT_KEY_HANDLE &KeyHandle)
{
   NCRYPT_PROV_HANDLE provider = 0;
   if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return false;

   std::wstring wide_password = password_to_wide(Password);
   NCryptBuffer password_buffer {};
   NCryptBufferDesc password_desc {};

   if (Password.has_value()) {
      password_buffer.cbBuffer = DWORD((wide_password.size() + 1) * sizeof(wchar_t));
      password_buffer.BufferType = NCRYPTBUFFER_PKCS_SECRET;
      password_buffer.pvBuffer = (PVOID)wide_password.c_str();

      password_desc.ulVersion = NCRYPTBUFFER_VERSION;
      password_desc.cBuffers = 1;
      password_desc.pBuffers = &password_buffer;
   }

   auto result = try_import_pkcs8_private_key(provider, KeyDer, Password.has_value() ? &password_desc : nullptr,
      KeyHandle);
   if ((!result) and Password.has_value()) {
      result = try_import_pkcs8_private_key(provider, KeyDer, nullptr, KeyHandle);
   }

   NCryptFreeObject(provider);
   if (result) allow_private_key_use(KeyHandle);

   return result;
}

//********************************************************************************************************************

static bool certificate_has_private_key(PCCERT_CONTEXT Cert)
{
   HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key_handle = 0;
   DWORD key_spec = 0;
   BOOL free_key = false;

   if (!CryptAcquireCertificatePrivateKey(Cert, CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG,
       nullptr, &key_handle, &key_spec, &free_key)) {
      return false;
   }

   if (free_key) NCryptFreeObject(NCRYPT_KEY_HANDLE(key_handle));
   return true;
}

//********************************************************************************************************************

static bool acquire_certificate_private_key(PCCERT_CONTEXT Cert, NCRYPT_KEY_HANDLE &KeyHandle, bool &Owned)
{
   HCRYPTPROV_OR_NCRYPT_KEY_HANDLE key_handle = 0;
   DWORD key_spec = 0;
   BOOL free_key = false;
   DWORD flags = CRYPT_ACQUIRE_ONLY_NCRYPT_KEY_FLAG | CRYPT_ACQUIRE_SILENT_FLAG;

   if (!CryptAcquireCertificatePrivateKey(Cert, flags, nullptr, &key_handle, &key_spec, &free_key)) {
      return false;
   }

   if (!(key_spec IS CERT_NCRYPT_KEY_SPEC)) {
      if (free_key) CryptReleaseContext(HCRYPTPROV(key_handle), 0);
      return false;
   }

   KeyHandle = NCRYPT_KEY_HANDLE(key_handle);
   Owned = free_key != false;
   return true;
}

//********************************************************************************************************************

static bool export_public_key_info(NCRYPT_KEY_HANDLE KeyHandle, std::vector<BYTE> &KeyInfoBuffer)
{
   DWORD key_info_size = 0;
   if (!CryptExportPublicKeyInfo(HCRYPTPROV_OR_NCRYPT_KEY_HANDLE(KeyHandle), CERT_NCRYPT_KEY_SPEC,
       X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, nullptr, &key_info_size)) {
      return false;
   }

   KeyInfoBuffer.resize(key_info_size);
   auto key_info = PCERT_PUBLIC_KEY_INFO(KeyInfoBuffer.data());
   if (!CryptExportPublicKeyInfo(HCRYPTPROV_OR_NCRYPT_KEY_HANDLE(KeyHandle), CERT_NCRYPT_KEY_SPEC,
       X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, key_info, &key_info_size)) {
      return false;
   }

   KeyInfoBuffer.resize(key_info_size);
   return true;
}

//********************************************************************************************************************

static bool private_key_matches_certificate(PCCERT_CONTEXT Cert, NCRYPT_KEY_HANDLE KeyHandle)
{
   if ((!Cert) or (!Cert->pCertInfo)) return false;

   std::vector<BYTE> private_key_info_buffer;
   if (!export_public_key_info(KeyHandle, private_key_info_buffer)) return false;

   auto private_key_info = PCERT_PUBLIC_KEY_INFO(private_key_info_buffer.data());
   return CertComparePublicKeyInfo(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      &Cert->pCertInfo->SubjectPublicKeyInfo, private_key_info);
}

//********************************************************************************************************************

static bool attach_private_key(PCCERT_CONTEXT Cert, NCRYPT_KEY_HANDLE KeyHandle)
{
   DWORD property_flags = CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG | CERT_STORE_NO_CRYPT_RELEASE_FLAG;

   CERT_KEY_CONTEXT key_context {};
   key_context.cbSize = sizeof(key_context);
   key_context.hNCryptKey = KeyHandle;
   key_context.dwKeySpec = CERT_NCRYPT_KEY_SPEC;

   if (!CertSetCertificateContextProperty(Cert, CERT_KEY_CONTEXT_PROP_ID, property_flags, &key_context)) {
      return false;
   }

   return certificate_has_private_key(Cert);
}

//********************************************************************************************************************

bool load_pkcs12_certificate(SSL_HANDLE SSL, const std::string &Path, std::optional<const std::string> &Password)
{
   std::vector<BYTE> p12_data;
   if (!read_binary_file(Path, p12_data)) return false;

   CRYPT_DATA_BLOB pfx_blob;
   pfx_blob.cbData = DWORD(p12_data.size());
   pfx_blob.pbData = p12_data.data();

   auto wide_password = password_to_wide(Password);
   DWORD import_flags = CRYPT_EXPORTABLE | CRYPT_USER_KEYSET;
   import_flags |= PKCS12_PREFER_CNG_KSP;

   HCERTSTORE pfx_store = PFXImportCertStore(&pfx_blob, wide_password.c_str(), import_flags);
   if (!pfx_store) return false;

   PCCERT_CONTEXT selected_cert = nullptr;
   NCRYPT_KEY_HANDLE selected_key = 0;
   bool selected_key_owned = false;
   PCCERT_CONTEXT cert_context = nullptr;

   while ((cert_context = CertEnumCertificatesInStore(pfx_store, cert_context))) {
      if (acquire_certificate_private_key(cert_context, selected_key, selected_key_owned)) {
         selected_cert = CertDuplicateCertificateContext(cert_context);
         break;
      }
   }

   CertCloseStore(pfx_store, 0);
   if (!selected_cert) return false;

   clear_server_certificate(SSL);
   SSL->server_certificate = selected_cert;
   SSL->imported_private_key = selected_key;
   SSL->imported_private_key_owned = selected_key_owned;
   SSL->imported_private_key_persistent = true;
   return true;
}

//********************************************************************************************************************

bool load_pem_certificate(SSL_HANDLE SSL, const std::string &Path, std::optional<const std::string> &KeyPath,
   std::optional<const std::string> &Password)
{
   std::string cert_pem;
   if (!read_text_file(Path, cert_pem)) return false;

   std::vector<BYTE> cert_der;
   if (!decode_pem_section(cert_pem, "CERTIFICATE", cert_der)) return false;

   PCCERT_CONTEXT cert_context = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
      cert_der.data(), DWORD(cert_der.size()));
   if (!cert_context) return false;

   std::string key_pem;
   if (KeyPath.has_value()) {
      if (!read_text_file(KeyPath.value(), key_pem)) {
         CertFreeCertificateContext(cert_context);
         return false;
      }
   }
   else key_pem = cert_pem;

   NCRYPT_KEY_HANDLE key_handle = 0;
   if (!import_private_key_pem(key_pem, Password, key_handle)) {
      CertFreeCertificateContext(cert_context);
      return false;
   }

   if (!private_key_matches_certificate(cert_context, key_handle)) {
      ssl_debug_log(SSL_DEBUG_INFO, "PEM private key does not match the certificate public key.");
      free_imported_private_key(key_handle);
      CertFreeCertificateContext(cert_context);
      return false;
   }

   if (!attach_private_key(cert_context, key_handle)) {
      free_imported_private_key(key_handle);
      CertFreeCertificateContext(cert_context);
      return false;
   }

   HCERTSTORE cert_store = nullptr;
   PCCERT_CONTEXT store_cert_context = nullptr;
   if (!add_certificate_to_memory_store(cert_context, cert_store, store_cert_context)) {
      free_imported_private_key(key_handle);
      CertFreeCertificateContext(cert_context);
      return false;
   }

   CertFreeCertificateContext(cert_context);

   if (!attach_private_key(store_cert_context, key_handle)) {
      CertFreeCertificateContext(store_cert_context);
      CertCloseStore(cert_store, 0);
      free_imported_private_key(key_handle);
      return false;
   }

   clear_server_certificate(SSL);
   SSL->server_certificate_store = cert_store;
   SSL->server_certificate = store_cert_context;
   SSL->imported_private_key = key_handle;
   SSL->imported_private_key_owned = true;
   SSL->imported_private_key_persistent = false;
   return true;
}

//********************************************************************************************************************

bool ssl_get_verify_result(SSL_HANDLE SSL)
{
   if (!SSL) return false;

   if (!SSL->context_initialised) return false;

   PCCERT_CONTEXT cert_context = nullptr;
   SECURITY_STATUS status = QueryContextAttributes(&SSL->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert_context);

   if (status != SEC_E_OK) {
      SSL->last_security_status = status;
      return false;
   }

   if (!cert_context) return false;

   SecPkgContext_ConnectionInfo conn_info;
   status = QueryContextAttributes(&SSL->context, SECPKG_ATTR_CONNECTION_INFO, &conn_info);

   if (status != SEC_E_OK) {
      CertFreeCertificateContext(cert_context);
      SSL->last_security_status = status;
      return false;
   }

   if (conn_info.aiCipher IS 0 or conn_info.aiHash IS 0) {
      CertFreeCertificateContext(cert_context);
      return false;
   }

   CERT_CHAIN_PARA chain_para{};
   chain_para.cbSize = sizeof(CERT_CHAIN_PARA);

   PCCERT_CHAIN_CONTEXT chain_context = nullptr;
   BOOL chain_result = CertGetCertificateChain(nullptr, cert_context, nullptr, cert_context->hCertStore,
      &chain_para, 0, nullptr,&chain_context);

   bool result = true;

   if (!chain_result or !chain_context) {
      result = false;
   }
   else {
      CERT_TRUST_STATUS trust_status = chain_context->TrustStatus;
      if (trust_status.dwErrorStatus != CERT_TRUST_NO_ERROR) result = false;
   }

   if (chain_context) CertFreeCertificateChain(chain_context);
   CertFreeCertificateContext(cert_context);

   return result;
}
