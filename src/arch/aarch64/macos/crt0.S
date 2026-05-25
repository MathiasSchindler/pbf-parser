.globl _start
.text
.p2align 2
_start:
    ldr x0, [sp]
    add x1, sp, #8
    bl _main
    mov x16, #1
    svc #0x80
