.syntax unified
.cpu cortex-m4
.fpu softvfp
.thumb

.global context_switch
.thumb_func

context_switch:
    // Assume r0 = pointer to stackptr (thread stack)
    ldmia r0!, {r4-r11}
    msr psp, r0
    mov r0, #2            // CONTROL.SPSEL = 1
    msr control, r0
    isb
    mov lr, #0xFFFFFFFD
    bx lr
