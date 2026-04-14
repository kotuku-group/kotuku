/*****************************************************************************
**
**  SRELL (std::regex-like library) version 4.080
**
**  Copyright (c) 2012-2025, Nozomu Katoo. All rights reserved.
**
**  Redistribution and use in source and binary forms, with or without
**  modification, are permitted provided that the following conditions are
**  met:
**
**  1. Redistributions of source code must retain the above copyright notice,
**     this list of conditions and the following disclaimer.
**
**  2. Redistributions in binary form must reproduce the above copyright
**     notice, this list of conditions and the following disclaimer in the
**     documentation and/or other materials provided with the distribution.
**
**  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS
**  IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
**  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
**  PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
**  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
**  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
**  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
**  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
**  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
**  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
**  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**
******************************************************************************
*/

#ifndef SRELL_REGEX_TEMPLATE_LIBRARY
#define SRELL_REGEX_TEMPLATE_LIBRARY

#define SRELL_NO_THROW 1

#include <stdexcept>
#include <climits>
#include <cwchar>
#include <limits>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>
#include <iterator>
#include <algorithm>
#include <type_traits>
#include <span>
#include <concepts>
#include <compare>

// C++20 baseline - no need for feature detection
#define SRELL_HAS_TYPE_TRAITS
#define SRELL_NOEXCEPT noexcept

#ifndef IS
#define IS ==
#endif

#if !defined(SRELL_NO_SIMD)
#if (defined(_M_X64) && !defined(_M_ARM64EC)) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
// Modern SIMD detection - prefer newer instruction sets
#if defined(__AVX512F__)
   #define SRELL_HAS_AVX512
   #define SRELL_HAS_AVX2
   #define SRELL_HAS_SSE42
#elif defined(__AVX2__)
   #define SRELL_HAS_AVX2
   #define SRELL_HAS_SSE42
#elif defined(__SSE4_2__)
   #define SRELL_HAS_SSE42
#elif defined(_MSC_VER) && (_MSC_VER >= 1500)
   #define SRELL_HAS_SSE42
#elif defined(__clang__) && defined(__clang_major__) && ((__clang_major__ >= 4) || ((__clang_major__ == 3) && defined(__clang_minor__) && (__clang_minor__ >= 8)))
   #define SRELL_HAS_SSE42
#elif defined(__GNUC__) && ((__GNUC__ >= 5) || ((__GNUC__ == 4) && defined(__GNUC_MINOR__) && (__GNUC_MINOR__ >= 9)))
   #define SRELL_HAS_SSE42
#endif   //  SIMD instruction sets
#endif   //  x86/x64.
#endif

#define SRELL_AT_SSE42
#define SRELL_AT_AVX2
#define SRELL_AT_AVX512

#if defined(SRELL_HAS_SSE42) or defined(SRELL_HAS_AVX2) or defined(SRELL_HAS_AVX512)
#if defined(_MSC_VER)
   #include <intrin.h>
#else
   #include <x86intrin.h>

   #if !defined(__SSE4_2__)
   #undef SRELL_AT_SSE42
   #define SRELL_AT_SSE42 __attribute__((target("sse4.2")))
   #endif

   #if !defined(__AVX2__)
   #undef SRELL_AT_AVX2
   #define SRELL_AT_AVX2 __attribute__((target("avx2")))
   #endif

   #if !defined(__AVX512F__)
   #undef SRELL_AT_AVX512
   #define SRELL_AT_AVX512 __attribute__((target("avx512f")))
   #endif
#endif
#endif

#if defined(_MSC_VER)
#define SRELL_NO_VCWARNING(n) \
   __pragma(warning(push)) \
   __pragma(warning(disable:n))
#define SRELL_NO_VCWARNING_END __pragma(warning(pop))
#else
#define SRELL_NO_VCWARNING(x)
#define SRELL_NO_VCWARNING_END
#endif

// SRELL_NOEXCEPT now defined above with C++20 baseline

namespace srell
{
//  C++20 Concepts for better template constraints
template<typename T>
concept character_type =
   std::same_as<T, char> or
   std::same_as<T, wchar_t> or
   std::same_as<T, char8_t> or
   std::same_as<T, char16_t> or
   std::same_as<T, char32_t>;

template<typename T>
concept bidirectional_iterator = std::bidirectional_iterator<T>;

template<typename T>
concept allocator_type = requires(T alloc) {
   typename T::value_type;
   { alloc.allocate(std::size_t{1}) } -> std::convertible_to<typename T::value_type*>;
   { alloc.deallocate(std::declval<typename T::value_type*>(), std::size_t{1}) } -> std::same_as<void>;
};

//  ["regex_constants.h" ...

   namespace regex_constants
   {
      enum syntax_option_type
      {
         icase = 1 << 1,
         nosubs = 1 << 2,
         optimize = 1 << 3,
         collate = 0,
         ECMAScript = 1 << 0,
         multiline = 1 << 4,
         basic = 0,
         extended = 0,
         awk = 0,
         grep = 0,
         egrep = 0,

         //  SRELL's extensions.
         sticky = 1 << 5,
         dotall = 1 << 6,  //  singleline.
         unicodesets = 1 << 7,
         vmode = unicodesets,
         quiet = 1 << 8,

         //  For internal use.
         back_ = 1 << 9,
         pflagsmask_ = (1 << 9) - 1
      };

      constexpr syntax_option_type operator&(const syntax_option_type left, const syntax_option_type right) noexcept
      {
         return syntax_option_type(int(left) & int(right));
      }
      constexpr syntax_option_type operator|(const syntax_option_type left, const syntax_option_type right) noexcept
      {
         return syntax_option_type(int(left) | int(right));
      }
      constexpr syntax_option_type operator^(const syntax_option_type left, const syntax_option_type right) noexcept
      {
         return syntax_option_type(int(left) ^ int(right));
      }
      constexpr syntax_option_type operator~(const syntax_option_type b) noexcept
      {
         return syntax_option_type(~int(b));
      }
      constexpr syntax_option_type &operator&=(syntax_option_type &left, const syntax_option_type right) noexcept
      {
         left = left & right;
         return left;
      }
      constexpr syntax_option_type &operator|=(syntax_option_type &left, const syntax_option_type right) noexcept
      {
         left = left | right;
         return left;
      }
      constexpr syntax_option_type &operator^=(syntax_option_type &left, const syntax_option_type right) noexcept
      {
         left = left ^ right;
         return left;
      }
   }

   namespace regex_constants
   {
      enum match_flag_type
      {
         match_default = 0,         // Default matching behavior with no special constraints applied.
         match_not_bol = 1 << 0,    // Treats the first character in the sequence as NOT being at the beginning of a line, preventing ^ from matching at that position.
         match_not_eol = 1 << 1,    // Treats the last character in the sequence as NOT being at the end of a line, preventing $ from matching at that position.
         match_not_bow = 1 << 2,    // Treats the first character in the sequence as NOT being at the beginning of a word, affecting \b word boundary matching.
         match_not_eow = 1 << 3,    // Treats the last character in the sequence as NOT being at the end of a word, affecting \b word boundary matching.
         match_any = 0,             // An alias for match_default with the same zero value.
         match_not_null = 1 << 4,   // Prevents the regex engine from matching zero-length (empty) sequences.
         match_continuous = 1 << 5, // Requires the match to start at the beginning of the sequence (anchored matching), used internally to implement regex_match.
         match_prev_avail = 1 << 6, // Indicates that a valid character exists before the first position in the sequence, enabling proper lookbehind and boundary assertions.

         format_default = 0,         // Default formatting behavior when replacing matched text.
         format_sed = 0,             // An alias for format_default, indicating sed-style formatting.
         format_no_copy = 1 << 7,    // In regex_replace, prevents copying non-matched portions of the input to the output.
         format_first_only = 1 << 8, // In regex_replace, replaces only the first match and leaves subsequent matches unchanged.

         //  For internal use.
         match_whole = 1 << 9       // Internal flag used to distinguish regex_match operations from regex_search operations.
      };

      constexpr match_flag_type operator&(const match_flag_type left, const match_flag_type right) noexcept
      {
         return match_flag_type(int(left) & int(right));
      }
      constexpr match_flag_type operator|(const match_flag_type left, const match_flag_type right) noexcept
      {
         return match_flag_type(int(left) | int(right));
      }
      constexpr match_flag_type operator^(const match_flag_type left, const match_flag_type right) noexcept
      {
         return match_flag_type(int(left) ^ int(right));
      }
      constexpr match_flag_type operator~(const match_flag_type b) noexcept
      {
         return match_flag_type(~int(b));
      }
      constexpr match_flag_type &operator&=(match_flag_type &left, const match_flag_type right) noexcept
      {
         left = left & right;
         return left;
      }
      constexpr match_flag_type &operator|=(match_flag_type &left, const match_flag_type right) noexcept
      {
         left = left | right;
         return left;
      }
      constexpr match_flag_type &operator^=(match_flag_type &left, const match_flag_type right) noexcept
      {
         left = left ^ right;
         return left;
      }
   }
   //  namespace regex_constants

   namespace regex_constants
   {
      using error_type = unsigned int;

      inline constexpr error_type error_collate{100};
      inline constexpr error_type error_ctype{101};
      inline constexpr error_type error_escape{102};
      inline constexpr error_type error_backref{103};
      inline constexpr error_type error_brack{104};
      inline constexpr error_type error_paren{105};
      inline constexpr error_type error_brace{106};
      inline constexpr error_type error_badbrace{107};
      inline constexpr error_type error_range{108};
      inline constexpr error_type error_space{109};
      inline constexpr error_type error_badrepeat{110};
      inline constexpr error_type error_complexity{111};
      inline constexpr error_type error_stack{112};

      //  SRELL's extensions.
      inline constexpr error_type error_utf8{113};
      //  The expression contained an invalid UTF-8 sequence.

      inline constexpr error_type error_property{114};
      //  The expression contained an invalid Unicode property name or value.

      inline constexpr error_type error_noescape{115};
      //  (Only in v-mode) ( ) [ ] { } / - \ | need to be escaped in a character class.

      inline constexpr error_type error_operator{116};
      //  (Only in v-mode) A character class contained a reserved double punctuation
      //  operator or different types of operators at the same level, such as [ab--cd].

      inline constexpr error_type error_complement{117};
      //  (Only in v-mode) \P or a negated character class contained a property of strings.

      inline constexpr error_type error_modifier{118};
      //  A specific flag modifier appeared more then once, or the un-bounded form
      //  ((?ism-ism)) appeared at a position other than the beginning of the expression.

      inline constexpr error_type error_first_{error_collate};
      inline constexpr error_type error_last_{error_modifier};

      inline constexpr error_type error_internal{999};
   }
   //  namespace regex_constants

   namespace re_detail
   {

#if defined(__cpp_unicode_characters)

      using ui_l32 = char32_t;   //  uint_least32.

#elif defined(UINT_MAX) && UINT_MAX >= 0xFFFFFFFF

      using ui_l32 = unsigned int;

#elif defined(ULONG_MAX) && ULONG_MAX >= 0xFFFFFFFF

      using ui_l32 = unsigned long;

#else
#error could not find a suitable type for 32-bit Unicode integer values.
#endif   //  defined(__cpp_unicode_characters)
   }  //  namespace re_detail

//  ... "rei_type.h"]
//  ["rei_constants.h" ...

   namespace re_detail
   {
      enum re_state_type
      {
         st_character,               //  0x00
         st_character_class,         //  0x01

         st_epsilon,                 //  0x02

         st_check_counter,           //  0x03
         st_increment_counter,       //  0x04
         st_decrement_counter,       //  0x05
         st_save_and_reset_counter,  //  0x06
         st_restore_counter,         //  0x07

         st_roundbracket_open,       //  0x08
         st_roundbracket_pop,        //  0x09
         st_roundbracket_close,      //  0x0a

         st_repeat_in_push,          //  0x0b
         st_repeat_in_pop,           //  0x0c
         st_check_0_width_repeat,    //  0x0d

         st_backreference,           //  0x0e

         st_lookaround_open,         //  0x0f

         st_lookaround_pop,          //  0x10

         st_bol,                     //  0x11
         st_eol,                     //  0x12
         st_boundary,                //  0x13

                        st_success,                 //  0x14

                        st_lookaround_close    = st_success,
                        st_zero_width_boundary = st_lookaround_open
                };
      //  re_state_type

      namespace constants
      {
         static const ui_l32 unicode_max_codepoint = 0x10ffff;
         static const ui_l32 invalid_u32value = ui_l32(-1);
         static const ui_l32 max_u32value = ui_l32(-2);
         static const ui_l32 ccstr_empty = ui_l32(-1);
         static const ui_l32 infinity = ui_l32(~0);
         static const ui_l32 errshift = 24;
      }
      //  constants

      namespace masks
      {
         static const ui_l32 asc_icase = 0x20;
         static const ui_l32 pos_cf = 0x200000; //  1 << 21.
         static const ui_l32 pos_char = 0x1fffff;
         static const ui_l32 fcc_simd = 0xffffff00;
         static const ui_l32 fcc_simd_num = 0xff;
         static const ui_l32 errmask = 0xff000000;
         static const ui_l32 somask = 0xffffff;
      }
      //  masks

      namespace sflags
      {
         static const ui_l32 is_not = 1;
         static const ui_l32 icase = 1;
         static const ui_l32 multiline = 1;
         static const ui_l32 backrefno_unresolved = 1 << 1;
         static const ui_l32 hooking = 1 << 2;
         static const ui_l32 hookedlast = 1 << 3;
         static const ui_l32 byn2 = 1 << 4;
         static const ui_l32 clrn2 = 1 << 5;
      }
      //  sflags

      namespace meta_char
      {
         static const ui_l32 mc_exclam = 0x21;  //  '!'
         static const ui_l32 mc_sharp  = 0x23;  //  '#'
         static const ui_l32 mc_dollar = 0x24;  //  '$'
         static const ui_l32 mc_rbraop = 0x28;  //  '('
         static const ui_l32 mc_rbracl = 0x29;  //  ')'
         static const ui_l32 mc_astrsk = 0x2a;  //  '*'
         static const ui_l32 mc_plus   = 0x2b;  //  '+'
         static const ui_l32 mc_comma  = 0x2c;  //  ','
         static const ui_l32 mc_minus  = 0x2d;  //  '-'
         static const ui_l32 mc_period = 0x2e;  //  '.'
         static const ui_l32 mc_colon  = 0x3a;  //  ':'
         static const ui_l32 mc_lt = 0x3c;      //  '<'
         static const ui_l32 mc_eq = 0x3d;      //  '='
         static const ui_l32 mc_gt = 0x3e;      //  '>'
         static const ui_l32 mc_query  = 0x3f;  //  '?'
         static const ui_l32 mc_sbraop = 0x5b;  //  '['
         static const ui_l32 mc_escape = 0x5c;  //  '\\'
         static const ui_l32 mc_sbracl = 0x5d;  //  ']'
         static const ui_l32 mc_caret  = 0x5e;  //  '^'
         static const ui_l32 mc_cbraop = 0x7b;  //  '{'
         static const ui_l32 mc_bar    = 0x7c;  //  '|'
         static const ui_l32 mc_cbracl = 0x7d;  //  '}'
      }
      //  meta_char

      namespace char_ctrl
      {
         static const ui_l32 cc_nul  = 0x00; //  '\0' //0x00:NUL
         static const ui_l32 cc_bs   = 0x08; //  '\b' //0x08:BS
         static const ui_l32 cc_htab = 0x09; //  '\t' //0x09:HT
         static const ui_l32 cc_nl   = 0x0a; //  '\n' //0x0a:LF
         static const ui_l32 cc_vtab = 0x0b; //  '\v' //0x0b:VT
         static const ui_l32 cc_ff   = 0x0c; //  '\f' //0x0c:FF
         static const ui_l32 cc_cr   = 0x0d; //  '\r' //0x0d:CR
      }
      //  char_ctrl

      namespace char_alnum
      {
         static const ui_l32 ch_0 = 0x30; //  '0'
         static const ui_l32 ch_1 = 0x31; //  '1'
         static const ui_l32 ch_7 = 0x37; //  '7'
         static const ui_l32 ch_8 = 0x38; //  '8'
         static const ui_l32 ch_9 = 0x39; //  '9'
         static const ui_l32 ch_A = 0x41; //  'A'
         static const ui_l32 ch_B = 0x42; //  'B'
         static const ui_l32 ch_D = 0x44; //  'D'
         static const ui_l32 ch_F = 0x46; //  'F'
         static const ui_l32 ch_P = 0x50; //  'P'
         static const ui_l32 ch_S = 0x53; //  'S'
         static const ui_l32 ch_W = 0x57; //  'W'
         static const ui_l32 ch_Z = 0x5a; //  'Z'
         static const ui_l32 ch_a = 0x61; //  'a'
         static const ui_l32 ch_b = 0x62; //  'b'
         static const ui_l32 ch_c = 0x63; //  'c'
         static const ui_l32 ch_d = 0x64; //  'd'
         static const ui_l32 ch_f = 0x66; //  'f'
         static const ui_l32 ch_i = 0x69; //  'i'
         static const ui_l32 ch_k = 0x6b; //  'k'
         static const ui_l32 ch_m = 0x6d; //  'm'
         static const ui_l32 ch_n = 0x6e; //  'n'
         static const ui_l32 ch_p = 0x70; //  'p'
         static const ui_l32 ch_q = 0x71; //  'q'
         static const ui_l32 ch_r = 0x72; //  'r'
         static const ui_l32 ch_s = 0x73; //  's'
         static const ui_l32 ch_t = 0x74; //  't'
         static const ui_l32 ch_u = 0x75; //  'u'
         static const ui_l32 ch_v = 0x76; //  'v'
         static const ui_l32 ch_w = 0x77; //  'w'
         static const ui_l32 ch_x = 0x78; //  'x'
         static const ui_l32 ch_y = 0x79; //  'y'
         static const ui_l32 ch_z = 0x7a; //  'z'
      }
      //  char_alnum

      namespace char_other
      {
         static const ui_l32 co_perc  = 0x25;   //  '%'
         static const ui_l32 co_amp   = 0x26;   //  '&'
         static const ui_l32 co_apos  = 0x27;   //  '\''
         static const ui_l32 co_slash = 0x2f;   //  '/'
         static const ui_l32 co_smcln = 0x3b;   //  ';'
         static const ui_l32 co_atmrk = 0x40;   //  '@'
         static const ui_l32 co_ll    = 0x5f;   //  '_'
         static const ui_l32 co_grav  = 0x60;   //  '`'
         static const ui_l32 co_tilde = 0x7e;   //  '~'
      }
      //  char_other

      namespace epsilon_type  //  Used only in the pattern compiler.
      {
         static const ui_l32 et_dfastrsk = 0x40;   //  '@'
         static const ui_l32 et_ccastrsk = 0x2a;   //  '*'
         static const ui_l32 et_alt      = 0x7c;   //  '|'
         static const ui_l32 et_ncgopen  = 0x3a;   //  ':'
         static const ui_l32 et_ncgclose = 0x3b;   //  ';'
         static const ui_l32 et_jmpinlp  = 0x2b;   //  '+'
         static const ui_l32 et_brnchend = 0x2f;   //  '/'
         static const ui_l32 et_fmrbckrf = 0x5c;   //  '\\'
         static const ui_l32 et_bo1fmrbr = 0x31;   //  '1'
         static const ui_l32 et_bo2fmrbr = 0x32;   //  '2'
         static const ui_l32 et_bo2skpd  = 0x21;   //  '!'
         static const ui_l32 et_rvfmrcg  = 0x28;   //  '('
         static const ui_l32 et_mfrfmrcg = 0x29;   //  ')'
         static const ui_l32 et_aofmrast = 0x78;   //  'x'
      }
      //  epsilon_type
   }
   //  namespace re_detail

//  ... "rei_constants.h"]
//  ["rei_utf_traits.hpp" ...

   namespace re_detail
   {

#if defined(_MSC_VER)
#define SRELL_FORCEINLINE __forceinline
#elif defined(__GNUC__)
#define SRELL_FORCEINLINE __attribute__((always_inline))
#else
#define SRELL_FORCEINLINE
#endif

template <typename charT>
struct utf_traits_core
{
public:

   typedef charT char_type;

   enum
   {
      maxseqlen = 1,
      cb_ = sizeof (charT) IS 1 ? CHAR_BIT : std::numeric_limits<charT>::digits,
      charbit = cb_ < 21 ? cb_ : 21
   };
   static const ui_l32 bitsetsize = ui_l32(1) << charbit;
   static const ui_l32 bitsetmask = bitsetsize - 1;
   static const ui_l32 maxcpvalue = charbit < 21 ? bitsetmask : 0x10ffff;

   //  *iter++
   template <typename ForwardIterator>
   static ui_l32 codepoint_inc(ForwardIterator &begin, const ForwardIterator /* end */)
   {
      return ui_l32(*begin++);
      //  Caller is responsible for begin != end.
   }

   //  *--iter
   template <typename BidirectionalIterator>
   static ui_l32 dec_codepoint(BidirectionalIterator &cur, const BidirectionalIterator /* begin */)
   {
      return ui_l32(*--cur);
      //  Caller is responsible for cur != begin.
   }

   template <typename I>   //  ui_l32 or char_type2.
   static bool is_mculeading(const I)
   {
      return false;
   }

   template <typename charT2>
   static bool is_trailing(const charT2 /* cu */)
   {
      return false;
   }

   static ui_l32 to_codeunits(charT out[maxseqlen], ui_l32 cp)
   {
      out[0] = charT(cp);
      return 1;
   }

   static ui_l32 seqlen(const ui_l32)
   {
      return 1;
   }

   static ui_l32 firstcodeunit(const ui_l32 cp)
   {
      return cp;
   }

   static ui_l32 nextlengthchange(const ui_l32)
   {
      return maxcpvalue + 1;
   }
};
template <typename charT> const ui_l32 utf_traits_core<charT>::bitsetsize;
template <typename charT> const ui_l32 utf_traits_core<charT>::bitsetmask;
template <typename charT> const ui_l32 utf_traits_core<charT>::maxcpvalue;
//  utf_traits_core

//  common and utf-32.
template <typename charT>
struct utf_traits : public utf_traits_core<charT>
{
};
//  utf_traits

//  utf-8 specific.
template <typename charT>
struct utf8_traits : public utf_traits_core<charT>
{
public:

   enum
   {
      maxseqlen = 4
   };
   static const ui_l32 bitsetsize = 0x100;
   static const ui_l32 bitsetmask = 0xff;
   static const ui_l32 maxcpvalue = 0x10ffff;

   template <typename ForwardIterator>
   static SRELL_FORCEINLINE ui_l32 codepoint_inc(ForwardIterator &begin, const ForwardIterator end)
   {
      ui_l32 codepoint = ui_l32(*begin++ & 0xff);

      if ((codepoint & 0x80) IS 0)
         return codepoint;

      if (begin != end)
      {
//       codepoint = ui_l32((codepoint << 6) | _pdep_u32(*begin, 0xc03f));
         codepoint = ui_l32((*begin & 0x3f) | ((*begin & 0xc0) << 8) | (codepoint << 6));
         ++begin;

         //  1011 0aaa aabb bbbb?
         if ((codepoint - 0xb080) < 0x780)
            return ui_l32(codepoint & 0x7ff);

         if (begin != end)
         {
            codepoint = ui_l32((*begin & 0x3f) | ((*begin & 0xc0) << 16) | (codepoint << 6));
            ++begin;

            //  1010 1110 aaaa bbbb bbcc cccc?
            if ((codepoint - 0xae0800) < 0xf800)
               return ui_l32(codepoint & 0xffff);

            if (begin != end)
            {
               codepoint = ui_l32((*begin & 0x3f) | ((*begin & 0xc0) << 24) | (codepoint << 6));
               ++begin;

               //  1010 1011 110a aabb bbbb cccc ccdd dddd?
               if ((codepoint - 0xabc10000) < 0x100000)
                  return ui_l32(codepoint & 0x1fffff);
            }
         }
      }
      return re_detail::constants::invalid_u32value;
   }

   template <typename BidirectionalIterator>
   static SRELL_FORCEINLINE ui_l32 dec_codepoint(BidirectionalIterator &cur, const BidirectionalIterator begin)
   {
      ui_l32 codepoint = ui_l32(*--cur);

      if ((codepoint & 0x80) IS 0)
         return ui_l32(codepoint & 0xff);

      if (cur != begin)
      {
         codepoint = ui_l32((codepoint & 0x3f) | ((codepoint & 0xc0) << 8) | ((*--cur & 0xff) << 6));

         //  1011 0bbb bbaa aaaa?
         if ((codepoint - 0xb080) < 0x780)
            return ui_l32(codepoint & 0x7ff);

         if (cur != begin)
         {
            codepoint = ui_l32((codepoint & 0xfff) | ((codepoint & 0xf000) << 8) | ((*--cur & 0xff) << 12));

            //  1010 1110 cccc bbbb bbaa aaaa?
            if ((codepoint - 0xae0800) < 0xf800)
               return ui_l32(codepoint & 0xffff);

            if (cur != begin)
            {
               codepoint = ui_l32((codepoint & 0x3ffff) | ((codepoint & 0xfc0000) << 8) | ((*--cur & 0xff) << 18));

               //  1010 1011 110d ddcc cccc bbbb bbaa aaaa?
               if ((codepoint - 0xabc10000) < 0x100000)
                  return ui_l32(codepoint & 0x1fffff);
            }
         }
      }
      return re_detail::constants::invalid_u32value;
   }

   template <typename I>
   static bool is_mculeading(const I c)
   {
      return (c & 0x80) ? true : false;
   }

   template <typename charT2>
   static bool is_trailing(const charT2 cu)
   {
      return (cu & 0xc0) IS 0x80;
   }

   static ui_l32 to_codeunits(charT out[maxseqlen], ui_l32 cp)
   {
      if (cp < 0x80)
      {
         out[0] = charT(cp);
         return 1;
      }
      else if (cp < 0x800)
      {
         out[0] = charT(((cp >> 6) & 0x1f) | 0xc0);
         out[1] = charT((cp & 0x3f) | 0x80);
         return 2;
      }
      else if (cp < 0x10000)
      {
         out[0] = charT(((cp >> 12) & 0x0f) | 0xe0);
         out[1] = charT(((cp >> 6) & 0x3f) | 0x80);
         out[2] = charT((cp & 0x3f) | 0x80);
         return 3;
      }

      out[0] = charT(((cp >> 18) & 0x07) | 0xf0);
      out[1] = charT(((cp >> 12) & 0x3f) | 0x80);
      out[2] = charT(((cp >> 6) & 0x3f) | 0x80);
      out[3] = charT((cp & 0x3f) | 0x80);
      return 4;
   }

   static ui_l32 seqlen(const ui_l32 cp)
   {
      return (cp < 0x80) ? 1 : ((cp < 0x800) ? 2 : ((cp < 0x10000) ? 3 : 4));
   }

   static ui_l32 firstcodeunit(const ui_l32 cp)
   {
      if (cp < 0x80)
         return cp;

      if (cp < 0x800)
         return ui_l32(((cp >> 6) & 0x1f) | 0xc0);

      if (cp < 0x10000)
         return ui_l32(((cp >> 12) & 0x0f) | 0xe0);

      return ui_l32(((cp >> 18) & 0x07) | 0xf0);
   }

   static ui_l32 nextlengthchange(const ui_l32 cp)
   {
      return (cp < 0x80) ? 0x80 : ((cp < 0x800) ? 0x800 : ((cp < 0x10000) ? 0x10000 : 0x110000));
   }
};
template <typename charT> const ui_l32 utf8_traits<charT>::bitsetsize;
template <typename charT> const ui_l32 utf8_traits<charT>::bitsetmask;
template <typename charT> const ui_l32 utf8_traits<charT>::maxcpvalue;
//  utf8_traits

//  utf-16 specific.
template <typename charT>
struct utf16_traits : public utf_traits_core<charT>
{
public:

   enum
   {
      maxseqlen = 2
   };
   static const ui_l32 bitsetsize = 0x10000;
   static const ui_l32 bitsetmask = 0xffff;
   static const ui_l32 maxcpvalue = 0x10ffff;

   template <typename ForwardIterator>
   static SRELL_FORCEINLINE ui_l32 codepoint_inc(ForwardIterator &begin, const ForwardIterator end)
   {
      const ui_l32 codeunit = *begin++;

      if ((codeunit & 0xfc00) != 0xd800)
         return ui_l32(codeunit & 0xffff);

      if (begin != end and (*begin & 0xfc00) IS 0xdc00)
         return ui_l32((((codeunit & 0x3ff) << 10) | (*begin++ & 0x3ff)) + 0x10000);

      return ui_l32(codeunit & 0xffff);
   }

   template <typename BidirectionalIterator>
   static SRELL_FORCEINLINE ui_l32 dec_codepoint(BidirectionalIterator &cur, const BidirectionalIterator begin)
   {
      const ui_l32 codeunit = *--cur;

      if ((codeunit & 0xfc00) != 0xdc00 or cur IS begin)
         return ui_l32(codeunit & 0xffff);

      if ((*--cur & 0xfc00) IS 0xd800)
         return ui_l32((((*cur & 0x3ff) << 10) | (codeunit & 0x3ff)) + 0x10000);

      ++cur;

      return ui_l32(codeunit & 0xffff);
   }

   template <typename I>
   static bool is_mculeading(const I c)
   {
      return (c & 0xfc00) IS 0xd800;
   }

   template <typename charT2>
   static bool is_trailing(const charT2 cu)
   {
      return (cu & 0xfc00) IS 0xdc00;
   }

   static ui_l32 to_codeunits(charT out[maxseqlen], ui_l32 cp)
   {
      if (cp < 0x10000)
      {
         out[0] = charT(cp);
         return 1;
      }

      cp -= 0x10000;
      out[0] = charT(((cp >> 10) & 0x3ff) | 0xd800);
      out[1] = charT((cp & 0x3ff) | 0xdc00);
      return 2;
   }

   static ui_l32 seqlen(const ui_l32 cp)
   {
      return (cp < 0x10000) ? 1 : 2;
   }

   static ui_l32 firstcodeunit(const ui_l32 cp)
   {
      if (cp < 0x10000)
         return cp;

      return ui_l32((cp >> 10) + 0xd7c0);
         //  aaaaa bbbbcccc ddddeeee -> AA AAbb bbcc/cc dddd eeee where AAAA = aaaaa - 1.
   }

   static ui_l32 nextlengthchange(const ui_l32 cp)
   {
      return (cp < 0x10000) ? 0x10000 : 0x110000;
   }
};
template <typename charT> const ui_l32 utf16_traits<charT>::bitsetsize;
template <typename charT> const ui_l32 utf16_traits<charT>::bitsetmask;
template <typename charT> const ui_l32 utf16_traits<charT>::maxcpvalue;
//  utf16_traits

//  specialisation for char.
template <>
struct utf_traits<char> : public utf_traits_core<char>
{
public:

   template <typename ForwardIterator>
   static ui_l32 codepoint_inc(ForwardIterator &begin, const ForwardIterator /* end */)
   {
      return ui_l32((unsigned char)(*begin++));
   }

   template <typename BidirectionalIterator>
   static ui_l32 dec_codepoint(BidirectionalIterator &cur, const BidirectionalIterator /* begin */)
   {
      return ui_l32((unsigned char)(*--cur));
   }
}; //  utf_traits<char>

//  specialisation for signed char.
template <>
struct utf_traits<signed char> : public utf_traits<char>
{
};

//  (signed) short, (signed) int, (signed) long, (signed) long long, ...

#if defined(__cpp_unicode_characters)
template <>
struct utf_traits<char16_t> : public utf16_traits<char16_t>
{
};
#endif

#if defined(__cpp_char8_t)
template <>
struct utf_traits<char8_t> : public utf8_traits<char8_t>
{
};
#endif

   }  //  re_detail

//  ... "rei_utf_traits.hpp"]
//  ["regex_traits.hpp" ...

template <class charT>
struct regex_traits
{
public:

   typedef charT char_type;
   typedef std::basic_string<char_type> string_type;
   typedef int locale_type;
   typedef int char_class_type;

   typedef re_detail::utf_traits<charT> utf_traits;
}; //  regex_traits

template <class charT>
struct u8regex_traits : public regex_traits<charT>
{
   typedef re_detail::utf8_traits<charT> utf_traits;
};

template <class charT>
struct u16regex_traits : public regex_traits<charT>
{
   typedef re_detail::utf16_traits<charT> utf_traits;
};

//  ... "regex_traits.hpp"]
//  ["rei_memory.hpp" ...

   namespace re_detail
   {

template <typename charT>
struct concon_view
{
   using size_type = std::size_t;

   const charT *data_;
   size_type size_;

   template <typename ContiguousContainer>
   constexpr concon_view(const ContiguousContainer &c) noexcept
      requires std::contiguous_iterator<typename ContiguousContainer::iterator>
      : data_(c.data()), size_(c.size()) {}

   constexpr concon_view() noexcept : data_(nullptr), size_(0) {}
   constexpr concon_view(const charT *const p, const size_type s) noexcept : data_(p), size_(s) {}
   constexpr concon_view(std::span<const charT> sp) noexcept : data_(sp.data()), size_(sp.size()) {}

   constexpr const charT *data() const noexcept
   {
      return data_;
   }
   constexpr size_type size() const noexcept
   {
      return size_;
   }
   constexpr std::span<const charT> as_span() const noexcept
   {
      return std::span<const charT>(data_, size_);
   }
};
// concon_view

template <typename ElemT, typename Alloc = std::allocator<ElemT> >
class simple_array
{
public:

   using value_type = ElemT;
   using size_type = std::size_t;
   using reference = ElemT &;
   using const_reference = const ElemT &;
   using pointer = ElemT *;
   using const_pointer = const ElemT *;
   using const_iterator = const_pointer;
   using sa_view = concon_view<ElemT>;

   inline static constexpr size_type npos = size_type(-1);

public:

   simple_array()
      : buffer_(nullptr), size_(0), capacity_p1_(1)
   {
   }

   simple_array(const size_type initsize)
      : buffer_(pointer(std::malloc(initsize * sizeof (ElemT)))), size_(initsize), capacity_p1_(initsize + 1)
   {
      if (buffer_ IS nullptr)
         abort();
   }

   simple_array(const simple_array &right)
      : buffer_(nullptr), size_(0), capacity_p1_(1)
   {
      operator=(right);
   }

   simple_array(const sa_view &v)
      : buffer_(nullptr), size_(0), capacity_p1_(1)
   {
      operator=(v);
   }

   simple_array(Alloc)
      : buffer_(nullptr), size_(0), capacity_p1_(1)
   {
   }

   simple_array &operator=(const simple_array &right)
   {
      if (this != &right)
      {
         resize(right.size_);
         std::memcpy((void *)(buffer_), right.buffer_, right.size_ * sizeof (ElemT));
      }
      return *this;
   }

   simple_array &operator=(const sa_view &v)
   {
      if (buffer_ != v.data_)
      {
         resize(v.size_);
         std::memcpy(buffer_, v.data_, v.size_ * sizeof (ElemT));
      }
      return *this;
   }

