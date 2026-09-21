# Samples / サンプル

## Hello world: HTML + JSON

```sh
cd samples/web
gd serve main.gd
```

Open [http://127.0.0.1:8080/](http://127.0.0.1:8080/). Enter a name and press **Say hello**. / ブラウザーで開き、名前を入力すると挨拶が変わります。

- `/?name=Alice` → HTML: **Hello, Alice!**
- `/api/hello?name=Alice` → JSON: `{"name":"Alice","message":"Hello, Alice!"}`

```sh
curl 'http://127.0.0.1:8080/api/hello?name=Alice'
```

There are two files / ファイルは2つです:

- `main.gd`: routes and response data / URLと返すデータ
- `index.html`: HTML template; `{{message}}` inserts the greeting / 挨拶を埋め込むHTMLテンプレート

Both routes use `greeting()` to build the same data. `GD.web.view()` renders it as HTML; `GD.web.json()` returns JSON. Template values are escaped automatically. No package installation or import is needed for the built-in APIs.

両方のURLで同じ`greeting()`がデータを作り、`GD.web.view()`はHTML、`GD.web.json()`はJSONとして返します。テンプレートに入れる値は自動でescapeされます。組み込みAPIなのでpackageの導入やimportは不要です。

Stop with **Ctrl+C**. For another port, use `gd serve main.gd 9000`. / 終了は**Ctrl+C**。portを変える場合は`gd serve main.gd 9000`と指定します。

## Package import / パッケージのimport

`packages/main.gd` uses `@import hello` to call an installed package. The first download needs a network connection; `gd.lock` pins its version. / 導入したpackageは`@import hello`で使います。初回取得にはnetworkが必要で、`gd.lock`でversionを固定します。

```sh
cd samples/packages
gd install --frozen
gd main.gd
```

See the [package guide](https://gd.progsha.com/pkg/) for installation and version locking. / 詳細は[パッケージガイド](https://gd.progsha.com/pkg/)を参照してください。
