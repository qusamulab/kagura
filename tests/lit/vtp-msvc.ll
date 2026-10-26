; RUN: %opt -load-pass-plugin=%kagura_plugin -passes=kagura-vtp -kagura-vtp -S %s | %FileCheck %s

; VTableProtectionPass MSVC ABI test.
;
; Checks:
;   1. ??_7 vtable is tagged (kagura.vtables metadata emitted).
;   2. ??_R0 TypeDescriptor name field is encrypted (plaintext ".?AVFoo@@" absent).
;   3. A module constructor for in-place decryption is injected.

target triple = "x86_64-pc-windows-msvc19.0.0"

; CHECK: @"??_R0extended" = internal constant { ptr, ptr, [10 x i8], i32 } { ptr null, ptr null, [10 x i8] c".?AVBar@@\00", i32 7 }
; CHECK-NOT: c".?AVFoo@@"
; CHECK: msvc_rtti_decrypt
; CHECK: kagura.vtables

; MSVC vtable for class Foo
@"??_7Foo@@6B@" = internal constant [2 x ptr] [
  ptr @"?method1@Foo@@UAEXXZ",
  ptr @"?method2@Foo@@UAEHXZ"
]

; MSVC TypeDescriptor for class Foo: { ptr vftable, ptr spare, [10 x i8] name }
@"??_R0?AVFoo@@8" = internal constant { ptr, ptr, [10 x i8] } {
  ptr null,
  ptr null,
  [10 x i8] c".?AVFoo@@\00"
}

declare void @"?method1@Foo@@UAEXXZ"()
declare i32  @"?method2@Foo@@UAEHXZ"()

define void @test() {
entry:
  %vt = load ptr, ptr @"??_7Foo@@6B@"
  call void @"?method1@Foo@@UAEXXZ"()
  ret void
}

; Unknown descriptor layouts must not be rebuilt with a mismatched last field.
@"??_R0extended" = internal constant { ptr, ptr, [10 x i8], i32 } {
 ptr null, ptr null, [10 x i8] c".?AVBar@@\00", i32 7
}