   simple_array(simple_array &&right) noexcept
      : buffer_(std::exchange(right.buffer_, nullptr))
      , size_(std::exchange(right.size_, 0))
      , capacity_p1_(std::exchange(right.capacity_p1_, 1))
   {
   }

   simple_array &operator=(simple_array &&right) noexcept
   {
      if (this != &right) [[likely]]
      {
         if (this->buffer_ != nullptr) [[likely]]
            std::free(this->buffer_);

         this->size_ = std::exchange(right.size_, 0);
         this->capacity_p1_ = std::exchange(right.capacity_p1_, 1);
         this->buffer_ = std::exchange(right.buffer_, nullptr);
      }
      return *this;
   }

   ~simple_array()
   {
      if (buffer_ != nullptr) [[likely]]
         std::free(buffer_);
   }

        [[nodiscard]] size_type size() const noexcept
   {
      return size_;
   }

   // Implicit conversion to sa_view/concon_view for compatibility
   operator sa_view() const noexcept
   {
      return sa_view(buffer_, size_);
   }

   bool operator==(const simple_array &right) const
   {
      if (this->size_ != right.size_) [[unlikely]]
         return false;

      for (size_type i = 0; i < size_; ++i)
         if (this->buffer_[i] != right[i]) [[unlikely]]
            return false;

      return true;
   }

   std::strong_ordering operator<=>(const simple_array &right) const
   {
      const auto size_cmp = this->size_ <=> right.size_;
      if (size_cmp != 0) [[likely]]
         return size_cmp;

      for (size_type i = 0; i < size_; ++i)
      {
         if (auto cmp = this->buffer_[i] <=> right[i]; cmp != 0) [[unlikely]]
            return cmp;
      }

      return std::strong_ordering::equal;
   }

   void clear()
   {
      size_ = 0;
   }

   void resize(const size_type newsize)
   {
      if (newsize >= capacity_p1_)
         reserve_<16>(newsize);

      size_ = newsize;
   }

   void resize(const size_type newsize, const ElemT &type)
   {
      size_type oldsize = size_;

      resize(newsize);
      for (; oldsize < size_; ++oldsize)
         buffer_[oldsize] = type;
   }

   void shrink(const size_type newsize)
   {
      size_ = newsize;
   }

   reference operator[](const size_type pos)
   {
      return buffer_[pos];
   }

   const_reference operator[](const size_type pos) const
   {
      return buffer_[pos];
   }

   void push_back(const_reference n)
   {
      const size_type oldsize = size_;

      if (++size_ >= capacity_p1_)
         reserve_<16>(size_);

      buffer_[oldsize] = n;
   }

   void push_back_c(const ElemT e)
   {
      push_back(e);
   }

   const_reference back() const
   {
      return buffer_[size_ - 1];
   }

   reference back()
   {
      return buffer_[size_ - 1];
   }

   void pop_back()
   {
      --size_;
   }

   void assign(const const_pointer p, const size_type len)
   {
      if (p != buffer_)
      {
         resize(len);
         std::memcpy(buffer_, p, len * sizeof (ElemT));
      }
   }

   simple_array &append(const size_type size, const ElemT &type)
   {
      resize(size_ + size, type);
      return *this;
   }

   simple_array &append(const const_pointer p, const size_type size)
   {
      resize(size_ + size);
      std::memcpy(buffer_ + size_ - size, p, size * sizeof (value_type));
      return *this;
   }

   simple_array &append(const simple_array &right)
   {
      const size_type oldsize = size_;
      const size_type rightsize = right.size_;

      resize(size_ + right.size_);
      std::memcpy(buffer_ + oldsize, right.buffer_, rightsize * sizeof (ElemT));
      return *this;
   }

   simple_array &append(const simple_array &right, const size_type pos, size_type len)
   {
      {
         const size_type len2 = right.size_ - pos;
         if (len > len2)
            len = len2;
      }

      const size_type oldsize = size_;

      resize(size_ + len);
      std::memcpy(buffer_ + oldsize, right.buffer_ + pos, len * sizeof (ElemT));
      return *this;
   }

   void erase(const size_type pos)
   {
      if (pos < size_)
      {
         std::memmove(buffer_ + pos, buffer_ + pos + 1, (size_ - pos - 1) * sizeof (ElemT));
         --size_;
      }
   }
   void erase(const size_type pos, const size_type len)
   {
      if (pos < size_)
      {
         size_type rmndr = size_ - pos;

         if (rmndr > len)
         {
            rmndr -= len;
            std::memmove(buffer_ + pos, buffer_ + pos + len, rmndr * sizeof (ElemT));
            size_ -= len;
         }
         else
            size_ = pos;
      }
   }

   //  For rei_compiler class.
   void insert(const size_type pos, const ElemT &type)
   {
      move_forwards_(pos, 1);
      buffer_[pos] = type;
   }

   void insert(const size_type pos, const simple_array &right)
   {
      move_forwards_(pos, right.size_);
      std::memcpy(buffer_ + pos, right.buffer_, right.size_ * sizeof (ElemT));
   }

   void insert(const size_type destpos, const simple_array &right, size_type srcpos, size_type srclen = npos)
   {
      {
         const size_type len2 = right.size_ - srcpos;
         if (srclen > len2)
            srclen = len2;
      }

      move_forwards_(destpos, srclen);
      std::memcpy(buffer_ + destpos, right.buffer_ + srcpos, srclen * sizeof (ElemT));
   }

   simple_array &replace(const size_type pos, size_type count, const simple_array &right)
   {
      if (count < right.size_)
         move_forwards_(pos + count, right.size_ - count);
      else if (count > right.size_)
      {
         const pointer base = buffer_ + pos;

         std::memmove(base + right.size_, base + count, (size_ - pos - count) * sizeof (ElemT));
         size_ -= count - right.size_;
      }

      std::memcpy(buffer_ + pos, right.buffer_, right.size_ * sizeof (ElemT));
      return *this;
   }

   size_type find(const value_type c, size_type pos = 0) const
   {
      for (; pos <= size_; ++pos)
         if (buffer_[pos] IS c)
            return pos;

      return npos;
   }

   const_pointer data() const
   {
      return buffer_;
   }

   const_iterator begin() const
   {
      return buffer_;
   }
   const_iterator end() const
   {
      return buffer_ + size_;
   }

        [[nodiscard]] size_type max_size() const noexcept
   {
      return maxsize_;
   }

        [[nodiscard]] bool no_alloc_failure() const noexcept
   {
      return capacity_p1_ > 0;
   }

   void swap(simple_array &right)
   {
      if (this != &right)
      {
         const pointer tmpbuffer = this->buffer_;
         const size_type tmpsize = this->size_;
         const size_type tmpcapacity = this->capacity_p1_;

         this->buffer_ = right.buffer_;
         this->size_ = right.size_;
         this->capacity_p1_ = right.capacity_p1_;

         right.buffer_ = tmpbuffer;
         right.size_ = tmpsize;
         right.capacity_p1_ = tmpcapacity;
      }
   }

protected:

   template <const size_type minsize>
   void reserve_(size_type newsize)
   {
      if (newsize <= maxsize_)
      {
         const pointer oldbuffer = buffer_;
         const size_type capa2 = newsize >= minsize ? capacity_p1_ << 1 : minsize;

         if (newsize < capa2)
         {
            newsize = capa2;
            if (newsize > maxsize_)
               newsize = maxsize_;
         }

         buffer_ = pointer(std::realloc((void *)(buffer_), newsize * sizeof (ElemT)));
         capacity_p1_ = newsize + 1;

         if (buffer_ != nullptr)
            return;

         std::free(oldbuffer);
//       buffer_ = nullptr;
         size_ = 0;
         capacity_p1_ = 1;
      }
      abort();
   }

   void move_forwards_(const size_type pos, const size_type count)
   {
      const size_type oldsize = size_;

      resize(size_ + count);

      if (pos < oldsize)
      {
         const pointer base = buffer_ + pos;

         std::memmove(base + count, base, (oldsize - pos) * sizeof (ElemT));
      }
   }

protected:

   pointer buffer_;
   size_type size_;
   size_type capacity_p1_;

   static const size_type maxsize_ = npos / sizeof (ElemT) / 2;
};
template <typename ElemT, typename Alloc>
const typename simple_array<ElemT, Alloc>::size_type simple_array<ElemT, Alloc>::npos;
//  simple_array

struct simple_stack : protected simple_array<char>
{
   using simple_array<char>::size_type;
   using simple_array<char>::clear;
   using simple_array<char>::size;
   using simple_array<char>::resize;

   template <typename T>
   void push_back_t_nc(const T &n)
   {
      std::memcpy(buffer_ + size_, &n, sizeof (T));
      size_ += sizeof (T);
   }

   template <typename T>
   void push_back_t(const T &n)
   {
      const size_type newsize = size_ + sizeof (T);

      if (newsize >= capacity_p1_)
         reserve_<256>(newsize);

      std::memcpy(buffer_ + size_, &n, sizeof (T));
      size_ = newsize;
   }

   template <typename T>
   void pop_back_t(T &t)
   {
      size_ -= sizeof (T);
      std::memcpy(&t, buffer_ + size_, sizeof (T));
   }

   void resize(const size_type newsize)
   {
      size_ = newsize;
   }

   void expand(const size_type add)
   {
      const size_type newsize = size_ + add;

      if (newsize >= capacity_p1_)
         reserve_<256>(newsize);
   }
};
//  simple_stack

   }  //  namespace re_detail

//  ... "rei_memory.hpp"]
//  ["rei_bitset.hpp" ...

   namespace re_detail
   {

template <const std::size_t Bits>
struct bitsetbase
{
   typedef std::size_t array_type;

#if defined(__cpp_constexpr)
   static constexpr std::size_t find_maxpow2(const array_type v, const std::size_t p2)
   {
      return v IS 0 ? (p2 >> 1) : find_maxpow2((v << (p2 - 1)) << 1, p2 << 1);
   }
   static const std::size_t bits_per_elem_ = find_maxpow2(32768, 16);
#else
   static const array_type maxval_ = array_type(-1);
   static const std::size_t bits_per_elem_ = (((((maxval_ >> 15) >> 15) >> 15) >> 15) >> 3) ? 64 : (maxval_ >= 0xFFFFFFFFul ? 32 : 16);
#endif
};

template <>
struct bitsetbase<256>
{
   typedef unsigned char array_type;

   static const std::size_t bits_per_elem_ = 1;
};

template <const std::size_t Bits>
class bitset : private bitsetbase<Bits>
{
   typedef bitsetbase<Bits> base_type;
   typedef typename base_type::array_type array_type;

public:

   bitset()
      : buffer_(nullptr)
   {
      ensure_alloc_();
      std::memset(buffer_, 0, size_in_byte_);
   }

   bitset(const bitset &right)
      : buffer_(nullptr)
   {
      ensure_alloc_();
      std::memcpy(buffer_, right.buffer_, size_in_byte_);
   }

#if defined(__cpp_rvalue_references)
   bitset(bitset &&right) SRELL_NOEXCEPT
      : buffer_(right.buffer_)
   {
      right.buffer_ = nullptr;
   }
#endif

   bitset &operator=(const bitset &right)
   {
      if (this != &right)
      {
         ensure_alloc_();
         std::memcpy(buffer_, right.buffer_, size_in_byte_);
      }
      return *this;
   }

#if defined(__cpp_rvalue_references)
   bitset &operator=(bitset &&right) SRELL_NOEXCEPT
   {
      if (this != &right)
      {
         if (this->buffer_ != nullptr)
            std::free(this->buffer_);

         this->buffer_ = right.buffer_;
         right.buffer_ = nullptr;
      }
      return *this;
   }
#endif

   ~bitset()
   {
      if (buffer_ != nullptr)
         std::free(buffer_);
   }

   void clear()
   {
      ensure_alloc_();
      std::memset(buffer_, 0, size_in_byte_);
   }

        [[nodiscard]] std::size_t size() const noexcept
        {
                return buffer_ != nullptr ? Bits : 0;
        }

   bitset &reset(const std::size_t bit)
   {
      buffer_[bit / base_type::bits_per_elem_] &= ~(array_type(1) << (bit & bitmask_));
      return *this;
   }

   bitset &set(const std::size_t bit)
   {
      buffer_[bit / base_type::bits_per_elem_] |= (array_type(1) << (bit & bitmask_));
      return *this;
   }

        [[nodiscard]] bool test(const std::size_t bit) const noexcept
        {
                return ((buffer_[bit / base_type::bits_per_elem_] >> (bit & bitmask_)) & 1) != 0;
        }

   void swap(bitset &right)
   {
      if (this != &right)
      {
         array_type *const tmpbuffer = this->buffer_;
         this->buffer_ = right.buffer_;
         right.buffer_ = tmpbuffer;
      }
   }

private:

   void ensure_alloc_()
   {
      if (buffer_ != nullptr)
         return;

      buffer_ = (array_type *)(std::malloc(size_in_byte_));
      if (buffer_ != nullptr)
         return;

      abort();
   }

   static const std::size_t bitmask_ = base_type::bits_per_elem_ - 1;
   static const std::size_t arraylength_ = (Bits + bitmask_) / base_type::bits_per_elem_;
   static const std::size_t size_in_byte_ = arraylength_ * sizeof (array_type);

   array_type *buffer_;
};

   }  //  namespace re_detail

//  ... "rei_bitset.hpp"]
//  ["rei_ucf.hpp" ...

   namespace re_detail
   {


      namespace ucf_constants
      {

#include "srell_ucfdata2.h"

      }  //  namespace ucf_constants

      namespace ucf_internal
      {

typedef ucf_constants::unicode_casefolding<ui_l32, ui_l32> ucfdata;

      }  //  namespace ucf_internal

      namespace ucf_constants
      {
         static const ui_l32 rev_maxset = ucf_internal::ucfdata::rev_maxset;
         static const ui_l32 rev_maxcp = ucf_internal::ucfdata::rev_maxcodepoint;
      }  //  namespace ucf_constants

class unicode_case_folding
{
public:

   static ui_l32 do_casefolding(const ui_l32 cp)
   {
      if (cp <= ucf_internal::ucfdata::ucf_maxcodepoint)
         return cp + ucf_internal::ucfdata::ucf_deltatable[ucf_internal::ucfdata::ucf_segmenttable[cp >> 8] + (cp & 0xff)];
      return cp;
   }

   static ui_l32 do_caseunfolding(ui_l32 out[ucf_constants::rev_maxset], const ui_l32 cp)
   {
      ui_l32 count = 0u;

      if (cp <= ucf_internal::ucfdata::rev_maxcodepoint)
      {
         const ui_l32 offset_of_charset = ucf_internal::ucfdata::rev_indextable[ucf_internal::ucfdata::rev_segmenttable[cp >> 8] + (cp & 0xff)];
         const ui_l32 *ptr = &ucf_internal::ucfdata::rev_charsettable[offset_of_charset];

         for (; *ptr != cfcharset_eos_ and count < ucf_constants::rev_maxset; ++ptr, ++count)
            out[count] = *ptr;
      }
      if (count IS 0u)
         out[count++] = cp;

      return count;
   }

   static ui_l32 try_casefolding(const ui_l32 cp)
   {
      if (cp <= ucf_internal::ucfdata::rev_maxcodepoint)
      {
         const ui_l32 offset_of_charset = ucf_internal::ucfdata::rev_indextable[ucf_internal::ucfdata::rev_segmenttable[cp >> 8] + (cp & 0xff)];
         const ui_l32 uf0 = ucf_internal::ucfdata::rev_charsettable[offset_of_charset];

         return uf0 != cfcharset_eos_ ? uf0 : constants::invalid_u32value;
      }
      return constants::invalid_u32value;
   }

private:

   static const ui_l32 cfcharset_eos_ = ucf_internal::ucfdata::eos;

public:  //  For debug.

   void print_tables() const;
};
//  unicode_case_folding

   }  //  namespace re_detail

//  ... "rei_ucf.hpp"]
//  ["rei_up.hpp" ...

   namespace re_detail
   {


      namespace up_constants
      {

#include "srell_updata3.h"

         static const ui_l32 error_property = ui_l32(-1);
      }  //  namespace up_constants

      namespace up_internal
      {
         typedef int up_type;
         typedef const char *pname_type;

         struct pnameno_map_type
         {
            pname_type name;
            up_type pno;
         };

         struct posinfo
         {
            ui_l32 offset;
            ui_l32 numofpairs;
         };

         typedef up_constants::unicode_property_data<
            pnameno_map_type,
            posinfo,
            ui_l32
            > updata;

      }  //  namespace up_internal

class unicode_property
{
public:

   typedef simple_array<char> pstring;

   static ui_l32 lookup_property(const pstring &name, const pstring &value)
   {
      up_type ptype = name.size() > 1 ? lookup_property_name(name) : up_constants::uptype_gc;
      const posinfo *pos = &updata::positiontable[ptype];
      ui_l32 pno = lookup_property_value(value, pos->offset, pos->numofpairs);

      if (pno IS upid_error and name.size() < 2)
      {
         ptype = up_constants::uptype_bp;
         pos = &updata::positiontable[ptype];
         pno = lookup_property_value(value, pos->offset, pos->numofpairs);
      }

      return pno != upid_error ? pno : up_constants::error_property;
   }

   static ui_l32 ranges_offset(const ui_l32 property_number)
   {
      return updata::positiontable[property_number].offset;
   }

   static ui_l32 number_of_ranges(const ui_l32 property_number)
   {
      return updata::positiontable[property_number].numofpairs;
   }

   static const ui_l32 *ranges_address(const ui_l32 pno)
   {
      return &updata::rangetable[ranges_offset(pno) << 1];
   }

   static bool is_valid_pno(const ui_l32 pno)
   {
      return pno != up_constants::error_property and pno <= max_property_number;
   }

   static bool is_pos(const ui_l32 pno)
   {
      return pno > max_property_number and pno <= max_pos_number;
   }

private:

   typedef up_internal::up_type up_type;
   typedef up_internal::pname_type pname_type;
   typedef up_internal::pnameno_map_type pnameno_map_type;
   typedef up_internal::posinfo posinfo;
   typedef up_internal::updata updata;

   static up_type lookup_property_name(const pstring &name)
   {
      return lookup_property_value(name, 1, updata::propertynumbertable[0].pno);
   }

   static ui_l32 lookup_property_value(const pstring &value, const ui_l32 offset, ui_l32 count)
   {
      const pnameno_map_type *base = &updata::propertynumbertable[offset];

      while (count)
      {
         ui_l32 mid = count >> 1;
         const pnameno_map_type &map = base[mid];
         const int cmp = compare(value, map.name);

         if (cmp < 0)
         {
            count = mid;
         }
         else if (cmp > 0)
         {
            ++mid;
            count -= mid;
            base += mid;
         }
         else  //if (cmp IS 0)
            return ui_l32(map.pno);
      }
      return upid_error;
   }

   static int compare(const pstring &value, pname_type pname)
   {
      for (pstring::size_type i = 0;; ++i, ++pname)
      {
         if (value[i] IS 0)
            return (*pname IS 0) ? 0 : (value[i] < *pname ? -1 : 1);

         if (value[i] != *pname)
            return value[i] < *pname ? -1 : 1;
      }
   }

private:

   static const ui_l32 max_property_number = ui_l32(up_constants::upid_max_property_number);
   static const ui_l32 max_pos_number = ui_l32(up_constants::upid_max_pos_number);
#if (SRELL_UPDATA_VERSION > 300)
   static const ui_l32 upid_error = ui_l32(up_constants::upid_error);
#else
   static const ui_l32 upid_error = ui_l32(up_constants::upid_unknown);
#endif
};
//  unicode_property

   }  //  namespace re_detail

//  ... "rei_up.hpp"]
//  ["rei_range_pair.hpp" ...

   namespace re_detail
   {

struct range_pair
{
   ui_l32 first;
   ui_l32 second;

   void set(const ui_l32 min, const ui_l32 max)
   {
      this->first = min;
      this->second = max;
   }

   void set(const ui_l32 minmax)
   {
      this->first = minmax;
      this->second = minmax;
   }

   bool is_range_valid() const
   {
      return first <= second;
   }

   bool operator==(const range_pair &right) const
   {
      return this->first IS right.first and this->second IS right.second;
   }

   bool operator<(const range_pair &right) const
   {
      return this->second < right.first;
   }

   void swap(range_pair &right)
   {
      const range_pair tmp = *this;
      *this = right;
      right = tmp;
   }
};
//  range_pair

struct range_pair_helper : public range_pair
{
   range_pair_helper(const ui_l32 min, const ui_l32 max)
   {
      this->first = min;
      this->second = max;
   }

   range_pair_helper(const ui_l32 minmax)
   {
      this->first = minmax;
      this->second = minmax;
   }
};
//  range_pair_helper

struct range_pairs : public simple_array<range_pair>
{
public:

   typedef simple_array<range_pair> array_type;
   typedef array_type::size_type size_type;
   typedef array_type::sa_view view_type;

   range_pairs()
   {
   }

   range_pairs(const range_pairs &rp) : array_type(rp)
   {
   }

   range_pairs(const view_type &v) : array_type(v)
   {
   }

   range_pairs &operator=(const range_pairs &rp)
   {
      array_type::operator=(rp);
      return *this;
   }

#if defined(__cpp_rvalue_references)
   range_pairs(range_pairs &&rp) SRELL_NOEXCEPT
      : array_type(std::move(rp))
   {
   }

   range_pairs &operator=(range_pairs &&rp) SRELL_NOEXCEPT
   {
      array_type::operator=(std::move(rp));
      return *this;
   }
#endif

   void set_solerange(const range_pair &right)
   {
      this->resize(1);
      (*this)[0] = right;
   }

   void append_newclass(const range_pairs &right)
   {
      this->append(right);
   }

   void append_newpair(const range_pair &right)
   {
      this->push_back(right);
   }

   void append_newpairs(const range_pair *const p, const ui_l32 n)
   {
      this->append(p, n);
   }

   void join(const range_pair &right)
   {
      range_pair *base = &(*this)[0];
      size_type count = this->size();

      while (count)
      {
         size_type mid = count / 2;
         range_pair *cp = &base[mid];

         if (cp->first and (right.second < cp->first - 1))
         {
            count = mid;
         }
         else if (right.first and (cp->second < right.first - 1))
         {
            ++mid;
            base += mid;
            count -= mid;
         }
         else
         {
            if (cp->first > right.first)
               cp->first = right.first;

            if (cp->second < right.second)
               cp->second = right.second;

            range_pair *lw = cp;

            if (cp->first > 0u)
            {
               for (--cp->first; lw != &(*this)[0];)
               {
                  if ((--lw)->second < cp->first)
                  {
                     ++lw;
                     break;
                  }
               }
               ++cp->first;
            }
            else
               lw = &(*this)[0];

            if (lw != cp)
            {
               if (cp->first > lw->first)
                  cp->first = lw->first;

               this->erase(lw - &(*this)[0], cp - lw);
               cp = lw;
            }

            range_pair *const rend = &(*this)[0] + this->size();
            range_pair *rw = cp;

            if (++cp->second > 0u)
            {
               for (; ++rw != rend;)
               {
                  if (cp->second < rw->first)
                     break;
               }
               --rw;
            }
            else
               rw = rend - 1;

            --cp->second;

            if (rw != cp)
            {
               if (rw->second < cp->second)
                  rw->second = cp->second;

               rw->first = cp->first;
               this->erase(cp - &(*this)[0], rw - cp);
            }
            return;
         }
      }
      this->insert(base - &(*this)[0], right);
   }

   void merge(const range_pairs &right)
   {
      for (size_type i = 0; i < right.size(); ++i)
         join(right[i]);
   }

   void merge(const view_type &v)
   {
      for (size_type i = 0; i < v.size_; ++i)
         join(v.data_[i]);
   }

   bool same(ui_l32 pos, const ui_l32 count, const range_pairs &right) const
   {
      if (count != right.size())
         return false;

      for (ui_l32 i = 0; i < count; ++i, ++pos)
         if (!((*this)[pos] IS right[i]))
            return false;

      return true;
   }

   int relationship(const range_pairs &right) const
   {
      if (this->size() IS right.size())
      {
         for (size_type i = 0; i < this->size(); ++i)
         {
            if (!((*this)[i] IS right[i]))
            {
               if (i IS 0)
                  goto check_overlap;

               return 1;   //  Overlapped.
            }
         }
         return 0;   //  Same.
      }
      check_overlap:
      return is_overlap(right) ? 1 : 2;   //  Overlapped or exclusive.
   }

   void negation()
   {
      ui_l32 begin = 0;
      size_type wpos = 0;

      for (size_type rpos = 0; rpos < this->size(); ++rpos)
      {
         const range_pair &rrange = (*this)[rpos];
         const ui_l32 nextbegin = rrange.second + 1;

         if (begin < rrange.first)
         {
            const ui_l32 prev2 = rrange.first - 1;
            range_pair &wrange = (*this)[wpos];

            wrange.second = prev2;
            wrange.first = begin;
            ++wpos;
         }
         begin = nextbegin;
      }

      if (begin <= constants::unicode_max_codepoint)
      {
         if (wpos >= this->size())
            this->resize(wpos + 1);

         (*this)[wpos].set(begin, constants::unicode_max_codepoint);
      }
      else
         this->shrink(wpos);
   }

   bool is_overlap(const range_pairs &right) const
   {
      for (size_type i = 0; i < this->size(); ++i)
      {
         const range_pair &leftrange = (*this)[i];

         for (size_type j = 0; j < right.size(); ++j)
         {
            const range_pair &rightrange = right[j];

            if (rightrange.first <= leftrange.second) //  Excludes l1 l2 < r1 r2.
               if (leftrange.first <= rightrange.second) //  Excludes r1 r2 < l1 l2.
                  return true;
         }
      }
      return false;
   }

   void load_from_memory(const ui_l32 *array, ui_l32 number_of_pairs)
   {
      for (; number_of_pairs; --number_of_pairs, array += 2)
         join(range_pair_helper(array[0], array[1]));
   }

   void make_caseunfoldedcharset()
   {
      ui_l32 table[ucf_constants::rev_maxset] = {};
      range_pairs newranges;

      for (size_type i = 0; i < this->size(); ++i)
      {
         const range_pair &range = (*this)[i];

         for (ui_l32 ucp = range.first; ucp <= range.second and ucp <= ucf_constants::rev_maxcp; ++ucp)
         {
            const ui_l32 setnum = unicode_case_folding::do_caseunfolding(table, ucp);

            for (ui_l32 j = 0; j < setnum; ++j)
            {
               if (table[j] != ucp)
                  newranges.join(range_pair_helper(table[j]));
            }
         }
      }
      merge(newranges);
   }

   //  For updataout.hpp.
   void remove_range(const range_pair &right)
   {
      for (size_type pos = 0; pos < this->size();)
      {
         range_pair &left = (*this)[pos];

         if (right.first <= left.first)   //  r1 <= l1
         {
            if (left.first <= right.second)  //  r1 <= l1 <= r2.
            {
               if (right.second < left.second)  //  r1 <= l1 <= r2 < l2.
               {
                  left.first = right.second + 1;
                  return;
               }
               else  //  r1 <= l1 <= l2 <= r2.
                  this->erase(pos);
            }
            else  //  r1 <= r2 < l1
               return;
         }
         //else   //  l1 < r1
         else if (right.first <= left.second)   //  l1 < r1 <= l2.
         {
            if (left.second <= right.second) //  l1 < r1 <= l2 <= r2.
            {
               left.second = right.first - 1;
               ++pos;
            }
            else  //  l1 < r1 <= r2 < l2
            {
               range_pair newrange(left);

               left.second = right.first - 1;
               newrange.first = right.second + 1;
               this->insert(++pos, newrange);
               return;
            }
         }
         else  //  l1 <= l2 < r1
            ++pos;
      }
   }

   ui_l32 consists_of_one_character(const bool icase) const
   {
      if (!icase)
      {
         if (this->size() IS 1 and (*this)[0].first IS (*this)[0].second)
            return (*this)[0].first;
      }
      else if (this->size())
      {
         const ui_l32 ucp1st = unicode_case_folding::do_casefolding((*this)[0].first);

         for (size_type i = 0; i < this->size(); ++i)
         {
            const range_pair &cr = (*this)[i];

            for (ui_l32 ucp = cr.first;; ++ucp)
            {
               if (ucp1st != unicode_case_folding::do_casefolding(ucp))
                  return constants::invalid_u32value;

               if (ucp IS cr.second)
                  break;
            }
         }
         return ucp1st;
      }
      return constants::invalid_u32value;
   }

   void split_ranges(range_pairs &removed, const range_pairs &rightranges)
   {
      range_pairs &kept = *this; //  Subtraction set.
      size_type prevolj = 0;
      range_pair newpair;

      removed.clear();  //  Intersection set.

      for (size_type i = 0;; ++i)
      {
         RETRY_SAMEINDEXNO:
         if (i >= kept.size())
            break;

         range_pair &left = kept[i];

         for (size_type j = prevolj; j < rightranges.size(); ++j)
         {
            const range_pair &right = rightranges[j];

            if (left.second < right.first)   //  Excludes l1 l2 < r1 r2.
               break;

            if (left.first <= right.second)  //  Excludes r1 r2 < l1 l2.
            {
               prevolj = j;

               if (left.first < right.first) //  l1 < r1 <= r2.
               {
                  if (right.second < left.second)  //  l1 < r1 <= r2 < l2.
                  {
                     removed.join(range_pair_helper(right.first, right.second));

                     newpair.set(right.second + 1, left.second);
                     left.second = right.first - 1;
                     kept.insert(i + 1, newpair);
                  }
                  else  //  l1 < r1 <= l2 <= r2.
                  {
                     removed.join(range_pair_helper(right.first, left.second));
                     left.second = right.first - 1;
                  }
               }
               //else   //  r1 <= l1.
               else if (right.second < left.second)   //  r1 <= l1 <= r2 < l2.
               {
                  removed.join(range_pair_helper(left.first, right.second));
                  left.first = right.second + 1;
               }
               else  //  r1 <= l1 <= l2 <= r2.
               {
                  removed.join(range_pair_helper(left.first, left.second));
                  kept.erase(i);
                  goto RETRY_SAMEINDEXNO;
               }
            }
         }
      }
   }


   //  For multiple_range_pairs functions.

   bool is_included(const ui_l32 pos, ui_l32 count, const ui_l32 c) const
   {
      const range_pair *base = &(*this)[pos];

      while (count)
      {
         ui_l32 mid = count >> 1;
         const range_pair &rp = base[mid];

         if (c <= rp.second)
         {
            if (c >= rp.first)
               return true;

            count = mid;
         }
         else
         {
            ++mid;
            count -= mid;
            base += mid;
         }
      }
      return false;
   }


   //  For Eytzinger layout functions.

   bool is_included_el(ui_l32 pos, const ui_l32 len, const ui_l32 c) const
   {
      const range_pair *const base = &(*this)[pos];

#if defined(__GNUC__)
      __builtin_prefetch(base);
#endif
      for (pos = 0; pos < len;)
      {
         const range_pair &rp = base[pos];

         if (c < rp.first)
            pos = (pos << 1) + 1;
         else if (c > rp.second)
            pos = (pos << 1) + 2;
         else
            return true;
      }
      return false;
   }

   ui_l32 create_el(const range_pair *srcbase, const ui_l32 srcsize)
   {
      const ui_l32 basepos = ui_l32(this->size());

      this->resize(basepos + srcsize);
      set_eytzinger_layout(0, srcbase, srcsize, &(*this)[basepos], 0);

      return srcsize;
   }


   template <typename utf_traits>
   ui_l32 num_codeunits() const
   {
      ui_l32 prev2 = constants::invalid_u32value;
      ui_l32 num = 0;

      for (size_type no = 0; no < this->size(); ++no)
      {
         const range_pair &cr = (*this)[no];

         for (ui_l32 first = cr.first; first <= utf_traits::maxcpvalue;)
         {
            const ui_l32 nlc = utf_traits::nextlengthchange(first);
            const ui_l32 second = cr.second < nlc ? cr.second : (nlc - 1);
            const ui_l32 cu1 = utf_traits::firstcodeunit(first);
            const ui_l32 cu2 = utf_traits::firstcodeunit(second);

            num += cu2 - cu1 + (prev2 IS cu1 ? 0 : 1);

            prev2 = cu2;
            if (second IS cr.second)
               break;

            first = second + 1;
         }
      }
      return num;
   }

private:

   using array_type::push_back;
   using array_type::append;


   ui_l32 set_eytzinger_layout(ui_l32 srcpos, const range_pair *const srcbase, const ui_l32 srclen,
      range_pair *const destbase, const ui_l32 destpos)
   {
      if (destpos < srclen)
      {
         const ui_l32 nextpos = (destpos << 1) + 1;

         srcpos = set_eytzinger_layout(srcpos, srcbase, srclen, destbase, nextpos);
         destbase[destpos] = srcbase[srcpos++];
         srcpos = set_eytzinger_layout(srcpos, srcbase, srclen, destbase, nextpos + 1);
      }
      return srcpos;
   }


public:  //  For debug.

   void print_pairs(const int, const char *const = nullptr, const char *const = nullptr) const;
};
//  range_pairs

   }  //  namespace re_detail

//  ... "rei_range_pair.hpp"]
//  ["rei_char_class.hpp" ...

   namespace re_detail
   {


//  For RegExpIdentifierStart and RegExpIdentifierPart
struct identifier_charclass
{
public:

   void clear()
   {
      char_class_.clear();
      char_class_pos_.clear();
   }

   void setup()
   {
      if (char_class_pos_.size() IS 0)
      {
         static const ui_l32 additions[] = {
            //  reg_exp_identifier_start, reg_exp_identifier_part.
            0x24, 0x24, 0x5f, 0x5f, 0x200c, 0x200d //  '$' '_' <ZWNJ>-<ZWJ>
         };
         range_pairs ranges;

         //  For reg_exp_identifier_start.
         {
            const ui_l32 *const IDs_address = unicode_property::ranges_address(upid_bp_ID_Start);
            const ui_l32 IDs_number = unicode_property::number_of_ranges(upid_bp_ID_Start);
            ranges.load_from_memory(IDs_address, IDs_number);
         }
         ranges.load_from_memory(&additions[0], 2);
         append_charclass(ranges);

         //  For reg_exp_identifier_part.
         ranges.clear();
         {
            const ui_l32 *const IDc_address = unicode_property::ranges_address(upid_bp_ID_Continue);
            const ui_l32 IDc_number = unicode_property::number_of_ranges(upid_bp_ID_Continue);
            ranges.load_from_memory(IDc_address, IDc_number);
         }
         ranges.load_from_memory(&additions[0], 3);
         append_charclass(ranges);
      }
   }

   bool is_identifier(const ui_l32 ch, const bool part) const
   {
      const range_pair &rp = char_class_pos_[part ? 1 : 0];

      return char_class_.is_included(rp.first, rp.second, ch);
   }

private:

   void append_charclass(const range_pairs &rps)
   {
      char_class_pos_.push_back(range_pair_helper(ui_l32(char_class_.size()), ui_l32(rps.size())));
      char_class_.append_newclass(rps);
   }

