; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-genc,verify' -S %s -o %t
; RUN: %opt -passes=verify -S %t | %FileCheck %s

; A global is transformed only if every access can be decoded.
; CHECK-DAG: @escaped = private constant i32 42
; CHECK-DAG: @dynamic = private constant [3 x i32] [i32 10, i32 20, i32 30]
; CHECK-DAG: @mixed = private constant i32 1065353216
; CHECK-DAG: @good = private global [2 x i32]
@escaped = private constant i32 42
@table = global ptr @escaped
@dynamic = private constant [3 x i32] [i32 10, i32 20, i32 30]
@mixed = private constant i32 1065353216
@good = private constant [2 x i32] [i32 17, i32 23]

define i32 @escaped_access() {
 %a = load i32, ptr @escaped
 %p = load ptr, ptr @table
 %b = load i32, ptr %p
 %r = add i32 %a, %b
 ret i32 %r
}
define i32 @dynamic_access(i64 %n) {
 %a = load i32, ptr getelementptr ([3 x i32], ptr @dynamic, i64 0, i64 1)
 %p = getelementptr [3 x i32], ptr @dynamic, i64 0, i64 %n
 %b = load i32, ptr %p
 %r = add i32 %a, %b
 ret i32 %r
}
define float @mixed_access() {
 %a = load i32, ptr @mixed
 %b = load float, ptr @mixed
 ret float %b
}
; CHECK-LABEL: define i32 @supported(
; CHECK: %a = load i32, ptr getelementptr{{( inbounds)?}} ({{.*}}), align 4{{$}}
; CHECK-NEXT: %a.dec = xor i32 %a,
; CHECK: %b = load i32, ptr %p, align 4
; CHECK-NEXT: %b.dec = xor i32 %b,
define i32 @supported() {
 %a = load i32, ptr getelementptr ([2 x i32], ptr @good, i64 0, i64 1), !range !0
 %p = getelementptr [2 x i32], ptr @good, i64 0, i64 0
 %b = load i32, ptr %p
 %r = add i32 %a, %b
 ret i32 %r
}
!0 = !{i32 0, i32 100}
