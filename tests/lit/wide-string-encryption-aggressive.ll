; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -kagura-string-aggressive \
; RUN:     -passes='kagura-wstr,verify' -S %s | %FileCheck %s \
; RUN:     --implicit-check-not=kagura_wflag_ --implicit-check-not=wlazy

; Both UTF-16 and UTF-32 storage referenced through static aggregates and
; PHIs must be writable and initialized at startup, with no lazy PHI guards.
; CHECK: @utf16 = private global [3 x i16]
; CHECK: @utf32 = private global [3 x i32]
; CHECK: @table = constant { ptr, [1 x ptr] } { ptr @utf16, [1 x ptr] [ptr getelementptr inbounds ([3 x i32], ptr @utf32, i32 0, i32 1)] }
; CHECK: @llvm.global_ctors = appending global [2 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 0, ptr @[[DEC16:kagura_wdecrypt_[0-9]+]], ptr null }, { i32, ptr, ptr } { i32 0, ptr @[[DEC32:kagura_wdecrypt_[0-9]+]], ptr null }]
; CHECK-LABEL: define ptr @pick(
; CHECK: merge:
; CHECK-NEXT: %p = phi ptr [ @utf16, %a ], [ @utf32, %b ]
; CHECK-NEXT: ret ptr %p
; CHECK: define internal void @[[DEC16]]()
; CHECK: xor i16
; CHECK: define internal void @[[DEC32]]()
; CHECK: xor i32

@utf16 = private constant [3 x i16] [i16 72, i16 105, i16 0]
@utf32 = private constant [3 x i32] [i32 79, i32 75, i32 0]
@table = constant { ptr, [1 x ptr] } { ptr @utf16, [1 x ptr] [ptr getelementptr inbounds ([3 x i32], ptr @utf32, i32 0, i32 1)] }

define ptr @pick(i1 %which) {
entry:
  br i1 %which, label %a, label %b
a:
  br label %merge
b:
  br label %merge
merge:
  %p = phi ptr [ @utf16, %a ], [ @utf32, %b ]
  ret ptr %p
}
