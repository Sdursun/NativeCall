; nativecall.library - resident tag. Must be the first object in the link: running the
; library as a program returns at once.

RTC_MATCHWORD   equ     $4afc
RTF_AUTOINIT    equ     (1<<7)
NT_LIBRARY      equ     9
LIB_VERSION     equ     1

                section code,code

                moveq   #-1,d0
                rts

                xref    _LibName
                xref    _LibIdString
                xref    _InitTable

romtag:         dc.w    RTC_MATCHWORD
                dc.l    romtag
                dc.l    romtag_end
                dc.b    RTF_AUTOINIT
                dc.b    LIB_VERSION
                dc.b    NT_LIBRARY
                dc.b    0                       ; priority
                dc.l    _LibName
                dc.l    _LibIdString
                dc.l    _InitTable
romtag_end:
