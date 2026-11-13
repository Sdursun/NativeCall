; Bare 68k test for the nativecall hypervisor under QEMU (no AmigaOS).
; Emu68 (raspi64, VARIANT none) loads this hunk executable from the initrd and starts it in
; supervisor mode. Output goes to the PL011 that Emu68 maps at $F2201000 for the 68k.
;
; Sequence: PING, SYNC (module), CALL add(40,2), CALL fault, INFO. Each line is
;   <name> result=XXXXXXXX status=XXXXXXXX
; and the run ends with "NCTEST END".

UART_DR         equ     $f2201000

B_MAGIC         equ     0
B_OP            equ     4
B_ARG0          equ     8
B_ARG1          equ     12
B_ARG2          equ     16
B_RESULT        equ     20
B_STATUS        equ     24
B_VERSION       equ     28
B_INFO          equ     32

                section code,code

start:
                lea     s_hello(pc),a0
                bsr     puts

                ; 64-byte aligned call block
                lea     blockbuf,a2
                move.l  a2,d0
                add.l   #63,d0
                and.l   #$ffffffc0,d0
                move.l  d0,a2

                ; 16-byte aligned copy of hello.narm
                lea     modbuf,a3
                move.l  a3,d0
                add.l   #15,d0
                and.l   #$fffffff0,d0
                move.l  d0,a3
                lea     narm_module,a0
                move.l  a3,a1
                move.w  #(narm_module_end-narm_module+3)/4-1,d0
.copy:          move.l  (a0)+,(a1)+
                dbf     d0,.copy
                move.l  a3,modbase

                ; 16-byte aligned params {40, 2}
                lea     parbuf,a4
                move.l  a4,d0
                add.l   #15,d0
                and.l   #$fffffff0,d0
                move.l  d0,a4
                move.l  #40,(a4)
                move.l  #2,4(a4)

                ; SPOOF: data that looks like a request, flushed without the doorbell in D0/D1
                ; (what a DMA buffer holding one of our binaries looks like). The hypervisor
                ; must leave it alone: magic still 'NCAL', status still 0.
                move.l  #$4e43414c,B_MAGIC(a2)
                move.l  #1,B_OP(a2)
                move.l  #-1,B_RESULT(a2)
                clr.l   B_STATUS(a2)
                move.l  a2,a0
                moveq   #0,d0
                moveq   #0,d1
                dc.w    $f468                   ; CPUSHL DC,(A0) without doorbell
                nop
                move.l  B_MAGIC(a2),d0
                move.l  B_STATUS(a2),d1
                lea     s_spoof(pc),a0
                bsr     report

                ; PING
                moveq   #1,d0
                moveq   #0,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                lea     s_ping(pc),a0
                bsr     report
                move.l  B_INFO+28(a2),features  ; hypervisor 6: NC_FEAT_* (0 before)
                lea     s_feat(pc),a0
                bsr     puts
                move.l  features,d0
                bsr     puthex
                bsr     newline

                ; SYNC module
                moveq   #2,d0
                move.l  a3,d1
                move.l  #narm_module_end-narm_module,d2
                moveq   #0,d3
                bsr     request
                lea     s_sync(pc),a0
                bsr     report

                ; CALL add(40, 2): entry 0
                moveq   #3,d0
                move.l  a3,d1
                add.l   12(a3),d1
                move.l  a4,d2
                move.l  a3,d3
                bsr     request
                lea     s_add(pc),a0
                bsr     report
                lea     s_ticks(pc),a0          ; hypervisor 2: native run time of the call
                bsr     puts
                move.l  B_INFO+24(a2),d0
                bsr     puthex
                lea     s_freq(pc),a0
                bsr     puts
                move.l  B_INFO+28(a2),d0
                bsr     puthex
                bsr     newline

                ; CALL fault: entry 1, must come back with status $84
                moveq   #3,d0
                move.l  a3,d1
                add.l   16(a3),d1
                move.l  a4,d2
                move.l  a3,d3
                bsr     request
                lea     s_fault(pc),a0
                bsr     report
                lea     s_esr(pc),a0
                bsr     puts
                move.l  B_INFO+4(a2),d0
                bsr     puthex
                lea     s_far(pc),a0
                bsr     puts
                move.l  B_INFO+12(a2),d0
                bsr     puthex
                bsr     newline

                ; CALL with a function that is not an entry: status $82
                moveq   #3,d0
                move.l  a3,d1
                add.l   #4,d1
                move.l  a4,d2
                move.l  a3,d3
                bsr     request
                lea     s_badentry(pc),a0
                bsr     report

                ; 1000 x add
                move.l  #999,d6
                moveq   #0,d5                   ; errors
