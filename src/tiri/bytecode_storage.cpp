
#include <kotuku/main.h>
#include <kotuku/modules/compression.h>

#include "bytecode_storage.h"
#include "defs.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>

namespace tiri::bytecode_storage {
namespace {

//********************************************************************************************************************
// Writes the complete byte sequence to an object, rejecting short writes so callers never accept truncated storage.

ERR write_complete(OBJECTPTR Output, std::string_view Bytes)
{
   size_t position = 0;
   while (position < Bytes.size()) {
      const auto count = std::min(Bytes.size() - position, size_t(std::numeric_limits<int>::max()));
      int written = 0;
      auto error = acWrite(Output, std::span((const int8_t *)Bytes.data() + position, count), &written);
      if (error != ERR::Okay) return error;
      if (written != int(count)) return ERR::Write;
      position += count;
   }
   return ERR::Okay;
}

} // namespace

//********************************************************************************************************************
// Compresses an internal VM bytecode payload as one bounded gzip member for persistent storage.

Error compress(std::string_view Input, std::string &Output)
{
   Output.clear();
   if (Input.size() > MAX_DECODED_SIZE) return Error::DECODED_LIMIT;

   objFile::create destination = { fl::Size(0), fl::Flags(FL::BUFFER|FL::READ|FL::WRITE) };
   if (not destination.ok()) return Error::COMPRESSION;

   objCompressedStream::create stream(NF::LOCAL);
   if (not stream.ok() or (stream->setOutput(*destination) != ERR::Okay) or
       (stream->setFormat(CF::GZIP) != ERR::Okay) or (stream->init() != ERR::Okay)) return Error::COMPRESSION;

   size_t position = 0;
   while (position < Input.size()) {
      const auto count = std::min(Input.size() - position, size_t(std::numeric_limits<int>::max()));
      int consumed = 0;
      auto error = stream->write(std::span((const int8_t *)Input.data() + position, count), &consumed);
      if ((error != ERR::Okay) or (consumed != int(count))) return Error::COMPRESSION;
      position += count;
   }

   if (stream->write(std::span<const int8_t>()) != ERR::Okay or not stream->Finished) return Error::COMPRESSION;
   if ((stream->TotalOutput < 0) or (uint64_t(stream->TotalOutput) > MAX_ENCODED_SIZE)) return Error::ENCODED_LIMIT;

   std::span<int8_t> bytes;
   if (destination->getBuffer(bytes) != ERR::Okay) return Error::COMPRESSION;
   if (uint64_t(bytes.size()) != uint64_t(stream->TotalOutput)) return Error::COMPRESSION;
   Output.assign((const char *)bytes.data(), bytes.size());
   return Error::OKAY;
}

//********************************************************************************************************************
// Decompresses and validates one bounded gzip member containing VM bytecode, with no trailing or concatenated data.

Error decompress(std::string_view Input, std::string &Output, uint64_t ExpectedSize, uint64_t DecodedLimit)
{
   Output.clear();
   if ((Input.size() < 3) or (uint8_t(Input[0]) != 0x1f) or (uint8_t(Input[1]) != 0x8b) or
       (uint8_t(Input[2]) != 8)) return Error::INVALID_GZIP;
   if (uint64_t(Input.size()) > MAX_ENCODED_SIZE) return Error::ENCODED_LIMIT;
   if ((ExpectedSize != UINT64_MAX) and (ExpectedSize > DecodedLimit)) return Error::DECODED_LIMIT;

   objFile::create source = {
      fl::Size(int64_t(Input.size())), fl::Flags(FL::BUFFER|FL::READ|FL::WRITE)
   };

   if (not source.ok() or (write_complete(*source, Input) != ERR::Okay) or
       (source->seekStart(0) != ERR::Okay)) return Error::DECOMPRESSION;

   objCompressedStream::create stream(NF::LOCAL);
   if (not stream.ok() or (stream->setInput(*source) != ERR::Okay) or
       (stream->setFormat(CF::GZIP) != ERR::Okay) or (stream->init() != ERR::Okay)) return Error::DECOMPRESSION;

   std::array<int8_t, 32 * 1024> buffer;
   while (not stream->Finished) {
      int produced = 0;
      if (stream->read(buffer, &produced) != ERR::Okay) {
         Output.clear();
         return Error::DECOMPRESSION;
      }

      if ((produced < 0) or (size_t(produced) > buffer.size())) {
         Output.clear();
         return Error::DECOMPRESSION;
      }

      if (uint64_t(produced) > DecodedLimit - uint64_t(Output.size())) {
         Output.clear();
         return Error::DECODED_LIMIT;
      }
      Output.append((const char *)buffer.data(), size_t(produced));
   }

   if ((stream->TotalInput < 0) or (uint64_t(stream->TotalInput) != uint64_t(Input.size()))) {
      Output.clear();
      return Error::TRAILING_DATA;
   }

   if ((ExpectedSize != UINT64_MAX) and (uint64_t(Output.size()) != ExpectedSize)) {
      Output.clear();
      return Error::DECOMPRESSION;
   }

   if (not Output.starts_with("\x1bLJ")) {
      Output.clear();
      return Error::INVALID_BYTECODE;
   }

   return Error::OKAY;
}

//********************************************************************************************************************
// Splits a persisted compiled wrapper into its optional identity token and gzip-encoded payload.

Error parse_wrapper(std::string_view Input, std::string_view &EncodedPayload, std::string_view *IdentityToken)
{
   constexpr size_t marker_size = sizeof(LUA_COMPILED) - 1;
   EncodedPayload = {};
   if (not Input.starts_with(LUA_COMPILED)) return Error::INVALID_WRAPPER;

   auto window = Input.substr(0, std::min(Input.size(), marker_size + MAX_IDENTITY_TOKEN + 1));
   auto separator = window.find('\0', marker_size);
   if (separator IS std::string_view::npos) return Error::INVALID_WRAPPER;

   if (IdentityToken) {
      auto token = Input.substr(marker_size, separator - marker_size);
      while ((not token.empty()) and (token.front() IS ' ')) token.remove_prefix(1);
      *IdentityToken = token;
   }

   EncodedPayload = Input.substr(separator + 1);
   if ((EncodedPayload.size() < 3) or (uint8_t(EncodedPayload[0]) != 0x1f) or
       (uint8_t(EncodedPayload[1]) != 0x8b) or (uint8_t(EncodedPayload[2]) != 8)) return Error::INVALID_GZIP;

   return Error::OKAY;
}

//********************************************************************************************************************
// Parses a compiled wrapper and decompresses its payload into validated internal VM bytecode.

Error decode_wrapper(std::string_view Input, std::string &Output, std::string_view *IdentityToken)
{
   std::string_view payload;
   if (auto error = parse_wrapper(Input, payload, IdentityToken); error != Error::OKAY) return error;
   return decompress(payload, Output);
}

//********************************************************************************************************************
// Returns the stable diagnostic name associated with a bytecode storage error.

const char *error_name(Error ErrorValue) noexcept
{
   switch (ErrorValue) {
      case Error::OKAY: return "okay";
      case Error::INVALID_WRAPPER: return "invalid-wrapper";
      case Error::INVALID_GZIP: return "invalid-gzip";
      case Error::ENCODED_LIMIT: return "encoded-size-limit";
      case Error::DECODED_LIMIT: return "decoded-size-limit";
      case Error::COMPRESSION: return "compression-failed";
      case Error::DECOMPRESSION: return "decompression-failed";
      case Error::TRAILING_DATA: return "trailing-data";
      case Error::INVALID_BYTECODE: return "invalid-bytecode";
   }
   return "unknown";
}

} // namespace tiri::bytecode_storage
