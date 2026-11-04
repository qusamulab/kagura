# コンパイラドライバ

`kagura` は upstream LLVM 17–22 で C・C++・Objective-C・LLVM IR を処理します。
既定のビルドと新しいリリースには `kagura-opt` とともに同梱されます。
Python 3.9 以上と、kagura-opt と同じメジャーバージョンの clang が必要です。
Apple clang は対象外です。Windows では Python を PATH に追加し、
`bin\kagura.cmd` を使います。ロード可能なプラグインがない環境でも、
静的リンクされた kagura-opt 経由で処理できます。

```bash
export LLVM_PREFIX=/path/to/llvm
build/bin/kagura -O1 -kagura-str -kagura-fla main.c -o app
build/bin/kagura -kagura-config=policy.json main.cpp -o app
build/bin/kagura --config-check policy.json
```

clang による未最適化ビットコード生成 → kagura-opt による最適化と難読化 →
clang によるコード生成 → リンクの順に実行します。前後の clang では LLVM パスを
無効にし、最適化を一度だけ適用します。最適化レベルの既定値は `-O2` です。

`--clang`・`--kagura-opt`・`--plugin`・`--runtime` で明示的なパスを指定できます。
`KAGURA_CLANG`・`KAGURA_OPT`・`KAGURA_RUNTIME`・`LLVM_PREFIX` も使えます。
`--verbose` は実行するコマンドを stderr に表示し、失敗時はツールの終了コードを返します。

ネイティブリンクではビルド／インストール先のランタイムを自動検出します。
オブジェクトだけのリンクでも有効です。C++ のオブジェクトだけをリンクする場合は
`--clang` に clang++ を指定してください。クロスコンパイルでは `-target`・
`--target`・`-arch`・sysroot の指定によりホスト用ランタイムの自動検出を止めます。
対象SDK・対象ランタイムの `--runtime`・必要なシステムライブラリを指定してください。
Windows では `-fms-runtime-lib` をランタイムのCRT設定に合わせます
（CMakeのMSVC既定値は `dll`）。clang-cl 形式ではなく clang 形式のフラグを使います。

複数ソース、ソースとオブジェクトの混在、`-c`・`-S`、それらと併用する
`-emit-llvm` に対応します。レスポンスファイル、LTO、`-fpass-plugin`、
`-save-temps`、複数アーキテクチャの同時処理には対応していません。
詳細は [英語版](compiler-driver.md) を参照してください。

設定だけを検証する場合は `kagura --config-check policy.json`、または
`kagura-opt -kagura-config-check -kagura-config=policy.json` を使います。
未選択フレーバを含むキー・型・範囲を検査し、IRは出力しません。
ソースのコンパイル時も `-kagura-config` を先に検証します。
LLVM がない環境では Python CLI の `config-check` や MCP の `validate_config` を
使えます。これらは独立した検証実装で、重複したJSONキーも検出します。
