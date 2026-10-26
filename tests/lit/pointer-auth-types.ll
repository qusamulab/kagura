; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-pac,verify' -S %s -o %t
; RUN: %opt -passes=verify -S %t | %FileCheck %s --check-prefix=SW
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -mtriple=arm64e-apple-darwin -passes='kagura-pac,verify' -S %s -o %t
; RUN: %opt -passes=verify -S %t | %FileCheck %s --check-prefix=HW

@fp = private global ptr @target
@mixed = private global ptr @target
@public = global ptr @target
; SW-DAG: @fp.pac = private global i64 ptrtoint (ptr @target to i64), align 8
; SW-DAG: @mixed = private global ptr @target
; HW-DAG: @fp.hwpac = private global ptr @target
; HW-DAG: @mixed = private global ptr @target

define i32 @target(i32 %x) { ret i32 %x }
; SW-LABEL: define ptr @get(
; SW: xor i64
; SW: inttoptr i64
; HW-LABEL: define ptr @get(
; HW: call void @fp.hwpac.once()
; HW: call i64 @llvm.ptrauth.auth(i64 {{.*}}, i32 0, i64
; HW: inttoptr i64
; HW: ret ptr
define ptr @get() {
 %p = load ptr, ptr @fp
 ret ptr %p
}
; HW-LABEL: define void @set(
; HW: call i64 @llvm.ptrauth.sign(i64 {{.*}}, i32 0, i64
; HW: store ptr {{.*}}, ptr @fp.hwpac
define void @set(ptr %p) {
 store ptr %p, ptr @fp
 ret void
}
define i64 @mixed_load() {
 %p = load i64, ptr @mixed
 ret i64 %p
}
