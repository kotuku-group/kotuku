// Keyword sets use the selected core's zero-based SCI_SETKEYWORDS API.
#include "tiri_keywords.h"

static constexpr char glCppKeywords[] =
   "alignas alignof and and_eq asm atomic_cancel atomic_commit atomic_noexcept auto bitand bitor bool break case "
   "catch char char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast "
   "continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit "
   "export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq "
   "nullptr operator or or_eq private protected public reflexpr register reinterpret_cast requires return short "
   "signed sizeof static static_assert static_cast struct switch synchronized template this thread_local throw "
   "true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq ";

static constexpr char glPythonKeywords[] =
   "False None True and as assert async await break class continue def del elif else except finally for from "
   "global if import in is lambda nonlocal not or pass raise return try while with yield ";

static constexpr char glVbKeywords[] =
   "addhandler addressof alias and andalso as boolean byref byte byval call case catch cbool cbyte cchar cdate "
   "cdbl cdec char cint class clng cobj const continue csbyte cshort csng cstr ctype cuint culng cushort date "
   "decimal declare default delegate dim directcast do double each else elseif end endif enum erase error event "
   "exit false finally for friend function get gettype global gosub goto handles if implements imports in "
   "inherits integer interface is isnot let lib like long loop me mod module mustinherit mustoverride mybase "
   "myclass namespace narrowing new next not nothing notinheritable notoverridable object of on operator option "
   "optional or orelse overloads overridable overrides paramarray partial private property protected public "
   "raiseevent readonly redim rem removehandler resume return select set shadows shared short single static step "
   "stop string structure sub synclock then throw to true try trycast typeof uinteger ulong ushort using variant "
   "wend when while widening with withevents writeonly xor ";

static constexpr char glPerlKeywords[] =
   "BEGIN END CORE __DATA__ __END__ __FILE__ __LINE__ __PACKAGE__ abs accept alarm and atan2 bind binmode bless "
   "caller chdir chmod chomp chop chown chr chroot close closedir cmp connect cos crypt dbmclose dbmopen defined "
   "delete die do dump each else elsif endgrent endhostent endnetent endprotoent endpwent endservent eof eq eval "
   "exec exists exit exp fcntl fileno flock for foreach fork format formline ge getc glob gmtime goto grep gt "
   "hex if index int ioctl join keys kill last lc lcfirst le length link listen local localtime log lt m map "
   "mkdir my ne next no not oct open opendir or ord our pack package pipe pop pos print printf prototype push q "
   "qq qr quotemeta qw qx rand read readdir readline readlink recv redo ref rename require reset return reverse "
   "rewinddir rindex rmdir s say scalar seek seekdir select send shift shutdown sin sleep socket sort splice "
   "split sprintf sqrt srand stat state study sub substr symlink syscall sysopen sysread sysseek system syswrite "
   "tell telldir tie time times tr truncate uc ucfirst umask undef unless unlink unpack unshift untie until use "
   "utime values vec wait waitpid wantarray warn while write x xor y ";

static constexpr char glSqlKeywords[] =
   "add all alter and any as asc authorization backup begin between bigint binary bit blob boolean both by call "
   "cascade case cast char character check close coalesce collate column commit constraint continue convert "
   "create cross current current_date current_time current_timestamp cursor database date day deallocate dec "
   "decimal declare default delete desc describe distinct do double drop else end escape except exec execute "
   "exists explain false fetch float for foreign from full function grant group having if in index inner insert "
   "int integer intersect interval into is join key language leading left like limit local lock merge national "
   "natural nchar no not null nullif numeric of offset on only open option or order outer over partition "
   "precision prepare primary procedure public real references rename replace restrict return returns revoke "
   "right rollback row rows schema select session set smallint some table temporary then time timestamp to "
   "trailing transaction trigger true truncate union unique unknown update upper use user using values varchar "
   "varying view when where while with ";

static constexpr char glBatchKeywords[] =
   "assoc at attrib break call cd chcp chdir choice cls cmd color comp compact copy date del dir diskcomp "
   "diskcopy doskey echo endlocal erase exit fc find findstr for format ftype goto help if label md mkdir mklink "
   "mode more move net path pause popd print prompt pushd rd rem ren rename replace rmdir robocopy set setlocal "
   "shift sort start subst time title tree type ver verify vol xcopy ";

