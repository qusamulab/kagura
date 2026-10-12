# CocoaPods 統合

詳細は GitHub の [`integration/cocoapods/README.md`](https://github.com/qusamulab/kagura/blob/main/integration/cocoapods/README.md) (英語) を参照してください。

> ⚠️ 日本語訳は今後追加予定です。最新かつ完全な手順は上記英語版にあります。

## クイックスタート

`Podfile` に追加:

```ruby
target 'MyApp' do
  pod 'KaguraObfuscator',
      :git    => 'https://github.com/qusamulab/kagura.git',
      :tag    => 'v0.1.0'
end
```

```bash
pod install
```

CocoaPods は Kagura ランタイムをワークスペースに組み込み、プラグインを `OTHER_CFLAGS` 経由でロードする `before_compile` スクリプトフェーズを登録します。プラグインのビルドは手動で実行する必要があります — 詳細は英語版 README を参照してください。

関連項目は [統合](index.md) を参照。
