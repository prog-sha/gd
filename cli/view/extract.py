# Materialize recorded display sources for the view executable.
# Keep those sources out of the default executable and out of versioned duplicates.

import os
import re
import shutil
import subprocess


BASE = "4.7.2-stable"  # Recorded engine tag whose display startup is reused.
ROOT = "tmp/view-src"  # Generated sources. Not part of the default link.
PATCH = "cli/view/online.patch"  # Online hooks layered onto the recorded display sources.
SCRATCH = "tmp/view-patch"  # Work directory where the hooks are applied.

# Translation units whose headless copies dropped the display startup.
FILES = (
    "main/main.cpp",
    "main/main.h",
    "main/main_timer_sync.cpp",
    "main/main_timer_sync.h",
    "main/performance.cpp",
    "main/performance.h",
    "scene/main/node.cpp",
    "scene/main/node.h",
    "scene/main/scene_tree.cpp",
    "scene/main/scene_tree.h",
    "scene/register_scene_types.cpp",
    "scene/register_scene_types.h",
    "servers/register_server_types.cpp",
    "servers/register_server_types.h",
)


# Quoted includes resolve beside the translation unit.
LOCAL_INCLUDE = re.compile(br'^\s*#include\s+"([^"/]+)"', re.M)


def _show(rel):
    return subprocess.check_output(["git", "show", f"{BASE}:{rel}"])


def _hooked():
    """Return recorded sources with the online hooks applied, keyed by path."""
    with open(PATCH, encoding="utf-8") as src:
        names = re.findall(r"^\+\+\+ b/(\S+)", src.read(), re.M)
    shutil.rmtree(SCRATCH, ignore_errors=True)
    try:
        for rel in names:
            dest = os.path.join(SCRATCH, rel)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with open(dest, "wb") as out:
                out.write(_show(rel))
        subprocess.run(["git", "apply", "--unsafe-paths", f"--directory={SCRATCH}", PATCH], check=True)
        hooked = {}
        for rel in names:
            with open(os.path.join(SCRATCH, rel), "rb") as src:
                hooked[rel] = src.read()
        return hooked
    finally:
        shutil.rmtree(SCRATCH, ignore_errors=True)


def _write(rel, data):
    dest = os.path.join(ROOT, rel)
    os.makedirs(os.path.dirname(dest) or ROOT, exist_ok=True)
    if os.path.exists(dest) and open(dest, "rb").read() == data:
        return
    with open(dest, "wb") as out:
        out.write(data)


def _siblings(rel, data):
    parent = os.path.dirname(rel)
    names = []
    for name in LOCAL_INCLUDE.findall(data):
        item = name.decode()
        names.append(os.path.join(parent, item) if parent else item)
    return names


def materialize():
    """Write recorded display sources when their bytes differ."""
    os.makedirs(ROOT, exist_ok=True)
    hooked = _hooked()
    pending = []
    seen = set()
    for rel in FILES:
        data = hooked.get(rel) or _show(rel)
        _write(rel, data)
        seen.add(rel)
        pending.extend(_siblings(rel, data))
    while pending:
        rel = pending.pop(0)
        if rel in seen:
            continue
        seen.add(rel)
        try:
            data = _show(rel)
        except subprocess.CalledProcessError:
            continue
        _write(rel, data)
        pending.extend(_siblings(rel, data))
    link_dir = os.path.join(ROOT, "drivers")
    os.makedirs(link_dir, exist_ok=True)
    link = os.path.join(link_dir, "png")
    target = os.path.abspath("game/drivers_png")
    if os.path.islink(link) and os.path.realpath(link) == target:
        return
    if os.path.lexists(link):
        os.remove(link)
    os.symlink(target, link)


SUITE = "tmp/godot-suite"  # Parent of the recorded tests directory. Not part of the default link.

# Modules whose recorded tests were removed from the default tree.
MODULE_TESTS = (
    "csg",
    "dds",
    "gdscript",
    "gltf",
    "gridmap",
    "jsonrpc",
    "mbedtls",
    "multiplayer",
    "noise",
    "regex",
    "visual_shader",
    "zip",
)


def _data_linked():
    link = os.path.join("tests", "data")
    target = os.path.abspath(os.path.join(SUITE, "tests", "data"))
    return os.path.islink(link) and os.path.realpath(link) == target


def _tests_ready():
    stamp = os.path.join(SUITE, ".tag")
    if not (os.path.isfile(stamp) and open(stamp, encoding="utf-8").read().strip() == BASE and os.path.isdir(os.path.join(SUITE, "tests"))):
        return False
    if not _data_linked():
        return False
    return all(os.path.isdir(os.path.join("modules", name, "tests")) for name in MODULE_TESTS)


def materialize_tests():
    """Write recorded unit-test sources for the display executable."""
    if not _tests_ready():
        os.makedirs(SUITE, exist_ok=True)
        suite = subprocess.check_output(["git", "archive", BASE, "tests"])
        subprocess.run(["tar", "-x", "-C", SUITE], input=suite, check=True)
        paths = [f"modules/{name}/tests" for name in MODULE_TESTS]
        modules = subprocess.check_output(["git", "archive", BASE, *paths])
        subprocess.run(["tar", "-x", "-C", "."], input=modules, check=True)
        with open(os.path.join(SUITE, ".tag"), "w", encoding="utf-8") as out:
            out.write(BASE + "\n")
        _link_data()
    _apply_expect()


def _apply_expect():
    """Overlay expectations that new syntax answers differently."""
    root = os.path.join(os.path.dirname(__file__), "suite_expect")
    if not os.path.isdir(root):
        return
    for dirpath, _, names in os.walk(root):
        for name in names:
            src = os.path.join(dirpath, name)
            dest = os.path.relpath(src, root)
            os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
            data = open(src, "rb").read()
            if os.path.isfile(dest) and open(dest, "rb").read() == data:
                continue
            with open(dest, "wb") as out:
                out.write(data)


def _link_data():
    """Point tests/data at the recorded fixtures beside the executable."""
    link = os.path.join("tests", "data")
    target = os.path.abspath(os.path.join(SUITE, "tests", "data"))
    if _data_linked():
        return
    if os.path.lexists(link):
        raise SystemExit("tests/data is not the recorded fixture link")
    os.symlink(os.path.relpath(target, "tests"), link)
