
//********************************************************************************************************************
// Decrypt any complete encrypted record that has already been supplied by the socket backend.

SSL_ERROR_CODE ssl_prepare_read(SSL_HANDLE SSL)
{
   if (!SSL) return SSL_ERROR_ARGS;
   if (!SSL->context_initialised) return SSL_ERROR_FAILED;
   if (SSL->decrypted_buffer.size() > SSL->decrypted_buffer_offset) return SSL_OK;
   if (SSL->recv_buffer.empty()) return SSL_ERROR_WOULD_BLOCK;
   if (SSL->stream_sizes.cbMaximumMessage IS 0) return SSL_ERROR_FAILED;

   std::array<SecBuffer, 4> buffers;
   auto recv_data = SSL->recv_buffer.used_data_mutable();
   buffers[0].pvBuffer = recv_data.data();
   buffers[0].cbBuffer = (uint32_t)recv_data.size();
   buffers[0].BufferType = SECBUFFER_DATA;

   buffers[1].BufferType = SECBUFFER_EMPTY;
   buffers[2].BufferType = SECBUFFER_EMPTY;
   buffers[3].BufferType = SECBUFFER_EMPTY;

   SecBufferDesc bufferDesc;
   bufferDesc.ulVersion = SECBUFFER_VERSION;
   bufferDesc.cBuffers = (uint32_t)buffers.size();
   bufferDesc.pBuffers = buffers.data();

   SECURITY_STATUS status = DecryptMessage(&SSL->context, &bufferDesc, 0, nullptr);

   if (status IS SEC_E_OK) {
      int extra_bytes = 0;
      unsigned char* decrypted_data = nullptr;
      size_t decrypted_size = 0;

      for (const auto& buf : buffers) {
         if ((buf.BufferType IS SECBUFFER_DATA) and buf.pvBuffer and buf.cbBuffer > 0) {
            decrypted_data = (unsigned char*)buf.pvBuffer;
            decrypted_size = buf.cbBuffer;
         }
         else if ((buf.BufferType IS SECBUFFER_EXTRA) and buf.cbBuffer > 0) {
            extra_bytes = int(buf.cbBuffer);
         }
      }

      if (decrypted_data and decrypted_size > 0) {
         SSL->decrypted_buffer.reset();
         SSL->decrypted_buffer.ensure_capacity(decrypted_size);
         std::span<const unsigned char> decrypted_span(decrypted_data, decrypted_size);
         SSL->decrypted_buffer.append(decrypted_span);
         SSL->decrypted_buffer_offset = 0;

         if (extra_bytes > 0) SSL->recv_buffer.compact(SSL->recv_buffer.size() - extra_bytes);
         else SSL->recv_buffer.reset();

         return SSL_OK;
      }

      if (extra_bytes > 0) SSL->recv_buffer.compact(SSL->recv_buffer.size() - extra_bytes);
      else SSL->recv_buffer.reset();
      return SSL_ERROR_WOULD_BLOCK;
   }
   else if (status IS SEC_E_INCOMPLETE_MESSAGE) {
      return SSL_ERROR_WOULD_BLOCK;
   }
   else if (status IS SEC_E_DECRYPT_FAILURE) {
      SSL->last_security_status = status;
      return SSL_ERROR_DISCONNECTED;
   }
   else if (status IS SEC_E_INVALID_TOKEN) {
      SSL->recv_buffer.reset();
      set_error_status(SSL, status);
      return SSL_ERROR_DISCONNECTED;
   }
   else {
      set_error_status(SSL, status);
      return SSL_ERROR_FAILED;
   }
}

//********************************************************************************************************************
// Read data from SSL connection (post-handshake).  Designed for non-blocking sockets only.

