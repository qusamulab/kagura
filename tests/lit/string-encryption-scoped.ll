; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-string-scoped -passes='kagura-str,verify' -S %s | %FileCheck %s --check-prefix=XOR
; RUN: %opt -load-pass-plugin=%kagura_plugin -kagura-string-scoped -passes='kagura-str-aes,verify' -S %s | %FileCheck %s --check-prefix=AES
;
; Safe consumers get private stack plaintext and a volatile wipe. Returned
; pointers, throwing calls, writable/capturing parameters and musttail calls retain
; persistent backing storage. A GEP offset must survive argument replacement.

@safe = private unnamed_addr constant [14 x i8] c"scoped-secret\00"
@escaped = private unnamed_addr constant [15 x i8] c"escaped-secret\00"
@writable = private unnamed_addr constant [16 x i8] c"writable-secret\00"
@captured = private unnamed_addr constant [16 x i8] c"captured-secret\00"
@throwing = private unnamed_addr constant [16 x i8] c"throwing-secret\00"
@tail = private unnamed_addr constant [12 x i8] c"tail-secret\00"

declare i32 @consume(ptr nocapture readonly, ptr nocapture readonly) nounwind
declare void @write(ptr nocapture) nounwind
declare void @capture(ptr readonly) nounwind
declare void @may_throw(ptr nocapture readonly)
declare i32 @tail_consumer(ptr nocapture readonly) nounwind

; XOR-LABEL: define i32 @scoped()
; XOR: %[[BUF:.*]] = alloca [14 x i8]
; XOR: call void @kagura_decrypt_{{.*}}(ptr %[[BUF]])
; XOR: getelementptr inbounds [14 x i8], ptr %[[BUF]], i64 0, i64 3
; XOR: call i32 @consume(ptr %[[BUF]], ptr %{{.*}})
; XOR-NEXT: call void @llvm.memset{{.*}}(ptr align {{.*}}%[[BUF]], i8 0, i64 14, i1 true)
; AES-LABEL: define i32 @scoped()
; AES: %[[BUF:.*]] = alloca [14 x i8]
; AES: call ptr @kagura_aesdec_{{.*}}(ptr %[[BUF]])
; AES: call i32 @consume(ptr %[[BUF]], ptr %{{.*}})
; AES-NEXT: call void @llvm.memset{{.*}}(ptr align {{.*}}%[[BUF]], i8 0, i64 14, i1 true)
define i32 @scoped() {
  %r = call i32 @consume(ptr @safe, ptr getelementptr inbounds ([14 x i8], ptr @safe, i64 0, i64 3))
  ret i32 %r
}

; XOR-LABEL: define ptr @escape()
; XOR-NOT: alloca [15 x i8]
; XOR: ret ptr @kagura_enc_
; AES-LABEL: define ptr @escape()
; AES-NOT: alloca [15 x i8]
; AES: ret ptr @kagura_aesbuf_
define ptr @escape() {
  ret ptr @escaped
}

; XOR-LABEL: define void @unsafe()
; XOR-NOT: alloca [16 x i8]
; XOR: call void @write(ptr @kagura_enc_
; XOR: call void @capture(ptr @kagura_enc_
; XOR: call void @may_throw(ptr @kagura_enc_
; AES-LABEL: define void @unsafe()
; AES-NOT: alloca [16 x i8]
; AES: call void @write(ptr @kagura_aesbuf_
; AES: call void @capture(ptr @kagura_aesbuf_
; AES: call void @may_throw(ptr @kagura_aesbuf_
define void @unsafe() {
  call void @write(ptr @writable)
  call void @capture(ptr @captured)
  call void @may_throw(ptr @throwing)
  ret void
}

; XOR-LABEL: define i32 @tail_call(ptr %unused)
; XOR-NOT: alloca [12 x i8]
; XOR: musttail call i32 @tail_consumer(ptr @kagura_enc_
; AES-LABEL: define i32 @tail_call(ptr %unused)
; AES-NOT: alloca [12 x i8]
; AES: musttail call i32 @tail_consumer(ptr @kagura_aesbuf_
define i32 @tail_call(ptr %unused) {
  %r = musttail call i32 @tail_consumer(ptr @tail)
  ret i32 %r
}

; Address-significant globals must keep one persistent address even with safe
; parameter attributes. readonly/nocapture alone do not permit relocation.
@identity = private constant [16 x i8] c"identity-secret\00"
; XOR-LABEL: define i32 @identity_sensitive()
; XOR-NOT: alloca [16 x i8]
; XOR: call i32 @consume(ptr @kagura_enc_
; AES-LABEL: define i32 @identity_sensitive()
; AES-NOT: alloca [16 x i8]
; AES: call i32 @consume(ptr @kagura_aesbuf_
define i32 @identity_sensitive() {
  %r = call i32 @consume(ptr @identity, ptr @identity)
  ret i32 %r
}
