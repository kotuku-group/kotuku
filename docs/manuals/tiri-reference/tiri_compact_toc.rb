# frozen_string_literal: true

# Compact (run-in) table of contents for Asciidoctor PDF.
#
# The stock converter inks one TOC entry per line.  This extension renders entries at and below a configured
# level as a flowing run of items packed several to a line, which collapses a deep table of contents to a
# fraction of its original length.
#
# Entries promoted to run-in form truncate the tree: their own children are not listed.  Levels above the
# threshold are inked by the stock converter, dot leaders and all.
#
# Levels are counted the way the stock converter counts them, i.e. section level plus one.  In a book with
# parts, parts are level 1, chapters are level 2, `===` sections are level 3 and `====` sections are level 4.
#
# Two styles are available.  `entry` gives every item its own page number:
#
#     8.3.1. string.alloc() 93 · 8.3.2. string.byte() 93 · 8.3.3. string.char() 94
#     8.3.4. string.format() 94 · 8.3.5. string.len() 94
#
# `range` drops the page number from each item and closes the group with a dot leader and the page range that
# the group spans:
#
#     string.alloc(), string.byte(), string.char(), string.format(),
#     string.len(), string.rep() . . . . . . . . . . . . . . . . . . 93 - 95
#
# Enable and configure through the PDF theme:
#
#   toc:
#     run-in:
#       level: 4                        # first level rendered run-in; 0 or absent disables the extension
#       style: range                    # 'range' or 'entry' (default)
#       numbered: false                 # include section numbers; defaults to false for range, true for entry
#       separator-content: ', '         # drawn between items on the same line, never at a line break
#       separator-font-color: 999999
#       pagenum-font-color: 777777
#       range-separator-content: ' - '  # drawn between the first and last page of a range
#       dot-leader: true                # draw a dot leader before the page range (range style only)
#
# The dot leader itself takes its glyph, colour and size from the stock `toc: dot-leader:` keys.
#
# Load alongside the document, e.g.
#
#   asciidoctor-pdf -r ./tiri_compact_toc.rb book.adoc

require 'asciidoctor/pdf' unless defined? ::Asciidoctor::PDF::Converter

