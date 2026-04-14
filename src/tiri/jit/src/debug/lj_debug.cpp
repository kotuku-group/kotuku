// Debugging and introspection.
// Copyright (C) 2005-2022 Mike Pall. See Copyright Notice in luajit.h

#define lj_debug_c
#define LUA_CORE

#include "lj_obj.h"
#include "lj_err.h"
#include "lj_debug.h"
#include "filesource.h"
#include "lj_buf.h"
#include "lj_tab.h"
#include "lj_state.h"
#include "lj_frame.h"
#include "lj_bc.h"
#include "lj_strfmt.h"
#include "lj_gc.h"
#include "lj_jit.h"

// Invalid bytecode position.
#define NO_BCPOS   (~(BCPOS)0)

//********************************************************************************************************************
// Get frame corresponding to a level.

cTValue * lj_debug_frame(lua_State *L, int level, int *size)
{
   cTValue * frame, *nextframe, *bot = tvref(L->stack) + LJ_FR2;

   // Traverse frames backwards.

   for (nextframe = frame = L->base - 1; frame > bot; ) {
      if (frame_gc(frame) IS obj2gco(L)) level++;  //  Skip dummy frames. See lj_err_optype_call().

      if (level-- IS 0) {
         *size = (int)(nextframe - frame);
         return frame;  //  Level found.
      }

      nextframe = frame;

      if (frame_islua(frame)) frame = frame_prevl(frame);
      else {
         if (frame_isvarg(frame)) level++;  //  Skip vararg pseudo-frame.
         frame = frame_prevd(frame);
      }
   }

   *size = level;
   return nullptr;  //  Level not found.
}

//********************************************************************************************************************
// Return bytecode position for function/frame or NO_BCPOS.

static BCPOS debug_framepc(lua_State *L, GCfunc *fn, cTValue *nextframe)
{
   const BCIns* ins;
   GCproto* pt;
   BCPOS pos;
   lj_assertL(fn->c.gct IS ~LJ_TFUNC or fn->c.gct IS ~LJ_TTHREAD, "function or frame expected");

   if (not isluafunc(fn)) {  //  Cannot derive a PC for non-Tiri functions.
      return NO_BCPOS;
   }
   else if (nextframe IS nullptr) {  //  Tiri function on top.
      void *cf = cframe_raw(L->cframe);
      if (cf IS nullptr or (char*)cframe_pc(cf) IS (char*)cframe_L(cf)) return NO_BCPOS;
      ins = cframe_pc(cf);  //  Only happens during error/hook handling.
   }
   else {
      if (frame_islua(nextframe)) ins = frame_pc(nextframe);
      else if (frame_iscont(nextframe)) ins = frame_contpc(nextframe);
      else { // Tiri function below errfunc/gc/hook: find cframe to get the PC.
         void *cf = cframe_raw(L->cframe);
         TValue *f = L->base - 1;
         while (true) {
            if (cf IS nullptr) return NO_BCPOS;

            while (cframe_nres(cf) < 0) {
               if (f >= restorestack(L, -cframe_nres(cf))) break;
               cf = cframe_raw(cframe_prev(cf));
               if (cf IS nullptr) return NO_BCPOS;
            }

            if (f < nextframe) break;

            if (frame_islua(f)) f = frame_prevl(f);
            else {
               if (frame_isc(f) or (frame_iscont(f) and frame_iscont_fficb(f))) cf = cframe_raw(cframe_prev(cf));
               f = frame_prevd(f);
            }
         }
         ins = cframe_pc(cf);
         if (not ins) return NO_BCPOS;
      }
   }

   pt = funcproto(fn);
   pos = proto_bcpos(pt, ins) - 1;

   if (pos > pt->sizebc) {  //  Undo the effects of lj_trace_exit for JLOOP.
      GCtrace* T = (GCtrace*)((char*)(ins - 1) - offsetof(GCtrace, startins));
      lj_assertL(bc_isret(bc_op(ins[-1])), "return bytecode expected");
      pos = proto_bcpos(pt, mref<const BCIns>(T->startpc));
   }

   return pos;
}

