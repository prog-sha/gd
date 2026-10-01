"""Rewrite safe 0.7 result syntax and report source that needs a human decision."""

import argparse
import os
import re
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Token:
    """Locate an identifier outside comments and quoted text."""

    name: str
    start: int
    end: int


def quoted_end(source: str, start: int) -> int:
    """Skip one quoted string, including escapes and triple quotes."""
    quote = source[start]
    triple = source.startswith(quote * 3, start)
    width = 3 if triple else 1
    pos = start + width
    while pos < len(source):
        if source[pos] == "\\":
            pos += 2
        elif source.startswith(quote * width, pos):
            return pos + width
        else:
            pos += 1
    return len(source)


def tokens(source: str) -> list[Token]:
    """Scan code identifiers without interpreting strings or comments."""
    found = []
    pos = 0
    while pos < len(source):
        char = source[pos]
        if char == "#":
            end = source.find("\n", pos)
            pos = len(source) if end < 0 else end
        elif char in "\"'":
            pos = quoted_end(source, pos)
        elif char == "_" or char.isalpha():
            end = pos + 1
            while end < len(source) and (source[end] == "_" or source[end].isalnum()):
                end += 1
            found.append(Token(source[pos:end], pos, end))
            pos = end
        else:
            pos += 1
    return found


def dotted(source: str, left: Token, right: Token) -> bool:
    """Check that two identifiers form one dotted access on the same line."""
    gap = source[left.end:right.start]
    return "\n" not in gap and gap.strip() == "."


def call_open(source: str, end: int) -> int:
    """Locate an immediately following call without crossing a newline."""
    while end < len(source) and source[end] in " \t":
        end += 1
    return end if source[end:end + 1] == "(" else -1


def close_paren(source: str, opening: int) -> int:
    """Find a call's closing parenthesis while skipping nested text."""
    depth = 0
    pos = opening
    while pos < len(source):
        char = source[pos]
        if char in "\"'":
            pos = quoted_end(source, pos)
            continue
        if char == "#":
            end = source.find("\n", pos)
            pos = len(source) if end < 0 else end
            continue
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                return pos
        pos += 1
    return -1


def one_arg(source: str, opening: int, closing: int) -> str | None:
    """Accept one expression without a top-level argument separator."""
    depth = 0
    pos = opening + 1
    while pos < closing:
        char = source[pos]
        if char in "\"'":
            pos = quoted_end(source, pos)
            continue
        if char in "([{":
            depth += 1
        elif char in ")]}":
            depth -= 1
        elif char == "," and depth == 0:
            return None
        pos += 1
    return source[opening + 1:closing].strip() or None


def direct_return(source: str, items: list[Token], index: int, closing: int) -> bool:
    """Limit wrapper removal to a complete return expression."""
    if index == 0 or items[index - 1].name != "return":
        return False
    gap = source[items[index - 1].end:items[index].start]
    if not gap.isspace() or "\n" in gap:
        return False
    end = source.find("\n", closing)
    tail = source[closing + 1:len(source) if end < 0 else end].strip()
    return not tail or tail.startswith("#")


def simple_value(arg: str) -> bool:
    """Accept success expressions that cannot themselves return two values."""
    return arg in {"null", "true", "false"} or bool(re.fullmatch(r"[+-]?(?:\d+|\d+\.\d+)", arg)) or (arg[:1] in "\"'" and quoted_end(arg, 0) == len(arg))


def result_function(source: str, items: list[Token], index: int) -> bool:
    """Require an explicit old result signature before removing its wrapper."""
    for prior_index in range(index - 1, -1, -1):
        prior = items[prior_index]
        if prior.name == "func":
            end = source.find("\n", prior.end)
            header = source[prior.start:len(source) if end < 0 else end]
            return bool(re.search(r"->\s*R\s*:", header))
    return False


