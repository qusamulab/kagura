# アーキテクチャ

```
kagura/
├── include/kagura/
│   ├── PassRegistry.def    パス一覧。フラグ・パイプライン・JSON ポリシーキー・
│   │                       リンクスモークテストをここから生成する
│   ├── Passes/             lib/Transforms/ のサブディレクトリごとに 1 ヘッダ
│   ├── Passes.h            Passes/ のアンブレラ。Plugin.cpp と fuzzer 用
│   ├── Options.h           CLI フラグ宣言 (レジストリから生成)
│   ├── Utils.h             共通 IR ヘルパー, PRNG, ターゲット判定
│   ├── VM.h + VMOpcodes.def  バイトコード契約。C ランタイムと共有
│   └── game_protect.h      Protected<T> — 唯一の利用者向け公開ヘッダ
├── lib/Transforms/
│   ├── ABI/                C++ RTTI 名と vtable 整合性
│   ├── AntiAnalysis/       アンチデバッグ, 整合性, 呼び出し間接化, ハニー値
│   ├── CFG/                制御フロー難読化パス
│   ├── Data/               文字列 / 定数 / グローバル / ワイド文字列 / メモリ値暗号化
│   ├── Infrastructure/     ポリシー, メトリクス, シンボルマップ, 監査ログ
│   ├── Platform/           iOS (ObjC), Android (JNI)
│   ├── VM/                 関数仮想化
│   ├── Support/            共通ヘルパーの実装と非公開ヘッダ
│   ├── Profiles.def        FAST / BALANCED / STRONG。integration/profiles/*.json を生成
│   ├── Options.cpp         CLI フラグ定義
│   └── Plugin.cpp          パス登録 & パイプライン接続
├── runtime/
│   ├── core/               AES, VM インタプリタ, クラッシュシンボル化, デバイス鍵
│   ├── anti_debug/         アンチデバッグ / アンチ Frida (クロスプラットフォーム POSIX)
│   ├── android/            Android + Linux: ルート検出, アテステーション, /proc, syscall
│   ├── ios/                iOS / macOS: jailbreak 検出, Mach-O 整合性
│   ├── windows/            Windows: IsDebuggerPresent, NtQueryInformationProcess, PE 整合性
│   └── game/               アンチチート, IL2CPP 保護, テレメトリ
├── integration/            Xcode, Gradle, Unity, Unreal, CMake, Bazel, CocoaPods, SPM
├── scripts/
│   ├── cli/                自分のビルドに対して使うツール (設定, strip, diff, variant)
│   ├── eval/               コストモデル, バッテリ推定, ベンチマーク
│   └── ci/                 差分テスト, 再現ビルド検証, プロファイル生成
└── tests/                  CTest + FileCheck lit ベース回帰テスト
```

## パス共通処理

`include/kagura/Utils.h` が共通ヘルパーの API を宣言し、
`lib/Transforms/Support/` の各ファイルが責務ごとに実装します。

| ファイル | 責務 |
|:---------|:-----|
| `ObfuscationPolicy.cpp` | 関数アノテーション、生成シンボル名、保護対象のフィルタ |
| `Random.cpp` | 決定的な PRNG、シード導出、ハッシュ、乱数バイト列 |
| `Target.cpp` | LLVM のターゲットトリプル互換処理とアーキテクチャ判定 |
| `IRUtils.cpp` | 生成 IR のマーカー、整数幅、コンストラクタ、例外処理の確認、IR 書き換え |
| `StringUtils.cpp` | 文字列の選択、参照解析、同期付き復号ガード |
| `AES128.h` | 暗号化パスで共有する非公開 AES 実装 |

CMake が共通処理のソースを自動検出し、unity build でも個別にコンパイルします。
各ファイルは必要な依存ヘッダを自身で include します。パスの実装は各カテゴリの
ディレクトリへ、複数のパスで使うヘルパーは `Support/` へ追加します。

## プラグインエントリポイント

`lib/Transforms/Plugin.cpp` は `PassPluginLibraryInfo` 経由で LLVM **New Pass Manager** にすべてのパスを登録します。2つのことを行います:

1. すべてのパスを名前で公開し、`opt` (`-passes="kagura-str,..."`) または clang の `-mllvm -kagura-<name>` から要求できるようにする。
2. [推奨順序](pass-order.md) を `OptimizerLast` 拡張点に自動接続し、ユーザーが `-fpass-plugin=KaguraObfuscator.dylib` だけで合理的なデフォルトパイプラインを得られるようにする。

## 設定 & オプション

パスごとの有効化フラグと数値チューニングパラメータは `PassRegistry.def` から生成されます。`Options.cpp` の定義と `Options.h` の extern 宣言の両方が生成されるため、どちらもパス一覧とドリフトしません。新しいチューナブルを追加するには `KAGURA_TUNING` 行を足し、パスからフラグを読むだけです。パスごとの行を持たない文字列フラグとパイプライン制御フラグのみ手書きです。

[`kagura-config`](configuration.md) ローダは **パスではありません**。`opt::` の値はパイプライン構築時に読まれてどのパスを追加するか決まるため、`Plugin.cpp` がパイプライン構築前に呼び出します。パイプライン内のパスとして動かしてもその判断には影響できません。
