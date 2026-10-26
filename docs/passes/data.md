# Data Obfuscation

Source: `lib/Transforms/Data/`

| Flag | Pass | Effect |
|:-----|:-----|:-------|
| `-kagura-str` | StringEncryption | XOR-encrypts narrow string literals; lazy decrypt on first access |
| `-kagura-str-aes` | StringEncryptionAES | AES-128-CTR string encryption (requires runtime) |
| `-kagura-string-split` | StringSplit | Fragments long string literals (≥16 bytes) into multiple smaller globals; recombines at runtime on first use |
| `-kagura-wstr` | WideStringEncryption | XOR-encrypts wide strings (wchar_t / char16_t / char32_t) and CFString buffers |
| `-kagura-co` | ConstantObfuscation | Replaces integer constants with MBA expressions |
| `-kagura-sub` | Substitution | Replaces arithmetic/bitwise ops with equivalent MBA |
| `-kagura-genc` | GlobalEncryption | Encrypts private integer globals; inline XOR at load sites |
| `-kagura-mvo` | MemoryValueObfuscation | XOR-encrypts alloca'd integer locals at every store/load site |
| `-kagura-pe` | PointerEncryption | XOR-encrypts alloca'd pointer variables to defeat memory-dump analysis |
| `-kagura-cse-break` | CSEBreak | Duplicates shared SSA expressions so decompilers cannot re-fold them via CSE recovery |

See [Before / After Examples](before-after.md) for `kagura-str` and `kagura-sub` walkthroughs.

`-kagura-string-aggressive` (JSON `"string": {"aggressive": true}`) extends
`str`, `str_aes` and `wstr` to short/format strings and global initializer uses
with startup decryption and permanent backing storage. The default remains
conservative. `str` precedes `str_aes`; disable `str` when AES should own the
narrow strings. See [Broader string protection](../configuration.md#broader-string-protection)
for coverage, initialization ordering and memory-lifetime tradeoffs.

For protecting game-runtime values (HP, currency, etc.) at the C++ level, see
[Game Protection](../game-protection.md) — `Protected<T>` complements `kagura-mvo`
by adding a runtime shadow-copy integrity check.