   range_pairs char_class_;
   range_pairs::array_type char_class_pos_;

//  UnicodeIDStart::
//    any Unicode code point with the Unicode property "ID_Start"
//  UnicodeIDContinue::
//    any Unicode code point with the Unicode property "ID_Continue"
   static const ui_l32 upid_bp_ID_Start = ui_l32(up_constants::bp_ID_Start);
   static const ui_l32 upid_bp_ID_Continue = ui_l32(up_constants::bp_ID_Continue);
};
//  identifier_charclass

class re_character_class
{
public:

   enum
   {  //    0       1      2      3     4           5
      newline, dotall, space, digit, word, icase_word,
      //                6
      number_of_predefcls
   };

   typedef unicode_property::pstring pstring;

   re_character_class()
   {
      setup_predefinedclass();
   }

   re_character_class &operator=(const re_character_class &that)
   {
      if (this != &that)
      {
         this->char_class_ = that.char_class_;
         this->char_class_pos_ = that.char_class_pos_;
         this->char_class_el_ = that.char_class_el_;
         this->char_class_pos_el_ = that.char_class_pos_el_;
      }
      return *this;
   }

#if defined(__cpp_rvalue_references)
   re_character_class &operator=(re_character_class &&that) SRELL_NOEXCEPT
   {
      if (this != &that)
      {
         this->char_class_ = std::move(that.char_class_);
         this->char_class_pos_ = std::move(that.char_class_pos_);
         this->char_class_el_ = std::move(that.char_class_el_);
         this->char_class_pos_el_ = std::move(that.char_class_pos_el_);
      }
      return *this;
   }
#endif

   bool no_alloc_failure() const
   {
      return char_class_.no_alloc_failure() and char_class_pos_.no_alloc_failure()
         and char_class_el_.no_alloc_failure() and char_class_pos_el_.no_alloc_failure()
      ;
   }

   bool is_included(const ui_l32 class_number, const ui_l32 c) const
   {
//    return char_class_.is_included(char_class_pos_[class_number], c);
      const range_pair &rp = char_class_pos_[class_number];

      return char_class_.is_included(rp.first, rp.second, c);
   }

   bool is_included(const ui_l32 pos, const ui_l32 len, const ui_l32 c) const
   {
      return char_class_el_.is_included_el(pos, len, c);
   }

   void reset()
   {
      setup_predefinedclass();

      char_class_el_.clear();
      char_class_pos_el_.clear();
   }

   ui_l32 register_newclass(const range_pairs &rps)
   {
      for (range_pairs::size_type no = 0; no < char_class_pos_.size(); ++no)
      {
         const range_pair &rp = char_class_pos_[no];

         if (char_class_.same(rp.first, rp.second, rps))
            return ui_l32(no);
      }

      append_charclass(rps);
      return ui_l32(char_class_pos_.size() - 1);
   }

   void copy_to(range_pairs &out, const ui_l32 no) const
   {
      const range_pair &ccpos = char_class_pos_[no];

      out.assign(&char_class_[ccpos.first], ccpos.second);
   }
   range_pairs::view_type view(const ui_l32 no) const
   {
      const range_pair &ccpos = char_class_pos_[no];

      return range_pairs::view_type(&char_class_[ccpos.first], ccpos.second);
   }


   const range_pair &charclasspos(const ui_l32 no) //  const
   {
      range_pair &elpos = char_class_pos_el_[no];

      if (elpos.second IS 0)
      {
         const range_pair &posinfo = char_class_pos_[no];

         if (posinfo.second > 0)
         {
            elpos.first = ui_l32(char_class_el_.size());
            elpos.second = char_class_el_.create_el(&char_class_[posinfo.first], posinfo.second);
         }
      }
      return elpos;
   }

   void finalise()
   {
      char_class_el_.clear();
      char_class_pos_el_.resize(char_class_pos_.size());
      std::memset(&char_class_pos_el_[0], 0, char_class_pos_el_.size() * sizeof (range_pairs::array_type::value_type));
   }


   void optimise()
   {
   }


   ui_l32 get_propertynumber(const pstring &pname, const pstring &pvalue) const
   {
      const ui_l32 pno = ui_l32(unicode_property::lookup_property(pname, pvalue));

      return (pno != up_constants::error_property) ? pno : up_constants::error_property;
   }

   bool load_upranges(range_pairs &newranges, const ui_l32 property_number) const
   {
      newranges.clear();

      if (unicode_property::is_valid_pno(property_number))
      {
         if (property_number IS upid_bp_Assigned)
         {
            load_updata(newranges, upid_gc_Cn);
            newranges.negation();
         }
         else
            load_updata(newranges, property_number);

         return true;
      }
      return false;
   }

   //  Properties of strings.
   bool is_pos(const ui_l32 pno) const
   {
      return unicode_property::is_pos(pno);
   }

   bool get_prawdata(simple_array<ui_l32> &seq, ui_l32 property_number)
   {
      if (property_number != up_constants::error_property)
      {
         if (property_number IS upid_bp_Assigned)
            property_number = upid_gc_Cn;

         const ui_l32 *const address = unicode_property::ranges_address(property_number);
//       const ui_l32 offset = unicode_property::ranges_offset(property_number);
         const ui_l32 number = unicode_property::number_of_ranges(property_number) * 2;

         seq.resize(number);
         for (ui_l32 i = 0; i < number; ++i)
            seq[i] = address[i];

         return true;
      }
      seq.clear();
      return false;
   }


   void swap(re_character_class &right)
   {
      if (this != &right)
      {
         this->char_class_.swap(right.char_class_);
         this->char_class_pos_.swap(right.char_class_pos_);
         this->char_class_el_.swap(right.char_class_el_);
         this->char_class_pos_el_.swap(right.char_class_pos_el_);
      }
   }

private:


   void load_updata(range_pairs &newranges, const ui_l32 property_number) const
   {
      const ui_l32 *const address = unicode_property::ranges_address(property_number);
//    const ui_l32 offset = unicode_property::ranges_offset(property_number);
      const ui_l32 number = unicode_property::number_of_ranges(property_number);

      newranges.load_from_memory(address, number);
   }


   void append_charclass(const range_pairs &rps)
   {
      char_class_pos_.push_back(range_pair_helper(ui_l32(char_class_.size()), ui_l32(rps.size())));
      char_class_.append_newclass(rps);
   }

//  The production CharacterClassEscape::s  evaluates as follows:
//    Return the set of characters containing the characters that are on the right-hand side of the WhiteSpace or LineTerminator productions.
//  WhiteSpace::<TAB> <VT> <FF> <SP> <NBSP> <ZWNBSP> <USP>
//               0009 000B 000C 0020   00A0     FEFF    Zs
//  LineTerminator::<LF> <CR> <LS> <PS>
//                  000A 000D 2028 2029
//
//  gc=Space_Separator:Zs
//  0x0020, 0x0020, 0x00A0, 0x00A0, 0x1680, 0x1680, 0x2000, 0x200A,
//  0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000,

   void setup_predefinedclass()
   {
      static const range_pair allranges[] = {
         //  newline.
         { 0x0a, 0x0a }, { 0x0d, 0x0d }, { 0x2028, 0x2029 },   //  \n \r
         //  dotall.
         { 0x0000, 0x10ffff },
         //  space.
         { 0x09, 0x0d },   //  \t \n \v \f \r
         { 0x20, 0x20 },   //  ' '
         { 0xa0, 0xa0 },   //  <NBSP>
         { 0x1680, 0x1680 }, { 0x2000, 0x200a }, { 0x2028, 0x2029 },
         { 0x202f, 0x202f }, { 0x205f, 0x205f }, { 0x3000, 0x3000 },
         { 0xfeff, 0xfeff },  //  <BOM>
         //  digit, word. word-icase.
         { 0x30, 0x39 },   //  '0'-'9'
         { 0x41, 0x5a }, { 0x5f, 0x5f }, { 0x61, 0x7a }, //  'A'-'Z' '_' 'a'-'z'
         { 0x017f, 0x017f }, { 0x212a, 0x212a }
      };
      static const range_pair offsets[] = {
         { 0, 3 },   //  newline.
         { 3, 1 },   //  dotall.
         { 4, 10 },  //  space.
         { 14, 1 },  //  digit.
         { 14, 4 },  //  word.
         { 14, 6 }   //  icase_word.
      };
      const std::size_t numofranges = sizeof allranges / sizeof (range_pair);

      if (char_class_.size() >= numofranges)
         char_class_.shrink(numofranges);
      else
      {
//       char_class_.clear();
         char_class_.append_newpairs(allranges, numofranges);
      }

      if (char_class_pos_.size() >= number_of_predefcls)
         char_class_pos_.shrink(number_of_predefcls);
      else
      {
//       char_class_pos_.clear();
         char_class_pos_.append(offsets, number_of_predefcls);
      }
   }

private:

   range_pairs char_class_;
   range_pairs::array_type char_class_pos_;

   range_pairs char_class_el_;
   range_pairs::array_type char_class_pos_el_;

   static const ui_l32 upid_gc_Zs = ui_l32(up_constants::gc_Space_Separator);
   static const ui_l32 upid_gc_Cn = ui_l32(up_constants::gc_Unassigned);
   static const ui_l32 upid_bp_Assigned = ui_l32(up_constants::bp_Assigned);

public:  //  For debug.

   void print_classes(const int) const;
};
//  re_character_class

   }  //  namespace re_detail

//  ... "rei_char_class.hpp"]
//  ["rei_groupname_mapper.hpp" ...

   namespace re_detail
   {


template <typename charT>
class groupname_mapper
{
public:

   typedef simple_array<charT> gname_string;
   typedef typename gname_string::sa_view view_type;
   using size_type = std::size_t;
   static const ui_l32 notfound = 0u;

   groupname_mapper()
   {
   }

   groupname_mapper(const groupname_mapper &right)
      : names_(right.names_), keysize_classno_(right.keysize_classno_)
   {
   }

#if defined(__cpp_rvalue_references)
   groupname_mapper(groupname_mapper &&right) SRELL_NOEXCEPT
      : names_(std::move(right.names_)), keysize_classno_(std::move(right.keysize_classno_))
   {
   }
#endif

   groupname_mapper &operator=(const groupname_mapper &right)
   {
      if (this != &right)
      {
         names_ = right.names_;
         keysize_classno_ = right.keysize_classno_;
      }
      return *this;
   }

#if defined(__cpp_rvalue_references)
   groupname_mapper &operator=(groupname_mapper &&right) SRELL_NOEXCEPT
   {
      if (this != &right)
      {
         names_ = std::move(right.names_);
         keysize_classno_ = std::move(right.keysize_classno_);
      }
      return *this;
   }
#endif

   void clear()
   {
      names_.clear();
      keysize_classno_.clear();
   }

   bool no_alloc_failure() const
   {
      return names_.no_alloc_failure() and keysize_classno_.no_alloc_failure();
   }

   const ui_l32 *operator[](const view_type &v) const
   {
      ui_l32 pos = 0;

      for (std::size_t i = 1; i < std::size_t(keysize_classno_.size());)
      {
         const ui_l32 keysize = keysize_classno_[i];
         const ui_l32 keynum = keysize_classno_[++i];

         if (keysize IS v.size_ and sameseq_(pos, v))
            return &keysize_classno_[i];

         pos += keysize;
         i += keynum + 1;
      }
      return nullptr;
   }

   view_type operator[](const ui_l32 indexno) const
   {
      ui_l32 pos = 0;

      for (std::size_t i = 1; i < std::size_t(keysize_classno_.size()); ++i)
      {
         const ui_l32 keysize = keysize_classno_[i];

         for (ui_l32 keynum = keysize_classno_[++i]; keynum; --keynum)
         {
            if (keysize_classno_[++i] IS indexno)
               return view_type(&names_[pos], keysize);
         }
         pos += keysize;
      }
      return view_type();
   }

   size_type size() const
   {
      return keysize_classno_.size() ? keysize_classno_[0] : 0;
   }

   int push_back(const gname_string &gname, const ui_l32 gno, const simple_array<ui_l32> &dupranges)
   {
      const ui_l32 *list = operator[](gname);

      if (list IS nullptr)
      {
         size_type curpos = keysize_classno_.size();

         names_.append(gname);
         keysize_classno_.resize(curpos ? (curpos + 3) : 4);
         if (curpos)
            ++keysize_classno_[0];
         else
            keysize_classno_[curpos++] = 1;
         keysize_classno_[curpos++] = ui_l32(gname.size());
         keysize_classno_[curpos++] = 1;
         keysize_classno_[curpos] = gno;
         return 1;
      }

      const size_type offset = list - keysize_classno_.data();
      const size_type keynum = list[0];

      for (size_type i = 1; i <= keynum; ++i)
      {
         const ui_l32 no = list[i];

         for (typename simple_array<ui_l32>::size_type j = 0;; ++j)
         {
            if (j >= dupranges.size())
               return 0;

            if (no < dupranges[j])
            {
               if (j & 1)
                  break;

               return 0;
            }
         }
      }

      const size_type newkeynum = ++keysize_classno_[offset];

      keysize_classno_.insert(offset + newkeynum, gno);

      return 1;
   }

   ui_l32 assign_number(const gname_string &gname, const ui_l32 gno)
   {
      const ui_l32 *list = operator[](gname);

      if (list IS nullptr)
      {
         size_type curpos = keysize_classno_.size();

         names_.append(gname);
         keysize_classno_.resize(curpos ? (curpos + 3) : 4);
         if (curpos)
            ++keysize_classno_[0];
         else
            keysize_classno_[curpos++] = 1;
         keysize_classno_[curpos++] = ui_l32(gname.size());
         keysize_classno_[curpos++] = 1;
         keysize_classno_[curpos] = gno;
         return gno;
      }
      return list[1];
   }

   void swap(groupname_mapper &right)
   {
      this->names_.swap(right.names_);
      keysize_classno_.swap(right.keysize_classno_);
   }

private:

   bool sameseq_(size_type pos, const view_type &v) const
   {
      for (size_type i = 0; i < v.size_; ++i, ++pos)
         if (pos >= names_.size() or names_[pos] != v.data_[i])
            return false;

      return true;
   }

   gname_string names_;
   simple_array<ui_l32> keysize_classno_;

public:  //  For debug.

   void print_mappings(const int) const;
};
template <typename charT>
const ui_l32 groupname_mapper<charT>::notfound;
//  groupname_mapper


   }  //  namespace re_detail

//  ... "rei_groupname_mapper.hpp"]
//  ["rei_state.hpp" ...

   namespace re_detail
   {

struct re_quantifier
{
   //  atleast and atmost: for check_counter and roundbracket_close.
   //  (Special case 1) in charcter_class, bol, eol, boundary, represents the offset and length
   //    of the range in the array of character classes.
   //  (Special case 1+) in NFA_states[0] holds a character class for one character lookahead.
   //  (Special case 2) in roundbracket_open and roundbracket_pop atleast and atmost represent
   //    the minimum and maximum bracket numbers respectively inside the brackets itself.
   //  (Special case 3) in repeat_in_push and repeat_in_pop atleast and atmost represent the
   //    minimum and maximum bracket numbers respectively inside the repetition.
   //  (Special case 4) in lookaround_open and lookaround_pop atleast and atmost represent the
   //    minimum and maximum bracket numbers respectively inside the lookaround.

   ui_l32 atleast;
   ui_l32 atmost;
   ui_l32 is_greedy;
      //  (Special case 1: v1) in lookaround_open represents the number of characters to be rewound.
      //  (Special case 2: v2) in lookaround_open represents: 0=lookaheads, 1=lookbehinds,
      //    2=matchpointrewinder, 3=rewinder+rerun.

   void reset(const ui_l32 len = 1)
   {
      atleast = atmost = len;
      is_greedy = 1;
   }

   void set(const ui_l32 min, const ui_l32 max)
   {
      atleast = min;
      atmost = max;
   }

   void set(const ui_l32 min, const ui_l32 max, const ui_l32 greedy)
   {
      atleast = min;
      atmost = max;
      is_greedy = greedy;
   }

   bool is_valid() const
   {
      return atleast <= atmost;
   }

   void set_infinity()
   {
      atmost = constants::infinity;
   }

   bool is_infinity() const
   {
      return atmost IS constants::infinity;
   }

   bool is_same() const
   {
      return atleast IS atmost;
   }

   bool is_default() const
   {
      return atleast IS 1 and atmost IS 1;
   }

   bool is_question() const
   {
      return atleast IS 0 and atmost IS 1;
   }
   bool is_asterisk() const
   {
      return atleast IS 0 and atmost IS constants::infinity;
   }
   bool is_plus() const
   {
      return atleast IS 1 and atmost IS constants::infinity;
   }
   bool is_asterisk_or_plus() const
   {
      return atleast <= 1 and atmost IS constants::infinity;
   }

   bool has_simple_equivalence() const
   {
      return (atleast <= 1 and atmost <= 3) or (atleast IS 2 and atmost <= 4) or (atleast IS atmost and atmost <= 6);
   }

   void multiply(const re_quantifier &q)
   {
      const ui_l32 newal = atleast * q.atleast;

      atleast = (newal IS 0 or (atleast != constants::infinity and q.atleast != constants::infinity and newal >= atleast)) ? newal : constants::infinity;

      const ui_l32 newam = atmost * q.atmost;

      atmost = (newam IS 0 or (atmost != constants::infinity and q.atmost != constants::infinity and newam >= atmost)) ? newam : constants::infinity;
   }

   void add(const re_quantifier &q)
   {
      if (atleast != constants::infinity)
      {
         if (q.atleast != constants::infinity and (atleast + q.atleast) >= atleast)
            atleast += q.atleast;
         else
            atleast = constants::infinity;
      }

      if (atmost != constants::infinity)
      {
         if (q.atmost != constants::infinity and (atmost + q.atmost) >= atmost)
            atmost += q.atmost;
         else
            atmost = constants::infinity;
      }
   }
};
//  re_quantifier

struct re_state
{
   re_state_type type;

   ui_l32 char_num;
      //  character: for character.
      //  number: for character_class, brackets, counter, repeat, backreference.
      //  (Special case) in [0] represents a code unit for finding an entry point if
      //    the firstchar class consists of a single code unit; otherwise invalid_u32value.

   re_quantifier quantifier;  //  For check_counter, roundbrackets, repeasts, (?<=...) and (?<!...),
      //  and character_class.

   ui_l32 flags;
      //  Bit
      //    0: is_not; for \B, (?!...) and (?<!...).
      //       icase; for [0], backreference.
      //       multiline; for bol, eol.
      //       (Only bit used across compiler and algorithm).
      //    1: backrefno_unresolved. Used only in compiler.
      //    2: hooking. Used only in compiler.
      //    3: hookedlast. Used only in compiler.
      //    4: byn2. Used only in compiler.

   union
   {
      std::ptrdiff_t next1;
      re_state *next_state1;
      //  (Special case 1) in lookaround_open points to the next of lookaround_close.
      //  (Special case 2) in lookaround_pop points to the content of brackets instead of lookaround_open.
   };
   union
   {
      std::ptrdiff_t next2;
      re_state *next_state2;
      //  character and character_class: points to another possibility, non-backtracking.
      //  epsilon: points to another possibility, backtracking.
      //  save_and_reset_counter, roundbracket_open, and repeat_in_push: points to a
      //    restore state, backtracking.
      //  check_counter: complementary to next1 based on quantifier.is_greedy.
      //  (Special case 1) roundbracket_close, check_0_width_repeat, and backreference:
      //    points to the next state as an exit after 0 width match.
      //  (Special case 2) in NFA_states[0] holds the entry point for match_continuous/regex_match.
      //  (Special case 3) in lookaround_open points to the contents of brackets.
   };

   void reset(const re_state_type t = st_character, const ui_l32 c = char_ctrl::cc_nul)
   {
      type = t;
      char_num = c;
      next1 = 1;
      next2 = 0;
      flags = 0u;
      quantifier.reset();
   }

   bool is_character_or_class() const
   {
      return type IS st_character or type IS st_character_class;
   }

   bool has_quantifier() const
   {
      //  1. character:  size == 1 and type == character,
      //  2. [...]:      size == 1 and type == character_class,
      //  3. (...):      size == ? and type == roundbracket_open,
      //  4. (?:...):    size == ? and type == epsilon and character == ':',
      //  5. backref:    size == ? and type == backreference,
      //  -- assertions boundary --
      //  6. lookaround: size == ? and type == lookaround_open,
      //  7. assertion:  size == 0 and type == one of assertions (^, $, \b and \B).
#if !defined(SRELL_ENABLE_GT)
      return type < st_zero_width_boundary;
#else
      //  5.5. independent: size == ? and type == lookaround and char_num == '>',
      return type < st_zero_width_boundary or (type IS st_lookaround_open and char_num IS meta_char::mc_gt);
#endif
   }

   bool is_ncgroup_open() const
   {
      return type IS st_epsilon and char_num IS epsilon_type::et_ncgopen;
   }

   bool is_ncgroup_open_or_close() const
   {
      return type IS st_epsilon and next2 IS 0 and (char_num IS epsilon_type::et_ncgopen or char_num IS epsilon_type::et_ncgclose);
   }

   bool is_alt() const
   {
      return type IS st_epsilon and next2 != 0 and char_num IS epsilon_type::et_alt;  //  '|'
   }

   bool is_question_or_asterisk_before_corcc() const
   {
      return type IS st_epsilon and char_num IS epsilon_type::et_ccastrsk;
   }

   bool is_asterisk_or_plus_for_onelen_atom() const
   {
      return type IS st_epsilon and ((next1 IS 1 and next2 IS 2) or (next1 IS 2 and next2 IS 1)) and quantifier.is_asterisk_or_plus();
   }

   bool is_same_character_or_charclass(const re_state &right) const
   {
      return type IS right.type and char_num IS right.char_num
         and (type != st_character or !((flags ^ right.flags) & regex_constants::icase));
   }

   std::ptrdiff_t nearnext() const
   {
      return quantifier.is_greedy ? next1 : next2;
   }

   std::ptrdiff_t farnext() const
   {
      return quantifier.is_greedy ? next2 : next1;
   }
};
//  re_state

template <typename charT>
struct re_compiler_state
{
   const ui_l32 *begin;
   ui_l32 soflags;
   ui_l32 depth;

   bool backref_used;

   groupname_mapper<charT> unresolved_gnames;
   simple_array<ui_l32> dupranges;

   identifier_charclass idchecker;

   void reset(const regex_constants::syntax_option_type f, const ui_l32 *const b)
   {
      begin = b;
      soflags = f;
      depth = 0;
      backref_used = false;

      unresolved_gnames.clear();
      dupranges.clear();

//    idchecker.clear();   //  Keeps data once created.
   }

   bool is_back() const
   {
      return (soflags & regex_constants::back_) ? true : false;
   }

   bool is_icase() const
   {
      return (soflags & regex_constants::icase) ? true : false;
   }

   bool is_multiline() const
   {
      return (soflags & regex_constants::multiline) ? true : false;
   }

   bool is_dotall() const
   {
      return (soflags & regex_constants::dotall) ? true : false;
   }

        bool is_vmode() const
        {
                return (soflags & regex_constants::unicodesets) ? true : false;
        }

   bool is_nosubs() const
   {
      return (soflags & regex_constants::nosubs) ? true : false;
   }
};
//  re_compiler_state

   }  //  namespace re_detail

//  ... "rei_state.hpp"]
//  ["rei_search_state.hpp" ...

   namespace re_detail
   {

template <typename BidirectionalIterator>
struct re_search_state_core
{
   const re_state *state;
   BidirectionalIterator iter;
};

template <typename BidirectionalIterator>
struct re_submatch_core
{
   BidirectionalIterator open_at;
   BidirectionalIterator close_at;
};

struct re_counter
{
   union
   {
      ui_l32 no;
      void *padding_;
   };
};

template <typename BidirectionalIterator>
struct re_submatch_type
{
   re_submatch_core<BidirectionalIterator> core;
   re_counter counter;

   void init(const BidirectionalIterator b)
   {
      core.open_at = core.close_at = b;
      counter.no = 0;
   }
};

#if defined(SRELL_HAS_TYPE_TRAITS)

template <typename T, typename Alloc, const bool>
struct container_type
{
   typedef std::vector<T, Alloc> type;
};
template <typename T, typename Alloc>
struct container_type<T, Alloc, true>
{
   typedef simple_array<T, Alloc> type;
};

template <typename BidirectionalIterator, const bool>
#else

template <typename Iter, typename T, typename Alloc>
struct container_type
{
   typedef std::vector<T, Alloc> type;
};
template <typename Iter, typename T, typename Alloc>
struct container_type<const Iter *, T, Alloc>
{
   typedef simple_array<T, Alloc> type;
};

template <typename BidirectionalIterator>
#endif   //   defined(SRELL_HAS_TYPE_TRAITS)
struct re_search_state_types
{
   typedef re_submatch_core<BidirectionalIterator> submatch_core;
   typedef re_submatch_type<BidirectionalIterator> submatch_type;
   typedef re_counter counter_type;
   typedef BidirectionalIterator position_type;

   typedef std::vector<submatch_type> submatch_array;

   typedef re_search_state_core<BidirectionalIterator> search_state_core;

   typedef std::vector<search_state_core> backtracking_array;
   typedef std::vector<submatch_core> capture_array;
   typedef std::vector<counter_type> counter_array;
   typedef std::vector<position_type> repeat_array;

   typedef typename backtracking_array::size_type btstack_size_type;

private:

   backtracking_array bt_stack;
   capture_array capture_stack;
   counter_array counter_stack;
   repeat_array repeat_stack;

public:

   void clear_stacks()
   {
      bt_stack.clear();
      capture_stack.clear();
      repeat_stack.clear();
      counter_stack.clear();
   }

   btstack_size_type bt_size() const
   {
      return bt_stack.size();
   }
   void bt_resize(const btstack_size_type s)
   {
      bt_stack.resize(s);
   }

   void expand(const btstack_size_type /* addlen */)
   {
   }
   void push_bt_wc(const search_state_core &ssc)
   {
      bt_stack.push_back(ssc);
   }

   void push_bt(const search_state_core &ssc)
   {
      bt_stack.push_back(ssc);
   }
   void push_sm(const submatch_core &smc)
   {
      capture_stack.push_back(smc);
   }
   void push_c(const counter_type c)
   {
      counter_stack.push_back(c);
   }
   void push_rp(const position_type p)
   {
      repeat_stack.push_back(p);
   }

   void pop_bt(search_state_core &ssc)
   {
      ssc = bt_stack.back();
      bt_stack.pop_back();
   }
   void pop_sm(submatch_core &smc)
   {
      smc = capture_stack.back();
      capture_stack.pop_back();
   }
   void pop_c(counter_type &c)
   {
      c = counter_stack.back();
      counter_stack.pop_back();
   }
   void pop_rp(position_type &p)
   {
      p = repeat_stack.back();
      repeat_stack.pop_back();
   }

public:

   struct bottom_state
   {
      btstack_size_type btstack_size;
      typename capture_array::size_type capturestack_size;
      typename counter_array::size_type counterstack_size;
      typename repeat_array::size_type repeatstack_size;

      bottom_state(const btstack_size_type bt, const re_search_state_types &ss)
         : btstack_size(bt)
         , capturestack_size(ss.capture_stack.size())
         , counterstack_size(ss.counter_stack.size())
         , repeatstack_size(ss.repeat_stack.size())
      {
      }
      void restore(btstack_size_type &bt, re_search_state_types &ss) const
      {
         bt = btstack_size;
         ss.capture_stack.resize(capturestack_size);
         ss.counter_stack.resize(counterstack_size);
         ss.repeat_stack.resize(repeatstack_size);
      }
   };
};

#if !defined(SRELL_NO_UNISTACK)
#if defined(SRELL_HAS_TYPE_TRAITS)
template <typename BidirectionalIterator>
struct re_search_state_types<BidirectionalIterator, true>
{
#else
template <typename charT>
struct re_search_state_types<const charT *>
{
   typedef const charT *BidirectionalIterator;
#endif
   typedef re_submatch_core<BidirectionalIterator> submatch_core;
   typedef re_submatch_type<BidirectionalIterator> submatch_type;
   typedef re_counter counter_type;
   typedef BidirectionalIterator position_type;

   typedef simple_array<submatch_type> submatch_array;

   typedef re_search_state_core<BidirectionalIterator> search_state_core;

   typedef simple_stack backtracking_array;
   typedef simple_array<counter_type> counter_array;
   typedef simple_array<position_type> repeat_array;

   typedef typename backtracking_array::size_type btstack_size_type;

private:

   backtracking_array bt_stack;

public:

   void clear_stacks()
   {
      bt_stack.clear();
   }

   btstack_size_type bt_size() const
   {
      return bt_stack.size();
   }
   void bt_resize(const btstack_size_type s)
   {
      bt_stack.resize(s);
   }

   void expand(const btstack_size_type addlen)
   {
      bt_stack.expand(addlen);
   }
   void push_bt_wc(const search_state_core &ssc)
   {
      bt_stack.push_back_t<search_state_core>(ssc);
   }

   void push_bt(const search_state_core &ssc)
   {
      bt_stack.push_back_t_nc<search_state_core>(ssc);
   }
   void push_sm(const submatch_core &smc)
   {
      bt_stack.push_back_t_nc<submatch_core>(smc);
   }
   void push_c(const counter_type c)
   {
      bt_stack.push_back_t_nc<counter_type>(c);
   }
   void push_rp(const position_type p)
   {
      bt_stack.push_back_t_nc<position_type>(p);
   }

   void pop_bt(search_state_core &ssc)
   {
      bt_stack.pop_back_t<search_state_core>(ssc);
   }
   void pop_sm(submatch_core &smc)
   {
      bt_stack.pop_back_t<submatch_core>(smc);
   }
   void pop_c(counter_type &c)
   {
      bt_stack.pop_back_t<counter_type>(c);
   }
   void pop_rp(position_type &p)
   {
      bt_stack.pop_back_t<position_type>(p);
   }

public:

   struct bottom_state
   {
      btstack_size_type btstack_size;

      bottom_state(const btstack_size_type bt, const re_search_state_types &)
         : btstack_size(bt)
      {
      }
      void restore(btstack_size_type &bt, re_search_state_types &) const
      {
         bt = btstack_size;
      }
   };
};
#endif   //  !defined(SRELL_NO_UNISTACK)
//  re_search_state_types

#if defined(SRELL_HAS_TYPE_TRAITS)

template <typename BidirectionalIterator>
class re_search_state : public re_search_state_types<BidirectionalIterator, std::is_trivially_copyable<BidirectionalIterator>::value>
{
private:
   typedef re_search_state_types<BidirectionalIterator, std::is_trivially_copyable<BidirectionalIterator>::value> base_type;

#else

template <typename BidirectionalIterator>
class re_search_state : public re_search_state_types<BidirectionalIterator>
{
private:
   typedef re_search_state_types<BidirectionalIterator> base_type;

#endif

public:

   typedef typename base_type::submatch_core submatchcore_type;
   typedef typename base_type::submatch_type submatch_type;
   typedef typename base_type::counter_type counter_type;
   typedef typename base_type::position_type position_type;

   typedef typename base_type::submatch_array submatch_array;

   typedef typename base_type::search_state_core search_state_core;

   typedef typename base_type::backtracking_array backtracking_array;
   typedef typename base_type::counter_array counter_array;
   typedef typename base_type::repeat_array repeat_array;

   typedef typename backtracking_array::size_type btstack_size_type;

   typedef typename base_type::bottom_state bottom_state;

public:

   search_state_core ssc;

   submatch_array bracket;
   counter_array counter;
   repeat_array repeat;

   btstack_size_type btstack_size;

   std::size_t failure_counter;

   BidirectionalIterator reallblim;
   BidirectionalIterator srchbegin;
   BidirectionalIterator lblim;
   BidirectionalIterator curbegin;
   BidirectionalIterator nextpos;
   BidirectionalIterator srchend;

   const re_state *entry_state;
   regex_constants::match_flag_type flags;

public:

   void init
   (
      const BidirectionalIterator begin,
      const BidirectionalIterator end,
      const BidirectionalIterator lookbehindlimit,
      const regex_constants::match_flag_type f
   )
   {
      reallblim = lblim = lookbehindlimit;
      nextpos = srchbegin = begin;
      srchend = end;
      flags = f;
   }

   void init_for_automaton
   (
      const ui_l32 num_of_brackets,
      const ui_l32 num_of_counters,
      const ui_l32 num_of_repeats
   )
   {
      counter.resize(num_of_counters);
      repeat.resize(num_of_repeats);

      if (num_of_brackets > 1)   //  [0] is no longer used.
      {
         bracket.resize(num_of_brackets);

         for (ui_l32 i = 1; i < num_of_brackets; ++i)
            bracket[i].init(this->srchend);
      }

      btstack_size = 0;
      base_type::clear_stacks();
   }

   void reset(const std::size_t limit)
   {
      ssc.state = this->entry_state;

      curbegin = ssc.iter;

      failure_counter = limit;
   }

   bool set_bracket0(const BidirectionalIterator begin, const BidirectionalIterator end)
   {
      ssc.iter = begin;
      nextpos = end;
      return true;
   }
};
//  re_search_state

   }  //  namespace re_detail

//  ... "rei_search_state.hpp"]
//  ["rei_bmh.hpp" ...

   namespace re_detail
   {


template <typename charT, typename utf_traits>
class re_bmh
{
public:

   re_bmh()
   {
   }

   re_bmh(const re_bmh &right)
   {
      operator=(right);
   }

#if defined(__cpp_rvalue_references)
   re_bmh(re_bmh &&right) SRELL_NOEXCEPT
   {
      operator=(std::move(right));
   }
#endif

   re_bmh &operator=(const re_bmh &that)
   {
      if (this != &that)
      {
         this->u32string_ = that.u32string_;

         this->bmtable_ = that.bmtable_;
         this->repseq_ = that.repseq_;
      }
      return *this;
   }

#if defined(__cpp_rvalue_references)
   re_bmh &operator=(re_bmh &&that) SRELL_NOEXCEPT
   {
      if (this != &that)
      {
         this->u32string_ = std::move(that.u32string_);

         this->bmtable_ = std::move(that.bmtable_);
         this->repseq_ = std::move(that.repseq_);
      }
      return *this;
   }
#endif

   void clear()
   {
      u32string_.clear();

      bmtable_.clear();
      repseq_.clear();
   }

   bool no_alloc_failure() const
   {
      return u32string_.no_alloc_failure()
         and bmtable_.no_alloc_failure()
         and repseq_.no_alloc_failure();
   }

   void setup(const simple_array<ui_l32> &u32s, const bool icase)
   {
      u32string_ = u32s;
      setup_();

      if (!icase)
         setup_for_casesensitive();
      else
         setup_for_icase();
   }

   template <typename RandomAccessIterator>
   bool do_casesensitivesearch(re_search_state<RandomAccessIterator> &sstate, const std::random_access_iterator_tag) const
   {
      RandomAccessIterator begin = sstate.srchbegin;
      const RandomAccessIterator end = sstate.srchend;
      std::size_t offset = std::size_t(repseq_.size() - 1);
      const charT *const relastchar = &repseq_[offset];

      for (; std::size_t(end - begin) > offset;)
      {
         begin += offset;

         if (*begin IS *relastchar)
         {
            const charT *re = relastchar;
            RandomAccessIterator tail = begin;

            for (; *--re IS *--tail;)
            {
               if (re IS repseq_.data())
                  return sstate.set_bracket0(tail, ++begin);
            }
         }
         offset = bmtable_[*begin & 0xff];
      }
      return false;
   }

