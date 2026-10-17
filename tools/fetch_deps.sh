#!/usr/bin/env bash
# Fetch pinned third-party sources into third_party/.
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

# --- Dobby's own CMakeLists never gives dobby_static the asm-trampoline define
# (only the shared target gets it) and passes the defines as one bogus token.
# Rewrite that block so the static library is built the same way as the shared one.
python3 - "$TP/Dobby/CMakeLists.txt" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '''target_compile_definitions(dobby_static PRIVATE
  "COMPILE_DEFINITIONS ${compile_definitions}"
  )'''
new = '''target_compile_definitions(dobby_static PRIVATE
  BUILD_WITH_TRAMPOLINE_ASM
  DOBBY_LOGGING_DISABLE
  )'''
if old in s:
    open(p, 'w').write(s.replace(old, new, 1))
    print("   patched dobby_static compile definitions")
elif 'BUILD_WITH_TRAMPOLINE_ASM' in s:
    print("   dobby_static definitions already ok")
else:
    sys.exit("cannot patch Dobby/CMakeLists.txt (layout changed)")
PY

echo ">> deps ready"
