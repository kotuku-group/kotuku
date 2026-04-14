
#include <kotuku/main.h>

/*
typedef void * iconv_t;
iconv_t (*iconv_open)(const char* tocode, const char* fromcode);
size_t  (*iconv)(iconv_t cd, const char** inbuf, size_t* inbytesleft,   char** outbuf, size_t* outbytesleft);
int     (*iconv_close)(iconv_t cd);
void    (*iconvlist)(int (*do_one)(unsigned int namescount, const char* const* names, void* data), void* data);

STRING glIconvBuffer = nullptr;
OBJECTPTR modIconv = nullptr;
static iconv_t glIconv = nullptr;

void free_iconv(void)
{
   if (modIconv) {
      if (glIconv) { iconv_close(glIconv); glIconv = nullptr; }
      if (glIconvBuffer) { FreeResource(glIconvBuffer); glIconvBuffer = nullptr; }

      FreeResource(modIconv);
      modIconv = nullptr;
   }
}
*/
/*********************************************************************************************************************

-FUNCTION-
UTF8Copy: Copies the characters of one string to another (UTF8).

This function copies part of one string over to another.  If the Chars value is `COPY_ALL` then this function will
copy the entire Src string to Dest.  If this function encounters the end of the Src string (null byte) while
copying, it will terminate the output string at the same point and return.

Please note that the Dest string will <i>always</i> be null-terminated by this function regardless of the Chars value.
For example, if `123` is copied into the middle of string `ABCDEFGHI` then the result would be `ABC123` and the `GHI`
part of the string would be lost.

-INPUT-
cstr Src:  Pointer to the source string.
str Dest:  Pointer to the destination buffer.
int Chars: The maximum number of UTF8 characters to copy.  Can be set to `COPY_ALL` to copy up to the null terminator of the Src.
int Size:  Byte size of the destination buffer.

-RESULT-
int: Returns the total amount of <i>bytes</i> that were copied, not including the null byte at the end.

*********************************************************************************************************************/

int UTF8Copy(CSTRING String, STRING Dest, int Chars, int Size)
{
   if (!Dest) return 0;
   if (Size < 1) return 0;
   if ((Chars < 0) or (!String)) {
      Dest[0] = 0;
      return 0;
   }

   uint8_t copy;
   int i = 0;
   while (Chars > 0) {
      // Determine the number of bytes to copy for this character

      if (!*String) break;
      else if ((uint8_t)(*String) < 128) copy = 1;
      else if ((*String & 0xe0) IS 0xc0) copy = 2;
      else if ((*String & 0xf0) IS 0xe0) copy = 3;
      else if ((*String & 0xf8) IS 0xf0) copy = 4;
      else if ((*String & 0xfc) IS 0xf8) copy = 5;
      else if ((*String & 0xfc) IS 0xfc) copy = 6;
      else copy = 1;

      // Check if there's enough room to accept the number of bytes and the null byte

      if (i + copy + 1 >= Size) break;

      // Do the copy

      Dest[i++] = *String++; // First character
      for (uint8_t j=1; j < copy; j++) { // Subsequent characters are subject to UTF8 validity
         if ((*String & 0xc0) != 0x80) break;
         Dest[i++] = *String++;
      }

      Chars--;
   }

   Dest[i] = 0;
   return i;
}

/*********************************************************************************************************************

-FUNCTION-
UTF8ReadValue: Converts UTF-8 characters into 32-bit unicode values.

This function converts UTF-8 character strings into 32-bit unicode values.  To determine how many bytes were used in
making up the UTF-8 character, set the Length argument so that the function can return the total number of bytes.

-INPUT-
cstr String: Pointer to a character in a UTF-8 string that you want to convert.
&int Length: Optional argument that will store the resulting number of bytes that make up the UTF-8 character.

-RESULT-
uint: Returns the extracted unicode value.  If a failure occurs (the encoding is invalid) then a value of zero is returned. Zero can also be returned if the String parameter is NULL or begins with a null character.

*********************************************************************************************************************/