//********************************************************************************************************************
// Get line number for a bytecode position.
// Returns BCLine with file index encoded in upper 8 bits (direct read from lineinfo array).

BCLine lj_debug_line(GCproto* pt, BCPOS pc)
{
   auto lineinfo = (const BCLine*)proto_lineinfo(pt);

   if (pc <= pt->sizebc and lineinfo) {
      MSize n = pt->sizebc - 1;

      if (pc IS 0) return pt->firstline; // FUNCF/FUNCV instruction has no lineinfo entry

      if (pc IS pt->sizebc) {
         // Edge case: past end of bytecode - use last instruction's file index
         if (n > 0) return BCLine::encode(lineinfo[n - 1].fileIndex(), pt->firstline.lineNumber() + pt->numline);
         return BCLine::encode(pt->file_source_idx, pt->firstline.lineNumber() + pt->numline);
      }

      return lineinfo[pc - 1]; // Direct read - BCLine already contains file index in upper 8 bits
   }
   else return 0;
}

//********************************************************************************************************************
// Get line number for function/frame.

static BCLine debug_frameline(lua_State *L, GCfunc *fn, cTValue *nextframe)
{
   BCPOS pc = debug_framepc(L, fn, nextframe);
   if (pc != NO_BCPOS) {
      GCproto *pt = funcproto(fn);
      lj_assertL(pc <= pt->sizebc, "PC out of range");
      return lj_debug_line(pt, pc);
   }
   return -1;
}

//********************************************************************************************************************
// Get name of a local variable from slot number and PC.

static CSTRING debug_varname(const GCproto *pt, BCPOS pc, BCREG slot)
{
   if (auto p = (CSTRING)proto_varinfo(pt)) {
      BCPOS lastpc = 0;
      while (true) {
         CSTRING name = p;
         uint32_t vn = *(const uint8_t*)p;
         BCPOS startpc, endpc;
         if (vn < VARNAME__MAX) {
            if (vn IS VARNAME_END) break;  //  End of varinfo.
         }
         else {
            do { p++; } while (*(const uint8_t*)p);  //  Skip over variable name.
         }

         p++;
         lastpc = startpc = lastpc + lj_buf_ruleb128(&p);
         if (startpc > pc) break;
         endpc = startpc + lj_buf_ruleb128(&p);
         if (pc < endpc and slot-- IS 0) {
            if (vn < VARNAME__MAX) {
#define VARNAMESTR(name, str)   str "\0"
               name = VARNAMEDEF(VARNAMESTR);
#undef VARNAMESTR
               if (--vn) while (*name++ or --vn);
            }
            return name;
         }
      }
   }

   return nullptr;
}

//********************************************************************************************************************

// Get name of local variable from semantic slot number and function/frame.

static TValue * debug_localname(lua_State *L, const lua_Debug *ar, CSTRING *name, int32_t Slot)
{
   uint32_t offset = (uint32_t)ar->i_ci & 0xffff;
   uint32_t size = (uint32_t)ar->i_ci >> 16;
   TValue *frame = tvref(L->stack) + offset;
   TValue *nextframe = size ? frame + size : nullptr;
   GCfunc *fn = frame_func(frame);
   BCPOS pc = debug_framepc(L, fn, nextframe);

   if (not nextframe) nextframe = L->top + LJ_FR2;

   if (Slot < 0) {  //  Negative slot number is for varargs.
      if (pc != NO_BCPOS) {
         GCproto* pt = funcproto(fn);
         if ((pt->flags & PROTO_VARARG)) {
            auto slot1 = (BCREG)(pt->numparams + (BCREG)(-Slot));
            if (frame_isvarg(frame)) {  //  Vararg frame has been set up? (pc!=0)
               nextframe = frame;
               frame = frame_prevd(frame);
            }

            if (frame + slot1 + LJ_FR2 < nextframe) {
               *name = "(*vararg)";
               return frame + slot1;
            }
         }
      }
      return nullptr;
   }

   if (pc != NO_BCPOS and (*name = debug_varname(funcproto(fn), pc, (BCREG)Slot - 1)) != nullptr);
   else if (Slot > 0 and frame + Slot + LJ_FR2 < nextframe) *name = "(*temporary)";
   return frame + Slot;
}

