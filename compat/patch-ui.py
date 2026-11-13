#!/usr/bin/env python3
"""Give Hwp's formatting toolbar Hancom's own large (32 px) icons.

The toolbar (hwp_frame.xml, <ToolBar id="IDC_Format">) uses each command's
small 16 px icon. Hancom ships 32 px versions ("lg_*.png", used by the ribbon)
of some of them; this copies those to "tb_*.png" (including the ko-KR
variants of 가/가/가) and points the toolbar items at them with an image=""
attribute. On high-DPI screens libhwp-fastpatch.so then shows the toolbar
icons at 32 px (16 x round(DPI / 96)), using these images unscaled and
enlarging the rest (new, open, save, undo, redo, ...) with Scale2x.

usage: patch-ui.py <path to Bin/UxXml>
"""
import os
import re
import shutil
import sys

# toolbar command -> large icon name (Common/Image/lg_<name>.png)
LARGE = {
    "PrintDlg": "printdlg",
    "Bold": "bold",
    "Italic": "italic",
    "UnderlineButton": "underline",
    "StrikeOutButton": "strikeout",
    "TextColorButton": "textfillcolor",
    "ParaBoth": "paraboth",
    "ParaLeft": "paraleft",
    "ParaCenter": "paracenter",
    "ParaRight": "pararight",
    "ParaDistribution": "paradistribution",
}


def main(uxxml):
    images = os.path.join(uxxml, "Hancom2016", "Common", "Image")
    for name in LARGE.values():
        for sub in ("", "ko-KR"):
            src = os.path.join(images, sub, f"lg_{name}.png")
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(images, sub, f"tb_{name}.png"))
            elif not sub:
                sys.exit(f"missing {src}")

    frame = os.path.join(uxxml, "Hancom2016", "Hwp", "hwp_frame.xml")
    xml = open(frame, encoding="utf-8").read()
    start = xml.index('<ToolBar id="IDC_Format"')
    end = xml.index("</ToolBar>", start)
    toolbar = xml[start:end]
    for command, name in LARGE.items():
        toolbar, n = re.subn(
            rf'<CommandItem command="{command}"(?![^>]*\bimage=)',
            f'<CommandItem command="{command}" image="tb_{name}.png"',
            toolbar,
            count=1,
        )
        if n != 1:
            sys.exit(f"{frame}: toolbar item {command} not found")
    open(frame, "w", encoding="utf-8").write(xml[:start] + toolbar + xml[end:])
    print(f"{frame}: {len(LARGE)} toolbar items use 32 px icons")


if __name__ == "__main__":
    main(sys.argv[1])
