/*********************************************************************************************************************

The source code of the Kotuku project is made publicly available under the terms described in the LICENSE.TXT file
that is distributed with this package.  Please refer to it for further information on licensing.

**********************************************************************************************************************

RAII wrapper for a zlib z_stream.  This pairs the stream with the direction it is currently operating in, so that the
matching deflateEnd()/inflateEnd() call can never be forgotten, duplicated, or applied in the wrong direction.

The wrapper is pinned in place (move and copy deleted) because it is embedded directly as a member within its owning
class.  No throwing paths are present; the init helpers return the raw zlib result code in keeping with the project's
error-by-return-value convention.

This header must be included after zlib.h and the ZLIB_MEM_LEVEL macro have been defined.

*********************************************************************************************************************/

#pragma once

struct ZStream {
   z_stream stream;
   enum class Mode { NIL, INFLATE, DEFLATE } mode = Mode::NIL;

   ZStream() { clearmem(&stream, sizeof(stream)); }
   ~ZStream() { reset(); }

   ZStream(const ZStream &) = delete;
   ZStream(ZStream &&) = delete;
   ZStream & operator=(const ZStream &) = delete;
   ZStream & operator=(ZStream &&) = delete;

   // Begin a decompression stream.  Any previously active stream is terminated first.  Returns the raw zlib result
   // code, which is Z_OK on success.

   int inflate_init(int WindowBits) {
      reset();
      clearmem(&stream, sizeof(stream));
      auto err = inflateInit2(&stream, WindowBits);
      if (err IS Z_OK) mode = Mode::INFLATE;
      return err;
   }

   // Begin a compression stream.  Any previously active stream is terminated first.  Returns the raw zlib result
   // code, which is Z_OK on success.

   int deflate_init(int Level, int WindowBits) {
      reset();
      clearmem(&stream, sizeof(stream));
      auto err = deflateInit2(&stream, Level, Z_DEFLATED, WindowBits, ZLIB_MEM_LEVEL, Z_DEFAULT_STRATEGY);
      if (err IS Z_OK) mode = Mode::DEFLATE;
      return err;
   }

   // Terminate any active stream.  Idempotent and safe to call regardless of the current state.

   void reset() {
      if (mode IS Mode::INFLATE) inflateEnd(&stream);
      else if (mode IS Mode::DEFLATE) deflateEnd(&stream);
      mode = Mode::NIL;
   }

   bool active() const { return mode != Mode::NIL; }
   z_stream * get() { return &stream; }
   z_stream * operator-> () { return &stream; }
};
