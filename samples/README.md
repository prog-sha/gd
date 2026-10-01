# Samples

English | [日本語](README.ja.md)

These examples target version 0.8. Fallible APIs return their value and `Err` separately.

## Hello world: HTML + JSON

```sh
cd samples/web
gd serve main.gd
```

Open [http://127.0.0.1:8080/](http://127.0.0.1:8080/). Enter a name and press **Say hello**.

- `/?name=Alice` → HTML: **Hello, Alice!**
- `/api/hello?name=Alice` → JSON: `{"name":"Alice","message":"Hello, Alice!"}`

```sh
curl 'http://127.0.0.1:8080/api/hello?name=Alice'
```

There are two files:

- `main.gd`: routes and response data
- `index.html`: an HTML template; `{{message}}` inserts the greeting

Both routes use `greeting()` to build the same data. `GD.web.view()` renders it as HTML; `GD.web.json()` returns JSON. The handlers forward the response and `Err` together so failures reach the caller. Template values are escaped automatically. No package installation or import is needed for built-in APIs.

Stop with **Ctrl+C**. For another port, use `gd serve main.gd 9000`.

## Package import

`packages/main.gd` uses `@import hello` to call an installed package. The first download needs a network connection; `gd.lock` pins its version.

```sh
cd samples/packages
gd install --frozen
gd main.gd
```

See the [package guide](https://gd.progsha.com/pkg/) for installation and version locking.
