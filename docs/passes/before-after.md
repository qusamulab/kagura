# Before / After Examples

What an attacker actually sees, before and after Kagura.

---

## String Encryption (`-kagura-str`)

Default mode decrypts on first access and retains persistent plaintext.
Adding `-kagura-string-scoped` produces this shape for eligible calls only.
These excerpts are schematic; the consumer must be readonly, noncapturing
and nonthrowing.

```llvm
; Before
@secret = private unnamed_addr constant [14 x i8] c"scoped-secret\00"
declare void @consume(ptr nocapture readonly) nounwind

define void @connect() {
  call void @consume(ptr @secret)
  ret void
}
```

```llvm
; After (encrypted bytes and decryptor body omitted)
define void @connect() {
  %buffer = alloca [14 x i8]
  call void @kagura_decrypt_0(ptr %buffer)
  call void @consume(ptr %buffer)
  call void @llvm.memset.p0.i64(ptr %buffer, i8 0, i64 14, i1 true)
  ret void
}
```

Ineligible strings retain persistent storage. AES uses the same eligibility rules.
See [Configuration](../configuration.md#limit-plaintext-lifetime).

---

## String Splitting (`-kagura-string-split`)

**Before** — a single string global at a contiguous offset, easy to spot:

```llvm
@api_key = private constant [29 x i8] c"this is a long secret API key"
```

A binary scan immediately reveals the secret as a contiguous span — even if
encrypted by `kagura-str` first, the *ciphertext* is still contiguous.

**After** — the literal is sliced into N random-length fragments stored in
separate globals; a flag-guarded init stub reassembles them on first use:

```llvm
@kagura_str_frag_0_0 = private constant [6 x i8] c"this i"
@kagura_str_frag_0_1 = private constant [5 x i8] c"s a l"
@kagura_str_frag_0_2 = private constant [6 x i8] c"ong se"
@kagura_str_frag_0_3 = private constant [7 x i8] c"cret AP"
@kagura_str_frag_0_4 = private constant [3 x i8] c"I k"
@kagura_str_frag_0_5 = private constant [1 x i8] c"e"
@kagura_str_frag_0_6 = private constant [1 x i8] c"y"
@kagura_str_recombined_0 = private global [29 x i8] zeroinitializer

define internal void @__kagura_strsplit_0() {
entry:
  %f = load i8, ptr @kagura_str_flag_0
  %g = icmp ne i8 %f, 0
  br i1 %g, label %done, label %init
init:
  memcpy(@kagura_str_recombined_0[0], @kagura_str_frag_0_0, 6)
  memcpy(@kagura_str_recombined_0[6], @kagura_str_frag_0_1, 5)
  ...
  store i8 1, ptr @kagura_str_flag_0
  br label %done
done:
  ret void
}
```

Compose with `kagura-str` (or `kagura-str-aes`) — the string is encrypted
first, then the ciphertext is fragmented. The binary has neither a
contiguous plaintext nor a contiguous ciphertext.

---

## CFG Flattening (`-kagura-fla`)

**Before** — readable if/else chain:

```c
int classify(int x) {
    if (x < 0)  return -1;
    if (x == 0) return 0;
    return 1;
}
```

**After** — switch-dispatched state machine; static CFG analysis fails:

```c
int classify(int x) {
    uint32_t state = 0xA3F1C2B0u;   // initial state (obfuscated)
    int result;
    while (1) {
        switch (state) {
        case 0xA3F1C2B0u:
            state = (x < 0) ? 0x12DE4F91u : 0x7C830B22u;  break;
        case 0x12DE4F91u:
            result = -1; state = 0xFFFFFFFFu;              break;
        case 0x7C830B22u:
            state = (x == 0) ? 0x3A9E17C4u : 0x88D20F5Bu; break;
        case 0x3A9E17C4u:
            result = 0;  state = 0xFFFFFFFFu;              break;
        case 0x88D20F5Bu:
            result = 1;  state = 0xFFFFFFFFu;              break;
        case 0xFFFFFFFFu: return result;
        }
    }
}
```

---

## CSE Break (`-kagura-cse-break`)

**Before** — clang `-O2` shares a common subexpression across users:

```llvm
%t = add i32 %a, %b
%x = mul i32 %t, 2
%y = sub i32 %t, 3
```

Decompilers easily re-fold this back to readable `t = a + b; x = t*2; y = t - 3`.

**After** — each user gets its own private copy:

```llvm
%t = add i32 %a, %b           ; original — first use keeps it
%x = mul i32 %t, 2
%cse.break = add i32 %a, %b   ; fresh clone for the second use
%y = sub i32 %cse.break, 3
```

Functionally identical, but Ghidra / IDA hex-rays / Binary Ninja MLIL all
report this as **two separate computations** during decompilation, hurting
readability of the recovered C.

---

## Arithmetic Substitution (`-kagura-sub`)

**Before:**

```llvm
%sum = add i32 %a, %b
```

**After** — one of 7 MBA equivalents selected at random:

```llvm
; a + b  ≡  (a | b) + (a & b)
%or  = or  i32 %a, %b
%and = and i32 %a, %b
%sum = add i32 %or, %and
```

Decompilers reconstruct the expression, not the original `a + b`, breaking
pattern-matching deobfuscators.
