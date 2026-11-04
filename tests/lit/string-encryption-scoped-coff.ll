; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-string-scoped -passes='kagura-str,verify' -S %s | %FileCheck %s
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-string-scoped -passes='kagura-str-aes,verify' -S %s | %FileCheck %s
; MSVC pooled literals can be scoped only when all uses permit relocation.
target triple = "x86_64-pc-windows-msvc"
$pooled = comdat any
$escaped = comdat any
@pooled = linkonce_odr dso_local unnamed_addr constant [14 x i8] c"scoped-secret\00", comdat
@escaped = linkonce_odr dso_local unnamed_addr constant [15 x i8] c"escaped-secret\00", comdat
declare void @consume(ptr nocapture readonly) nounwind
; CHECK-NOT: @pooled =
; CHECK: @escaped = linkonce_odr
; CHECK-LABEL: define void @safe()
; CHECK: %[[BUF:.*]] = alloca [14 x i8]
; CHECK: call void @consume(ptr %[[BUF]])
; CHECK-NEXT: call void @llvm.memset{{.*}}(ptr align {{.*}}%[[BUF]], i8 0, i64 14, i1 true)
define void @safe() {
  call void @consume(ptr @pooled)
  ret void
}
; CHECK-LABEL: define ptr @escape()
; CHECK-NEXT: ret ptr @escaped
define ptr @escape() { ret ptr @escaped }
