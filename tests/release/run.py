#!/usr/bin/env python3
# Run the public acceptance suite with isolated files and bounded child lifetimes.
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent  # Directory containing the complete public suite.
ROOT = HERE.parents[1]  # Distribution root owning disposable test output.
CASES = ("language", "data", "async", "storage", "scene", "web")  # Public runtime contracts, run in independent processes.


def duration(value):
    """Accept only a finite positive per-process timeout."""
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("timeout must be finite and positive")
    return result


def run(gd, args, cwd, log, timeout, code=0, marker=None, diagnostic=None):
    """Require both the expected exit status and proof that the intended checks completed."""
    temporary = cwd / "runtime"
    temporary.mkdir(exist_ok=True)
    env = {**os.environ, "GD_CACHE_HOME": str(cwd / "cache"), "TMPDIR": str(temporary), "TMP": str(temporary), "TEMP": str(temporary)}
    with log.open("wb") as stream:
        try:
            result = subprocess.run([str(gd), *args], cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                                    stdin=subprocess.DEVNULL, timeout=timeout, check=False, env=env)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError(f"timeout; see {log}") from error
    text = log.read_text(encoding="utf-8", errors="replace")
    if (result.returncode != code or (marker is not None and marker not in text.splitlines())
            or (diagnostic is not None and diagnostic not in text)):
        raise RuntimeError(f"exit={result.returncode}, expected={code}; see {log}\n{text[-2000:]}")


def suite(gd, work, timeout):
    """Execute public language, storage, service, and offline package contracts."""
    outcomes = []

    def check(name, args, cwd, code=0, marker=None, diagnostic=None):
        """Record each outcome and continue to expose independent failures."""
        try:
            run(gd, args, cwd, work / (name + ".log"), timeout, code, marker, diagnostic)
            outcomes.append({"name": name, "passed": True})
            print("PASS " + name, flush=True)
        except (OSError, RuntimeError) as error:
            outcomes.append({"name": name, "passed": False, "error": str(error)})
            print(f"FAIL {name}: {error}", flush=True)

    # Copy scripts into disposable projects so res:// writes cannot modify the suite.
    for name in (*CASES, "bad_type", "check_failure"):
        project = work / name
        project.mkdir()
        shutil.copyfile(HERE / (name + ".gd"), project / "main.gd")
        if name == "bad_type":
            check("reject-type", ["--strict", "check", "main.gd"], project, code=1, diagnostic="Cannot return")
        elif name == "check_failure":
            check("detect-failure", ["--strict", "main.gd"], project, code=1, marker="release:check_failure:1")
        else:
            check("type-" + name, ["--strict", "check", "main.gd"], project)
            flags = ["--strict"]
            if name == "web":
                flags += ["--allow-net=127.0.0.1", "--no-scene-tree", "serve"]
            check(name, [*flags, "main.gd"], project, marker=f"release:{name}:0")

    # Reject implicit access above the startup directory through content and metadata APIs.
    project = work / "isolation"
    project.mkdir()
    outside = work / "outside.txt"
    outside.write_text("private", encoding="utf-8")
    (project / "inside.txt").write_text("inside", encoding="utf-8")
    path = json.dumps(str(outside))
    (project / "main.gd").write_text(
        '# Verify startup-directory isolation.\nfunc main() -> int:\n'
        '\tvar ck := GD.test.check()\n'
        '\tck.eq(FileAccess.get_file_as_string("res://inside.txt"), "inside", "project read")\n'
        f'\tck.ok(FileAccess.open({path}, FileAccess.READ) == null, "external read")\n'
        f'\tck.ok(not FileAccess.file_exists({path}), "external metadata")\n'
        '\tck.ok(FileAccess.open("../outside.txt", FileAccess.READ) == null, "parent read")\n'
        '\tck.ok(DirAccess.open("res://..") == null, "parent directory")\n'
        '\tprint("release:isolation:%d" % ck.failures)\n\treturn ck.code()\n', encoding="utf-8")
    check("isolation", ["--strict", "-A", "main.gd"], project, marker="release:isolation:0")

    # Build a local package graph without registries, remote tools, or user cache access.
    project = work / "package"
    module = project / "local"
    module.mkdir(parents=True)
    (module / "mod.gd").write_text('# Return the local module value.\nstatic func value() -> int:\n\treturn 42\n', encoding="utf-8")
    (project / "gd.json").write_text(json.dumps({"imports": {"local": "./local"}, "place": "project"}), encoding="utf-8")
    (project / "main.gd").write_text(
        '# Verify an installed local import.\nconst Local = preload("pkg://local/mod.gd")\n'
        '# Return an explicit status after checking the dependency.\nfunc main() -> int:\n'
        '\tvar code := 0 if Local.value() == 42 else 1\n\tprint("release:package:%d" % code)\n\treturn code\n', encoding="utf-8")
    check("package-install", ["--strict", "install"], project)
    check("package-frozen", ["--strict", "install", "--frozen", "--cached-only"], project)
    check("package", ["--strict", "main.gd"], project, marker="release:package:0")
    return outcomes


def main():
    """Select an executable and leave reproducible outcome logs below the distribution tmp directory."""
    parser = argparse.ArgumentParser(description="Check the public runtime contracts without development dependencies.")
    parser.add_argument("--gd", default=os.environ.get("GD", "gd"), help="executable path or command name")
    parser.add_argument("--timeout", type=duration, default=20.0, help="seconds per child process (default: 20)")
    args = parser.parse_args()
    found = shutil.which(args.gd)
    if found is None:
        parser.error("executable not found: " + args.gd)
    gd = Path(found).resolve()
    for name in (*CASES, "bad_type", "check_failure"):
        if not (HERE / (name + ".gd")).is_file():
            parser.error("suite file missing: " + name + ".gd")
    (ROOT / "tmp").mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="release-check-", dir=ROOT / "tmp"))
    outcomes = suite(gd, work, args.timeout)
    report = {"executable": str(gd), "sha256": hashlib.sha256(gd.read_bytes()).hexdigest(), "outcomes": outcomes}
    (work / "results.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    passed = sum(item["passed"] for item in outcomes)
    print(f"pass={passed} fail={len(outcomes) - passed}\nlogs: {work}")
    return int(passed != len(outcomes))


if __name__ == "__main__":
    raise SystemExit(main())
