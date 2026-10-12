# Bazel 統合

詳細は GitHub の [`integration/bazel/README.md`](https://github.com/qusamulab/kagura/blob/main/integration/bazel/README.md) (英語) を参照してください。

> ⚠️ 日本語訳は今後追加予定です。最新かつ完全な手順は上記英語版にあります。

## クイックスタート

```python
load("@kagura//integration/bazel:kagura.bzl",
     "kagura_cc_binary", "kagura_cc_library")

kagura_cc_binary(
    name = "my_binary",
    srcs = ["main.cc"],
    kagura_passes = ["-kagura-fla", "-kagura-sub", "-kagura-str"],
    kagura_config = "//path/to:policy.json",  # オプション
)
```

詳細は英語版 README を、関連項目は [統合](index.md) を参照してください。
