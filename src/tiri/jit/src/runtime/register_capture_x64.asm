; Register capture and verification for VM assembly unit tests (Windows x64 MASM)
; Captures callee-saved registers before/after function calls to detect corruption.
;
; Windows x64 calling convention callee-saved registers:
;   General purpose: RBX, RBP, RDI, RSI, R12, R13, R14, R15
;   XMM: XMM6-XMM15 (128-bit, only lower 128 bits are callee-saved)
;
; This file provides functions to capture and verify these registers.

.code

; Structure layout for RegisterSnapshot (must match C++ definition)
; Offsets for 64-bit registers (8 bytes each):
;   0: rbx, 8: rbp, 16: rdi, 24: rsi, 32: r12, 40: r13, 48: r14, 56: r15, 64: rsp
; Offsets for XMM registers (16 bytes each, starting at offset 80):
;   80: xmm6, 96: xmm7, 112: xmm8, 128: xmm9, 144: xmm10, 160: xmm11, 176: xmm12, 192: xmm13, 208: xmm14, 224: xmm15

RBX_OFF     EQU 0
RBP_OFF     EQU 8
RDI_OFF     EQU 16
RSI_OFF     EQU 24
R12_OFF     EQU 32
R13_OFF     EQU 40
R14_OFF     EQU 48
R15_OFF     EQU 56
RSP_OFF     EQU 64
XMM6_OFF    EQU 80
XMM7_OFF    EQU 96
XMM8_OFF    EQU 112
XMM9_OFF    EQU 128
XMM10_OFF   EQU 144
XMM11_OFF   EQU 160
XMM12_OFF   EQU 176
XMM13_OFF   EQU 192
XMM14_OFF   EQU 208
XMM15_OFF   EQU 224

;------------------------------------------------------------------------------
; void asm_capture_registers(RegisterSnapshot* snap)
;
; Captures all callee-saved registers into the provided snapshot structure.
; RCX = pointer to RegisterSnapshot
;------------------------------------------------------------------------------
asm_capture_registers PROC
    ; Save general-purpose callee-saved registers
    mov     QWORD PTR [rcx + RBX_OFF], rbx
    mov     QWORD PTR [rcx + RBP_OFF], rbp
    mov     QWORD PTR [rcx + RDI_OFF], rdi
    mov     QWORD PTR [rcx + RSI_OFF], rsi
    mov     QWORD PTR [rcx + R12_OFF], r12
    mov     QWORD PTR [rcx + R13_OFF], r13
    mov     QWORD PTR [rcx + R14_OFF], r14
    mov     QWORD PTR [rcx + R15_OFF], r15

    ; Save RSP (adjusted for return address that was pushed)
    lea     rax, [rsp + 8]
    mov     QWORD PTR [rcx + RSP_OFF], rax

    ; Save XMM callee-saved registers (XMM6-XMM15)
    movaps  XMMWORD PTR [rcx + XMM6_OFF], xmm6
    movaps  XMMWORD PTR [rcx + XMM7_OFF], xmm7
    movaps  XMMWORD PTR [rcx + XMM8_OFF], xmm8
    movaps  XMMWORD PTR [rcx + XMM9_OFF], xmm9
    movaps  XMMWORD PTR [rcx + XMM10_OFF], xmm10
    movaps  XMMWORD PTR [rcx + XMM11_OFF], xmm11
    movaps  XMMWORD PTR [rcx + XMM12_OFF], xmm12
    movaps  XMMWORD PTR [rcx + XMM13_OFF], xmm13
    movaps  XMMWORD PTR [rcx + XMM14_OFF], xmm14
    movaps  XMMWORD PTR [rcx + XMM15_OFF], xmm15

    ret
asm_capture_registers ENDP

;------------------------------------------------------------------------------
; int asm_verify_registers(const RegisterSnapshot* before, const RegisterSnapshot* after)
;
; Compares two register snapshots and returns a bitmask of corrupted registers.
; RCX = pointer to 'before' snapshot
; RDX = pointer to 'after' snapshot
; Returns: 0 if all registers match, otherwise bitmask of mismatched registers
;
; Bitmask values:
;   Bit 0: RBX, Bit 1: RBP, Bit 2: RDI, Bit 3: RSI
;   Bit 4: R12, Bit 5: R13, Bit 6: R14, Bit 7: R15, Bit 8: RSP
;   Bit 9: XMM6, Bit 10: XMM7, Bit 11: XMM8, Bit 12: XMM9
;   Bit 13: XMM10, Bit 14: XMM11, Bit 15: XMM12, Bit 16: XMM13
;   Bit 17: XMM14, Bit 18: XMM15
;------------------------------------------------------------------------------
asm_verify_registers PROC
    xor     eax, eax            ; Result bitmask = 0

    ; Check RBX
    mov     r8, QWORD PTR [rcx + RBX_OFF]
    cmp     r8, QWORD PTR [rdx + RBX_OFF]
    je      @check_rbp
    or      eax, 1              ; Bit 0

