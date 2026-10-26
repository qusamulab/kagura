; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 \
; RUN:     -passes='kagura-string-split,verify' -S %s | %FileCheck %s
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 \
; RUN:     -passes='kagura-str,kagura-str-aes,kagura-wstr,kagura-string-split,verify' -S %s | %FileCheck %s

; Initialize PHI operands in their incoming blocks, including a loop backedge.
; No initialization call may separate the PHIs at the beginning of a block.
@first = private constant [17 x i8] c"abcdefghijklmnop\00"
@second = private constant [17 x i8] c"qrstuvwxyzABCDEF\00"

; CHECK-LABEL: define ptr @pick(
; CHECK: a:
; CHECK-NEXT: call void @__kagura_strsplit_0()
; CHECK-NEXT: br label %merge
; CHECK: b:
; CHECK-NEXT: call void @__kagura_strsplit_1()
; CHECK-NEXT: br label %merge
; CHECK: merge:
; CHECK-NEXT: %p = phi ptr [ @kagura_str_recombined_0, %a ], [ @kagura_str_recombined_1, %b ]
; CHECK-NEXT: %n = phi i32 [ 1, %a ], [ 2, %b ]
; CHECK-NEXT: ret ptr %p
define ptr @pick(i1 %cond) {
entry:
  br i1 %cond, label %a, label %b
a:
  br label %merge
b:
  br label %merge
merge:
  %p = phi ptr [ @first, %a ], [ @second, %b ]
  %n = phi i32 [ 1, %a ], [ 2, %b ]
  ret ptr %p
}

; CHECK-LABEL: define ptr @loop(
; CHECK: entry:
; CHECK-NEXT: call void @__kagura_strsplit_0()
; CHECK-NEXT: br label %body
; CHECK: body:
; CHECK-NEXT: %p = phi ptr [ @kagura_str_recombined_0, %entry ], [ @kagura_str_recombined_1, %body ]
; CHECK-NEXT: call void @__kagura_strsplit_1()
; CHECK-NEXT: br i1 %again, label %body, label %exit
define ptr @loop(i1 %again) {
entry:
  br label %body
body:
  %p = phi ptr [ @first, %entry ], [ @second, %body ]
  br i1 %again, label %body, label %exit
exit:
  ret ptr %p
}
