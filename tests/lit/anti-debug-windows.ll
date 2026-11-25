; RUN: %opt -load-pass-plugin=%kagura_plugin -passes='kagura-anti-debug,verify' -S %s -o %t.ll
; RUN: %FileCheck %s --implicit-check-not=@ptrace --implicit-check-not=@sysctl < %t.ll
; RUN: %clang -Wno-override-module %t.ll -o %t.exe
; RUN: %t.exe
; Windows must emit runtime probes without any POSIX startup calls.
target triple = "x86_64-pc-windows-msvc"
; Run the Windows check chain with deterministic probe results. The returning
; application hook should observe both events before execution reaches main.
@hook_count = global i32 0

define void @kagura_on_tamper_detected() {
  %count = load i32, ptr @hook_count
  %next = add i32 %count, 1
  store i32 %next, ptr @hook_count
  ret void
}

define i32 @kagura_check_tracer_pid() { ret i32 1 }
define i32 @kagura_check_inline_hooks() { ret i32 1 }
define i32 @kagura_check_got_hooks() { ret i32 0 }
define i32 @kagura_check_sw_breakpoints() { ret i32 0 }
define i32 @kagura_check_hw_breakpoints() { ret i32 0 }
define i32 @kagura_check_emulator() { ret i32 0 }

define i32 @main() {
  %count = load i32, ptr @hook_count
  %ok = icmp eq i32 %count, 2
  %exit = select i1 %ok, i32 0, i32 1
  ret i32 %exit
}

; CHECK-LABEL: define internal void @kagura_anti_debug_init()
; CHECK: call i32 @kagura_check_tracer_pid()
; CHECK: br i1 {{[^,]+}}, label %[[HOOK:[^,]+]], label %[[NEXT:[^ ]+]]
; CHECK: [[HOOK]]:
; CHECK-NEXT: call void @kagura_on_tamper_detected()
; CHECK-NEXT: br label %[[NEXT]]
; CHECK: call i32 @kagura_check_inline_hooks()
; CHECK: call i32 @kagura_check_got_hooks()
; CHECK: call i32 @kagura_check_sw_breakpoints()
; CHECK: call i32 @kagura_check_hw_breakpoints()
; CHECK: call i32 @kagura_check_emulator()
