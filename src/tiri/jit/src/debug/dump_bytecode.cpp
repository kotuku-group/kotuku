// Copyright (C) 2025-2026 Paul Manias.

#define LUA_CORE

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_state.h"
#include "lj_bc.h"
#include "lj_strfmt.h"
#include "lexer.h"
#include "parser.h"
#include "lj_vm.h"
#include "lj_vmevent.h"

#include <kotuku/main.h>

#include "filesource.h"
#include "token_types.h"
#include "parse_types.h"
#include "dump_bytecode.h"
#include "../../../defs.h"

//********************************************************************************************************************

static std::string format_string_constant(std::string_view Data)
{
   static constexpr size_t max_length = 40;
   static constexpr std::string_view hex_digits = "0123456789ABCDEF";

   std::string text;
   const auto limit = std::min(Data.size(), max_length);
   const bool truncated = Data.size() > max_length;

   text.reserve(limit * 2 + (truncated ? 6 : 2));

   for (size_t i = 0; i < limit; i++) {
      const unsigned char ch = Data[i];

      switch (ch) {
         case '\n': text += "\\n"; break;
         case '\r': text += "\\r"; break;
         case '\t': text += "\\t"; break;
         case '\\': text += "\\\\"; break;
         case '"':  text += "\\\""; break;
         default:
            if (ch < 32) {
               text += "\\x";
               text += hex_digits[(ch >> 4) & 15];
               text += hex_digits[ch & 15];
            }
            else text += char(ch);
            break;
      }
   }

   if (truncated) text += "...";

   return std::format("\"{}\"", text);
}

//********************************************************************************************************************

static std::string describe_num_constant(const TValue *Value)
{
   if (tvisint(Value)) return std::format("{}", intV(Value));
   if (tvisnum(Value)) return std::format("{}", numV(Value));
   return "<number>";
}

//********************************************************************************************************************

static std::string describe_gc_constant(GCproto *Proto, ptrdiff_t Index)
{
   GCobj *gc_obj = proto_kgc(Proto, Index);

   if (not gc_obj) {
      // Most likely an invalid index - could indicate an invalid bytecode stream has been generated.
      pf::Log("ByteCode").warning("describe_gc_constant: null GC object at index %" PRId64, uint64_t(Index));
      return "K<null>";
   }
   else if (gc_obj->gch.gct IS (uint8_t)~LJ_TSTR) {
      GCstr *str_obj = gco_to_string(gc_obj);
      return std::format("K{}", format_string_constant({strdata(str_obj), str_obj->len}));
   }
   else if (gc_obj->gch.gct IS (uint8_t)~LJ_TPROTO) {
      GCproto *child = gco_to_proto(gc_obj);
      int first_line = child->firstline.lineNumber();
      return std::format("K<func {}-{}>", first_line, first_line + int(child->numline));
   }
   else if (gc_obj->gch.gct IS (uint8_t)~LJ_TTAB) return "K<table>";
   else return "K<gc>";
}

//********************************************************************************************************************

static std::string describe_primitive(int Value)
{
   switch (Value) {
      case 0: return "nil";
      case 1: return "false";
      case 2: return "true";
      default: return std::format("pri({})", Value);
   }
}

//********************************************************************************************************************
// Get line number for a bytecode position from direct BCLine storage.

static BCLine get_proto_line(GCproto *Proto, BCPOS Pc)
{
   auto lineinfo = (const BCLine *)proto_lineinfo(Proto);

   if ((Pc <= Proto->sizebc) and lineinfo) {
      MSize n = Proto->sizebc - 1;
      if (Pc IS 0) return Proto->firstline;

      if (Pc IS Proto->sizebc) { // Use last instruction's file index for consistency
         if (n > 0) return BCLine::encode(lineinfo[n - 1].fileIndex(), Proto->firstline.lineNumber() + Proto->numline);
         else return BCLine::encode(Proto->file_source_idx, Proto->firstline.lineNumber() + Proto->numline);
      }

      return lineinfo[Pc - 1]; // Direct read - BCLine already contains file index in upper 8 bits
   }

   return 0;
}

//********************************************************************************************************************

