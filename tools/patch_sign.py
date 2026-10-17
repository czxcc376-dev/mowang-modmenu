#!/usr/bin/env python3
"""Patch TapSignKit.getSign into a native method so libmwmenu can implement it.

Why this is needed
------------------
ART's RegisterNatives only works on methods that are already declared native.
getSign() is a plain Java method, so a pure-.so redirect is rejected
("RegisterNatives refused (rc=-1)"), and inline-hooking its compiled body would
need a quick-ABI -> JNI-ABI trampoline (fragile, crashes).

What this changes
-----------------
Exactly one method:
    Lcom/taptap/sdk/kit/internal/utils/TapSignKit;->getSign(Landroid/content/Context;)Ljava/lang/String;
  - access flags gain ACC_NATIVE
  - the code item is dropped (native methods have none)
  - the method keeps its name and prototype

Nothing else in the dex is touched: no new strings, no new types, no new
methods, no table re-indexing.  libmwmenu then calls

    RegisterNatives(TapSignKit.class,
                    {"getSign", "(Landroid/content/Context;)Ljava/lang/String;", fn})

and returns the original certificate digest.

usage: patch_sign.py classes3.dex out.dex
"""
import struct
import sys


class Dex:
    def __init__(self, path):
        self.d = bytearray(open(path, 'rb').read())
        (self.string_ids_size, self.string_ids_off,
         self.type_ids_size, self.type_ids_off,
         self.proto_ids_size, self.proto_ids_off,
         self.field_ids_size, self.field_ids_off,
         self.method_ids_size, self.method_ids_off,
         self.class_defs_size, self.class_defs_off) = struct.unpack_from('<12I', self.d, 0x38)

    # ---- readers ---------------------------------------------------------
    def uleb(self, p):
        d, r, sh = self.d, 0, 0
        while True:
            b = d[p]
            p += 1
            r |= (b & 0x7f) << sh
            if not (b & 0x80):
                return r, p
            sh += 7

    def string(self, idx):
        off = struct.unpack_from('<I', self.d, self.string_ids_off + 4 * idx)[0]
        p = off
        while self.d[p] & 0x80:
            p += 1
        p += 1
        end = self.d.index(b'\x00', p)
        return self.d[p:end].decode('utf-8', 'replace')

    def type_name(self, idx):
        return self.string(struct.unpack_from('<I', self.d, self.type_ids_off + 4 * idx)[0])

    def method(self, idx):
        c, p, n = struct.unpack_from('<HHI', self.d, self.method_ids_off + 8 * idx)
        return self.type_name(c), self.string(n)

    # ---- locate the method ----------------------------------------------
    def find(self, cls_descr, name, proto_params):
        for i in range(self.class_defs_size):
            off = self.class_defs_off + 32 * i
            class_idx = struct.unpack_from('<I', self.d, off)[0]
            if self.type_name(class_idx) != cls_descr:
                continue
            cd_off = struct.unpack_from('<I', self.d, off + 24)[0]
            return self._walk(cd_off, name, proto_params)
        return None

    def _walk(self, cd_off, want_name, want_params):
        d = self.d
        p = cd_off
        sf, p = self.uleb(p)
        inf, p = self.uleb(p)
        dm, p = self.uleb(p)
        vm, p = self.uleb(p)
        for _ in range(sf):
            _, p = self.uleb(p)
            _, p = self.uleb(p)
        for _ in range(inf):
            _, p = self.uleb(p)
            _, p = self.uleb(p)

        for count in (dm, vm):
            midx = 0
            for _ in range(count):
                diff, p = self.uleb(p)
                acc_off = p
                acc, p = self.uleb(p)
                code_off_pos = p
                code_off, p = self.uleb(p)
                midx += diff
                cls, nm = self.method(midx)
                if nm != want_name:
                    continue
                if not self._proto_matches(midx, want_params):
                    continue
                return dict(acc=acc, acc_off=acc_off, code_off=code_off,
                            code_off_pos=code_off_pos, midx=midx, name=nm)
        return None

    def _proto_matches(self, midx, want_params):
        _, proto_idx, _ = struct.unpack_from('<HHI', self.d, self.method_ids_off + 8 * midx)
        shorty_idx = struct.unpack_from('<I', self.d, self.proto_ids_off + 12 * proto_idx)[0]
        ret_idx, params_off = struct.unpack_from('<II', self.d, self.proto_ids_off + 12 * proto_idx + 4)
        items = []
        if params_off:
            n = struct.unpack_from('<I', self.d, params_off)[0]
            for k in range(n):
                items.append(self.type_name(struct.unpack_from('<H', self.d, params_off + 4 + 2 * k)[0]))
        return "".join(items) == want_params


def main():
    src, dst = sys.argv[1], sys.argv[2]
    dx = Dex(src)
    cls = "Lcom/taptap/sdk/kit/internal/utils/TapSignKit;"
    info = dx.find(cls, "getSign", "Landroid/content/Context;")
    if not info:
        sys.exit("getSign(Landroid/content/Context;) not found")

    ACC_NATIVE = 0x0100
    print("getSign: access_flags=0x%x code_off=%d" % (info['acc'], info['code_off']))
    if info['acc'] & ACC_NATIVE:
        print("already native, nothing to do")
        open(dst, 'wb').write(dx.d)
        return

    new_acc = info['acc'] | ACC_NATIVE
    old_len = len(uleb128(info['acc']))
    new_b = uleb128(new_acc)
    if len(new_b) != old_len:
        sys.exit("access_flags uleb128 length would change (%d -> %d); refusing to shift"
                 % (old_len, len(new_b)))

    # code_off -> 0 (native methods have no CodeItem).  Must fit the same length.
    zero = uleb128(0)
    if len(zero) != len(uleb128(info['code_off'])):
        sys.exit("code_off uleb128 length would change; refusing to shift")

    dx.d[info['acc_off']:info['acc_off'] + old_len] = new_b
    dx.d[info['code_off_pos']:info['code_off_pos'] + len(zero)] = zero

    open(dst, 'wb').write(dx.d)
    print("patched: access_flags=0x%x, code_off=0" % new_acc)
    print("wrote", dst)


def uleb128(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


if __name__ == '__main__':
    main()