   template <typename BidirectionalIterator>
   bool do_casesensitivesearch(re_search_state<BidirectionalIterator> &sstate, const std::bidirectional_iterator_tag) const
   {
      BidirectionalIterator begin = sstate.srchbegin;
      const BidirectionalIterator end = sstate.srchend;
      std::size_t offset = std::size_t(repseq_.size() - 1);
      const charT *const relastchar = &repseq_[offset];

      for (;;)
      {
         for (; offset; --offset, ++begin)
            if (begin IS end)
               return false;

         if (*begin IS *relastchar)
         {
            const charT *re = relastchar;
            BidirectionalIterator tail = begin;

            for (; *--re IS *--tail;)
            {
               if (re IS repseq_.data())
                  return sstate.set_bracket0(tail, ++begin);
            }
         }
         offset = bmtable_[*begin & 0xff];
      }
   }

   template <typename RandomAccessIterator>
   bool do_icasesearch(re_search_state<RandomAccessIterator> &sstate, const std::random_access_iterator_tag) const
   {
      const RandomAccessIterator begin = sstate.srchbegin;
      const RandomAccessIterator end = sstate.srchend;
      std::size_t offset = bmtable_[256];
      const ui_l32 entrychar = u32string_[u32string_.size() - 1];
      const ui_l32 *const re2ndlastchar = &u32string_[u32string_.size() - 2];
      RandomAccessIterator curpos = begin;

      for (; std::size_t(end - curpos) > offset;)
      {
         curpos += offset;

         for (; utf_traits::is_trailing(*curpos);)
            if (++curpos IS end)
               return false;

         RandomAccessIterator la(curpos);
         const ui_l32 txtlastchar = utf_traits::codepoint_inc(la, end);

         if (txtlastchar IS entrychar or unicode_case_folding::do_casefolding(txtlastchar) IS entrychar)
         {
            const ui_l32 *re = re2ndlastchar;
            RandomAccessIterator tail = curpos;

            for (; *re IS unicode_case_folding::do_casefolding(utf_traits::dec_codepoint(tail, begin)); --re)
            {
               if (re IS u32string_.data())
                  return sstate.set_bracket0(tail, la);

               if (tail IS begin)
                  break;
            }
         }
         offset = bmtable_[txtlastchar & 0xff];
      }
      return false;
   }

   template <typename BidirectionalIterator>
   bool do_icasesearch(re_search_state<BidirectionalIterator> &sstate, const std::bidirectional_iterator_tag) const
   {
      const BidirectionalIterator begin = sstate.srchbegin;
      const BidirectionalIterator end = sstate.srchend;

      if (begin != end)
      {
         std::size_t offset = bmtable_[256];
         const ui_l32 entrychar = u32string_[offset];
         const ui_l32 *const re2ndlastchar = &u32string_[offset - 1];
         BidirectionalIterator curpos = begin;

         for (;;)
         {
            for (;;)
            {
               if (++curpos IS end)
                  return false;
               if (!utf_traits::is_trailing(*curpos))
                  if (--offset IS 0)
                     break;
            }
            BidirectionalIterator la(curpos);
            const ui_l32 txtlastchar = utf_traits::codepoint_inc(la, end);

            if (txtlastchar IS entrychar or unicode_case_folding::do_casefolding(txtlastchar) IS entrychar)
            {
               const ui_l32 *re = re2ndlastchar;
               BidirectionalIterator tail = curpos;

               for (; *re IS unicode_case_folding::do_casefolding(utf_traits::dec_codepoint(tail, begin)); --re)
               {
                  if (re IS u32string_.data())
                     return sstate.set_bracket0(tail, la);

                  if (tail IS begin)
                     break;
               }
            }
            offset = bmtable_[txtlastchar & 0xff];
         }
      }
      return false;
   }

private:

   void setup_()
   {
      bmtable_.resize(257);
   }

   void setup_for_casesensitive()
   {
      charT mbstr[utf_traits::maxseqlen];
      const std::size_t u32str_lastcharpos_ = std::size_t(u32string_.size() - 1);

      repseq_.clear();

      for (std::size_t i = 0; i <= u32str_lastcharpos_; ++i)
      {
         const ui_l32 seqlen = utf_traits::to_codeunits(mbstr, u32string_[i]);

         repseq_.append(mbstr, seqlen);
      }

      for (ui_l32 i = 0; i < 256; ++i)
         bmtable_[i] = std::size_t(repseq_.size());

      const std::size_t repseq_lastcharpos_ = std::size_t(repseq_.size() - 1);

      for (std::size_t i = 0; i < repseq_lastcharpos_; ++i)
         bmtable_[repseq_[i] & 0xff] = repseq_lastcharpos_ - i;
   }

   void setup_for_icase()
   {
      ui_l32 u32table[ucf_constants::rev_maxset];
      const std::size_t u32str_lastcharpos = std::size_t(u32string_.size() - 1);
      simple_array<std::size_t> minlen(u32string_.size());
      std::size_t cu_repseq_lastcharpos = 0;

      for (std::size_t i = 0; i <= u32str_lastcharpos; ++i)
      {
         const ui_l32 setnum = unicode_case_folding::do_caseunfolding(u32table, u32string_[i]);
         ui_l32 u32c = u32table[0];

         for (ui_l32 j = 1; j < setnum; ++j)
            if (u32c > u32table[j])
               u32c = u32table[j];

         if (i < u32str_lastcharpos)
            cu_repseq_lastcharpos += minlen[i] = utf_traits::seqlen(u32c);
      }

      ++cu_repseq_lastcharpos;

      for (std::size_t i = 0; i < 256; ++i)
         bmtable_[i] = cu_repseq_lastcharpos;

      bmtable_[256] = --cu_repseq_lastcharpos;

      for (std::size_t i = 0; i < u32str_lastcharpos; ++i)
      {
         const ui_l32 setnum = unicode_case_folding::do_caseunfolding(u32table, u32string_[i]);

         for (ui_l32 j = 0; j < setnum; ++j)
            bmtable_[u32table[j] & 0xff] = cu_repseq_lastcharpos;

         cu_repseq_lastcharpos -= minlen[i];
      }
   }

public:  //  For debug.

   void print_table() const;
   void print_seq() const;

private:

   simple_array<ui_l32> u32string_;
   simple_array<std::size_t> bmtable_;
   simple_array<charT> repseq_;
};
//  re_bmh

   }  //  namespace re_detail

//  ... "rei_bmh.hpp"]
//  ["rei_upos.hpp" ...

   namespace re_detail
   {

struct posdata_holder
{
   simple_array<ui_l32> indices;
   simple_array<ui_l32> seqs;
   range_pairs ranges;
   range_pair length;

   void clear()
   {
      indices.clear();
      seqs.clear();
      ranges.clear();
      length.set(1);
   }

   bool has_empty() const
   {
      return (indices.size() >= 2 and indices[0] != indices[1]) ? true : false;
   }

   bool has_data() const
   {
      return ranges.size() > 0 or indices.size() > 0;
   }

   bool may_contain_strings() const
   {
      return indices.size() > 0; //  >= 2;
   }

   void swap(posdata_holder &right)
   {
      indices.swap(right.indices);
      seqs.swap(right.seqs);
      ranges.swap(right.ranges);
      length.swap(right.length);
   }

   void do_union(const posdata_holder &right)
   {
      simple_array<ui_l32> curseq;

      ranges.merge(right.ranges);

      if (right.has_empty() and !has_empty())
         register_emptystring();

      for (ui_l32 seqlen = 2; seqlen < ui_l32(right.indices.size()); ++seqlen)
      {
         const ui_l32 end = right.indices[seqlen - 1];
         ui_l32 begin = right.indices[seqlen];

         if (begin != end)
         {
            ensure_length(seqlen);
            curseq.resize(seqlen);

            for (; begin < end;)
            {
               const ui_l32 inspos = find_seq(&right.seqs[begin], seqlen);

               if (inspos IS indices[seqlen - 1])
               {
                  for (ui_l32 i = 0; i < seqlen; ++i, ++begin)
                     curseq[i] = right.seqs[begin];

                  seqs.insert(inspos, curseq);
                  for (ui_l32 i = 0; i < seqlen; ++i)
                     indices[i] += seqlen;
               }
               else
                  begin += seqlen;
            }
         }
      }
      check_lengths();
   }

   void do_subtract(const posdata_holder &right)
   {
      const ui_l32 maxlen = ui_l32(indices.size() <= right.indices.size() ? indices.size() : right.indices.size());

      {
         range_pairs removed;

         ranges.split_ranges(removed, right.ranges);
      }

      if (right.has_empty() and has_empty())
         unregister_emptystring();

      for (ui_l32 seqlen = 2; seqlen < maxlen; ++seqlen)
      {
         const ui_l32 end = right.indices[seqlen - 1];
         ui_l32 begin = right.indices[seqlen];

         if (begin != end)
         {
            for (; begin < end;)
            {
               const ui_l32 delpos = find_seq(&right.seqs[begin], seqlen);

               if (delpos < indices[seqlen - 1])
               {
                  seqs.erase(delpos, seqlen);

                  for (ui_l32 i = 0; i < seqlen; ++i)
                     indices[i] -= seqlen;
               }
               else
                  begin += seqlen;
            }
         }
      }
      check_lengths();
   }

   void do_and(const posdata_holder &right)
   {
      const ui_l32 maxlen = ui_l32(indices.size() <= right.indices.size() ? indices.size() : right.indices.size());
      posdata_holder newpos;
      simple_array<ui_l32> curseq;

      ranges.split_ranges(newpos.ranges, right.ranges);
      ranges.swap(newpos.ranges);

      if (has_empty() and right.has_empty())
         newpos.register_emptystring();
      else if (may_contain_strings() or right.may_contain_strings())
         ensure_length(1);

      for (ui_l32 seqlen = 2; seqlen < maxlen; ++seqlen)
      {
         const ui_l32 end = right.indices[seqlen - 1];
         ui_l32 begin = right.indices[seqlen];

         if (begin != end)
         {
            const ui_l32 myend = indices[seqlen - 1];

            curseq.resize(seqlen);

            for (; begin < end; begin += seqlen)
            {
               const ui_l32 srcpos = find_seq(&right.seqs[begin], seqlen);

               if (srcpos < myend)
               {
                  newpos.ensure_length(seqlen);

                  const ui_l32 inspos = newpos.find_seq(&right.seqs[begin], seqlen);

                  if (inspos IS newpos.indices[seqlen - 1])
                  {
                     for (ui_l32 i = 0; i < seqlen; ++i)
                        curseq[i] = right.seqs[begin + i];

                     newpos.seqs.insert(inspos, curseq);
                     for (ui_l32 i = 0; i < seqlen; ++i)
                        newpos.indices[i] += seqlen;
                  }
               }
            }
         }
      }
      this->indices.swap(newpos.indices);
      this->seqs.swap(newpos.seqs);
      check_lengths();
   }

   void split_seqs_and_ranges(const simple_array<ui_l32> &inseqs, const bool icase, const bool back)
   {
      const ui_l32 max = ui_l32(inseqs.size());
      simple_array<ui_l32> curseq;

      clear();

      for (ui_l32 indx = 0; indx < max;)
      {
         const ui_l32 elen = inseqs[indx++];

         if (elen IS 1) //  Range.
         {
            ranges.join(range_pair_helper(inseqs[indx], inseqs[indx + 1]));
            indx += 2;
         }
         else if (elen IS 2)
         {
            const ui_l32 ucpval = inseqs[indx++];

            if (ucpval != constants::ccstr_empty)
               ranges.join(range_pair_helper(ucpval));
            else
               register_emptystring();
         }
         else if (elen >= 3)
         {
            const ui_l32 seqlen = elen - 1;

            ensure_length(seqlen);

            const ui_l32 inspos = indices[seqlen - 1];

            curseq.resize(seqlen);
            if (!back)
            {
               for (ui_l32 j = 0; j < seqlen; ++j, ++indx)
                  curseq[j] = inseqs[indx];
            }
            else
            {
               for (ui_l32 j = seqlen; j; ++indx)
                  curseq[--j] = inseqs[indx];
            }

            if (icase)
            {
               for (simple_array<ui_l32>::size_type i = 0; i < curseq.size(); ++i)
               {
                  const ui_l32 cf = unicode_case_folding::try_casefolding(curseq[i]);

                  if (cf != constants::invalid_u32value)
                     curseq[i] = cf | masks::pos_cf;
               }
            }

            for (ui_l32 i = indices[seqlen];; i += seqlen)
            {
               if (i IS inspos)
               {
                  seqs.insert(inspos, curseq);
                  for (ui_l32 j = 0; j < seqlen; ++j)
                     indices[j] += seqlen;
                  break;
               }

               if (is_sameseq(&seqs[i], curseq.data(), seqlen))
                  break;
            }

         }
         //elen == 0: Padding.
      }

      if (icase)
         ranges.make_caseunfoldedcharset();

      check_lengths();
   }

private:

   void register_emptystring()
   {
      if (indices.size() < 2)
      {
         indices.resize(2);
         indices[1] = 0;
         indices[0] = 1;
      }
      else if (indices[0] IS indices[1])
      {
         ++indices[0];
      }
      length.first = 0;
   }

   void unregister_emptystring()
   {
      if (indices.size() >= 2 and indices[0] != indices[1])
         indices[0] = indices[1];
   }

   void ensure_length(const ui_l32 seqlen)
   {
      ui_l32 curlen = ui_l32(indices.size());

      if (seqlen >= curlen)
      {
         indices.resize(seqlen + 1);
         for (; curlen <= seqlen; ++curlen)
            indices[curlen] = 0;
      }
   }

   ui_l32 find_seq(const ui_l32 *const seqbegin, const ui_l32 seqlen) const
   {
      const ui_l32 end = indices[seqlen - 1];

      for (ui_l32 begin = indices[seqlen]; begin < end; begin += seqlen)
      {
         if (is_sameseq(seqbegin, &seqs[begin], seqlen))
            return begin;
      }
      return end;
   }

   void check_lengths()
   {
      length.set(constants::max_u32value, 0);

      for (ui_l32 i = 2; i < ui_l32(indices.size()); ++i)
      {
         if (indices[i] != indices[i - 1])
         {
            if (length.first > i)
               length.first = i;
            if (length.second < i)
               length.second = i;
         }
      }

      if (ranges.size())
      {
         if (length.first > 1)
            length.first = 1;
         if (length.second < 1)
            length.second = 1;
      }

      if (has_empty())
         length.first = 0;

      if (length.second IS 0)
         length.first = 0;
   }

   bool is_sameseq(const ui_l32 *const s1, const ui_l32 *const s2, const ui_l32 len) const
   {
      for (ui_l32 i = 0; i < len; ++i)
         if (s1[i] != s2[i])
            return false;
      return true;
   }
};
//  posdata_holder

   }  //  namespace re_detail

//  ... "rei_upos.hpp"]
//  ["rei_compiler.hpp" ...

   namespace re_detail
   {





#if !defined(SRELL_MAX_DEPTH) || ((SRELL_MAX_DEPTH + 0) == 0)
#define SRELL_MAX_DEPTH 256
#endif

template <typename charT, typename traits>
struct re_object_core
{
protected:

   typedef re_state/*<charT>*/ state_type;
   typedef simple_array<state_type> state_array;

   state_array NFA_states;
   re_character_class character_class;

   bitset<traits::utf_traits::bitsetsize> firstchar_class_bs;

public:

   std::size_t limit_counter;

protected:

   typedef typename traits::utf_traits utf_traits;

   ui_l32 number_of_brackets;
   ui_l32 number_of_counters;
   ui_l32 number_of_repeats;
   ui_l32 soflags;

   groupname_mapper<charT> namedcaptures;
   typedef typename groupname_mapper<charT>::gname_string gname_string;

   typedef re_bmh<charT, utf_traits> bmh_type;
   bmh_type *bmdata;

#if defined(SRELL_HAS_SSE42)
   __m128i simdranges;
#endif

private:

   static const std::size_t lcounter_defnum_ = (1 << 15) << 6;


protected:

   re_object_core()
      : limit_counter(lcounter_defnum_)
      , bmdata(nullptr)
   {
   }

   re_object_core(const re_object_core &right)
      : bmdata(nullptr)
   {
      operator=(right);
   }

#if defined(__cpp_rvalue_references)
   re_object_core(re_object_core &&right) SRELL_NOEXCEPT
      : bmdata(nullptr)
   {
      operator=(std::move(right));
   }
#endif

   ~re_object_core()
   {
      if (bmdata)
         delete bmdata;
   }

   void reset(const regex_constants::syntax_option_type flags)
   {
      NFA_states.clear();
      character_class.reset();

      firstchar_class_bs.clear();

      limit_counter = lcounter_defnum_;

      number_of_brackets = 1;
      number_of_counters = 0;
      number_of_repeats  = 0;
      soflags = ui_l32(flags);  //  regex_constants::ECMAScript;


      namedcaptures.clear();

      if (bmdata)
         delete bmdata;
      bmdata = nullptr;
   }

   re_object_core &operator=(const re_object_core &that)
   {
      if (this != &that)
      {
         this->NFA_states = that.NFA_states;
         this->character_class = that.character_class;

         this->firstchar_class_bs = that.firstchar_class_bs;

         this->limit_counter = that.limit_counter;

         this->number_of_brackets = that.number_of_brackets;
         this->number_of_counters = that.number_of_counters;
         this->number_of_repeats = that.number_of_repeats;
         this->soflags = that.soflags;

         this->namedcaptures = that.namedcaptures;

         if (that.bmdata)
         {
            if (this->bmdata)
               *this->bmdata = *that.bmdata;
            else
               this->bmdata = new bmh_type(*that.bmdata);
         }
         else if (this->bmdata)
         {
            delete this->bmdata;
            this->bmdata = nullptr;
         }
#if defined(SRELL_HAS_SSE42)
         simdranges = that.simdranges;
#endif

         if (that.NFA_states.size())
            repair_nextstates(&that.NFA_states[0]);
      }
      return *this;
   }

#if defined(__cpp_rvalue_references)
   re_object_core &operator=(re_object_core &&that) SRELL_NOEXCEPT
   {
      if (this != &that)
      {
         this->NFA_states = std::move(that.NFA_states);
         this->character_class = std::move(that.character_class);

         this->firstchar_class_bs = std::move(that.firstchar_class_bs);

         this->limit_counter = that.limit_counter;

         this->number_of_brackets = that.number_of_brackets;
         this->number_of_counters = that.number_of_counters;
         this->number_of_repeats = that.number_of_repeats;
         this->soflags = that.soflags;

         this->namedcaptures = std::move(that.namedcaptures);

         if (this->bmdata)
            delete this->bmdata;
         this->bmdata = that.bmdata;
         that.bmdata = nullptr;
#if defined(SRELL_HAS_SSE42)
         simdranges = that.simdranges;
#endif
      }
      return *this;
   }
#endif   //  defined(__cpp_rvalue_references)

   void swap(re_object_core &right)
   {
      if (this != &right)
      {
         this->NFA_states.swap(right.NFA_states);
         this->character_class.swap(right.character_class);

         this->firstchar_class_bs.swap(right.firstchar_class_bs);

         {
            const std::size_t tmp_limit_counter = this->limit_counter;
            this->limit_counter = right.limit_counter;
            right.limit_counter = tmp_limit_counter;
         }

         {
            const ui_l32 tmp_numof_brackets = this->number_of_brackets;
            this->number_of_brackets = right.number_of_brackets;
            right.number_of_brackets = tmp_numof_brackets;
         }
         {
            const ui_l32 tmp_numof_counters = this->number_of_counters;
            this->number_of_counters = right.number_of_counters;
            right.number_of_counters = tmp_numof_counters;
         }
         {
            const ui_l32 tmp_numof_repeats = this->number_of_repeats;
            this->number_of_repeats = right.number_of_repeats;
            right.number_of_repeats = tmp_numof_repeats;
         }
         {
            const ui_l32 tmp_soflags = this->soflags;
            this->soflags = right.soflags;
            right.soflags = tmp_soflags;
         }

         this->namedcaptures.swap(right.namedcaptures);

         {
            bmh_type *const tmp_bmdata = this->bmdata;
            this->bmdata = right.bmdata;
            right.bmdata = tmp_bmdata;
         }
#if defined(SRELL_HAS_SSE42)
         {
            const __m128i tmp = this->simdranges;
            this->simdranges = right.simdranges;
            right.simdranges = tmp;
         }
#endif
      }
   }

   bool set_error(const regex_constants::error_type e)
   {
//    reset();
      NFA_states.clear();
      soflags |= ui_l32(e) << constants::errshift;
      return false;
   }

   regex_constants::error_type ecode() const
   {
      return regex_constants::error_type(soflags >> constants::errshift);
   }

private:

   void repair_nextstates(const state_type *const oldbase)
   {
      state_type *const newbase = &this->NFA_states[0];

      for (typename state_array::size_type i = 0; i < this->NFA_states.size(); ++i)
      {
         state_type &state = this->NFA_states[i];

         if (state.next_state1)
            state.next_state1 = state.next_state1 - oldbase + newbase;

         if (state.next_state2)
            state.next_state2 = state.next_state2 - oldbase + newbase;
      }
   }
};
//  re_object_core

#if defined(SRELL_HAS_SSE42)

template <typename T>
struct cpu_checker
{
   static T x86simd()
   {
      static const T v = check_();

      return v;
   }

private:

   static T check_()
   {
#if defined(__GNUC__)
      return __builtin_cpu_supports("sse4.2") ? 1 : 0;
#elif defined(_MSC_VER)
      int cpuInfo[4];
      T v = 0;

      __cpuid(cpuInfo, 0);
      const int max = cpuInfo[0];

      if (max >= 1)
      {
         __cpuid(cpuInfo, 1);
         v |= (cpuInfo[2] & (1 << 20)) ? 1 : 0; //  ecx. SSE4.2.
      }
      return v;
#else
      return 0;
#endif
   }
};
//  cpu_checker

#endif   //  defined(SRELL_HAS_SSE42)

template <typename charT, typename traits>
class re_compiler : public re_object_core<charT, traits>
{
protected:

   template <typename InputIterator>
   bool compile(InputIterator begin, const InputIterator end, const regex_constants::syntax_option_type flags)
   {
      u32array u32;

                if (!to_u32array(u32, begin, end) or !compile_core(u32.data(), u32.data() + u32.size(), flags & regex_constants::pflagsmask_))
                {
                        if (this->bmdata)
                                delete this->bmdata;
                        this->bmdata = nullptr;
                        return false;
                }
                return true;
   }

   bool is_ricase() const
   {
      return this->NFA_states.size() and this->NFA_states[0].flags ? true : false;   //  icase.
   }

private:

   typedef re_object_core<charT, traits> base_type;
   typedef typename base_type::utf_traits utf_traits;
   typedef typename base_type::state_type state_type;
   typedef typename base_type::state_array state_array;
   typedef typename base_type::gname_string gname_string;
   typedef typename re_character_class::pstring pstring;
   typedef typename base_type::bmh_type bmh_type;
   typedef typename state_array::size_type state_size_type;

   typedef simple_array<ui_l32> u32array;
   typedef typename u32array::size_type u32array_size_type;

   typedef re_compiler_state<charT> cvars_type;

   template <typename InputIterator>
   bool to_u32array(u32array &u32, InputIterator begin, const InputIterator end)
   {
      while (begin != end)
      {
         const ui_l32 u32c = utf_traits::codepoint_inc(begin, end);

         if (u32c > constants::unicode_max_codepoint)
            return this->set_error(regex_constants::error_utf8);

         u32.push_back_c(u32c);
      }
      return true;
   }

   bool compile_core(const ui_l32 *begin, const ui_l32 *const end, const regex_constants::syntax_option_type flags)
   {
      re_quantifier piecesize;
      cvars_type cvars;
      state_type flstate;

      this->reset(flags);
      cvars.reset(flags, begin);

      flstate.reset(st_epsilon);
      flstate.next2 = 1;
      this->NFA_states.push_back(flstate);

      if (!make_nfa_states(this->NFA_states, piecesize, begin, end, cvars))
      {
         return false;
      }

      if (begin != end)
         return this->set_error(regex_constants::error_paren); //  ')'s are too many.

      setup_bmhdata();

      flstate.type = st_success;
      flstate.next1 = 0;
      flstate.next2 = 0;
      flstate.quantifier = piecesize;
      this->NFA_states.push_back(flstate);

      if (cvars.backref_used and !check_backreferences(cvars))
         return false;

      optimise(cvars);
      relativejump_to_absolutejump();

      return true;
   }

   bool make_nfa_states(state_array &piece, re_quantifier &piecesize, const ui_l32 *&curpos, const ui_l32 *const end, cvars_type &cvars)
   {
      const ui_l32 gno_at_groupbegin = this->number_of_brackets;
      bool already_pushed = false;
      state_size_type prevbranch_end = 0;
      state_type bstate;
      state_array branch;
      re_quantifier branchsize;

      piecesize.set(constants::infinity, 0u);

      bstate.reset(st_epsilon, epsilon_type::et_alt);

      for (;;)
      {
         branch.clear();

         if (!make_branch(branch, branchsize, curpos, end, cvars))
            return false;

         if (!piecesize.is_valid() or piecesize.atleast > branchsize.atleast)
            piecesize.atleast = branchsize.atleast;

         if (piecesize.atmost < branchsize.atmost)
            piecesize.atmost = branchsize.atmost;

         if (curpos != end and *curpos IS meta_char::mc_bar)
         {
            bstate.next2 = std::ptrdiff_t(branch.size()) + 2;
            branch.insert(0, bstate);

            if (gno_at_groupbegin != this->number_of_brackets)
            {
               if (!already_pushed)
               {
                  cvars.dupranges.push_back(gno_at_groupbegin);
                  cvars.dupranges.push_back(this->number_of_brackets);
                  already_pushed = true;
               }
               else
                  cvars.dupranges.back() = this->number_of_brackets;
            }
         }

         if (prevbranch_end)
         {
            state_type &pbend = piece[prevbranch_end];

            pbend.next1 = std::ptrdiff_t(branch.size()) + 1;
            pbend.char_num = epsilon_type::et_brnchend;  //  '/'
         }

         piece.append(branch);

         if (curpos IS end or *curpos IS meta_char::mc_rbracl)
            break;

         //  *curpos == '|'

         prevbranch_end = piece.size();
         bstate.next2 = 0;
         piece.push_back(bstate);

         ++curpos;
      }
      return true;
   }

   bool make_branch(state_array &branch, re_quantifier &branchsize, const ui_l32 *&curpos, const ui_l32 *const end, cvars_type &cvars)
   {
      state_array piece;
      state_array piece_with_quantifier;
      re_quantifier quantifier;
      range_pairs tmpcc;
      state_type astate;
      posdata_holder pos;

      branchsize.reset(0);

      for (;;)
      {
         if (curpos IS end or *curpos IS meta_char::mc_bar or *curpos IS meta_char::mc_rbracl)  //  '|', ')'.
            return true;

         piece.clear();
         piece_with_quantifier.clear();

         astate.reset(st_character, *curpos++);

         switch (astate.char_num)
         {
         case meta_char::mc_rbraop: //  '(':
            if (!parse_group(piece, astate.quantifier, curpos, end, cvars))
               return false;
            goto AFTER_PIECE_SET;

         case meta_char::mc_sbraop: //  '[':
            pos.clear();

            if (!parse_unicharset(pos, curpos, end, cvars))
               return false;

            if (pos.may_contain_strings())
               goto ADD_POS;

            tmpcc.swap(pos.ranges);

            astate.char_num = tmpcc.consists_of_one_character((regex_constants::icase & this->soflags & cvars.soflags) ? true : false);

            if (astate.char_num != constants::invalid_u32value)
            {
               const ui_l32 cf = unicode_case_folding::try_casefolding(astate.char_num);

               if ((this->soflags ^ cvars.soflags) & regex_constants::icase)
               {
                  if (cf != constants::invalid_u32value)
                     goto REGISTER_CC;
               }
               else if (cvars.is_icase() and cf != constants::invalid_u32value)
                  this->NFA_states[0].flags |= astate.flags = sflags::icase;
            }
            else
            {
               REGISTER_CC:
               astate.type = st_character_class;
               astate.char_num = this->character_class.register_newclass(tmpcc);
            }

            goto SKIP_ICASE_CHECK_FOR_CHAR;

         case meta_char::mc_escape: //  '\\':
            if (curpos IS end)
               return this->set_error(regex_constants::error_escape);

            astate.char_num = *curpos;

            if (astate.char_num >= char_alnum::ch_1 and astate.char_num <= char_alnum::ch_9)  //  \1, \9.
            {
               astate.char_num = translate_numbers(curpos, end, 10, 0, 0, 0xfffffffe);
                  //  22.2.1.1 Static Semantics: Early Errors:
                  //  It is a Syntax Error if NcapturingParens >= 23^2 - 1.

               if (astate.char_num IS constants::invalid_u32value)
                  return this->set_error(regex_constants::error_escape);

               astate.flags = 0u;

               BACKREF_POSTPROCESS:
               astate.next2 = 1;
               astate.type = st_backreference;
               astate.quantifier.atleast = 0;

               cvars.backref_used = true;

               if (cvars.is_icase())
                  astate.flags |= sflags::icase;

               break;
            }

            ++curpos;

            switch (astate.char_num)
            {
            case char_alnum::ch_B:  //  'B':
               astate.flags = sflags::is_not;
               //@fallthrough@

            case char_alnum::ch_b:  //  'b':
               astate.type = st_boundary; //  \b, \B.
               astate.quantifier.reset(0);
               astate.char_num = ui_l32(!cvars.is_icase() ? re_character_class::word : re_character_class::icase_word);   //  \w, \W.
               break;

//          case char_alnum::ch_A:  //  'A':
//             astate.type = st_bol;   //  '\A'
//          case char_alnum::ch_Z:  //  'Z':
//             astate.type = st_eol;   //  '\Z'
//          case char_alnum::ch_z:  //  'z':
//             astate.type = st_eol;   //  '\z'

            case char_alnum::ch_k:  //  'k':
               if (curpos IS end or *curpos != meta_char::mc_lt)
                  return this->set_error(regex_constants::error_escape);
               else
               {
                  const gname_string groupname = get_groupname(++curpos, end, cvars);

                  if (groupname.size() IS 0)
                     return false;

                  astate.flags = sflags::backrefno_unresolved;
                  astate.char_num = ui_l32(cvars.unresolved_gnames.size() + 1);
                  astate.char_num = cvars.unresolved_gnames.assign_number(groupname, astate.char_num);
                  goto BACKREF_POSTPROCESS;
               }
            default:
               pos.clear();
               if (!translate_escape(pos, astate, curpos, end, false, cvars))
                  return false;

               if (pos.may_contain_strings())
               {
                  ADD_POS:
                  transform_seqdata(piece, pos, cvars);
                  astate.quantifier.set(pos.length.first, pos.length.second);
                  goto AFTER_PIECE_SET;
               }

               if (astate.type IS st_character_class)
                  astate.char_num = this->character_class.register_newclass(pos.ranges);
            }

            break;

         case meta_char::mc_period: //  '.':
            astate.type = st_character_class;
            if (cvars.is_dotall())
            {
               astate.char_num = ui_l32(re_character_class::dotall);
            }
            else
            {
               this->character_class.copy_to(tmpcc, ui_l32(re_character_class::newline));

               tmpcc.negation();
               astate.char_num = this->character_class.register_newclass(tmpcc);
            }
            break;

         case meta_char::mc_caret:  //  '^':
            astate.type = st_bol;
            astate.char_num = ui_l32(re_character_class::newline);
            astate.quantifier.reset(0);
            if (cvars.is_multiline())
               astate.flags = sflags::multiline;
            break;

         case meta_char::mc_dollar: //  '$':
            astate.type = st_eol;
            astate.char_num = ui_l32(re_character_class::newline);
            astate.quantifier.reset(0);
            if (cvars.is_multiline())
               astate.flags = sflags::multiline;
            break;

         case meta_char::mc_astrsk: //  '*':
         case meta_char::mc_plus:   //  '+':
         case meta_char::mc_query:  //  '?':
         case meta_char::mc_cbraop: //  '{'
            return this->set_error(regex_constants::error_badrepeat);

         case meta_char::mc_cbracl: //  '}'
            return this->set_error(regex_constants::error_brace);

         case meta_char::mc_sbracl: //  ']'
            return this->set_error(regex_constants::error_brack);

         default:;
         }

         if (astate.type IS st_character and ((this->soflags | cvars.soflags) & regex_constants::icase))
         {
            const ui_l32 cf = unicode_case_folding::try_casefolding(astate.char_num);

            if (cf != constants::invalid_u32value)
            {
               if ((this->soflags ^ cvars.soflags) & regex_constants::icase)
               {
                  tmpcc.set_solerange(range_pair_helper(astate.char_num));
                  if (cvars.is_icase())
                     tmpcc.make_caseunfoldedcharset();
                  astate.char_num = this->character_class.register_newclass(tmpcc);
                  astate.type = st_character_class;
               }
               else
               {
                  astate.char_num = cf;
                  this->NFA_states[0].flags |= astate.flags = sflags::icase;
               }
            }
         }
         SKIP_ICASE_CHECK_FOR_CHAR:

         piece.push_back(astate);
         AFTER_PIECE_SET:

         if (piece.size())
         {
            const state_type &firststate = piece[0];

            quantifier.reset();

            if (firststate.has_quantifier() and curpos != end)
            {
               switch (*curpos)
               {
               case meta_char::mc_astrsk: //  '*':
                  --quantifier.atleast;
                  //@fallthrough@

               case meta_char::mc_plus:   //  '+':
                  quantifier.set_infinity();
                  break;

               case meta_char::mc_query:  //  '?':
                  --quantifier.atleast;
                  break;

               case meta_char::mc_cbraop: //  '{':
                  ++curpos;
                  quantifier.atleast = translate_numbers(curpos, end, 10, 1, 0, constants::max_u32value);

                  if (quantifier.atleast IS constants::invalid_u32value)
                     return this->set_error(regex_constants::error_brace);

                  if (curpos IS end)
                     return this->set_error(regex_constants::error_brace);

                  if (*curpos IS meta_char::mc_comma) //  ','
                  {
                     ++curpos;
                     quantifier.atmost = translate_numbers(curpos, end, 10, 1, 0, constants::max_u32value);

                     if (quantifier.atmost IS constants::invalid_u32value)
                        quantifier.set_infinity();

                     if (!quantifier.is_valid())
                        return this->set_error(regex_constants::error_badbrace);
                  }
                  else
                     quantifier.atmost = quantifier.atleast;

                  if (curpos IS end or *curpos != meta_char::mc_cbracl) //  '}'
                     return this->set_error(regex_constants::error_brace);

                  //  *curpos == '}'
                  break;

               default:
                  goto AFTER_GREEDINESS_CHECK;
               }

               if (++curpos != end and *curpos IS meta_char::mc_query)   //  '?'
               {
                  quantifier.is_greedy = 0u;
                  ++curpos;
               }
               AFTER_GREEDINESS_CHECK:;
            }

            if (piece.size() IS 2 and firststate.is_ncgroup_open())
            {
               //  (?:) alone or followed by a quantifier.
//             piece_with_quantifier += piece;
               ;  //  Does nothing.
            }
            else if (!combine_piece_with_quantifier(piece_with_quantifier, piece, quantifier, astate.quantifier))
               return false;

            astate.quantifier.multiply(quantifier);
            branchsize.add(astate.quantifier);


            if (!cvars.is_back())
               branch.append(piece_with_quantifier);
            else
               branch.insert(0, piece_with_quantifier);
         }
      }
   }