static std::string_view get_proto_uvname(GCproto *Proto, uint32_t Index)
{
   const uint8_t *info = proto_uvinfo(Proto);
   if (not info or Index >= Proto->sizeuv) return {};

   const uint8_t *ptr = info;

   for (uint32_t i = 0; i < Index; ++i) {
      while (*ptr) ++ptr;
      ++ptr;
   }

   if (not *ptr) return {};
   return std::string_view(reinterpret_cast<const char *>(ptr));
}

//********************************************************************************************************************

static std::string describe_operand_value(GCproto *Proto, FuncState *fs, BCMode Mode, int Value, BCPOS Pc)
{
   switch (Mode) {
      case BCMdst:
      case BCMbase:
      case BCMvar:
      case BCMrbase:
         return std::format("R{}", Value);

      case BCMuv:
         if (Proto) {
            auto name = get_proto_uvname(Proto, (uint32_t)Value);
            return name.empty() ? std::format("U{}", Value) : std::format("U{}({})", Value, name);
         }
         return std::format("U{}", Value);

      case BCMlit:
         return std::format("#{}", Value);

      case BCMlits:
         return std::format("#{}", (int16_t)Value);

      case BCMpri:
         return describe_primitive(Value);

      case BCMnum:
         if (Proto) {
            return std::format("#{}", describe_num_constant(proto_knumtv(Proto, Value)));
         }
         else if (fs) {
            // Look up number constant in the constant table
            GCtab *kt = fs->kt;
            Node *node = noderef(kt->node);

            for (uint32_t i = 0; i <= kt->hmask; ++i) {
               TValue *val = &node[i].val;
               if (tvhaskslot(val) and tvkslot(val) IS (uint32_t)Value) {
                  TValue *key_tv = &node[i].key;
                  if (tvisnum(key_tv) or tvisint(key_tv)) {
                     return std::format("#{}", describe_num_constant(key_tv));
                  }
                  break;
               }
            }
         }
         return std::format("#<num{}>", Value);

      case BCMstr:
      case BCMfunc:
      case BCMtab:
         if (Proto) {
            return describe_gc_constant(Proto, -(ptrdiff_t)Value - 1);
         }
         else if (fs) {
            // Look up GC constant in the constant table
            GCtab *kt = fs->kt;
            Node *node = noderef(kt->node);

            for (uint32_t i = 0; i <= kt->hmask; ++i) {
               TValue *val = &node[i].val;
               if (tvhaskslot(val) and tvkslot(val) IS (uint32_t)Value) {
                  TValue *key_tv = &node[i].key;

                  if (tvisstr(key_tv)) {
                     GCstr *str_obj = strV(key_tv);
                     return std::format("K{}", format_string_constant({strdata(str_obj), str_obj->len}));
                  }

                  if (tvisproto(key_tv)) {
                     GCproto *child = protoV(key_tv);
                     int first_line = child->firstline.lineNumber();
                     return std::format("K<func {}-{}>", first_line, first_line + int(child->numline));
                  }

                  if (tvistab(key_tv)) return "K<table>";
                  break;
               }
            }
         }
         return std::format("K<gc{}>", Value);

      case BCMjump: {
         if ((BCPOS)Value IS NO_JMP) return "->(no)";

         const ptrdiff_t offset = (ptrdiff_t)Value - BCBIAS_J;
         const ptrdiff_t dest = (ptrdiff_t)Pc + 1 + offset;

         if (dest < 0) return "->(neg)";

         if (Proto) {
            if (dest >= (ptrdiff_t)Proto->sizebc) return "->(out)";
         }
         else if (fs) {
            if (dest >= (ptrdiff_t)fs->pc) return "->(out)";
         }

         return std::format("->{}{}", dest, offset >= 0 ? std::format("(+{})", offset) : std::format("({})", offset));
      }

      default:
         return std::format("?{}", Value);
   }
}

//********************************************************************************************************************

static void append_operand(std::string &Operands, std::string_view Label, std::string_view Value)
{
   if (not Operands.empty()) Operands += ' ';
   Operands += std::format("{}={}", Label, Value);
}

//********************************************************************************************************************
// Extract bytecode info and build operands string - common helper

struct BytecodeInfo {
   BCOp op;
   CSTRING op_name;
   BCMode mode_a, mode_b, mode_c, mode_d;
   int value_a, value_b, value_c, value_d;
};

