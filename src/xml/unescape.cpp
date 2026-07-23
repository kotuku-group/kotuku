//********************************************************************************************************************
// XML Entity Decoding and Character Reference Processing
//
// Provides functions for decoding XML entity references and numeric character references in XML content.  This
// module handles both named entities (like &amp;, &lt;, &gt;) and numeric character references (&#xHH; and &#DDD;)
// according to XML specifications.
//
// Key functionality:
//   - Numeric character reference decoding (decimal and hexadecimal)
//   - Unicode codepoint to UTF-8 conversion
//   - Entity reference lookup and substitution
//   - Character validation and bounds checking
//
// The implementation ensures safe decoding of XML escape sequences whilst preserving Unicode correctness and
// handling edge cases like invalid references and out-of-range codepoints.

#include "unescape.h"
#include <stdint.h>
#include <string.h>
#include "xml.h"
#include "../link/unicode.h"

#define IS ==

bool decode_numeric_reference(const char *Start, size_t Length, char *Buffer, size_t &ResultLength)
{
   if (Length < 3) return false;
   if (!(Start[0] IS '&') or !(Start[1] IS '#') or !(Start[Length - 1] IS ';')) return false;

   size_t position = 2;
   bool is_hex = false;
   if ((position < Length - 1) and ((Start[position] IS 'x') or (Start[position] IS 'X'))) {
      is_hex = true;
      position++;
   }

   uint32_t unicode = 0;
   while (position < Length - 1) {
      auto digit = Start[position];
      if (is_hex) {
         uint32_t value = 0;
         if ((digit >= '0') and (digit <= '9')) value = uint32_t(digit - '0');
         else if ((digit >= 'a') and (digit <= 'f')) value = uint32_t(digit - 'a' + 10);
         else if ((digit >= 'A') and (digit <= 'F')) value = uint32_t(digit - 'A' + 10);
         else return false;
         unicode <<= 4;
         unicode += value;
      }
      else {
         if ((digit < '0') or (digit > '9')) return false;
         unicode *= 10;
         unicode += uint32_t(digit - '0');
      }
      position++;
   }

   ResultLength = UTF8WriteValue(unicode, std::span<int8_t>((int8_t *)Buffer, 6));
   return true;
}

static ankerl::unordered_dense::map<std::string, std::string> glOfficial = {
   { "amp",  "&" },
   { "lt",   "<" },
   { "gt",   ">" },
   { "apos", "\'" },
   { "quot", "\"" }
};