.loop:          move.l  d6,(a4)
                moveq   #3,d0
                move.l  a3,d1
                add.l   12(a3),d1
                move.l  a4,d2
                move.l  a3,d3
                bsr     request
                move.l  d6,d4
                addq.l  #2,d4
                cmp.l   d4,d0
                beq.s   .ok
                addq.l  #1,d5
.ok:            dbf     d6,.loop
                lea     s_loop(pc),a0
                bsr     puts
                move.l  d5,d0
                bsr     puthex
                bsr     newline

                ; INFO
                moveq   #4,d0
                moveq   #0,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                lea     s_info(pc),a0
                bsr     report
                lea     s_traps(pc),a0
                bsr     puts
                move.l  B_INFO(a2),d0
                bsr     puthex
                lea     s_calls(pc),a0
                bsr     puts
                move.l  B_INFO+4(a2),d0
                bsr     puthex
                lea     s_faults(pc),a0
                bsr     puts
                move.l  B_INFO+8(a2),d0
                bsr     puthex
                bsr     newline

                ; PING again: helper cores ready for RUNMP (hypervisor 3)
                moveq   #1,d0
                moveq   #0,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                lea     s_helpers(pc),a0
                bsr     puts
                move.l  B_INFO(a2),d0
                bsr     puthex
                bsr     newline

                ; RUNMP mark over [0, 4096), pieces of 64, some work per unit
                lea     markbuf,a5
                move.l  a5,d0
                add.l   #15,d0
                and.l   #$fffffff0,d0
                move.l  d0,a5
                move.l  a5,a0
                move.w  #4095,d6
.clr:           clr.l   (a0)+
                dbf     d6,.clr
                clr.l   (a4)
                move.l  #4096,4(a4)
                move.l  a5,8(a4)
                move.l  #100000,12(a4)          ; ~1 s: longer than a QEMU time slice
                bsr     runmp_mark
                lea     s_runmp(pc),a0
                bsr     report
                lea     s_cores(pc),a0
                bsr     puts
                move.l  B_INFO(a2),d0
                bsr     puthex
                lea     s_helped(pc),a0
                bsr     puts
                move.l  B_INFO+4(a2),d0
                bsr     puthex
                lea     s_ticks(pc),a0
                bsr     puts
                move.l  B_INFO+24(a2),d0
                bsr     puthex
                bsr     newline

                ; units per core (cnt[0]: units nobody marked)
                lea     cnt,a1
                clr.l   (a1)
                clr.l   4(a1)
                clr.l   8(a1)
                clr.l   12(a1)
                clr.l   16(a1)
                move.l  a5,a0
                move.w  #4095,d6
.count:         move.l  (a0)+,d0
                cmp.l   #4,d0
                bls.s   .valid
                moveq   #0,d0
.valid:         lsl.l   #2,d0
                addq.l  #1,(a1,d0.l)
                dbf     d6,.count
                lea     s_percore(pc),a0
                bsr     puts
                moveq   #1,d6
