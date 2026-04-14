#pragma once

#include <kotuku/main.h>

int UTF8Copy(CSTRING String, STRING Dest, int Chars, int Size);
uint32_t UTF8ReadValue(CSTRING String, int *Length);
//CSTRING UTF8ValidEncoding(CSTRING String, CSTRING Encoding);
int UTF8WriteValue(int Value, STRING String, int StringSize);

/*********************************************************************************************************************

-FUNCTION-
UTF8CharLength: Returns the number of bytes used to define a single UTF-8 character.

This function will return the total number of bytes used to create a single UTF-8 character.  A pointer to the start of
the character string that is to be analysed is required.

-INPUT-
cstr String: Pointer to a UTF-8 string.

-RESULT-
int: Returns the number of bytes used to create the UTF-8 character referred to by the String argument.

*********************************************************************************************************************/

[[nodiscard]] static inline int UTF8CharLength(CSTRING String)
{
   if ((!String) or (!*String)) return 0;

   int total;
   for (total=1; ((String[total] & 0xc0) IS 0x80); total++);
   return total;
}

/*********************************************************************************************************************

-FUNCTION-
UTF8CharOffset: Retrieves the byte position of a character in a UTF-8 string.

Determines the true byte position of a character within a UTF-8 string, where Offset is a
column number.

-INPUT-
cstr String: A null-terminated UTF-8 string.
int Offset: The character number to translate to a byte offset.

-RESULT-
int: Returns the byte offset of the character.

*********************************************************************************************************************/

[[nodiscard]] static inline int UTF8CharOffset(CSTRING String, int Index)
{
   if (!String) return 0;

   int offset = 0;
   while ((String[offset]) and (Index > 0)) {
      for (++offset; ((String[offset] & 0xc0) IS 0x80); offset++);
      Index--;
   }
   return offset;
}

/*********************************************************************************************************************

-FUNCTION-
UTF8Length: Returns the total number of characters in a UTF-8 string.

This function will return the total number of decoded unicode characters in a UTF-8 string.

-INPUT-
cstr String: Pointer to a UTF-8 string.

-RESULT-
int: Returns the total number of characters used in the supplied UTF-8 string.

*********************************************************************************************************************/

[[nodiscard]] static inline int UTF8Length(CSTRING String)
{
   if (!String) return 0;

   int total;
   for (total=0; *String; total++) {
      for (++String; ((*String & 0xc0) IS 0x80); String++);
   }
   return total;
}

/*********************************************************************************************************************

-FUNCTION-
UTF8OffsetToChar: Converts a byte offset into a character position.

This function will convert a byte position in a UTF-8 string to its character column number.

-INPUT-
cstr String: A null-terminated UTF-8 string.
int Offset: The byte offset that you need a character number for.

-RESULT-
int: Returns the number of the character at the given byte position.

*********************************************************************************************************************/

[[nodiscard]] static inline int UTF8OffsetToChar(CSTRING String, int Offset)
{
   if (!String) return 0;

   int pos = 0;
   while ((Offset) and (String[pos])) {
      for (++pos; ((String[pos] & 0xc0) IS 0x80); pos++);
      Offset--;
   }
   return pos;
}

/*********************************************************************************************************************

-FUNCTION-
UTF8PrevLength: Gets the byte length of the previous character at a specific position in a UTF-8 string.

This function calculates the byte-length of the previous character in a string, from an initial position given by
Offset.

-INPUT-
cstr String: Pointer to a UTF-8 string.
int Offset: The byte index from which the size of the previous character should be calculated.

-RESULT-
int: Returns the byte-length of the previous character.

*********************************************************************************************************************/

[[nodiscard]] static inline int UTF8PrevLength(CSTRING String, int ByteIndex)
{
   int len = 0;
   for (--ByteIndex; ByteIndex > 0; --ByteIndex) {
      len++;
      if ((String[ByteIndex] & 0xc0) != 0x80) return len;
   }
   return len;
}