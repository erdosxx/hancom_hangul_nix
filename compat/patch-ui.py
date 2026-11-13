#!/usr/bin/env python3
"""Give Hwp's formatting toolbar and the ribbon's small buttons Hancom's own
large (32 px) icons.

The toolbar (hwp_frame.xml, <ToolBar id="IDC_Format">) and the small,
caption-less buttons of the ribbon (<ToolBox> tabs: 서식, 편집, ...) use each
command's small 16 px icon "sm_<command>.png". Hancom ships 32 px versions
"lg_<command>.png" of many of them (used by the ribbon's large buttons). This
points those items at the large images with an image="" attribute (for the
toolbar via "tb_*.png" copies, including the ko-KR variants of 가/가/가). On
high-DPI screens libhwp-fastpatch.so then shows these icons at 32 px
(16 x round(DPI / 96)), using the large images unscaled and enlarging the
rest (new, open, save, undo, redo, ...) with Scale2x. The ribbon's large
buttons already use the same lg_ images, so they do not change.

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
    xml = xml[:start] + toolbar + xml[end:]

    # Ribbon: commands whose definition does not choose its own image
    custom = set()
    for path in commands_files(uxxml):
        for m in re.finditer(r'<Command\s+name="([^"]+)"([^>]*)>', open(path, encoding="utf-8").read()):
            if re.search(r'\b(image|smallImage|largeImage)=', m.group(2)):
                custom.add(m.group(1))
    hwp_images = os.path.join(uxxml, "Hancom2016", "Hwp", "Image")
    count = 0

    def large(m):
        nonlocal count
        attrs, command = m.group(1), re.search(r'command="([^"]+)"', m.group(1))
        if not command or "image=" in attrs or command.group(1) in custom:
            return m.group(0)
        # FooButton (split buttons: UnderlineButton, ...) uses Foo's image
        lower = command.group(1).lower()
        for name in (f"lg_{lower}.png", f"lg_{lower[:-6]}.png" if lower.endswith("button") else None):
            if name and any(os.path.exists(os.path.join(d, name)) for d in (images, hwp_images)):
                break
        else:
            return m.group(0)
        count += 1
        return f'<CommandItem command="{command.group(1)}" image="{name}"' + attrs.replace(command.group(0), "", 1)

    def toolbox(m):
        return re.sub(r"<CommandItem\b([^>]*?)(?=/?>)", large, m.group(0))

    xml = re.sub(r"<ToolBox\b.*?</ToolBox>", toolbox, xml, flags=re.S)
    open(frame, "w", encoding="utf-8").write(xml)
    print(f"{frame}: {len(LARGE)} toolbar and {count} ribbon items use 32 px icons")


def commands_files(uxxml):
    for sub in ("Common", "Hwp"):
        d = os.path.join(uxxml, "Hancom2016", sub)
        for f in sorted(os.listdir(d)):
            if f.endswith(".xml") and "commands" in f:
                yield os.path.join(d, f)


if __name__ == "__main__":
    main(sys.argv[1])
