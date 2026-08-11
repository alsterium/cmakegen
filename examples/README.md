# Usage examples

このディレクトリは、`project.yml` の書式とcmakegenのCLIを手元で試すための
使用例です。自動テストのfixtureである `tests/fixtures/` とは別管理です。

各例には手書きの `project.yml` とソースツリーだけを含めています。実行時に
生成される `CMakeLists.txt`、`CMakePresets.json`、
`cmake/cmakegen.generated.cmake`、`.cmakegen/`、`build/` はコミットしません。

## 使い方

リポジトリルートでcmakegenをbootstrapビルドします。

```bash
cmake -S bootstrap -B build/bootstrap
cmake --build build/bootstrap --parallel
generator="$PWD/build/bootstrap/cmakegen"
```

通常ビルド済みなら、`generator` は `build/debug/cmakegen` に置き換えられます。

例えば最小例を実行します。

```bash
cd examples/basic

"$generator" --no-color validate --resolved
"$generator" --no-color generate
cmake --preset debug
cmake --build --preset debug --parallel
"$generator" --no-color generate --check
```

`generate --check` が `No changes.` になれば、生成結果は最新です。

リポジトリルートから実行する場合は、次のようにプロジェクトと出力先を明示
できます。

```bash
"$generator" \
  --project "$PWD/examples/internal-library/project.yml" \
  --root "$PWD/examples/internal-library" \
  --no-color generate
```

## 例の一覧

| 例 | 内容 |
|---|---|
| [`basic`](basic/) | 最小の実行ファイル |
| [`internal-library`](internal-library/) | 静的ライブラリ、公開ヘッダー、内部依存 |
| [`platform`](platform/) | macOS/Linux/Windowsのソースと定義の追加 |
| [`exclusions`](exclusions/) | テスト・ベンチマークのソース除外 |
| [`package`](package/) | `find_package()` と外部CMakeターゲット |

`basic`、`internal-library`、`platform`、`exclusions` は外部パッケージなしで
configure/buildできます。`package` は `fmt` の開発パッケージが必要です。
ただし、`validate`、`dump`、`graph`、`generate` まではfmtがなくても試せます。

## CLIを試す

```bash
"$generator" --no-color dump --format yaml
"$generator" graph
"$generator" graph --format dot --include-external
"$generator" --verbose --no-color validate --resolved
"$generator" --no-color clean --dry-run
```

詳細なschemaは [`docs/04_PROJECT_SPEC.md`](../docs/04_PROJECT_SPEC.md)、CLIは
[`docs/06_CLI_SPEC.md`](../docs/06_CLI_SPEC.md) を参照してください。