.pc:            move.l  (a1,d6.l*4),d0
                bsr     puthex
                moveq   #' ',d0
                bsr     putc
                addq.l  #1,d6
                cmp.l   #5,d6
                bne.s   .pc
                lea     s_missing(pc),a0
                bsr     puts
                move.l  (a1),d0
                bsr     puthex
                bsr     newline

                ; RUNMP with a bad destination: the core that faults first is reported
                move.l  #4,8(a4)
                bsr     runmp_mark
                lea     s_runmpflt(pc),a0
                bsr     report

                ; 200 x RUNMP over [0, 256), no extra work
                move.l  a5,8(a4)
                clr.l   12(a4)
                move.l  #256,4(a4)
                move.l  #199,d6
                moveq   #0,d5
.mploop:        bsr     runmp_mark
                cmp.l   #256,d0
                bne.s   .mperr
                cmp.l   #1,d1
                beq.s   .mpok
.mperr:         addq.l  #1,d5
.mpok:          dbf     d6,.mploop
                lea     s_mploop(pc),a0
                bsr     puts
                move.l  d5,d0
                bsr     puthex
                bsr     newline

                ; helper statistics: pieces and idle "wfe" traps of cores 1-3
                moveq   #1,d0
                moveq   #0,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                lea     s_hstat(pc),a0
                bsr     puts
                moveq   #1,d6
.hs:            move.l  B_INFO(a2,d6.l*4),d0
                bsr     puthex
                moveq   #' ',d0
                bsr     putc
                addq.l  #1,d6
                cmp.l   #7,d6
                bne.s   .hs
                bsr     newline

                ; ---- asynchronous jobs (hypervisor 4) ----
                lea     jobstat,a6
                move.l  a6,d0
                add.l   #15,d0
                and.l   #$fffffff0,d0
                move.l  d0,a6                   ; a6 = struct NCJobStatus (16 bytes)

                ; START mark over [0, 4096) with ~1 s of work; the 68k counts meanwhile
                move.l  a5,a0
                move.w  #4095,d6
.aclr:          clr.l   (a0)+
                dbf     d6,.aclr
                clr.l   (a4)
                move.l  #4096,4(a4)
                move.l  a5,8(a4)
                move.l  #100000,12(a4)
                bsr     start_mark
                lea     s_astart(pc),a0
                bsr     report
                move.l  d0,d7                   ; job number
                moveq   #0,d5                   ; 68k loop count while the job runs
                cmp.l   #1,d1                   ; not started (no hypervisor 4): nothing to wait for
                bne.s   .adone
.aspin:         tst.l   (a6)
                bne.s   .adone
                addq.l  #1,d5
                cmp.l   #$7fffffff,d5
                bne.s   .aspin
.adone:         lea     s_a68k(pc),a0
                bsr     puts
                move.l  d5,d0
                bsr     puthex
                bsr     newline
                bsr     wait_job                ; d0 result, d1 status, d4 WAIT requests
                lea     s_await(pc),a0
                bsr     report
                lea     s_cores(pc),a0
                bsr     puts
                move.l  B_INFO(a2),d0
                bsr     puthex
                lea     s_waits(pc),a0
                bsr     puts
                move.l  d4,d0
                bsr     puthex
                bsr     newline
                bsr     count_units

                ; five STARTs: the fifth finds all four slots taken
                move.l  #64,4(a4)
                moveq   #3,d6
                lea     jobs,a3                 ; a3 is not needed from here on: module in a1
                move.l  a3,-(sp)
.a5:            bsr     start_mark
                move.l  (sp),a0
                move.l  d0,(a0)+
                move.l  a0,(sp)
                dbf     d6,.a5
                addq.l  #4,sp
                bsr     start_mark
                lea     s_abusy(pc),a0
                bsr     report
                lea     jobs,a3
                moveq   #3,d6
