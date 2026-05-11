#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

enum class SSLCERTFORMAT : uint8_t {
   NIL,
   PEM,
   PKCS12,
   PRIVATE_KEY
};

static std::string ssl_file_extension(std::string_view Path)
{
   auto dot = Path.find_last_of('.');
   if (dot >= Path.size()) return {};

   std::string ext(Path.substr(dot + 1));
   std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char Char) { return char(std::tolower(Char)); });
   return ext;
}

static SSLCERTFORMAT ssl_certificate_format(std::string_view Path)
{
   auto ext = ssl_file_extension(Path);

   if ((!ext.compare("pem")) or (!ext.compare("crt")) or (!ext.compare("cert"))) return SSLCERTFORMAT::PEM;
   if ((!ext.compare("p12")) or (!ext.compare("pfx"))) return SSLCERTFORMAT::PKCS12;

   return SSLCERTFORMAT::NIL;
}

static SSLCERTFORMAT ssl_private_key_format(std::string_view Path)
{
   auto ext = ssl_file_extension(Path);

   if ((!ext.compare("pem")) or (!ext.compare("key"))) return SSLCERTFORMAT::PRIVATE_KEY;

   return SSLCERTFORMAT::NIL;
}
