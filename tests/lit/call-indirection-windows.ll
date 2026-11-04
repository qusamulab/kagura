; RUN: %opt -load-pass-plugin=%kagura_plugin -passes='kagura-ci,verify' -S %s | %FileCheck %s
target triple = "x86_64-pc-windows-msvc"
@msg = private constant [6 x i8] c"hello\00"
declare dllimport i32 @puts(ptr)
declare i32 @other_translation_unit()

; CHECK-NOT: @dlsym
; CHECK-NOT: @kagura_rtld_default_handle
; CHECK-LABEL: define i32 @main()
; CHECK: call ptr @__kagura_resolve_0()
; CHECK-NOT: call i32 @puts
; CHECK: call i32 %{{.*}}(ptr @msg)
; CHECK: call i32 @other_translation_unit()
define i32 @main() {
  %r = call i32 @puts(ptr @msg)
  %local = call i32 @other_translation_unit()
  ret i32 %r
}
; CHECK-LABEL: define internal ptr @__kagura_resolve_0()
; CHECK: load atomic ptr
; CHECK: call ptr @kagura_windows_resolve_symbol(ptr {{.*}}, ptr @puts)
; CHECK: store atomic ptr
; CHECK-LABEL: define internal void @kagura_init_thunk_table()
; CHECK: call ptr @kagura_windows_resolve_symbol(ptr {{.*}}, ptr @puts)
; CHECK: store atomic ptr