static constexpr char glPascalKeywords[] =
   "absolute abstract and array as asm assembler begin case cdecl class const constructor contains default "
   "destructor dispinterface div do downto dynamic else end except export exports external far file final "
   "finalization finally for forward function goto if implementation in inherited initialization inline "
   "interface is label library message mod name near nil nodefault not object of on operator or out overload "
   "override packed pascal private procedure program property protected public published raise read readonly "
   "record register reintroduce repeat requires resourcestring safecall sealed set shl shr static stdcall stored "
   "strict string then threadvar to try type unit unsafe until uses var virtual while with write writeonly xor ";

static constexpr char glRubyKeywords[] =
   "BEGIN END alias and begin break case class def defined? do else elsif end ensure false for if in module next "
   "nil not or redo rescue retry return self super then true undef unless until when while yield __FILE__ "
   "__LINE__ __ENCODING__ ";

static constexpr char glBashKeywords[] =
   "alias bg bind break builtin caller case cd command compgen complete continue declare dirs disown do done "
   "echo elif else enable esac eval exec exit export false fc fg fi for function getopts hash help history if in "
   "jobs kill let local logout mapfile popd printf pushd pwd read readonly return select set shift shopt source "
   "test then time times trap true type typeset ulimit umask unalias unset until wait while ";

static constexpr char glRebolKeywords[] =
   "about action alias all also alter and any any-block? any-function? any-path? any-string? any-word? append "
   "apply array as-pair ask at attempt back bind block? break browse case catch change charset checksum "
   "clean-path clear close collect comment compose confirm construct context continue copy create datatype? "
   "debase decode dehex delete difference dir? do does either else empty? enbase encode enhex equal? error? "
   "even? event? exclude exists? exit exp extract false fifth file? find first for forall foreach forever form "
   "fourth free func function get halt has head help if import in index? insert integer? intersect join last "
   "length? load loop make map! maximum minimum mold native negate next none not now object? odd? open or parse "
   "path? pick poke prin print probe protect query quit random read rebol recycle reduce rejoin remainder remove "
   "rename repeat replace request return reverse round save second select series? set skip sort split "
   "square-root strict-equal? string? switch tail third throw to trace trim true try type? union unique unless "
   "unprotect unset until update use value? values wait while word? write xor yes ";

static constexpr char glJavascriptKeywords[] =
   "async await break case catch class const continue debugger default delete do else enum export extends false "
   "finally for from function get if implements import in instanceof interface let new null of package private "
   "protected public return set static super switch this throw true try typeof undefined var void while with "
   "yield ";

static constexpr char glPhpKeywords[] =
   "abstract and array as bool break callable case catch class clone const continue declare default die do echo "
   "else elseif empty enddeclare endfor endforeach endif endswitch endwhile enum eval exit extends false final "
   "finally float fn for foreach function global goto if implements include include_once instanceof insteadof "
   "int interface isset list match namespace new null object or print private protected public readonly require "
   "require_once return static string switch throw trait true try unset use var void while xor yield ";

static constexpr char glHtmlKeywords[] =
   "a abbr address area article aside audio b base bdi bdo blockquote body br button canvas caption cite code "
   "col colgroup data datalist dd del details dfn dialog div dl dt em embed fieldset figcaption figure footer "
   "form h1 h2 h3 h4 h5 h6 head header hgroup hr html i iframe img input ins kbd label legend li link main map "
   "mark menu meta meter nav noscript object ol optgroup option output p picture pre progress q rp rt ruby s "
   "samp script search section select slot small source span strong style sub summary sup table tbody td "
   "template textarea tfoot th thead time title tr track u ul var video wbr accept action alt async charset "
   "checked class cols colspan content controls data defer disabled download enctype for height hidden href "
   "http-equiv id lang method multiple name placeholder readonly rel required role rows rowspan selected src "
   "style target title type value width xmlns ";