   //  '('.

   bool parse_group(state_array &piece, re_quantifier &piecesize, const ui_l32 *&curpos, const ui_l32 *const end, cvars_type &cvars)
   {
      const ui_l32 originalflags(cvars.soflags);
      state_type rbstate;

      if (curpos IS end)
         return this->set_error(regex_constants::error_paren);

      rbstate.reset(st_roundbracket_open);

      if (*curpos IS meta_char::mc_query) //  '?'
      {
         if (++curpos IS end)
            return this->set_error(regex_constants::error_paren);

         rbstate.char_num = *curpos;

         if (rbstate.char_num IS meta_char::mc_lt) //  '<'
         {
            if (++curpos IS end)
               return this->set_error(regex_constants::error_paren);

            rbstate.char_num = *curpos;

            if (rbstate.char_num != meta_char::mc_eq and rbstate.char_num != meta_char::mc_exclam)
            {
               const gname_string groupname = get_groupname(curpos, end, cvars);

               if (groupname.size() IS 0)
                  return false;

               const int res = this->namedcaptures.push_back(groupname, this->number_of_brackets, cvars.dupranges);

               if (res IS 0)
                  return this->set_error(regex_constants::error_backref);

               goto AFTER_EXTRB;
            }
            //  "(?<=" or "(?<!"
         }
         else
            rbstate.quantifier.is_greedy = 0;
            //  0: Other than lookbehind assertions.
            //  1: Lookbehind.

         switch (rbstate.char_num)
         {
         case meta_char::mc_exclam: //  '!':
            rbstate.flags = sflags::is_not;
            //@fallthrough@

         case meta_char::mc_eq:  //  '=':
            cvars.soflags = rbstate.quantifier.is_greedy ? (cvars.soflags | regex_constants::back_) : (cvars.soflags & ~regex_constants::back_);

#if defined(SRELL_ENABLE_GT)
            //@fallthrough@
         case meta_char::mc_gt:
#endif
            rbstate.type = st_lookaround_open;
            rbstate.next2 = 1;
            rbstate.quantifier.atleast = this->number_of_brackets;
            piece.push_back(rbstate);
            rbstate.next1 = 1;
            rbstate.next2 = 0;
            rbstate.type = st_lookaround_pop;
            break;

         default:
            {
               const u32array_size_type boffset = curpos - cvars.begin;
               ui_l32 to_be_modified = 0;
               ui_l32 modified = 0;
               ui_l32 localflags = cvars.soflags;
               bool negate = false;

               for (;;)
               {
                  switch (rbstate.char_num)
                  {
                  case meta_char::mc_colon:  //  ':':
                     //  (?ims-ims:...)
                     if (modified)
                     {
                        if (modified & (regex_constants::unicodesets | regex_constants::sticky | regex_constants::nosubs))
                           goto ERROR_PAREN;

                        goto COLON_FOUND;
                     }
                     //  "(?-:"
                     goto ERROR_MODIFIER;
#if !defined(SRELL_NO_UBMOD)
                  case meta_char::mc_rbracl: //  ')':
                     if (modified)
                     {
                        cvars.soflags = localflags;
                        if (boffset IS 2)
                        {
                           this->soflags = localflags;
                        }
                        else if (modified & regex_constants::sticky)
                           goto ERROR_MODIFIER;

                        if (boffset IS 2) //  Restricts so that unbounded forms (?ims-ims) can be used only at the beginning of an expression.
                        {
                           ++curpos;
                           return true;
                        }
                     }
                     //  "(?)" or "(?-)"
                     goto ERROR_MODIFIER;
#endif
                  case meta_char::mc_minus:  //  '-':
                     if (negate)
                        goto ERROR_MODIFIER;
                     negate = true;
                     break;

                  case char_alnum::ch_i:  //  'i':
                     to_be_modified = regex_constants::icase;
                     goto TRY_MODIFICATION;

                  case char_alnum::ch_m:  //  'm':
                     to_be_modified = regex_constants::multiline;
                     goto TRY_MODIFICATION;

                  case char_alnum::ch_s:  //  's':
                     to_be_modified = regex_constants::dotall;
                     goto TRY_MODIFICATION;

                  case char_alnum::ch_v:  //  'v':
                     to_be_modified = regex_constants::unicodesets;
                     goto TRY_MODIFICATION;

                  case char_alnum::ch_y:  //  'y':
                     to_be_modified = regex_constants::sticky;
                     goto TRY_MODIFICATION;

                  case char_alnum::ch_n:  //  'n':
                     to_be_modified = regex_constants::nosubs;
                     goto TRY_MODIFICATION;

                  default:
                     ERROR_PAREN:
                     return this->set_error(regex_constants::error_paren);

                     TRY_MODIFICATION:
                     if (modified & to_be_modified)
                     {
                        ERROR_MODIFIER:
                        return this->set_error(regex_constants::error_modifier);
                     }

                     modified |= to_be_modified;
                     if (!negate)
                        localflags |= to_be_modified;
                     else
                        localflags &= ~to_be_modified;
                  }

                  if (++curpos IS end)
                     goto ERROR_PAREN;

                  rbstate.char_num = *curpos;
               }
               COLON_FOUND:;
               cvars.soflags = localflags;
            }
            //@fallthrough@

         case meta_char::mc_colon:
            ++curpos;
            goto NCGROUP;
         }

         ++curpos;
         piece.push_back(rbstate);
      }
      else
      {
         if (cvars.is_nosubs())
         {
            NCGROUP:
            rbstate.type = st_epsilon;
            rbstate.char_num = epsilon_type::et_ncgopen;
            rbstate.quantifier.atleast = this->number_of_brackets;
         }
         else
         {
            AFTER_EXTRB:
            if (this->number_of_brackets > constants::max_u32value)
               return this->set_error(regex_constants::error_complexity);

            rbstate.char_num = this->number_of_brackets++;
            rbstate.next1 = 2;
            rbstate.next2 = 1;
            rbstate.quantifier.atleast = this->number_of_brackets;
            piece.push_back(rbstate);

            rbstate.type  = st_roundbracket_pop;
            rbstate.next1 = 0;
            rbstate.next2 = 0;
         }
         piece.push_back(rbstate);
      }

      const u32array_size_type dzsize = cvars.dupranges.size();

      if (++cvars.depth > SRELL_MAX_DEPTH)
         return this->set_error(regex_constants::error_complexity);

      if (!make_nfa_states(piece, piecesize, curpos, end, cvars))
         return false;

      //  end or ')'?
      if (curpos IS end)
         return this->set_error(regex_constants::error_paren);

      --cvars.depth;
      ++curpos;

      cvars.dupranges.resize(dzsize);
      cvars.soflags = originalflags;

      state_type &firststate = piece[0];

      firststate.quantifier.atmost = this->number_of_brackets - 1;

      switch (rbstate.type)
      {
      case st_epsilon:
         if (piece.size() IS 2)  //  ':' + something.
         {
            piece.erase(0);
            return true;
         }

         firststate.quantifier.is_greedy = piecesize.atleast != 0u;
         rbstate.char_num = epsilon_type::et_ncgclose;
         break;

      case st_lookaround_pop:

#if defined(SRELL_ENABLE_GT)
         if (firststate.char_num != meta_char::mc_gt)
#endif
            piecesize.reset(0);

         firststate.next1 = std::ptrdiff_t(piece.size()) + 1;
         piece[1].quantifier.atmost = firststate.quantifier.atmost;

         rbstate.type  = st_lookaround_close;
         rbstate.next1 = 0;
         break;

      default:
         rbstate.type = st_roundbracket_close;
         rbstate.next1 = 1;
         rbstate.next2 = 1;

         piece[1].quantifier.atmost = firststate.quantifier.atmost;
         firststate.quantifier.is_greedy = piecesize.atleast != 0u;
      }

      piece.push_back(rbstate);
      return true;
   }

   bool combine_piece_with_quantifier(state_array &piece_with_quantifier, state_array &piece, const re_quantifier &quantifier, const re_quantifier &piecesize)
   {
      if (quantifier.atmost IS 0)
         return true;

      state_type &firststate = piece[0];
      state_type qstate;

      qstate.reset(st_epsilon, firststate.is_character_or_class()
         ? epsilon_type::et_ccastrsk
         : epsilon_type::et_dfastrsk);
      qstate.quantifier = quantifier;

      if (quantifier.atmost IS 1)
      {
         if (quantifier.atleast IS 0)
         {
            qstate.next2 = std::ptrdiff_t(piece.size()) + 1;
            if (!quantifier.is_greedy)
            {
               qstate.next1 = qstate.next2;
               qstate.next2 = 1;
            }

            piece[piece.size() - 1].quantifier = quantifier;
            piece_with_quantifier.push_back(qstate);
         }

         if (firststate.type IS st_roundbracket_open)
            firststate.quantifier.atmost = piece[1].quantifier.atmost = 0;

         piece_with_quantifier.append(piece);
         return true;
      }

      //  atmost >= 2


      //  A counter requires at least 6 states: save, restore, check, inc, dec, ATOM(s).
      //  A character or charclass quantified by one of these has a simple equivalent representation:
      //  a{0,2}  1.epsilon(2|5), 2.CHorCL(3), 3.epsilon(4|5), 4.CHorCL(5), [5].
      //  a{0,3}  1.epsilon(2|7), 2.CHorCL(3), 3.epsilon(4|7), 4.CHorCL(5), 5.epsilon(6|7), 6.CHorCL(7), [7].
      //  a{1,2}  1.CHorCL(2), 2.epsilon(3|4), 3.CHorCL(4), [4].
      //  a{1,3}  1.CHorCL(2), 2.epsilon(3|6), 3.CHorCL(4), 4.epsilon(5|6), 5.CHorCL(6), [6].
      //  a{2,3}  1.CHorCL(2), 2.CHorCL(3), 3.epsilon(4|5), 4.CHorCL(5), [5].
      //  a{2,4}  1.CHorCL(2), 2.CHorCL(3), 3.epsilon(4|7), 4.CHorCL(5), 5.epsilon(6|7), 6.CHorCL(7), [7].
      if (qstate.char_num IS epsilon_type::et_ccastrsk and quantifier.has_simple_equivalence())
      {
         const state_size_type branchsize = piece.size() + 1;

         for (ui_l32 i = 0; i < quantifier.atleast; ++i)
            piece_with_quantifier.append(piece);

         firststate.quantifier.set(0, 1, quantifier.is_greedy);

         qstate.next2 = (quantifier.atmost - quantifier.atleast) * branchsize;
         if (!quantifier.is_greedy)
         {
            qstate.next1 = qstate.next2;
            qstate.next2 = 1;
         }

         for (ui_l32 i = quantifier.atleast; i < quantifier.atmost; ++i)
         {
            piece_with_quantifier.push_back(qstate);
            piece_with_quantifier.append(piece);
            quantifier.is_greedy ? (qstate.next2 -= branchsize) : (qstate.next1 -= branchsize);
         }
         return true;
      }

      if (firststate.type IS st_backreference and (firststate.flags & sflags::backrefno_unresolved))
      {
         firststate.quantifier = quantifier;
         qstate.quantifier.set(1, 0);
         goto ADD_CHECKER;
      }
      else if (firststate.is_ncgroup_open() and (piecesize.atleast IS 0 or firststate.quantifier.is_valid()))
      {
         qstate.quantifier = firststate.quantifier;
         ADD_CHECKER:

         if (this->number_of_repeats > constants::max_u32value)
            return this->set_error(regex_constants::error_complexity);

         qstate.char_num = this->number_of_repeats++;

         qstate.type = st_repeat_in_pop;
         qstate.next1 = 0;
         qstate.next2 = 0;
         piece.insert(0, qstate);

         qstate.type = st_repeat_in_push;
         qstate.next1 = 2;
         qstate.next2 = 1;
         piece.insert(0, qstate);

         qstate.quantifier = quantifier;
         qstate.type = st_check_0_width_repeat;
         qstate.next2 = 1;
         piece.push_back(qstate);

         if (piecesize.atleast IS 0 and piece[2].type != st_backreference)
            goto USE_COUNTER;

         qstate.char_num = epsilon_type::et_dfastrsk;
      }

      qstate.type = st_epsilon;

      if (quantifier.is_asterisk()) //  {0,}
      {
         //  greedy:  1.epsilon(2|4), 2.piece, 3.LAorC0WR(1|0), 4.OutOfLoop.
         //  !greedy: 1.epsilon(4|2), 2.piece, 3.LAorC0WR(1|0), 4.OutOfLoop.
         //  LAorC0WR: LastAtomOfPiece or Check0WidthRepeat.
      }
      else if (quantifier.is_plus())   //  {1,}
      {

         if (qstate.char_num IS epsilon_type::et_ccastrsk)
         {
            piece_with_quantifier.append(piece);
            --qstate.quantifier.atleast;  //  /.+/ -> /..*/.
         }
         else
         {
            const ui_l32 backup = qstate.char_num;

            qstate.next1 = 2;
            qstate.next2 = 0;
            qstate.char_num = epsilon_type::et_jmpinlp;
            piece_with_quantifier.push_back(qstate);
            qstate.char_num = backup;
            //  greedy:  1.epsilon(3), 2.epsilon(3|5), 3.piece, 4.LAorC0WR(2|0), 5.OutOfLoop.
            //  !greedy: 1.epsilon(3), 2.epsilon(5|3), 3.piece, 4.LAorC0WR(2|0), 5.OutOfLoop.
         }
      }
      else
      {
         if (qstate.char_num IS epsilon_type::et_ccastrsk and quantifier.is_infinity())
         {
            if (quantifier.atleast <= 6)
            {
               for (ui_l32 i = 0; i < quantifier.atleast; ++i)
                  piece_with_quantifier.append(piece);
               qstate.quantifier.atleast = 0;
               goto APPEND_ATOM;
            }
            qstate.quantifier.atmost = qstate.quantifier.atleast;
         }

         USE_COUNTER:

         if (this->number_of_counters > constants::max_u32value)
            return this->set_error(regex_constants::error_complexity);

         qstate.char_num = this->number_of_counters++;

         qstate.type = st_save_and_reset_counter;
         qstate.next1 = 2;
         qstate.next2 = 1;
         piece_with_quantifier.push_back(qstate);

         qstate.type = st_restore_counter;
         qstate.next1 = 0;
         qstate.next2 = 0;
         piece_with_quantifier.push_back(qstate);
         //  1.save_and_reset_counter(3|2), 2.restore_counter(0|0),

         qstate.type = st_decrement_counter;
         piece.insert(0, qstate);

         qstate.next1 = 2;
         qstate.next2 = piece[1].is_character_or_class() ? 0 : 1;
         qstate.type = st_increment_counter;
         piece.insert(0, qstate);

         qstate.type = st_check_counter;
         //  greedy:  3.check_counter(4|6), 4.piece, 5.LAorC0WR(3|0), 6.OutOfLoop.
         //  !greedy: 3.check_counter(6|4), 4.piece, 5.LAorC0WR(3|0), 6.OutOfLoop.
         //  4.piece = { 4a.increment_counter(4c|4b), 4b.decrement_counter(0|0), 4c.OriginalPiece }.
      }

      APPEND_ATOM:

      const std::ptrdiff_t piece_size = std::ptrdiff_t(piece.size());
      state_type &laststate = piece[piece_size - 1];

      laststate.quantifier = qstate.quantifier;
      laststate.next1 = 0 - piece_size;

      qstate.next1 = 1;
      qstate.next2 = piece_size + 1;
      if (!quantifier.is_greedy)
      {
         qstate.next1 = qstate.next2;
         qstate.next2 = 1;
      }
      piece_with_quantifier.push_back(qstate);
      piece_with_quantifier.append(piece);


      if (qstate.quantifier.atmost != quantifier.atmost)
      {
         qstate.type = st_epsilon;
         qstate.char_num = epsilon_type::et_ccastrsk;
         qstate.quantifier.atleast = 0;
         qstate.quantifier.atmost = quantifier.atmost;
         piece.erase(0, piece_size - 1);
         goto APPEND_ATOM;
      }

      return true;
   }

   //  '['.

   bool parse_unicharset(posdata_holder &basepos, const ui_l32 *&curpos, const ui_l32 *const end, cvars_type &cvars)
   {
      if (curpos IS end)
         return this->set_error(regex_constants::error_brack);

      const bool is_umode = !cvars.is_vmode();
      const bool invert = (*curpos IS meta_char::mc_caret) ? (++curpos, true) : false; //  '^'
      enum operation_type
      {
         op_init, op_firstcc, op_union, op_intersection, op_subtraction
      };
      operation_type otype = op_init;
      posdata_holder newpos;
      range_pair code_range;
      state_type castate;

      //  ClassSetCharacter ::
      //  \ CharacterEscape[+UnicodeMode]
      //  \ ClassSetReservedPunctuator
      //  \ b

      for (;;)
      {
         if (curpos IS end)
            goto ERROR_NOT_CLOSED;

         if (*curpos IS meta_char::mc_sbracl)   //   ']'
            break;

         if (!is_umode)
         {
            ui_l32 next2chars = constants::invalid_u32value;

            if (curpos + 1 != end and *curpos IS curpos[1])
            {
               switch (*curpos)
               {
               //  ClassSetReservedDoublePunctuator :: one of
               //  and !! ## $$ %% ** ++ ,, .. :: ;; << == >> ?? @@ ^^ `` ~~
               case char_other::co_amp:   //  '&'
               case meta_char::mc_exclam: //  '!'
               case meta_char::mc_sharp:  //  '#'
               case meta_char::mc_dollar: //  '$'
               case char_other::co_perc:  //  '%'
               case meta_char::mc_astrsk: //  '*'
               case meta_char::mc_plus:   //  '+'
               case meta_char::mc_comma:  //  ','
               case meta_char::mc_period: //  '.'
               case meta_char::mc_colon:  //  ':'
               case char_other::co_smcln: //  ';'
               case meta_char::mc_lt:     //  '<'
               case meta_char::mc_eq:     //  '='
               case meta_char::mc_gt:     //  '>'
               case meta_char::mc_query:  //  '?'
               case char_other::co_atmrk: //  '@'
               case meta_char::mc_caret:  //  '^'
               case char_other::co_grav:  //  '`'
               case char_other::co_tilde: //  '~'
               case meta_char::mc_minus:  //  '-'
                  next2chars = *curpos;
                  //@fallthrough@
               default:;
               }
            }

            switch (otype)
            {
            case op_intersection:
               if (next2chars != char_other::co_amp)
                  goto ERROR_DOUBLE_PUNCT;
               curpos += 2;
               break;

            case op_subtraction:
               if (next2chars != meta_char::mc_minus)
                  goto ERROR_DOUBLE_PUNCT;
               curpos += 2;
               break;

            case op_firstcc:
               if (next2chars IS char_other::co_amp)
                  otype = op_intersection;
               else if (next2chars IS meta_char::mc_minus)
                  otype = op_subtraction;
               else if (next2chars IS constants::invalid_u32value)
                  break;
               else
                  goto ERROR_DOUBLE_PUNCT;

               curpos += 2;
               break;

//          case op_union:
//          case op_init:
            default:
               if (next2chars != constants::invalid_u32value)
                  goto ERROR_DOUBLE_PUNCT;
            }
         }

         AFTER_OPERATOR:

         if (curpos IS end)
            goto ERROR_NOT_CLOSED;

         castate.reset();

         if (!is_umode and *curpos IS meta_char::mc_sbraop)  //  '['
         {
            if (++cvars.depth > SRELL_MAX_DEPTH)
               return this->set_error(regex_constants::error_complexity);

            ++curpos;
            if (!parse_unicharset(newpos, curpos, end, cvars))
               return false;
            --cvars.depth;
         }
         else if (!get_classatom(newpos, castate, curpos, end, cvars, false))
            return false;

         if (curpos IS end)
            goto ERROR_NOT_CLOSED;

         if (otype IS op_init)
            otype = op_firstcc;
         else if (otype IS op_firstcc)
            otype = op_union;

         if (castate.type IS st_character_class)
         {
            //  In the u-mode, '-' following a character class is an error except "-]", immediately before ']'.
            if (is_umode and curpos != end and *curpos IS meta_char::mc_minus)  //  '-'
               if ((curpos + 1) != end and curpos[1] != meta_char::mc_sbracl)
                  goto ERROR_BROKEN_RANGE;
         }
         else if (castate.type IS st_character)
         {
            if (!newpos.has_data())
            {
               code_range.set(castate.char_num);

               if (otype <= op_union)
               {
                  if (*curpos IS meta_char::mc_minus and (curpos + 1) != end and curpos[1] != meta_char::mc_sbracl) //  '-'
                  {
                     ++curpos;

                     if (!is_umode and otype < op_union and *curpos IS meta_char::mc_minus) //  '-'
                     {
                        otype = op_subtraction;
                        ++curpos;
                        basepos.ranges.join(code_range);
                        goto AFTER_OPERATOR;
                     }

                     if (!get_classatom(newpos, castate, curpos, end, cvars, true))
                        return false;

                     otype = op_union;
                     code_range.second = castate.char_num;
                     if (!code_range.is_range_valid())
                        goto ERROR_BROKEN_RANGE;
                  }
               }

               newpos.ranges.join(code_range);
               if (cvars.is_icase())
                  newpos.ranges.make_caseunfoldedcharset();
            }
         }

         if (is_umode)
            basepos.ranges.merge(newpos.ranges);
         else
         {
            switch (otype)
            {
            case op_union:
               basepos.do_union(newpos);
               break;

            case op_intersection:
               basepos.do_and(newpos);
               break;

            case op_subtraction:
               basepos.do_subtract(newpos);
               break;

//          case op_firstcc:
            default:
               basepos.swap(newpos);
            }
         }
      }

      //  *curpos == ']'
      ++curpos;

      if (invert)
      {
         if (basepos.may_contain_strings())
            return this->set_error(regex_constants::error_complement);

         basepos.ranges.negation();
      }

      return true;

      ERROR_NOT_CLOSED:
      return this->set_error(regex_constants::error_brack);

      ERROR_BROKEN_RANGE:
      return this->set_error(regex_constants::error_range);

      ERROR_DOUBLE_PUNCT:
      return this->set_error(regex_constants::error_operator);
   }

   bool get_classatom(
      posdata_holder &pos,
      state_type &castate,
      const ui_l32 *&curpos,
      const ui_l32 *const end,
      const cvars_type &cvars,
      const bool no_ccesc
   )
   {
      pos.clear();

      castate.char_num = *curpos++;

      switch (castate.char_num)
      {
      //  ClassSetSyntaxCharacter :: one of
      //  ( ) [ ] { } / - \ |
      case meta_char::mc_rbraop: //  '('
      case meta_char::mc_rbracl: //  ')'
      case meta_char::mc_sbraop: //  '['
      case meta_char::mc_sbracl: //  ']'
      case meta_char::mc_cbraop: //  '{'
      case meta_char::mc_cbracl: //  '}'
      case char_other::co_slash: //  '/'
      case meta_char::mc_minus:  //  '-'
      case meta_char::mc_bar:    //  '|'
         return !cvars.is_vmode() ? true : this->set_error(regex_constants::error_noescape);

      case meta_char::mc_escape: //  '\\'
         break;

      default:
         return true;
      }

      if (curpos IS end)
         return this->set_error(regex_constants::error_escape);

      castate.char_num = *curpos++;

      switch (castate.char_num)
      {
      case char_alnum::ch_b:
         castate.char_num = char_ctrl::cc_bs;   //  '\b' 0x08:BS
         //@fallthrough@

      case meta_char::mc_minus:  //  '-'
         return true;

      //  ClassSetReservedPunctuator :: one of
      //  & - ! # % , : ; < = > @ ` ~
      case char_other::co_amp:   //  '&'
      case meta_char::mc_exclam: //  '!'
      case meta_char::mc_sharp:  //  '#'
      case char_other::co_perc:  //  '%'
      case meta_char::mc_comma:  //  ','
      case meta_char::mc_colon:  //  ':'
      case char_other::co_smcln: //  ';'
      case meta_char::mc_lt:     //  '<'
      case meta_char::mc_eq:     //  '='
      case meta_char::mc_gt:     //  '>'
      case char_other::co_atmrk: //  '@'
      case char_other::co_grav:  //  '`'
      case char_other::co_tilde: //  '~'
         if (cvars.is_vmode())
            return true;
         break;

#if !defined(SRELL_NO_UNICODE_POS)
      case char_alnum::ch_q:  //  '\\q'
         if (cvars.is_vmode() and !no_ccesc)
         {
            if (curpos IS end or *curpos != meta_char::mc_cbraop) //  '{'
               return this->set_error(regex_constants::error_escape);

            u32array seqs;
            u32array curseq;
            posdata_holder dummypos;
            state_type castate2;

            ++curpos;

            for (;;)
            {
               if (curpos IS end)
                  return this->set_error(regex_constants::error_escape);

               if (*curpos IS meta_char::mc_bar or *curpos IS meta_char::mc_cbracl) //  '|' or '}'.
               {
                  const ui_l32 seqlen = ui_l32(curseq.size());

                  if (seqlen <= 1)
                  {
                     seqs.push_back_c(2);
                     seqs.push_back_c(seqlen != 0 ? curseq[0] : constants::ccstr_empty);
                  }
                  else  //  >= 2
                  {
                     seqs.push_back_c(seqlen + 1);
                     seqs.append(curseq);
                  }

                  if (*curpos IS meta_char::mc_cbracl)   //  '}'
                     break;

                  curseq.clear();
                  ++curpos;
               }
               else
               {
                  castate2.reset();
                  if (!get_classatom(dummypos, castate2, curpos, end, cvars, true))
                     return false;

                  curseq.push_back_c(castate2.char_num);
               }
            }

            ++curpos;
            pos.split_seqs_and_ranges(seqs, cvars.is_icase(), cvars.is_back());

            return true;
         }
         //@fallthrough@
#endif   //  !defined(SRELL_NO_UNICODE_POS)

      default:;
      }

      return translate_escape(pos, castate, curpos, end, no_ccesc, cvars);
   }

   bool translate_escape(posdata_holder &pos, state_type &eastate, const ui_l32 *&curpos, const ui_l32 *const end, const bool no_ccesc, const cvars_type &cvars)
   {
      if (!no_ccesc)
      {
         //  Predefined classes.
         switch (eastate.char_num)
         {
         case char_alnum::ch_D:  //  'D':
            eastate.flags = sflags::is_not;
            //@fallthrough@

         case char_alnum::ch_d:  //  'd':
            eastate.char_num = ui_l32(re_character_class::digit);   //  \d, \D.
            break;

         case char_alnum::ch_S:  //  'S':
            eastate.flags = sflags::is_not;
            //@fallthrough@

         case char_alnum::ch_s:  //  's':
            eastate.char_num = ui_l32(re_character_class::space);   //  \s, \S.
            break;

         case char_alnum::ch_W:  //  'W':
            eastate.flags = sflags::is_not;
            //@fallthrough@

         case char_alnum::ch_w:  //  'w':
            eastate.char_num = ui_l32(!cvars.is_icase() ? re_character_class::word : re_character_class::icase_word);  //  \w, \W.
            break;

         case char_alnum::ch_P:  //  \P{...}
            eastate.flags = sflags::is_not;
            //@fallthrough@

         case char_alnum::ch_p:  //  \p{...}
         {
            pstring pname;
            pstring pvalue;

            if (curpos IS end or *curpos != meta_char::mc_cbraop) //  '{'
               return this->set_error(regex_constants::error_property);

            const bool digit_seen = get_property_name_or_value(pvalue, ++curpos, end);

            if (!pvalue.size())
               return this->set_error(regex_constants::error_property);

            if (!digit_seen)
            {
               if (curpos IS end)
                  return this->set_error(regex_constants::error_property);

               if (*curpos IS meta_char::mc_eq) //  '='
               {
                  pname = pvalue;
                  get_property_name_or_value(pvalue, ++curpos, end);
                  if (!pvalue.size())
                     return this->set_error(regex_constants::error_property);
               }
            }

            if (curpos IS end or *curpos != meta_char::mc_cbracl) //  '}'
               return this->set_error(regex_constants::error_property);

            ++curpos;

            pname.push_back_c(0);
            pvalue.push_back_c(0);

            eastate.char_num = this->character_class.get_propertynumber(pname, pvalue);

            if (eastate.char_num IS up_constants::error_property)
               return this->set_error(regex_constants::error_property);

            if (!this->character_class.is_pos(eastate.char_num))
            {
               pos.clear();

               this->character_class.load_upranges(pos.ranges, eastate.char_num);

               if (cvars.is_vmode() and cvars.is_icase() and eastate.char_num >= ui_l32(re_character_class::number_of_predefcls))
                  pos.ranges.make_caseunfoldedcharset();

               if (eastate.flags)   //  is_not.
               {
                  pos.ranges.negation();
                  eastate.flags = 0u;
               }

               if (!cvars.is_vmode() and cvars.is_icase())
                  pos.ranges.make_caseunfoldedcharset();

               eastate.type = st_character_class;
               eastate.quantifier.reset(1);
            }
            else
            {
#if !defined(SRELL_NO_UNICODE_POS)
               if (!cvars.is_vmode())
#endif
                  return this->set_error(regex_constants::error_property);

               u32array sequences;

               this->character_class.get_prawdata(sequences, eastate.char_num);
               pos.split_seqs_and_ranges(sequences, cvars.is_icase(), cvars.is_back());

               eastate.quantifier.set(pos.length.first, pos.length.second);

               if (eastate.flags)   //  is_not.
                  return this->set_error(regex_constants::error_complement);
            }
            return true;
         }

         default:
            goto CHARACTER_ESCAPE;
         }

         range_pairs predefclass(this->character_class.view(eastate.char_num));

         if (eastate.flags)   //  is_not.
            predefclass.negation();

         pos.ranges.merge(predefclass);

         eastate.flags = 0u;
         eastate.type = st_character_class;
         return true;
      }

      CHARACTER_ESCAPE:

      switch (eastate.char_num)
      {
      case char_alnum::ch_t:
         eastate.char_num = char_ctrl::cc_htab; //  '\t' 0x09:HT
         break;

      case char_alnum::ch_n:
         eastate.char_num = char_ctrl::cc_nl;   //  '\n' 0x0a:LF
         break;

      case char_alnum::ch_v:
         eastate.char_num = char_ctrl::cc_vtab; //  '\v' 0x0b:VT
         break;

      case char_alnum::ch_f:
         eastate.char_num = char_ctrl::cc_ff;   //  '\f' 0x0c:FF
         break;

      case char_alnum::ch_r:
         eastate.char_num = char_ctrl::cc_cr;   //  '\r' 0x0d:CR
         break;

      case char_alnum::ch_c:  //  \cX
         if (curpos != end)
         {
            eastate.char_num = ui_l32(*curpos | masks::asc_icase);

            if (eastate.char_num >= char_alnum::ch_a and eastate.char_num <= char_alnum::ch_z)
            {
               eastate.char_num = ui_l32(*curpos++ & 0x1f);
               break;
            }
         }
         return this->set_error(regex_constants::error_escape);

      case char_alnum::ch_0:
         eastate.char_num = char_ctrl::cc_nul;  //  '\0' 0x00:NUL
         if (curpos != end and *curpos >= char_alnum::ch_0 and *curpos <= char_alnum::ch_9)
            return this->set_error(regex_constants::error_escape);
         break;

      case char_alnum::ch_x:  //  \xhh
         eastate.char_num = translate_numbers(curpos, end, 16, 2, 2, 0xff);
         break;

      case char_alnum::ch_u:  //  \uhhhh, \u{h~hhhhhh}
         eastate.char_num = parse_escape_u(curpos, end);
         break;

      //  SyntaxCharacter, and '/'.
      case meta_char::mc_caret:  //  '^'
      case meta_char::mc_dollar: //  '$'
      case meta_char::mc_escape: //  '\\'
      case meta_char::mc_period: //  '.'
      case meta_char::mc_astrsk: //  '*'
      case meta_char::mc_plus:   //  '+'
      case meta_char::mc_query:  //  '?'
      case meta_char::mc_rbraop: //  '('
      case meta_char::mc_rbracl: //  ')'
      case meta_char::mc_sbraop: //  '['
      case meta_char::mc_sbracl: //  ']'
      case meta_char::mc_cbraop: //  '{'
      case meta_char::mc_cbracl: //  '}'
      case meta_char::mc_bar:    //  '|'
      case char_other::co_slash: //  '/'
         break;

      default:
         eastate.char_num = constants::invalid_u32value;
      }

      if (eastate.char_num IS constants::invalid_u32value)
         return this->set_error(regex_constants::error_escape);

      return true;
   }

   ui_l32 parse_escape_u(const ui_l32 *&curpos, const ui_l32 *const end) const
   {
      ui_l32 ucp;

      if (curpos IS end)
         return constants::invalid_u32value;

      if (*curpos IS meta_char::mc_cbraop)
      {
         ucp = translate_numbers(++curpos, end, 16, 1, 0, constants::unicode_max_codepoint);

         if (curpos IS end or *curpos != meta_char::mc_cbracl)
            return constants::invalid_u32value;

         ++curpos;
      }
      else
      {
         ucp = translate_numbers(curpos, end, 16, 4, 4, 0xffff);

         if (ucp >= 0xd800 and ucp <= 0xdbff and (curpos + 6) <= end and *curpos IS meta_char::mc_escape and curpos[1] IS char_alnum::ch_u)
         {
            const ui_l32 *la = curpos + 2;
            const ui_l32 nextucp = translate_numbers(la, end, 16, 4, 4, 0xffff);

            if (nextucp >= 0xdc00 and nextucp <= 0xdfff)
            {
               curpos = la;
               ucp = ((ucp << 10) + nextucp) - 0x35fdc00;   //  - ((0xd800 << 10) + 0xdc00) + 0x10000.
            }
         }
      }
      return ucp;
   }


   bool get_property_name_or_value(pstring &name_or_value, const ui_l32 *&curpos, const ui_l32 *const end) const
   {
      bool number_found = false;

      name_or_value.clear();

      for (;; ++curpos) {
         if (curpos IS end) break;

         const ui_l32 curchar = *curpos;

         if (curchar >= char_alnum::ch_A and curchar <= char_alnum::ch_Z);
         else if (curchar >= char_alnum::ch_a and curchar <= char_alnum::ch_z);
         else if (curchar IS char_other::co_ll);   //  '_'
         else if (curchar >= char_alnum::ch_0 and curchar <= char_alnum::ch_9) number_found = true;
         else break;

         name_or_value.append(1, typename pstring::value_type(curchar));
      }

      //  A string containing a digit cannot be a property name.
      return number_found;
   }

