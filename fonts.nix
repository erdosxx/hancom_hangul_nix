# Korean fonts bundled with Hancom Office (opt/hnc/hoffice11/Shared/TTF),
# exposed in share/fonts so other programs (e.g. IeumOffice, LibreOffice) can use them.
#
# fontSets selects by font family (read with fc-scan at build time):
#   "hcr"     함초롬바탕 / 함초롬돋움 (HCR Batang / Dotum, incl. Ext): the default
#             fonts of HWP documents
#   "hancom"  every Hancom font: 함초롬, 한컴 고딕, 한컴산뜻돋움, 한컴 바탕/돋움,
#             한컴 윤고딕/윤체/백제/쿨재즈/..., Haan symbol/equation fonts
#   "public"  free Korean public fonts in the bundle: 경기천년, KBIZ한마음,
#             문체부 (MBatang, MDotum, MHunmin, ...), 전주 완판본, 나눔손글씨 붓
#   "all"     every font in Shared/TTF/{All,Hwp,Install}. It includes Microsoft
#             fonts (Arial, Calibri, Times New Roman, Tahoma, Verdana, Malgun
#             Gothic, ...), Bitstream, HY and Yoon fonts. System-wide, these take
#             precedence over metric-compatible fonts (Liberation, Carlito) in
#             browsers and other apps.
{
  lib,
  runCommand,
  fontconfig,
  hancom-office,
  fontSets ? [ "hcr" ],
}:

let
  patterns = {
    hcr = "^HCR ";
    hancom = "^(HCR |Hancom|Haansoft|Haan |Han Santteut|HanGT|HanMJ|HyhwpEQ|FZSong_Superfont)";
    public = "^(Gyeonggi|KBIZ|M(Batang|Dotum|Hunmin|Gung|Sugi|Jemok)|전주 완판본|Nanum Brush)";
    all = ".";
  };
in

assert lib.assertMsg (
  fontSets != [ ] && lib.all (s: patterns ? ${s}) fontSets
) "hancom-fonts: fontSets must be a non-empty list of: ${toString (lib.attrNames patterns)}";

let
  regex = lib.concatMapStringsSep "|" (s: "(${patterns.${s}})") fontSets;
in
runCommand "hancom-fonts-${hancom-office.version}"
  {
    nativeBuildInputs = [ fontconfig ];
    passthru = { inherit fontSets; };
    meta = removeAttrs hancom-office.meta [ "mainProgram" ] // {
      description = "Korean fonts bundled with Hancom Office (${toString fontSets})";
    };
  }
  ''
    src=${hancom-office}/opt/hnc/hoffice11/Shared/TTF
    dest=$out/share/fonts/truetype/hancom
    mkdir -p $dest

    find "$src" -type f \( -iname '*.ttf' -o -iname '*.otf' -o -iname '*.ttc' \) -print0 |
      while IFS= read -r -d "" f; do
        family=$(fc-scan --format '%{family[0]}' "$f" 2>/dev/null) || continue
        if printf '%s\n' "$family" | grep -qE ${lib.escapeShellArg regex}; then
          # copy (not link) so the fonts don't keep the 2.9 GB Hancom Office install alive
          rel=''${f#$src/}
          mkdir -p "$dest/$(dirname "$rel")"
          cp "$f" "$dest/$rel"
          echo "  $family  <- $rel"
        fi
      done

    n=$(find $dest -type f | wc -l)
    echo "hancom-fonts: $n font files (${toString fontSets})"
    [ "$n" -gt 0 ]
  ''