@check_rbp:
    mov     r8, QWORD PTR [rcx + RBP_OFF]
    cmp     r8, QWORD PTR [rdx + RBP_OFF]
    je      @check_rdi
    or      eax, 2              ; Bit 1

@check_rdi:
    mov     r8, QWORD PTR [rcx + RDI_OFF]
    cmp     r8, QWORD PTR [rdx + RDI_OFF]
    je      @check_rsi
    or      eax, 4              ; Bit 2

@check_rsi:
    mov     r8, QWORD PTR [rcx + RSI_OFF]
    cmp     r8, QWORD PTR [rdx + RSI_OFF]
    je      @check_r12
    or      eax, 8              ; Bit 3

@check_r12:
    mov     r8, QWORD PTR [rcx + R12_OFF]
    cmp     r8, QWORD PTR [rdx + R12_OFF]
    je      @check_r13
    or      eax, 16             ; Bit 4

@check_r13:
    mov     r8, QWORD PTR [rcx + R13_OFF]
    cmp     r8, QWORD PTR [rdx + R13_OFF]
    je      @check_r14
    or      eax, 32             ; Bit 5

@check_r14:
    mov     r8, QWORD PTR [rcx + R14_OFF]
    cmp     r8, QWORD PTR [rdx + R14_OFF]
    je      @check_r15
    or      eax, 64             ; Bit 6

@check_r15:
    mov     r8, QWORD PTR [rcx + R15_OFF]
    cmp     r8, QWORD PTR [rdx + R15_OFF]
    je      @check_rsp
    or      eax, 128            ; Bit 7

@check_rsp:
    mov     r8, QWORD PTR [rcx + RSP_OFF]
    cmp     r8, QWORD PTR [rdx + RSP_OFF]
    je      @check_xmm6
    or      eax, 256            ; Bit 8

    ; Check XMM registers using PCMPEQQ (compare 64-bit integers)
    ; We need to compare both 64-bit halves of each XMM register

@check_xmm6:
    movaps  xmm0, XMMWORD PTR [rcx + XMM6_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM6_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm7
    or      eax, 512            ; Bit 9

@check_xmm7:
    movaps  xmm0, XMMWORD PTR [rcx + XMM7_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM7_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm8
    or      eax, 1024           ; Bit 10

@check_xmm8:
    movaps  xmm0, XMMWORD PTR [rcx + XMM8_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM8_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm9
    or      eax, 2048           ; Bit 11

@check_xmm9:
    movaps  xmm0, XMMWORD PTR [rcx + XMM9_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM9_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm10
    or      eax, 4096           ; Bit 12

@check_xmm10:
    movaps  xmm0, XMMWORD PTR [rcx + XMM10_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM10_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm11
    or      eax, 8192           ; Bit 13

@check_xmm11:
    movaps  xmm0, XMMWORD PTR [rcx + XMM11_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM11_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm12
    or      eax, 16384          ; Bit 14

@check_xmm12:
    movaps  xmm0, XMMWORD PTR [rcx + XMM12_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM12_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm13
    or      eax, 32768          ; Bit 15

@check_xmm13:
    movaps  xmm0, XMMWORD PTR [rcx + XMM13_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM13_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm14
    or      eax, 65536          ; Bit 16

@check_xmm14:
    movaps  xmm0, XMMWORD PTR [rcx + XMM14_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM14_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @check_xmm15
    or      eax, 131072         ; Bit 17

@check_xmm15:
    movaps  xmm0, XMMWORD PTR [rcx + XMM15_OFF]
    movaps  xmm1, XMMWORD PTR [rdx + XMM15_OFF]
    pcmpeqq xmm0, xmm1
    pmovmskb r8d, xmm0
    cmp     r8d, 0FFFFh
    je      @done
    or      eax, 262144         ; Bit 18

@done:
    ret
asm_verify_registers ENDP

;------------------------------------------------------------------------------
; int asm_call_and_capture(RegisterSnapshot* before, RegisterSnapshot* after,
;    int (*fn)(void*), void* ctx)
;
; Captures registers, invokes the supplied function pointer and captures the
; registers again immediately after the call. Returns the function result.
;------------------------------------------------------------------------------
asm_call_and_capture PROC
    ; RCX=before, RDX=after, R8=fn, R9=ctx
    sub     rsp, 32                  ; shadow space, stack stays 16-byte aligned

    mov     r10, rcx                 ; preserve snapshot pointers across calls
    mov     r11, rdx

    mov     rcx, r10
    call    asm_capture_registers

    mov     rcx, r9                  ; ctx -> RCX
    call    r8                       ; call fn(ctx)
    mov     r10, rax                 ; save return value

    mov     rcx, r11
    call    asm_capture_registers

    mov     rax, r10
    add     rsp, 32
    ret
asm_call_and_capture ENDP

END
