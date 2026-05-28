# -*- coding: utf-8 -*- #
# frozen_string_literal: true

# Rouge syntax highlighter for the Tiri scripting language.
# Tiri is a heavily modified Lua derivative built on LuaJIT.
#
# Usage with Asciidoctor:
#   asciidoctor -r ./tiri_lexer.rb -a source-highlighter=rouge document.adoc
#
# Then use [source,tiri] in your AsciiDoc files.

require 'rouge'
require 'set'

module Rouge
   module Lexers
      class Tiri < RegexLexer
         title 'Tiri'
         desc 'The Tiri scripting language (Lua derivative)'
         tag 'tiri'
         aliases 'tiri'
         filenames '*.tiri'
         mimetypes 'text/x-tiri'

         def self.detect?(text)
            return true if text =~ /\b(thunk|defer|global\s+function)\b/
         end

         # Built-in global functions (from lib_base.cpp and tiri_class.cpp)
         BUILTINS = Set.new %w(
            assert type pairs ipairs values keys next
            tonumber tostring print error
            rawget rawset rawequal rawlen
            getmetatable setmetatable
            newproxy select
            isthunk resolve
            arg loadFile exec require load
            collectgarbage ltr
         )

         # Standard library namespaces
         MODULES = Set.new %w(
            string table math debug io bit jit
            range array obj
            processing regex struct thread input num mod
         )

         state :root do
            rule %r(#!(.*?)$), Comment::Preproc
            rule %r//, Text, :base
         end

         state :base do
            # Annotations: @Test, @BeforeEach(hotpath=true), @if, @end
            rule %r/@(?:if|end)\b/, Comment::Preproc
            rule %r/@[A-Za-z_]\w*/, Name::Decorator

            # Comments
            rule %r(--\[(=*)\[.*?\]\1\])m, Comment::Multiline
            rule %r(--.*$), Comment::Single

            # F-strings (must precede normal strings and identifiers)
            rule %r/f"/, Str::Double, :fstring_dq
            rule %r/f'/, Str::Single, :fstring_sq

            # Numbers
            rule %r((?i)(\d*\.\d+|\d+\.\d*)(e[+-]?\d+)?), Num::Float
            rule %r((?i)\d+e[+-]?\d+), Num::Float
            rule %r((?i)0x[0-9a-f]+), Num::Hex
            rule %r(\d+), Num::Integer

            # Whitespace
            rule %r(\n), Text
            rule %r([^\S\n]+), Text

            # Long strings [[ ... ]] and [=[ ... ]=]
            rule %r(\[(=*)\[.*?\]\1\])m, Str

            # Unicode operators (must precede ASCII equivalents)
            rule %r(⧺), Operator        # increment (++)
            rule %r(⁇), Operator        # null coalescing (??)
            rule %r(≠), Operator        # not equal (!=)
            rule %r(≤), Operator        # less or equal (<=)
            rule %r(≥), Operator        # greater or equal (>=)
            rule %r(«), Operator        # left shift (<<)
            rule %r(»), Operator        # right shift (>>)
            rule %r(‥), Operator        # concatenation (..)
            rule %r(…), Operator        # varargs (...)
            rule %r(▷), Operator        # ternary separator (:>)
            rule %r(×), Operator        # multiplication (*)
            rule %r(÷), Operator        # division (/)
            rule %r(↑), Operator        # exponentiation (**)

            # Multi-character operators (longest match first)
            rule %r(\?\?=), Operator
            rule %r(\?\?), Operator
            rule %r(\?=), Operator
            rule %r(\?\.), Operator
            rule %r(\?\[), Operator
            rule %r(\?\:), Operator
            rule %r(=>), Operator
            rule %r(:>), Operator
            rule %r(\|>), Operator
            rule %r(\+\+), Operator
            rule %r(\*\*), Operator
            rule %r(<<), Operator
            rule %r(>>), Operator
            rule %r(\.\.=), Operator
            rule %r(\.\.\.), Operator
            rule %r(\.\.(?!\.)), Operator
            rule %r(\+=), Operator
            rule %r(-=), Operator
            rule %r(\*=), Operator
            rule %r(/=), Operator
            rule %r(%=), Operator
            rule %r(==|!=|~=|<=|>=), Operator
            rule %r(<\{), Operator
            rule %r(\}>), Operator
            rule %r([=+\-*/%^<>#~&|!?]), Operator

            # Punctuation
            rule %r([\[\]\{\}\(\)\.,;:]), Punctuation

            # All keywords and identifiers via a single rule to avoid ordering conflicts.
            # Rouge matches rules top-to-bottom; using one identifier rule with a block
            # ensures keywords are never accidentally consumed as plain names.
            rule %r([A-Za-z_]\w*) do |m|
               name = m[0]
               case name
               when 'and', 'or', 'not', 'is', 'in'
                  token Operator::Word
               when 'true', 'false', 'nil'
                  token Keyword::Constant
               when 'function', 'thunk'
                  token Keyword::Declaration
                  push :function_name
               when 'local', 'global'
                  token Keyword::Declaration
                  push :declaration_context
               when 'break', 'continue', 'do', 'else', 'elseif', 'end',
                    'for', 'if', 'repeat', 'return', 'then', 'until', 'while',
                    'try', 'except', 'when', 'success', 'raise', 'check', 'defer',
                    'as', 'from', 'import', 'namespace', 'with', 'choose'
                  token Keyword
               else
                  if BUILTINS.include?(name) || MODULES.include?(name)
                     token Name::Builtin
                  else
                     token Name
                  end
               end
            end

            # Strings
            rule %r('), Str::Single, :sqs
            rule %r("), Str::Double, :dqs
         end

         # After 'function' or 'thunk': highlight the function name
         state :function_name do
            rule %r/\s+/, Text
            rule %r(([A-Za-z_]\w*)(\.)) do
               groups Name::Class, Punctuation
            end
            rule %r(([A-Za-z_]\w*)(:)([A-Za-z_]\w*)) do
               groups Name::Class, Punctuation, Name::Function
               pop!
            end
            rule %r([A-Za-z_]\w*), Name::Function, :pop!
            rule %r(\(), Punctuation, :pop!
         end

         # After 'local' or 'global': check for 'function' or 'thunk'
         state :declaration_context do
            rule %r/\s+/, Text
            rule %r/(function|thunk)\b/ do |m|
               token Keyword::Declaration
               goto :function_name
            end
            rule %r//, Text, :pop!
         end

         # Single-quoted strings
         state :sqs do
            mixin :string_escape
            rule %r(\\'), Str::Escape
            rule %r('), Str::Single, :pop!
            rule %r([^'\\]+), Str::Single
         end

         # Double-quoted strings
         state :dqs do
            mixin :string_escape
            rule %r(\\"), Str::Escape
            rule %r("), Str::Double, :pop!
            rule %r([^"\\]+), Str::Double
         end

         # F-string with double quotes: f"text {expr} text"
         state :fstring_dq do
            rule %r(\{\{), Str::Double
            rule %r(\{), Str::Interpol, :fstring_interp
            rule %r(\\"), Str::Escape
            rule %r("), Str::Double, :pop!
            mixin :string_escape
            rule %r([^"\\{]+), Str::Double
         end

         # F-string with single quotes: f'text {expr} text'
         state :fstring_sq do
            rule %r(\{\{), Str::Single
            rule %r(\{), Str::Interpol, :fstring_interp
            rule %r(\\'), Str::Escape
            rule %r('), Str::Single, :pop!
            mixin :string_escape
            rule %r([^'\\{]+), Str::Single
         end

         # F-string interpolation: the Tiri expression inside { }
         state :fstring_interp do
            rule %r(\}\}), Str::Interpol
            rule %r(\}), Str::Interpol, :pop!
            mixin :base
         end

         # Common string escape sequences
         state :string_escape do
            rule %r(\\([abfnrtv\\"']|\d{1,3}|x[0-9a-fA-F]{2})), Str::Escape
         end
      end
   end
end
