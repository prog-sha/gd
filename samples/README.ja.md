# サンプル

[English](README.md) | 日本語

例は0.8向けです。エラーのあるAPIは値と`Err`を別々に返します。

## Hello world: HTML + JSON

```sh
cd samples/web
gd serve main.gd
```

[http://127.0.0.1:8080/](http://127.0.0.1:8080/)をブラウザーで開き、名前を入力して **Say hello** を押すと挨拶が変わります。

- `/?name=Alice` → HTML: **Hello, Alice!**
- `/api/hello?name=Alice` → JSON: `{"name":"Alice","message":"Hello, Alice!"}`

```sh
curl 'http://127.0.0.1:8080/api/hello?name=Alice'
```

ファイルは2つです。

- `main.gd`: ルートとレスポンスのデータ
- `index.html`: `{{message}}`で挨拶を埋め込むHTMLテンプレート

両方のルートで`greeting()`が同じデータを作り、`GD.web.view()`はHTML、`GD.web.json()`はJSONとして返します。ハンドラーはレスポンスと`Err`をそのまま返し、失敗を呼び出し元へ渡します。テンプレートに入れる値は自動でエスケープされます。組み込みAPIなのでパッケージの導入やimportは不要です。

終了は **Ctrl+C**。ポートを変える場合は`gd serve main.gd 9000`と指定します。

## パッケージのimport

`packages/main.gd`は`@import hello`で導入済みのパッケージを呼び出します。初回取得にはネットワークが必要で、`gd.lock`でバージョンを固定します。

```sh
cd samples/packages
gd install --frozen
gd main.gd
```

導入とバージョン固定は[パッケージガイド](https://gd.progsha.com/pkg/)を参照してください。
