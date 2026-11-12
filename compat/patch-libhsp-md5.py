#!/usr/bin/env python3
"""Replace the per-repaint MD5 window fingerprint in libhsp.so with a fast hash.

Hancom Office 11.20.0.1520's libhsp.so (Hancom's Win32 emulation layer)
fingerprints a window's back-buffer QImage when a DC is created (CreateDCW)
and again when painting ends, and calls QWidget::update() only if the two
fingerprints differ. The comparison stops the repaint -> WM_PAINT -> repaint
loop, so it has to stay (forcing update() makes Hcell repaint forever and
its grid stops refreshing), but the fingerprint is an MD5 of the whole image:
~4.7 MB hashed twice per paint on a 1600x1000 window, ~30 MB per keystroke,
~90% of Hwp's CPU time while typing. (Hwp 11.20.0.989 had no such check.)

This replaces the fingerprint function (which returns a QByteArray) with
libhsp-fingerprint.s: a 64-bit hash, h = rotl((h ^ w) * K, 31) per 8-byte word,
seeded with the size. Every step is a bijection of h, so a change in any
single word always changes the result. It is about 8x cheaper than MD5.

The original bytes and the PLT call targets are verified first; anything
unexpected (another libhsp.so, an already patched file) aborts the patch.
"""
import sys

FINGERPRINT = 0x169FE0  # file offset == vaddr in the R E segment

# MD5(QImage::bits(), QImage::sizeInBytes()) -> QByteArray(hex digest)
ORIGINAL = bytes.fromhex(
    "41554154554889fd53488d5e184889df4883ec18e8b732f7ff4889df488d5c2408"
    "4989c4e8b732f7ff4889df4989c5e8ecc3fbff4489ea4c89e64889dfe86ec4fbff"
    "4889dfe876c7fbffbaffffffff4889c64889efe82626f7ff4889dfe81ec4fbff48"
    "83c4184889e85b5d415c41"
)

# libhsp-fingerprint.s, assembled at 0x169fe0 with
#   QImage::bits() const = 0xdd2b0, QImage::sizeInBytes() const = 0xdd2c0,
#   QByteArray::QByteArray(const char *, int) = 0xdc660 (PLT stubs)
REPLACEMENT = bytes.fromhex(
    "55534883ec184889fd488d5e184889dfe8bb32f7ff488904244889dfe8bf32f7ff"
    "488b34244889c24883e80872144833164869d2157c4a7f48c1c21f4883c608ebe6"
    "a804740c8b0e4831ca4869d2157c4a7f488914244889e64889ef6a085ae81c26f7"
    "ff4889e84883c4185b5dc3"
)
assert len(REPLACEMENT) <= len(ORIGINAL)


def rel_call(at, target):
    return b"\xe8" + ((target - (at + 5)) & 0xFFFFFFFF).to_bytes(4, "little")


# The original calls must go to the PLT stubs the replacement relies on.
CHECKS = [
    (0x169FF4, rel_call(0x169FF4, 0xDD2B0)),  # QImage::bits() const
    (0x16A035, rel_call(0x16A035, 0xDC660)),  # QByteArray(const char *, int)
]


def main(path):
    data = bytearray(open(path, "rb").read())
    if data[FINGERPRINT : FINGERPRINT + len(ORIGINAL)] != ORIGINAL or any(
        data[off : off + len(want)] != want for off, want in CHECKS
    ):
        sys.exit(f"{path}: unexpected bytes; not the expected libhsp.so (11.20.0.1520)")
    data[FINGERPRINT : FINGERPRINT + len(REPLACEMENT)] = REPLACEMENT
    open(path, "wb").write(data)
    print(f"{path}: replaced MD5 window fingerprint with a fast hash")


if __name__ == "__main__":
    main(sys.argv[1])