uint32_t UTF8ReadValue(CSTRING String, int *Length)
{
   uint32_t code;
   const char *str;

   if (!(str = String)) {
      if (Length) *Length = 0;
      return 0;
   }

   if (*str IS 0) {
      if (Length) *Length = 0;
      return 0;
   }
   else if ((uint8_t)(*str) < 128) {
      if (Length) *Length = 1;
      return *str;
   }
   else if ((*str & 0xe0) IS 0xc0) {
      if (Length) *Length = 2;
      return ((str[0] & 0x1f)<<6) | (str[1] & 0x3f);
   }
   else if ((*str & 0xf0) IS 0xe0) {
      if (Length) *Length = 3;
      code = *str & 0x0f;
      for (int16_t i=1; i < 3; i++) {
         if ((str[i] & 0xc0) != 0x80) return 0;
         code = (code<<6) | (str[i] & 0x3f);
      }
   }
   else if ((*str & 0xf8) IS 0xf0) {
      if (Length) *Length = 4;
      code = *str & 0x07;
      for (int16_t i=1; i < 4; i++) {
         if ((str[i] & 0xc0) != 0x80) return 0;
         code = (code<<6) | (str[i] & 0x3f);
      }
   }
   else if ((*str & 0xfc) IS 0xf8) {
      if (Length) *Length = 5;
      code = *str & 0x03;
      for (int16_t i=1; i < 5; i++) {
         if ((str[i] & 0xc0) != 0x80) return 0;
         code = (code<<6) | (str[i] & 0x3f);
      }
   }
   else if ((*str & 0xfc) IS 0xfc) {
      if (Length) *Length = 6;
      code = *str & 0x01;
      for (int16_t i=1; i < 6; i++) {
         if ((str[i] & 0xc0) != 0x80) return 0;
         code = (code<<6) | (str[i] & 0x3f);
      }
   }
   else {
      if (Length) *Length = 1;
      return 0;
   }

   return code;
}