.aw:            move.l  (a3)+,d7
                bsr     wait_job
                dbf     d6,.aw
                lea     s_awall(pc),a0
                bsr     report

                ; a job that faults
                clr.l   (a4)
                move.l  #256,4(a4)
                move.l  #4,8(a4)
                clr.l   12(a4)
                bsr     start_mark
                move.l  d0,d7
                bsr     wait_job
                lea     s_afault(pc),a0
                bsr     report

                ; WAIT on a job that does not exist
                moveq   #7,d0
                moveq   #3,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                lea     s_abad(pc),a0
                bsr     report

                ; ---- time limit (hypervisor 6): hello's spin (entry 5) never returns ----
                ; Only where the PING said the limit works (stub; not with loader.S, which has
                ; no Secure GIC setup): elsewhere the spin would never end.
                btst    #0,features+3
                bne.s   .tmo
                lea     s_tmoskip(pc),a0
                bsr     puts
                bra     .tmoend
.tmo:
                ; CALL spin with 20 ms: status $87, ELR inside the module
                move.l  #20000,B_INFO+20(a2)
                moveq   #3,d0
                move.l  modbase,d1
                move.l  d1,a0
                add.l   32(a0),d1
                move.l  a4,d2
                move.l  modbase,d3
                bsr     request
                lea     s_tmo(pc),a0
                bsr     report
                lea     s_elr(pc),a0            ; ELR - spin's entry: a few bytes
                bsr     puts
                move.l  B_INFO+20(a2),d0
                move.l  modbase,a0
                sub.l   a0,d0
                sub.l   32(a0),d0
                bsr     puthex
                bsr     newline

                ; the machine goes on: add(40, 2) with the default limit
                move.l  #40,(a4)
                move.l  #2,4(a4)
                clr.l   B_INFO+20(a2)
                moveq   #3,d0
                move.l  modbase,d1
                move.l  d1,a0
                add.l   12(a0),d1
                move.l  a4,d2
                move.l  modbase,d3
                bsr     request
                lea     s_tmoadd(pc),a0
                bsr     report

                ; RUNMP spin over [0, 256), 20 ms per piece: core 0 and the helpers stopped
                clr.l   (a4)
                move.l  #256,4(a4)
                move.l  #20000,B_INFO+20(a2)
                moveq   #32,d4
                bsr     runmp_entry
                lea     s_tmomp(pc),a0
                bsr     report

                ; START spin, WAIT until it ends: the helpers' pieces stopped, the job completes
                move.l  #20000,B_INFO+20(a2)
                moveq   #32,d4
                bsr     start_entry
                move.l  d0,d7
                bsr     wait_job
                lea     s_tmoasync(pc),a0
                bsr     report
                lea     s_jstat(pc),a0          ; the status block says so too
                bsr     puts
                move.l  8(a6),d0
                bsr     puthex
                bsr     newline

                ; and everything still works: RUNMP mark with the default limit
                clr.l   B_INFO+20(a2)
                move.l  a5,8(a4)
                clr.l   12(a4)
                moveq   #24,d4
                bsr     runmp_entry
                lea     s_tmook(pc),a0
                bsr     report

.tmoend:
                ; ---- log and fault details (hypervisor 7) ----
                ; CALL logtest (entry 6): nc_printf into the hypervisor's log
                moveq   #36,d4
                bsr     call_entry
                lea     s_logcall(pc),a0
                bsr     report
                ; LOGREAD from 0: everything since boot, printed as it is
                moveq   #8,d0
                move.l  #logbuf,d1
                move.l  #16384,d2
                moveq   #0,d3
                bsr     request
                move.l  B_INFO(a2),logpos
                lea     s_logread(pc),a0
                bsr     report
                bsr     print_log

                ; CALL deepfault (entry 7): a crash three functions deep
                moveq   #40,d4
                bsr     call_entry
                lea     s_deep(pc),a0
                bsr     report
                ; the hypervisor's line about it
                moveq   #8,d0
                move.l  #logbuf,d1
                move.l  #16384,d2
                move.l  logpos,d3
                bsr     request
                bsr     print_log
                ; FAULTINFO: status, FAR, x0 (the bad pointer), backtrace
                moveq   #9,d0
                move.l  #fibuf,d1
                move.l  #336,d2
                moveq   #0,d3
                bsr     request
                lea     s_finfo(pc),a0
                bsr     report
                cmp.l   #1,d1
                bne.s   .nofi
                lea     fibuf,a5
                lea     s_fst(pc),a0
                bsr     puts
                move.l  4(a5),d0
                bsr     puthex
                lea     s_ffar(pc),a0
                bsr     puts
                move.l  28(a5),d0
                bsr     puthex
                lea     s_fx0(pc),a0
                bsr     puts
                move.l  52(a5),d0
                bsr     puthex
                bsr     newline
                lea     s_bt(pc),a0
                bsr     puts
                lea     296(a5),a1
                moveq   #9,d6
