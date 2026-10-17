#!/usr/bin/env bash
# Fetch pinned third-party sources into third_party/ and make them build with a
# current NDK / clang (NDK r27d, clang 18).
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
# Dobby @5dfc854 does not build out of the box with a modern NDK. Fixes below.
# ---------------------------------------------------------------------------
python3 - "$TP/Dobby" <<'PY'
import os, sys

root = sys.argv[1]


def edit(rel, pairs, must=True):
    p = os.path.join(root, rel)
    s = open(p).read()
    hit = False
    for old, new in pairs:
        if old in s:
            s = s.replace(old, new, 1)
            hit = True
    if hit:
        open(p, "w").write(s)
    print("   %-58s %s" % (rel, "patched" if hit else ("ok" if not must else "UNCHANGED")))


def create(rel, content):
    p = os.path.join(root, rel)
    if os.path.exists(p):
        print("   %-58s exists" % rel)
        return
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(content)
    print("   %-58s created" % rel)


# 1) Mach-O relocation syntax in an .asm file that has no __APPLE__ guard.
#    clang 18 rejects it on ELF ("invalid fixup for add (uimm12)").
#    NB: @PAGEOFF must be replaced before @PAGE ("@PAGE" is a prefix of "@PAGEOFF").
edit("source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm", [
    ("""adrp TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGE
add TMP_REG_0, TMP_REG_0, cdecl(common_closure_bridge_handler)@PAGEOFF""",
     """adrp TMP_REG_0, :got:cdecl(common_closure_bridge_handler)
ldr TMP_REG_0, [TMP_REG_0, :got_lo12:cdecl(common_closure_bridge_handler)]"""),
])

# 2) dobby_static never receives BUILD_WITH_TRAMPOLINE_ASM and its compile
#    definitions are handed to the compiler as one bogus token.
edit("CMakeLists.txt", [(
    '''target_compile_definitions(dobby_static PRIVATE
  "COMPILE_DEFINITIONS ${compile_definitions}"
  )''',
    '''target_compile_definitions(dobby_static PRIVATE
  BUILD_WITH_TRAMPOLINE_ASM
  DOBBY_LOGGING_DISABLE
  )''')])

# 3) os_arch_features.h uses OSMemory/kReadExecute, which live in
#    PlatformUnifiedInterface/platform.h -- but that header itself includes
#    common.h -> this file, so they are not declared yet (circular include).
edit("common/os_arch_features.h", [
    ('''#if defined(ANDROID)
  auto page = (void *)ALIGN_FLOOR(address, OSMemory::PageSize());
  if (!OSMemory::SetPermission(page, OSMemory::PageSize(), kReadExecute)) {
    return;
  }
#endif''',
     '''#if defined(ANDROID)
  size_t page_size = (size_t)getpagesize();
  auto page = (void *)ALIGN_FLOOR(address, page_size);
  if (mprotect(page, page_size, PROT_READ | PROT_EXEC) != 0) {
    return;
  }
#endif'''),
])

p = os.path.join(root, "common/os_arch_features.h")
s = open(p).read()
if "<sys/mman.h>" not in s:
    lines = s.split("\n")
    lines.insert(1, "#include <sys/mman.h>\n#include <unistd.h>")
    open(p, "w").write("\n".join(lines))
    print("   %-58s includes added" % "common/os_arch_features.h")

# 4) RuntimeModule has only {base, path}, but ProcessRuntime.cc and the
#    SymbolResolver plugin read module.load_address.
edit("source/PlatformUtil/ProcessRuntime.h", [(
    '''struct RuntimeModule {
  void *base;
  char path[1024];
};''',
    '''struct RuntimeModule {
  void *base;
  void *load_address;  /* used by ProcessRuntime.cc and the SymbolResolver plugin */
  char path[1024];
};''')])

# 5) MemRange::start is a method, not a field.
edit("source/Backend/UserMode/PlatformUtil/Linux/ProcessRuntime.cc", [
    ("return (a.start < b.start);", "return (a.start() < b.start());"),
])

# 6) core/arch/Cpu.h is referenced by six translation units but is not shipped
#    in this commit.  Only CpuFeatures is needed; nothing in the Android build
#    links against it (the definitions live in the unused *-dummy.cc files).
create("source/core/arch/Cpu.h", '''#pragma once
// Reconstructed: jmpews/Dobby @5dfc854 includes "core/arch/Cpu.h" from several
// translation units but does not ship the file.
#include <stddef.h>

class CpuFeatures {
public:
  static void FlushICache(void *start, void *end);
  static void ClearCache(void *start, void *end);
};
''')
PY

echo ">> deps ready"
