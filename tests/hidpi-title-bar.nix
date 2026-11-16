# SPDX-License-Identifier: MIT
#
# Starts hwp on a virtual X server at Xft.dpi 192 and at 96, takes a
# screenshot of each and measures Hwp's own title bar (it draws one; there is
# no window manager decoration): at 192 DPI the window buttons and the program
# icon must be about twice their 96 DPI size, and the menu row must start below
# the taller title bar instead of under it. At 96 DPI nothing may change.
{
  lib,
  runCommand,
  dbus,
  xvfb-run,
  xdotool,
  xrdb,
  imagemagick,
  python3,
}:

hwp:

let
  python = python3.withPackages (p: [ p.pillow ]);
  # Prints "<close glyph height> <program icon pixels> <menu tab top>" for the
  # top-left window, from a screenshot and the window width.
  measure = builtins.toFile "measure-title-bar.py" ''
    import sys
    from PIL import Image

    im = Image.open(sys.argv[1]).convert("RGB")
    width = int(sys.argv[2])
    px = im.load()


    def dark(p):
        return sum(p) < 3 * 110


    def blue(p):
        r, g, b = p
        return b > 150 and r < 120 and b - r > 60


    # The 최소화 / 이전 크기로 / 끝 glyphs in the top-right corner.
    ys = [y for y in range(80) for x in range(width - 60, width) if dark(px[x, y])]
    glyph = max(ys) - min(ys) + 1 if ys else 0
    # The selected (blue) ribbon tab 편집 in the menu row.
    tab = next(y for y in range(150) if sum(1 for x in range(80, 170) if blue(px[x, y])) > 30)
    # The blue program icon (한) in the top-left corner, above the menu row.
    icon = sum(1 for y in range(tab) for x in range(80) if blue(px[x, y]))
    print(glyph, icon, tab)
  '';
in
runCommand "${hwp.name}-hidpi-title-bar"
  {
    nativeBuildInputs = [
      dbus
      xvfb-run
      xdotool
      xrdb
      imagemagick
      python
    ];
  }
  ''
    export DBUS_FATAL_WARNINGS=0
    shot() { # <dpi> <output>
      export HOME=$(mktemp -d)
      # -noreset: Xvfb would reset (dropping Xft.dpi) when xrdb disconnects
      xvfb-run -a -s "-screen 0 2560x1600x24 -noreset" \
        dbus-run-session --config-file=${dbus}/share/dbus-1/session.conf -- sh -c '
        echo "Xft.dpi: '$1'" | xrdb -merge
        ${lib.getExe hwp} > "$HOME/hwp.log" 2>&1 &
        pid=$!
        for _ in $(seq 120); do
          win=$(xdotool search --onlyvisible --name "한글" | head -1)
          [ -n "$win" ] && break
          sleep 1
        done
        if [ -z "$win" ]; then
          echo "hwp did not map a window within 120 s" >&2
          cat "$HOME/hwp.log" >&2
          exit 1
        fi
        sleep 10 # let Hwp finish laying out and painting
        import -window root "'$2'"
        xdotool getwindowgeometry "$win" | sed -n "s/.*Geometry: \([0-9]*\)x.*/\1/p" > "'$2'.width"
        kill $pid
      '
      read -r glyph icon tab <<< "$(python3 ${measure} "$2" "$(cat "$2.width")")"
      echo "$1 DPI: window button glyphs $glyph px high, program icon $icon px, menu row at y=$tab"
    }

    shot 96 low.png
    low_glyph=$glyph low_icon=$icon low_tab=$tab
    shot 192 high.png

    fail=
    if [ "$low_glyph" -gt 11 ] || [ "$low_tab" -gt 30 ]; then
      echo "FAIL: the title bar changed at 96 DPI" >&2; fail=1
    fi
    if [ "$glyph" -lt $((low_glyph * 3 / 2)) ]; then
      echo "FAIL: window buttons are not enlarged at 192 DPI" >&2; fail=1
    fi
    if [ "$icon" -lt $((low_icon * 3)) ]; then
      echo "FAIL: program icon is not enlarged at 192 DPI" >&2; fail=1
    fi
    if [ "$tab" -lt 50 ]; then
      echo "FAIL: at 192 DPI the menu row starts at y=$tab, inside a 50 px title bar" >&2; fail=1
    fi
    [ -z "$fail" ]
    touch $out
  ''