static BytecodeInfo extract_instruction_info(BCIns Ins)
{
   BytecodeInfo info;
   info.op = bc_op(Ins);
   info.op_name = (info.op < BC__MAX) ? glBytecodeNames[info.op] : "???";
   info.mode_a  = bcmode_a(info.op);
   info.mode_b  = bcmode_b(info.op);
   info.mode_c  = bcmode_c(info.op);
   info.mode_d  = bcmode_d(info.op);
   info.value_a = bc_a(Ins);
   info.value_b = bc_b(Ins);
   info.value_c = bc_c(Ins);
   info.value_d = bc_d(Ins);
   return info;
}

//********************************************************************************************************************

void format_bc_line(lua_State *L, BCLine Line, int FileWidth, BytecodeLogger Logger, std::string_view Indent, BCPOS pc,
   const std::string &Operands, void *Meta, BytecodeInfo &Info, bool JumpTarget, bool Verbose)
{
   if (Verbose) {
      std::string file_and_line;
      int line_no_width = 4;

      if (Line.isValid() and Line.lineNumber() > 0) {
         const FileSource *src = get_file_source(L, Line.fileIndex());

         std::string_view sv("<unknown>");
         if (src) sv = std::string_view(src->filename);
         if (sv.size() > size_t(FileWidth)) sv.remove_suffix(sv.size() - FileWidth);
         file_and_line = std::format("{}:{}", sv, Line.lineNumber());

         if (src->total_lines > 10000) line_no_width += 2;
         else if (src->total_lines > 1000) line_no_width++;
      }
      else file_and_line = "<unknown>:-";

      FileWidth += line_no_width;
      Logger(std::format("{}[{:04d}] {:{}.{}} {} {:<9} {}", Indent, int(pc), file_and_line, FileWidth, FileWidth, JumpTarget ? "=>" : "  ", Info.op_name, Operands), Meta);
   }
   else Logger(std::format("{}[{:04d}] {:<10} {}", Indent, int(pc), Info.op_name, Operands), Meta);
}

//********************************************************************************************************************
// Recursively print bytecode for a finalized prototype.

void trace_proto_bytecode(lua_State *L, GCproto *Proto, BytecodeLogger Logger, void *Meta, bool Verbose, int Indent)
{
   if (not Proto) return;

   const BCIns *bc_stream = proto_bc(Proto);
   std::string indent_str(Indent * 2, ' ');

   // In verbose mode, pre-scan to identify jump targets

   std::vector<uint8_t> targets;
   if (Verbose) {
      targets.resize(Proto->sizebc ? Proto->sizebc : 1, 0);
      for (BCPOS pc = 0; pc < Proto->sizebc; ++pc) {
         BCIns instruction = bc_stream[pc];
         BCOp opcode = bc_op(instruction);

         if (bcmode_hasd(opcode) and bcmode_d(opcode) IS BCMjump) {
            int value = bc_d(instruction);
            if ((BCPOS)value != NO_JMP) {
               ptrdiff_t offset = (ptrdiff_t)value - BCBIAS_J;
               ptrdiff_t dest = (ptrdiff_t)pc + 1 + offset;
               if (dest >= 0 and dest < (ptrdiff_t)Proto->sizebc) targets[size_t(dest)] = 1;
            }
         }
      }
   }

   // Output function header - use lineNumber() to extract decoded line values

   int first_line = Proto->firstline.lineNumber();
   int last_line = first_line + int(Proto->numline);

   if (Indent IS 0) {
      if (Verbose) {
         Logger(std::format("Function (lines {}-{})", first_line, last_line), Meta);
         Logger(std::format("Bytecodes: {}, Constants: {} numeric, {} objects", int(Proto->sizebc), int(Proto->sizekn), int(Proto->sizekgc)), Meta);
         Logger("", Meta);
      }
   }
   else if (Verbose) {
      Logger(std::format("{}--- Nested function: lines {}-{}, {} bytecodes ---",
         indent_str, first_line, last_line, int(Proto->sizebc)), Meta);
   }
   else {
      Logger(std::format("{}--- Nested function: lines {}-{}, {} bytecodes ---",
         indent_str, first_line, last_line, int(Proto->sizebc)), Meta);
   }

   auto file_width = widest_file_source(L, false);

   for (BCPOS pc = 0; pc < Proto->sizebc; ++pc) {
      BCIns instruction = bc_stream[pc];
      auto info = extract_instruction_info(instruction);

      std::string operands;

      if (info.mode_a != BCMnone) append_operand(operands, "A", describe_operand_value(Proto, nullptr, info.mode_a, info.value_a, pc));

      if (bcmode_hasd(info.op)) {
         if (info.mode_d != BCMnone) append_operand(operands, "D", describe_operand_value(Proto, nullptr, info.mode_d, info.value_d, pc));
      }
      else {
         if (info.mode_b != BCMnone) append_operand(operands, "B", describe_operand_value(Proto, nullptr, info.mode_b, info.value_b, pc));
         if (info.mode_c != BCMnone) append_operand(operands, "C", describe_operand_value(Proto, nullptr, info.mode_c, info.value_c, pc));
      }

      BCLine line = get_proto_line(Proto, pc);

      format_bc_line(L, line, file_width, Logger, indent_str, pc, operands, Meta, info, targets[pc] ? true : false, Verbose);

      // If this is a FNEW instruction, recursively disassemble the child prototype

      if (info.op IS BC_FNEW) {
         const ptrdiff_t index = -(ptrdiff_t)info.value_d - 1;
         const bool valid = ((uintptr_t)(intptr_t)index >= (uintptr_t)-(intptr_t)Proto->sizekgc);

         if (valid) {
            GCobj *gc_obj = proto_kgc(Proto, index);
            if (gc_obj->gch.gct IS (uint8_t)~LJ_TPROTO) {
               GCproto *child = gco_to_proto(gc_obj);
               trace_proto_bytecode(L, child, Logger, Meta, Verbose, Indent + 1);
            }
         }
      }
   }
}

