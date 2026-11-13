// This code implements the MD5 message-digest algorithm.  The algorithm is due to Ron Rivest.  Original code
// written by Colin Plumb in 1993, no copyright is claimed.  This code is in the public domain; do with it what you wish.
//
// To compute the message digest of a chunk of bytes, declare an MD5Context structure, pass it to MD5Init, call MD5Update as
// needed on buffers full of bytes, and then call MD5Final, which will fill a supplied 16-byte array with the digest.

#include <cstring>
#include <cstdint>
#include <bit>

struct alignas(16) MD5Context {
   uint32_t buf[4];
   uint64_t bits;
   uint32_t in[16]; // Message block.  Byte-level writes go through bytes(), which may alias any object type.

   unsigned char * bytes() noexcept { return reinterpret_cast<unsigned char *>(in); }
};

void MD5Init(MD5Context *) noexcept;
void MD5Update(MD5Context *, const unsigned char *, std::size_t) noexcept;
void MD5Final(unsigned char Digest[16], MD5Context *Context) noexcept;
void MD5Transform(uint32_t Buf[4], const uint32_t In[16]) noexcept;

inline void byteReverse(unsigned char *Buf, unsigned Longs) noexcept {
   if constexpr (std::endian::native IS std::endian::big) {
      uint32_t *words = reinterpret_cast<uint32_t*>(Buf);
      for (unsigned i = 0; i < Longs; ++i) {
         uint32_t t = words[i];
         words[i] = ((t & 0xFF000000) >> 24) | ((t & 0x00FF0000) >> 8) |
                   ((t & 0x0000FF00) << 8) | ((t & 0x000000FF) << 24);
      }
   }
}

// Start MD5 accumulation.  Set bit count to 0 and buffer to mysterious
// initialization constants.