def rewrite(source: str) -> tuple[str, list[tuple[int, str]]]:
    """Apply syntax-only changes and list expressions requiring semantic review."""
    items = tokens(source)
    edits: list[tuple[int, int, str]] = []
    issues: list[tuple[int, str]] = []
    handled: set[int] = set()
    for index, token in enumerate(items):
        next_token = items[index + 1] if index + 1 < len(items) else None
        if token.name == "Err" and next_token and dotted(source, token, next_token):
            if next_token.name == "NONE":
                edits.append((token.start, next_token.end, "Err.ERROR"))
                handled.update((index, index + 1))
            elif next_token.name == "err" and call_open(source, next_token.end) >= 0:
                edits.append((token.start, next_token.end, "Err"))
                handled.update((index, index + 1))
        if token.name == "R":
            before = token.start - 1
            while before >= 0 and source[before] in " \t":
                before -= 1
            if before >= 1 and source[before - 1:before + 1] == "->":
                edits.append((token.start, token.end, "Variant, Err"))
                handled.add(index)
            elif next_token and dotted(source, token, next_token) and next_token.name in {"ok", "err"}:
                opening = call_open(source, next_token.end)
                closing = close_paren(source, opening) if opening >= 0 else -1
                arg = one_arg(source, opening, closing) if closing >= 0 else None
                if arg and direct_return(source, items, index, closing) and result_function(source, items, index):
                    # Unwrap a plain success. Err and string failures belong only to R.err.
                    if next_token.name == "ok" and simple_value(arg):
                        edits.extend(((token.start, opening + 1, ""), (closing, closing + 1, "")))
                        handled.update((index, index + 1))
                    elif next_token.name == "err" and (arg.startswith("Err.") or arg.startswith("Err(")):
                        edits.extend(((token.start, opening + 1, ""), (closing, closing + 1, "")))
                        handled.update((index, index + 1))
                    elif next_token.name == "err" and arg[0] in "\"'":
                        edits.append((token.start, next_token.end, "Err"))
                        handled.update((index, index + 1))
            if index not in handled:
                issues.append((token.start, "R needs a two-result rewrite"))
        if token.name in {"ok", "v", "e", "is"} and index > 0 and items[index - 1].name != "R" and source[items[index - 1].end:token.start].strip() == ".":
            if index not in handled:
                issues.append((token.start, f"review result member .{token.name}"))
    for start, end, replacement in sorted(edits, reverse=True):
        source = source[:start] + replacement + source[end:]
    return source, issues


def paths(target: Path) -> list[Path]:
    """Select script files without following directory symlinks."""
    if target.is_file():
        return [target] if target.suffix == ".gd" else []
    return sorted(path for path in target.rglob("*.gd") if path.is_file() and not path.is_symlink() and not any(part.startswith(".") or part == "tmp" for part in path.relative_to(target).parts))


def save(path: Path, source: str) -> None:
    """Replace a script atomically while preserving its permission bits."""
    descriptor, temp = tempfile.mkstemp(prefix=".gd-migrate-", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="") as output:
            output.write(source)
        os.chmod(temp, path.stat().st_mode)
        os.replace(temp, path)
    finally:
        if os.path.exists(temp):
            os.unlink(temp)


def main() -> int:
    """Scan by default, write safe edits on request, and fail on unresolved syntax."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("path", type=Path, help="0.7 script or project directory")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true", help="apply only unambiguous rewrites")
    mode.add_argument("--check", action="store_true", help="fail if changes or manual work remain")
    args = parser.parse_args()
    if not args.path.exists() or not (args.path.is_file() or args.path.is_dir()):
        parser.error("path does not exist")
    files = paths(args.path)
    if not files:
        parser.error("path contains no .gd files")
    changed = 0
    manual = 0
    for path in files:
        original = path.read_text(encoding="utf-8")
        updated, issues = rewrite(original)
        if updated != original:
            changed += 1
            action = "updated" if args.write and not issues else "needs review" if issues else "would update"
            print(f"{path}: {action}")
            if args.write and not issues:
                save(path, updated)
        for pos, message in issues:
            line = original.count("\n", 0, pos) + 1
            print(f"{path}:{line}: {message}")
            manual += 1
    print(f"files={len(files)} changed={changed} manual={manual}")
    return 1 if manual or (args.check and changed) else 0


if __name__ == "__main__":
    raise SystemExit(main())