module Tiri
  module CompactToc
    LF = ?\n
    NoBreakSpace = ?\u00a0
    DefaultSeparator = '  ·  '
    DefaultRangeSeparator = ' - '

    # Diverts sibling groups at or below the configured level to the run-in renderer.  All entries in a group
    # share a level, so the first entry decides for the group.
    def ink_toc_level entries, num_levels, dot_leader, num_front_matter_pages
      if (run_in_level = (@theme.toc_run_in_level || 0).to_i) > 0 && !entries.empty? &&
          (entries[0].level + 1) >= run_in_level
        ink_toc_run_in entries, num_levels, dot_leader, num_front_matter_pages
      else
        super
      end
    end

    private

    def ink_toc_run_in entries, num_levels, dot_leader, num_front_matter_pages
      entry_level = entries[0].level + 1
      range_style = (@theme.toc_run_in_style || 'entry').to_s == 'range'
      theme_font :toc, level: entry_level do
        numbered = @theme.toc_run_in_numbered
        numbered = !range_style if numbered.nil?
        inherited = (apply_text_decoration ::Set.new, :toc, entry_level).merge color: @font_color

        items = entries.filter_map do |entry|
          next if (entry.attr 'toclevels', num_levels).to_i < entry_level.pred
          next if (entry.option? 'notitle') && entry == entry.document.last_child && entry.empty?
          title = if entry.context == :section
            numbered ? entry.numbered_title : entry.title
          else
            entry.title? ? entry.title : (entry.xreftext 'basic')
          end
          next if title.empty?
          title = title.gsub ::Asciidoctor::PDF::Converter::DropAnchorRx, '' if title.include? '<a'
          title = transform_text title, @text_transform if @text_transform
          anchor = (entry.attr 'pdf-anchor') || entry.id
          fragments = text_formatter.format title, inherited: (inherited.merge anchor: anchor)
          {
            fragments: fragments,
            anchor: anchor,
            physical_pgnum: (resolve_run_in_pgnum entry, anchor),
            title_width: (width_of_unwrapped_fragments fragments),
          }
        end

        unless items.empty?
          if range_style
            ink_run_in_range items, dot_leader, num_front_matter_pages
          else
            ink_run_in_entries items, num_front_matter_pages
          end
        end
      end
      nil
    end

    # Inks a run-in group in which every item carries its own page number.
    #
    # Line breaking is computed here rather than left to Prawn so that a separator never lands at the end of a
    # line.  Each item reserves the same page number width that the stock converter reserves
    # (toc-max-pagenum-digits), which makes the packing identical during the dry run that allocates TOC pages
    # and the real run that inks them.  The line count therefore matches in both passes.
    def ink_run_in_entries items, num_front_matter_pages
      separator = @theme.toc_run_in_separator_content || DefaultSeparator
      separator_color = @theme.toc_run_in_separator_font_color || @font_color
      separator_width = rendered_width_of_string separator
      gap_width = rendered_width_of_char NoBreakSpace
      pgnum_color = @theme.toc_run_in_pagenum_font_color || @font_color
      pgnum_slot_width = rendered_width_of_string '0' * @toc_max_pagenum_digits

      # A separator that opens with punctuation, such as ', ', belongs to the item it follows and is retained
      # when a line breaks after it.  A separator that opens with a space, such as ' \u00b7 ', is dropped.
      separator_tail = separator[/\A\S*/]
      separator_tail_width = separator_tail.empty? ? 0 : (rendered_width_of_string separator_tail)
      content_width = bounds.width - separator_tail_width

      items.each {|item| item[:width] = item[:title_width] + gap_width + pgnum_slot_width }
      lines = pack_run_in_items items, separator_width, content_width

      fragments = []
      lines.each_with_index do |packed_line, line_idx|
        packed_line.each_with_index do |item, item_idx|
          fragments << { text: separator, color: separator_color } if item_idx > 0
          fragments.concat item[:fragments]
          unless scratch? || !(pgnum_label = pgnum_label_for item[:physical_pgnum], num_front_matter_pages)
            fragments << { text: NoBreakSpace }
            fragments << { text: pgnum_label, anchor: item[:anchor], color: pgnum_color }
          end
        end
        unless line_idx == lines.size - 1
          fragments << { text: separator_tail, color: separator_color } unless separator_tail.empty?
          fragments << { text: LF }
        end
      end
      typeset_formatted_text fragments, (calc_line_metrics @base_line_height)
    end

    # Inks a run-in group as a comma-separated list closed by a dot leader and the page range the group spans.
    #
    # Room for the leader and the range label is reserved on the final line at packing time, using a label
    # width derived from toc-max-pagenum-digits so that packing is identical in the dry run and the real run.
    def ink_run_in_range items, dot_leader, num_front_matter_pages
      separator = @theme.toc_run_in_separator_content || DefaultSeparator
      separator_color = @theme.toc_run_in_separator_font_color || @font_color
      separator_width = rendered_width_of_string separator
      range_separator = @theme.toc_run_in_range_separator_content || DefaultRangeSeparator
      pgnum_color = @theme.toc_run_in_pagenum_font_color || @font_color
      hanging_indent = @theme.toc_hanging_indent.to_f
      draw_leader = @theme.toc_run_in_dot_leader != false && dot_leader[:width] > 0
      toc_font = font

      # Widest label the group can produce, e.g. '000 - 000'.  Reserved in both passes.
      digits = '0' * @toc_max_pagenum_digits
      range_slot_width = rendered_width_of_string %(#{digits}#{range_separator}#{digits})
      tail_width = hanging_indent + dot_leader[:spacer_width] + range_slot_width
      tail_width += dot_leader[:width] * 3 if draw_leader

      # A separator that opens with punctuation, such as ', ', belongs to the item it follows and is retained
      # when a line breaks after it.  A separator that opens with a space, such as ' \u00b7 ', is dropped.
      separator_tail = separator[/\A\S*/]
      separator_tail_width = separator_tail.empty? ? 0 : (rendered_width_of_string separator_tail)
      content_width = bounds.width - separator_tail_width

      items.each {|item| item[:width] = item[:title_width] }
      lines = pack_run_in_items items, separator_width, content_width
      # Give the leader and range label room on the final line, breaking a new line for them if necessary.
      last_line_width = line_width_of lines[-1], separator_width
      if last_line_width + tail_width > content_width
        if lines[-1].size > 1
          lines << [lines[-1].pop]
          leader_on_own_line = false
        else
          leader_on_own_line = true
        end
      end

      fragments = []
      fragment_positions = []
      lines.each_with_index do |packed_line, line_idx|
        packed_line.each_with_index do |item, item_idx|
          fragments << { text: separator, color: separator_color } if item_idx > 0
          item[:fragments].each do |fragment|
            fragment_positions << (fragment_position = ::Asciidoctor::PDF::FormattedText::FragmentPositionRenderer.new)
            (fragment[:callback] ||= []) << fragment_position
            fragments << fragment
          end
        end
        unless line_idx == lines.size - 1
          fragments << { text: separator_tail, color: separator_color } unless separator_tail.empty?
          fragments << { text: LF }
        end
      end

      line_metrics = calc_line_metrics @base_line_height
      start_page_number = page_number
      start_cursor = cursor
      typeset_formatted_text fragments, line_metrics

      # The dry run inks the widest label the group can produce rather than skipping the leader, so that a
      # leader forced onto a line of its own consumes the same vertical space in both passes.
      pgnums = items.filter_map {|item| item[:physical_pgnum] }
      range_label = if scratch?
        %(#{digits}#{range_separator}#{digits})
      elsif pgnums.empty?
        '?'
      elsif (from_label = pgnum_label_for pgnums.min, num_front_matter_pages) ==
          (to_label = pgnum_label_for pgnums.max, num_front_matter_pages)
        from_label
      else
        %(#{from_label}#{range_separator}#{to_label})
      end
      range_anchor = items[0][:anchor]

      if leader_on_own_line
        start_dots = 0
      elsif (last_position = fragment_positions.select(&:page_number)[-1])
        start_dots = last_position.right + hanging_indent
        last_position_cursor = last_position.top + line_metrics.padding_top
        if last_position.page_number > start_page_number || (start_cursor - last_position_cursor) > line_metrics.height
          start_cursor = last_position_cursor
        end
      else
        return
      end

      end_cursor = cursor
      move_cursor_to start_cursor unless leader_on_own_line
      range_label_width = rendered_width_of_string range_label
      range_font_settings = { color: pgnum_color, font: font_family, size: @font_size, styles: font_styles }
      if draw_leader
        save_font do
          set_font toc_font, dot_leader[:font_size]
          font_style dot_leader[:font_style]
          num_dots = [((bounds.width - start_dots - dot_leader[:spacer_width] - range_label_width) / dot_leader[:width]).floor, 0].max
          typeset_formatted_text [
            { text: dot_leader[:text] * num_dots, color: dot_leader[:font_color] },
            dot_leader[:spacer],
            ({ text: range_label, anchor: range_anchor }.merge range_font_settings),
          ], line_metrics, align: :right
        end
      else
        typeset_formatted_text [({ text: range_label, anchor: range_anchor }.merge range_font_settings)], line_metrics, align: :right
      end
      move_cursor_to end_cursor unless leader_on_own_line
    end

    # Packs items greedily into lines no wider than available_width.  Never returns an empty line.
    def pack_run_in_items items, separator_width, available_width
      lines = [(line = [])]
      line_width = 0
      items.each do |item|
        addition = line.empty? ? item[:width] : separator_width + item[:width]
        if !line.empty? && line_width + addition > available_width
          lines << (line = [item])
          line_width = item[:width]
        else
          line << item
          line_width += addition
        end
      end
      lines
    end

    def line_width_of packed_line, separator_width
      packed_line.sum {|item| item[:width] } + (separator_width * (packed_line.size - 1))
    end

    # Returns the physical page an entry starts on, or nil during a dry run when destinations are unresolvable.
    def resolve_run_in_pgnum entry, anchor
      return if scratch?
      if (physical_pgnum = entry.attr 'pdf-page-start')
        physical_pgnum
      elsif (target_page_ref = (get_dest anchor)&.first) &&
          (target_page_idx = state.pages.index {|candidate| candidate.dictionary == target_page_ref })
        target_page_idx + 1
      end
    end

    def pgnum_label_for physical_pgnum, num_front_matter_pages
      return unless physical_pgnum
      virtual_pgnum = physical_pgnum - num_front_matter_pages
      (virtual_pgnum < 1 ? (::Asciidoctor::PDF::RomanNumeral.new physical_pgnum, :lower) : virtual_pgnum).to_s
    end

    # Measures formatted fragments as a single unwrapped line under the current font settings.
    def width_of_unwrapped_fragments fragments
      arranger = arrange_fragments_by_line fragments.map(&:dup)
      arranger.finalize_line
      width_of_fragments arranger.fragments
    end
  end
end

::Asciidoctor::PDF::Converter.prepend ::Tiri::CompactToc