.bt:            moveq   #' ',d0
                bsr     putc
                move.l  (a1)+,d0
                bsr     puthex
                dbf     d6,.bt
                bsr     newline
.nofi:
                ; ---- memory protection (hypervisor 7): hello's protect (entry 8), modes 0-2 ----
                moveq   #0,d7
.prot:          move.l  d7,(a4)
                clr.l   4(a4)
                moveq   #44,d4
                bsr     call_entry
                lea     s_prot(pc),a0
                bsr     puts
                move.l  d7,d0
                add.b   #'0',d0
                bsr     putc
                lea     s_result(pc),a0
                bsr     puts
                move.l  d0,-(sp)
                move.l  B_RESULT(a2),d0
                bsr     puthex
                lea     s_status(pc),a0
                bsr     puts
                move.l  B_STATUS(a2),d0
                bsr     puthex
                lea     s_far(pc),a0
                bsr     puts
                move.l  B_INFO+12(a2),d0
                bsr     puthex
                bsr     newline
                move.l  (sp)+,d0
                addq.l  #1,d7
                cmp.l   #3,d7
                bne.s   .prot
                ; still alive: add(40, 2)
                move.l  #40,(a4)
                move.l  #2,4(a4)
                moveq   #12,d4
                bsr     call_entry
                lea     s_protadd(pc),a0
                bsr     report
                ; the hypervisor's line about the 68k RAM it lets native code see
                moveq   #8,d0
                move.l  #logbuf,d1
                move.l  #16384,d2
                moveq   #0,d3
                bsr     request
                bsr     print_log

                ; ---- statistics (hypervisor 8): NCStats, 104 bytes ----
                moveq   #11,d0
                move.l  #stbuf,d1
                moveq   #104,d2
                moveq   #0,d3
                bsr     request
                lea     s_stats(pc),a0
                bsr     report
                cmp.l   #1,d1
                bne.s   .nost
                lea     stbuf,a5
                lea     s_stfreq(pc),a0
                bsr     puts
                move.l  4(a5),d0
                bsr     puthex
                lea     s_sttemp(pc),a0
                bsr     puts
                move.l  16(a5),d0
                bsr     puthex
                lea     s_faults(pc),a0
                bsr     puts
                move.l  24(a5),d0
                bsr     puthex
                bsr     newline
                lea     32(a5),a1
                moveq   #3,d6
.stc:           lea     s_stcore(pc),a0
                bsr     puts
                move.l  4(a1),d0                ; busy ticks (low word)
                bsr     puthex
                moveq   #' ',d0
                bsr     putc
                move.l  8(a1),d0                ; calls
                bsr     puthex
                moveq   #' ',d0
                bsr     putc
                move.l  12(a1),d0               ; flags
                bsr     puthex
                bsr     newline
                lea     16(a1),a1
                subq.l  #1,d6
                bpl.s   .stc
.nost:

                ; ---- cancel (hypervisor 7): START mark with ~1 s of work, CANCEL at once ----
                lea     markbuf,a5
                move.l  a5,d0
                add.l   #15,d0
                and.l   #$fffffff0,d0
                move.l  d0,a5
                move.l  a5,a0
                move.w  #4095,d6