static ankerl::unordered_dense::map<std::string, uint16_t> glHTML = {
   { "AElig",   0xc6 },
   { "Aacute",  0xC1 },
   { "Acirc",   0xC2 },
   { "Agrave",  0xC0 },
   { "Alpha",   0x391 },
   { "Aring",   0xC5 },
   { "Atilde",  0xC3 },
   { "Auml",    0xC4 },
   { "Beta",    0x392 },
   { "Ccedil",  0xC7 },
   { "Chi",     0x3A7 },
   { "Dagger",  0x2021 },
   { "Delta",   0x394 },
   { "ETH",     0xD0 },
   { "Eacute",  0xC9 },
   { "Ecirc",   0xCA },
   { "Egrave",  0xC8 },
   { "Epsilon", 0x395 },
   { "Eta",     0x397 },
   { "Euml",    0xCB },
   { "Gamma",   0x393 },
   { "Iacute",  0xCD },
   { "Icirc",   0xCE },
   { "Igrave",  0xCC },
   { "Iota",    0x399 },
   { "Iuml",    0xCF },
   { "Kappa",   0x39A },
   { "Lambda",  0x39B },
   { "Mu",      0x39C },
   { "Ntilde",  0xD1 },
   { "Nu",      0x39D },
   { "OElig",   0x152 },
   { "Oacute",  0xD3 },
   { "Ocirc",   0xD4 },
   { "Ograve",  0xD2 },
   { "Omega",   0x3A9 },
   { "Omicron", 0x39F },
   { "Oslash",  0xD8 },
   { "Otilde",  0xD5 },
   { "Ouml",    0xD6 },
   { "Phi",     0x3A6 },
   { "Pi",      0x3A0 },
   { "Prime",   0x2033 },
   { "Psi",     0x3A8 },
   { "Rho",     0x3A1 },
   { "Scaron",  0x160 },
   { "Sigma",   0x3A3 },
   { "THORN",   0xDE },
   { "Tau",     0x3A4 },
   { "Theta",   0x398 },
   { "Uacute",  0xDA },
   { "Ucirc",   0xDB },
   { "Ugrave",  0xD9 },
   { "Upsilon", 0x3A5 },
   { "Uuml",    0xDC },
   { "Xi",      0x39E },
   { "Yacute",  0xDD },
   { "Yuml",    0x178 },
   { "Zeta",    0x396 },
   { "aacute",  0xE1 },
   { "acirc",   0xE2 },
   { "acute",   0xB4 },
   { "aelig",   0xE6 },
   { "agrave",  0xE0 },
   { "alefsym", 0x2135 },
   { "alpha",   0x3B1 },
   { "and",     0x2227 },
   { "ang",     0x2220 },
   { "aring",   0xE5 },
   { "asymp",   0x2248 },
   { "atilde",  0xE3 },
   { "auml",    0xE4 },
   { "bdquo",   0x201E },
   { "beta",    0x3B2 },
   { "brvbar",  0xA6 },
   { "bull",    0x2022 },
   { "cap",     0x2229 },
   { "ccedil",  0xE7 },
   { "cedil",   0xB8 },
   { "cent",    0xA2 },
   { "chi",     0x3C7 },
   { "circ",    0x2C6 },
   { "clubs",   0x2663 },
   { "cong",    0x2245 },
   { "copy",    0xA9 },
   { "crarr",   0x21B5 },
   { "cup",     0x222A },
   { "curren",  0xA4 },
   { "dArr",    0x21D3 },
   { "dagger",  0x2020 },
   { "darr",    0x2193 },
   { "deg",     0xB0 },
   { "delta",   0x3B4 },
   { "diams",   0x2666 },
   { "divide",  0xF7 },
   { "eacute",  0xE9 },
   { "ecirc",   0xEA },
   { "egrave",  0xE8 },
   { "empty",   0x2205 },
   { "emsp",    0x2003 },
   { "ensp",    0x2002 },
   { "epsilon", 0x3B5 },
   { "equiv",   0x2261 },
   { "eta",     0x3B7 },
   { "eth",     0xF0 },
   { "euml",    0xEB },
   { "euro",    0x20AC },
   { "exist",   0x2203 },
   { "fnof",    0x192 },
   { "forall",  0x2200 },
   { "frac12",  0xBD },
   { "frac14",  0xBC },
   { "frac34",  0xBE },
   { "frasl",   0x2044 },
   { "gamma",   0x3B3 },
   { "ge",      0x2265 },
   { "gt",      0x3E },
   { "hArr",    0x21D4 },
   { "harr",    0x2194 },
   { "hearts",  0x2665 },
   { "hellip",  0x2026 },
   { "iacute",  0xED },
   { "icirc",   0xEE },
   { "iexcl",   0xA1 },
   { "igrave",  0xEC },
   { "image",   0x2111 },
   { "infin",   0x221E },
   { "int",     0x222B },
   { "iota",    0x3B9 },
   { "iquest",  0xBF },
   { "isin",    0x2208 },
   { "iuml",    0xEF },
   { "kappa",   0x3BA },
   { "lArr",    0x21D0 },
   { "lambda",  0x3BB },
   { "lang",    0x2329 },
   { "laquo",   0xAB },
   { "larr",    0x2190 },
   { "lceil",   0x2308 },
   { "ldquo",   0x201C },
   { "le",      0x2264 },
   { "lfloor",  0x230A },
   { "lowast",  0x2217 },
   { "loz",     0x25CA },
   { "lrm",     0x200E },
   { "lsaquo",  0x2039 },
   { "lsquo",   0x2018 },
   { "lt",     '<' },
   { "macr",    0xAF },
   { "mdash",   0x2014 },
   { "micro",   0xB5 },
   { "middot",  0xB7 },
   { "minus",   0x2212 },
   { "mu",      0x3BC },
   { "nabla",   0x2207 },
   { "nbsp",    0xA0 },
   { "ndash",   0x2013 },
   { "ne",      0x2260 },
   { "ni",      0x220B },
   { "not",     0xAC },
   { "notin",   0x2209 },
   { "nsub",    0x2284 },
   { "ntilde",  0xF1 },
   { "nu",      0x3BD },
   { "oacute",  0xF3 },
   { "ocirc",   0xF4 },
   { "oelig",   0x153 },
   { "ograve",  0xF2 },
   { "oline",   0x203E },
   { "omega",   0x3C9 },
   { "omicron", 0x3BF },
   { "oplus",   0x2295 },
   { "or",      0x2228 },
   { "ordf",    0xAA },
   { "ordm",    0xBA },
   { "oslash",  0xF8 },
   { "otilde",  0xF5 },
   { "otimes",  0x2297 },
   { "ouml",    0xF6 },
   { "para",    0xB6 },
   { "part",    0x2202 },
   { "permil",  0x2030 },
   { "perp",    0x22A5 },
   { "phi",     0x3D5 },
   { "pi",      0x3C0 },
   { "piv",     0x3D6 },
   { "plusmn",  0xB1 },
   { "pound",   0xA3 },
   { "prime",   0x2032 },
   { "prod",    0x220F },
   { "prop",    0x221D },
   { "psi",     0x3C8 },
   { "quot",    0x22 },
   { "rArr",    0x21D2 },
   { "radic",   0x221A },
   { "rang",    0x232A },
   { "raquo",   0xBB },
   { "rarr",    0x2192 },
   { "rceil",   0x2309 },
   { "rdquo",   0x201D },
   { "real",    0x211C },
   { "reg",     0xAE },
   { "rfloor",  0x230B },
   { "rho",     0x3C1 },
   { "rlm",     0x200F },
   { "rsaquo",  0x203A },
   { "rsquo",   0x2019 },
   { "sbquo",   0x201A },
   { "scaron",  0x161 },
   { "sdot",    0x22C5 },
   { "sect",    0xA7 },
   { "shy",     0xAD },
   { "sigma",   0x3C3 },
   { "sigmaf",  0x3C2 },
   { "sim",     0x223C },
   { "spades",  0x2660 },
   { "sub",     0x2282 },
   { "sube",    0x2286 },
   { "sum",     0x2211 },
   { "sup",     0x2283 },
   { "sup1",    0xB9 },
   { "sup2",    0xB2 },
   { "sup3",    0xB3 },
   { "supe",    0x2287 },
   { "szlig",   0xDF },
   { "tau",     0x3C4 },
   { "there4",  0x2234 },
   { "theta",   0x3B8 },
   { "thetasym",0x3D1 },
   { "thinsp",  0x2009 },
   { "thorn",   0xFE },
   { "tilde",   0x2DC },
   { "times",   0xD7 },
   { "trade",   0x2122 },
   { "uArr",    0x21D1 },
   { "uacute",  0xFA },
   { "uarr",    0x2191 },
   { "ucirc",   0xFB },
   { "ugrave",  0xF9 },
   { "uml",     0xA8 },
   { "upsih",   0x3D2 },
   { "upsilon", 0x3C5 },
   { "uuml",    0xFC },
   { "weierp",  0x2118 },
   { "xi",      0x3BE },
   { "yacute",  0xFD },
   { "yen",     0xA5 },
   { "yuml",    0xFF },
   { "zeta",    0x3B6 },
   { "zwj",     0x200D },
   { "zwnj",    0x200C }
};

