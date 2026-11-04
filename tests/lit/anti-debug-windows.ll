; RUN: %opt -load-pass-plugin=%kagura_plugin -passes='kagura-anti-debug,verify' -S %s | %FileCheck %s --implicit-check-not=@ptrace --implicit-check-not=@sysctl
; Windows must emit runtime probes without any POSIX startup calls.
target triple = "x86_64-pc-windows-msvc"
define i32 @main() { ret i32 0 }
; CHECK-LABEL: define internal void @kagura_anti_debug_init()
; CHECK: call i32 @kagura_check_tracer_pid()
; CHECK: call i32 @kagura_check_inline_hooks()
; CHECK: call i32 @kagura_check_got_hooks()
; CHECK: call i32 @kagura_check_sw_breakpoints()
; CHECK: call i32 @kagura_check_hw_breakpoints()
; CHECK: call i32 @kagura_check_emulator()
