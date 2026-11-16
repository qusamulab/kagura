; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-deny=excluded -passes='kagura-wstr,verify' -S %s | %FileCheck %s
;
; CHECK-DAG: @shared = private constant [6 x i16] [i16 72, i16 101, i16 108, i16 108, i16 111, i16 0]
; CHECK-DAG: @hot = private constant [6 x i16] [i16 72, i16 101, i16 108, i16 108, i16 111, i16 0]
; CHECK-DAG: @kagura_wflag_
; CHECK-DAG: @kagura_wkey_
@shared = private constant [6 x i16] [i16 72, i16 101, i16 108, i16 108, i16 111, i16 0]
@hot = private constant [6 x i16] [i16 72, i16 101, i16 108, i16 108, i16 111, i16 0]
@permitted = private constant [6 x i16] [i16 72, i16 101, i16 108, i16 108, i16 111, i16 0]
declare void @use(ptr)
; CHECK-LABEL: define void @excluded()
; CHECK-NEXT: call void @use(ptr @shared)
; CHECK-NEXT: ret void
define void @excluded() {
  call void @use(ptr @shared)
  ret void
}
; CHECK-LABEL: define void @allowed()
; CHECK: cmpxchg
; CHECK: ret void
define void @allowed() {
  call void @use(ptr @shared)
  call void @use(ptr @permitted)
  ret void
}
; CHECK-LABEL: define void @critical()
; CHECK-NEXT: call void @use(ptr @hot)
; CHECK-NEXT: ret void
define void @critical() !kagura_hotpath !0 {
  call void @use(ptr @hot)
  ret void
}
!0 = !{}
