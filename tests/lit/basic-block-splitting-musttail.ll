; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -kagura-bbs-min=4 -kagura-bbs-max-splits=1 -passes='kagura-bbs,verify' -S %s | %FileCheck %s

declare ptr @target(ptr, i32)
; CHECK-LABEL: define ptr @tail(
; CHECK: %p = musttail call ptr @target(ptr %v, i32 %c)
; CHECK-NEXT: %q = bitcast ptr %p to ptr
; CHECK-NEXT: ret ptr %q
define ptr @tail(ptr %v, i32 %n) {
 %a = add i32 %n, 1
 %b = add i32 %a, 1
 %c = add i32 %b, 1
 %p = musttail call ptr @target(ptr %v, i32 %c)
 %q = bitcast ptr %p to ptr
 ret ptr %q
}
; CHECK-LABEL: define i32 @ordinary(
; CHECK: br label %{{.*}}split
; CHECK: add i32
define i32 @ordinary(i32 %n) {
 %a = add i32 %n, 1
 %b = add i32 %a, 1
 %c = add i32 %b, 1
 %d = add i32 %c, 1
 %e = add i32 %d, 1
 ret i32 %e
}
