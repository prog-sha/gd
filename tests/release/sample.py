#!/usr/bin/env python3
# Exercise the runnable web sample through real HTTP and always stop its server.
import argparse
import html
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
from urllib.error import HTTPError
from urllib.parse import urlencode
from urllib.request import ProxyHandler, build_opener

ROOT = Path(__file__).resolve().parents[2]  # Distribution root for disposable test output.


def check(gd, sample):
    """Run an isolated copy on an assigned port and verify HTML, JSON, and escaping."""
    (ROOT / "tmp").mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="web-sample-", dir=ROOT / "tmp"))
    for name in ("main.gd", "index.html"):
        shutil.copyfile(sample / name, work / name)
    env = dict(os.environ, GD_CACHE_HOME=str(work / "cache"))
    log = work / "server.log"
    with log.open("w", encoding="utf-8") as output:
        proc = subprocess.Popen([str(gd), "serve", "main.gd", "0"],
                                cwd=work, env=env, stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        try:
            # Discover the actual bound port without racing another process for a free port.
            deadline = time.monotonic() + 15
            while True:
                text = log.read_text(encoding="utf-8")
                ready = re.search(r"^http://127\.0\.0\.1:([0-9]+)$", text, re.MULTILINE)
                if proc.poll() is not None:
                    raise RuntimeError("sample stopped before serving: " + text)
                if ready:
                    base = ready.group(0)
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError("sample did not start: " + text)
                time.sleep(0.05)
            http = build_opener(ProxyHandler({}))
            for name in (None, "Alice", "日本語 😀", '<script>alert("x")</script>&'):
                query = "" if name is None else "?" + urlencode({"name": name})
                value = "world" if name is None else name
                message = "Hello, " + value + "!"
                with http.open(base + "/api/hello" + query, timeout=5) as reply:
                    if reply.status != 200 or reply.headers.get_content_type() != "application/json":
                        raise RuntimeError("JSON response status or content type is incorrect")
                    if json.load(reply) != {"name": value, "message": message}:
                        raise RuntimeError("JSON response data is incorrect")
                with http.open(base + "/" + query, timeout=5) as reply:
                    if reply.status != 200 or reply.headers.get_content_type() != "text/html":
                        raise RuntimeError("HTML response status or content type is incorrect")
                    body = reply.read().decode("utf-8")
                    if "{{" in body or "<h1>" not in body:
                        raise RuntimeError("HTML template was not rendered")
                    # Compare the visible heading while rejecting raw markup from the input.
                    heading = re.search(r"<h1>(.*?)</h1>", body, re.DOTALL)
                    if not heading or html.unescape(heading.group(1)) != message or '<script>' in body:
                        raise RuntimeError("HTML interpolation or escaping is incorrect")
            # Reject byte-valued names without crashing or exposing implementation errors.
            for route in ("/", "/api/hello"):
                for value in ("%FF", "%00"):
                    try:
                        http.open(base + route + "?name=" + value, timeout=5).close()
                        raise RuntimeError("binary name did not return 400")
                    except HTTPError as error:
                        with error:
                            if error.code != 400:
                                raise
            try:
                http.open(base + "/missing", timeout=5).close()
                raise RuntimeError("missing route did not return 404")
            except HTTPError as error:
                with error:
                    if error.code != 404:
                        raise
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
    print("web sample: HTML, JSON, Unicode, escaping, binary input rejection, and 404 passed")
    print("logs: " + str(work))


def main():
    """Select the sample and executable without depending on development-only tools."""
    parser = argparse.ArgumentParser(description="Check the HTML and JSON web sample.")
    parser.add_argument("--gd", default=os.environ.get("GD", "gd"))
    parser.add_argument("--sample", type=Path, default=Path("samples/web"))
    args = parser.parse_args()
    found = shutil.which(args.gd)
    if found is None:
        parser.error("executable not found: " + args.gd)
    check(Path(found).resolve(), args.sample.resolve())


if __name__ == "__main__":
    main()
