; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-deny=excluded -passes='kagura-str,verify' -S %s | %FileCheck %s --check-prefixes=COMMON,XOR --implicit-check-not='c"allowed string literal'
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-deny=excluded -passes='kagura-str-aes,verify' -S %s | %FileCheck %s --check-prefixes=COMMON,AES --implicit-check-not='c"allowed string literal'
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-deny=excluded -passes='kagura-string-split,verify' -S %s | %FileCheck %s --check-prefixes=COMMON,SPLIT --implicit-check-not='c"allowed string literal'
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-string-aggressive -kagura-deny=excluded -passes='kagura-str,verify' -S %s | %FileCheck %s --check-prefixes=COMMON,STARTUP --implicit-check-not='c"allowed string literal'
;
; Mixed consumers retain one shared object and its pointer identity. Excluded,
; hotpath and pass-disabled consumers must receive neither rewrites nor guards.
; A permitted literal is actually transformed in every mode.
; COMMON-DAG: @excluded_literal = private constant [24 x i8] c"excluded string literal\00"
; COMMON-DAG: @shared_literal = private constant [22 x i8] c"shared string literal\00"
; COMMON-DAG: @hot_literal = private constant [19 x i8] c"hot string literal\00"
; COMMON-DAG: @disabled_literal = private constant [24 x i8] c"disabled string literal\00"
; XOR-DAG: @kagura_enc_
; AES-DAG: @kagura_aesenc_
; SPLIT-DAG: @kagura_str_recombined_
; STARTUP-DAG: @kagura_enc_
; STARTUP-DAG: @llvm.global_ctors

@excluded_literal = private constant [24 x i8] c"excluded string literal\00"
@shared_literal = private constant [22 x i8] c"shared string literal\00"
@allowed_literal = private constant [23 x i8] c"allowed string literal\00"
@hot_literal = private constant [19 x i8] c"hot string literal\00"
@disabled_literal = private constant [24 x i8] c"disabled string literal\00"
@table = private constant [1 x ptr] [ptr @shared_literal]

declare void @use(ptr)

; COMMON-LABEL: define void @excluded()
; COMMON-NEXT: call void @use(ptr @excluded_literal)
; COMMON-NEXT: call void @use(ptr @shared_literal)
; COMMON-NEXT: ret void
define void @excluded() {
  call void @use(ptr @excluded_literal)
  call void @use(ptr @shared_literal)
  ret void
}

; The first use dominates two subsequent uses: only one lazy initialization
; guard is needed, retaining the original first-use timing and acquire/release.
; COMMON-LABEL: define void @allowed()
; XOR: cmpxchg
; XOR-NOT: cmpxchg
; AES: cmpxchg
; AES-NOT: cmpxchg
; COMMON: call void @use(ptr @kagura_{{enc_|aesbuf_|str_recombined_}}
; COMMON: ret void
define void @allowed() {
  call void @use(ptr @allowed_literal)
  call void @use(ptr @allowed_literal)
  br label %next
next:
  call void @use(ptr @allowed_literal)
  call void @use(ptr @shared_literal)
  call void @use(ptr @table)
  ret void
}

; COMMON-LABEL: define void @hot()
; COMMON-NEXT: call void @use(ptr @hot_literal)
; COMMON-NEXT: ret void
define void @hot() !kagura_hotpath !0 {
  call void @use(ptr @hot_literal)
  ret void
}

; COMMON-LABEL: define void @disabled()
; COMMON-NEXT: call void @use(ptr @disabled_literal)
; COMMON-NEXT: ret void
define void @disabled() !kagura_nostr !0 !kagura_nostring-split !0 {
  call void @use(ptr @disabled_literal)
  ret void
}
!0 = !{}