void ScintillaKTK::SetKeywords()
{
   // A lexer switch can reuse an existing lexer instance.  Clear all sets before applying the new profile.
   for (int list=0; list < 9; list++) SendScintilla(SCI_SETKEYWORDS, list, "");

   switch (scintilla->Lexer) {
      case SCLEX::CPP:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glCppKeywords);
         break;

      case SCLEX::TIRI:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glTiriKeywords);
         SendScintilla(SCI_SETKEYWORDS, 1,
            "assert arg check collectgarbage exec forEach include ipairs loadFile module pairs print processing "
            "rawequal rawlen rawtype require subscribe subscribeEvent tonumber tostring type unsubscribe "
            "unsubscribeEvent values");
         break;

      case SCLEX::PYTHON:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glPythonKeywords);
         break;

      case SCLEX::PERL:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glPerlKeywords);
         break;

      case SCLEX::SQL:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glSqlKeywords);
         break;

      case SCLEX::VB:
      case SCLEX::VBSCRIPT:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glVbKeywords);
         break;

      case SCLEX::BATCH:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glBatchKeywords);
         break;

      case SCLEX::PASCAL:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glPascalKeywords);
         break;

      case SCLEX::RUBY:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glRubyKeywords);
         break;

      case SCLEX::BASH:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glBashKeywords);
         break;

      case SCLEX::REBOL:
         SendScintilla(SCI_SETKEYWORDS, 0UL, glRebolKeywords);
         break;

      case SCLEX::HTML:
      case SCLEX::XML:
      case SCLEX::ASP:
      case SCLEX::PHPSCRIPT:
         // An empty XML element list accepts arbitrary application-specific tags and attributes.
         if (scintilla->Lexer != SCLEX::XML) SendScintilla(SCI_SETKEYWORDS, 0UL, glHtmlKeywords);
         SendScintilla(SCI_SETKEYWORDS, 1, glJavascriptKeywords);
         SendScintilla(SCI_SETKEYWORDS, 2, glVbKeywords);
         SendScintilla(SCI_SETKEYWORDS, 3, glPythonKeywords);
         SendScintilla(SCI_SETKEYWORDS, 4, glPhpKeywords);
         SendScintilla(SCI_SETKEYWORDS, 5,
            "doctype element attlist entity notation pcdata cdata implied required fixed");
         break;

      case SCLEX::ASSEMBLER:
         SendScintilla(SCI_SETKEYWORDS, 0UL,
            "adc add and b bl call cbz cmp dec div eor idiv imul inc int jmp je jne jz jnz lea ldr ldp mov mul "
            "nop not or pop push ret rol ror sal sar sbb shl shr stp str sub svc test xchg xor");
         SendScintilla(SCI_SETKEYWORDS, 1, "fadd fdiv fld fmul fst fstp fsub");
         SendScintilla(SCI_SETKEYWORDS, 2,
            "al ah ax bl bh bx cl ch cx dl dh dx eax ebx ecx edx esi edi ebp esp rax rbx rcx rdx rsi rdi rbp rsp "
            "r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 r13 r14 r15 x0 x1 x2 x3 x4 x5 x6 x7 x8 sp lr pc");
         SendScintilla(SCI_SETKEYWORDS, 3, "align bits byte db dd dq dw equ extern global section segment word");
         SendScintilla(SCI_SETKEYWORDS, 4, "byte dword far near ptr qword short word");
         SendScintilla(SCI_SETKEYWORDS, 5, "addps addsd addss movaps movdqa movdqu movsd movss pxor subps xorps");
         break;

      case SCLEX::CSS:
         SendScintilla(SCI_SETKEYWORDS, 0UL,
            "background background-color background-image border border-color border-style border-width bottom "
            "clear color display float font font-family font-size font-style font-weight height left line-height "
            "list-style margin margin-bottom margin-left margin-right margin-top padding position right text-align "
            "text-decoration text-indent top vertical-align white-space width word-spacing");
         SendScintilla(SCI_SETKEYWORDS, 1,
            "active checked disabled empty enabled first-child first-of-type focus focus-visible focus-within "
            "hover is lang last-child last-of-type link not nth-child nth-of-type only-child root target visited "
            "where");
         SendScintilla(SCI_SETKEYWORDS, 2,
            "border-collapse border-spacing caption-side content cursor max-height max-width min-height min-width "
            "outline overflow table-layout visibility z-index");
         SendScintilla(SCI_SETKEYWORDS, 3,
            "align-items animation appearance aspect-ratio border-radius box-shadow box-sizing column-gap "
            "flex flex-basis flex-direction flex-grow flex-shrink flex-wrap gap grid grid-template-columns "
            "grid-template-rows justify-content object-fit opacity order overflow-x overflow-y row-gap "
            "text-overflow transform transition user-select");
         SendScintilla(SCI_SETKEYWORDS, 4, "after before first-letter first-line marker placeholder selection");
         break;

      default: break; // Structural lexers such as Diff, Properties and Makefile do not consume keyword lists.
   }
}
