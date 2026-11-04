# kagura-mcp

Kagura の設定生成・検証とビルド結果の確認を、MCP 対応クライアントから
利用できるローカル stdio サーバーです。Python 3.10 以上が必要です。

```bash
uv run --project /absolute/path/to/kagura/mcp kagura-mcp \
  --workspace /absolute/path/to/your-app
```

クライアントには上記の実行コマンドと引数を登録します。`mcpServers` 形式の例:

```json
{
  "mcpServers": {
    "kagura": {
      "command": "uv",
      "args": [
        "run", "--project", "/absolute/path/to/kagura/mcp",
        "kagura-mcp", "--workspace", "/absolute/path/to/your-app"
      ]
    }
  }
}
```

パスを実際の絶対パスに置き換えてください。クライアントの PATH に uv が
なければ `command` に uv の絶対パスを指定します。設定は自動登録しません。

| ツール | 用途 |
|:------|:-----|
| `doctor` | LLVM・kagura-opt・プラグイン読み込みと必要ツールの診断 |
| `list_passes` | 定義ファイル由来のパス・調整項目・プロファイルを取得 |
| `generate_config` | プロファイルと上書き設定から JSON の設定案を生成 |
| `validate_config` | JSON オブジェクトまたは設定ファイルをコンパイルなしで検証 |
| `inspect_audit` | JSONL 監査ログの集計、関数検索、ページ単位の取得 |
| `compare_binaries` | バイナリ間のサイズ・セクション・シンボル・文字列数を比較 |

たとえば「課金関数だけを対象に BALANCED の設定を作る」と依頼し、設定案を
確認して保存します。ビルドは既存の統合機能で実行し、監査ログとバイナリ差分を
確認できます。全組み込みプロファイルは `kagura_runtime` のリンクが必要です。

設定案は返すだけで、ファイル書き込みやビルドは実行しません。読み取りは
`--workspace` 配下に限定し、外へ出るシンボリックリンクも拒否します。
JSON とログは 16 MiB、バイナリは各 256 MiB までです。壊れた監査ログ行は
エラーとして返し、部分的な結果であることを明示します。

設定検証はコンパイラより厳しく、未知のルートキー、不正な型・範囲、同一キーの
表記重複、未使用フレーバ内の不正も検出します。ターゲット対応や保護強度は
判定しません。`audit`・`symmap`・`build_id` は JSON の設定キーではなく
コンパイラフラグで指定します。

バイナリ比較には PATH 上の `llvm-nm`/`nm`、`llvm-size`/`size`、`strings` を
使います。`--llvm-bin` は doctor の診断対象、`--plugin` は kagura-opt の
読み込み確認対象を指定します。サーバーはネットワーク待ち受けを行いません。

インストールと開発の詳細は [英語版](mcp.md) またはリポジトリの
[README](https://github.com/qusamulab/kagura/blob/main/mcp/README.md) を参照してください。

`generate_config` の `scoped_strings=true` は `"string": {"scoped": true}` を生成します。
`aggressive_strings` と併用でき、`str` または `str_aes` も有効にする必要があります。
readonly・非キャプチャ・非例外の条件に合う呼び出しだけ一時領域を消去し、
対象外は永続領域を維持します。CLI では `config-gen --scoped-strings` を使います。
`doctor` の `compiler_driver_ready` はドライバの存在と clang/kagura-opt の
対応する同一LLVMメジャーを確認します。ソースのビルドとネイティブ設定検証は
[コンパイラドライバ](../getting-started/compiler-driver.ja.md)を使ってください。
