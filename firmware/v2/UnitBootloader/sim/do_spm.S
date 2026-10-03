.section .text
.global do_spm
do_spm:
  movw r30, r24      ; Z = addr
  movw r0, r20       ; r0:r1 = data word (fill)
  out 0x37, r22      ; SPMCSR = action
  spm
1:in r23, 0x37
  sbrc r23, 0
  rjmp 1b
  clr r1
  ret