/*********************************************************************************************************************

-FUNCTION-
UTF8ValidEncoding: Corrects invalid UTF-8 encodings and converts string encodings to UTF-8.

This function recovers strings that should be in UTF-8 format but could contain characters from another type of
character encoding.  Such uncertainty can occur - for example - when file names in a filesystem are expected to be in
UTF-8 format by default, but there is no absolute guarantee that this rule has been followed by the user or the
installed programs.

When processing the String, any non-UTF8 characters are converted from the given Encoding into UTF-8 format.  Any
invalid characters will be converted to a unicode value of 0xfffd.

Conversion support is provided by the iconv library.  The following encodings are supported:

<types>
<type name="European">ASCII, ISO-8859-{1,2,3,4,5,7,9,10,13,14,15,16}, KOI8-R, KOI8-U, KOI8-RU, CP{1250,1251,1252,1253,1254,1257}, CP{850,866}, Mac{Roman,CentralEurope,Iceland,Croatian,Romania}, Mac{Cyrillic,Ukraine,Greek,Turkish}, Macintosh, CP{437,737,775,852,853,855,857,858,860,861,863,865,869,1125}</>
<type name="Semitic">ISO-8859-{6,8}, CP{1255,1256}, CP862, Mac{Hebrew,Arabic}, CP864</>
<type name="Japanese">EUC-JP, SHIFT_JIS, CP932, ISO-2022-JP, ISO-2022-JP-2, ISO-2022-JP-1, EUC-JISX0213, Shift_JISX0213, ISO-2022-JP-3</>
<type name="Chinese">EUC-CN, HZ, GBK, GB18030, EUC-TW, BIG5, CP950, BIG5-HKSCS, ISO-2022-CN, ISO-2022-CN-EXT</>
<type name="Korean">EUC-KR, CP949, ISO-2022-KR, JOHAB</>
<type name="Armenian">ARMSCII-8</>
<type name="Georgian">Georgian-Academy, Georgian-PS</>
<type name="Tajik">KOI8-T</>
<type name="Thai">TIS-620, CP874, MacThai</>
<type name="Laotian">MuleLao-1, CP1133</>
<type name="Vietnamese">VISCII, TCVN, CP1258</>
<type name="Platform specifics">HP-ROMAN8, NEXTSTEP</>
<type name="Full Unicode">UTF-8 UCS-2, UCS-2BE, UCS-2LE UCS-4, UCS-4BE, UCS-4LE, UTF-16, UTF-16BE, UTF-16LE, UTF-32, UTF-32BE, UTF-32LE, UTF-7, C99, JAVA, UCS-2-INTERNAL, UCS-4-INTERNAL</>
<type name="Locale Dependent">char, wchar_t</>
<type name="Turkmen">TDS565</>
</types>

-INPUT-
cstr String:   The string to be validated.
cstr Encoding: The encoding that should be tried for invalid UTF-8 characters.  Set to zero if the encoding is unknown (the system default will be used if the encoding cannot be determined).

-RESULT-
cstr: Returns the original string pointer if it is already valid, otherwise a converted string is returned.  The converted string remains valid up until the next call to UTF8ValidEncoding().  A return of NULL is possible if an internal error occurs during the conversion process (e.g. invalid encoding type).

*********************************************************************************************************************/
#if 0
CSTRING UTF8ValidEncoding(CSTRING String, CSTRING Encoding)
{
   static int buffersize = 0;
   static uint32_t icvhash = 0;
   static bool init_failed = false;
   CSTRING str, output, input;
   uint32_t uchar, enchash;
   int len, in, out;
   size_t inleft, outleft;

   if ((!String) or (init_failed)) {
      if (glIconvBuffer) {
         // Calling this function with a NULL String is an easy/valid way to free the internal buffer
         FreeResource(glIconvBuffer);
         glIconvBuffer = nullptr;
         buffersize = 0;
      }
      return nullptr;
   }

   auto context = tlContext;
   tlContext = &glTopContext;

   if (!modIconv) {
      #ifdef _WIN32
         modIconv = objModule::create::global(fl::Name("libiconv2.dll"), fl::Flags(MOF::LINK_LIBRARY|MOF::STATIC));

         if (!modIconv) {
            init_failed = true;
            tlContext   = context;
            return nullptr;
         }

         modResolveSymbol(modIconv, "libiconv_open", (APTR *)&iconv_open);
         modResolveSymbol(modIconv, "libiconv_close", (APTR *)&iconv_close);
         modResolveSymbol(modIconv, "libiconv", (APTR *)&iconv);

         if ((!iconv) or (!iconv_open) or (!iconv_close)) {
            FreeResource(modIconv);
            modIconv    = nullptr;
            tlContext   = context;
            init_failed = true;
            return nullptr;
         }
      #else
         modIconv = objModule::create::global(fl::Name("libiconv2"), fl::Flags(MOF::LINK_LIBRARY));

         if (!modIconv) {
            init_failed = true;
            tlContext   = context;
            return nullptr;
         }
      #endif
   }

   // Check if the string is valid UTF-8 and if so, return

   str    = String;
   in     = 0; // Current input index
   out    = 0; // No of bytes written
   while (str[in]) {
      uchar = UTF8ReadValue(str+in, &len);

      if (!uchar) {
         // An invalid character has been found

         if (!Encoding) {
            Encoding = "char"; // Convert from the system default
         }

         // Initialise iconv

         enchash = strihash(Encoding);
         if ((glIconv) and (enchash IS icvhash)); // Correct iconv encoding is already loaded.
         else {
            if (glIconv) iconv_close(glIconv);

            glIconv = iconv_open("UTF-8", Encoding);

            if (!glIconv) {
               tlContext = context;
               return nullptr;
            }
            icvhash = enchash;
         }

         // Allocate a conversion buffer if we don't already have one

         if (!glIconvBuffer) {
            buffersize = 4096;
            if (buffersize < in) buffersize = in + 1024;

            if (AllocMemory(buffersize, MEM::STRING|MEM::NO_CLEAR, (APTR *)&glIconvBuffer, nullptr) != ERR::Okay) {
               tlContext = context;
               return nullptr;
            }
         }

         // Copy all characters up to the point at which the last invalid character was encountered.

         if (in > 0) copymem(str, glIconvBuffer, in);

         while (str[in]) {
            // Check/Expand the buffer size

            if (out+12 > buffersize) {
               if (ReallocMemory(glIconvBuffer, buffersize + 4096, (APTR *)&glIconvBuffer, nullptr) != ERR::Okay) {
                  tlContext = context;
                  return nullptr;
               }
               buffersize += 4096;
            }

            uchar = UTF8ReadValue(str+in, &len);

            if (!uchar) {
               // Attempt encoding conversion

               input = str + in;
               inleft = 1;
               output = glIconvBuffer + out;
               outleft = buffersize - out;
               if (iconv(glIconv, (const char **)&input, &inleft, (char **)&output, &outleft) != (size_t)-1) {
                  out += (buffersize - out) - outleft;
               }
               else {
                  // Failed to convert character.  Unknown characters are converted to 0xFFFD (the UTF-8 'replacement
                  // character'

                  out += UTF8WriteValue(0xfffd, glIconvBuffer + out, buffersize - out);
               }

               in++;
            }
            else while (len-- > 0) glIconvBuffer[out++] = str[in++];
         }

         glIconvBuffer[out] = 0;
         tlContext = context;
         return glIconvBuffer;
      }
      else {
         if (len) in += len;
         else in++;
      }
   }

   tlContext = context;
   return String;
}
#endif
/*********************************************************************************************************************

-FUNCTION-
UTF8WriteValue: Writes a 32-bit unicode value into a UTF-8 character buffer.

This function is used to write out unicode values in UTF-8 string format.  You need to provide a string buffer, keeping
in mind that anything from 1 to 6 bytes may be written to the buffer, depending on the size of the unicode value.

If the buffer is not large enough to hold the UTF-8 string then the function will do nothing and return a value of
zero.  Otherwise, the character will be written and the total number of bytes used will be returned.

This function will not add a null terminator to the end of the UTF-8 string.

-INPUT-
int Value:  A 32-bit unicode value.
buf(str) Buffer: Pointer to a string buffer that will hold the UTF-8 characters.
bufsize Size:   The size of the destination string buffer (does not need to be any larger than 6 bytes).

-RESULT-
int: Returns the total amount of characters written to the string buffer.

-END-

*********************************************************************************************************************/