   gname_string get_groupname(const ui_l32 *&curpos, const ui_l32 *const end, cvars_type &cvars) {
      charT mbstr[utf_traits::maxseqlen];
      gname_string groupname;

      cvars.idchecker.setup();
      for (;;) {
         if (curpos IS end) {
            groupname.clear();
            break;
         }

         ui_l32 curchar = *curpos++;

         if (curchar IS meta_char::mc_gt) //  '>'
            break;

         if (curchar IS meta_char::mc_escape and curpos != end and *curpos IS char_alnum::ch_u)   //  '\\', 'u'.
            curchar = parse_escape_u(++curpos, end);

         if (cvars.idchecker.is_identifier(curchar, groupname.size() != 0));  //  OK.
         else curchar = constants::invalid_u32value;

         if (curchar IS constants::invalid_u32value) {
            groupname.clear();
            break;
         }

         const ui_l32 seqlen = utf_traits::to_codeunits(mbstr, curchar);

         groupname.append(mbstr, seqlen);
      }

      if (groupname.size() IS 0)
         this->set_error(regex_constants::error_escape);

      return groupname;
   }

   void transform_seqdata(state_array &piece, const posdata_holder &pos, const cvars_type &cvars)
   {
      ui_l32 seqlen = ui_l32(pos.indices.size());
      state_type castate;

      castate.reset(st_character_class);
      castate.char_num = this->character_class.register_newclass(pos.ranges);

      if (seqlen > 0)
      {
         const bool has_empty = pos.has_empty();
         bool hooked = false;
         state_size_type prevbranch_end = 0;
         state_type branchstate;
         state_type jumpstate;
         state_array branch;

         branch.resize(seqlen);
         for (ui_l32 i = 0; i < seqlen; ++i)
            branch[i].reset();

         branchstate.reset(st_epsilon, epsilon_type::et_alt);

         jumpstate.reset(st_epsilon, epsilon_type::et_brnchend);  //  '/'

         for (--seqlen; seqlen >= 2; --seqlen)
         {
            ui_l32 offset = pos.indices[seqlen];
            const ui_l32 seqend = pos.indices[seqlen - 1];

            if (offset != seqend)
            {
               branch.shrink(seqlen + 1);
               branch[seqlen] = jumpstate;

               for (ui_l32 count = 0; offset < seqend; ++offset) {
                  const ui_l32 seqch = pos.seqs[offset];
                  state_type *const ost = &branch[count++];

                  ost->char_num = seqch & masks::pos_char;
                  this->NFA_states[0].flags |= ost->flags = (seqch & masks::pos_cf) ? sflags::icase : 0u;

                  if (count IS seqlen) {
                     state_size_type bpos = 0;

                     for (state_size_type ppos = 0; ppos < piece.size();) {
                        if (bpos + 1 IS branch.size()) {
                           piece.push_back_c(piece[ppos]);

                           state_type &pst = piece[ppos];

                           pst.reset(st_epsilon, epsilon_type::et_alt);
                           pst.next1 = std::ptrdiff_t(piece.size()) - ppos - 1;
                           pst.next2 = std::ptrdiff_t(prevbranch_end) - ppos;
                           pst.flags |= sflags::hooking;
                           hooked = true;

                           state_type &bst = piece[piece.size() - 1];

                           bst.next1 = bst.next1 - pst.next1;
                           bst.next2 = bst.next2 ? (bst.next2 - pst.next1) : 0;
                           bst.flags |= sflags::hookedlast;
                           goto SKIP_APPEND;
                        }

                        state_type &pst = piece[ppos];

                        if (pst.char_num IS branch[bpos].char_num) {
                           ++bpos;
                           ppos += pst.next1;
                        }
                        else if (pst.next2) ppos += pst.next2;
                        else {
                           pst.next2 = std::ptrdiff_t(piece.size()) - ppos;
                           break;
                        }
                     }

                     {
                        const state_size_type alen = branch.size() - bpos;

                        if (piece.size()) piece[prevbranch_end].next1 = piece.size() + alen - 1 - prevbranch_end;

                        piece.append(branch, bpos, alen);
                        prevbranch_end = piece.size() - 1;
                     }
                     SKIP_APPEND:
                     count = 0;

                  }
               }
            }
         }

         if (piece.size())
         {
            state_type &laststate = piece[prevbranch_end];

            laststate.next1 = piece.size() + (has_empty ? 2 : 1) - prevbranch_end;

            branchstate.next2 = std::ptrdiff_t(piece.size()) + 1;
            piece.insert(0, branchstate);
         }

         if (has_empty)
         {
            branchstate.next2 = 2;
            piece.push_back(branchstate);
         }

         piece.push_back(castate);

         branchstate.char_num = epsilon_type::et_ncgopen;
         branchstate.next1 = 1;
         branchstate.next2 = 0;
         branchstate.quantifier.set(1, 0);
         piece.insert(0, branchstate);

         branchstate.char_num = epsilon_type::et_ncgclose;
         branchstate.quantifier.atmost = 1;
         piece.push_back(branchstate);

         if (hooked)
            reorder_piece(piece);

         if ((this->soflags ^ cvars.soflags) & regex_constants::icase) {
            range_pairs charclass;

            if (cvars.is_icase()) {
               ui_l32 ucftable[ucf_constants::rev_maxset] = {};

               for (state_size_type i = 0; i < piece.size(); ++i) {
                  state_type &st = piece[i];

                  if (st.type IS st_character and (st.flags & sflags::icase)) {
                     const ui_l32 setnum = unicode_case_folding::do_caseunfolding(ucftable, st.char_num);

                     charclass.clear();
                     for (ui_l32 j = 0; j < setnum; ++j)
                        charclass.join(range_pair_helper(ucftable[j]));

                     st.char_num = this->character_class.register_newclass(charclass);
                     st.type = st_character_class;
                     st.flags = 0u; //  icase.
                  }
               }
            }
            else
            {
               charclass.resize(1);
               for (state_size_type i = 0; i < piece.size(); ++i)
               {
                  state_type &st = piece[i];

                  if (st.type IS st_character and unicode_case_folding::try_casefolding(st.char_num) != constants::invalid_u32value)
                  {
                     charclass[0] = range_pair_helper(st.char_num);

                     st.type = st_character_class;
                     st.char_num = this->character_class.register_newclass(charclass);
                  }
               }
            }
         }
      }
   }

   ui_l32 translate_numbers(const ui_l32 *&curpos, const ui_l32 *const end, const int radix, const std::size_t minsize, const std::size_t maxsize, const ui_l32 maxvalue) const
   {
      std::size_t count = 0;
      ui_l32 u32value = 0;
      ui_l32 num;

      for (; maxsize IS 0 or count < maxsize; ++curpos, ++count) {
         if (curpos IS end) break;

         const ui_l32 ch = *curpos;

         if ((ch >= char_alnum::ch_0 and ch <= char_alnum::ch_7) or (radix >= 10 and (ch IS char_alnum::ch_8 or ch IS char_alnum::ch_9)))
            num = ch - char_alnum::ch_0;
         else if (radix IS 16)
         {
            if (ch >= char_alnum::ch_A and ch <= char_alnum::ch_F)
               num = ch - char_alnum::ch_A + 10;
            else if (ch >= char_alnum::ch_a and ch <= char_alnum::ch_f)
               num = ch - char_alnum::ch_a + 10;
            else
               break;
         }
         else
            break;

         const ui_l32 nextvalue = u32value * radix + num;

         if ((/* maxvalue != 0 and */ nextvalue > maxvalue) or nextvalue < u32value) break;

         u32value = nextvalue;
      }

      if (count >= minsize) return u32value;

      return constants::invalid_u32value;
   }

   bool check_backreferences(cvars_type &cvars)
   {
      const state_size_type orgsize = this->NFA_states.size();
      simple_array<bool> gno_found;
      state_array additions;

      gno_found.resize(this->number_of_brackets, false);

      for (state_size_type backrefpos = 1; backrefpos < orgsize; ++backrefpos) {
         state_type &brs = this->NFA_states[backrefpos];

         if (brs.type IS st_roundbracket_close)
         {
            gno_found[brs.char_num] = true;
         }
         else if (brs.type IS st_backreference)
         {
            const ui_l32 &backrefno = brs.char_num;

            if (brs.flags & sflags::backrefno_unresolved)
            {
               if (backrefno > cvars.unresolved_gnames.size())
                  return this->set_error(regex_constants::error_backref);  //  Internal error.

               brs.flags &= ~sflags::backrefno_unresolved;

               const ui_l32 *list = this->namedcaptures[cvars.unresolved_gnames[backrefno]];

               if (list IS nullptr or *list < 1)
                  return this->set_error(regex_constants::error_backref);

               const ui_l32 num = list[0];
               state_type newbrs(brs);

               for (ui_l32 ino = 1; ino <= num; ++ino)
               {
                  if (gno_found[list[ino]])
                  {
                     newbrs.char_num = list[ino];
                     additions.push_back(newbrs);
                  }
               }

               if (additions.size() IS 0)
                  goto REMOVE_BACKREF;

               brs.char_num = additions[0].char_num;
               additions.erase(0);

               if (additions.size())
               {
                  const std::ptrdiff_t next1abs = std::ptrdiff_t(backrefpos + brs.next1);
                  const std::ptrdiff_t next2abs = std::ptrdiff_t(backrefpos + brs.next2);

                  brs.next1 = std::ptrdiff_t(this->NFA_states.size() - backrefpos);
                  brs.next2 = std::ptrdiff_t(this->NFA_states.size() - backrefpos);
                  brs.flags |= sflags::hooking;

                  const std::ptrdiff_t lastabs = std::ptrdiff_t(this->NFA_states.size() + additions.size() - 1);
                  state_type &laststate = additions.back();

                  laststate.flags |= sflags::hookedlast;
                  laststate.next1 = std::ptrdiff_t(next1abs - lastabs);
                  laststate.next2 = std::ptrdiff_t(next2abs - lastabs);
                  this->NFA_states.append(additions);
                  additions.clear();
               }
            }
            else
            {
               if (backrefno >= this->number_of_brackets)
                  return this->set_error(regex_constants::error_backref);

               if (!gno_found[backrefno])
               {
                  REMOVE_BACKREF:
                  if (brs.next1 IS -1)
                  {
                     state_type &prevstate = this->NFA_states[backrefpos + brs.next1];

                     if (prevstate.is_asterisk_or_plus_for_onelen_atom())
                     {
                        prevstate.next1 = 2;
                        prevstate.next2 = 0;
                        prevstate.char_num = epsilon_type::et_fmrbckrf;
                     }
                  }

                  brs.type = st_epsilon;
                  brs.next2 = 0;
                  brs.char_num = epsilon_type::et_fmrbckrf;
               }
            }
         }
      }
      if (orgsize != this->NFA_states.size())
         reorder_piece(this->NFA_states);

      return true;
   }


   void create_firstchar_class()
   {
      range_pairs fcc;

      const int canbe0length = gather_nextchars(fcc, state_size_type(this->NFA_states[0].next1), 0u, false);

      if (canbe0length)
      {
         fcc.set_solerange(range_pair_helper(0, constants::unicode_max_codepoint));
         //  Expressions would consist of assertions only, such as /^$/.
         //  We cannot but accept every codepoint.
      }

      this->NFA_states[0].quantifier.is_greedy = this->character_class.register_newclass(fcc);

                ui_l32 entrychar = constants::max_u32value;
#if defined(SRELL_HAS_SSE42)
      charT sranges[16];
      const int maxnum = sizeof (charT) ? (16 / sizeof (charT)) : 0;
      int curnum = 0;
#endif
      ui_l32 cu2 = 0;

      for (typename range_pairs::size_type i = 0; i < fcc.size(); ++i)
      {
         const range_pair &range = fcc[i];

         if (range.first > utf_traits::maxcpvalue)
            break;

         const ui_l32 maxr2 = range.second <= utf_traits::maxcpvalue ? range.second : utf_traits::maxcpvalue;
         ui_l32 r1 = range.first;

         for (; r1 <= maxr2;)
         {
            const ui_l32 prev2 = cu2;
            const ui_l32 cu1 = utf_traits::firstcodeunit(r1) & utf_traits::bitsetmask;
            ui_l32 r2 = utf_traits::nextlengthchange(r1) - 1;

            if (r2 > maxr2)
               r2 = maxr2;

            cu2 = utf_traits::firstcodeunit(r2) & utf_traits::bitsetmask;

            for (ui_l32 cu = cu1; cu <= cu2; ++cu)
               this->firstchar_class_bs.set(cu);
            if (entrychar != constants::invalid_u32value)
            {
               if (cu1 IS cu2 and (entrychar IS cu1 or entrychar IS constants::max_u32value))
                  entrychar = cu1;
               else
                  entrychar = constants::invalid_u32value;
            }
#if defined(SRELL_HAS_SSE42)
            if (curnum >= 0)
            {
               if (curnum > 0 and (prev2 IS cu1 or prev2 + 1 IS cu1))
               {
                  sranges[curnum - 1] = charT(cu2);
               }
               else if (curnum < maxnum)
               {
                  sranges[curnum++] = charT(cu1);
                  sranges[curnum++] = charT(cu2);
               }
               else
                  curnum = -1;
            }
#endif
      (void)prev2;
            if (r2 IS maxr2)
               break;

            r1 = r2 + 1;
         }
      }

#if defined(SRELL_HAS_SSE42)
      if (cpu_checker<int>::x86simd() and sizeof (charT) <= 2)
      {
         if (curnum > 0)
         {
            entrychar = masks::fcc_simd | curnum;
            std::memcpy(&this->simdranges, sranges, 16);
         }
      }
#endif

                this->NFA_states[0].char_num = entrychar;
   }

   int gather_nextchars(range_pairs &nextcharclass, state_size_type pos, simple_array<bool> &checked, const ui_l32 bracket_number, const bool subsequent) const
   {
      int canbe0length = 0;

      for (;;)
      {
         const state_type &state = this->NFA_states[pos];

         if (checked[pos])
            break;

         checked[pos] = true;

         if (state.next2
               and (state.type != st_increment_counter)
               and (state.type != st_save_and_reset_counter)
               and (state.type != st_roundbracket_open)
               and (state.type != st_roundbracket_close or state.char_num != bracket_number)
               and (state.type != st_repeat_in_push)
               and (state.type != st_backreference or (state.next1 != state.next2))
               and (state.type != st_lookaround_open))
         {
            const int c0l = gather_nextchars(nextcharclass, pos + state.next2, checked, bracket_number, subsequent);
            if (c0l)
               canbe0length = 1;
         }

         switch (state.type)
         {
         case st_character:
            if (!(state.flags & sflags::icase))
            {
               nextcharclass.join(range_pair_helper(state.char_num));
            }
            else
            {
               ui_l32 table[ucf_constants::rev_maxset] = {};
               const ui_l32 setnum = unicode_case_folding::do_caseunfolding(table, state.char_num);

               for (ui_l32 j = 0; j < setnum; ++j)
                  nextcharclass.join(range_pair_helper(table[j]));
            }
            return canbe0length;

         case st_character_class:
            nextcharclass.merge(this->character_class.view(state.char_num));
            return canbe0length;

         case st_backreference:
            {
               const state_size_type nextpos = find_next1_of_bracketopen(state.char_num);

               gather_nextchars(nextcharclass, nextpos, state.char_num, subsequent);
            }
            break;

         case st_eol:
         case st_bol:
         case st_boundary:
            if (subsequent)
               nextcharclass.set_solerange(range_pair_helper(0, constants::unicode_max_codepoint));

            break;

         case st_lookaround_open:
            if (!state.flags and state.quantifier.is_greedy IS 0)  //  !is_not.
            {
               gather_nextchars(nextcharclass, pos + 2, checked, 0u, subsequent);
            }
            else if (subsequent)
               nextcharclass.set_solerange(range_pair_helper(0, constants::unicode_max_codepoint));

            break;

         case st_roundbracket_close:
            if (/* bracket_number IS 0 or */ state.char_num != bracket_number)
               break;
            //@fallthrough@

         case st_success:  //  IS st_lookaround_close.
            return true;

         default:;
         }

         if (state.next1)
            pos += state.next1;
         else
            break;
      }
      return canbe0length;
   }

   int gather_nextchars(range_pairs &nextcharclass, const state_size_type pos, const ui_l32 bracket_number, const bool subsequent) const
   {
      simple_array<bool> checked;

      checked.resize(this->NFA_states.size(), false);
      return gather_nextchars(nextcharclass, pos, checked, bracket_number, subsequent);
   }

   state_size_type find_next1_of_bracketopen(const ui_l32 bracketno) const
   {
      for (state_size_type no = 0; no < this->NFA_states.size(); ++no)
      {
         const state_type &state = this->NFA_states[no];

         if (state.type IS st_roundbracket_open and state.char_num IS bracketno)
            return no + state.next1;
      }
      return 0;
   }

   void relativejump_to_absolutejump()
   {
      for (state_size_type pos = 0; pos < this->NFA_states.size(); ++pos)
      {
         state_type &state = this->NFA_states[pos];

         if (state.next1 or state.type IS st_character or state.type IS st_character_class)
            state.next_state1 = &this->NFA_states[pos + state.next1];
         else
            state.next_state1 = nullptr;

         if (state.next2)
            state.next_state2 = &this->NFA_states[pos + state.next2];
         else
            state.next_state2 = nullptr;
      }
   }

   void optimise(const cvars_type &cvars)
   {
      const bool needs_prefilter =
          !this->bmdata &&
          !(this->soflags & regex_constants::sticky);

      branch_optimisation2();

      if (needs_prefilter)
         find_better_es(1u, cvars);

      asterisk_optimisation();

      branch_optimisation();

      if (needs_prefilter)
         create_firstchar_class();

      skip_epsilon();

      set_charclass_posinfo(needs_prefilter);

      (void)needs_prefilter;
      (void)cvars;
   }


   void skip_epsilon()
   {
      for (state_size_type pos = 0; pos < this->NFA_states.size(); ++pos)
      {
         state_type &state = this->NFA_states[pos];

         if (state.next1)
            state.next1 = std::ptrdiff_t(skip_nonbranch_epsilon(pos + state.next1)) - pos;

         if (state.next2)
            state.next2 = std::ptrdiff_t(skip_nonbranch_epsilon(pos + state.next2)) - pos;
      }
   }

   state_size_type skip_nonbranch_epsilon(state_size_type pos) const
   {
      for (;;)
      {
         const state_type &state = this->NFA_states[pos];

         if (state.type IS st_epsilon and state.next2 IS 0)
         {
            pos += state.next1;
            continue;
         }
         break;
      }
      return pos;
   }



   void asterisk_optimisation()
   {
      const state_size_type orgsize = this->NFA_states.size();
      range_pairs removed;
      range_pairs curcc;
      range_pairs nextcc;
      state_array additions;

      for (state_size_type pos = 1u; pos < orgsize; ++pos)
      {
         state_type &curstate = this->NFA_states[pos];

         if ((curstate.type IS st_character or curstate.type IS st_character_class) and !curstate.quantifier.is_same())
         {
            const state_size_type bpos = pos + (curstate.next1 < 0 ? curstate.next1 : (curstate.quantifier.is_question() ? -1 : 0));

            if (bpos IS pos)
               continue;

            state_type &bstate = this->NFA_states[bpos];
            const state_size_type nextno = bpos + bstate.farnext();
            const re_quantifier &bq = bstate.quantifier;
            state_type orgcur(curstate);

            if (curstate.type IS st_character)
            {
               curcc.set_solerange(range_pair_helper(curstate.char_num));
               if (curstate.flags & sflags::icase)
                  curcc.make_caseunfoldedcharset();
            }
            else
            {
               this->character_class.copy_to(curcc, curstate.char_num);
               if (curcc.size() IS 0)  //  Means [], which always makes matching fail.
                  goto IS_EXCLUSIVE;   //  For preventing the automaton from pushing bt data.
            }

            additions.clear();

            {
               nextcc.clear();
               const int canbe0length = gather_nextchars(nextcc, nextno, 0u, true);

               if (nextcc.size())
               {
                  if (!canbe0length or bq.is_greedy)
                  {
                     curcc.split_ranges(removed, nextcc);

                     range_pairs &kept = curcc;

                     if (removed.size() IS 0)   //  !curcc.is_overlap(nextcc)
                        goto IS_EXCLUSIVE;

                     if (curstate.type IS st_character_class and kept.size())
                     {
                        curstate.char_num = this->character_class.register_newclass(kept);
                        curstate.flags |= (sflags::hooking | sflags::byn2);
                        curstate.next2 = std::ptrdiff_t(this->NFA_states.size()) - pos;

                        additions.resize(2);
                        state_type &n0 = additions[0];
                        state_type &n1 = additions[1];

                        n0.reset(st_epsilon, epsilon_type::et_ccastrsk);
                        n0.quantifier = bq;
                        n0.next2 = std::ptrdiff_t(nextno) - this->NFA_states.size();
                        if (!n0.quantifier.is_greedy)
                        {
                           n0.next1 = n0.next2;
                           n0.next2 = 1;
                        }

                        n1.reset(st_character_class, this->character_class.register_newclass(removed));
                        n1.next1 = std::ptrdiff_t(bq.is_infinity() ? pos : (pos + curstate.next1)) - this->NFA_states.size() - 1;
//                      n1.next2 = 0;
                        n1.flags |= sflags::hookedlast;
                        goto IS_EXCLUSIVE;
                     }

                  }
               }
               else  //  nextcc.size() IS 0
               if (!canbe0length or bq.is_greedy)
                  goto IS_EXCLUSIVE;

               continue;
            }
            IS_EXCLUSIVE:

            if (bstate.type != st_check_counter)
            {
               bstate.next1 = 1;
               bstate.next2 = 0;
               bstate.char_num = epsilon_type::et_aofmrast;

               if (curstate.next1 < 0)
                  curstate.next1 = 0;
            }
            else
            {
               if (bstate.quantifier.atleast != 0)
               {
                  const std::ptrdiff_t addpos = std::ptrdiff_t(this->NFA_states.size()) + additions.size();
                  const state_size_type srpos = bpos - 2;
                  const state_size_type rcpos = bpos - 1;
                  state_type &srstate = this->NFA_states[srpos];
                  state_type &rcstate = this->NFA_states[rcpos];

                  if (bstate.quantifier.atleast <= 4)
                  {
                     orgcur.next1 = 1;
                     orgcur.next2 = 0;
                     orgcur.quantifier.reset();
                     additions.append(bstate.quantifier.atleast, orgcur);

                     orgcur.flags |= sflags::hooking;
                     orgcur.next1 = addpos - srpos;

                     const std::ptrdiff_t movedsrpos = addpos + bstate.quantifier.atleast - 1;

                     srstate.next1 = std::ptrdiff_t(bpos) - movedsrpos;
                     srstate.next2 = std::ptrdiff_t(rcpos) - movedsrpos;
                     srstate.flags |= sflags::hookedlast;
                     additions.back() = srstate;

                     srstate = orgcur;

                     bstate.quantifier.atmost -= bstate.quantifier.atleast;
                  }
                  else
                  {
                     additions.append(this->NFA_states, bpos, 4);

                     srstate.next1 = addpos - srpos;

                     rcstate.flags |= (sflags::hooking | sflags::byn2 | sflags::clrn2);
                     rcstate.next2 = addpos - rcpos;

                     state_type &flcc = additions[additions.size() - 4];

                     (flcc.quantifier.is_greedy ? flcc.next2 : flcc.next1) = std::ptrdiff_t(bpos) - addpos;
                     flcc.quantifier.atmost = flcc.quantifier.atleast;

                     orgcur.flags |= sflags::hookedlast;
                     orgcur.quantifier.atmost = orgcur.quantifier.atleast;
                     additions.back() = orgcur;
                  }
               }
               bstate.quantifier.atleast = bstate.quantifier.atmost;

               curstate.quantifier.atmost -= curstate.quantifier.atleast;
               curstate.quantifier.atleast = 0;
            }

            if (curstate.next2 IS 0)
               curstate.next2 = std::ptrdiff_t(nextno) - pos;

            this->NFA_states.append(additions);
         }
      }
      if (orgsize != this->NFA_states.size())
         reorder_piece(this->NFA_states);
   }


   void reorder_piece(state_array &piece) const
   {
      u32array newpos;
      ui_l32 offset = 0;

      newpos.resize(piece.size() + 1, 0);
      newpos[piece.size()] = ui_l32(piece.size());

      for (ui_l32 indx = 0; indx < piece.size(); ++indx)
      {
         if (newpos[indx] IS 0)
         {
            newpos[indx] = indx + offset;

            state_type &st = piece[indx];

            if (st.flags & sflags::hooking)
            {
               const std::ptrdiff_t next1or2 = (st.flags & sflags::byn2) ? (st.flags ^= sflags::byn2, st.next2) : st.next1;
               st.flags ^= sflags::hooking;

               if (st.flags & sflags::clrn2)
                  st.flags ^= sflags::clrn2, st.next2 = 0;

               for (ui_l32 i = ui_l32(indx + next1or2); i < piece.size(); ++i)
               {
                  ++offset;
                  newpos[i] = indx + offset;
                  if (piece[i].flags & sflags::hookedlast)
                  {
                     piece[i].flags ^= sflags::hookedlast;
                     break;
                  }
               }
            }
         }
         else
            --offset;
      }

      state_array newpiece(piece.size());

      for (state_size_type indx = 0; indx < piece.size(); ++indx)
      {
         state_type &st = piece[indx];

         if (st.next1 != 0)
            st.next1 = std::ptrdiff_t(newpos[indx + st.next1]) - newpos[indx];

         if (st.next2 != 0)
            st.next2 = std::ptrdiff_t(newpos[indx + st.next2]) - newpos[indx];

         newpiece[newpos[indx]] = piece[indx];
      }
      newpiece.swap(piece);
   }

   bool check_if_backref_used(state_size_type pos, const ui_l32 number) const
   {
      for (; pos < this->NFA_states.size(); ++pos)
      {
         const state_type &state = this->NFA_states[pos];

         if (state.type IS st_backreference and state.char_num IS number)
            return true;
      }
      return false;
   }


   state_size_type gather_if_char_or_charclass(range_pairs &charclass, state_size_type pos) const
   {
      for (;;)
      {
         const state_type &cst = this->NFA_states[pos];

         if (cst.next2 != 0)
            break;

         if (cst.type IS st_character)
         {
            charclass.set_solerange(range_pair_helper(cst.char_num));
            if (cst.flags & sflags::icase)
               charclass.make_caseunfoldedcharset();
            return pos;
         }
         else if (cst.type IS st_character_class)
         {
            this->character_class.copy_to(charclass, cst.char_num);
            return pos;
         }
         else if (cst.type IS st_epsilon and cst.char_num != epsilon_type::et_jmpinlp)
         {
            pos += cst.next1;
         }
         else
            break;
      }
      return 0;
   }

   void branch_optimisation()
   {
      range_pairs nextcharclass1;

      for (state_size_type pos = 1; pos < this->NFA_states.size(); ++pos)
      {
         const state_type &state = this->NFA_states[pos];

         if (state.is_alt())
         {
            const state_size_type nextcharpos = gather_if_char_or_charclass(nextcharclass1, pos + state.next1);

            if (nextcharpos)
            {
               range_pairs nextcharclass2;
               const int canbe0length = gather_nextchars(nextcharclass2, pos + state.next2, 0u /* bracket_number */, true);

               if (!canbe0length and !nextcharclass1.is_overlap(nextcharclass2))
               {
                  state_type &branch = this->NFA_states[pos];
                  state_type &next1 = this->NFA_states[nextcharpos];

                  next1.next2 = pos + branch.next2 - nextcharpos;
                  branch.next2 = 0;
                  branch.char_num = epsilon_type::et_bo1fmrbr;
               }
            }
         }
      }
   }

   void setup_bmhdata()
   {
      u32array u32s;

      for (state_size_type i = 1; i < this->NFA_states.size(); ++i)
      {
         const state_type &state = this->NFA_states[i];

         if (state.type != st_character)
            return;

         u32s.push_back_c(state.char_num);
      }

      if (u32s.size() > 1)
      {
         this->bmdata = new bmh_type;
         this->bmdata->setup(u32s, this->is_ricase());
      }
   }

   void set_charclass_posinfo(const bool has_fcc)
   {
      this->character_class.finalise();

      for (state_size_type i = 1; i < this->NFA_states.size(); ++i)
      {
         state_type &state = this->NFA_states[i];

         if (state.type IS st_character_class or state.type IS st_bol or state.type IS st_eol or state.type IS st_boundary)
         {
            const range_pair &posinfo = this->character_class.charclasspos(state.char_num);
            state.quantifier.set(posinfo.first, posinfo.second);
         }
      }

      if (has_fcc)
      {
         const range_pair &posinfo = this->character_class.charclasspos(this->NFA_states[0].quantifier.is_greedy);
         this->NFA_states[0].quantifier.set(posinfo.first, posinfo.second);
      }
   }


   void branch_optimisation2()
   {
      bool hooked = false;
      range_pairs basealt1stch;
      range_pairs nextalt1stch;

      for (state_size_type pos = 1; pos < this->NFA_states.size(); ++pos)
      {
         const state_type &curstate = this->NFA_states[pos];

         if (curstate.is_alt())
         {
            state_size_type precharchainpos = pos;
            const state_size_type n1pos = gather_if_char_or_charclass(basealt1stch, pos + curstate.next1);

            if (n1pos != 0)
            {
               state_type &n1ref = this->NFA_states[n1pos];
               state_size_type n2pos = precharchainpos + curstate.next2;
               state_size_type postcharchainpos = 0;

               for (;;)
               {
                  state_type &n2ref = this->NFA_states[n2pos];
                  const bool n2isalt = n2ref.is_alt();
                  const state_size_type next2next1poso = n2pos + (n2isalt ? n2ref.next1 : 0);
                  const state_size_type next2next2pos = n2isalt ? n2pos + n2ref.next2 : 0;
                  const state_size_type next2next1pos = gather_if_char_or_charclass(nextalt1stch, next2next1poso);

                  if (next2next1pos != 0)
                  {
                     const int relation = basealt1stch.relationship(nextalt1stch);

                     if (relation IS 0)
                     {
                        state_type &prechainalt = this->NFA_states[precharchainpos];
                        state_type &becomes_unused = this->NFA_states[next2next1pos];
                        const state_size_type next1next1pos = n1pos + n1ref.next1;

                        becomes_unused.type = st_epsilon;

                        if (next2next2pos)
                        {
                           becomes_unused.char_num = epsilon_type::et_bo2fmrbr;  //  '2'

                           if (postcharchainpos IS 0)
                           {
                              n2ref.next1 = next1next1pos - n2pos;
                              n2ref.next2 = next2next1pos - n2pos;

                              n1ref.next1 = n2pos - n1pos;
                              n1ref.flags |= sflags::hooking;
                              n2ref.flags |= sflags::hookedlast;
                              hooked = true;
                           }
                           else
                           {
                              state_type &becomes_alt = this->NFA_states[postcharchainpos];

                              becomes_alt.char_num = epsilon_type::et_alt; //  '|' <- '2'
                              becomes_alt.next2 = next2next1pos - postcharchainpos;

                              n2ref.next2 = 0;
                              n2ref.char_num = epsilon_type::et_bo2skpd;   //  '!'
                           }
                           postcharchainpos = next2next1pos;
                           prechainalt.next2 = next2next2pos - precharchainpos;
                        }
                        else
                        {
                           if (postcharchainpos IS 0)
                           {
                              becomes_unused.char_num = epsilon_type::et_alt; //  '|'
                              becomes_unused.next2 = becomes_unused.next1;
                              becomes_unused.next1 = next1next1pos - next2next1pos;

                              n1ref.next1 = next2next1pos - n1pos;
                              n1ref.flags |= sflags::hooking;
                              becomes_unused.flags |= sflags::hookedlast;
                              hooked = true;
                           }
                           else
                           {
                              state_type &becomes_alt = this->NFA_states[postcharchainpos];

                              becomes_alt.char_num = epsilon_type::et_alt; //  '|' <- '2'
                              becomes_alt.next2 = next2next1pos + becomes_unused.next1 - postcharchainpos;

                              becomes_unused.char_num = epsilon_type::et_bo2skpd;   //  '!'
                           }
                           prechainalt.next2 = 0;
                           prechainalt.char_num = epsilon_type::et_bo2fmrbr;  //  '2'
                        }
                     }
                     else if (relation IS 1)
                     {
                        break;
                     }
                     else
                        precharchainpos = n2pos;
                  }
                  else
                  {
                     //  Fix for bug210428.
                     //  Original: /mm2|m|mm/
                     //  1st step: /m(?:m2||m)/ <- No more optimisation can be performed. Must quit.
                     //  2nd step: /mm(?:2||)/ <- BUG.
                     break;
                  }

                  if (next2next2pos IS 0)
                     break;

                  n2pos = next2next2pos;
               }
            }
         }
      }

      if (hooked)
         reorder_piece(this->NFA_states);
   }


   bool has_obstacle_to_reverse(state_size_type pos, const state_size_type end, const bool check_optseq) const
   {
      for (; pos < end;)
      {
         const state_type &s = this->NFA_states[pos];

         if (s.type IS st_epsilon)
         {
            if (s.char_num IS epsilon_type::et_alt)
               return true;
               //  The rewinder cannot support Alternatives because forward matching
               //  and backward matching can go through different routes:
               //  * In a forward search /(?:.|ab)c/ against "abc" matches "abc",
               //    while in a backward search from 'c' it matches "bc".

            //  Because of the same reason, the rewinder cannot support an optional
            //  group either. Semantically, /(\d+-)?\d{1,2}-\d{1,2}/ is equivalent to
            //  /(\d+-|)\d{1,2}-\d{1,2}/.
            if (check_optseq)
            {
               if (s.char_num IS epsilon_type::et_jmpinlp)
               {
                  pos += s.next1;
                  continue;
               }

               if (s.char_num IS epsilon_type::et_dfastrsk and !this->NFA_states[pos + s.nearnext()].is_character_or_class())
                  return true;
            }

         }
         else if (s.type IS st_backreference)
            return true;
         else if (s.type IS st_lookaround_open)
            return true;
         else if (check_optseq and s.type IS st_check_counter)
         {
            if (s.quantifier.atleast IS 0 and !this->NFA_states[pos + 3].is_character_or_class())
               return true;
            pos += 3;
            continue;
         }
         ++pos;
      }
      return false;
   }

   state_size_type skip_bracket(const ui_l32 no, const state_array &NFAs, state_size_type pos) const
   {
      return find_pair(st_roundbracket_close, NFAs, no, pos);
   }

   state_size_type skip_0width_checker(const ui_l32 no, const state_array &NFAs, state_size_type pos) const
   {
      return find_pair(st_check_0_width_repeat, NFAs, no, pos);
   }

   state_size_type find_pair(const re_state_type type, const state_array &NFAs, const ui_l32 no, state_size_type pos) const
   {
      for (++pos; pos < NFAs.size(); ++pos)
      {
         const state_type &s = NFAs[pos];

         if (s.type IS type and s.char_num IS no)
            return pos;
      }
      return 0u;
   }

