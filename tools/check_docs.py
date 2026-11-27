#!/usr/bin/env python3
"""Compile (and optionally run) Lyra code fences in the documentation.

Conventions
-----------
- ` ```lyra ` blocks are complete Lyra source files. They are compiled with
  `lyra check`; blocks containing `fn main` are also run with `lyra run`.
- ` ```lyra check ` blocks are compiled only. Use them for servers, examples
  that need external files, or programs that intentionally block.
- ` ```lyra no-check ` blocks are skipped. Use them only for multi-file
  illustrations or fragments that cannot stand alone.
- An ` ```output ` block immediately following a `lyra` block is compared with
  the program's standard output.

Usage
-----
    python tools/check_docs.py --lyra build/windows-release/bin/lyra.exe
    python tools/check_docs.py --filter async
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Block:
    path: Path
    line: int
    language: str
    meta: list[str]
    body: str


def iter_blocks(path: Path):
    lines = path.read_text(encoding="utf-8").splitlines()
    blocks: list[Block] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not line.lstrip().startswith("```"):
            i += 1
            continue
        stripped = line.strip()
        info = stripped[3:].strip()
        parts = info.split()
        language = parts[0] if parts else ""
        meta = parts[1:]
        start = i + 1
        body: list[str] = []
        i += 1
        while i < len(lines) and not lines[i].strip().startswith("```"):
            body.append(lines[i])
            i += 1
        if i >= len(lines):
            raise SystemExit(f"{path}:{start}: unterminated code fence")
        blocks.append(Block(path, start, language, meta, "\n".join(body) + "\n"))
        i += 1
    return blocks


def normalize_output(text: str) -> list[str]:
    return [line.rstrip() for line in text.replace("\r\n", "\n").splitlines() if line.strip()]


def compile_or_run(lyra: Path, source: str, run: bool) -> tuple[bool, str, str]:
    directory = Path(tempfile.mkdtemp(prefix="lyra-docs-"))
    try:
        src = directory / "src"
        src.mkdir()
        (directory / "lyra.json5").write_text(
            '{ name: "docs/example", version: "0.1.0", kind: "%s" }\n'
            % ("app" if "fn main" in source else "lib"),
            encoding="utf-8",
        )
        (src / "main.ly").write_text(source, encoding="utf-8")
        command = [str(lyra), "run" if run else "check", str(directory), "--offline"]
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=120,
            cwd=directory,
        )
        return completed.returncode == 0, completed.stdout, completed.stderr
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    default_lyra = (
        Path("build/windows-release/bin/lyra.exe")
        if Path("build/windows-release/bin/lyra.exe").exists()
        else Path("build/dev/bin/lyra")
    )
    parser.add_argument("--lyra", type=Path, default=default_lyra)
    parser.add_argument("--docs", type=Path, default=Path("docs/src/content/docs"))
    parser.add_argument("--filter", default="", help="only files whose path contains this text")
    parser.add_argument("--list", action="store_true", help="list blocks without compiling")
    args = parser.parse_args()

    if not args.lyra.exists():
        raise SystemExit(f"Lyra compiler not found: {args.lyra}")

    files = sorted(args.docs.rglob("*.md")) + sorted(args.docs.rglob("*.mdx"))
    files = [path for path in files if args.filter in path.as_posix()]
    if not files:
        raise SystemExit("no documentation files matched")

    failures: list[str] = []
    checked = 0
    skipped = 0
    for path in files:
        blocks = iter_blocks(path)
        for index, block in enumerate(blocks):
            if block.language != "lyra":
                continue
            mode = block.meta[0] if block.meta else ""
            if mode == "no-check":
                skipped += 1
                continue
            if mode not in ("", "check"):
                raise SystemExit(f"{path}:{block.line}: unknown Lyra block mode {mode!r}")
            expected = None
            if index + 1 < len(blocks):
                following = blocks[index + 1]
                if following.language == "output":
                    expected = normalize_output(following.body)
            should_run = mode == "" and "fn main" in block.body and expected is not None
            if args.list:
                print(f"{path}:{block.line}: {'run' if should_run else 'check'}")
                continue
            checked += 1
            ok, stdout, stderr = compile_or_run(args.lyra, block.body, should_run)
            location = f"{path}:{block.line}"
            if not ok:
                failures.append(f"{location}: {'run' if should_run else 'check'} failed\n{stderr.strip()}")
                continue
            if expected is not None:
                actual = normalize_output(stdout)
                if actual != expected:
                    failures.append(
                        f"{location}: output mismatch\n"
                        f"expected: {expected!r}\n"
                        f"actual:   {actual!r}\n"
                        f"stderr:   {stderr.strip()}"
                    )

    if args.list:
        return 0
    for failure in failures:
        print(failure, file=sys.stderr)
        print("-" * 72, file=sys.stderr)
    print(f"checked {checked} Lyra block(s), skipped {skipped}, failures {len(failures)}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