.cclr:          clr.l   (a0)+
                dbf     d6,.cclr
                clr.l   (a4)
                move.l  #4096,4(a4)
                move.l  a5,8(a4)
                move.l  #100000,12(a4)
                clr.l   B_INFO+20(a2)
                bsr     start_mark
                move.l  d0,d7
                moveq   #10,d0
                move.l  d7,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                lea     s_cancel(pc),a0
                bsr     report
                bsr     wait_job
                lea     s_cwait(pc),a0
                bsr     report
                bsr     count_units
                ; CANCEL of a job that does not exist
                moveq   #10,d0
                moveq   #3,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                lea     s_cbad(pc),a0
                bsr     report
                ; and jobs still work
                clr.l   12(a4)
                move.l  #256,4(a4)
                clr.l   B_INFO+20(a2)
                moveq   #24,d4
                bsr     runmp_entry
                lea     s_cok(pc),a0
                bsr     report

                lea     s_end(pc),a0
                bsr     puts
.halt:          bra.s   .halt

; START hello's mark (entry 3) with the params at a4, status block a6 -> d0 job, d1 status
start_mark:
                move.l  (a4),B_INFO(a2)
                move.l  4(a4),B_INFO+4(a2)
                move.l  #64,B_INFO+8(a2)
                move.l  #16,B_INFO+12(a2)
                move.l  a6,B_INFO+16(a2)
                moveq   #6,d0
                move.l  modbase,d1
                move.l  d1,a0
                add.l   24(a0),d1
                move.l  a4,d2
                move.l  modbase,d3
                bra     request

; WAIT for job d7 until it is complete -> d0 result, d1 status, d4 requests
wait_job:
                moveq   #0,d4
.again:         addq.l  #1,d4
                moveq   #7,d0
                move.l  d7,d1
                moveq   #0,d2
                moveq   #0,d3
                bsr     request
                cmp.l   #2,d1
                beq.s   .again
                rts

; units of markbuf (a5, 4096) per core, as after RUNMP
count_units:
                lea     cnt,a1
                clr.l   (a1)
                clr.l   4(a1)
                clr.l   8(a1)
                clr.l   12(a1)
                clr.l   16(a1)
                move.l  a5,a0
                move.w  #4095,d6
.cu:            move.l  (a0)+,d0
                cmp.l   #4,d0
                bls.s   .cv
                moveq   #0,d0
.cv:            lsl.l   #2,d0
                addq.l  #1,(a1,d0.l)
                dbf     d6,.cu
                lea     s_percore(pc),a0
                bsr     puts
                moveq   #1,d6
.cp:            move.l  (a1,d6.l*4),d0
                bsr     puthex
                moveq   #' ',d0
                bsr     putc
                addq.l  #1,d6
                cmp.l   #5,d6
                bne.s   .cp
                lea     s_missing(pc),a0
                bsr     puts
                move.l  (a1),d0
                bsr     puthex
                bra     newline

; RUNMP of hello's mark (entry 3) with the params at a4: { from, to, dst, work }
runmp_mark:
                move.l  (a4),B_INFO(a2)
                move.l  4(a4),B_INFO+4(a2)
                move.l  #64,B_INFO+8(a2)
                move.l  #16,B_INFO+12(a2)
                moveq   #5,d0
                move.l  a3,d1
                add.l   24(a3),d1
                move.l  a4,d2
                move.l  a3,d3
                bra     request

; CALL of the module entry at offset d4 of the header, params a4, default time limit
call_entry:
                clr.l   B_INFO+20(a2)
                moveq   #3,d0
                move.l  modbase,d1
                move.l  d1,a0
                add.l   (a0,d4.l),d1
                move.l  a4,d2
                move.l  modbase,d3
                bra     request

; d0 bytes of logbuf to the UART, only if the request worked (d1 = 1)
print_log:
                cmp.l   #1,d1
                bne.s   .pdone
                move.l  d0,d6
                beq.s   .pdone
                cmp.l   #16384,d6
                bhi.s   .pdone
                lea     logbuf,a0
.pl:            move.b  (a0)+,d0
                bsr     putc
                subq.l  #1,d6
                bne.s   .pl
.pdone:         rts