   state_size_type skip_group(const state_array &NFAs, state_size_type pos) const
   {
      ui_l32 depth = 1u;

      for (++pos; pos < NFAs.size(); ++pos)
      {
         const state_type &s = NFAs[pos];

         if (s.type IS st_epsilon)
         {
            if (s.char_num IS epsilon_type::et_ncgopen)
               ++depth;
            else if (s.char_num IS epsilon_type::et_ncgclose)
            {
               if (--depth IS 0u)
                  return pos;
            }
         }
      }
      return 0u;
   }

   int create_rewinder(const state_size_type end, const int needs_rerun, const cvars_type &cvars)
   {
      state_array newNFAs;
      state_type rwstate;

      {
         const int res = reverse_atoms(newNFAs, this->NFA_states, 1u, end, cvars);

         if (res < 1)
            return res;
      }
      if (newNFAs.size() IS 0u)
         return 0;

      rwstate.reset(st_lookaround_pop, meta_char::mc_eq);
      rwstate.quantifier.atmost = 0;
      newNFAs.insert(0, rwstate);

      rwstate.type = st_lookaround_open;
      rwstate.next1 = std::ptrdiff_t(end + newNFAs.size() + 2) - 1;
      rwstate.next2 = 1;
      rwstate.quantifier.is_greedy = needs_rerun ? 3 : 2; //  Match point rewinder.
         //  "singing" problem: /\w+ing/ against "singing" matches the
         //  entire "singing". However, if modified like /(?<=\K\w+)ing/
         //  it matches "sing" only, which is not correct (but correct if
         //  /\w+?ing/). To avoid this problem, after rewinding is
         //  finished rerunning the automaton forwards from the found
         //  point is needed if the reversed states contain a variable
         //  length (non fixed length) atom.
         //  TODO: This rerunning can be avoided if the reversed atoms
         //  are an exclusive sequence, like /\d+[:,]+\d+abcd/.
      newNFAs.insert(0, rwstate);

      rwstate.type = st_lookaround_close;
      rwstate.next1 = 0;
      rwstate.next2 = 0;
      newNFAs.append(1, rwstate);

      this->NFA_states.insert(1, newNFAs);
      this->NFA_states[0].next2 = std::ptrdiff_t(newNFAs.size()) + 1;

      return 1;
   }

   int reverse_atoms(state_array &revNFAs, const state_array &NFAs, state_size_type cur, const state_size_type send, const cvars_type &cvars)
   {
      const state_size_type orglen = send - cur;
      state_array atomseq;
      state_array revgrp;
      state_type epsilon;

      epsilon.reset(st_epsilon, epsilon_type::et_rvfmrcg);

      revNFAs.clear();

      for (; cur < send;)
      {
         const state_type &state = NFAs[cur];

         switch (state.type)
         {
         case st_epsilon:
            if (state.is_ncgroup_open_or_close())
            {
               revNFAs.insert(0, epsilon);
               ++cur;
               continue;
            }
            break;

         case st_roundbracket_open:
            atomseq.clear();
            atomseq.push_back(epsilon);
            atomseq.push_back(epsilon);
            revNFAs.insert(0, atomseq);
            cur += 2;
            continue;

         case st_roundbracket_close:
            revNFAs.insert(0, epsilon);
            cur += 1;
            continue;

         default:;
         }

         const state_size_type boundary = find_atom_boundary(NFAs, cur, send, false);

         if (boundary IS 0u or cur IS boundary)
            return 0;

         atomseq.clear();
         atomseq.append(NFAs, cur, boundary - cur);

         for (state_size_type pos = 0; pos < atomseq.size(); ++pos)
         {
            state_type &s = atomseq[pos];

            switch (s.type)
            {
            case st_roundbracket_open:
               if (!cvars.backref_used or !check_if_backref_used(pos + 1, s.char_num))
               {
                  const state_size_type rbend = skip_bracket(s.char_num, atomseq, pos);

                  if (rbend != 0u)
                  {
                     pos += 2;

                     {
                        const int res = reverse_atoms(revgrp, atomseq, pos, rbend, cvars);
                        if (res < 1)
                           return res;
                     }
                     {
                        if (s.quantifier.is_greedy)
                        {
                           atomseq[pos - 2].reset(st_epsilon, epsilon_type::et_mfrfmrcg);
                           atomseq[pos - 1].reset(st_epsilon, epsilon_type::et_mfrfmrcg);
                           atomseq[rbend].type = st_epsilon;
                           atomseq[rbend].char_num = epsilon_type::et_mfrfmrcg;
                           atomseq[rbend].next2 = 0;
                        }
                        else
                        {
                           state_type &bro = atomseq[pos - 2];
                           state_type &brp = atomseq[pos - 1];
                           state_type &brc = atomseq[rbend];

                           bro.type = st_repeat_in_push;
                           brp.type = st_repeat_in_pop;
                           brc.type = st_check_0_width_repeat;

                           if (this->number_of_repeats > constants::max_u32value)
                              return false;  //  Treats as a failure of optimisation, not an error.

                           bro.char_num = this->number_of_repeats;
                           brp.char_num = this->number_of_repeats;
                           brc.char_num = this->number_of_repeats;
                           ++this->number_of_repeats;
                        }
                        atomseq.replace(pos, rbend - pos, revgrp);
                        pos = rbend;
                        continue;
                     }
                  }
               }
               return 0;

            case st_epsilon:
               if (s.char_num IS epsilon_type::et_ncgopen)
               {
                  state_size_type grend = skip_group(atomseq, pos);

                  if (grend != 0u)
                  {
                     ++pos;

                     {
                        const int res = reverse_atoms(revgrp, atomseq, pos, grend, cvars);
                        if (res < 1)
                           return res;
                     }
                     {
                        atomseq.replace(pos, grend - pos, revgrp);
                        pos = grend;
                        continue;
                     }
                  }
                  return 0;
               }
               else if ((s.char_num IS epsilon_type::et_ccastrsk or s.char_num IS epsilon_type::et_dfastrsk)
                  and s.next2 != 0 and !s.quantifier.is_greedy)
               {
                  s.next2 = s.next1;
                  s.next1 = 1;
                  s.quantifier.is_greedy = 1u;
               }
               continue;

            case st_check_counter:
               if (pos + 3 < atomseq.size())
               {
                  if (!s.quantifier.is_greedy)
                  {
                     s.next2 = s.next1;
                     s.next1 = 1;
                     s.quantifier.is_greedy = 1u;
                  }
                  continue;
               }
               return 0;

            default:;
            }
         }

         cur = boundary;
         revNFAs.insert(0, atomseq);
      }
      return revNFAs.size() IS orglen ? 1 : 0;
   }

   state_size_type find_atom_boundary(const state_array &NFAs, state_size_type cur, const state_size_type end, const bool separate) const
   {
      const state_size_type begin = cur;
      state_size_type charatomseq_endpos = cur;
      const state_type *charatomseq_begin = nullptr;

      for (; cur < end;)
      {
         const state_type &cst = NFAs[cur];

         switch (cst.type)
         {
         case st_character:
         case st_character_class:
            if (charatomseq_begin IS nullptr)
               charatomseq_begin = &cst;
            else if (separate or !charatomseq_begin->is_same_character_or_charclass(cst))
               return charatomseq_endpos;

            charatomseq_endpos = ++cur;
            continue;

         case st_epsilon:
            if (cst.next2 IS 0)
            {
               if (charatomseq_begin)
                  return charatomseq_endpos;

               if (cst.char_num IS epsilon_type::et_jmpinlp)
               {
                  ++cur;
                  continue;
               }
               else if (cst.char_num IS epsilon_type::et_ncgopen)
               {
                  const state_size_type gend = skip_group(NFAs, cur);

                  return gend != 0u ? gend + 1 : 0u;
               }
               else if (cst.char_num != epsilon_type::et_brnchend)
                  return cur + 1;

               return 0u;
            }

            if (cst.char_num IS epsilon_type::et_ccastrsk)
            {
               if (cur + 1 < end)
               {
                  const state_type &repatom = NFAs[cur + 1];

                  if (charatomseq_begin IS nullptr)
                     charatomseq_begin = &repatom;
                  else if (separate or !charatomseq_begin->is_same_character_or_charclass(repatom))
                     return charatomseq_endpos;

                  return cur + cst.farnext();
               }
               return 0u;
            }
            else if (cst.char_num IS epsilon_type::et_alt)  //  '|'
            {
               if (charatomseq_begin)
                  return charatomseq_endpos;

               state_size_type altend = cur + cst.next2 - 1;

               for (; NFAs[altend].type IS st_epsilon and NFAs[altend].char_num IS epsilon_type::et_brnchend; altend += NFAs[altend].next1);

               return altend;
            }

            if (cst.char_num IS epsilon_type::et_dfastrsk)
               return charatomseq_begin ? charatomseq_endpos : cur + cst.farnext();

            return 0u;

         case st_save_and_reset_counter:
            cur += cst.next1;
            //@fallthrough@

         case st_check_counter:
            {
               const state_type &ccstate = NFAs[cur];
               const state_type &repatom = NFAs[cur + 3];

               if (charatomseq_begin)
               {
                  if (separate or !charatomseq_begin->is_same_character_or_charclass(repatom))
                     return charatomseq_endpos;
               }
               else if (repatom.is_character_or_class())
                  charatomseq_begin = &repatom;
               else
                  return cur + ccstate.farnext();

               charatomseq_endpos = cur += ccstate.farnext();
            }
            continue;

         case st_bol:
         case st_eol:
         case st_boundary:
         case st_backreference:
            if (charatomseq_begin)
               return charatomseq_endpos;
            return cur + 1;

         case st_roundbracket_open:
            if (charatomseq_begin)
               return charatomseq_endpos;
            else
            {
               const state_size_type rbend = skip_bracket(cst.char_num, NFAs, cur);

               if (rbend != 0u)
                  return rbend + 1;
            }
            return 0u;

         case st_repeat_in_push:
            if (charatomseq_begin)
               return charatomseq_endpos;
            else
            {
               const state_size_type rpend = skip_0width_checker(cst.char_num, NFAs, cur);

               if (rpend != 0u)
                  return rpend + 1;
            }
            return 0u;

         case st_lookaround_open:
            if (charatomseq_begin)
               return charatomseq_endpos;
            return cur + cst.next1;

         case st_roundbracket_close:
         case st_check_0_width_repeat:
         case st_lookaround_close:
            return charatomseq_endpos;

         //  restore_counter, increment_counter, decrement_counter,
         //  roundbracket_pop, repeat_in_pop, lookaround_pop.
         default:
            return 0u;
         }
      }
      return begin != charatomseq_endpos ? charatomseq_endpos : 0u;
   }

   int find_better_es(state_size_type cur, const cvars_type &cvars)
   {
      const state_array &NFAs = this->NFA_states;
      state_size_type betterpos = 0u;
      ui_l32 bp_cunum = constants::infinity;
      ui_l32 charcount = 0u;
      int needs_rerun = 0;
      int next_nr = 0;
      range_pairs nextcc;

      for (; cur < NFAs.size();)
      {
         const state_type &state = NFAs[cur];

         if (state.type IS st_epsilon)
         {
            if (state.next2 IS 0 and state.char_num != epsilon_type::et_jmpinlp)
            {
               ++cur;
               continue;
            }
         }
         else if (state.type IS st_roundbracket_open)
         {
            cur += state.next1;
            next_nr = 1;
            continue;
         }
         else if (state.type IS st_bol or state.type IS st_eol or state.type IS st_boundary)
         {
            cur += state.next1;
            continue;
         }
         else if (state.type IS st_roundbracket_close)
         {
            cur += state.next2;
            continue;
         }
         else if (state.type IS st_backreference or state.type IS st_lookaround_open)
            break;

         const state_size_type boundary = find_atom_boundary(NFAs, cur, NFAs.size(), true);

         if (boundary IS 0u or cur IS boundary)
            break;

         nextcc.clear();
         const int canbe0length = gather_nextchars(nextcc, cur, 0u, false);

         if (canbe0length)
            break;

         const ui_l32 cunum = nextcc.num_codeunits<utf_traits>();
         const bool has_obstacle = has_obstacle_to_reverse(cur, boundary, true);

         if (bp_cunum >= cunum)
         {
            betterpos = cur;
            bp_cunum = cunum;
            ++charcount;
            needs_rerun |= next_nr;
         }

         if (has_obstacle)
            break;

         const state_size_type atomlen = boundary - cur;

         if ((atomlen != 1 or !state.is_character_or_class())
            and (atomlen != 6 or NFAs[cur + 2].type != st_check_counter or !NFAs[cur + 2].quantifier.is_same() or !NFAs[cur + 5].is_character_or_class()))
            next_nr = 1;

         cur = boundary;
      }

      return (charcount > 1u) ? create_rewinder(betterpos, needs_rerun, cvars) : 0;
   }


public:  //  For debug.

   void print_NFA_states(const int) const;
};
//  re_compiler

   }  //  namespace re_detail

//  ... "rei_compiler.hpp"]
//  ["regex_sub_match.hpp" ...

template <class BidirectionalIterator>
class sub_match : public std::pair<BidirectionalIterator, BidirectionalIterator>
{
public:

   typedef typename std::iterator_traits<BidirectionalIterator>::value_type value_type;
   typedef typename std::iterator_traits<BidirectionalIterator>::difference_type difference_type;
   typedef BidirectionalIterator iterator;
   typedef std::basic_string<value_type> string_type;

   bool matched;

// constexpr sub_match();  //  C++11.

   sub_match() : matched(false)
   {
   }

   difference_type length() const
   {
      return matched ? std::distance(this->first, this->second) : 0;
   }

   void swap(sub_match &s)
   {
      if (this != &s)
      {
         this->std::pair<BidirectionalIterator, BidirectionalIterator>::swap(s);
         std::swap(matched, s.matched);
      }
   }

   void set_(const typename re_detail::re_submatch_type<BidirectionalIterator> &br)
   {
      this->first = br.core.open_at;
      this->second = br.core.close_at;
      this->matched = br.counter.no != 0;
   }
};


//  ... "regex_sub_match.hpp"]
//  ["regex_match_results.hpp" ...

template <class BidirectionalIterator, class Allocator = std::allocator<sub_match<BidirectionalIterator> > >
class match_results
{
public:

   typedef sub_match<BidirectionalIterator> value_type;
   typedef const value_type & const_reference;
   typedef const_reference reference;
#if defined(SRELL_HAS_TYPE_TRAITS)
   typedef typename re_detail::container_type<value_type, Allocator, std::is_trivially_copyable<BidirectionalIterator>::value>::type sub_match_array;
#else
   typedef typename re_detail::container_type<BidirectionalIterator, value_type, Allocator>::type sub_match_array;
#endif
   typedef typename sub_match_array::const_iterator const_iterator;
   typedef const_iterator iterator;
   typedef typename std::iterator_traits<BidirectionalIterator>::difference_type difference_type;

#if defined(__cplusplus) && __cplusplus >= 201103L
   typedef typename std::allocator_traits<Allocator>::size_type size_type;
#else
   typedef typename Allocator::size_type size_type;   //  TR1.
#endif

   typedef Allocator allocator_type;
   typedef typename std::iterator_traits<BidirectionalIterator>::value_type char_type;
   typedef std::basic_string<char_type> string_type;
   typedef typename re_detail::concon_view<char_type> contiguous_container_view;

public:

   explicit match_results(const Allocator &a = Allocator()) : ready_(0u), sub_matches_(a)
   {
   }

   match_results(const match_results &m)
   {
      operator=(m);
   }

#if defined(__cpp_rvalue_references)
   match_results(match_results &&m) SRELL_NOEXCEPT
   {
      operator=(std::move(m));
   }
#endif

   match_results &operator=(const match_results &m)
   {
      if (this != &m)
      {
//       this->sstate_ = m.sstate_;
         this->ready_ = m.ready_;
         this->sub_matches_ = m.sub_matches_;
         this->prefix_ = m.prefix_;
         this->suffix_ = m.suffix_;
         this->base_ = m.base_;
         this->gnames_ = m.gnames_;
      }
      return *this;
   }

#if defined(__cpp_rvalue_references)
   match_results &operator=(match_results &&m) SRELL_NOEXCEPT
   {
      if (this != &m)
      {
//       this->sstate_ = std::move(m.sstate_);
         this->ready_ = m.ready_;
         this->sub_matches_ = std::move(m.sub_matches_);
         this->prefix_ = std::move(m.prefix_);
         this->suffix_ = std::move(m.suffix_);
         this->base_ = m.base_;
         this->gnames_ = std::move(m.gnames_);
      }
      return *this;
   }
#endif

// ~match_results();

   bool ready() const
   {
      return (ready_ & 1u) ? true : false;
   }

   size_type size() const
   {
      return sub_matches_.size();
   }

   const_reference operator[](const size_type n) const
   {
      return n < sub_matches_.size() ? sub_matches_[n] : unmatched_;
   }

   difference_type length(const size_type sub = 0) const
   {
      return (*this)[sub].length();
   }

   difference_type position(const size_type sub = 0) const
   {
      return std::distance(base_, (*this)[sub].first);
   }

public:  //  For internal.

   typedef match_results<BidirectionalIterator> match_results_type;
   typedef typename match_results_type::size_type match_results_size_type;
   typedef typename re_detail::re_search_state</*charT, */BidirectionalIterator> search_state_type;
   typedef typename re_detail::groupname_mapper<char_type> gnamemap_type;

   search_state_type sstate_;

   void clear_()
   {
      ready_ = 0u;
      sub_matches_.clear();
//    prefix_.matched = false;
//    suffix_.matched = false;
      gnames_.clear();
   }

// template <typename charT>
   bool set_match_results_(const re_detail::ui_l32 num_of_brackets, const gnamemap_type &gnames)
   {
      sub_matches_.resize(num_of_brackets);

      sub_matches_[0].matched = true;

      for (re_detail::ui_l32 i = 1; i < num_of_brackets; ++i)
         sub_matches_[i].set_(sstate_.bracket[i]);

      base_ = sstate_.lblim;
      prefix_.first = sstate_.srchbegin;
      prefix_.second = sub_matches_[0].first = sstate_.curbegin;
      suffix_.first = sub_matches_[0].second = sstate_.ssc.iter;
      suffix_.second = sstate_.srchend;

      prefix_.matched = prefix_.first != prefix_.second;
      suffix_.matched = suffix_.first != suffix_.second;

      gnames_ = gnames;
      ready_ = 1u;
      return true;
   }

   bool set_match_results_bmh_()
   {
      sub_matches_.resize(1);
//    value_type &m0 = sub_matches_[0];

      sub_matches_[0].matched = true;

      base_ = sstate_.lblim;
      prefix_.first = sstate_.srchbegin;
      prefix_.second = sub_matches_[0].first = sstate_.ssc.iter;
      suffix_.first = sub_matches_[0].second = sstate_.nextpos;
      suffix_.second = sstate_.srchend;

      prefix_.matched = prefix_.first != prefix_.second;
      suffix_.matched = suffix_.first != suffix_.second;

      ready_ = 1u;
      return true;
   }

   void set_prefix1_(const BidirectionalIterator pf)
   {
      prefix_.first = pf;
   }

   void update_prefix1_(const BidirectionalIterator pf)
   {
      prefix_.first = pf;
      prefix_.matched = prefix_.first != prefix_.second;
   }

   void update_prefix2_(const BidirectionalIterator ps)
   {
      prefix_.second = ps;
      prefix_.matched = prefix_.first != prefix_.second;
   }

   void update_m0_(const BidirectionalIterator mf, const BidirectionalIterator ms)
   {
      sub_matches_.resize(1);

      sub_matches_[0].first = mf;
      sub_matches_[0].second = ms;
      sub_matches_[0].matched = true;

      prefix_.first = prefix_.second = mf;
   }

   bool set_as_failed_(const re_detail::ui_l32 reason)
   {
      ready_ = reason ? (reason << 1) : 1u;
      return false;
   }


   typename gnamemap_type::gname_string lookup_gname_(const re_detail::ui_l32 gno) const
   {
      return gnames_[gno];
   }


private:


   re_detail::ui_l32 lookup_backref_number(const char_type *begin, const char_type *const end) const
   {
      const re_detail::ui_l32 *list = gnames_[typename gnamemap_type::view_type(begin, end - begin)];
      re_detail::ui_l32 gno = gnamemap_type::notfound;

      if (list)
      {
         const re_detail::ui_l32 num = list[0];

         for (re_detail::ui_l32 i = 1; i <= num; ++i)
         {
            gno = list[i];
            if (gno < re_detail::ui_l32(sub_matches_.size()) and sub_matches_[gno].matched)
               break;
         }
      }
      return gno;
   }


public:  //  For debug.

   template <typename BasicRegexT>
   void print_sub_matches(const BasicRegexT &, const int) const;
   void print_addresses(const value_type &, const char *const) const;

private:

   re_detail::ui_l32 ready_;
   sub_match_array sub_matches_;
   value_type prefix_;
   value_type suffix_;
   value_type unmatched_;
   BidirectionalIterator base_;

   gnamemap_type gnames_;
};


//  ... "regex_match_results.hpp"]


//  ["rei_algorithm.hpp" ...

   namespace re_detail
   {

struct is_cont_iter {};
struct non_cont_iter {};

template <typename charT, typename traits>
class re_object : public re_compiler<charT, traits>
{
public:

   template <typename BidirectionalIterator, typename Allocator>
   bool search
   (
      const BidirectionalIterator begin,
      const BidirectionalIterator end,
      const BidirectionalIterator lookbehind_limit,
      match_results<BidirectionalIterator, Allocator> &results,
      const regex_constants::match_flag_type flags
   ) const
   {
      ui_l32 reason = 0;

      results.clear_();

      if (this->NFA_states.size())
      {
         typedef typename contiguous_checker<BidirectionalIterator, 0>::itype ci_checker;
#if defined(SRELL_HAS_SSE42)
         typedef ci_checker maybe_cic;
#else
         typedef non_cont_iter maybe_cic;
#endif
         re_search_state<BidirectionalIterator> &sstate = results.sstate_;

         sstate.init(begin, end, lookbehind_limit, flags | regex_constants::match_flag_type(this->soflags & regex_constants::sticky));

         if (this->bmdata and !(sstate.flags & regex_constants::match_continuous))
         {
            if (!this->is_ricase() ? this->bmdata->do_casesensitivesearch(sstate, typename std::iterator_traits<BidirectionalIterator>::iterator_category()) : this->bmdata->do_icasesearch(sstate, typename std::iterator_traits<BidirectionalIterator>::iterator_category()))
               return results.set_match_results_bmh_();
         }
         else
         {
            sstate.init_for_automaton(this->number_of_brackets, this->number_of_counters, this->number_of_repeats);
            {
               if (sstate.flags & regex_constants::match_continuous)
               {
                  sstate.entry_state = this->NFA_states[0].next_state2;

                  sstate.ssc.iter = sstate.nextpos;

                  sstate.reset(this->limit_counter);
                  reason = !this->is_ricase() ? run_automaton<false, false>(sstate) : run_automaton<true, false>(sstate);
               }
               else
               {
                  sstate.entry_state = this->NFA_states[0].next_state1;

                  if (this->NFA_states[0].char_num <= utf_traits::maxcpvalue)
                  {
                     reason = !this->is_ricase() ? do_search_sc<false>(sstate, ci_checker()) : do_search_sc<true>(sstate, ci_checker());
                  }
                  else
                  {
                     reason = !this->is_ricase() ? do_search<false>(sstate, maybe_cic()) : do_search<true>(sstate, maybe_cic());
                  }
               }

               if (reason IS 1)
               {
                  return results.set_match_results_(this->number_of_brackets, this->namedcaptures);
               }
            }

         }
      }
      return results.set_as_failed_(reason);
   }

private:

   typedef typename traits::utf_traits utf_traits;

#if defined(SRELL_HAS_SSE42)

   template <const bool icase, typename ContiguousIterator>
   SRELL_AT_SSE42 ui_l32 do_search(re_search_state<ContiguousIterator> &sstate, const is_cont_iter) const
   {
      typedef typename std::iterator_traits<ContiguousIterator>::value_type char_type2;

SRELL_NO_VCWARNING(4127)
      if (sizeof (charT) IS sizeof (char_type2))
SRELL_NO_VCWARNING_END
      {
         const int numofranges = int(this->NFA_states[0].char_num & masks::fcc_simd_num);

         if (numofranges <= 16)
         {
            const int maxsize = sizeof (char_type2) IS 1 ? 16 : 8;
            const __m128i sranges = this->simdranges;

            for (; (sstate.srchend - sstate.nextpos) >= maxsize;)
            {
               __m128i data;
               std::memcpy(&data, &*sstate.nextpos, 16);
               const int pos = _mm_cmpestri(sranges, numofranges, data, maxsize, sizeof (char_type2) IS 1 ? 4 : 5);

               if (pos IS maxsize)
               {
                  sstate.nextpos += maxsize;
                  continue;
               }
               sstate.nextpos += pos;
               sstate.ssc.iter = sstate.nextpos;

SRELL_NO_VCWARNING(4127)
               if (utf_traits::maxseqlen > 1)
SRELL_NO_VCWARNING_END
               {
                  const ui_l32 cu = *sstate.nextpos & utf_traits::bitsetmask;

                  if (utf_traits::is_mculeading(cu))
                  {
                     const ui_l32 cp = utf_traits::codepoint_inc(sstate.nextpos, sstate.srchend);
                     const re_quantifier &r0q = this->NFA_states[0].quantifier;

                     if (!this->character_class.is_included(r0q.atleast, r0q.atmost, cp))
                        continue;

                     goto SKIP_INC;
                  }
               }
               ++sstate.nextpos;
               SKIP_INC:;

               sstate.reset(this->limit_counter);
               const ui_l32 reason = run_automaton<icase, false>(sstate);
               if (reason)
                  return reason;
            }
         }
      }
      return do_search<icase>(sstate, non_cont_iter());
   }

#endif //  defined(SRELL_HAS_SSE42)

   template <const bool icase, typename BidirectionalIterator>
   ui_l32 do_search(re_search_state<BidirectionalIterator> &sstate, const non_cont_iter) const
   {
      for (;;)
      {
         const bool final = sstate.nextpos IS sstate.srchend;

         sstate.ssc.iter = sstate.nextpos;

         if (!final)
         {
            const ui_l32 cu = *sstate.nextpos & utf_traits::bitsetmask;

            if (!this->firstchar_class_bs.test(cu))
            {
               ++sstate.nextpos;
               continue;
            }

SRELL_NO_VCWARNING(4127)
            if (utf_traits::maxseqlen > 1)
SRELL_NO_VCWARNING_END
            {
               if (utf_traits::is_mculeading(cu))
               {
                  const ui_l32 cp = utf_traits::codepoint_inc(sstate.nextpos, sstate.srchend);
                  const re_quantifier &r0q = this->NFA_states[0].quantifier;

                  if (!this->character_class.is_included(r0q.atleast, r0q.atmost, cp))
                     continue;

                  goto SKIP_INC;
               }
            }
            ++sstate.nextpos;
            SKIP_INC:;
         }
         //  Even when final == true, we have to try for such expressions
         //  as "" =~ /^$/ or "..." =~ /$/.

         sstate.reset(/* first, */ this->limit_counter);
         const ui_l32 reason = run_automaton<icase, false>(sstate /* , false */);
         if (reason)
            return reason;

         if (final)
            break;
      }
      return 0;
   }


   template <const bool icase, typename ContiguousIterator>
   ui_l32 do_search_sc(re_search_state<ContiguousIterator> &sstate, const is_cont_iter) const
   {
      typedef typename std::iterator_traits<ContiguousIterator>::value_type char_type2;
      const char_type2 ec = char_type2(this->NFA_states[0].char_num);

      for (; sstate.nextpos < sstate.srchend;)
      {
         sstate.ssc.iter = sstate.nextpos;

         const char_type2 *const bgnpos = std::char_traits<char_type2>::find(&*sstate.nextpos, sstate.srchend - sstate.nextpos, ec);

         if (bgnpos)
         {
//          sstate.ssc.iter = bgnpos;
            sstate.ssc.iter += bgnpos - &*sstate.nextpos;
            sstate.nextpos = sstate.ssc.iter;

SRELL_NO_VCWARNING(4127)
            if (utf_traits::maxseqlen > 1)
SRELL_NO_VCWARNING_END
            {
               if (utf_traits::is_mculeading(ec))
               {
                  const ui_l32 cp = utf_traits::codepoint_inc(sstate.nextpos, sstate.srchend);
                  const re_quantifier &r0q = this->NFA_states[0].quantifier;

                  if (!this->character_class.is_included(r0q.atleast, r0q.atmost, cp))
                     continue;

                  goto SKIP_INC;
               }
            }
            ++sstate.nextpos;
            SKIP_INC:;

            sstate.reset(this->limit_counter);
            const ui_l32 reason = run_automaton<icase, false>(sstate);
            if (reason)
               return reason;
         }
         else
            break;
      }
      return 0;
   }

   template <const bool icase, typename BidirectionalIterator>
   ui_l32 do_search_sc(re_search_state<BidirectionalIterator> &sstate, const non_cont_iter) const
   {
      typedef typename std::iterator_traits<BidirectionalIterator>::value_type char_type2;
      const char_type2 ec = char_type2(this->NFA_states[0].char_num);

      for (; sstate.nextpos != sstate.srchend;)
      {
         sstate.ssc.iter = find(sstate.nextpos, sstate.srchend, ec);

         if (sstate.ssc.iter != sstate.srchend)
         {
            sstate.nextpos = sstate.ssc.iter;

SRELL_NO_VCWARNING(4127)
            if (utf_traits::maxseqlen > 1)
SRELL_NO_VCWARNING_END
            {
               if (utf_traits::is_mculeading(ec))
               {
                  const ui_l32 cp = utf_traits::codepoint_inc(sstate.nextpos, sstate.srchend);
                  const re_quantifier &r0q = this->NFA_states[0].quantifier;

                  if (!this->character_class.is_included(r0q.atleast, r0q.atmost, cp))
                     continue;

                  goto SKIP_INC;
               }
            }
            ++sstate.nextpos;
            SKIP_INC:;

            sstate.reset(this->limit_counter);
            const ui_l32 reason = run_automaton<icase, false>(sstate);
            if (reason)
               return reason;
         }
         else
            break;
      }
      return 0;
   }

   template <typename BidirectionalIterator, typename CharT0>
   BidirectionalIterator find(BidirectionalIterator begin, const BidirectionalIterator end, const CharT0 c) const
   {
      for (; begin != end; ++begin)
         if ((*begin & utf_traits::bitsetmask) IS (c & utf_traits::bitsetmask))
            break;

      return begin;
   }


   template <typename BidiIter, int N>
   struct contiguous_checker
   {
      typedef non_cont_iter itype;
   };

#if defined(__cpp_concepts)

   template <std::contiguous_iterator CI, int N>
   struct contiguous_checker<CI, N>
   {
      typedef is_cont_iter itype;
   };

#else

   template <int N>
   struct contiguous_checker<const charT *, N>
   {
      typedef is_cont_iter itype;
   };
   template <int N>
   struct contiguous_checker<typename std::basic_string<charT>::const_iterator, N>
   {
      typedef is_cont_iter itype;
   };

#endif

   template <typename T, const bool>
   struct casehelper
   {
      static T canonicalise(const T t)
      {
         return t;
      }
   };

   template <typename T>
   struct casehelper<T, true>
   {
      static T canonicalise(const T t)
      {
         return unicode_case_folding::do_casefolding(t);
      }
   };

