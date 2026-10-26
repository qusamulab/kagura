; RUN: echo '{"passes":{"str":true},"string":{"aggressive":true},"flavors":{"plain":{"string":{"aggressive":false}}}}' > %t.json
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-config=%t.json -kagura-config-strict \
; RUN:     -passes='default<O1>,verify' -S %s | %FileCheck %s --check-prefix=STARTUP
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-config=%t.json -kagura-config-strict \
; RUN:     -kagura-string-aggressive=false -passes='default<O1>,verify' -S %s | %FileCheck %s --check-prefix=PLAIN
; RUN: env KAGURA_FLAVOR=plain %opt -load-pass-plugin=%kagura_plugin -kagura-config=%t.json \
; RUN:     -kagura-config-strict -passes='default<O1>,verify' -S %s | %FileCheck %s --check-prefix=PLAIN
; RUN: env KAGURA_FLAVOR=plain %opt -load-pass-plugin=%kagura_plugin -kagura-config=%t.json \
; RUN:     -kagura-string-aggressive -kagura-config-strict -passes='default<O1>,verify' -S %s | %FileCheck %s --check-prefix=STARTUP
; RUN: echo '{"string":{"agresive":true}}' > %t.bad.json
; RUN: not %opt -load-pass-plugin=%kagura_plugin -kagura-config=%t.bad.json \
; RUN:     -kagura-config-strict -passes='default<O1>' -disable-output %s 2>&1 | %FileCheck %s --check-prefix=ERROR

; STARTUP: @llvm.global_ctors
; STARTUP: @kagura_enc_
; STARTUP: define internal void @kagura_decrypt_
; PLAIN: @format = private {{.*}}constant [9 x i8] c"value=%d\00"
; PLAIN-NOT: @kagura_enc_
; ERROR: unknown key "agresive" in the "string" object
; ERROR: did you mean "aggressive"?

@format = private constant [9 x i8] c"value=%d\00"
declare void @use(ptr)
define void @consumer() {
  call void @use(ptr @format)
  ret void
}
