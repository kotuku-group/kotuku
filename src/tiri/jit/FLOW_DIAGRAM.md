
  Diagram 1: High-Level Compilation Pipeline

  ┌─────────────────────────────────────────────────────────────────────┐
  │                    TIRI COMPILATION PIPELINE                       │
  └─────────────────────────────────────────────────────────────────────┘

      SOURCE CODE (.tiri file)
      │
      │  local x = 5 + 10
      │  print(x)
      │
      ├──────────────────────────────────────────────────────────────────
      ▼  STAGE 1: LEXICAL ANALYSIS
      │
      │  Files: src/parser/lexer.{h,cpp}
      │
      │  ┌──────────────────────────────────────────────────┐
      │  │ LexState - Character Stream Scanner              │
      │  │  • Single-pass O(n) tokenization                 │
      │  │  • Zero-copy string_view for identifiers         │
      │  │  • Constexpr token definitions (C++20)           │
      │  │  • Extended tokens for Tiri syntax              │
      │  └──────────────────────────────────────────────────┘
      │
      │  TOKEN_DEF_LIST: Single source of truth for all tokens
      │   • Standard: TK_if, TK_while, TK_return, etc.
      │   • Tiri extensions: TK_if_empty (??), TK_cadd (+=),
      │                       TK_plusplus (++), TK_shl (<<)
      │
      ▼  Token Stream:
      │
      │  [TK_local] [TK_name:"x"] ['='] [TK_number:5] ['+']
      │  [TK_number:10] [TK_name:"print"] ['('] [TK_name:"x"] [')']
      │
      ├──────────────────────────────────────────────────────────────────
      ▼  STAGE 2: AST CONSTRUCTION (Parsing)
      │
      │  Files: src/parser/parser.cpp, ast_builder.cpp, ast_nodes.{h,cpp}
      │  Entry: lj_parse(LexState*) → run_ast_pipeline()
      │
      │  ┌──────────────────────────────────────────────────┐
      │  │ Parser - Recursive Descent + Operator Precedence │
      │  │  • O(n) single-pass                              │
      │  │  • Strongly-typed AST nodes (C++20)              │
      │  │  • RAII memory management                        │
      │  └──────────────────────────────────────────────────┘
      │
      │  AST Node Hierarchy:
      │   ExprNode: LiteralExpr, BinaryExpr, CallExpr, etc.
      │   StmtNode: LocalDeclStmt, AssignmentStmt, IfStmt, etc.
      │
      ▼  Abstract Syntax Tree:
      │
      │  BlockStmt {
      │    LocalDeclStmt {
      │      identifiers: ["x"]
      │      initialisers: [
      │        BinaryExpr {
      │          op: Add,
      │          left: LiteralExpr(5),
      │          right: LiteralExpr(10)
      │        }
      │      ]
      │    },
      │    ExpressionStmt {
      │      CallExpr {
      │        callee: IdentifierExpr("print"),
      │        args: [IdentifierExpr("x")]
      │      }
      │    }
      │  }
      │
      ├──────────────────────────────────────────────────────────────────
      ▼  STAGE 3: IR EMISSION (Bytecode Generation)
      │
      │  Files: src/parser/ir_emitter.{h,cpp}, parse_regalloc.cpp,
      │         operator_emitter.cpp, parse_scope.cpp
      │
      │  ┌──────────────────────────────────────────────────┐
      │  │ IrEmitter - AST → Bytecode Lowering              │
      │  │  • RegisterAllocator: Virtual register tracking  │
      │  │  • OperatorEmitter: Constant folding             │
      │  │  • ScopeManager: Variable resolution             │
      │  │  • ControlFlowGraph: Jump list management        │
      │  └──────────────────────────────────────────────────┘
      │
      │  ExpDesc (Expression Descriptor):
      │   - Represents expressions during compilation
      │   - Kinds: Nil, Num, Local, Global, Relocable, etc.
      │   - Contains jump lists for control flow
      │
      ▼  LuaJIT Bytecode (32-bit instructions):
      │
      │  GCproto {
      │    0001  KSHORT   A=R0 D=#15      ; x = 15 (folded!)
      │    0002  GGET     A=R1 D="print"  ; Load print
      │    0003  MOV      A=R2 B=R0       ; Copy x to arg
      │    0004  CALL     A=R1 B=1 C=2    ; print(x)
      │    0005  RET0     A=R0 D=#1       ; Return
      │  }
      │
      │  Format ABC: [B:8][C:8][A:8][OP:8]
      │  Format AD:  [D:16][A:8][OP:8]
      │
      ├──────────────────────────────────────────────────────────────────
      │  EXECUTION SPLIT
      ├──────────────────────────────────────────────────────────────────
      │                          │
      │  ┌───────────────────────┴───────────────────────┐
      │  │                                               │
      │  ▼ COLD PATH                     HOT PATH ▼      │
      │                                                  │
      │  Bytecode Interpreter              JIT Compiler  │
      │  • VM dispatcher                   (STAGE 4)     │
      │  • Stack-based execution                         │
      │  • Tracks hot loops                              │
      │                                                  │
      │  When loop/call reaches            ┌─────────────┘
      │  hot threshold (56 iterations)  ───┤
      │                                    │
      │                                    ▼
      │                    ┌───────────────────────────────────┐
      │                    │ JIT COMPILATION (HOT PATH)        │
      │                    │ Files: lj_trace.cpp, lj_record.cpp│
      │                    └───────────────────────────────────┘
      │                                    │
      │                    ┌───────────────▼───────────────┐
      │                    │ Phase 1: TRACE RECORDING      │
      │                    │ (lj_record.cpp)               │
      │                    │                               │
      │                    │ • Execute bytecode in         │
      │                    │   recording mode              │
      │                    │ • Build SSA IR                │
      │                    │ • Record type guards          │
      │                    │ • Snapshot VM state           │
      │                    └───────────────┬───────────────┘
      │                                    │
      │                                    ▼
      │                        SSA IR (Intermediate Repr):
      │
      │                        0001 SLOAD  #0 T      ; Load x
      │                        0002 IR_GT  #0 #0     ; Type guard
      │                        0003 IR_KINT #15      ; Constant 15
      │                        0004 IR_ADD #0 #3     ; x + 15
      │
      │                    ┌───────────────┬────────────────┐
      │                    │ Phase 2: IR OPTIMIZATION       │
      │                    │ (lj_opt_*.cpp)                 │
      │                    │                                │
      │                    │ • Constant folding             │
      │                    │ • Dead code elimination        │
      │                    │ • Type narrowing               │
      │                    │ • Loop invariant code motion   │
      │                    │ • Allocation sinking           │
      │                    └───────────────┬────────────────┘
      │                                    │
      │                                    ▼
      │                        Optimized SSA IR
      │
      │                    ┌───────────────┬────────────────┐
      │                    │ Phase 3: CODE GENERATION       │
      │                    │ (lj_asm.cpp, lj_asm_x86.h)     │
      │                    │                                │
      │                    │ • Register allocation          │
      │                    │ • Platform-specific codegen    │
      │                    │ • Instruction encoding         │
      │                    └───────────────┬────────────────┘
      │                                    │
      │                                    ▼
      │                        MACHINE CODE (x64 example):
      │
      │                        mov rax, [rbp+8]    ; Load x
      │                        cmp byte [rax], 0x3 ; Type guard
      │                        jne exit_1          ; Side exit
      │                        mov rbx, 15         ; Constant
      │                        add rax, rbx        ; Addition
      │
      │                    ┌───────────────┬────────────────┐
      │                    │ Phase 4: TRACE LINKING         │
      │                    │ (lj_trace.cpp)                 │
      │                    │                                │
      │                    │ • Store in GCtrace             │
      │                    │ • Patch bytecode with JLOOP    │
      │                    │ • Link side exits              │
      │                    └───────────────┬────────────────┘
      │                                    │
      └────────────────────────────────────┼─────────────────
                                           │
                                           ▼
                                   NATIVE EXECUTION
                                   Direct CPU execution

  Diagram 2: Expression Compilation Detail

  ┌─────────────────────────────────────────────────────────────────────┐
  │              EXPRESSION COMPILATION: x = 5 + a * 2                  │
  └─────────────────────────────────────────────────────────────────────┘

  AST REPRESENTATION:
  ┌────────────────────────────────────────────────────────────────┐
  │ AssignmentStmt                                                 │
  │   lhs: [IdentifierExpr("x")]                                   │
  │   rhs: [                                                       │
  │     BinaryExpr {                                               │
  │       op: Add,                                                 │
  │       left: LiteralExpr(5),                                    │
  │       right: BinaryExpr {                                      │
  │         op: Mul,                                               │
  │         left: IdentifierExpr("a"),                             │
  │         right: LiteralExpr(2)                                  │
  │       }                                                        │
  │     }                                                          │
  │   ]                                                            │
  └────────────────────────────────────────────────────────────────┘

  EXPRESSION LOWERING (via IrEmitter):
  ┌────────────────────────────────────────────────────────────────┐
  │ Step 1: emit_expression(BinaryExpr Add)                        │
  │   ├─ Step 1a: emit_expression(LiteralExpr 5)                   │
  │   │   ExpDesc { k=Num, u.num=5 }                               │
  │   │   → bcemit_AD(BC_KSHORT, R2, #5)                           │
  │   │   ExpDesc { k=NonReloc, u.s.info=R2 }                      │
  │   │                                                            │
  │   └─ Step 1b: emit_expression(BinaryExpr Mul)                  │
  │       ├─ emit_expression(IdentifierExpr "a")                   │
  │       │   Resolve: local variable at register R0               │
  │       │   ExpDesc { k=Local, u.s.info=R0 }                     │
  │       │                                                        │
  │       └─ emit_expression(LiteralExpr 2)                        │
  │           ExpDesc { k=Num, u.num=2 }                           │
  │           → bcemit_AD(BC_KSHORT, R3, #2)                       │
  │           ExpDesc { k=NonReloc, u.s.info=R3 }                  │
  │                                                                │
  │       OperatorEmitter::emit_binary(Mul, R0, R3)                │
  │       → bcemit_ABC(BC_MULVV, R4, R0, R3)                       │
  │       ExpDesc { k=NonReloc, u.s.info=R4 }                      │
  │                                                                │
  │   OperatorEmitter::emit_binary(Add, R2, R4)                    │
  │   → bcemit_ABC(BC_ADDVV, R5, R2, R4)                           │
  │   ExpDesc { k=NonReloc, u.s.info=R5 }                          │
  │                                                                │
  │ Step 2: emit_assignment(x, R5)                                 │
  │   Resolve x: local variable at R1                              │
  │   → bcemit_ABC(BC_MOV, R1, R5)                                 │
  └────────────────────────────────────────────────────────────────┘

  GENERATED BYTECODE:
  ┌────────────────────────────────────────────────────────────────┐
  │ 0001  KSHORT   A=R2 D=#5       ; Load constant 5               │
  │ 0002  KSHORT   A=R3 D=#2       ; Load constant 2               │
  │ 0003  MULVV    A=R4 B=R0 C=R3  ; R4 = a * 2                    │
  │ 0004  ADDVV    A=R5 B=R2 C=R4  ; R5 = 5 + R4                   │
  │ 0005  MOV      A=R1 B=R5       ; x = R5                        │
  └────────────────────────────────────────────────────────────────┘

  REGISTER ALLOCATION STATE:
  ┌────────────────────────────────────────────────────────────────┐
  │ nactvar = 2   (locals: a@R0, x@R1)                             │
  │ freereg = 2   (first free temporary)                           │
  │                                                                │
  │ R0: a          (local variable)                                │
  │ R1: x          (local variable)                                │
  │ R2: temp       (constant 5)                                    │
  │ R3: temp       (constant 2)                                    │
  │ R4: temp       (a * 2)                                         │
  │ R5: temp       (5 + a*2)                                       │
  │                                                                │
  │ Invariant: freereg >= nactvar                                  │
  │ (temporaries allocated above local variables)                  │
  └────────────────────────────────────────────────────────────────┘

  Diagram 3: JIT Trace Recording Detail

  ┌─────────────────────────────────────────────────────────────────────┐
  │                    JIT TRACE RECORDING PROCESS                      │
  └─────────────────────────────────────────────────────────────────────┘

  BYTECODE (Hot loop detected):
  ┌────────────────────────────────────────────────────────────────┐
  │ function sum(n)                                                │
  │   local total = 0                                              │
  │   for i = 1, n do                                              │
  │     total = total + i                                          │
  │   end                                                          │
  │   return total                                                 │
  │ end                                                            │
  │                                                                │
  │ BYTECODE:                                                      │
  │ 0001  KSHORT  A=R0 D=#0        ; total = 0                     │
  │ 0002  KSHORT  A=R1 D=#1        ; i = 1                         │
  │ 0003  MOV     A=R2 B=R0        ; Loop limit = n                │
  │ 0004  KSHORT  A=R3 D=#1        ; Step = 1                      │
  │ 0005  FORI    A=R1 D=→0009     ; for setup                     │
  │ 0006→ ADDVV   A=R4 B=R0 C=R1   ; total + i                     │
  │ 0007  MOV     A=R0 B=R4        ; total = R4                    │
  │ 0008  FORL    A=R1 D=→0006     ; Loop ← HOT TRACE              │
  │ 0009  RET1    A=R0 D=#2        ; return total                  │
  └────────────────────────────────────────────────────────────────┘

  After 56 iterations, JIT compiler triggers:

  ┌────────────────────────────────────────────────────────────────┐
  │ TRACE RECORDING (lj_record.cpp)                                │
  │                                                                │
  │ Record bytecode 0006-0008:                                     │
  │                                                                │
  │ BC: ADDVV A=R4 B=R0 C=R1                                       │
  │ ├─ lj_record_ins(BC_ADDVV)                                     │
  │ │  ├─ TRef a = getslot(R0)     → IR ref #5                     │
  │ │  ├─ TRef b = getslot(R1)     → IR ref #6                     │
  │ │  ├─ Type guard: check R0 is number                           │
  │ │  │  → emitir(IR_GT, #5, IR_KNUM)                             │
  │ │  ├─ Type guard: check R1 is number                           │
  │ │  │  → emitir(IR_GT, #6, IR_KNUM)                             │
  │ │  ├─ Emit addition                                            │
  │ │  │  → TRef result = emitir(IR_ADD, #5, #6) → IR ref #9       │
  │ │  └─ setslot(R4, #9)                                          │
  │ │                                                              │
  │ BC: MOV A=R0 B=R4                                              │
  │ ├─ lj_record_ins(BC_MOV)                                       │
  │ │  └─ setslot(R0, getslot(R4))                                 │
  │ │                                                              │
  │ BC: FORL A=R1 D=→0006                                          │
  │ ├─ lj_record_ins(BC_FORL)                                      │
  │ │  ├─ TRef idx = getslot(R1)   → IR ref #6                     │
  │ │  ├─ TRef lim = getslot(R2)   → IR ref #7                     │
  │ │  ├─ TRef step = getslot(R3)  → IR ref #8                     │
  │ │  ├─ Emit increment                                           │
  │ │  │  → TRef newidx = emitir(IR_ADD, #6, #8) → IR ref #10      │
  │ │  ├─ Emit loop condition                                      │
  │ │  │  → emitir(IR_LE, #10, #7)  (guard: i <= n)                │
  │ │  ├─ setslot(R1, #10)                                         │
  │ │  └─ emitir(IR_LOOP)           ; Close trace                  │
  │ └──────────────────────────────────────────────────────────────│
  │                                                                │
  │ SNAPSHOT: VM state captured for side exits                     │
  │   Snap #1: [R0=#5, R1=#6, R2=#7, R3=#8]                        │
  └────────────────────────────────────────────────────────────────┘

  SSA IR OUTPUT:
  ┌────────────────────────────────────────────────────────────────┐
  │ 0001  SLOAD   #0   T           ; Load total (type: number)     │
  │ 0002  SLOAD   #1   T           ; Load i (type: number)         │
  │ 0003  SLOAD   #2   T           ; Load n (type: number)         │
  │ 0004  SLOAD   #3   T           ; Load step (type: number)      │
  │ 0005  IR_GT   #0   IR_KNUM     ; Guard: total is number        │
  │ 0006  IR_GT   #1   IR_KNUM     ; Guard: i is number            │
  │ 0007  IR_ADD  #0   #1          ; total + i                     │
  │ 0008  IR_ADD  #1   #3          ; i + 1                         │
  │ 0009  IR_LE   #8   #2          ; Guard: i <= n                 │
  │ 0010  IR_LOOP                  ; Loop header                   │
  │ 0011  IR_PHI  #0   #7          ; total = φ(initial, #7)        │
  │ 0012  IR_PHI  #1   #8          ; i = φ(initial, #8)            │
  └────────────────────────────────────────────────────────────────┘

  IR OPTIMIZATION (lj_opt_fold.cpp):
  ┌────────────────────────────────────────────────────────────────┐
  │ • Fold: IR_ADD(IR_KINT, IR_KINT) → IR_KINT                     │
  │ • Narrow: Type analysis (int32 sufficient for i)               │
  │ • DCE: Remove unused guards                                    │
  │ • LICM: Hoist loop-invariant loads                             │
  └────────────────────────────────────────────────────────────────┘

  ASSEMBLY GENERATION (lj_asm_x86.h):
  ┌────────────────────────────────────────────────────────────────┐
  │ REGISTER ALLOCATION:                                           │
  │   rax = total                                                  │
  │   rbx = i                                                      │
  │   rcx = n                                                      │
  │                                                                │
  │ x64 MACHINE CODE:                                              │
  │ ->LOOP:                                                        │
  │   cmp byte [rax], 0x03      ; Type guard: is number?           │
  │   jne ->exit1               ; Side exit if not                 │
  │   cmp byte [rbx], 0x03      ; Type guard: is number?           │
  │   jne ->exit2               ; Side exit if not                 │
  │   mov rdx, [rax+8]          ; Load total value                 │
  │   add rdx, [rbx+8]          ; total += i                       │
  │   mov [rax+8], rdx          ; Store total                      │
  │   add qword [rbx+8], 1      ; i++                              │
  │   cmp [rbx+8], rcx          ; i <= n?                          │
  │   jle ->LOOP                ; Continue loop                    │
  │   ret                       ; Exit trace                       │
  │                                                                │
  │ ->exit1:                                                       │
  │   ; Restore VM state from snapshot                             │
  │   ; Return to bytecode interpreter                             │
  └────────────────────────────────────────────────────────────────┘

  TRACE LINKING:
  ┌────────────────────────────────────────────────────────────────┐
  │ Original bytecode patched:                                     │
  │ 0008  JLOOP   A=R1 D=→trace#1  ; Jump to native code           │
  │                                                                │
  │ VM dispatcher now redirects hot loop to machine code           │
  │ Side exits return to interpreter at specific bytecode PC       │
  └────────────────────────────────────────────────────────────────┘

  Summary

  The Tiri LuaJIT compilation pipeline consists of 4 main stages:

  1. Lexical Analysis (lexer.cpp) - Tokenizes source code using C++20 constexpr token definitions
  2. AST Construction (parser.cpp, ast_builder.cpp) - Builds strongly-typed syntax tree
  3. IR Emission (ir_emitter.cpp) - Lowers AST to stack-based bytecode with register allocation
  4. JIT Compilation (lj_trace.cpp, lj_record.cpp, lj_asm.cpp) - Traces hot paths, optimizes to SSA IR, generates native machine code

  The system uses modern C++20 features throughout (concepts, constexpr, RAII) and achieves high performance through aggressive JIT optimization while maintaining clean separation of compilation stages.
