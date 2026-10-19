#!/usr/bin/env python3
"""Stage GD 2.2091 cocos2d loader headers into sdk-compat/.

Why this exists
---------------
The mod is built against the released Geode SDK (5.11.2), because that is the
loader the mod has to run on: every `GEODE_DLL` symbol the mod imports must exist
in the installed Geode.dll.

The *bindings* however describe GD 2.2091, and GD 2.2091 changed parts of the
cocos2d ABI that Geode's 5.11.2 loader headers still describe as 2.2081:

  * two new mixins that are base classes of game classes (`CCTouchClipper`,
    `CCKeyboardActionManager`) -- 2.2091 binding data references them
  * `CCMouseDelegate::scrollWheel` now returns `bool`
  * `DS_Dictionary::loadRootSubDictFromCompressedFile` /
    `saveRootSubDictToCompressedFile` gained a trailing `bool`
  * `CCTouchDispatcher::registerForcePrio` gained a third `int`
  * `CCTouchDispatcher` gained three fields, `CCNode` gained two virtuals and
    renamed `m_bUnkBool2` to `m_bDisableScheduling`
  * CCTouchClipper/CCKeyboardDispatcher/... gained members

Geode's `v6` branch is where 2.2091 support is being written, so its
`loader/include/Geode/cocos` tree is the authoritative description of the 2.2091
cocos2d ABI. This script copies the files that differ from the SDK's own copy
into `sdk-compat/`, which is put first on the include path, so the compiler sees
2.2091 cocos2d declarations while still seeing 5.11.2 Geode loader APIs.

The copied tree is the *whole* `Geode/cocos` directory rather than only the files
that differ. Partial shadowing is not sound: these headers include each other with
self-relative paths (`#include "../../cocoa/CCGeometry.h"`, `#include "CCNode.h"`),
so a file left in the SDK would be reached from an SDK header and the compiler
would end up with two different definitions of the same cocos2d class in one
program (an ODR violation, and a genuinely wrong `sizeof`/vtable). Copying the
tree keeps every cocos2d include inside the 2.2091 set.

v6 also refactored Geode's own filesystem API (`asp::fs`), and three cocos2d
declarations were swept along with it. Those are reverted after copying (see
LOCAL_FIXES): the 2.2091 *game* ABI change in the same declarations is kept, but
the `asp::fs` type is put back to the released loader's `std::filesystem` form.

Usage:
    python tools/sync-2.2091-headers.py --sdk <5.11.2 sdk> --v6 <v6 checkout>
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

# Object files in the tree are never compiled by a mod build (implementations live
# in the game's libcocos2d.dll), so they are not copied.
SKIP_SUFFIXES = (".cpp", ".mm", ".m")

# Files where v6's copy mixes a genuine 2.2091 *game* ABI change with Geode's own
# v6-only `asp::fs` refactor. The game change has to be kept (the 2.2091 binding
# data and the game's exports use it), the refactor must not be: `asp::fs::Path`
# belongs to an API the released 5.11.2 loader does not export.
#
#   ZipUtils.h: `ccDeflateMemory` gained a trailing `int unk` argument and
#   `compressString` gained a trailing `int unk`, and the file's `gd::string`
#   return/parameter types became `std::string` (the same type). Both are needed.
#   `ZipFile::unzipAllTo` however was changed from `std::filesystem::path` to
#   `asp::fs::Path`, which is Geode's own type, so it is reverted here.
#
# Every replacement must match, otherwise the script fails loudly instead of
# silently producing a tree that no longer describes 2.2091 or no longer links.
LOCAL_FIXES = {
    "support/zip_support/ZipUtils.h": [
        ("#include <asp/fs/Path.hpp>", "#include <filesystem>"),
        ("bool unzipAllTo(asp::fs::Path const& path);",
         "bool unzipAllTo(std::filesystem::path const& path);"),
    ],
}


def main() -> None:
    project = Path(__file__).resolve().parent.parent
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sdk", default=os.environ.get("GEODE_SDK"),
                    help="released Geode SDK directory (has loader/include/Geode/cocos)")
    ap.add_argument("--v6", default=os.environ.get("GEODE_V6_SDK"),
                    help="Geode v6 checkout (same layout)")
    ap.add_argument("--out", default=None,
                    help="target directory (default: <project>/sdk-compat)")
    args = ap.parse_args()

    if not args.sdk or not args.v6:
        raise SystemExit("need --sdk and --v6 (or GEODE_SDK / GEODE_V6_SDK)")

    sdk_cocos = Path(args.sdk) / "loader" / "include" / "Geode" / "cocos"
    v6_cocos = Path(args.v6) / "loader" / "include" / "Geode" / "cocos"
    for p in (sdk_cocos, v6_cocos):
        if not p.is_dir():
            raise SystemExit(f"not a cocos header dir: {p}")

    out = Path(args.out) if args.out else project / "sdk-compat"
    dest_root = out / "Geode" / "cocos"

    kept: set[Path] = set()
    copied = changed = 0
    for src in sorted(v6_cocos.rglob("*")):
        if not src.is_file() or src.suffix in SKIP_SUFFIXES:
            continue
        rel = src.relative_to(v6_cocos)
        dst = dest_root / rel
        kept.add(dst)
        sdk_file = sdk_cocos / rel
        if not sdk_file.is_file() or sdk_file.read_bytes() != src.read_bytes():
            changed += 1  # differs from (or is new since) the SDK: a 2.2091 update
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        copied += 1

    removed = 0
    for path in sorted(dest_root.rglob("*"), reverse=True):
        if path.is_file() and path not in kept:
            print("removing stale:", path.relative_to(out).as_posix())
            path.unlink()
            removed += 1

    sdk_count = sum(1 for p in sdk_cocos.rglob("*") if p.is_file())

    fixed = 0
    for rel, edits in LOCAL_FIXES.items():
        path = dest_root / rel
        text = path.read_text(encoding="utf-8")
        for old, new in edits:
            if old not in text:
                raise SystemExit(
                    f"local fix does not apply to {rel!r}: {old!r} not found.\n"
                    f"v6 likely changed; update LOCAL_FIXES in this script.")
            text = text.replace(old, new)
            fixed += 1
        path.write_text(text, encoding="utf-8", newline="")

    print(f"staged {copied} file(s) into {dest_root}")
    print(f"  {changed} of them differ from the SDK's own copy "
          f"(the 2.2081 -> 2.2091 changes); the SDK has {sdk_count} files there")
    if fixed:
        print(f"  applied {fixed} local fix(es) to undo v6-only API refactors")
    if removed:
        print(f"  removed {removed} stale file(s)")


if __name__ == "__main__":
    sys.exit(main())
