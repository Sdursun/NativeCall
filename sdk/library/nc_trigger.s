; nc_trigger(a0 = 64-byte aligned call block)
;
; Runs CPUSHL DC,(A0) in supervisor mode with the doorbell in D0/D1. On Emu68 this becomes
; "dc civac" on the block, which the nativecall hypervisor traps (NOTES.md 9); it only looks at
; the block when D0/D1 hold the doorbell (nc_abi.h). Anywhere else it is a plain cache push.
; Caller checks AttnFlags for a 68040/68060 first (CPUSHL is line-F on older CPUs).

_LVOSupervisor  equ     -30
NC_DOORBELL_D0  equ     $4e43414c               ; 'NCAL'
NC_DOORBELL_D1  equ     $b1bcbeb3               ; ~'NCAL'

                section code,code

                xdef    _nc_trigger
_nc_trigger:
                movem.l a5/a6,-(sp)
                move.l  4.w,a6
                lea     .super(pc),a5
                jsr     _LVOSupervisor(a6)
                movem.l (sp)+,a5/a6
                rts

.super:         move.l  d0,-(sp)
                move.l  d1,-(sp)
                move.l  #NC_DOORBELL_D0,d0
                move.l  #NC_DOORBELL_D1,d1
                dc.w    $f468                   ; CPUSHL DC,(A0)
                nop
                move.l  (sp)+,d1
                move.l  (sp)+,d0
                rte
