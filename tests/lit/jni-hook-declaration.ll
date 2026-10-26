; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-jni,verify' -S %s -o %t
; RUN: %opt -passes=verify -S %t | %FileCheck %s

target triple = "aarch64-linux-android"
declare i32 @JNI_OnLoad(ptr, ptr)
; CHECK: define i32 @JNI_OnLoad(ptr {{.*}}, ptr {{.*}})
; CHECK: call ptr @kagura_jni_get_env
; CHECK: ret i32 65542
; CHECK: define internal i32 @kagura_jni_
define i32 @Java_com_example_Main_foo(ptr %env, ptr %cls) { ret i32 1 }