SSL_ERROR_CODE ssl_read(SSL_HANDLE SSL, void *Buffer, int BufferSize, int* BytesRead)
{
   if ((!SSL) or (!Buffer) or (BufferSize <= 0) or (!BytesRead)) {
      if (BytesRead) *BytesRead = 0;
      return SSL_ERROR_ARGS;
   }

   *BytesRead = 0;
   if (!SSL->context_initialised) return SSL_ERROR_FAILED;

   // Priority 1: Return any buffered decrypted data from previous calls
   if (SSL->decrypted_buffer.size() > SSL->decrypted_buffer_offset) {
      size_t available = SSL->decrypted_buffer.size() - SSL->decrypted_buffer_offset;
      size_t to_copy = std::min(size_t(BufferSize), available);

      memcpy(Buffer, SSL->decrypted_buffer.data() + SSL->decrypted_buffer_offset, to_copy);
      SSL->decrypted_buffer_offset += to_copy;

      if (SSL->decrypted_buffer_offset >= SSL->decrypted_buffer.size()) {
         SSL->decrypted_buffer.reset();
         SSL->decrypted_buffer_offset = 0;
      }

      *BytesRead = int(to_copy);
      return SSL_OK;
   }

   while (true) {
      if (!SSL->recv_buffer.empty()) {
         if (!SSL->context_initialised) {
            return SSL_ERROR_FAILED;
         }

         if (SSL->stream_sizes.cbMaximumMessage IS 0) {
            return SSL_ERROR_FAILED;
         }

         std::array<SecBuffer, 4> buffers;
         auto recv_data = SSL->recv_buffer.used_data_mutable();
         buffers[0].pvBuffer = recv_data.data();
         buffers[0].cbBuffer = (uint32_t)recv_data.size();
         buffers[0].BufferType = SECBUFFER_DATA;

         buffers[1].BufferType = SECBUFFER_EMPTY;
         buffers[2].BufferType = SECBUFFER_EMPTY;
         buffers[3].BufferType = SECBUFFER_EMPTY;

         SecBufferDesc bufferDesc;
         bufferDesc.ulVersion = SECBUFFER_VERSION;
         bufferDesc.cBuffers = (uint32_t)buffers.size();
         bufferDesc.pBuffers = buffers.data();

         SECURITY_STATUS status = DecryptMessage(&SSL->context, &bufferDesc, 0, nullptr);

         if (status IS SEC_E_OK) {
            int extra_bytes = 0;
            unsigned char* decrypted_data = nullptr;
            size_t decrypted_size = 0;
            for (const auto& buf : buffers) {
               if ((buf.BufferType IS SECBUFFER_DATA) and buf.pvBuffer and buf.cbBuffer > 0) {
                  decrypted_data = (unsigned char*)buf.pvBuffer;
                  decrypted_size = buf.cbBuffer;
               }
               else if ((buf.BufferType IS SECBUFFER_EXTRA) and buf.cbBuffer > 0) {
                  extra_bytes = int(buf.cbBuffer);
               }
            }

            if (decrypted_data and decrypted_size > 0) {
               SSL->decrypted_buffer.reset();
               SSL->decrypted_buffer.ensure_capacity(decrypted_size);
               std::span<const unsigned char> decrypted_span(decrypted_data, decrypted_size);
               SSL->decrypted_buffer.append(decrypted_span);
               SSL->decrypted_buffer_offset = 0;

               if (extra_bytes > 0) {
                  SSL->recv_buffer.compact(SSL->recv_buffer.size() - extra_bytes);
               }
               else {
                  SSL->recv_buffer.reset();
               }

               size_t to_copy = std::min(size_t(BufferSize), decrypted_size);
               memcpy(Buffer, SSL->decrypted_buffer.data(), to_copy);
               SSL->decrypted_buffer_offset = to_copy;

               if (to_copy >= decrypted_size) {
                  SSL->decrypted_buffer.reset();
                  SSL->decrypted_buffer_offset = 0;
               }

               *BytesRead = int(to_copy);
               return SSL_OK;
            }
            else {
               if (extra_bytes > 0) SSL->recv_buffer.compact(SSL->recv_buffer.size() - extra_bytes);
               else SSL->recv_buffer.reset();
            }
         }
         else if (status IS SEC_E_INCOMPLETE_MESSAGE) {
         }
         else {
            SSL->last_security_status = status;
            if (status IS SEC_E_DECRYPT_FAILURE) {
              return SSL_ERROR_DISCONNECTED;
            }
            else if (status IS SEC_I_RENEGOTIATE) {
               int extra_bytes = 0;
               for (const auto& buf : buffers) {
                  if ((buf.BufferType IS SECBUFFER_EXTRA) and buf.cbBuffer > 0) {
                     extra_bytes = int(buf.cbBuffer);
                     break;
                  }
               }

               if (extra_bytes > 0) {
                  SSL->recv_buffer.compact(SSL->recv_buffer.size() - extra_bytes);
               }
               else {
                  SSL->recv_buffer.reset();
               }

               set_error_status(SSL, status);
               return SSL_ERROR_FAILED;
            }
            else if (status IS 0x90321) {
               SSL->recv_buffer.reset();
               set_error_status(SSL, status);
               return SSL_ERROR_FAILED;
            }
            else if (status IS SEC_E_INVALID_TOKEN) {
               // This can happen if:
               // You fed in garbage or truncated bytes (common if you didn't wait for a full record and passed partial data).
               // The peer sent malformed TLS (e.g., not really TLS, wrong port).
               // The state machine is out of sync (e.g., you called ISC/ASC at the wrong time).
               // There's a protocol/cipher mismatch that manifests as the peer aborting the handshake.

               SSL->recv_buffer.reset();
               set_error_status(SSL, status);
               return SSL_ERROR_DISCONNECTED;
            }
            set_error_status(SSL, status);
            return SSL_ERROR_FAILED;
         }
      }

      return SSL_ERROR_WOULD_BLOCK;
   }
   return SSL_ERROR_FAILED;
}

