# Swift Package Manager 統合

詳細は GitHub の [`integration/swiftpm/README.md`](https://github.com/qusamulab/kagura/blob/main/integration/swiftpm/README.md) (英語) を参照してください。

> ⚠️ 日本語訳は今後追加予定です。最新かつ完全な手順は上記英語版にあります。

## クイックスタート

```swift
let package = Package(
    name: "MyApp",
    dependencies: [
        .package(url: "https://github.com/qusamulab/kagura.git", from: "0.1.0"),
    ],
    targets: [
        .target(
            name: "MyApp",
            dependencies: [
                .product(name: "KaguraRuntime", package: "kagura"),
            ]
        ),
    ]
)
```

SPM で配布されるのは **ランタイムライブラリのみ**。難読化を有効化するにはコンパイラプラグイン (`libKaguraObfuscator.dylib`) を Xcode ビルド設定経由でロードする必要があります — 詳細は [Xcode 統合](xcode.md) を参照。

関連項目は [統合](index.md) を参照。
