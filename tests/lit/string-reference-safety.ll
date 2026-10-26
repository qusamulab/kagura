; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-str,verify' -S %s | %FileCheck %s --check-prefixes=COMMON,XOR --implicit-check-not='memory(none)'
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-str-aes,verify' -S %s | %FileCheck %s --check-prefixes=COMMON,AES --implicit-check-not='memory(none)'
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-wstr,verify' -S %s | %FileCheck %s --check-prefix=WIDE

@text = private constant [19 x i8] c"prefix-secret-data\00"
@wide = private constant [3 x i16] [i16 72, i16 105, i16 0]
@wide_table = constant [1 x ptr] [ptr @wide]
@wide_direct = private constant [3 x i16] [i16 72, i16 105, i16 0]
; WIDE-DAG: @wide = private constant [3 x i16] [i16 72, i16 105, i16 0]
; WIDE-DAG: @kagura_wflag_
; COMMON-NOT: @text =
; AES: @kagura_aesbuf_{{[0-9]+}} = private global [19 x i8] zeroinitializer
; COMMON-LABEL: define ptr @substring(
; XOR: call void @kagura_decrypt_
; AES: call void @kagura_aesinit_
; COMMON: ret ptr getelementptr ([19 x i8], ptr @kagura_{{(enc|aesbuf)}}_{{[0-9]+}}, i64 0, i64 7)
define ptr @substring() noinline memory(none) {
 ret ptr getelementptr ([19 x i8], ptr @text, i64 0, i64 7)
}
; COMMON-LABEL: define ptr @base(
; COMMON-NOT: alloca
; COMMON: ret ptr @kagura_{{(enc|aesbuf)}}_
define ptr @base() {
 ret ptr @text
}
; WIDE-LABEL: define i16 @wide_offset(
; WIDE: call void @kagura_wdecrypt_
; WIDE: load i16, ptr getelementptr ([3 x i16], ptr @wide_direct, i64 0, i64 1)
define i16 @wide_offset() {
 %v = load i16, ptr getelementptr ([3 x i16], ptr @wide_direct, i64 0, i64 1)
 ret i16 %v
}

; Propagate new initialization effects to callers and their call-site attrs.
; COMMON-LABEL: define ptr @forward(
; COMMON: call ptr @substring()
define ptr @forward() memory(none) {
 %p = call ptr @substring() memory(none)
 ret ptr %p
}