//********************************************************************************************************************
// Write data to SSL connection

SSL_ERROR_CODE ssl_write(SSL_HANDLE SSL, const void* Buffer, size_t BufferSize, size_t* bytes_sent)
{
   if ((!SSL) or (!Buffer) or (BufferSize IS 0) or (!bytes_sent)) return SSL_ERROR_ARGS;

   *bytes_sent = 0;
   if (!SSL->context_initialised) return SSL_ERROR_FAILED;

   size_t header_size = SSL->stream_sizes.cbHeader;
   size_t trailer_size = SSL->stream_sizes.cbTrailer;
   size_t max_message_size = SSL->stream_sizes.cbMaximumMessage;

   size_t data_to_send = std::min(BufferSize, max_message_size);
   size_t total_size = header_size + data_to_send + trailer_size;

   SSL->send_buffer.ensure_capacity(total_size);
   SSL->send_buffer.resize(total_size);

   std::array<SecBuffer, 4> buffers;

   buffers[0].pvBuffer = SSL->send_buffer.data();
   buffers[0].cbBuffer = (uint32_t)header_size;
   buffers[0].BufferType = SECBUFFER_STREAM_HEADER;

   buffers[1].pvBuffer = SSL->send_buffer.data() + header_size;
   buffers[1].cbBuffer = (uint32_t)data_to_send;
   buffers[1].BufferType = SECBUFFER_DATA;
   memcpy(buffers[1].pvBuffer, Buffer, data_to_send);

   buffers[2].pvBuffer = SSL->send_buffer.data() + header_size + data_to_send;
   buffers[2].cbBuffer = (uint32_t)trailer_size;
   buffers[2].BufferType = SECBUFFER_STREAM_TRAILER;

   buffers[3].pvBuffer = nullptr;
   buffers[3].cbBuffer = 0;
   buffers[3].BufferType = SECBUFFER_EMPTY;

   SecBufferDesc bufferDesc;
   bufferDesc.ulVersion = SECBUFFER_VERSION;
   bufferDesc.cBuffers = (uint32_t)buffers.size();
   bufferDesc.pBuffers = buffers.data();

   auto status = EncryptMessage(&SSL->context, 0, &bufferDesc, 0);

   if (status != SEC_E_OK) {
      set_error_status(SSL, status);
      return SSL_ERROR_FAILED;
   }

   DWORD encrypted_size = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;

   ssl_debug_log(SSL_DEBUG_INFO, "SSL Write - queued %lu encrypted bytes for socket %d", encrypted_size,
      int(SSL->socket_handle));

   std::span<const unsigned char> encrypted_span(SSL->send_buffer.data(), size_t(encrypted_size));
   if (!SSL->handshake_buffer.append(encrypted_span)) {
      return SSL_ERROR_MEMORY;
   }

   *bytes_sent = data_to_send;
   return SSL_OK;
}
