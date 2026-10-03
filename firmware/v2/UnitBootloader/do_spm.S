; Split-flap #499: the callable self-program primitive placed at a fixed address
; (0x7F80, start of page 7) in the second-generation twiboot image.
;
; The fielded twiboot has no app-callable do_spm; the application can only run
; SPM from the boot section, where the only code is twiboot's own write handler.
; Stage 1 of the in-system update installs exactly these bytes into the empty
; page 7 of the fielded image (by driving twiboot's write handler), and stage 2
; then calls here to rewrite pages 0-6. The same bytes are also page 7 of the
; new image, so there is one source of truth for page 7.
;
; Optiboot calling convention (avr-gcc ABI):
;   r24:r25 = address, r22 = SPMCSR action, r20:r21 = data word (fill)
; Caller holds interrupts off. Position-independent (only rjmp 1b is relative),
; so it may be assembled anywhere and placed at 0x7F80 by the image builder.

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
