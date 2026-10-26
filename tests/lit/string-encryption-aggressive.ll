; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 \
; RUN:     -passes='kagura-str,verify' -S %s | %FileCheck %s --check-prefix=DEFAULT
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -kagura-string-aggressive \
; RUN:     -passes='kagura-str,verify' -S %s | %FileCheck %s --check-prefixes=STARTUP,XOR \
; RUN:     --implicit-check-not='c"value=%d' --implicit-check-not='c"ab\00"' \
; RUN:     --implicit-check-not='c"Alpha' --implicit-check-not='c"xx-offset'
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -kagura-string-aggressive \
; RUN:     -passes='kagura-str,kagura-str-aes,verify' -S %s | %FileCheck %s --check-prefixes=STARTUP,XOR \
; RUN:     --implicit-check-not=kagura_aes --implicit-check-not='c"value=%d' \
; RUN:     --implicit-check-not='c"Alpha' --implicit-check-not='c"xx-offset'
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-seed=42 -kagura-string-aggressive \
; RUN:     -passes='kagura-str-aes,verify' -S %s | %FileCheck %s --check-prefixes=STARTUP,AES \
; RUN:     --implicit-check-not='c"value=%d' --implicit-check-not='c"ab\00"' \
; RUN:     --implicit-check-not='c"Alpha' --implicit-check-not='c"xx-offset'

; The default keeps its heuristics and rejects PHI/global-initializer uses.
; DEFAULT-DAG: @format = private constant [9 x i8] c"value=%d\00"
; DEFAULT-DAG: @short = private constant [3 x i8] c"ab\00"
; DEFAULT-DAG: @name = private constant [6 x i8] c"Alpha\00"
; DEFAULT-DAG: @kagura_enc_

; Startup decryption must preserve nested aggregates, cycles, GEP offsets,
; PHIs and permanent addresses. Metadata, special sections, TLS and public
; storage remain untouched even in aggressive mode.
; STARTUP-DAG: @names = constant { i32, [2 x ptr] } { i32 2, [2 x ptr] [ptr @kagura_{{(enc|aesbuf)}}_{{[0-9]+}}, ptr getelementptr inbounds ([10 x i8], ptr @kagura_{{(enc|aesbuf)}}_{{[0-9]+}}, i32 0, i32 3)] }
; STARTUP-DAG: @cycle = global { ptr, ptr } { ptr @cycle, ptr @kagura_{{(enc|aesbuf)}}_{{[0-9]+}} }
; STARTUP-DAG: @public = constant [7 x i8] c"public\00"
; STARTUP-DAG: @section = private constant [8 x i8] c"section\00", section "metadata"
; STARTUP-DAG: @tls = private thread_local constant [4 x i8] c"tls\00"
; STARTUP-DAG: @annotation = private constant [11 x i8] c"annotation\00"
; STARTUP-DAG: @kagura_test_key = private constant [7 x i8] c"ownkey\00"
; STARTUP-DAG: @llvm.global_ctors = appending global
; XOR-DAG: @kagura_enc_{{[0-9]+}} = private global
; AES-DAG: @kagura_aesbuf_{{[0-9]+}} = private global {{.*}} zeroinitializer
; AES-DAG: @kagura_aesenc_{{[0-9]+}} = private constant

@format = private constant [9 x i8] c"value=%d\00"
@short = private constant [3 x i8] c"ab\00"
@name = private constant [6 x i8] c"Alpha\00"
@offset = private constant [10 x i8] c"xx-offset\00"
@regular = private constant [11 x i8] c"protection\00"
@names = constant { i32, [2 x ptr] } { i32 2, [2 x ptr] [ptr @name, ptr getelementptr inbounds ([10 x i8], ptr @offset, i32 0, i32 3)] }
@cycle = global { ptr, ptr } { ptr @cycle, ptr @name }
@public = constant [7 x i8] c"public\00"
@section = private constant [8 x i8] c"section\00", section "metadata"
@tls = private thread_local constant [4 x i8] c"tls\00"
@annotation = private constant [11 x i8] c"annotation\00"
@llvm.global.annotations = appending global [1 x { ptr, ptr, ptr, i32, ptr }] [{ ptr, ptr, ptr, i32, ptr } { ptr @consumer, ptr @annotation, ptr @annotation, i32 1, ptr null }], section "llvm.metadata"
@kagura_test_key = private constant [7 x i8] c"ownkey\00"

declare void @use(ptr)

; STARTUP-LABEL: define ptr @consumer(
; STARTUP: merge:
; STARTUP-NEXT: %p = phi ptr [ @kagura_{{(enc|aesbuf)}}_{{[0-9]+}}, %a ], [ @kagura_{{(enc|aesbuf)}}_{{[0-9]+}}, %b ]
; STARTUP-NEXT: ret ptr %p
define ptr @consumer(i1 %cond) {
entry:
  call void @use(ptr @format)
  call void @use(ptr @regular)
  call void @use(ptr @public)
  call void @use(ptr @section)
  call void @use(ptr @tls)
  call void @use(ptr @kagura_test_key)
  br i1 %cond, label %a, label %b
a:
  br label %merge
b:
  br label %merge
merge:
  %p = phi ptr [ @short, %a ], [ @name, %b ]
  ret ptr %p
}

; XOR: define internal void @kagura_decrypt_
; AES: define internal void @kagura_aesinit_
; AES: call ptr @kagura_aesdec_{{[0-9]+}}(ptr @kagura_aesbuf_
