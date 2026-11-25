; RUN: %opt -load-pass-plugin=%kagura_plugin -passes='function(kagura-bbcheck),verify' -kagura-seed=1 -kagura-allow=work -S %s -o %t.ll
; RUN: %FileCheck %s < %t.ll
; RUN: %clang %t.ll -o %t.exe
; RUN: %t.exe

source_filename = "bbcheck-returning-hook.ll"

@observed = global i32 0

define i32 @work() {
; CHECK-LABEL: define i32 @work()
; CHECK: call i32 @kagura_bb_check(
; CHECK: br i1 {{[^,]+}}, label %[[HOOK:[^,]+]], label %[[CONT:[^ ]+]]
; CHECK: [[HOOK]]:
; CHECK-NEXT: call void @kagura_on_tamper_detected()
; CHECK-NEXT: br label %[[CONT]]
entry:
  br label %body
body:
  ret i32 42
}

define i32 @kagura_bb_check(i32 %id, i32 %expected) {
  ret i32 0
}

define void @kagura_on_tamper_detected() {
  store i32 1, ptr @observed
  ret void
}

define i32 @main() {
  %value = call i32 @work()
  %seen = load i32, ptr @observed
  %returned = icmp eq i32 %value, 42
  %called = icmp eq i32 %seen, 1
  %ok = and i1 %returned, %called
  %exit = select i1 %ok, i32 0, i32 1
  ret i32 %exit
}
