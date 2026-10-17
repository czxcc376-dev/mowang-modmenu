#!/usr/bin/env bash
# Fetch pinned third-party sources into third_party/ and make them build with a
# current NDK / clang.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

IMGUI_TAG=v1.91.0
DOBBY_COMMIT=5dfc8546954ce3b3198132ab13fddb89ee92cdd7   # jmpews/Dobby (tag: latest)

cd "$TP"

if [ ! -f imgui/imgui.cpp ]; then
  echo ">> imgui $IMGUI_TAG"
  git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui.git
else
  echo ">> imgui already present"
fi

if [ ! -f Dobby/CMakeLists.txt ]; then
  echo ">> Dobby"
  git clone https://github.com/jmpews/Dobby.git Dobby
  ( cd Dobby && git checkout -q "$DOBBY_COMMIT" )
else
  echo ">> Dobby already present"
fi

# ---------------------------------------------------------------------------
# Dobby fixes for modern clang (NDK r27d / clang 18):
#
# 1) closure_bridge_arm64.asm is unguarded Mach-O syntax ("sym@PAGE",
#    "sym@PAGEOFF").  clang's integrated assembler rejects it on ELF with
#    "invalid fixup for add (uimm12)" / "invalid symbol kind for ADRP
#    relocation".  Use the ELF forms instead.
#
# 2) dobby_static never receives BUILD_WITH_TRAMPOLINE_ASM (only the shared
#    target does) and its compile definitions are passed as one bogus token.
# ---------------------------------------------------------------------------
python3 - "$TP/Dobby" <<'PY'
import os, sys

root = sys.argv[1]

asm = os.path.join(root, "source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm")
s = open(asm).read()
if "@PAGE" in s or "@PAGEOFF" in s:
    s = s.replace("cdecl(common_closure_bridge_handler)@PAGE",
                  "cdecl(common_closure_bridge_handler)")
    s = s.replace("cdecl(common_closure_bridge_handler)@PAGEOFF",
                  ":lo12:cdecl(common_closure_bridge_handler)")
    open(asm, "w").write(s)
    print("   patched closure_bridge_arm64.asm -> ELF relocation syntax")
else:
    print("   closure_bridge_arm64.asm already ELF syntax")

cml = os.path.join(root, "CMakeLists.txt")
s = open(cml).read()
old = '''target_compile_definitions(dobby_static PRIVATE
  "COMPILE_DEFINITIONS ${compile_definitions}"
  )'''
new = '''target_compile_definitions(dobby_static PRIVATE
  BUILD_WITH_TRAMPOLINE_ASM
  DOBBY_LOGGING_DISABLE
  )'''
if old in s:
    open(cml, "w").write(s.replace(old, new, 1))
    print("   patched dobby_static compile definitions")
else:
    print("   dobby_static definitions already ok")
PY

echo ">> deps ready"