//********************************************************************************************************************
// Print a complete disassembly of bytecode instructions.

extern void dump_bytecode(FuncState &fs)
{
   pf::Log log("ByteCode");

   auto log_callback = [](std::string_view Msg, void *Meta) {
      fprintf(stderr, "%.*s\n", int(Msg.size()), Msg.data());
   };

   fprintf(stderr, "Instruction Count: %u\n", (unsigned)fs.pc);

   auto file_width = widest_file_source(fs.L, false);

   for (BCPOS pc = 0; pc < fs.pc; ++pc) {
      const BCInsLine &iline = fs.bcbase[pc];
      auto info = extract_instruction_info(iline.ins);

      std::string operands;

      if (info.mode_a != BCMnone) append_operand(operands, "A", describe_operand_value(nullptr, &fs, info.mode_a, info.value_a, pc));

      if (bcmode_hasd(info.op)) {
         if (info.mode_d != BCMnone) append_operand(operands, "D", describe_operand_value(nullptr, &fs, info.mode_d, info.value_d, pc));
      }
      else {
         if (info.mode_b != BCMnone) append_operand(operands, "B", describe_operand_value(nullptr, &fs, info.mode_b, info.value_b, pc));
         if (info.mode_c != BCMnone) append_operand(operands, "C", describe_operand_value(nullptr, &fs, info.mode_c, info.value_c, pc));
      }

      format_bc_line(fs.L, iline.line, file_width, log_callback, "", pc, operands, nullptr, info, false, true);

      // If this is a FNEW instruction, look up and print the child prototype
      if (info.op IS BC_FNEW) {
         // FNEW uses D operand which stores the constant slot index
         // Search the hash table to find the proto with this slot number
         GCtab *kt = fs.kt;
         Node *node = noderef(kt->node);

         for (uint32_t i = 0; i <= kt->hmask; ++i) {
            TValue *val = &node[i].val;
            if (tvhaskslot(val) and tvkslot(val) IS (uint32_t)info.value_d) {
               TValue *key_tv = &node[i].key;
               if (tvisproto(key_tv)) {
                  GCproto *child = protoV(key_tv);
                  trace_proto_bytecode(fs.L, child, log_callback, &log, true, 1);
               }
               break;
            }
         }
      }
   }
}
