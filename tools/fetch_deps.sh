#!/usr/bin/env bash
# Fetch pinned third-party sources into third_party/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP"

IMGUI_TAG=v1.91.0
SH_COMMIT=593f491be68799f03ee3bab71ce5908846437147   # android-inline-hook (ShadowHook)

cd "$TP"

if [ ! -f imgui/imgui.cpp ]; then
  echo ">> imgui $IMGUI_TAG"
  git clone --depth 1 --branch "$IMGUI_TAG" https://github.com/ocornut/imgui.git
else
  echo ">> imgui already present"
fi

if [ ! -f shadowhook/shadowhook/src/main/cpp/shadowhook.c ]; then
  echo ">> shadowhook"
  git clone https://github.com/bytedance/android-inline-hook.git shadowhook
  ( cd shadowhook && git checkout -q "$SH_COMMIT" )
else
  echo ">> shadowhook already present"
fi

echo ">> deps ready"
