#!/usr/bin/env python3
"""Scroll once per mouse-wheel notch instead of `lines` times (Hwp 11.20.0.1520).

Hwp's wheel handler (hwp, 0x3201a0) asks SystemParametersInfoW for the wheel
scroll lines (3 by default) and then calls

    for (i = 0; i < lines; ++i)
        view->OnScroll(code, 0, 1, notches);      // CHwpView::OnScroll

CHwpView::OnScroll's last argument multiplies the line distance, so

    view->OnScroll(code, 0, 1, notches * lines);  // once

scrolls exactly as far. Every scroll step costs ~100+ ms of Hwp's own UI work
(independent of what is repainted), so one step per notch makes the wheel
about 3x faster. The loop body (0x3202a0..0x3202c3) is replaced by:

    mov  0x48(%r12),%rdi        ; view
    mov  lines(%rip),%r8d       ; 0x85db98: wheel scroll lines
    imul %ebp,%r8d              ; * notches
    xor  %edx,%edx
    lea  0x1(%rdx),%ecx
    mov  %r13d,%esi             ; scroll code
    mov  (%rdi),%rax
    call *0x58(%rax)            ; OnScroll, once
    nopl 0x0(%rax,%rax,1)

The original bytes are verified first; anything unexpected aborts the build.
"""
import struct
import sys

START, END = 0x3202A0, 0x3202C3  # vaddr
LINES = 0x85DB98  # global: wheel scroll lines (SPI_GETWHEELSCROLLLINES, default 3)

ORIGINAL = bytes.fromhex(
    "498b7c2448"  # mov 0x48(%r12),%rdi
    "31d2"  # xor %edx,%edx
    "4189e8"  # mov %ebp,%r8d
    "b901000000"  # mov $0x1,%ecx
    "4489ee"  # mov %r13d,%esi
    "83c301"  # add $0x1,%ebx
    "488b07"  # mov (%rdi),%rax
    "ff5058"  # call *0x58(%rax)
    "391dd7d85300"  # cmp %ebx,0x53d8d7(%rip)   (lines)
    "7fdd"  # jg 0x3202a0
)
# the code just before the loop: lines == -1 (page scroll) and lines <= 0 are
# handled there, so the loop only runs for lines >= 1
CONTEXT = [
    (0x320340, "e85b94e4ff"),  # call SystemParametersInfoW
    (0x32034D, "c705" + ((LINES - 0x320357) & 0xFFFFFFFF).to_bytes(4, "little").hex() + "03000000"),
]


def file_offset(data, vaddr):
    phoff = struct.unpack_from("<Q", data, 0x20)[0]
    for i in range(struct.unpack_from("<H", data, 0x38)[0]):
        ptype, _, off, va, _, filesz, _, _ = struct.unpack_from("<IIQQQQQQ", data, phoff + i * 56)
        if ptype == 1 and va <= vaddr < va + filesz:
            return vaddr - va + off
    sys.exit(f"vaddr {vaddr:#x} not mapped")


def main(path):
    data = bytearray(open(path, "rb").read())
    start = file_offset(data, START)
    if data[start : start + len(ORIGINAL)] != ORIGINAL or any(
        data[file_offset(data, va) : file_offset(data, va) + len(bytes.fromhex(h))] != bytes.fromhex(h)
        for va, h in CONTEXT
    ):
        sys.exit(f"{path}: unexpected bytes; not the expected hwp (11.20.0.1520)")
    disp = (LINES - (START + 5 + 7)) & 0xFFFFFFFF
    code = (
        bytes.fromhex("498b7c2448")
        + bytes.fromhex("448b05") + disp.to_bytes(4, "little")
        + bytes.fromhex("440fafc5")
        + bytes.fromhex("31d2")
        + bytes.fromhex("8d4a01")
        + bytes.fromhex("4489ee")
        + bytes.fromhex("488b07")
        + bytes.fromhex("ff5058")
    )
    code += bytes.fromhex("0f1f440000")  # 5-byte nop up to END
    assert len(code) == END - START
    data[start : start + len(code)] = code
    open(path, "wb").write(data)
    print(f"{path}: one OnScroll call per wheel notch")


if __name__ == "__main__":
    main(sys.argv[1])
