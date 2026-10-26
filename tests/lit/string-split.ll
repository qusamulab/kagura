; RUN: %opt -load-pass-plugin=%kagura_plugin -passes=kagura-string-split -S %s | %FileCheck %s

; The string-split pass fragments long string literals (>= 16 bytes) into
; multiple private globals, then rewrites every use to call an init stub and
; load from a recombined zero-init buffer.

; Short string (< 16 bytes) is left as-is, and appears before the new globals.
; CHECK-DAG: @short = private constant [5 x i8] c"abcd\00"

; Fragments / recombined / flag are emitted for the long string.
; CHECK-DAG: @kagura_str_frag_0_{{[0-9]+}} = private constant
; CHECK-DAG: @kagura_str_recombined_0 = private global [29 x i8] zeroinitializer
; CHECK-DAG: @kagura_str_flag_0 = private global i8 0

@hello = private constant [29 x i8] c"this is a long secret API key", align 1
@short = private constant [5 x i8] c"abcd\00", align 1

declare void @use(ptr)

; The consumer should now call __kagura_strsplit_0 before using the
; recombined buffer.
; CHECK-LABEL: define void @consumer()
; CHECK: call void @__kagura_strsplit_0()
; CHECK: call void @use(ptr @kagura_str_recombined_0)

define void @consumer() {
  call void @use(ptr @hello)
  ret void
}

; Negative: a short string (< 16 bytes) should not gain an init stub.
; CHECK-LABEL: define void @short_consumer()
; CHECK: call void @use(ptr @short)
; CHECK-NOT: __kagura_strsplit
; CHECK: ret void
define void @short_consumer() {
  call void @use(ptr @short)
  ret void
}

; The init stub must have the flag-guard pattern.
; CHECK-LABEL: define internal void @__kagura_strsplit_0()
; CHECK: load atomic i8, ptr @kagura_str_flag_0 acquire
; CHECK: cmpxchg ptr @kagura_str_flag_0, i8 0, i8 1 acquire monotonic
; CHECK: store atomic i8 2, ptr @kagura_str_flag_0 release