; RUNMP / START of the module entry at offset d4 of the header (24 = mark, 32 = spin), params a4
; { from, to, ... } 16 bytes, pieces of 64; START: status block a6. Leaves B_INFO+20 (limit).
runmp_entry:
                moveq   #5,d0
                bra.s   job_entry
start_entry:
                move.l  a6,B_INFO+16(a2)
                moveq   #6,d0
job_entry:
                move.l  (a4),B_INFO(a2)
                move.l  4(a4),B_INFO+4(a2)
                move.l  #64,B_INFO+8(a2)
                move.l  #16,B_INFO+12(a2)
                move.l  modbase,d1
                move.l  d1,a0
                add.l   (a0,d4.l),d1
                move.l  a4,d2
                move.l  modbase,d3
                bra     request

; d0 = op, d1-d3 = arg0-arg2, a2 = block -> d0 = result, d1 = status
request:
                move.l  #$4e43414c,B_MAGIC(a2)  ; 'NCAL'
                move.l  d0,B_OP(a2)
                move.l  d1,B_ARG0(a2)
                move.l  d2,B_ARG1(a2)
                move.l  d3,B_ARG2(a2)
                move.l  #-1,B_RESULT(a2)
                clr.l   B_STATUS(a2)
                clr.l   B_INFO+24(a2)
                clr.l   B_INFO+28(a2)
                move.l  a2,a0
                move.l  #$4e43414c,d0           ; doorbell (nc_abi.h)
                move.l  #$b1bcbeb3,d1
                dc.w    $f468                   ; CPUSHL DC,(A0)
                nop
                move.l  B_RESULT(a2),d0
                move.l  B_STATUS(a2),d1
                rts

; a0 = name, d0 = result, d1 = status
report:
                movem.l d0-d1,-(sp)
                bsr     puts
                lea     s_result(pc),a0
                bsr     puts
                move.l  (sp),d0
                bsr     puthex
                lea     s_status(pc),a0
                bsr     puts
                move.l  4(sp),d0
                bsr     puthex
                bsr     newline
                movem.l (sp)+,d0-d1
                rts

newline:
                moveq   #10,d0
                bra.s   putc

; a0 = zero terminated string
puts:
                move.l  d0,-(sp)
.next:          move.b  (a0)+,d0
                beq.s   .done
                bsr.s   putc
                bra.s   .next
.done:          move.l  (sp)+,d0
                rts

; d0.b -> PL011 data register (little-endian 32-bit register: char goes to the first byte)
putc:
                move.l  d0,-(sp)
                and.l   #$ff,d0
                ror.l   #8,d0
                move.l  d0,UART_DR
                move.l  (sp)+,d0
                rts

; d0 as 8 hex digits
puthex:
                movem.l d0-d2,-(sp)
                move.l  d0,d1
                moveq   #7,d2
.digit:         rol.l   #4,d1
                move.l  d1,d0
                and.l   #15,d0
                cmp.b   #10,d0
                blt.s   .num
                add.b   #'a'-10-'0',d0
.num:           add.b   #'0',d0
                bsr.s   putc
                dbf     d2,.digit
                movem.l (sp)+,d0-d2
                rts

