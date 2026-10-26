; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-mvo,verify' -S %s -o %t
; RUN: %opt -passes=verify -S %t | %FileCheck %s --check-prefix=MVO
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-pe,verify' -S %s -o %t
; RUN: %opt -passes=verify -S %t | %FileCheck %s --check-prefix=PE

target datalayout = "e-p:64:64-p1:32:32-p5:64:64-A5"

; MVO-LABEL: define i32 @integer_slot(
; MVO: xor i32 %x,
; MVO: %v = load i32, ptr addrspace(5) %slot, align 4{{$}}
; MVO-NEXT: %v.mvo = xor i32 %v,
define i32 @integer_slot(i32 %x) {
  %slot = alloca i32, align 4, addrspace(5)
  store i32 %x, ptr addrspace(5) %slot, align 4
  %v = load i32, ptr addrspace(5) %slot, align 4, !range !0
  ret i32 %v
}

; MVO-LABEL: define i32 @mixed_integer(
; MVO-NOT: xor
; MVO: store float %x,
; MVO-NOT: xor
; MVO: ret i32 %v
define i32 @mixed_integer(float %x) {
  %slot = alloca i32, align 4, addrspace(5)
  store float %x, ptr addrspace(5) %slot, align 4
  %v = load i32, ptr addrspace(5) %slot, align 4
  ret i32 %v
}

; PE-LABEL: define ptr addrspace(1) @pointer_slot(
; PE: %slot = alloca ptr addrspace(1), align 1, addrspace(5)
; PE: ptrtoint ptr addrspace(1) %x to i32
; PE: xor i32
; PE: store i32 {{.*}}, ptr addrspace(5) %slot, align 1
; PE: load i32, ptr addrspace(5) %slot, align 1
; PE: inttoptr i32 {{.*}} to ptr addrspace(1)
define ptr addrspace(1) @pointer_slot(ptr addrspace(1) %x) {
  %slot = alloca ptr addrspace(1), align 1, addrspace(5)
  store ptr addrspace(1) %x, ptr addrspace(5) %slot, align 1
  %v = load ptr addrspace(1), ptr addrspace(5) %slot, align 1
  ret ptr addrspace(1) %v
}

; PE-LABEL: define i64 @mixed_pointer(
; PE-NOT: pe.
; PE: store i64 %x,
; PE: ret i64 %v
define i64 @mixed_pointer(i64 %x) {
  %slot = alloca ptr, align 8, addrspace(5)
  store i64 %x, ptr addrspace(5) %slot, align 8
  %v = load i64, ptr addrspace(5) %slot, align 8
  ret i64 %v
}

; MVO-LABEL: define i32 @volatile_slot(
; MVO-NOT: xor
; MVO: store volatile i32 %x,
; MVO-NOT: xor
; MVO: ret i32 %v
define i32 @volatile_slot(i32 %x) {
  %slot = alloca i32, align 4, addrspace(5)
  store volatile i32 %x, ptr addrspace(5) %slot
  %v = load volatile i32, ptr addrspace(5) %slot
  ret i32 %v
}
!0 = !{i32 0, i32 100}
