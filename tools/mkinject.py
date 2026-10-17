#!/usr/bin/env python3
"""Repackage the game APK so that libcocos.so loads libmwmenu.so at startup.

Append-only zip surgery (no full recompression):

  * add   lib/<abi>/libmwmenu.so
  * patch lib/<abi>/libcocos.so  ->  patchelf --add-needed libmwmenu.so   (new bytes appended)
  * every other byte of the original APK is preserved

usage:
  mkinject.py in.apk --so libmwmenu.so --abi arm64-v8a [--so ... --abi ...] --out out.apk

The result is UNSIGNED:  zipalign -p -f 4 out.apk aligned.apk && apksigner sign ...
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

NEEDED = "libmwmenu.so"
LFH = b"PK\x03\x04"
CDH = b"PK\x01\x02"
EOCD = b"PK\x05\x06"


def patchelf_add_needed(path: str) -> None:
    exe = shutil.which("patchelf")
    if not exe:
        sys.exit("patchelf not found in PATH (apt-get install patchelf)")
    subprocess.run([exe, "--add-needed", NEEDED, path], check=True)
    out = subprocess.run([exe, "--print-needed", path], capture_output=True, text=True, check=True)
    if NEEDED not in out.stdout.split():
        sys.exit("patchelf did not record DT_NEEDED (%r)" % out.stdout)
    print("    DT_NEEDED ok: %s" % " ".join(out.stdout.split()[:3]))


def find_eocd(buf: bytes) -> int:
    i = buf.rfind(EOCD)
    if i < 0:
        sys.exit("not a zip: no EOCD")
    return i


def local_entry(name: str, data: bytes, method: int, dos_time=0, dos_date=0) -> bytes:
    """Build a complete local file header (+data). method 8=deflate, 0=store."""
    raw = data
    if method == 8:
        co = zlib.compressobj(level=6, wbits=-15)
        raw = co.compress(data) + co.flush()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    nb = name.encode("utf-8")
    hdr = struct.pack(
        "<4sHHHHHIIIHH", LFH, 20, 0, method, dos_time, dos_date,
        crc, len(raw), len(data), len(nb), 0)
    return hdr + nb + raw


def cd_entry(name: str, method: int, crc: int, csize: int, usize: int,
             offset: int, dos_time: int, dos_date: int, ext_attr: int) -> bytes:
    nb = name.encode("utf-8")
    return struct.pack(
        "<4sHHHHHHIIIHHHHHII", CDH, 20, 20, 0, method, dos_time, dos_date,
        crc, csize, usize, len(nb), 0, 0, 0, 0, ext_attr, offset) + nb


def parse_cd(cd: bytes):
    """Yield (name, dict-of-fields, raw_record_bytes, record_offset_in_cd)."""
    off = 0
    while off + 46 <= len(cd) and cd[off:off + 4] == CDH:
        f = struct.unpack_from("<4sHHHHHHIIIHHHHHII", cd, off)
        method, dos_time, dos_date = f[4], f[5], f[6]
        crc, csize, usize = f[7], f[8], f[9]
        nlen, elen, clen = f[10], f[11], f[12]
        ext = f[15]
        lho = f[16]
        name = cd[off + 46:off + 46 + nlen].decode("utf-8", "replace")
        rec_len = 46 + nlen + elen + clen
        if lho == 0xFFFFFFFF or usize == 0xFFFFFFFF or csize == 0xFFFFFFFF:
            sys.exit("zip64 entry %s not supported" % name)
        yield name, dict(method=method, dos_time=dos_time, dos_date=dos_date, crc=crc,
                         csize=csize, usize=usize, ext=ext, lho=lho,
                         nlen=nlen, rec_len=rec_len), cd[off:off + rec_len], off
        off += rec_len
    if off != len(cd):
        sys.exit("central directory parse mismatch (%d/%d)" % (off, len(cd)))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("apk")
    ap.add_argument("--so", action="append", default=[])
    ap.add_argument("--abi", action="append", default=[])
    ap.add_argument("--dex", action="append", default=[],
                    metavar="ENTRY=FILE",
                    help="replace an in-apk entry, e.g. classes3.dex=patched.dex")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    if not a.so or len(a.so) != len(a.abi):
        sys.exit("--so and --abi must be given the same number of times")

    libs = {}
    for so, abi in zip(a.so, a.abi):
        if not os.path.isfile(so):
            sys.exit("missing %s" % so)
        libs[abi] = so

    dex_repl = {}
    for spec in a.dex:
        if "=" not in spec:
            sys.exit("--dex expects ENTRY=FILE")
        entry, path = spec.split("=", 1)
        if not os.path.isfile(path):
            sys.exit("missing %s" % path)
        dex_repl[entry] = path

    size = os.path.getsize(a.apk)
    with open(a.apk, "rb") as f:
        f.seek(max(0, size - 70000))
        tail = f.read()
        eo = find_eocd(tail) + max(0, size - 70000)
        f.seek(eo)
        eocd = f.read(22)
        _, _, _, _n_this, _n_total, cd_size, cd_off, _clen = struct.unpack("<4sHHHHIIH", eocd[:22])
        f.seek(cd_off)
        cd = f.read(cd_size)

    entries = list(parse_cd(cd))
    by_name = {e[0]: e for e in entries}
    print("[i] %d entries, cd@%d size=%d" % (len(entries), cd_off, cd_size))

    targets = {("lib/%s/libcocos.so" % abi) for abi in libs}
    add_names = {("lib/%s/%s" % (abi, NEEDED)) for abi in libs}
    for t in list(targets) + list(add_names):
        if t not in by_name and t not in add_names:
            sys.exit("apk has no %s" % t)

    tmp = tempfile.mkdtemp(prefix="mkinject_")
    new_payload = []          # (name, local_entry_bytes, cd_fields)
    try:
        # --- patched libcocos.so for each abi -----------------------------
        for abi in libs:
            name = "lib/%s/libcocos.so" % abi
            rec = by_name[name][1]
            p = os.path.join(tmp, "libcocos-%s.so" % abi)
            with open(a.apk, "rb") as f:
                f.seek(rec["lho"])
                lh = f.read(30)
                nlen, elen = struct.unpack_from("<HH", lh, 26)
                f.seek(rec["lho"] + 30 + nlen + elen)
                if rec["method"] == 0:
                    payload = f.read(rec["csize"])
                else:
                    payload = zlib.decompress(f.read(rec["csize"]), -15)
            with open(p, "wb") as g:
                g.write(payload)
            print("[+] patchelf %s (%d bytes)" % (name, len(payload)))
            patchelf_add_needed(p)
            data = open(p, "rb").read()
            new_payload.append((name, (name, data, rec["method"], rec)))

        # --- our mod menu library ----------------------------------------
        for abi, so in libs.items():
            name = "lib/%s/%s" % (abi, NEEDED)
            data = open(so, "rb").read()
            new_payload.append((name, (name, data, 8, None)))

        # --- replaced in-apk entries (e.g. the patched classes3.dex) ------
        for entry, path in dex_repl.items():
            if entry not in by_name:
                sys.exit("apk has no entry %s" % entry)
            rec = by_name[entry][1]
            data = open(path, "rb").read()
            new_payload.append((entry, (entry, data, rec["method"], rec)))
            print("[+] replacing %s with %s (%d bytes)" % (entry, path, len(data)))

        # --- write the new apk -------------------------------------------
        with open(a.apk, "rb") as fin, open(a.out, "wb") as fout:
            # 1. everything up to the old central directory (all local entries)
            remaining = cd_off
            while remaining:
                chunk = fin.read(min(1 << 20, remaining))
                if not chunk:
                    break
                fout.write(chunk)
                remaining -= len(chunk)

            # 2. appended local entries
            appended = {}
            for name, (nm, data, method, rec) in new_payload:
                off = fout.tell()
                dt = rec["dos_time"] if rec else 0
                dd = rec["dos_date"] if rec else (1 << 5) | 1  # 1981-01-01
                fout.write(local_entry(nm, data, method, dt, dd))
                appended[nm] = dict(method=method, crc=zlib.crc32(data) & 0xFFFFFFFF,
                                    csize=fout.tell() - off - 30 - len(nm.encode()),
                                    usize=len(data), offset=off, dos_time=dt, dos_date=dd,
                                    ext=(rec["ext"] if rec else (0o100644 << 16)))
                print("[+] wrote %s @%d (%d bytes)" % (nm, off, len(data)))

            # 3. central directory
            new_cd_off = fout.tell()
            replaced = {}
            for name, (nm, data, method, rec) in new_payload:
                if rec:
                    replaced[name] = appended[nm]
            count = 0
            for name, fields, raw, _ in entries:
                if name in replaced:
                    ap_ = replaced[name]
                    fout.write(cd_entry(name, ap_["method"], ap_["crc"], ap_["csize"],
                                        ap_["usize"], ap_["offset"], ap_["dos_time"],
                                        ap_["dos_date"], ap_["ext"]))
                else:
                    fout.write(raw)
                count += 1
            for name, (nm, data, method, rec) in new_payload:
                if rec:
                    continue
                ap_ = appended[nm]
                fout.write(cd_entry(nm, ap_["method"], ap_["crc"], ap_["csize"],
                                    ap_["usize"], ap_["offset"], ap_["dos_time"],
                                    ap_["dos_date"], ap_["ext"]))
                count += 1

            cd_end = fout.tell()
            fout.write(struct.pack("<4sHHHHIIH", EOCD, 0, 0, count, count,
                                   cd_end - new_cd_off, new_cd_off, 0))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("[+] wrote %s (%d entries) -- UNSIGNED" % (a.out, count))
    print("    next: zipalign -p -f 4 %s aligned.apk && apksigner sign --ks ks.jks --out signed.apk aligned.apk" % a.out)


if __name__ == "__main__":
    main()
