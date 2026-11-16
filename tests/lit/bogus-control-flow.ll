; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-bcf-prob=100 -passes='function(kagura-bcf),verify' -S %s | %FileCheck %s

; After bogus CF, the function must still have a ret instruction (correctness)
; CHECK: define i32 @classify
; CHECK: br i1 %bcf.pred
; CHECK: bcf.bogus
; CHECK: ret i32

define i32 @classify(i32 %x) {
entry:
  %cmp_neg = icmp slt i32 %x, 0
  br i1 %cmp_neg, label %ret_neg, label %check_zero

check_zero:
  %cmp_zero = icmp eq i32 %x, 0
  br i1 %cmp_zero, label %ret_zero, label %ret_pos

ret_neg:
  ret i32 -1

ret_zero:
  ret i32 0

ret_pos:
  ret i32 1
}
