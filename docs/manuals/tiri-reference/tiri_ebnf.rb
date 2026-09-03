# -*- coding: utf-8 -*- #
# frozen_string_literal: true

# Flat rendering for EBNF grammar blocks in the PDF backend.
#
# Asciidoctor PDF renders every listing and literal block with the 'code' theme
# category, so the grammar blocks in this manual inherit the left rule, the
# indent and the background of a Tiri code sample.  Grammar is notation rather
# than source, and the visual weight of a code block overstates it.  This
# extension keeps the monospaced 'code' font for those blocks but suppresses the
# decoration, leaving the text flush with the surrounding prose.
#
# A block opts in either by declaring 'ebnf' as its language:
#
#   [source,ebnf]
#   ----
#   block = { statement } ;
#   ----
#
# or by carrying the 'ebnf' role ([.ebnf] or role=ebnf) on a listing or literal
# block.  Rouge has no 'ebnf' lexer, so a [source,ebnf] block is highlighted as
# plain text and picks up no colouring from the source highlighter.
#
# Any 'ebnf' keys in the theme are applied over the 'code' category for the
# duration of the block, so the treatment can be tuned from tiri-theme.yml
# without editing this file.  For example:
#
#   ebnf:
#     font-family: Inconsolata
#     font-size: 0.9em
#
# Usage with Asciidoctor:
#   asciidoctor-pdf -r ./tiri_ebnf.rb document.adoc

require 'asciidoctor/pdf'

module Asciidoctor
   module PDF
      module TiriEbnfBlock
         # Theming that turns the decorated code block into plain preformatted text.
         # Suppressing both the background and the border width makes the converter
         # skip the fill-and-stroke step for the block entirely.
         FLAT_CODE_THEME = {
            'code_background_color' => nil,
            'code_border_width' => 0,
            'code_padding' => [0, 0, 0, 0],
         }.freeze

         def convert_code node
            return super unless ebnf_block? node

            overrides = FLAT_CODE_THEME.merge ebnf_theme_overrides
            saved = {}
            overrides.each_key {|key| saved[key] = @theme[key] }
            overrides.each {|key, value| @theme[key] = value }
            begin
               super
            ensure
               saved.each {|key, value| @theme[key] = value }
            end
         end

         private

         def ebnf_block? node
            (node.attr 'language') == 'ebnf' || (node.has_role? 'ebnf')
         end

         # Map the flattened 'ebnf_*' theme keys onto their 'code_*' counterparts.
         def ebnf_theme_overrides
            @ebnf_theme_overrides ||= @theme.each_pair.each_with_object({}) do |(key, value), accum|
               accum[%(code_#{(key.to_s.slice 5, key.length)})] = value if (key.to_s.start_with? 'ebnf_')
            end
         end
      end

      Converter.prepend TiriEbnfBlock
   end
end