void MD5Init(MD5Context *Context) noexcept
{
   constexpr uint32_t init_constants[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
   std::memcpy(Context->buf, init_constants, sizeof(init_constants));
   Context->bits = 0;
}

// Update context to reflect the concatenation of another buffer full
// of bytes.

void MD5Update(MD5Context *Context, const unsigned char *Buf, std::size_t Len) noexcept
{
   const uint64_t old_bits = Context->bits;
   Context->bits += Len * 8;

   const uint32_t bytes_in_buffer = (old_bits >> 3) & 0x3f;

   if (bytes_in_buffer) {
      unsigned char *p = Context->bytes() + bytes_in_buffer;
      const uint32_t bytes_needed = 64 - bytes_in_buffer;

      if (Len < bytes_needed) {
         std::memcpy(p, Buf, Len);
         return;
      }

      std::memcpy(p, Buf, bytes_needed);
      byteReverse(Context->bytes(), 16);
      MD5Transform(Context->buf, Context->in);
      Buf += bytes_needed;
      Len -= bytes_needed;
   }

   while (Len >= 64) {
      std::memcpy(Context->in, Buf, 64);
      byteReverse(Context->bytes(), 16);
      MD5Transform(Context->buf, Context->in);
      Buf += 64;
      Len -= 64;
   }

   if (Len > 0) std::memcpy(Context->in, Buf, Len);
}

// Final wrapup - pad to 64-byte boundary with the bit pattern
// 1 0* (64-bit count of bits processed, MSB-first)

void MD5Final(unsigned char Digest[16], MD5Context *Context) noexcept
{
   const uint32_t count = (Context->bits >> 3) & 0x3F;
   unsigned char *p = Context->bytes() + count;

   *p++ = 0x80;

   const uint32_t pad_bytes = 64 - 1 - count;

   if (pad_bytes < 8) {
      std::memset(p, 0, pad_bytes);
      byteReverse(Context->bytes(), 16);
      MD5Transform(Context->buf, Context->in);
      std::memset(Context->in, 0, 56);
   }
   else std::memset(p, 0, pad_bytes - 8);

   byteReverse(Context->bytes(), 14);

   Context->in[14] = uint32_t(Context->bits);
   Context->in[15] = uint32_t(Context->bits >> 32);

   MD5Transform(Context->buf, Context->in);
   byteReverse(reinterpret_cast<unsigned char*>(Context->buf), 4);
   std::memcpy(Digest, Context->buf, 16);

   std::memset(Context, 0, sizeof(*Context));
}

// The four core functions - F1 is optimized somewhat

// #define F1(x, y, z) (x & y | ~x & z)
constexpr uint32_t F1(uint32_t x, uint32_t y, uint32_t z) noexcept { return z ^ (x & (y ^ z)); }
constexpr uint32_t F2(uint32_t x, uint32_t y, uint32_t z) noexcept { return F1(z, x, y); }
constexpr uint32_t F3(uint32_t x, uint32_t y, uint32_t z) noexcept { return x ^ y ^ z; }
constexpr uint32_t F4(uint32_t x, uint32_t y, uint32_t z) noexcept { return y ^ (x | ~z); }

template<auto F>
constexpr void MD5STEP(uint32_t& w, uint32_t x, uint32_t y, uint32_t z, uint32_t data, int s) noexcept {
   w += F(x, y, z) + data;
   w = std::rotl(w, s);
   w += x;
}

// The core of the MD5 algorithm, this alters an existing MD5 hash to
// reflect the addition of 16 longwords of new data.  MD5Update blocks
// the data and converts bytes into longwords for this routine.

void MD5Transform(uint32_t Buf[4], const uint32_t In[16]) noexcept
{
   uint32_t a = Buf[0];
   uint32_t b = Buf[1];
   uint32_t c = Buf[2];
   uint32_t d = Buf[3];

   MD5STEP<F1>(a, b, c, d, In[0] + 0xd76aa478, 7);
   MD5STEP<F1>(d, a, b, c, In[1] + 0xe8c7b756, 12);
   MD5STEP<F1>(c, d, a, b, In[2] + 0x242070db, 17);
   MD5STEP<F1>(b, c, d, a, In[3] + 0xc1bdceee, 22);
   MD5STEP<F1>(a, b, c, d, In[4] + 0xf57c0faf, 7);
   MD5STEP<F1>(d, a, b, c, In[5] + 0x4787c62a, 12);
   MD5STEP<F1>(c, d, a, b, In[6] + 0xa8304613, 17);
   MD5STEP<F1>(b, c, d, a, In[7] + 0xfd469501, 22);
   MD5STEP<F1>(a, b, c, d, In[8] + 0x698098d8, 7);
   MD5STEP<F1>(d, a, b, c, In[9] + 0x8b44f7af, 12);
   MD5STEP<F1>(c, d, a, b, In[10] + 0xffff5bb1, 17);
   MD5STEP<F1>(b, c, d, a, In[11] + 0x895cd7be, 22);
   MD5STEP<F1>(a, b, c, d, In[12] + 0x6b901122, 7);
   MD5STEP<F1>(d, a, b, c, In[13] + 0xfd987193, 12);
   MD5STEP<F1>(c, d, a, b, In[14] + 0xa679438e, 17);
   MD5STEP<F1>(b, c, d, a, In[15] + 0x49b40821, 22);

   MD5STEP<F2>(a, b, c, d, In[1] + 0xf61e2562, 5);
   MD5STEP<F2>(d, a, b, c, In[6] + 0xc040b340, 9);
   MD5STEP<F2>(c, d, a, b, In[11] + 0x265e5a51, 14);
   MD5STEP<F2>(b, c, d, a, In[0] + 0xe9b6c7aa, 20);
   MD5STEP<F2>(a, b, c, d, In[5] + 0xd62f105d, 5);
   MD5STEP<F2>(d, a, b, c, In[10] + 0x02441453, 9);
   MD5STEP<F2>(c, d, a, b, In[15] + 0xd8a1e681, 14);
   MD5STEP<F2>(b, c, d, a, In[4] + 0xe7d3fbc8, 20);
   MD5STEP<F2>(a, b, c, d, In[9] + 0x21e1cde6, 5);
   MD5STEP<F2>(d, a, b, c, In[14] + 0xc33707d6, 9);
   MD5STEP<F2>(c, d, a, b, In[3] + 0xf4d50d87, 14);
   MD5STEP<F2>(b, c, d, a, In[8] + 0x455a14ed, 20);
   MD5STEP<F2>(a, b, c, d, In[13] + 0xa9e3e905, 5);
   MD5STEP<F2>(d, a, b, c, In[2] + 0xfcefa3f8, 9);
   MD5STEP<F2>(c, d, a, b, In[7] + 0x676f02d9, 14);
   MD5STEP<F2>(b, c, d, a, In[12] + 0x8d2a4c8a, 20);

   MD5STEP<F3>(a, b, c, d, In[5] + 0xfffa3942, 4);
   MD5STEP<F3>(d, a, b, c, In[8] + 0x8771f681, 11);
   MD5STEP<F3>(c, d, a, b, In[11] + 0x6d9d6122, 16);
   MD5STEP<F3>(b, c, d, a, In[14] + 0xfde5380c, 23);
   MD5STEP<F3>(a, b, c, d, In[1] + 0xa4beea44, 4);
   MD5STEP<F3>(d, a, b, c, In[4] + 0x4bdecfa9, 11);
   MD5STEP<F3>(c, d, a, b, In[7] + 0xf6bb4b60, 16);
   MD5STEP<F3>(b, c, d, a, In[10] + 0xbebfbc70, 23);
   MD5STEP<F3>(a, b, c, d, In[13] + 0x289b7ec6, 4);
   MD5STEP<F3>(d, a, b, c, In[0] + 0xeaa127fa, 11);
   MD5STEP<F3>(c, d, a, b, In[3] + 0xd4ef3085, 16);
   MD5STEP<F3>(b, c, d, a, In[6] + 0x04881d05, 23);
   MD5STEP<F3>(a, b, c, d, In[9] + 0xd9d4d039, 4);
   MD5STEP<F3>(d, a, b, c, In[12] + 0xe6db99e5, 11);
   MD5STEP<F3>(c, d, a, b, In[15] + 0x1fa27cf8, 16);
   MD5STEP<F3>(b, c, d, a, In[2] + 0xc4ac5665, 23);

   MD5STEP<F4>(a, b, c, d, In[0] + 0xf4292244, 6);
   MD5STEP<F4>(d, a, b, c, In[7] + 0x432aff97, 10);
   MD5STEP<F4>(c, d, a, b, In[14] + 0xab9423a7, 15);
   MD5STEP<F4>(b, c, d, a, In[5] + 0xfc93a039, 21);
   MD5STEP<F4>(a, b, c, d, In[12] + 0x655b59c3, 6);
   MD5STEP<F4>(d, a, b, c, In[3] + 0x8f0ccc92, 10);
   MD5STEP<F4>(c, d, a, b, In[10] + 0xffeff47d, 15);
   MD5STEP<F4>(b, c, d, a, In[1] + 0x85845dd1, 21);
   MD5STEP<F4>(a, b, c, d, In[8] + 0x6fa87e4f, 6);
   MD5STEP<F4>(d, a, b, c, In[15] + 0xfe2ce6e0, 10);
   MD5STEP<F4>(c, d, a, b, In[6] + 0xa3014314, 15);
   MD5STEP<F4>(b, c, d, a, In[13] + 0x4e0811a1, 21);
   MD5STEP<F4>(a, b, c, d, In[4] + 0xf7537e82, 6);
   MD5STEP<F4>(d, a, b, c, In[11] + 0xbd3af235, 10);
   MD5STEP<F4>(c, d, a, b, In[2] + 0x2ad7d2bb, 15);
   MD5STEP<F4>(b, c, d, a, In[9] + 0xeb86d391, 21);

   Buf[0] += a;
   Buf[1] += b;
   Buf[2] += c;
   Buf[3] += d;
}

