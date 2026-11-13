#!/usr/bin/env python3
"""Route libhsp.so call sites to libhwp-fastpatch.so (hwp_fastpatch_entry).

Hancom Office 11.20.0.1520's libhsp.so fingerprints a window's back-buffer
QImage with MD5 when a DC is created and again when it is released, and calls
QWidget::update() (whole window) if the two differ; its ScrollWindow() does
nothing and its UpdateWindow() repaints the whole window. See
libhwp-fastpatch.cpp.

The fingerprint function (0x169fe0) is the only user of libhsp's
QImage::sizeInBytes() import (PLT stub 0xdd2c0). This script
  * replaces the start of the fingerprint function with `jmp 0xdd2c0`
    (a tail call with the original arguments), and
  * points the ScrollWindow / UpdateWindow calls in the ATL::CWindow wrappers
    (0xe99c7, 0xe9c17) at 0xdd2c0 too.
package.nix then uses patchelf to rename the import to hwp_fastpatch_entry,
clear its symbol version, and add libhwp-fastpatch.so as a NEEDED library.
libhwp-fastpatch.cpp tells the call sites apart by their return addresses.

The original bytes (the fingerprint function, its two call sites, the two
wrapper calls and the PLT target) are verified first; anything unexpected
aborts the build.
"""
import sys

FINGERPRINT = 0x169FE0  # file offset == vaddr in the R E segment
SIZEINBYTES_PLT = 0xDD2C0
SCROLLWINDOW_PLT = 0xD97B0
UPDATEWINDOW_PLT = 0xD9890
SCROLL_CALL = 0xE99C7  # in ATL::CWindow::ScrollWindow, returns to 0xe99cc
UPDATE_CALL = 0xE9C17  # in ATL::CWindow::UpdateWindow, returns to 0xe9c1c

# MD5(QImage::bits(), QImage::sizeInBytes()) -> QByteArray(hex digest)
ORIGINAL = bytes.fromhex(
    "41554154554889fd53488d5e184889df4883ec18e8b732f7ff4889df488d5c2408"
    "4989c4e8b732f7ff4889df4989c5e8ecc3fbff4489ea4c89e64889dfe86ec4fbff"
    "4889dfe876c7fbffbaffffffff4889c64889efe82626f7ff4889dfe81ec4fbff48"
    "83c4184889e85b5d415c41"
)


def rel(op, at, target):
    return op + ((target - (at + 5)) & 0xFFFFFFFF).to_bytes(4, "little")


CHECKS = [
    (FINGERPRINT, ORIGINAL),
    (0x16A004, rel(b"\xe8", 0x16A004, SIZEINBYTES_PLT)),  # call sizeInBytes()
    (0x16B069, rel(b"\xe8", 0x16B069, FINGERPRINT)),  # after CreateDCW -> 0x16b06e
    (0x16A6A7, rel(b"\xe8", 0x16A6A7, FINGERPRINT)),  # DC release -> 0x16a6ac
    (SCROLL_CALL - 7, bytes.fromhex("4883ec08488b3f")),  # sub $8,%rsp; mov (%rdi),%rdi
    (SCROLL_CALL, rel(b"\xe8", SCROLL_CALL, SCROLLWINDOW_PLT)),
    (UPDATE_CALL - 7, bytes.fromhex("4883ec08488b3f")),
    (UPDATE_CALL, rel(b"\xe8", UPDATE_CALL, UPDATEWINDOW_PLT)),
]


def main(path):
    data = bytearray(open(path, "rb").read())
    for off, want in CHECKS:
        if data[off : off + len(want)] != want:
            sys.exit(f"{path}: unexpected bytes at {off:#x}; not the expected libhsp.so (11.20.0.1520)")
    for off, code in (
        (FINGERPRINT, rel(b"\xe9", FINGERPRINT, SIZEINBYTES_PLT)),
        (SCROLL_CALL, rel(b"\xe8", SCROLL_CALL, SIZEINBYTES_PLT)),
        (UPDATE_CALL, rel(b"\xe8", UPDATE_CALL, SIZEINBYTES_PLT)),
    ):
        data[off : off + len(code)] = code
    open(path, "wb").write(data)
    print(f"{path}: fingerprint, ScrollWindow and UpdateWindow routed to hwp_fastpatch_entry")


if __name__ == "__main__":
    main(sys.argv[1])