s_hello:        dc.b    10,"NCTEST START",10,0
s_spoof:        dc.b    "SPOOF    ",0      ; expected: result=4e43414c status=00000000
s_ticks:        dc.b    "  ticks=",0
s_freq:         dc.b    " freq=",0
s_ping:         dc.b    "PING     ",0
s_sync:         dc.b    "SYNC     ",0
s_add:          dc.b    "ADD      ",0
s_fault:        dc.b    "FAULT    ",0
s_badentry:     dc.b    "BADENTRY ",0
s_info:         dc.b    "INFO     ",0
s_result:       dc.b    " result=",0
s_status:       dc.b    " status=",0
s_esr:          dc.b    "  esr=",0
s_far:          dc.b    " far=",0
s_loop:         dc.b    "LOOP1000 errors=",0
s_traps:        dc.b    "  traps=",0
s_calls:        dc.b    " calls=",0
s_faults:       dc.b    " faults=",0
s_helpers:      dc.b    "HELPERS  cores=",0
s_runmp:        dc.b    "RUNMP    ",0
s_cores:        dc.b    "  cores=",0
s_helped:       dc.b    " helped=",0
s_percore:      dc.b    "  units per core 0-3: ",0
s_missing:      dc.b    " missing=",0
s_runmpflt:     dc.b    "RUNMPFLT ",0
s_mploop:       dc.b    "RUNMP200 errors=",0
s_hstat:        dc.b    "HSTAT    pieces 1-3, wfe 1-3: ",0
s_astart:       dc.b    "ASTART   ",0
s_a68k:         dc.b    "  68k loops while the job ran: ",0
s_await:        dc.b    "AWAIT    ",0
s_waits:        dc.b    " waits=",0
s_abusy:        dc.b    "ABUSY    ",0
s_awall:        dc.b    "AWAIT4   ",0
s_afault:       dc.b    "AFAULT   ",0
s_abad:         dc.b    "ABADJOB  ",0
s_feat:         dc.b    "FEATURES ",0      ; hypervisor 6: 00000001 = time limit works
s_tmoskip:      dc.b    "TIMEOUT  skipped: no time limit here",10,0
s_tmo:          dc.b    "TIMEOUT  ",0      ; expected: status=00000087
s_elr:          dc.b    "  elr-spin=",0
s_tmoadd:       dc.b    "TMOADD   ",0      ; expected: result=0000002a status=00000001
s_tmomp:        dc.b    "TMORUNMP ",0      ; expected: status=00000087
s_tmoasync:     dc.b    "TMOASYNC ",0      ; expected: status=00000087
s_jstat:        dc.b    "  job status block=",0
s_tmook:        dc.b    "TMOMARK  ",0      ; expected: result=00000100 status=00000001
s_logcall:      dc.b    "LOGCALL  ",0      ; expected: result=00000001 status=00000001
s_logread:      dc.b    "LOGREAD  ",0      ; then the log text itself
s_deep:         dc.b    "DEEPFLT  ",0      ; expected: status=00000084
s_finfo:        dc.b    "FINFO    ",0      ; expected: result=00000150 status=00000001
s_fst:          dc.b    "  status=",0
s_ffar:         dc.b    " far=",0
s_fx0:          dc.b    " x0=",0
s_bt:           dc.b    "  backtrace:",0
s_prot:         dc.b    "PROTECT",0         ; + mode; expected: status=00000084
s_protadd:      dc.b    "PROTADD  ",0      ; expected: result=0000002a status=00000001
s_stats:        dc.b    "STATS    ",0      ; expected: result=00000068 status=00000001
s_stfreq:       dc.b    "  freq=",0
s_sttemp:       dc.b    " temp=",0
s_stcore:       dc.b    "  STCORE busy/calls/flags: ",0
s_cancel:       dc.b    "ACANCEL  ",0      ; expected: result=00000000 status=00000001
s_cwait:        dc.b    "ACWAIT   ",0      ; expected: status=00000088, then missing != 0
s_cbad:         dc.b    "ACBADJOB ",0      ; expected: status=00000086
s_cok:          dc.b    "ACMARK   ",0      ; expected: result=00000100 status=00000001
s_end:          dc.b    "NCTEST END",10,0
                even

                section data,data
narm_module:    incbin  "hello.narm"
narm_module_end:
                even

                section bss,bss
blockbuf:       ds.b    128
modbuf:         ds.b    16384
parbuf:         ds.b    32
markbuf:        ds.l    4096+4
cnt:            ds.l    5
jobstat:        ds.l    8
jobs:           ds.l    4
modbase:        ds.l    1
features:       ds.l    1
logpos:         ds.l    1
fibuf:          ds.l    96
stbuf:          ds.l    26
logbuf:         ds.b    16384+16
