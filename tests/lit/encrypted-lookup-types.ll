; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-elt,verify' -S %s | %FileCheck %s

; CHECK-LABEL: define i64 @lookup(
; CHECK: zext i8 %n to i64
; CHECK: zext i8 %elt.dec to i64
; CHECK: ret i64 %elt.val
define i64 @lookup(i8 %n) {
 switch i8 %n, label %d [i8 0, label %a i8 1, label %b]
a: ret i64 128
b: ret i64 255
d: ret i64 -1
}
; CHECK-LABEL: define i1 @wide(
; CHECK: icmp ult i128 %n, 2
; CHECK: trunc i8 %elt.dec to i1
; CHECK: ret i1 %elt.val
define i1 @wide(i128 %n) {
 switch i128 %n, label %d [i128 0, label %a i128 1, label %b]
a: ret i1 0
b: ret i1 1
d: ret i1 0
}
; CHECK-LABEL: define i8 @boolean(
; CHECK: zext i1 %n to i64
; CHECK: icmp ult i64 %elt.idx, 2
; CHECK: ret i8 %elt.dec
define i8 @boolean(i1 %n) {
 switch i1 %n, label %d [i1 0, label %a i1 1, label %b]
a: ret i8 200
b: ret i8 255
d: ret i8 0
}
; CHECK-LABEL: define i128 @large_result(
; CHECK: switch i128
; CHECK-NOT: elt.
; CHECK: ret i128 18446744073709551616
define i128 @large_result(i128 %n) {
 switch i128 %n, label %d [i128 0, label %a i128 1, label %b]
a: ret i128 18446744073709551616
b: ret i128 2
d: ret i128 0
}
