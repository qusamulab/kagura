; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -passes='kagura-fsplit,verify' -S %s | %FileCheck %s

declare i32 @__CxxFrameHandler3(...)
declare void @leaf()
declare i32 @twice() returns_twice
; CHECK-LABEL: define void @funclet(
; CHECK: call void @leaf() [ "funclet"(token %pad) ]
; CHECK: catchret from %pad
define void @funclet() personality ptr @__CxxFrameHandler3 {
entry: invoke void @leaf() to label %done unwind label %catch
catch: %cs = catchswitch within none [label %handler] unwind to caller
handler:
 %pad = catchpad within %cs [ptr null, i32 64, ptr null]
 br label %body
body:
 call void @leaf() ["funclet"(token %pad)]
 br label %tail
tail: catchret from %pad to label %done
done: ret void
}
; CHECK-LABEL: define i32 @plain(
; CHECK: call void @__kg_plain_bb
define i32 @plain(i32 %x) {
entry: br label %a
a:
 %y = add i32 %x, 1
 br label %b
b:
 %z = add i32 %y, 2
 br label %c
c:
 %w = add i32 %z, 3
 br label %exit
exit: ret i32 %w
}