int UTF8WriteValue(int Value, STRING String, int StringSize)
{
   if (Value < 128) {
      if (Value < 0) return 0;
      *String = (uint8_t)Value;
      return 1;
   }
   else if (Value < 0x800) {
      if (StringSize < 2) return 0;
      String[1] = (Value & 0x3f) | 0x80;
      Value  = Value>>6;
      String[0] = Value | 0xc0;
      return 2;
   }
   else if (Value < 0x10000) {
      if (StringSize < 3) return 0;
      String[2] = (Value & 0x3f)|0x80;
      Value  = Value>>6;
      String[1] = (Value & 0x3f)|0x80;
      Value  = Value>>6;
      String[0] = Value | 0xe0;
      return 3;
   }
   else if (Value < 0x200000) {
      if (StringSize < 4) return 0;
      String[3] = (Value & 0x3f)|0x80;
      Value  = Value>>6;
      String[2] = (Value & 0x3f)|0x80;
      Value  = Value>>6;
      String[1] = (Value & 0x3f)|0x80;
      Value  = Value>>6;
      String[0] = Value | 0xf0;
      return 4;
   }
   else if (Value < 0x4000000) {
      if (StringSize < 5) return 0;
      String[4]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[3]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[2]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[1]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[0]  = Value | 0xf8;
      return 5;
   }
   else {
      if (StringSize < 6) return 0;
      String[5]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[4]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[3]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[2]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[1]  = (Value & 0x3f)|0x80;
      Value = Value>>6;
      String[0]  = Value | 0xfc;
      return 6;
   }
}
