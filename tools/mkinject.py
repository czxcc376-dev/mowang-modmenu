#!/usr/bin/env python3
"""Repackage the game APK so that libcocos.so loads libmwmenu.so at startup.

Strategy
--------
* add  lib/<abi>/libmwmenu.so                     (our mod menu)
* patch lib/<abi>/libcocos.so  ->  add DT_NEEDED "libmwmenu.so"   (patchelf)
* keep every other entry byte identical

The APK is re-zipped (uncompressed native libs stay uncompressed) and must then be
aligned with `zipalign -p 4` and re-signed before installing.

usage:
  mkinject.py in.apk --so libmwmenu.so --abi arm64-v8a [--so ... --abi ...] --out out.apk
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

NEEDED = "libmwmenu.so"


def patchelf_add_needed(path: str) -> None:
    exe = shutil.which("patchelf")
    if not exe:
        sys.exit("patchelf not found in PATH (apt-get install patchelf)")
    subprocess.run([exe, "--add-needed", NEEDED, path], check=True)
    out = subprocess.run([exe, "--print-needed", path], capture_output=True, text=True, check=True)
    if NEEDED not in out.stdout.split():
        sys.exit("patchelf did not record DT_NEEDED (%s)" % out.stdout.strip())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("--so", action="append", default=[])
    ap.add_argument("--abi", action="append", default=[])
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    if len(a.so) != len(a.abi):
        sys.exit("--so and --abi must be given the same number of times")

    libs = {}
    for so, abi in zip(a.so, a.abi):
        if not os.path.isfile(so):
            sys.exit("missing %s" % so)
        libs[abi] = so

    tmp = tempfile.mkdtemp(prefix="mkinject_")
    patched = {}
    try:
        with zipfile.ZipFile(a.apk) as zin:
            names = zin.namelist()
            for abi in libs:
                target = "lib/%s/libcocos.so" % abi
                if target not in names:
                    sys.exit("apk has no %s" % target)
                raw = zin.read(target)
                p = os.path.join(tmp, "libcocos-%s.so" % abi)
                with open(p, "wb") as f:
                    f.write(raw)
                before = len(raw)
                patchelf_add_needed(p)
                after = os.path.getsize(p)
                patched[abi] = p
                print("[+] patched %s  %d -> %d bytes" % (target, before, after))

            with zipfile.ZipFile(a.out, "w") as zout:
                done = set()
                for info in zin.infolist():
                    name = info.filename
                    if name in done:
                        continue
                    done.add(name)
                    method = info.compress_type

                    if any(name == "lib/%s/libcocos.so" % abi for abi in libs):
                        abi = name.split("/")[1]
                        ni = zipfile.ZipInfo(name, info.date_time)
                        ni.compress_type = method
                        ni.external_attr = info.external_attr
                        with open(patched[abi], "rb") as f:
                            zout.writestr(ni, f.read())
                        continue

                    if any(name == "lib/%s/%s" % (abi, NEEDED) for abi in libs):
                        continue  # replaced below
                    zout.writestr(info, zin.read(name))

                for abi, so in libs.items():
                    dest = "lib/%s/%s" % (abi, NEEDED)
                    ni = zipfile.ZipInfo(dest, (1981, 1, 1, 1, 1, 0))
                    ni.compress_type = zipfile.ZIP_STORED
                    ni.external_attr = 0o100644 << 16
                    with open(so, "rb") as f:
                        zout.writestr(ni, f.read())
                    print("[+] added %s" % dest)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("[+] wrote %s (now run: zipalign -p -f 4 %s aligned.apk && apksigner sign ...)" % (a.out, a.out))


if __name__ == "__main__":
    main()
