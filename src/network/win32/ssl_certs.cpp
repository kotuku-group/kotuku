
//********************************************************************************************************************

bool load_pkcs12_certificate(SSL_HANDLE SSL, const std::string &Path)
{
   HANDLE p12_file = CreateFileA(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (p12_file == INVALID_HANDLE_VALUE) return false;

   DWORD p12_size = GetFileSize(p12_file, nullptr);
   if (p12_size == INVALID_FILE_SIZE) {
      CloseHandle(p12_file);
      return false;
   }

   std::vector<BYTE> p12_data(p12_size);
   DWORD bytes_read;
   if (!ReadFile(p12_file, p12_data.data(), p12_size, &bytes_read, nullptr) or bytes_read != p12_size) {
      CloseHandle(p12_file);
      return false;
   }
   CloseHandle(p12_file);

   CRYPT_DATA_BLOB pfx_blob;
   pfx_blob.cbData = p12_size;
   pfx_blob.pbData = p12_data.data();

   HCERTSTORE pfx_store = PFXImportCertStore(&pfx_blob, L"", CRYPT_EXPORTABLE | CRYPT_USER_KEYSET);
   if (!pfx_store) return false;

   // Find the certificate in the imported store
   PCCERT_CONTEXT cert_context = CertEnumCertificatesInStore(pfx_store, nullptr);
   if (!cert_context) {
      CertCloseStore(pfx_store, 0);
      return false;
   }

   PCCERT_CONTEXT final_cert = CertDuplicateCertificateContext(cert_context);

   HCERTSTORE personal_store = CertOpenSystemStore(0, "MY");
   if (personal_store) {
      CertAddCertificateContextToStore(personal_store, cert_context, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
      CertCloseStore(personal_store, 0);
   }

   CertCloseStore(pfx_store, 0);

   SSL->server_certificate = final_cert;
   return true;
}

//********************************************************************************************************************

bool load_pem_certificate(SSL_HANDLE SSL, const std::string &Path)
{
   HANDLE cert_file = CreateFileA(Path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
   if (cert_file == INVALID_HANDLE_VALUE) return false;

   DWORD cert_size = GetFileSize(cert_file, nullptr);
   if (cert_size == INVALID_FILE_SIZE) {
      CloseHandle(cert_file);
      return false;
   }

   std::vector<BYTE> cert_data(cert_size + 1);
   DWORD bytes_read;
   if (!ReadFile(cert_file, cert_data.data(), cert_size, &bytes_read, nullptr) or bytes_read != cert_size) {
      CloseHandle(cert_file);
      return false;
   }
   cert_data[cert_size] = 0;
   CloseHandle(cert_file);

   DWORD der_size = 0;
   if (!CryptStringToBinaryA((const char*)cert_data.data(), 0, CRYPT_STRING_BASE64HEADER, nullptr, &der_size, nullptr, nullptr)) {
      return false;
   }

   std::vector<BYTE> der_data(der_size);
   if (!CryptStringToBinaryA((const char*)cert_data.data(), 0, CRYPT_STRING_BASE64HEADER, der_data.data(), &der_size, nullptr, nullptr)) {
      return false;
   }

   PCCERT_CONTEXT cert_context = CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der_data.data(), der_size);
   if (!cert_context) {
      return false;
   }

   // Open the personal certificate store
   HCERTSTORE cert_store = CertOpenSystemStore(0, "MY");
   if (!cert_store) {
      CertFreeCertificateContext(cert_context);
      return false;
   }

   PCCERT_CONTEXT existing_cert = CertFindCertificateInStore(cert_store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_EXISTING, cert_context, nullptr);
   if (existing_cert) {
      CertFreeCertificateContext(cert_context);
      CertCloseStore(cert_store, 0);
      if (SSL->server_certificate) CertFreeCertificateContext(SSL->server_certificate);
      SSL->server_certificate = existing_cert;
      return true;
   }

   if (!CertAddCertificateContextToStore(cert_store, cert_context, CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
      CertFreeCertificateContext(cert_context);
      CertCloseStore(cert_store, 0);
      return false;
   }

   CertCloseStore(cert_store, 0);

   SSL->server_certificate = cert_context;
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
