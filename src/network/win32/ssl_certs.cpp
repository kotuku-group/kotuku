
//********************************************************************************************************************

static void clear_server_certificate(SSL_HANDLE SSL)
{
   if (SSL->server_certificate) {
      CertFreeCertificateContext(SSL->server_certificate);
      SSL->server_certificate = nullptr;
   }

   if (SSL->imported_private_key) {
      NCryptFreeObject(SSL->imported_private_key);
      SSL->imported_private_key = 0;
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
   NCryptBufferDesc *params = nullptr;

   if (Password.has_value()) {
      password_buffer.cbBuffer = DWORD((wide_password.size() + 1) * sizeof(wchar_t));
      password_buffer.BufferType = NCRYPTBUFFER_PKCS_SECRET;
      password_buffer.pvBuffer = (PVOID)wide_password.c_str();

      password_desc.ulVersion = NCRYPTBUFFER_VERSION;
      password_desc.cBuffers = 1;
      password_desc.pBuffers = &password_buffer;
      params = &password_desc;
   }

   auto result = try_import_pkcs8_private_key(provider, KeyDer, params, KeyHandle);
   if ((!result) and Password.has_value()) result = try_import_pkcs8_private_key(provider, KeyDer, nullptr, KeyHandle);

   NCryptFreeObject(provider);
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

static bool attach_private_key(PCCERT_CONTEXT Cert, NCRYPT_KEY_HANDLE KeyHandle)
{
   CERT_KEY_CONTEXT key_context {};
   key_context.cbSize = sizeof(key_context);
   key_context.hNCryptKey = KeyHandle;
   key_context.dwKeySpec = CERT_NCRYPT_KEY_SPEC;

   if (!CertSetCertificateContextProperty(Cert, CERT_KEY_CONTEXT_PROP_ID, CERT_SET_PROPERTY_INHIBIT_PERSIST_FLAG,
       &key_context)) {
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
   #ifdef PKCS12_NO_PERSIST_KEY
      import_flags |= PKCS12_NO_PERSIST_KEY;
   #endif

   HCERTSTORE pfx_store = PFXImportCertStore(&pfx_blob, wide_password.c_str(), import_flags);
   if (!pfx_store) return false;

   PCCERT_CONTEXT selected_cert = nullptr;
   PCCERT_CONTEXT cert_context = nullptr;

   while ((cert_context = CertEnumCertificatesInStore(pfx_store, cert_context))) {
      if (certificate_has_private_key(cert_context)) {
         selected_cert = CertDuplicateCertificateContext(cert_context);
         break;
      }
   }

   CertCloseStore(pfx_store, 0);
   if (!selected_cert) return false;

   clear_server_certificate(SSL);
   SSL->server_certificate = selected_cert;
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

   std::vector<BYTE> key_der;
   if ((!decode_pem_section(key_pem, "PRIVATE KEY", key_der)) and
       (!decode_pem_section(key_pem, "ENCRYPTED PRIVATE KEY", key_der))) {
      CertFreeCertificateContext(cert_context);
      return false;
   }

   NCRYPT_KEY_HANDLE key_handle = 0;
   if (!import_pkcs8_private_key(key_der, Password, key_handle)) {
      CertFreeCertificateContext(cert_context);
      return false;
   }

   if (!attach_private_key(cert_context, key_handle)) {
      NCryptFreeObject(key_handle);
      CertFreeCertificateContext(cert_context);
      return false;
   }

   clear_server_certificate(SSL);
   SSL->server_certificate = cert_context;
   SSL->imported_private_key = key_handle;
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

   if (conn_info.aiCipher == 0 or conn_info.aiHash == 0) {
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
