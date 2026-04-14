#pragma once

//  unicodetable.h
//  (C) Copyright 2001-2022 Paul Manias

struct UnicodeDescriptor {
   STRING Name;
   UWORD Value;
};

// The best way to access the unicode table is to grab it from the keyboard
// module or the SystemKeyboard object rather than including this table
// in your code.

// This table is sorted in order of the unicode values.

#ifdef UNICODE_TABLE

static struct UnicodeDescriptor glUnicodeTable[] = {
  { "Unprintable",          0x0000 },  // Unprintable character
  { "Space",                0x0020 },  // space
  { "Exclamation",          0x0021 },  // exclamation mark
  { "DoubleQuote",          0x0022 },  // quotation mark
  { "Hash",                 0x0023 },  // number sign
  { "Dollar",               0x0024 },  // dollar sign
  { "Percent",              0x0025 },  // percent sign
  { "Ampersand",            0x0026 },  // ampersand
  { "Quote",                0x0027 },  // Single quotation mark
  { "LeftBracket",          0x0028 },  // left parenthesis
  { "RightBracket",         0x0029 },  // right parenthesis
  { "Asterisk",             0x002a },  // asterisk
  { "Plus",                 0x002b },  // sign
  { "Comma",                0x002c },  // comma
  { "Minus",                0x002d },  // minus
  { "Period",               0x002e },  // period
  { "ForwardSlash",         0x002f },  // slash
  { "Zero",                 0x0030 },  // digit zero
  { "One",                  0x0031 },  // digit one
  { "Two",                  0x0032 },  // digit two
  { "Three",                0x0033 },  // digit three
  { "Four",                 0x0034 },  // digit four
  { "Five",                 0x0035 },  // digit five
  { "Six",                  0x0036 },  // digit six
  { "Seven",                0x0037 },  // digit seven
  { "Eight",                0x0038 },  // digit eight
  { "Nine",                 0x0039 },  // digit nine
  { "Colon",                0x003a },  // colon
  { "SemiColon",            0x003b },  // semicolon
  { "Lesser",               0x003c },  // less-than sign
  { "Equals",               0x003d },  // equals sign
  { "Greater",              0x003e },  // greater-than sign
  { "Question",             0x003f },  // question mark
  { "At",                   0x0040 },  // commercial at
  { "A",                    0x0041 },  // latin capital letter a
  { "B",                    0x0042 },  // latin capital letter b
  { "C",                    0x0043 },  // latin capital letter c
  { "D",                    0x0044 },  // latin capital letter d
  { "E",                    0x0045 },  // latin capital letter e
  { "F",                    0x0046 },  // latin capital letter f
  { "G",                    0x0047 },  // latin capital letter g
  { "H",                    0x0048 },  // latin capital letter h
  { "I",                    0x0049 },  // latin capital letter i
  { "J",                    0x004a },  // latin capital letter j
  { "K",                    0x004b },  // latin capital letter k
  { "L",                    0x004c },  // latin capital letter l
  { "M",                    0x004d },  // latin capital letter m
  { "N",                    0x004e },  // latin capital letter n
  { "O",                    0x004f },  // latin capital letter o
  { "P",                    0x0050 },  // latin capital letter p
  { "Q",                    0x0051 },  // latin capital letter q
  { "R",                    0x0052 },  // latin capital letter r
  { "S",                    0x0053 },  // latin capital letter s
  { "T",                    0x0054 },  // latin capital letter t
  { "U",                    0x0055 },  // latin capital letter u
  { "V",                    0x0056 },  // latin capital letter v
  { "W",                    0x0057 },  // latin capital letter w
  { "X",                    0x0058 },  // latin capital letter x
  { "Y",                    0x0059 },  // latin capital letter y
  { "Z",                    0x005a },  // latin capital letter z
  { "LeftSquare",           0x005b },  // left square bracket
  { "BackSlash",            0x005c },  // backslash
  { "RightSquare",          0x005d },  // right square bracket
  { "Exponent",             0x005e },  // circumflex accent
  { "Underscore",           0x005f },  // underline
  { "ReverseQuote",         0x0060 },  // grave accent
  { "a",                    0x0061 },  // latin small letter a
  { "b",                    0x0062 },  // latin small letter b
  { "c",                    0x0063 },  // latin small letter c
  { "d",                    0x0064 },  // latin small letter d
  { "e",                    0x0065 },  // latin small letter e
  { "f",                    0x0066 },  // latin small letter f
  { "g",                    0x0067 },  // latin small letter g
  { "h",                    0x0068 },  // latin small letter h
  { "i",                    0x0069 },  // latin small letter i
  { "j",                    0x006a },  // latin small letter j
  { "k",                    0x006b },  // latin small letter k
  { "l",                    0x006c },  // latin small letter l
  { "m",                    0x006d },  // latin small letter m
  { "n",                    0x006e },  // latin small letter n
  { "o",                    0x006f },  // latin small letter o
  { "p",                    0x0070 },  // latin small letter p
  { "q",                    0x0071 },  // latin small letter q
  { "r",                    0x0072 },  // latin small letter r
  { "s",                    0x0073 },  // latin small letter s
  { "t",                    0x0074 },  // latin small letter t
  { "u",                    0x0075 },  // latin small letter u
  { "v",                    0x0076 },  // latin small letter v
  { "w",                    0x0077 },  // latin small letter w
  { "x",                    0x0078 },  // latin small letter x
  { "y",                    0x0079 },  // latin small letter y
  { "z",                    0x007a },  // latin small letter z
  { "CurlyLeft",            0x007b },  // left curly bracket
  { "VerticalBar",          0x007c },  // vertical line
  { "CurlyRight",           0x007d },  // right curly bracket
  { "Tilde",                0x007e },  // tilde
  { "NoBreakSpace",         0x00a0 },  // no-break space
  { "InvertedExclamation",  0x00a1 },  // inverted exclamation mark
  { "Cent",                 0x00a2 },  // cent sign
  { "Sterling",             0x00a3 },  // pound sign
  { "Currency",             0x00a4 },  // currency sign
  { "Yen",                  0x00a5 },  // yen sign
  { "BrokenVerticalBar",    0x00a6 },  // broken bar
  { "Section",              0x00a7 },  // section sign
  { "Dieresis",             0x00a8 },  // dieresis
  { "Copyright",            0x00a9 },  // copyright sign
  { "OrdFeminine",          0x00aa },  // feminine ordinal indicator
  { "GuillemotLeft",        0x00ab },  // left guillemet (double arrow)
  { "LogicalNot",           0x00ac },  // not sign
  { "Hyphen",               0x00ad },  // soft hyphen
  { "Registered",           0x00ae },  // registered trade mark sign
  { "MacronOverline",       0x00af },  // macron, overline
  { "Degree",               0x00b0 },  // degree sign
  { "PlusMinus",            0x00b1 },  // plus-minus sign
  { "SuperscriptTwo",       0x00b2 },  // superscript two
  { "SuperscriptThree",     0x00b3 },  // superscript three
  { "Acute",                0x00b4 },  // acute accent
  { "Micro",                0x00b5 },  // micro sign
  { "Paragraph",            0x00b6 },  // paragraph sign
  { "CenteredPeriod",       0x00b7 },  // middle dot, kana conjoctive
  { "Cedilla",              0x00b8 },  // cedilla
  { "SuperscriptOne",       0x00b9 },  // superscript one
  { "OrdMasculine",         0x00ba },  // masculine ordinal indicator
  { "GuillemotRight",       0x00bb },  // right guillemet
  { "OneQuarter",           0x00bc },  // vulgar fraction one quarter
  { "OneHalf",              0x00bd },  // vulgar fraction one half
  { "ThreeQuarters",        0x00be },  // vulgar fraction three quarters
  { "InvertedQuestion",     0x00bf },  // inverted question mark
  { "AGrave",               0x00c0 },  // latin capital letter a with grave accent
  { "AAcute",               0x00c1 },  // latin capital letter a with acute accent
  { "ACircumflex",          0x00c2 },  // latin capital letter a with circumflex accent
  { "ATilde",               0x00c3 },  // latin capital letter a with tilde
  { "ADieresis",            0x00c4 },  // latin capital letter a with dieresis
  { "ARing",                0x00c5 },  // latin capital letter a with ring above
  { "AE",                   0x00c6 },  // latin capital letter a with e
  { "CCedilla",             0x00c7 },  // latin capital letter c with cedilla
  { "EGrave",               0x00c8 },  // latin capital letter e with grave accent
  { "EAcute",               0x00c9 },  // latin capital letter e with acute accent
  { "ECircumflex",          0x00ca },  // latin capital letter e with circumflex accent
  { "EDieresis",            0x00cb },  // latin capital letter e with dieresis
  { "IGrave",               0x00cc },  // latin capital letter i with grave accent
  { "IAcute",               0x00cd },  // latin capital letter i with acute accent
  { "ICircumflex",          0x00ce },  // latin capital letter i with circumflex accent
  { "IDieresis",            0x00cf },  // latin capital letter i with dieresis
  { "Eth",                  0x00d0 },  // latin capital letter eth
  { "NTilde",               0x00d1 },  // latin capital letter n with tilde
  { "OGrave",               0x00d2 },  // latin capital letter o with grave accent
  { "OAcute",               0x00d3 },  // latin capital letter o with acute accent
  { "OCircumflex",          0x00d4 },  // latin capital letter o with circumflex accent
  { "OTilde",               0x00d5 },  // latin capital letter o with tilde
  { "ODieresis",            0x00d6 },  // latin capital letter o with dieresis
  { "Multiply",             0x00d7 },  // multiplication sign
  { "OSlash",               0x00d8 },  // latin capital letter o with oblique stroke
  { "UGrave",               0x00d9 },  // latin capital letter u with grave accent
  { "UAcute",               0x00da },  // latin capital letter u with acute accent
  { "UCircumflex",          0x00db },  // latin capital letter u with circumflex accent
  { "UDieresis",            0x00dc },  // latin capital letter u with dieresis
  { "YAcute",               0x00dd },  // latin capital letter y with acute accent
  { "Thorn",                0x00de },  // latin capital letter thorn
  { "sSharp",               0x00df },  // latin small letter sharp s
  { "aGrave",               0x00e0 },  // latin small letter a with grave accent
  { "aAcute",               0x00e1 },  // latin small letter a with acute accent
  { "aCircumflex",          0x00e2 },  // latin small letter a with circumflex accent
  { "aTilde",               0x00e3 },  // latin small letter a with tilde
  { "aDieresis",            0x00e4 },  // latin small letter a with dieresis
  { "aRing",                0x00e5 },  // latin small letter a with ring above
  { "ae",                   0x00e6 },  // latin small letter a with e
  { "cCedilla",             0x00e7 },  // latin small letter c with cedilla
  { "eGrave",               0x00e8 },  // latin small letter e with grave accent
  { "eAcute",               0x00e9 },  // latin small letter e with acute accent
  { "eCircumflex",          0x00ea },  // latin small letter e with circumflex accent
  { "eDieresis",            0x00eb },  // latin small letter e with dieresis
  { "iGrave",               0x00ec },  // latin small letter i with grave accent
  { "iAcute",               0x00ed },  // latin small letter i with acute accent
  { "iCircumflex",          0x00ee },  // latin small letter i with circumflex accent
  { "iDieresis",            0x00ef },  // latin small letter i with dieresis
  { "eth",                  0x00f0 },  // latin small letter eth
  { "nTilde",               0x00f1 },  // latin small letter n with tilde
  { "oGrave",               0x00f2 },  // latin small letter o with grave accent
  { "oAcute",               0x00f3 },  // latin small letter o with acute accent
  { "oCircumflex",          0x00f4 },  // latin small letter o with circumflex accent
  { "oTilde",               0x00f5 },  // latin small letter o with tilde
  { "oDieresis",            0x00f6 },  // latin small letter o with dieresis
  { "Divide",               0x00f7 },  // division sign
  { "oSlash",               0x00f8 },  // latin small letter o with oblique stroke
  { "uGrave",               0x00f9 },  // latin small letter u with grave accent
  { "uAcute",               0x00fa },  // latin small letter u with acute accent
  { "uCircumflex",          0x00fb },  // latin small letter u with circumflex accent
  { "uDieresis",            0x00fc },  // latin small letter u with dieresis
  { "yAcute",               0x00fd },  // latin small letter y with acute accent
  { "thorn",                0x00fe },  // latin small letter thorn
  { "yDieresis",            0x00ff },  // latin small letter y with dieresis
  { "AMacron",              0x0100 },  // latin capital letter a with macron
  { "aMacron",              0x0101 },  // latin small letter a with macron
  { "ABreve",               0x0102 },  // latin capital letter a with breve
  { "aBreve",               0x0103 },  // latin small letter a with breve
  { "AOgonek",              0x0104 },  // latin capital letter a with ogonek
  { "aOgonek",              0x0105 },  // latin small letter a with ogonek
  { "CAcute",               0x0106 },  // latin capital letter c with acute accent
  { "cAcute",               0x0107 },  // latin small letter c with acute accent
  { "CCircumflex",          0x0108 },  // latin capital letter c with circumflex
  { "cCircumflex",          0x0109 },  // latin small letter c with circumflex
  { "CDotAccent",           0x010a },  // latin capital letter c with dot above
  { "cDotAccent",           0x010b },  // latin small letter c with dot above
  { "CCaron",               0x010c },  // latin capital letter c with caron
  { "cCaron",               0x010d },  // latin small letter c with caron
  { "DCaron",               0x010e },  // latin capital letter d with hacek
  { "dCaron",               0x010f },  // latin small letter d with hacek
  { "DCroat",               0x0110 },  // latin capital letter d with stroke
  { "dCroat",               0x0111 },  // latin small letter d with stroke
  { "EMacron",              0x0112 },  // latin capital letter e with macron
  { "eMacron",              0x0113 },  // latin small letter e with macron
  { "EBreve",               0x0114 },  // latin capital letter e with breve
  { "eBreve",               0x0115 },  // latin small letter e with breve
  { "EDotAccent",           0x0116 },  // latin capital letter e with dot above
  { "eDotAccent",           0x0117 },  // latin small letter e with dot above
  { "EOgonek",              0x0118 },  // latin capital letter e with ogenek
  { "eOgonek",              0x0119 },  // latin small letter e with ogenek
  { "ECaron",               0x011a },  // latin capital letter e with hacek
  { "eCaron",               0x011b },  // latin small letter e with hacek
  { "GCircumflex",          0x011c },  // latin capital letter g with circumflex
  { "gCircumflex",          0x011d },  // latin small letter g with circumflex
  { "GBreve",               0x011e },  // latin capital letter g with breve
  { "gBreve",               0x011f },  // latin small letter g with breve
  { "GDotAccent",           0x0120 },  // latin capital letter g with dot above
  { "gDotAccent",           0x0121 },  // latin small letter g with dot above
  { "GCedilla",             0x0122 },  // latin capital letter g with cedilla
  { "gCedilla",             0x0123 },  // latin small letter g with cedilla
  { "HCircumflex",          0x0124 },  // latin capital letter h with circumflex
  { "hCircumflex",          0x0125 },  // latin small letter h with circumflex
  { "HBar",                 0x0126 },  // latin capital letter h with stroke
  { "hBar",                 0x0127 },  // latin small letter h with stroke
  { "ITilde",               0x0128 },  // latin capital letter i with tilde
  { "iTilde",               0x0129 },  // latin small letter i with tilde
  { "IMacron",              0x012a },  // latin capital letter i with macron
  { "iMacron",              0x012b },  // latin small letter i with macron
  { "IBreve",               0x012c },  // latin capital letter i with breve
  { "iBreve",               0x012d },  // latin small letter i with breve
  { "IOgonek",              0x012e },  // latin capital letter i with ogonek
  { "iOgonek",              0x012f },  // latin small letter i with ogonek
  { "IDotAccent",           0x0130 },  // latin capital letter i with dot above
  { "iDotless",             0x0131 },  // latin small letter i without dot above
  { "IJ",                   0x0132 },  // latin capital ligature ij
  { "ij",                   0x0133 },  // latin small ligature ij
  { "JCircumflex",          0x0134 },  // latin capital letter j with circumflex
  { "jCircumflex",          0x0135 },  // latin small letter j with circumflex
  { "KCedilla",             0x0136 },  // latin capital letter k with cedilla
  { "kCedilla",             0x0137 },  // latin small letter k with cedilla
  { "kra",                  0x0138 },  // latin small letter kra
  { "LAcute",               0x0139 },  // latin capital letter l with acute accent
  { "lAcute",               0x013a },  // latin small letter l with acute accent
  { "LCedilla",             0x013b },  // latin capital letter l with cedilla
  { "lCedilla",             0x013c },  // latin small letter l with cedilla
  { "LCaron",               0x013d },  // latin capital letter l with hacek
  { "lCaron",               0x013e },  // latin small letter l with hacek
  { "LDot",                 0x013f },  // latin capital letter l with middle dot
  { "lDot",                 0x0140 },  // latin small letter l with middle dot
  { "LSlash",               0x0141 },  // latin capital letter l with stroke
  { "lSlash",               0x0142 },  // latin small letter l with stroke
  { "NAcute",               0x0143 },  // latin capital letter n with acute accent
  { "nAcute",               0x0144 },  // latin small letter n with acute accent
  { "NCedilla",             0x0145 },  // latin capital letter n with cedilla
  { "nCedilla",             0x0146 },  // latin small letter n with cedilla
  { "NCaron",               0x0147 },  // latin capital letter n with hacek
  { "nCaron",               0x0148 },  // latin small letter n with hacek
  { "nApostrophe",          0x0149 },  // latin small letter n preceded by apostrophe
  { "Eng",                  0x014a },  // latin capital letter eng
  { "eng",                  0x014b },  // latin small letter eng
  { "OMacron",              0x014c },  // latin capital letter o with macron
  { "oMacron",              0x014d },  // latin small letter o with macron
  { "OBreve",               0x014e },  // latin capital letter o with breve
  { "oBreve",               0x014f },  // latin small letter o with breve
  { "ODoubleAcute",         0x0150 },  // latin capital letter o with double acute accent
  { "oDoubleAcute",         0x0151 },  // latin small letter o with double acute accent
  { "OE",                   0x0152 },  // latin capital ligature o with e
  { "oe",                   0x0153 },  // latin small ligature o with e
  { "RAcute",               0x0154 },  // latin capital letter r with acute accent
  { "rAcute",               0x0155 },  // latin small letter r with acute accent
  { "RCedilla",             0x0156 },  // latin capital letter r with cedilla
  { "rCedilla",             0x0157 },  // latin small letter r with cedilla
  { "RCaron",               0x0158 },  // latin capital letter r with hacek
  { "rCaron",               0x0159 },  // latin small letter r with hacek
  { "SAcute",               0x015a },  // latin capital letter s with acute accent
  { "sAcute",               0x015b },  // latin small letter s with acute accent
  { "SCircumflex",          0x015c },  // latin capital letter s with circumflex
  { "sCircumflex",          0x015d },  // latin small letter s with circumflex
  { "SCedilla",             0x015e },  // latin capital letter s with cedilla
  { "sCedilla",             0x015f },  // latin small letter s with cedilla
  { "SCaron",               0x0160 },  // latin capital letter s with hacek
  { "sCaron",               0x0161 },  // latin small letter s with hacek
  { "TCedilla",             0x0162 },  // latin capital letter t with cedilla
  { "tCedilla",             0x0163 },  // latin small letter t with cedilla
  { "TCaron",               0x0164 },  // latin capital letter t with hacek
  { "tCaron",               0x0165 },  // latin small letter t with hacek
  { "TBar",                 0x0166 },  // latin capital letter t with stroke
  { "tBar",                 0x0167 },  // latin small letter t with stroke
  { "UTilde",               0x0168 },  // latin capital letter u with tilde
  { "uTilde",               0x0169 },  // latin small letter u with tilde
  { "UMacron",              0x016a },  // latin capital letter u with macron
  { "uMacron",              0x016b },  // latin small letter u with macron
  { "UBreve",               0x016c },  // latin capital letter u with breve
  { "uBreve",               0x016d },  // latin small letter u with breve
  { "URing",                0x016e },  // latin capital letter u with ring above
  { "uRing",                0x016f },  // latin small letter u with ring above
  { "UDoubleAcute",         0x0170 },  // latin capital letter u with double acute accent
  { "uDoubleAcute",         0x0171 },  // latin small letter u with double acute accent
  { "UOgonek",              0x0172 },  // latin capital letter u with ogonek
  { "uOgonek",              0x0173 },  // latin small letter u with ogonek
  { "WCircumflex",          0x0174 },  // latin capital letter w with circumflex
  { "wCircumflex",          0x0175 },  // latin cmall letter w with circumflex
  { "YCircumflex",          0x0176 },  // latin capital letter y with circumflex
  { "yCircumflex",          0x0177 },  // latin small letter y with circumflex
  { "YDieresis",            0x0178 },  // latin capital letter y with dieresis
  { "ZAcute",               0x0179 },  // latin capital letter z with acute accent
  { "zAcute",               0x017a },  // latin small letter z with acute accent
  { "ZDotAccent",           0x017b },  // latin capital letter z with dot above
  { "zDotAccent",           0x017c },  // latin small letter z with dot above
  { "ZCaron",               0x017d },  // latin capital letter z with hacek
  { "zCaron",               0x017e },  // latin small letter z with hacek
  { "sLong",                0x017f },  // latin small letter long s
  { "florin",               0x0192 },  // latin small letter script f,florin sign
  { "ARingAcute",           0x01fa },  // latin capital letter a with ring above and acute
  { "aRingAcute",           0x01fb },  // latin small letter a with ring above and acute
  { "aeAcute",              0x01fc },  // latin capital ligature ae with acute
  { "aeAcute",              0x01fd },  // latin small ligature ae with acute
  { "OSlashAcute",          0x01fe },  // latin capital letter o with stroke and acute
  { "oSlashAcute",          0x01ff },  // latin small letter o with stroke and acute
  // Non-spacing characters 0x2b0 - 0x2ff
  { "Circumflex",           0x02c6 },  // nonspacing circumflex accent
  { "Caron",                0x02c7 },  // modifier letter hacek
  { "Macron",               0x02c9 },  // modifier letter macron
  { "Breve",                0x02d8 },  // breve
  { "DotAccent",            0x02d9 },  // dot above
  { "Ring",                 0x02da },  // ring above
  { "Ogonek",               0x02db },  // ogonek
  { "NonSpacingTilde",      0x02dc },  // nonspacing tilde
  { "DoubleAcute",          0x02dd },  // modifier letter double prime
  // Combining characters 0x300 - 0x36f
  { "CombineGrave",         0x0300 },  /* combining grave accent */
  { "CombineAcute",         0x0301 },  /* combining acute accent */
  { "CombineCircumflex",    0x0302 },  /* combining circumflex accent */
  { "CombineTilde",         0x0303 },  /* combining tilde */
  { "CombineMacron",        0x0304 },  /* combining macron */
  { "CombineOverline",      0x0305 },  /* combining overline */
  { "CombineBreve",         0x0306 },  /* combining breve */
  { "CombineDotAccent",     0x0307 },  /* combining dot above */
  { "CombineDieresis",      0x0308 },  /* combining diaeresis */
  { "CombineHook",          0x0309 },  /* combining hook above */
  { "CombineRing",          0x030a },  /* combining ring above */
  { "CombineDblAcute",      0x030b },  /* combining double acute accent */
  { "CombineCaron",         0x030c },  /* combining caron */
  { "CombineVLineAbove",    0x030d },  /* combining vertical line above */
  { "CombineDblVLineAbove", 0x030e },  /* combining double vertical line above */
  { "CombineDblGrave",      0x030f },  /* combining double grave accent */
  { "CombineCandrabindu",   0x0310 },  /* combining candrabindu */
  { "CombineInvertedBreve", 0x0311 },  /* combining inverted breve */
  { "CombineTurnedComma",   0x0312 },  /* combining turned comma above */
  { "CombineComma",         0x0313 },  /* combining comma above */
  { "CombineReversedComma", 0x0314 },  /* combining reversed comma above */
  { "CombineCommaNE",       0x0315 },  /* combining comma above right */
  { "CombineGraveS",        0x0316 },  /* combining grave accent below */
  { "CombineAcuteS",        0x0316 },  /* combining acute accent below */
  { "CombineLTackS",        0x0317 },  /* combining left tack below */
  { "CombineRTackS",        0x0319 },  /* combining right tack below */
  { "CombineLAngle",        0x031a },  /* combining left angle above */
  { "CombineHorn",          0x031b },  /* combining horn */
  { "CombineLHalfRingS",    0x031c },  // combining left half ring below
  { "CombineCedilla",       0x0327 },  // combining cedilla
  { "CombineOgonek",        0x0328 },  /* combining odonek */
  // Greek characters 0x370+
  { "Tonos",                0x0384 },  // greek tonos
  { "DieresisTonos",        0x0385 },  // greek dialytika tonos
  { "AlphaTonos",           0x0386 },  // greek capital letter alpha with tonos
  { "AnoTeleia",            0x0387 },  // greek ano teleia
  { "EpsilonTonos",         0x0388 },  // greek capital letter epsilon with tonos
  { "EtaTonos",             0x0389 },  // greek capital letter eta with tonos
  { "IotaTonos",            0x038a },  // greek capital letter iota with tonos
  { "OmicronTonos",         0x038c },  // greek capital letter omicron with tonos
  { "UpsilonTonos",         0x038e },  // greek capital letter upsilon with tonos
  { "OmegaTonos",           0x038f },  // greek capital letter omega with tonos
  { "iotaDieresisTonos",    0x0390 },  // greek small letter iota with dialytika and tonos
  { "Alpha",                0x0391 },  // greek capital letter alpha
  { "Beta",                 0x0392 },  // greek capital letter beta
  { "Gamma",                0x0393 },  // greek capital letter gamma
  { "Delta",                0x0394 },  // greek capital letter delta
  { "Epsilon",              0x0395 },  // greek capital letter epsilon
  { "Zeta",                 0x0396 },  // greek capital letter zeta
  { "Eta",                  0x0397 },  // greek capital letter eta
  { "Theta",                0x0398 },  // greek capital letter theta
  { "Iota",                 0x0399 },  // greek capital letter iota
  { "Kappa",                0x039a },  // greek capital letter kappa
  { "Lambda",               0x039b },  // greek capital letter lamda
  { "Mu",                   0x039c },  // greek capital letter mu
  { "Nu",                   0x039d },  // greek capital letter nu
  { "Xi",                   0x039e },  // greek capital letter xi
  { "Omicron",              0x039f },  // greek capital letter omicron
  { "Pi",                   0x03a0 },  // greek capital letter pi
  { "Rho",                  0x03a1 },  // greek capital letter rho
  { "Sigma",                0x03a3 },  // greek capital letter sigma
  { "Tau",                  0x03a4 },  // greek capital letter tau
  { "Upsilon",              0x03a5 },  // greek capital letter upsilon
  { "Phi",                  0x03a6 },  // greek capital letter phi
  { "Chi",                  0x03a7 },  // greek capital letter chi
  { "Psi",                  0x03a8 },  // greek capital letter psi
  { "Omega",                0x03a9 },  // greek capital letter omega
  { "IotaDieresis",         0x03aa },  // greek capital letter iota with dialytika
  { "UpsilonDieresis",      0x03ab },  // greek capital letter upsilon with dialytika
  { "alphaTonos",           0x03ac },  // greek small letter alpha with tonos
  { "epsilonTonos",         0x03ad },  // greek small letter epsilon with tonos
  { "etaTonos",             0x03ae },  // greek small letter eta with tonos
  { "iotaTonos",            0x03af },  // greek small letter iota with tonos
  { "upsilonDieresisTonos", 0x03b0 },  // greek small letter upsilon with dialytika and tonos
  { "alpha",                0x03b1 },  // greek small letter alpha
  { "beta",                 0x03b2 },  // greek small letter beta
  { "gamma",                0x03b3 },  // greek small letter gamma
  { "delta",                0x03b4 },  // greek small letter delta
  { "epsilon",              0x03b5 },  // greek small letter epsilon
  { "zeta",                 0x03b6 },  // greek small letter zeta
  { "eta",                  0x03b7 },  // greek small letter eta
  { "theta",                0x03b8 },  // greek small letter theta
  { "iota",                 0x03b9 },  // greek small letter iota
  { "kappa",                0x03ba },  // greek small letter kappa
  { "lambda",               0x03bb },  // greek small letter lamda
  { "mu",                   0x03bc },  // greek small letter mu
  { "nu",                   0x03bd },  // greek small letter nu
  { "xi",                   0x03be },  // greek small letter xi
  { "omicron",              0x03bf },  // greek small letter omicron
  { "pi",                   0x03c0 },  // greek small letter pi
  { "rho",                  0x03c1 },  // greek small letter rho
  { "sigma1",               0x03c2 },  // greek small letter final sigma
  { "sigma",                0x03c3 },  // greek small letter sigma
  { "tau",                  0x03c4 },  // greek small letter tau
  { "upsilon",              0x03c5 },  // greek small letter upsilon
  { "phi",                  0x03c6 },  // greek small letter phi
  { "chi",                  0x03c7 },  // greek small letter chi
  { "psi",                  0x03c8 },  // greek small letter psi
  { "omega",                0x03c9 },  // greek small letter omega
  { "iotaDieresis",         0x03ca },  // greek small letter iota with dialytika
  { "upsilonDieresis",      0x03cb },  // greek small letter upsilon with dialytika
  { "omicronTonos",         0x03cc },  // greek small letter omicron with tonos
  { "upsilonTonos",         0x03cd },  // greek small letter upsilon with tonos
  { "omegaTonos",           0x03ce },  // greek small letter omega with tonos
  { "CyrillicIO",           0x0401 },  // cyrillic capital letter io
  { "CyrillicDJE",          0x0402 },  // cyrillic capital letter dje
  { "CyrillicGJE",          0x0403 },  // cyrillic capital letter gje
  { "CyrillicUkrainianIE",  0x0404 },  // cyrillic capital letter ukrainian ie
  { "CyrillicDZE",          0x0405 },  // cyrillic capital letter dze
  { "CyrillicUkrainianI",   0x0406 },  // cyrillic capital letter byelorussian-ukrainian i
  { "CyrillicYI",           0x0407 },  // cyrillic capital letter yi
  { "CyrillicJE",           0x0408 },  // cyrillic capital letter je
  { "CyrillicLJE",          0x0409 },  // cyrillic capital letter lje
  { "CyrillicNJE",          0x040a },  // cyrillic capital letter nje
  { "CyrillicTSHE",         0x040b },  // cyrillic capital letter tshe
  { "CyrillicKJE",          0x040c },  // cyrillic capital letter kje
  { "CyrillicShortU",       0x040e },  // cyrillic capital letter short u
  { "CyrillicDZHE",         0x040f },  // cyrillic capital letter dzhe
  { "CyrillicA",            0x0410 },  // cyrillic capital letter a
  { "CyrillicBE",           0x0411 },  // cyrillic capital letter be
  { "CyrillicVE",           0x0412 },  // cyrillic capital letter ve
  { "CyrillicGHE",          0x0413 },  // cyrillic capital letter ghe
  { "CyrillicDE",           0x0414 },  // cyrillic capital letter de
  { "CyrillicIE",           0x0415 },  // cyrillic capital letter ie
  { "CyrillicZHE",          0x0416 },  // cyrillic capital letter zhe
  { "CyrillicZE",           0x0417 },  // cyrillic capital letter ze
  { "CyrillicI",            0x0418 },  // cyrillic capital letter i
  { "CyrillicShortI",       0x0419 },  // cyrillic capital letter short i
  { "CyrillicKA",           0x041a },  // cyrillic capital letter ka
  { "CyrillicEL",           0x041b },  // cyrillic capital letter el
  { "CyrillicEM",           0x041c },  // cyrillic capital letter em
  { "CyrillicEN",           0x041d },  // cyrillic capital letter en
  { "CyrillicO",            0x041e },  // cyrillic capital letter o
  { "CyrillicPE",           0x041f },  // cyrillic capital letter pe
  { "CyrillicER",           0x0420 },  // cyrillic capital letter er
  { "CyrillicES",           0x0421 },  // cyrillic capital letter es
  { "CyrillicTE",           0x0422 },  // cyrillic capital letter te
  { "CyrillicU",            0x0423 },  // cyrillic capital letter u
  { "CyrillicEF",           0x0424 },  // cyrillic capital letter ef
  { "CyrillicHA",           0x0425 },  // cyrillic capital letter ha
  { "CyrillicTSE",          0x0426 },  // cyrillic capital letter tse
  { "CyrillicCHE",          0x0427 },  // cyrillic capital letter che
  { "CyrillicSHA",          0x0428 },  // cyrillic capital letter sha
  { "CyrillicSHCHA",        0x0429 },  // cyrillic capital letter shcha
  { "CyrillicHard",         0x042a },  // cyrillic capital letter hard sign
  { "CyrillicYERU",         0x042b },  // cyrillic capital letter yeru
  { "CyrillicSoft",         0x042c },  // cyrillic capital letter soft sign
  { "CyrillicE",            0x042d },  // cyrillic capital letter e
  { "CyrillicYU",           0x042e },  // cyrillic capital letter yu
  { "CyrillicYA",           0x042f },  // cyrillic capital letter ya
  { "Cyrillica",            0x0430 },  // cyrillic small letter a
  { "Cyrillicbe",           0x0431 },  // cyrillic small letter be
  { "Cyrillicve",           0x0432 },  // cyrillic small letter ve
  { "Cyrillicghe",          0x0433 },  // cyrillic small letter ghe
  { "Cyrillicde",           0x0434 },  // cyrillic small letter de
  { "Cyrillicie",           0x0435 },  // cyrillic small letter ie
  { "Cyrilliczhe",          0x0436 },  // cyrillic small letter zhe
  { "Cyrillicze",           0x0437 },  // cyrillic small letter ze
  { "Cyrillici",            0x0438 },  // cyrillic small letter i
  { "CyrillicShorti",       0x0439 },  // cyrillic small letter short i
  { "Cyrillicka",           0x043a },  // cyrillic small letter ka
  { "Cyrillicel",           0x043b },  // cyrillic small letter el
  { "Cyrillicem",           0x043c },  // cyrillic small letter em
  { "Cyrillicen",           0x043d },  // cyrillic small letter en
  { "Cyrillico",            0x043e },  // cyrillic small letter o
  { "Cyrillicpe",           0x043f },  // cyrillic small letter pe
  { "Cyrillicer",           0x0440 },  // cyrillic small letter er
  { "Cyrillices",           0x0441 },  // cyrillic small letter es
  { "Cyrillicte",           0x0442 },  // cyrillic small letter te
  { "Cyrillicu",            0x0443 },  // cyrillic small letter u
  { "Cyrillicef",           0x0444 },  // cyrillic small letter ef
  { "Cyrillicha",           0x0445 },  // cyrillic small letter ha
  { "Cyrillictse",          0x0446 },  // cyrillic small letter tse
  { "Cyrillicche",          0x0447 },  // cyrillic small letter che
  { "Cyrillicsha",          0x0448 },  // cyrillic small letter sha
  { "Cyrillicshcha",        0x0449 },  // cyrillic small letter shcha
  { "Cyrillichard",         0x044a },  // cyrillic small letter hard sign
  { "Cyrillicyeru",         0x044b },  // cyrillic small letter yeru
  { "Cyrillicsoft",         0x044c },  // cyrillic small letter soft sign
  { "Cyrillice",            0x044d },  // cyrillic small letter e
  { "Cyrillicyu",           0x044e },  // cyrillic small letter yu
  { "Cyrillicya",           0x044f },  // cyrillic small letter ya
  { "Cyrillicio",           0x0451 },  // cyrillic small letter io
  { "Cyrillicdje",          0x0452 },  // cyrillic small letter dje
  { "Cyrillicgje",          0x0453 },  // cyrillic small letter gje
  { "CyrillicUkrainianie",  0x0454 },  // cyrillic small letter ukrainian ie
  { "Cyrillicdze",          0x0455 },  // cyrillic small letter dze
  { "CyrillicUkrainiani",   0x0456 },  // cyrillic small letter byelorussian-ukrainian i
  { "Cyrillicyi",           0x0457 },  // cyrillic small letter yi
  { "Cyrillicje",           0x0458 },  // cyrillic small letter je
  { "Cyrilliclje",          0x0459 },  // cyrillic small letter lje
  { "Cyrillicnje",          0x045a },  // cyrillic small letter nje
  { "Cyrillictshe",         0x045b },  // cyrillic small letter tshe
  { "Cyrillickje",          0x045c },  // cyrillic small letter kje
  { "CyrillicShortu",       0x045e },  // cyrillic small letter short u
  { "Cyrillicdzhe",         0x045f },  // cyrillic small letter dzhe
  { "CyrillicUpturnGHE",    0x0490 },  // cyrillic capital letter ghe with upturn
  { "CyrillicUpturnghe",    0x0491 },  // cyrillic small letter ghe with upturn
  { "WGrave",               0x1e80 },  // latin capital letter w with grave
  { "wGrave",               0x1e81 },  // latin small letter w with grave
  { "WAcute",               0x1e82 },  // latin capital letter w with acute
  { "wAcute",               0x1e83 },  // latin small letter w with acute
  { "WDieresis",            0x1e84 },  // latin capital letter w with dieresis
  { "wDieresis",            0x1e85 },  // latin small letter w with dieresis
  { "YGrave",               0x1ef2 },  // latin capital letter y with grave
  { "yGrave",               0x1ef3 },  // latin small letter y with grave
  { "ENDash",               0x2013 },  // en dash
  { "EMDash",               0x2014 },  // em dash
  { "HorizontalBar",        0x2015 },  // horizontal bar
  { "DoubleUnderscore",     0x2017 },  // double low line
  { "QuoteLeft",            0x2018 },  // left single quotation mark
  { "QuoteRight",           0x2019 },  // right single quotation mark
  { "QuoteBase",            0x201a },  // single low-9 quotation mark
  { "QuoteReversed",        0x201b },  // single high-reversed-9 quotation mark
  { "DoubleQuoteLeft",      0x201c },  // left double quotation mark
  { "DoubleQuoteRight",     0x201d },  // right double quotation mark
  { "DoubleQuoteBase",      0x201e },  // double low-9 quotation mark
  { "Dagger",               0x2020 },  // dagger
  { "DoubleDagger",         0x2021 },  // double dagger
  { "Bullet",               0x2022 },  // bullet
  { "Ellipsis",             0x2026 },  // horizontal ellipsis
  { "PerThousand",          0x2030 },  // per mille sign
  { "Minute",               0x2032 },  // prime
  { "Second",               0x2033 },  // double prime
  { "AngleQuoteLeft",       0x2039 },  // single left-pointing angle quotation mark
  { "AngleQuoteRight",      0x203a },  // single right-pointing angle quotation mark
  { "DoubleExclamation",    0x203c },  // double exclamation mark
  { "Overline",             0x203e },  // overline
  { "Fraction",             0x2044 },  // fraction slash
  { "Superscriptn",         0x207f },  // superscript latin small letter n
  { "Franc",                0x20a3 },  // french franc sign
  { "Lira",                 0x20a4 },  // lira sign
  { "Peseta",               0x20a7 },  // peseta sign
  { "Euro",                 0x20ac },  // euro currency symbol
  { "CareOf",               0x2105 },  // care of
  { "Scriptl",              0x2113 },  // script small l
  { "Numero",               0x2116 },  // numero sign
  { "Trademark",            0x2122 },  // trademark sign
  { "Omega",                0x2126 },  // ohm sign
  { "Estimated",            0x212e },  // estimated symbol
  { "OneEighth",            0x215b },  // vulgar fraction one eighth
  { "ThreeEighths",         0x215c },  // vulgar fraction three eighths
  { "FiveEighths",          0x215d },  // vulgar fraction five eighths
  { "SevenEighths",         0x215e },  // vulgar fraction seven eighths
  { "LeftArrow",            0x2190 },  // leftwards arrow
  { "UpArrow",              0x2191 },  // upwards arrow
  { "RightArrow",           0x2192 },  // rightwards arrow
  { "DownArrow",            0x2193 },  // downwards arrow
  { "HorizontalArrow",      0x2194 },  // left right arrow
  { "VerticalArrow",        0x2195 },  // up down arrow
  { "VerticalArrowBase",    0x21a8 },  // up down arrow with base
  { "PartialDifferential",  0x2202 },  // partial differential
  { "Delta",                0x2206 },  // increment
  { "Product",              0x220f },  // n-ary product
  { "Summation",            0x2211 },  // n-ary summation
  { "Minus",                0x2212 },  // minus sign
  { "Fraction",             0x2215 },  // division slash
  { "BulletOperator",       0x2219 },  // bullet operator
  { "Radical",              0x221a },  // square root
  { "Infinity",             0x221e },  // infinity
  { "Orthogonal",           0x221f },  // right angle
  { "Intersection",         0x2229 },  // intersection
  { "Integral",             0x222b },  // integral
  { "AlmostEqual",          0x2248 },  // almost equal to
  { "NotEqual",             0x2260 },  // not equal to
  { "Equivalence",          0x2261 },  // identical to
  { "LessEqual",            0x2264 },  // less-than or equal to
  { "GreaterEqual",         0x2265 },  // greater-than or equal to
  { "House",                0x2302 },  // house
  { "Revlogicalnot",        0x2310 },  // reversed not sign
  { "IntegralTop",          0x2320 },  // top half integral
  { "IntegralBottom",       0x2321 },  // bottom half integral
  { "BoxLightH",            0x2500 },  // box drawings light horizontal
  { "BoxLightV",            0x2502 },  // box drawings light vertical
  { "BoxLightDownRight",    0x250c },  // box drawings light down and right
  { "BoxLightDownLeft",     0x2510 },  // box drawings light down and left
  { "BoxLightUpRight",      0x2514 },  // box drawings light up and right
  { "BoxLightUpLeft",       0x2518 },  // box drawings light up and left
  { "BoxLightVRight",       0x251c },  // box drawings light vertical and right
  { "BoxLightVLeft",        0x2524 },  // box drawings light vertical and left
  { "BoxLightDownH",        0x252c },  // box drawings light down and horizontal
  { "BoxLightUpH",          0x2534 },  // box drawings light up and horizontal
  { "BoxLightVH",           0x253c },  // box drawings light vertical and horizontal
  { "BoxDblH",              0x2550 },  // box drawings double horizontal
  { "BoxDblV",              0x2551 },  // box drawings double vertical
  { "BoxSglDownDblRight",   0x2552 },  // box drawings down single and right double
  { "BoxDblDownSglRight",   0x2553 },  // box drawings down double and right single
  { "BoxDblDownRight",      0x2554 },  // box drawings double down and right
  { "BoxSglDownLeftDbl",    0x2555 },  // box drawings down single and left double
  { "BoxDblDownLeftSgl",    0x2556 },  // box drawings down double and left single
  { "BoxDblDownLeft",       0x2557 },  // box drawings double down and left
  { "BoxSglUpDblRight",     0x2558 },  // box drawings up single and right double
  { "BoxDblUpSglRight",     0x2559 },  // box drawings up double and right single
  { "BoxDblUpRight",        0x255a },  // box drawings double up and right
  { "BoxSglUpDblLeft",      0x255b },  // box drawings up single and left double
  { "BoxDblUpSglLeft",      0x255c },  // box drawings up double and left single
  { "BoxDblUpLeft",         0x255d },  // box drawings double up and left
  { "BoxSglVDblRight",      0x255e },  // box drawings vertical single and right double
  { "BoxDblVSglRight",      0x255f },  // box drawings vertical double and right single
  { "BoxDblVRight",         0x2560 },  // box drawings double vertical and right
  { "BoxSglVDblLeft",       0x2561 },  // box drawings vertical single and left double
  { "BoxDblVSglLeft",       0x2562 },  // box drawings vertical double and left single
  { "BoxDblVLeft",          0x2563 },  // box drawings double vertical and left
  { "BoxSglDownDblH",       0x2564 },  // box drawings down single and horizontal double
  { "BoxDblDownSglH",       0x2565 },  // box drawings down double and horizontal single
  { "BoxDblDownH",          0x2566 },  // box drawings double down and horizontal
  { "BoxSglUpDblH",         0x2567 },  // box drawings up single and horizontal double
  { "BoxDblUpSglH",         0x2568 },  // box drawings up double and horizontal single
  { "BoxDblUpH",            0x2569 },  // box drawings double up and horizontal
  { "BoxSglVDblH",          0x256a },  // box drawings vertical single and horizontal double
  { "BoxDblVSglH",          0x256b },  // box drawings vertical double and horizontal single
  { "BoxDblVH",             0x256c },  // box drawings double vertical and horizontal
  { "UpperBlock",           0x2580 },  // Upper half block
  { "LowerBlock",           0x2584 },  // Lower half block
  { "Block",                0x2588 },  // Full block
  { "LeftBlock",            0x258c },  // Left half block
  { "RightBlock",           0x2590 },  // Right half block
  { "LightShade",           0x2591 },  // Light shade
  { "MediumShade",          0x2592 },  // Medium shade
  { "DarkShade",            0x2593 },  // Dark shade
  { "FilledSquare",         0x25a0 },  // Filled square
  { "EmptySquare",          0x25a1 },  // Unfilled square
  { "FilledSmallSquare",    0x25aa },  // Filled small square
  { "EmptySmallSquare",     0x25ab },  // Unfilled small square
  { "FilledRectangle",      0x25ac },  // Black rectangle
  { "TriangleUp",           0x25b2 },  // Black up-pointing triangle
  { "TriangleRight",        0x25ba },  // Black right-pointing pointer
  { "TriangleDown",         0x25bc },  // Black down-pointing triangle
  { "TriangleLeft",         0x25c4 },  // Black left-pointing pointer
  { "Lozenge",              0x25ca },  // Lozenge
  { "EmptyCircle",          0x25cb },  // Unfilled circle
  { "FilledCircle",         0x25cf },  // Filled circle
  { "InverseBullet",        0x25d8 },  // Inverse bullet
  { "InverseCircle",        0x25d9 },  // Inverse white circle
  { "EmptyBullet",          0x25e6 },  // Unfilled bullet
  { "SmilyFace",            0x263a },  // White smiling face
  { "FilledSmilyFace",      0x263b },  // Black smiling face
  { "Sun",                  0x263c },  // White sun with rays
  { "Female",               0x2640 },  // Female sign
  { "Male",                 0x2642 },  // Male sign
  { "Spade",                0x2660 },  // Black spade suit
  { "Club",                 0x2663 },  // Black club suit
  { "Heart",                0x2665 },  // Black heart suit
  { "Diamond",              0x2666 },  // Black diamond suit
  { "MusicalNote",          0x266a },  // Eighth note
  { "MusicalNoteDbl",       0x266b },  // Beamed eighth notes
  { "fi",                   0xf001 },  // fi ligature
  { "fl",                   0xf002 },  // fl ligature
  { 0, 0 }
};

#endif // UNICODE_TABLE
