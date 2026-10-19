#!/usr/bin/env python3
"""Build an import library for the *installed* Geometry Dash 2.2091 libcocos2d.dll.

Why this exists
---------------
Geode 5.11.2 ships ``loader/include/link/win64/libcocos2d.lib``, an import library
generated from the libcocos2d.dll of the GD version Geode targets (2.2081).  The
cocos2d class definitions of 2.2091 (in ``sdk-compat/``) declare a handful of
functions that 2.2081 did not have, or whose signature changed, and Geode's
generated bindings reference them, so linking a mod against the shipped import
library fails, e.g.::

    GeodeBindings.lib(unity_4_cxx.obj) : error LNK2019: unresolved external symbol
        "public: bool __cdecl DS_Dictionary::loadRootSubDictFromCompressedFile(char const *,bool)"
    ... : error LNK2001: unresolved external symbol
        "public: virtual void __cdecl cocos2d::CCNode::applyScale(float)"
    ... (6 symbols in total: CCNode::applyScale, CCNode::offsetPosition,
         CCKeyboardActionManager::handleGlobalAction, CCTouchDispatcher::registerForcePrio,
         DS_Dictionary::loadRootSubDictFromCompressedFile, DS_Dictionary::saveRootSubDictToCompressedFile)

All six *are* exported by the 2.2091 libcocos2d.dll; only the import library is
out of date.  So we regenerate it from the real DLL with dumpbin + lib and link
it in addition to (symbols identical to 2.2081's resolve from either library;
the new/changed ones only exist in ours).

Usage
-----
    python tools/make-2091-cocos-importlib.py                 # auto-detect the DLL
    python tools/make-2091-cocos-importlib.py --dll <path>    # explicit DLL
    python tools/make-2091-cocos-importlib.py --out <path>    # explicit output .lib

The output defaults to ``sdk-compat/libcocos2d-2091.lib`` (next to the vendored
2.2091 cocos headers), which is what CMakeLists.txt looks for.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_OUT = PROJECT_ROOT / "sdk-compat" / "libcocos2d-2091.lib"

# Export that only exists in the 2.2091 cocos2d ABI - used to refuse a wrong DLL.
VERSION_MARKER = "?applyScale@CCNode@cocos2d@@UEAAXM@Z"

# Symbols that must be present, otherwise the regenerated library will not fix the
# link errors this script exists for.
REQUIRED_EXPORTS = [
    VERSION_MARKER,
    "?offsetPosition@CCNode@cocos2d@@UEAAXMM@Z",
    "?handleGlobalAction@CCKeyboardActionManager@cocos2d@@UEAA_NH_N@Z",
    "?registerForcePrio@CCTouchDispatcher@cocos2d@@QEAAXPEAVCCObject@2@HH@Z",
    "?loadRootSubDictFromCompressedFile@DS_Dictionary@@QEAA_NPEBD_N@Z",
    "?saveRootSubDictToCompressedFile@DS_Dictionary@@QEAA_NPEBD_N@Z",
]

DLL_CANDIDATES = [
    # Explicit override, e.g. a test install next to the real one.
    Path(os.environ["GD_INSTALL_DIR"]) / "libcocos2d.dll"
    if os.environ.get("GD_INSTALL_DIR")
    else None,
    # The usual Steam locations.
    Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"))
    / "Steam" / "steamapps" / "common" / "Geometry Dash" / "libcocos2d.dll",
    Path(os.environ.get("ProgramFiles", r"C:\Program Files"))
    / "Steam" / "steamapps" / "common" / "Geometry Dash" / "libcocos2d.dll",
    # Next to the current directory, for portable installs.
    Path.cwd() / "libcocos2d.dll",
]

VS_TOOL_GLOBS = [
    r"C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\{tool}",
    r"C:\Program Files (x86)\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\{tool}",
]


def find_msvc_tool(name: str) -> Path:
    """Locate dumpbin.exe / lib.exe in any installed MSVC toolset."""
    import glob

    for pattern in VS_TOOL_GLOBS:
        hits = sorted(glob.glob(pattern.format(tool=name)))
        if hits:
            return Path(hits[-1])
    from_path = shutil.which(name)
    if from_path:
        return Path(from_path)
    sys.exit(
        f"error: could not find {name}. Install the MSVC build tools "
        f"(Visual Studio -> 'Desktop development with C++')."
    )


def find_dll(explicit: str | None) -> Path:
    if explicit:
        dll = Path(explicit)
        if not dll.is_file():
            sys.exit(f"error: --dll {dll} is not a file")
        return dll

    for candidate in DLL_CANDIDATES:
        if candidate and candidate.is_file():
            return candidate

    sys.exit(
        "error: could not find libcocos2d.dll. Pass it explicitly:\n"
        "    python tools/make-2091-cocos-importlib.py --dll "
        "\"C:\\path\\to\\Geometry Dash\\libcocos2d.dll\""
    )


def read_exports(dumpbin: Path, dll: Path) -> list[tuple[str, str | None]]:
    """Return [(symbol, forward_target_or_None)] parsed from `dumpbin /exports`."""
    result = subprocess.run(
        [str(dumpbin), "/nologo", "/exports", str(dll)],
        capture_output=True, text=True, errors="replace",
    )
    if result.returncode != 0:
        sys.exit(f"error: dumpbin failed on {dll}:\n{result.stdout}\n{result.stderr}")

    # Export lines look like:   "   2527  9DE 00002CA0 ?applyScale@CCNode@cocos2d@@UEAAXM@Z"
    line_re = re.compile(r"^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8}\s+(\S.*?)\s*$")
    exports: list[tuple[str, str | None]] = []

    for line in result.stdout.splitlines():
        if line.startswith("Summary"):
            break
        match = line_re.match(line)
        if not match:
            continue
        entry = match.group(1)
        forward = None
        marker = " (forwarded to "
        if marker in entry:
            entry, _, rest = entry.partition(marker)
            forward = rest.rstrip(")")
        exports.append((entry.strip(), forward))

    if not exports:
        sys.exit(f"error: no exports found in {dll} - is that a valid PE DLL?")
    return exports


def write_def(exports: list[tuple[str, str | None]], path: Path, dll_name: str) -> None:
    # The LIBRARY statement decides the DLL name the import descriptors point at,
    # i.e. what the Windows loader has to find next to the game exe. Without it
    # lib.exe would name it after the .def file ("libcocos2d-2091.dll"), and the
    # mod would fail to load.
    lines = [f"LIBRARY {dll_name}", "", "EXPORTS"]
    for symbol, forward in exports:
        if forward:
            # e.g. "Name=other_dll.ExportedName"
            lines.append(f"    {symbol}={forward}")
        else:
            lines.append(f"    {symbol}")
    path.write_text("\n".join(lines) + "\n", encoding="ascii", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--dll", help="path to the installed 2.2091 libcocos2d.dll")
    parser.add_argument("--out", default=str(DEFAULT_OUT), help="output import library")
    args = parser.parse_args()

    dll = find_dll(args.dll)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)

    dumpbin = find_msvc_tool("dumpbin.exe")
    lib = find_msvc_tool("lib.exe")

    print(f"dll:      {dll}")
    print(f"dumpbin:  {dumpbin}")
    print(f"lib:      {lib}")

    exports = read_exports(dumpbin, dll)
    names = {symbol for symbol, _ in exports}

    missing = [symbol for symbol in REQUIRED_EXPORTS if symbol not in names]
    if missing:
        listed = "\n".join(f"    {symbol}" for symbol in missing)
        sys.exit(
            f"error: {dll} does not look like the Geometry Dash 2.2091 libcocos2d.dll,\n"
            f"these 2.2091 symbols are missing from its export table:\n{listed}\n"
            f"Pass the DLL of the 2.2091 install with --dll."
        )

    def_path = out.with_suffix(".def")
    write_def(exports, def_path, dll.name)
    print(f"exports:  {len(exports)} (def written to {def_path.name}, LIBRARY {dll.name})")

    result = subprocess.run(
        [str(lib), "/nologo", f"/def:{def_path}", "/machine:x64", f"/out:{out}"],
        capture_output=True, text=True, errors="replace",
    )
    if result.returncode != 0 or not out.is_file():
        sys.exit(f"error: lib.exe failed:\n{result.stdout}\n{result.stderr}")

    print(f"wrote:    {out} ({out.stat().st_size / 1024:.0f} KiB)")

    # Self-check: the import descriptors must name the real DLL, otherwise the mod
    # would only fail at load time in the game.
    if dll.name.encode() not in out.read_bytes():
        sys.exit(f"error: {out} does not reference {dll.name} - the .def LIBRARY line is wrong")

    print("linked symbols check: all 2.2091-specific exports present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