static void xml_unescape(extXML *Self, std::string &String)
{
   auto c = String.find('&');
   while (c != std::string::npos) {
      if ((c + 1 < String.size()) and (String[c + 1] IS '#')) {
         auto end = String.find(';', c);
         if (!(end IS std::string::npos)) {
            char unichar[6];
            size_t ulen = 0;
            if (decode_numeric_reference(String.c_str() + c, end - c + 1, unichar, ulen)) {
               String.replace(c, end - c + 1, unichar, ulen);
               c += ulen;
               c = String.find('&', c);
               continue;
            }
         }
         c++;
      }
      else {
         auto end = String.find(';', c);
         if (end != std::string::npos) {
            auto lookup = String.substr(c+1, end-c-1);
            end++;

            if (glOfficial.contains(lookup)) { // The five official XML escape codes
               String.replace(c, end-c, glOfficial[lookup]);
               c++;
            }
            else if ((Self->Flags & XMF::PARSE_ENTITY) != XMF::NIL) {
               std::string resolved;
               if (!Self->resolveEntity(lookup, resolved)) {
                  String.replace(c, end - c, resolved);
                  // TODO: Rescan the inserted text to handle nested entity references.
               }
               else c++;
            }
            else if ((Self->Flags & XMF::PARSE_HTML) != XMF::NIL) { // Process HTML escape codes
               if (glHTML.contains(lookup)) {
                  auto unicode = glHTML[lookup];
                  char unichar[6];
                  auto unilen = UTF8WriteValue(unicode,
                     std::span<int8_t>((int8_t *)unichar, sizeof(unichar)));
                  String.replace(c, end - c, unichar, unilen);
                  c += unilen;
               }
               else c++;
            }
            else c++;
         }
         else c++;
      }
      c = String.find('&', c);
   }
}

//********************************************************************************************************************

void unescape_all(extXML *Self, TAGS &Tags)
{
   for (auto &tag : Tags) {
      if (!tag.Children.empty()) {
         unescape_all(Self, tag.Children);
      }

      if ((tag.Flags & XTF::CDATA) != XTF::NIL) continue;
      for (auto &attrib : tag.Attribs) {
         if (attrib.Value.empty()) continue;
         xml_unescape(Self, attrib.Value);
      }
   }
};