//********************************************************************************************************************
// Get name of upvalue.

[[nodiscard]] CSTRING lj_debug_uvname(GCproto *pt, uint32_t idx)
{
   const uint8_t* p = proto_uvinfo(pt);
   lj_assertX(idx < pt->sizeuv, "bad upvalue index");
   if (not p) return "";
   if (idx) while (*p++ or --idx);
   return (CSTRING)p;
}

//********************************************************************************************************************
// Get name and value of upvalue.

[[nodiscard]] CSTRING lj_debug_uvnamev(cTValue *o, uint32_t idx, TValue **tvp, GCobj **op)
{
   if (tvisfunc(o)) {
      GCfunc *fn = funcV(o);
      if (isluafunc(fn)) {
         GCproto *pt = funcproto(fn);
         if (idx < pt->sizeuv) {
            GCobj *uvo = gcref(fn->l.uvptr[idx]);
            *tvp = uvval(&uvo->uv);
            *op = uvo;
            return lj_debug_uvname(pt, idx);
         }
      }
      else if (idx < fn->c.nupvalues) {
         *tvp = &fn->c.upvalue[idx];
         *op = obj2gco(fn);
         return "";
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// Deduce name of an object from slot number and PC.

CSTRING lj_debug_slotname(GCproto *pt, const BCIns* ip, BCREG slot, CSTRING *name)
{
   CSTRING lname;
restart:
   lname = debug_varname(pt, proto_bcpos(pt, ip), slot);
   if (lname != nullptr) { *name = lname; return "local"; }
   while (--ip > proto_bc(pt)) {
      BCIns ins = *ip;
      BCOp op = bc_op(ins);
      BCREG ra = bc_a(ins);
      if (bcmode_a(op) IS BCMbase) {
         if (slot >= ra and (op != BC_KNIL or slot <= bc_d(ins))) return nullptr;
      }
      else if (bcmode_a(op) IS BCMdst and ra IS slot) {
         switch (bc_op(ins)) {
         case BC_MOV:
            if (ra IS slot) { slot = bc_d(ins); goto restart; }
            break;
         case BC_GGET:
            *name = strdata(gco_to_string(proto_kgc(pt, ~(ptrdiff_t)bc_d(ins))));
            return "global";
         case BC_TGETS:
            *name = strdata(gco_to_string(proto_kgc(pt, ~(ptrdiff_t)bc_c(ins))));
            if (ip > proto_bc(pt)) {
               BCIns insp = ip[-1];
               if (bc_op(insp) IS BC_MOV and bc_a(insp) IS ra + 1 + LJ_FR2 and bc_d(insp) IS bc_b(ins)) {
                  return "method";
               }
            }
            return "field";
         case BC_UGET:
            *name = lj_debug_uvname(pt, bc_d(ins));
            return "upvalue";
         default:
            return nullptr;
         }
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// Deduce function name from caller of a frame.

CSTRING lj_debug_funcname(lua_State *L, cTValue *frame, CSTRING *name)
{
   cTValue *pframe;
   GCfunc *fn;
   BCPOS pc;
   if (frame <= tvref(L->stack) + LJ_FR2) return nullptr;
   if (frame_isvarg(frame)) frame = frame_prevd(frame);
   pframe = frame_prev(frame);
   fn = frame_func(pframe);
   pc = debug_framepc(L, fn, frame);
   if (pc != NO_BCPOS) {
      GCproto* pt = funcproto(fn);
      const BCIns* ip = &proto_bc(pt)[check_exp(pc < pt->sizebc, pc)];
      MMS mm = bcmode_mm(bc_op(*ip));
      if (mm IS MM_call) {
         BCREG slot = bc_a(*ip);
         if (bc_op(*ip) IS BC_ITERC) slot -= 3;
         return lj_debug_slotname(pt, ip, slot, name);
      }
      else if (mm != MM__MAX) {
         *name = strdata(mmname_str(G(L), mm));
         return "metamethod";
      }
   }
   return nullptr;
}

//********************************************************************************************************************
// Generate shortened source name.

void lj_debug_shortname(char *out, GCstr *str, BCLine line)
{
   auto src = strdata(str);

   if (*src IS '=') {
      strncpy(out, src + 1, LUA_IDSIZE);  //  Remove first char.
      out[LUA_IDSIZE - 1] = '\0';  //  Ensures null termination.
   }
   else if (*src IS '@') {  //  Output "source", or "...source".
      size_t len = str->len - 1;
      src++;  //  Skip the `@'
      if (len >= LUA_IDSIZE) {
         src += len - (LUA_IDSIZE - 4);  //  Get last part of file name.
         *out++ = '.'; *out++ = '.'; *out++ = '.';
      }
      strcpy(out, src);
   }
   else {  //  Output [string "string"] or [builtin:name].
      size_t len;  //  Length, up to first control char.
      for (len = 0; len < LUA_IDSIZE - 12; len++) {
         if (((const unsigned char*)src)[len] < ' ') break;
      }

      strcpy(out, line.isBuiltin() ? "[builtin:" : "[string \""); out += 9;

      if (src[len] != '\0') {  //  Must truncate?
         if (len > LUA_IDSIZE - 15) len = LUA_IDSIZE - 15;
         strncpy(out, src, len); out += len;
         strcpy(out, "...");
         out += 3;
      }
      else {
         strcpy(out, src);
         out += len;
      }

      strcpy(out, line.isBuiltin() ? "]" : "\"]");
   }
}

//********************************************************************************************************************
// Add current location of a frame to error message.
// Uses FileSource tracking to display accurate file:line for imported code.
// The BCLine returned by debug_frameline encodes the file index in its upper 8 bits (no separate fileinfo array).

void lj_debug_addloc(lua_State *L, CSTRING msg, cTValue *frame, cTValue *nextframe)
{
   if (frame) {
      GCfunc *fn = frame_func(frame);
      if (isluafunc(fn)) {
         BCLine line = debug_frameline(L, fn, nextframe);
         if (line.isValid()) {
            GCproto* pt = funcproto(fn);

            // Use the file index from the BCLine (which comes from fileinfo if multi-file prototype)
            if (not L->file_sources.empty()) {
               const FileSource* src = get_file_source(L, line.fileIndex());
               if (src and not src->filename.empty()) {
                  lj_strfmt_pushf(L, "%s:%d: %s", src->filename.c_str(), line.lineNumber(), msg);
                  return;
               }
            }

            // Fallback to prototype chunk_name
            char buf[LUA_IDSIZE];
            lj_debug_shortname(buf, proto_chunk_name(pt), pt->firstline);
            lj_strfmt_pushf(L, "%s:%d: %s", buf, line.lineNumber(), msg);
            return;
         }
      }
   }
   lj_strfmt_pushf(L, "%s", msg);
}

//********************************************************************************************************************
// Push location string for a bytecode position to Tiri stack.
// Uses FileSource tracking to display accurate file:line for imported code.
// The BCLine returned by lj_debug_line includes the file index from fileinfo (if available).

void lj_debug_pushloc(lua_State *L, GCproto *pt, BCPOS pc)
{
   BCLine line = lj_debug_line(pt, pc);

   if (not L->file_sources.empty()) {
      const FileSource* src = get_file_source(L, line.fileIndex());
      if (src and not src->filename.empty()) {
         lj_strfmt_pushf(L, "%s:%d", src->filename.c_str(), line.lineNumber());
         return;
      }
   }

   // Fallback to prototype chunk_name for legacy behaviour
   GCstr *name = proto_chunk_name(pt);
   CSTRING s = strdata(name);
   MSize i, len = name->len;

   if (pt->firstline.isBuiltin()) lj_strfmt_pushf(L, "builtin:%s", s);
   else if (*s IS '@') {
      s++;
      len--;
      for (i = len; i > 0; i--)
         if (s[i] IS '/' or s[i] IS '\\') {
            s += i + 1;
            break;
         }
      lj_strfmt_pushf(L, "%s:%d", s, line.lineNumber());
   }
   else if (len > 40) lj_strfmt_pushf(L, "%p:%d", pt, line.lineNumber());
   else if (*s IS '=') lj_strfmt_pushf(L, "%s:%d", s + 1, line.lineNumber());
   else lj_strfmt_pushf(L, "\"%s\":%d", s, line.lineNumber());
}

//********************************************************************************************************************
// Note: n is the internal slot number (1-based). Call sites should convert from
// 0-based user indices if needed (e.g., debug_semantic_index_to_internal in lib_debug.cpp).

[[nodiscard]] extern CSTRING lua_getlocal(lua_State *L, const lua_Debug *ar, int n)
{
   CSTRING name = nullptr;
   if (ar) {
      TValue* o = debug_localname(L, ar, &name, n);
      if (name) {
         copyTV(L, L->top, o);
         incr_top(L);
      }
   }
   else if (tvisfunc(L->top - 1) and isluafunc(funcV(L->top - 1))) {
      name = debug_varname(funcproto(funcV(L->top - 1)), 0, (BCREG)n - 1);
   }
   return name;
}

//********************************************************************************************************************
// Note: n is the internal slot number (1-based). Call sites should convert from
// 0-based user indices if needed.

extern CSTRING lua_setlocal(lua_State *L, const lua_Debug *ar, int n)
{
   CSTRING name = nullptr;
   TValue *o = debug_localname(L, ar, &name, n);
   if (name) copyTV(L, o, L->top - 1);
   L->top--;
   return name;
}

//********************************************************************************************************************

int lj_debug_getinfo(lua_State *L, CSTRING what, lj_Debug *ar, int ext)
{
   int opt_f = 0, opt_L = 0;
   TValue *frame = nullptr;
   TValue *nextframe = nullptr;
   GCfunc *fn;

   if (*what IS '>') {
      TValue *func = L->top - 1;
      if (not tvisfunc(func)) return 0;
      fn = funcV(func);
      L->top--;
      what++;
   }
   else {
      uint32_t offset = (uint32_t)ar->i_ci & 0xffff;
      uint32_t size = (uint32_t)ar->i_ci >> 16;
      lj_assertL(offset != 0, "bad frame offset");
      frame = tvref(L->stack) + offset;
      if (size) nextframe = frame + size;
      lj_assertL(frame <= tvref(L->maxstack) and (not nextframe or nextframe <= tvref(L->maxstack)), "broken frame chain");
      fn = frame_func(frame);
      lj_assertL(fn->c.gct IS ~LJ_TFUNC, "bad frame function");
   }
   for (; *what; what++) {
      if (*what IS 'S') {
         if (isluafunc(fn)) {
            GCproto* pt = funcproto(fn);
            BCLine firstline = pt->firstline;

            // Try to use FileSource for accurate source information
            if (not L->file_sources.empty()) {
               const FileSource* src = get_file_source(L, pt->file_source_idx);
               if (src and not src->filename.empty()) {
                  ar->source = src->path.c_str();
                  // Copy filename to short_src (with truncation if needed)
                  strncpy(ar->short_src, src->filename.c_str(), LUA_IDSIZE - 1);
                  ar->short_src[LUA_IDSIZE - 1] = '\0';
                  ar->linedefined = firstline.lineNumber();
                  ar->lastlinedefined = firstline.lineNumber() + pt->numline;
                  ar->what = (firstline or !pt->numline) ? "Tiri" : "main";
               }
               else {
                  // Fallback to prototype chunk_name
                  GCstr* name = proto_chunk_name(pt);
                  ar->source = strdata(name);
                  lj_debug_shortname(ar->short_src, name, pt->firstline);
                  ar->linedefined = firstline.lineNumber();
                  ar->lastlinedefined = firstline.lineNumber() + pt->numline;
                  ar->what = (firstline or !pt->numline) ? "Tiri" : "main";
               }
            }
            else {
               // Fallback to prototype chunk_name
               GCstr* name = proto_chunk_name(pt);
               ar->source = strdata(name);
               lj_debug_shortname(ar->short_src, name, pt->firstline);
               ar->linedefined = firstline.lineNumber();
               ar->lastlinedefined = firstline.lineNumber() + pt->numline;
               ar->what = (firstline or !pt->numline) ? "Tiri" : "main";
            }
         }
         else {
            ar->source = "=[C]";
            ar->short_src[0] = '[';
            ar->short_src[1] = 'C';
            ar->short_src[2] = ']';
            ar->short_src[3] = '\0';
            ar->linedefined = -1;
            ar->lastlinedefined = -1;
            ar->what = "C";
         }
      }
      else if (*what IS 'l') {
         if (frame) {
            BCLine line = debug_frameline(L, fn, nextframe);
            ar->currentline = line.isValid() ? line.lineNumber() : -1;
         }
         else ar->currentline = -1;
      }
      else if (*what IS 'u') {
         ar->nups = fn->c.nupvalues;
         if (ext) {
            if (isluafunc(fn)) {
               GCproto* pt = funcproto(fn);
               ar->nparams = pt->numparams;
               ar->isvararg = !!(pt->flags & PROTO_VARARG);
            }
            else {
               ar->nparams = 0;
               ar->isvararg = 1;
            }
         }
      }
      else if (*what IS 'n') {
         ar->namewhat = frame ? lj_debug_funcname(L, frame, &ar->name) : nullptr;
         if (ar->namewhat IS nullptr) {
            ar->namewhat = "";
            ar->name = nullptr;
         }
      }
      else if (*what IS 'f') opt_f = 1;
      else if (*what IS 'L') opt_L = 1;
      else return 0;  //  Bad option.
   }

   if (opt_f) {
      setfuncV(L, L->top, fn);
      incr_top(L);
   }

   if (opt_L) {
      if (isluafunc(fn)) {
         GCtab* t = lj_tab_new(L, 0, 0);
         GCproto* pt = funcproto(fn);
         const BCLine* lineinfo = (const BCLine*)proto_lineinfo(pt);
         if (lineinfo) {
            // Direct BCLine access - extract line number for table key.
            // Note: Same line numbers in different files will share a table entry (existing semantic).
            MSize i, szl = pt->sizebc - 1;
            for (i = 0; i < szl; i++) {
               int32_t line = lineinfo[i].lineNumber();
               setboolV(lj_tab_setint(L, t, line), 1);
            }
         }
         settabV(L, L->top, t);
      }
      else setnilV(L->top);

      incr_top(L);
   }
   return 1;  //  Ok.
}

//********************************************************************************************************************

extern int lua_getinfo(lua_State* L, CSTRING what, lua_Debug* ar)
{
   return lj_debug_getinfo(L, what, (lj_Debug*)ar, 0);
}

// This function fills parts of a lua_Debug structure with an identification of the activation record of the function
// executing at a given level. Level 0 is the current running function, whereas level n+1 is the function that has
// called level n (except for tail calls, which do not count on the stack). When there are no errors, lua_getstack
// returns 1; when called with a level greater than the stack depth, it returns 0.

extern int lua_getstack(lua_State *L, int level, lua_Debug *ar)
{
   int size;

   if (cTValue *frame = lj_debug_frame(L, level, &size)) {
      ar->i_ci = (size << 16) + (int)(frame - tvref(L->stack));
      return 1;
   }
   else {
      ar->i_ci = level - size;
      return 0; // No frame found.
   }
}

//********************************************************************************************************************
// Number of frames for the leading and trailing part of a traceback.

#define TRACEBACK_LEVELS1   12
#define TRACEBACK_LEVELS2   10

extern void luaL_traceback(lua_State *L, lua_State *L1, CSTRING msg, int level)
{
   int top = (int)(L->top - L->base);
   int lim = TRACEBACK_LEVELS1;
   lua_Debug ar;
   if (msg) lua_pushfstring(L, "%s\n", msg);
   lua_pushliteral(L, "stack traceback:");

   while (lua_getstack(L1, level++, &ar)) {
      GCfunc *fn;
      if (level > lim) {
         if (not lua_getstack(L1, level + TRACEBACK_LEVELS2, &ar)) level--;
         else {
            lua_pushliteral(L, "\n\t...");
            lua_getstack(L1, -10, &ar);
            level = ar.i_ci - TRACEBACK_LEVELS2;
         }
         lim = 2147483647;
         continue;
      }

      lua_getinfo(L1, "Snlf", &ar);
      fn = funcV(L1->top - 1); L1->top--;

      if (isffunc(fn) and !*ar.namewhat) lua_pushfstring(L, "\n\t[builtin#%d]:", fn->c.ffid);
      else lua_pushfstring(L, "\n\t%s:", ar.short_src);

      if (ar.currentline > 0) lua_pushfstring(L, "%d:", ar.currentline);

      if (*ar.namewhat) lua_pushfstring(L, " in function " LUA_QS, ar.name);
      else if (*ar.what IS 'm') lua_pushliteral(L, " in main chunk");
      else if (*ar.what IS 'C') lua_pushfstring(L, " at %p", fn->c.f);
      else lua_pushfstring(L, " in function <%s:%d>", ar.short_src, ar.linedefined);

      if ((int)(L->top - L->base) - top >= 15) lua_concat(L, (int)(L->top - L->base) - top);
   }

   lua_concat(L, (int)(L->top - L->base) - top);
}

//********************************************************************************************************************
// Capture stack trace for try<trace> exception handling.  Allocates a CapturedStackTrace on the heap. Caller must
// free with lj_debug_free_trace().
//
// Building traces isn't super fast but that's OK - it only runs if the user requests it.
//
// NB: This function sets L->pending_trace early to root the trace structure during string allocations. The GC marks
// strings in pending_trace via gc_traverse_thread(), preventing them from being collected during incremental GC
// cycles.

CapturedStackTrace * lj_debug_capture_trace(lua_State *L, int skip_levels)
{
   auto trace = (CapturedStackTrace *)lj_mem_new(L, sizeof(CapturedStackTrace));
   trace->frame_count = 0;

   // Root the trace immediately so GC can mark strings as they're added.
   // Safety: The caller (check_try_handler) only calls this function when L->pending_trace is null,
   // so we never overwrite an existing trace. The caller's subsequent assignment of the return value
   // to L->pending_trace is effectively a no-op since we set it here.

   L->pending_trace = trace;

   lua_Debug ar;
   int level = skip_levels;

   while ((trace->frame_count < LJ_MAX_TRACE_FRAMES) and lua_getstack(L, level++, &ar)) {
      lua_getinfo(L, "Snl", &ar);

      CapturedFrame *cf = &trace->frames[trace->frame_count];

      // Initialize frame to null first, then increment frame_count.  This ensures GC will traverse this frame
      // (via frame_count) and find null pointers rather than uninitialized garbage, while we allocate the strings.

      cf->source = nullptr;
      cf->funcname = nullptr;
      cf->line = 0;
      trace->frame_count++;  // Increment before allocating strings so GC sees this frame

      // Copy source name (GC can now mark this frame via pending_trace)

      if (ar.short_src[0] != '\0') cf->source = lj_str_newz(L, ar.short_src);

      if (ar.name and ar.name[0] != '\0') cf->funcname = lj_str_newz(L, ar.name);
      else if (ar.what and *ar.what IS 'm') cf->funcname = lj_str_newlit(L, "main chunk");

      cf->line = BCLine(ar.currentline > 0 ? ar.currentline : 0);
   }

   return trace;
}

//********************************************************************************************************************
// Free a captured stack trace.

void lj_debug_free_trace(lua_State *L, CapturedStackTrace *trace)
{
   if (trace) lj_mem_free(G(L), trace, sizeof(CapturedStackTrace));
}