   template <const bool icase, const bool reverse, typename BidirectionalIterator>
   ui_l32 run_automaton(re_search_state<BidirectionalIterator> &sstate) const
   {
      typedef casehelper<ui_l32, icase> casehelper_type;
      typedef typename re_object_core<charT, traits>::state_type state_type;
      typedef re_search_state</*charT, */BidirectionalIterator> ss_type;
//    typedef typename ss_type::search_state_core ssc_type;
      typedef typename ss_type::submatchcore_type submatchcore_type;
      typedef typename ss_type::submatch_type submatch_type;
      typedef typename ss_type::counter_type counter_type;
      typedef typename ss_type::position_type position_type;

      goto START;

      NOT_MATCHED:

      if (--sstate.failure_counter)
      {
         NOT_MATCHED0:
         if (sstate.bt_size() > sstate.btstack_size)
         {
            sstate.pop_bt(sstate.ssc);

            sstate.ssc.state = sstate.ssc.state->next_state2;
         }
         else
            return 0;

      }
      else
         return ui_l32(regex_constants::error_complexity);

      for (;;)
      {
         START:

         if (sstate.ssc.state->type IS st_character)
         {
SRELL_NO_VCWARNING(4127)
            if (!reverse)
SRELL_NO_VCWARNING_END
            {
               if (!(sstate.ssc.iter IS sstate.srchend))
               {
                  const BidirectionalIterator prevpos = sstate.ssc.iter;
                  const ui_l32 uchar = casehelper_type::canonicalise(utf_traits::codepoint_inc(sstate.ssc.iter, sstate.srchend));

                  for (;;)
                  {
                     if (sstate.ssc.state->char_num IS uchar)
                        goto MATCHED;

                     if (sstate.ssc.state->next_state2)
                     {
                        sstate.ssc.state = sstate.ssc.state->next_state2;

                        if (sstate.ssc.state->type IS st_character)
                           continue;

                        sstate.ssc.iter = prevpos;
                        goto START2;
                     }
                     break;
                  }
               }
               else if (sstate.ssc.state->next_state2)
               {
                  sstate.ssc.state = sstate.ssc.state->next_state2;
                  continue;
               }
            }
            else  //  reverse IS true.
            {
               if (!(sstate.ssc.iter IS sstate.lblim))
               {
                  const BidirectionalIterator prevpos = sstate.ssc.iter;
                  const ui_l32 uchar = casehelper_type::canonicalise(utf_traits::dec_codepoint(sstate.ssc.iter, sstate.lblim));

                  for (;;)
                  {
                     if (sstate.ssc.state->char_num IS uchar)
                        goto MATCHED;

                     if (sstate.ssc.state->next_state2)
                     {
                        sstate.ssc.state = sstate.ssc.state->next_state2;

                        if (sstate.ssc.state->type IS st_character)
                           continue;

                        sstate.ssc.iter = prevpos;
                        goto START2;
                     }
                     break;
                  }
               }
               else if (sstate.ssc.state->next_state2)
               {
                  sstate.ssc.state = sstate.ssc.state->next_state2;
                  continue;
               }
            }
            goto NOT_MATCHED;
         }

         START2:

         if (sstate.ssc.state->type IS st_character_class)
         {
SRELL_NO_VCWARNING(4127)
            if (!reverse)
SRELL_NO_VCWARNING_END
            {
               if (!(sstate.ssc.iter IS sstate.srchend))
               {
                  const BidirectionalIterator prevpos = sstate.ssc.iter;
                  const ui_l32 uchar = utf_traits::codepoint_inc(sstate.ssc.iter, sstate.srchend);

                  if (this->character_class.is_included(sstate.ssc.state->quantifier.atleast, sstate.ssc.state->quantifier.atmost, uchar))
                     goto MATCHED;

                  if (sstate.ssc.state->next_state2)
                  {
                     sstate.ssc.state = sstate.ssc.state->next_state2;

                     sstate.ssc.iter = prevpos;
                     continue;
                  }
               }
               else if (sstate.ssc.state->next_state2)
               {
                  sstate.ssc.state = sstate.ssc.state->next_state2;
                  continue;
               }
            }
            else  //  reverse IS true.
            {
               if (!(sstate.ssc.iter IS sstate.lblim))
               {
                  const BidirectionalIterator prevpos = sstate.ssc.iter;
                  const ui_l32 uchar = utf_traits::dec_codepoint(sstate.ssc.iter, sstate.lblim);

                  if (this->character_class.is_included(sstate.ssc.state->quantifier.atleast, sstate.ssc.state->quantifier.atmost, uchar))
                     goto MATCHED;

                  if (sstate.ssc.state->next_state2)
                  {
                     sstate.ssc.state = sstate.ssc.state->next_state2;

                     sstate.ssc.iter = prevpos;
                     continue;
                  }
               }
               else if (sstate.ssc.state->next_state2)
               {
                  sstate.ssc.state = sstate.ssc.state->next_state2;
                  continue;
               }
            }
            goto NOT_MATCHED;
         }

         if (sstate.ssc.state->type IS st_epsilon)
         {
            {
               sstate.push_bt_wc(sstate.ssc);
            }

            MATCHED:
            sstate.ssc.state = sstate.ssc.state->next_state1;
            continue;
         }

         switch (sstate.ssc.state->type)
         {
         case st_check_counter:
            {
               ST_CHECK_COUNTER:
               const counter_type counter = sstate.counter[sstate.ssc.state->char_num];

               if (counter.no < sstate.ssc.state->quantifier.atleast)
               {
                  ++sstate.ssc.state;
               }
               else
               {
                  if (counter.no < sstate.ssc.state->quantifier.atmost or sstate.ssc.state->quantifier.is_infinity())
                  {
                     sstate.push_bt_wc(sstate.ssc);
                     sstate.ssc.state = sstate.ssc.state->next_state1;
                  }
                  else
                  {
                     sstate.ssc.state = sstate.ssc.state->quantifier.is_greedy
                        ? sstate.ssc.state->next_state2
                        : sstate.ssc.state->next_state1;
                  }
                  continue;
               }
            }
            //@fallthrough@

         case st_increment_counter:
            {
               counter_type &counter = sstate.counter[sstate.ssc.state->char_num];

               if (counter.no != constants::infinity)
               {
                  ++counter.no;
                  if (sstate.ssc.state->next_state2)
                     sstate.push_bt_wc(sstate.ssc);
               }
            }
            goto MATCHED;

         case st_decrement_counter:
            --sstate.counter[sstate.ssc.state->char_num].no;
            goto NOT_MATCHED0;

         case st_save_and_reset_counter:
            {
               counter_type &counter = sstate.counter[sstate.ssc.state->char_num];

               sstate.expand(sizeof counter + sizeof sstate.ssc);

               sstate.push_c(counter);
               sstate.push_bt(sstate.ssc);
               counter.no = 0;
            }
            sstate.ssc.state = sstate.ssc.state->next_state1;
            goto ST_CHECK_COUNTER;

         case st_restore_counter:
            sstate.pop_c(sstate.counter[sstate.ssc.state->char_num]);
            goto NOT_MATCHED0;

         case st_roundbracket_open: //  '(':
            {
               submatch_type &bracket = sstate.bracket[sstate.ssc.state->char_num];
               ui_l32 extra = (bracket.counter.no + 1) != 0 ? 0 : 2; //  To skip 0 and 1 after -1.
               const re_quantifier &sq = sstate.ssc.state->quantifier;
               const typename ss_type::btstack_size_type addsize = (sq.atleast <= sq.atmost ? ((sizeof (submatchcore_type) + sizeof (counter_type)) * (sq.atmost - sq.atleast + 1)) : 0) + sizeof (submatchcore_type) + sizeof sstate.ssc;

               TWOMORE:
               sstate.expand(addsize);

               sstate.push_sm(bracket.core);
               ++bracket.counter.no;

               for (ui_l32 brno = sstate.ssc.state->quantifier.atleast; brno <= sstate.ssc.state->quantifier.atmost; ++brno)
               {
                  submatch_type &inner_bracket = sstate.bracket[brno];

                  sstate.push_sm(inner_bracket.core);
                  sstate.push_c(inner_bracket.counter);
                  inner_bracket.core.open_at = inner_bracket.core.close_at = sstate.srchend;
                  inner_bracket.counter.no = 0;
                  //  ECMAScript spec (3-5.1) 15.10.2.5, NOTE 3.
                  //  ECMAScript 2018 (ES9) 21.2.2.5.1, Note 3.
               }
               sstate.push_bt(sstate.ssc);

               if (extra)
               {
                  --extra;
                  goto TWOMORE;
               }

               (!reverse ? bracket.core.open_at : bracket.core.close_at) = sstate.ssc.iter;
            }
            goto MATCHED;

         case st_roundbracket_pop:  //  '/':
            {
               for (ui_l32 brno = sstate.ssc.state->quantifier.atmost; brno >= sstate.ssc.state->quantifier.atleast; --brno)
               {
                  submatch_type &inner_bracket = sstate.bracket[brno];

                  sstate.pop_c(inner_bracket.counter);
                  sstate.pop_sm(inner_bracket.core);
               }

               submatch_type &bracket = sstate.bracket[sstate.ssc.state->char_num];

               --bracket.counter.no;
               sstate.pop_sm(bracket.core);
            }
            goto NOT_MATCHED0;

         case st_roundbracket_close:   //  ')':
            {
               submatch_type &bracket = sstate.bracket[sstate.ssc.state->char_num];
               submatchcore_type &brc = bracket.core;

               if ((!reverse ? brc.open_at : brc.close_at) != sstate.ssc.iter)
               {
                  sstate.ssc.state = sstate.ssc.state->next_state1;
               }
               else  //  0 width match, breaks from the loop.
               {
                  if (sstate.ssc.state->next_state1->type != st_check_counter)
                  {
                     //  .atleast is 0 (*) or 1 (+). To rewind correctly, if .counter being
                     //  equal to -1 is incremented the next value must be 2, skipping 0 and 1.
                     if (bracket.counter.no > sstate.ssc.state->quantifier.atleast)
                        goto NOT_MATCHED0;

                     sstate.ssc.state = sstate.ssc.state->next_state2;
                        //  Accepts 0 width match and exits.
                  }
                  else  //  A pair with check_counter.
                  {
                     const counter_type counter = sstate.counter[sstate.ssc.state->next_state1->char_num];

                     if (counter.no > sstate.ssc.state->quantifier.atleast)
                        goto NOT_MATCHED0;   //  Takes a captured string in the previous loop.

                     sstate.ssc.state = sstate.ssc.state->next_state1;
                        //  Accepts 0 width match and continues.
                  }
               }
               (!reverse ? brc.close_at : brc.open_at) = sstate.ssc.iter;
            }
            continue;

         case st_repeat_in_push:
            {
               position_type &r = sstate.repeat[sstate.ssc.state->char_num];
               const re_quantifier &sq = sstate.ssc.state->quantifier;

               sstate.expand(sizeof r + (sq.atleast <= sq.atmost ? ((sizeof (submatchcore_type) + sizeof (counter_type)) * (sq.atmost - sq.atleast + 1)) : 0) + sizeof sstate.ssc);

               sstate.push_rp(r);
               r = sstate.ssc.iter;

               for (ui_l32 brno = sstate.ssc.state->quantifier.atleast; brno <= sstate.ssc.state->quantifier.atmost; ++brno)
               {
                  submatch_type &inner_bracket = sstate.bracket[brno];

                  sstate.push_sm(inner_bracket.core);
                  sstate.push_c(inner_bracket.counter);
                  inner_bracket.core.open_at = inner_bracket.core.close_at = sstate.srchend;
                  inner_bracket.counter.no = 0;
                  //  ECMAScript 2019 (ES10) 21.2.2.5.1, Note 3.
               }
               sstate.push_bt(sstate.ssc);
            }
            goto MATCHED;

         case st_repeat_in_pop:
            for (ui_l32 brno = sstate.ssc.state->quantifier.atmost; brno >= sstate.ssc.state->quantifier.atleast; --brno)
            {
               submatch_type &inner_bracket = sstate.bracket[brno];

               sstate.pop_c(inner_bracket.counter);
               sstate.pop_sm(inner_bracket.core);
            }

            sstate.pop_rp(sstate.repeat[sstate.ssc.state->char_num]);
            goto NOT_MATCHED0;

         case st_check_0_width_repeat:
            if (sstate.ssc.iter != sstate.repeat[sstate.ssc.state->char_num])
               goto MATCHED;

            if (sstate.ssc.state->next_state1->type IS st_check_counter)
            {
               const counter_type counter = sstate.counter[sstate.ssc.state->next_state1->char_num];

               if (counter.no > sstate.ssc.state->next_state1->quantifier.atleast)
                  goto NOT_MATCHED0;

               sstate.ssc.state = sstate.ssc.state->next_state1;
            }
            else
               sstate.ssc.state = sstate.ssc.state->next_state2;

            continue;

         case st_backreference:  //  '\\':
            {
               const submatch_type &bracket = sstate.bracket[sstate.ssc.state->char_num];
               const submatchcore_type &brc = bracket.core;

               if (bracket.counter.no IS 0 or brc.open_at IS brc.close_at) //  Undefined or "".
               {
                  sstate.ssc.state = sstate.ssc.state->next_state2;
                  continue;
               }

SRELL_NO_VCWARNING(4127)
               if (!reverse)
SRELL_NO_VCWARNING_END
               {
                  BidirectionalIterator backrefpos = brc.open_at;

                  if (!sstate.ssc.state->flags) //  !icase.
                  {
                     for (; backrefpos != brc.close_at;)
                     {
                        if (sstate.ssc.iter IS sstate.srchend or *sstate.ssc.iter++ != *backrefpos++)
                           goto NOT_MATCHED;
                     }
                  }
                  else  //  icase.
                  {
                     for (; backrefpos != brc.close_at;)
                     {
                        if (!(sstate.ssc.iter IS sstate.srchend))
                        {
                           const ui_l32 uchartxt = utf_traits::codepoint_inc(sstate.ssc.iter, sstate.srchend);
                           const ui_l32 ucharref = utf_traits::codepoint_inc(backrefpos, brc.close_at);

                           if (unicode_case_folding::do_casefolding(uchartxt) IS unicode_case_folding::do_casefolding(ucharref))
                              continue;
                        }
                        goto NOT_MATCHED;
                     }
                  }
               }
               else  //  reverse IS true.
               {
                  BidirectionalIterator backrefpos = brc.close_at;

                  if (!sstate.ssc.state->flags) //  !icase.
                  {
                     for (; backrefpos != brc.open_at;)
                     {
                        if (sstate.ssc.iter IS sstate.lblim or *--sstate.ssc.iter != *--backrefpos)
                           goto NOT_MATCHED;
                     }
                  }
                  else  //  icase.
                  {
                     for (; backrefpos != brc.open_at;)
                     {
                        if (!(sstate.ssc.iter IS sstate.lblim))
                        {
                           const ui_l32 uchartxt = utf_traits::dec_codepoint(sstate.ssc.iter, sstate.lblim);
                           const ui_l32 ucharref = utf_traits::dec_codepoint(backrefpos, brc.open_at);

                           if (unicode_case_folding::do_casefolding(uchartxt) IS unicode_case_folding::do_casefolding(ucharref))
                              continue;
                        }
                        goto NOT_MATCHED;
                     }
                  }
               }
            }
            goto MATCHED;

         case st_lookaround_open:
            {
               const state_type *const lostate = sstate.ssc.state;
               const re_quantifier *const losq = &lostate->quantifier;

               sstate.expand((losq->atleast <= losq->atmost ? ((sizeof (submatchcore_type) + sizeof (counter_type)) * (losq->atmost - losq->atleast + 1)) : 0) + sizeof sstate.ssc);

               for (ui_l32 brno = losq->atleast; brno <= losq->atmost; ++brno)
               {
                  const submatch_type &sm = sstate.bracket[brno];
                  sstate.push_sm(sm.core);
                  sstate.push_c(sm.counter);
               }

               const typename ss_type::bottom_state backup_bottom(sstate.btstack_size, sstate);
               const BidirectionalIterator orgpos = sstate.ssc.iter;

               if (losq->atleast <= losq->atmost)
                  sstate.push_bt(sstate.ssc);

               if (losq->is_greedy >= 2)
                  sstate.lblim = sstate.srchbegin;

               sstate.btstack_size = sstate.bt_size();

               sstate.ssc.state = lostate->next_state2->next_state1;

               //  sstate.ssc.state is no longer pointing to lookaround_open!

               const ui_l32 is_matched = (losq->is_greedy IS 0 ? run_automaton<icase, false>(sstate) : run_automaton<icase, true>(sstate));

               if (is_matched >> 1)
                  return is_matched;

               sstate.bt_resize(sstate.btstack_size);

               if (losq->is_greedy >= 2)
               {
                  sstate.lblim = sstate.reallblim;
                  if (is_matched)
                     sstate.curbegin = sstate.ssc.iter;
               }

#if defined(SRELL_ENABLE_GT)
               if (lostate->char_num != meta_char::mc_gt)   //  '>'
#endif
               {
                  if (losq->is_greedy < 3)
                     sstate.ssc.iter = orgpos;
               }

               backup_bottom.restore(sstate.btstack_size, sstate);

               if (is_matched ^ lostate->flags)
               {
                  if (losq->is_greedy IS 3)
                     sstate.ssc.state = this->NFA_states[0].next_state2;
                  else
                     sstate.ssc.state = lostate->next_state1;
                  continue;
               }

               if (losq->atleast <= losq->atmost)
                  sstate.pop_bt(sstate.ssc);
               sstate.ssc.state = lostate->next_state2;
            }
            //@fallthrough@

         case st_lookaround_pop:
            for (ui_l32 brno = sstate.ssc.state->quantifier.atmost; brno >= sstate.ssc.state->quantifier.atleast; --brno)
            {
               submatch_type &sm = sstate.bracket[brno];

               sstate.pop_c(sm.counter);
               sstate.pop_sm(sm.core);
            }
            goto NOT_MATCHED0;

         case st_bol:   //  '^':
            if (sstate.ssc.iter IS sstate.lblim and !(sstate.reallblim != sstate.lblim or (sstate.flags & regex_constants::match_prev_avail) != 0))
            {
               if (!(sstate.flags & regex_constants::match_not_bol))
                  goto MATCHED;
            }
               //  !sstate.is_at_lookbehindlimit() or sstate.match_prev_avail_flag()
            else if (sstate.ssc.state->flags)   //  multiline.
            {
               BidirectionalIterator lb(sstate.ssc.iter);
               const ui_l32 prevchar = utf_traits::dec_codepoint(lb, sstate.reallblim);

               if (this->character_class.is_included(sstate.ssc.state->quantifier.atleast, sstate.ssc.state->quantifier.atmost, prevchar))
                  goto MATCHED;
            }
            goto NOT_MATCHED;

         case st_eol:   //  '$':
            if (sstate.ssc.iter IS sstate.srchend)
            {
               if (!(sstate.flags & regex_constants::match_not_eol))
                  goto MATCHED;
            }
            else if (sstate.ssc.state->flags)   //  multiline.
            {
               BidirectionalIterator la(sstate.ssc.iter);
               const ui_l32 nextchar = utf_traits::codepoint_inc(la, sstate.srchend);

               if (this->character_class.is_included(sstate.ssc.state->quantifier.atleast, sstate.ssc.state->quantifier.atmost, nextchar))
                  goto MATCHED;
            }
            goto NOT_MATCHED;

         case st_boundary: //  '\b' '\B'
            {
               ui_l32 is_matched = sstate.ssc.state->flags; //  is_not.
//             is_matched = sstate.ssc.state->char_num == char_alnum::ch_B;

               //  First, suppose the previous character is not \w but \W.

               if (sstate.ssc.iter IS sstate.srchend)
               {
                  if (sstate.flags & regex_constants::match_not_eow)
                     is_matched ^= 1u;
               }
               else
               {
                  BidirectionalIterator la(sstate.ssc.iter);
                  if (this->character_class.is_included(sstate.ssc.state->quantifier.atleast, sstate.ssc.state->quantifier.atmost, utf_traits::codepoint_inc(la, sstate.srchend)))
                  {
                     is_matched ^= 1u;
                  }
               }
               //      \W/last     \w
               //  \b  false       true
               //  \B  true        false

               //  Second, if the actual previous character is \w, flip is_matched.

               if (sstate.ssc.iter IS sstate.lblim and !(sstate.reallblim != sstate.lblim or (sstate.flags & regex_constants::match_prev_avail) != 0))
               {
                  if (sstate.flags & regex_constants::match_not_bow)
                     is_matched ^= 1u;
               }
               else
               {
                  BidirectionalIterator lb(sstate.ssc.iter);
                  //  !sstate.is_at_lookbehindlimit() or sstate.match_prev_avail_flag()
                  if (this->character_class.is_included(sstate.ssc.state->quantifier.atleast, sstate.ssc.state->quantifier.atmost, utf_traits::dec_codepoint(lb, sstate.reallblim)))
                  {
                     is_matched ^= 1u;
                  }
               }
               //  \b                          \B
               //  pre cur \W/last \w          pre cur \W/last \w
               //  \W/base false   true        \W/base true    false
               //  \w      true    false       \w      false   true

               if (is_matched)
                  goto MATCHED;

               goto NOT_MATCHED;
            }

         case st_success:  //  IS lookaround_close.
//          if (is_recursive)
            if (sstate.btstack_size)
               return 1;

            if
            (
               (!(sstate.flags & regex_constants::match_not_null) or !(sstate.ssc.iter IS sstate.curbegin))
               &&
               (!(sstate.flags & regex_constants::match_whole) or sstate.ssc.iter IS sstate.srchend)
            )
               return 1;

            goto NOT_MATCHED0;

         default:
            //  Reaching here means that this->NFA_states is corrupted.
            return ui_l32(regex_constants::error_internal);
         }
      }
   }
};
//  re_object

   }  //  namespace re_detail

//  ... "rei_algorithm.hpp"]
//  ["basic_regex.hpp" ...

template <class charT, class traits = regex_traits<charT> >
class basic_regex : public re_detail::re_object<charT, traits>
{
public:

   //  Types:
   typedef charT value_type;
   typedef traits traits_type;
   typedef typename traits::string_type string_type;
   typedef regex_constants::syntax_option_type flag_type;
   typedef typename traits::locale_type locale_type;
   typedef typename re_detail::concon_view<charT> contiguous_container_view;

   static const regex_constants::syntax_option_type icase = regex_constants::icase;
   static const regex_constants::syntax_option_type nosubs = regex_constants::nosubs;
   static const regex_constants::syntax_option_type optimize = regex_constants::optimize;
   static const regex_constants::syntax_option_type collate = regex_constants::collate;
   static const regex_constants::syntax_option_type ECMAScript = regex_constants::ECMAScript;
   static const regex_constants::syntax_option_type basic = regex_constants::basic;
   static const regex_constants::syntax_option_type extended = regex_constants::extended;
   static const regex_constants::syntax_option_type awk = regex_constants::awk;
   static const regex_constants::syntax_option_type grep = regex_constants::grep;
   static const regex_constants::syntax_option_type egrep = regex_constants::egrep;
   static const regex_constants::syntax_option_type multiline = regex_constants::multiline;

   static const regex_constants::syntax_option_type sticky = regex_constants::sticky;
   static const regex_constants::syntax_option_type dotall = regex_constants::dotall;
   static const regex_constants::syntax_option_type unicodesets = regex_constants::unicodesets;
   static const regex_constants::syntax_option_type vmode = regex_constants::vmode;
   static const regex_constants::syntax_option_type quiet = regex_constants::quiet;

   basic_regex()
   {
   }

   explicit basic_regex(const charT *const p, const flag_type f = regex_constants::ECMAScript)
   {
      assign(p, std::char_traits<charT>::length(p), f);
   }

   basic_regex(const charT *const p, const std::size_t len, const flag_type f = regex_constants::ECMAScript)
   {
      assign(p, len, f);
   }

   basic_regex(const basic_regex &e)
   {
      assign(e);
   }

#if defined(__cpp_rvalue_references)
   basic_regex(basic_regex &&e) SRELL_NOEXCEPT
   {
      assign(std::move(e));
   }
#endif

// ~basic_regex();

   basic_regex &operator=(const basic_regex &right)
   {
      return assign(right);
   }

#if defined(__cpp_rvalue_references)
   basic_regex &operator=(basic_regex &&e) SRELL_NOEXCEPT
   {
      return assign(std::move(e));
   }
#endif

   basic_regex &operator=(const charT *const ptr)
   {
      return assign(ptr);
   }

   basic_regex &assign(const basic_regex &right)
   {
      re_detail::re_object_core<charT, traits>::operator=(right);
      return *this;
   }

#if defined(__cpp_rvalue_references)
   basic_regex &assign(basic_regex &&right) SRELL_NOEXCEPT
   {
      re_detail::re_object_core<charT, traits>::operator=(std::move(right));
      return *this;
   }
#endif

   basic_regex &assign(const charT *const ptr, const flag_type f = regex_constants::ECMAScript)
   {
      return assign(ptr, std::char_traits<charT>::length(ptr), f);
   }

   basic_regex &assign(const charT *const p, std::size_t len, const flag_type f = regex_constants::ECMAScript)
   {
      this->compile(p, p + len, f);
      return *this;
   }

   unsigned mark_count() const
   {
      return this->number_of_brackets - 1;
   }

   flag_type flags() const
   {
      return flag_type(this->soflags & re_detail::masks::somask);
   }

   locale_type imbue(locale_type /* loc */)
   {
      return locale_type();
   }

   locale_type getloc() const
   {
      return locale_type();
   }

   void swap(basic_regex &e)
   {
      re_detail::re_object_core<charT, traits>::swap(e);
   }

   regex_constants::error_type ecode() const
   {
      return re_detail::re_object_core<charT, traits>::ecode();
   }

#if !defined(SRELL_NO_APIEXT)

   template <typename BidirectionalIterator, typename MA>
   bool match(
      const BidirectionalIterator begin,
      const BidirectionalIterator end,
      match_results<BidirectionalIterator, MA> &m,
      const regex_constants::match_flag_type flags = regex_constants::match_default) const
   {
      return base_type::search(begin, end, begin, m, flags | regex_constants::match_continuous | regex_constants::match_whole);
   }

   template <typename MA>
   bool match(
      const charT *const str,
      match_results<const charT *, MA> &m,
      const regex_constants::match_flag_type flags = regex_constants::match_default) const
   {
      return this->match(str, str + std::char_traits<charT>::length(str), m, flags);
   }

   template <typename BidirectionalIterator, typename MA>
   bool search(
      const BidirectionalIterator begin,
      const BidirectionalIterator end,
      const BidirectionalIterator lookbehind_limit,
      match_results<BidirectionalIterator, MA> &m,
      const regex_constants::match_flag_type flags = regex_constants::match_default) const
   {
      return base_type::search(begin, end, lookbehind_limit, m, flags);
   }

   template <class MA>
   bool search(
      const contiguous_container_view c,
      const std::size_t start,
      match_results<const charT *, MA> &m,
      const regex_constants::match_flag_type flags = regex_constants::match_default) const
   {
      return base_type::search(c.data_ + start, c.data_ + c.size_, c.data_, m, flags);
   }

   template <typename BidirectionalIterator, typename MA>
   bool search(
      const BidirectionalIterator begin,
      const BidirectionalIterator end,
      match_results<BidirectionalIterator, MA> &m,
      const regex_constants::match_flag_type flags = regex_constants::match_default) const
   {
      return base_type::search(begin, end, begin, m, flags);
   }

   template <typename MA>
   bool search(
      const charT *const str,
      match_results<const charT *, MA> &m,
      const regex_constants::match_flag_type flags = regex_constants::match_default) const
   {
      return this->search(str, str + std::char_traits<charT>::length(str), m, flags);
   }

private:

   typedef re_detail::re_object<charT, traits> base_type;

#endif   //  !defined(SRELL_NO_APIEXT)
};
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::icase;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::nosubs;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::optimize;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::collate;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::ECMAScript;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::basic;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::extended;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::awk;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::grep;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::egrep;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::multiline;

template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::sticky;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::dotall;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::unicodesets;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::vmode;
template <class charT, class traits>
   const regex_constants::syntax_option_type basic_regex<charT, traits>::quiet;

template <class charT, class traits>
void swap(basic_regex<charT, traits> &lhs, basic_regex<charT, traits> &rhs)
{
   lhs.swap(rhs);
}

//  ... "basic_regex.hpp"]
//  ["regex_iterator.hpp" ...

template <class BidirectionalIterator, class charT = typename std::iterator_traits<BidirectionalIterator>::value_type, class traits = regex_traits<charT> >
class regex_iterator
{
public:

   typedef basic_regex<charT, traits> regex_type;
   typedef match_results<BidirectionalIterator> value_type;
   typedef std::ptrdiff_t difference_type;
   typedef const value_type *pointer;
   typedef const value_type &reference;
   typedef std::forward_iterator_tag iterator_category;

   regex_iterator()
   {
      //  28.12.1.1: Constructs an end-of-sequence iterator.
   }

   regex_iterator(
      const BidirectionalIterator a,
      const BidirectionalIterator b,
      const regex_type &re,
      const regex_constants::match_flag_type m = regex_constants::match_default)
      : begin(a), end(b), pregex(&re), flags(m)
   {
      regex_search(begin, end, begin, match, *pregex, flags);
         //  28.12.1.1: If this call returns false the constructor
         //    sets *this to the end-of-sequence iterator.
   }

   regex_iterator(const regex_iterator &that)
   {
      operator=(that);
   }

   regex_iterator &operator=(const regex_iterator &that)
   {
      if (this != &that)
      {
         this->match = that.match;
         if (this->match.size() > 0)
         {
            this->begin = that.begin;
            this->end = that.end;
            this->pregex = that.pregex;
            this->flags = that.flags;
         }
      }
      return *this;
   }

   bool operator==(const regex_iterator &right) const
   {
      if (right.match.size() IS 0 or this->match.size() IS 0)
         return this->match.size() IS right.match.size();

      return this->begin IS right.begin
         and this->end IS right.end
         and this->pregex IS right.pregex
         and this->flags IS right.flags
         and this->match[0] IS right.match[0];
   }

   bool operator!=(const regex_iterator &right) const
   {
      return !(*this IS right);
   }

   const value_type &operator*() const
   {
      return match;
   }

   const value_type *operator->() const
   {
      return &match;
   }

   regex_iterator &operator++()
   {
      if (this->match.size())
      {
         BidirectionalIterator start = match[0].second;

         if (match[0].first IS start)  //  The iterator holds a 0-length match.
         {
            if (start IS end)
            {
               match.clear_();
               //    28.12.1.4.2: If the iterator holds a zero-length match and
               //  start == end the operator sets *this to the end-ofsequence
               //  iterator and returns *this.
            }
            else
            {
               //    28.12.1.4.3: Otherwise, if the iterator holds a zero-length match
               //  the operator calls regex_search(start, end, match, *pregex, flags
               //  | regex_constants::match_not_null | regex_constants::match_continuous).
               //  If the call returns true the operator returns *this. [Cont...]

               if (!regex_search(start, end, begin, match, *pregex, flags | regex_constants::match_not_null | regex_constants::match_continuous))
               {
                  const BidirectionalIterator prevend = start;

                  //  [...Cont] Otherwise the operator increments start and continues
                  //  as if the most recent match was not a zero-length match.
//                ++start;
                  utf_traits::codepoint_inc(start, end);

                  flags |= regex_constants::match_prev_avail;

                  if (regex_search(start, end, begin, match, *pregex, flags))
                  {
                     //    28.12.1.4.5-6: In all cases in which the call to regex_search
                     //  returns true, match.prefix().first shall be equal to the previous
                     //  value of match[0].second, ... match[i].position() shall return
                     //  distance(begin, match[i].first).
                     //    This means that match[i].position() gives the offset from the
                     //  beginning of the target sequence, which is often not the same as
                     //  the offset from the sequence passed in the call to regex_search.
                     //
                     //  To satisfy this:
                     match.update_prefix1_(prevend);
                  }
               }
            }
         }
         else
         {
            //    28.12.1.4.4: If the most recent match was not a zero-length match,
            //  the operator sets flags to flags | regex_constants::match_prev_avail
            //  and calls regex_search(start, end, match, *pregex, flags). [Cont...]
            flags |= regex_constants::match_prev_avail;

            regex_search(start, end, begin, match, *pregex, flags);
            //  [...Cont] If the call returns false the iterator sets *this to
            //  the end-of-sequence iterator. The iterator then returns *this.
            //
            //    28.12.1.4.5-6: In all cases in which the call to regex_search
            //  returns true, match.prefix().first shall be equal to the previous
            //  value of match[0].second, ... match[i].position() shall return
            //  distance(begin, match[i].first).
            //    This means that match[i].position() gives the offset from the
            //  beginning of the target sequence, which is often not the same as
            //  the offset from the sequence passed in the call to regex_search.
            //
            //  These should already be done in regex_search.
         }
      }
      return *this;
   }

   regex_iterator operator++(int)
   {
      const regex_iterator tmp = *this;
      ++(*this);
      return tmp;
   }

private:

   BidirectionalIterator                begin;
   BidirectionalIterator                end;
   const regex_type                    *pregex;
   regex_constants::match_flag_type     flags;
   match_results<BidirectionalIterator> match;

   typedef typename traits::utf_traits utf_traits;
};


//  ... "regex_iterator.hpp"]
//  ["regex_algorithm.hpp" ...

template <class BidirectionalIterator, class Allocator, class charT, class traits>
bool regex_search(
   const BidirectionalIterator first,
   const BidirectionalIterator last,
   const BidirectionalIterator lookbehind_limit,
   match_results<BidirectionalIterator, Allocator> &m,
   const basic_regex<charT, traits> &e,
   const regex_constants::match_flag_type flags = regex_constants::match_default)
{
   return e.search(first, last, lookbehind_limit, m, flags);
}

//  ... "regex_algorithm.hpp"]

typedef sub_match<const char *> u8ccsub_match;
typedef sub_match<std::string::const_iterator> u8cssub_match;

typedef match_results<const char *> u8ccmatch;
typedef match_results<std::string::const_iterator> u8csmatch;

typedef basic_regex<char, u8regex_traits<char> > u8cregex;

typedef regex_iterator<const char *, std::iterator_traits<const char *>::value_type, u8regex_traits<std::iterator_traits<const char *>::value_type> > u8ccregex_iterator;
typedef regex_iterator<std::string::const_iterator, std::iterator_traits<std::string::const_iterator>::value_type, u8regex_traits<std::iterator_traits<std::string::const_iterator>::value_type> > u8csregex_iterator;


#if defined(WCHAR_MAX)
        #if (WCHAR_MAX >= 0x10ffff)
                typedef sub_match<const wchar_t *> u32wcsub_match;
                typedef sub_match<std::wstring::const_iterator> u32wssub_match;
                typedef u32wcsub_match u1632wcsub_match;
                typedef u32wssub_match u1632wssub_match;

                typedef match_results<const wchar_t *> u32wcmatch;
                typedef match_results<std::wstring::const_iterator> u32wsmatch;
                typedef u32wcmatch u1632wcmatch;
                typedef u32wsmatch u1632wsmatch;

                typedef basic_regex<wchar_t> u32wregex;
                typedef u32wregex u1632wregex;

                typedef regex_iterator<const wchar_t *> u32wcregex_iterator;
                typedef regex_iterator<std::wstring::const_iterator> u32wsregex_iterator;
                typedef u32wcregex_iterator u1632wcregex_iterator;
                typedef u32wsregex_iterator u1632wsregex_iterator;


        #elif (WCHAR_MAX >= 0xffff)
                typedef sub_match<const wchar_t *> u16wcsub_match;
                typedef sub_match<std::wstring::const_iterator> u16wssub_match;
                typedef u16wcsub_match u1632wcsub_match;
                typedef u16wssub_match u1632wssub_match;

                typedef match_results<const wchar_t *> u16wcmatch;
                typedef match_results<std::wstring::const_iterator> u16wsmatch;
                typedef u16wcmatch u1632wcmatch;
                typedef u16wsmatch u1632wsmatch;

                typedef basic_regex<wchar_t, u16regex_traits<wchar_t> > u16wregex;
                typedef u16wregex u1632wregex;

                typedef regex_iterator<const wchar_t *, std::iterator_traits<const wchar_t *>::value_type, u16regex_traits<std::iterator_traits<const wchar_t *>::value_type> > u16wcregex_iterator;
                typedef regex_iterator<std::wstring::const_iterator, std::iterator_traits<std::wstring::const_iterator>::value_type, u16regex_traits<std::iterator_traits<std::wstring::const_iterator>::value_type> > u16wsregex_iterator;
                typedef u16wcregex_iterator u1632wcregex_iterator;
                typedef u16wsregex_iterator u1632wsregex_iterator;


        #endif
#endif

#if defined(__cpp_unicode_characters)
   typedef sub_match<const char16_t *> u16csub_match;
   typedef sub_match<const char32_t *> u32csub_match;
   typedef sub_match<std::u16string::const_iterator> u16ssub_match;
   typedef sub_match<std::u32string::const_iterator> u32ssub_match;

   typedef match_results<const char16_t *> u16cmatch;
   typedef match_results<const char32_t *> u32cmatch;
   typedef match_results<std::u16string::const_iterator> u16smatch;
   typedef match_results<std::u32string::const_iterator> u32smatch;

   typedef basic_regex<char16_t> u16regex;
   typedef basic_regex<char32_t> u32regex;

   typedef regex_iterator<const char16_t *> u16cregex_iterator;
   typedef regex_iterator<const char32_t *> u32cregex_iterator;
   typedef regex_iterator<std::u16string::const_iterator> u16sregex_iterator;
   typedef regex_iterator<std::u32string::const_iterator> u32sregex_iterator;


#endif

#if defined(__cpp_char8_t)
   #if defined(__cpp_lib_char8_t)
   #define SRELLTMP_U8S_CI std::u8string::const_iterator
   #else
   #define SRELLTMP_U8S_CI std::basic_string<char8_t>::const_iterator
   #endif
   typedef sub_match<const char8_t *> u8csub_match;
   typedef sub_match<SRELLTMP_U8S_CI> u8ssub_match;

   typedef match_results<const char8_t *> u8cmatch;
   typedef match_results<SRELLTMP_U8S_CI> u8smatch;

   typedef basic_regex<char8_t> u8regex;

   typedef regex_iterator<const char8_t *> u8cregex_iterator;
   typedef regex_iterator<SRELLTMP_U8S_CI> u8sregex_iterator;


   #undef SRELLTMP_U8S_CI
#else
   typedef u8ccsub_match u8csub_match;
   typedef u8cssub_match u8ssub_match;

   typedef u8ccmatch u8cmatch;
   typedef u8csmatch u8smatch;

   typedef u8cregex u8regex;

   typedef u8ccregex_iterator u8cregex_iterator;
   typedef u8csregex_iterator u8sregex_iterator;


#endif

}     //  namespace srell
#endif   //  SRELL_REGEX_TEMPLATE_LIBRARY
