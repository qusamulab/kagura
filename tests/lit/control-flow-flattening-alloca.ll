; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-fla,verify' -S %s | %FileCheck %s
; An alloca pointer defined outside entry must be spilled across dispatcher edges.
; CHECK-LABEL: define i32 @branch_alloca(
; CHECK: alloca ptr
; CHECK: %slot = alloca i32
; CHECK: store ptr %slot,
; CHECK: load ptr,
; CHECK: switch i32
define i32 @branch_alloca(i1 %which) {
entry:
 br i1 %which, label %a, label %b
a:
 %slot = alloca i32
 store i32 42, ptr %slot
 br label %exit
b:
 ret i32 13
exit:
 %v = load i32, ptr %slot
 ret i32 %v
}
