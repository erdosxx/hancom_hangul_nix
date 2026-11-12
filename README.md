# Hancom Hwp for Nix — native, no Docker

Packages Hwp (Hangul word processor) from `hoffice_11.20.0.1520_amd64.deb`
(Hancom Office 2022 Beta for Linux) with `autoPatchelfHook` (same approach as
`viber` in nixpkgs). Only Hwp is installed; Hcell, Hshow and Hword are removed
(1.9 GB instead of 2.8 GB, see "How it works").

Hancom no longer offers this `.deb` on its CDN. It is fetched from the
[bada-ie.com mirror](https://www.bada-ie.com/board/view/?code=all&uid=4366),
which only serves it with a matching `Referer` header (set in `package.nix`).
If the mirror goes away, add your copy of the file to the store:

```shell
nix-store --add-fixed sha256 hoffice_11.20.0.1520_amd64.deb
```

## Run

```shell
nix run github:erdosxx/hancom_hangul_nix            # or: nix run .
nix run . -- document.hwp
```

## Outputs

| Output                       | Description                                                  |
| ---------------------------- | ------------------------------------------------------------ |
| `packages.*.default`         | = `hancom-hwp` (fcitx5)                                      |
| `packages.*.hancom-hwp`      | Uses nixpkgs Qt 5.15 + `fcitx5-qt` plugin (**fcitx5 input**) |
| `packages.*.hancom-hwp-ibus` | Uses bundled Qt 5.11 + its ibus plugin (**ibus input**)      |
| `packages.*.hancom-fonts`    | Bundled Korean fonts for other apps (see below)              |
| `apps.*.default`             | Runs `hwp`                                                   |
| `overlays.default`           | Adds `pkgs.hancom-hwp`, `pkgs.hancom-fonts`                  |

The package installs `bin/hwp`, its `.desktop` entry, icons and MIME types.

## Use in NixOS / home-manager

```nix
inputs.hancom-hwp.url = "github:erdosxx/hancom_hangul_nix";

# package
home.packages = [ inputs.hancom-hwp.packages.${system}.default ];
# or keep the old style used with the docker flake
run_hwp = inputs.hancom-hwp.apps.${system}.default.program;
```

Customize with `override`:

```nix
inputs.hancom-hwp.packages.${system}.default.override {
  scaleFactor = "1.2";        # QT_SCALE_FACTOR (default "1"; env var still wins)
  inputMethod = "fcitx5";     # or "ibus"
  hwpLocale   = "ko_KR.UTF-8";
  fastRepaint = true;         # false = leave Hancom's libhsp.so unpatched
}
```

With the overlay, `pkgs.hancom-hwp` is unfree, so allow it
(`nixpkgs.config.allowUnfree` or an `allowUnfreePredicate`).

## Bundled Korean fonts for other apps (`hancom-fonts`)

Hwp ships its fonts inside its install directory, so other programs can't see
them. For example, IeumOffice reports "이 PC에 없는 글꼴 … 함초롬돋움, 함초롬바탕"
for HWP documents. `hancom-fonts` copies them into `share/fonts`:

```nix
# home-manager (with fonts.fontconfig.enable = true)
home.packages = [ inputs.hancom-hwp.packages.${system}.hancom-fonts ];
# or NixOS
fonts.packages = [ inputs.hancom-hwp.packages.${system}.hancom-fonts ];
```

`fontSets` picks the fonts by family name (default `[ "hcr" ]`):

| set      | fonts                                                                                   | size   |
| -------- | --------------------------------------------------------------------------------------- | ------ |
| `hcr`    | 함초롬바탕 / 함초롬돋움 (HCR Batang / Dotum, incl. Ext): the default fonts of HWP documents | 196 MB |
| `hancom` | all Hancom fonts: 함초롬, 한컴 고딕, 한컴산뜻돋움, 한컴바탕/돋움, 한컴 윤고딕/윤체/…, Haan symbol fonts | 454 MB |
| `public` | free Korean public fonts in the bundle: 경기천년, KBIZ한마음, 문체부, 전주 완판본, 나눔손글씨 붓 | 134 MB |
| `all`    | every font in `Shared/TTF`, **including Microsoft fonts** (Arial, Calibri, Times New Roman, Tahoma, Verdana, Malgun Gothic, …) | 760 MB |

```nix
inputs.hancom-hwp.packages.${system}.hancom-fonts.override { fontSets = [ "hancom" "public" ]; }
```

- `all` is not the default. System-wide, the Microsoft fonts take precedence
  over metric-compatible fonts (Liberation, Carlito) in browsers and other apps.
- The fonts are copied, not linked, so this package doesn't keep the 2.9 GB
  Hancom Office install in your closure.
- Building it needs the Hancom Office `.deb`, which is downloaded or taken from the store.
- The fonts come with Hancom's unfree package, so treat them as licensed for use
  with Hwp. Don't redistribute them.

## How it works

- The `.deb` is fetched with the `Referer` header the mirror requires.
- `autoPatchelfHook` patches all ELF files. Debian's `libcurl-gnutls.so.4` is
  provided as a symlink to `curlWithGnuTls`.
- **Hwp only:** Hcell, Hshow and Hword (their executables, libraries,
  resources, strings and templates, listed in `otherAppFiles` in
  `package.nix`) are deleted. No remaining binary links them or names them,
  and traced Hwp sessions (blank document, `.hwp`, `.hwpx`) never open them.
  This also drops OpenSSL 1.1 (only Hword used it) and ICU 63 (only the
  Hcell/Hshow equation library used it).
- Extra runtime libraries: GStreamer + PulseAudio (media objects).
- **fcitx5 mode (default):** Hwp uses only the public Qt5 ABI (`Qt_5`/`Qt_5.11`
  symbol versions), so the bundled Qt 5.11 is removed and nixpkgs Qt 5.15 is used.
  This lets the real `fcitx5-qt` Qt5 plugin load (`QT_IM_MODULE=fcitx`). A plugin
  built for Qt 5.15 cannot load into the bundled Qt 5.11.
  QtWebEngine 5 is not in nixpkgs anymore, so the bundled QtWebEngine 5.11 is
  kept (used by `Hmc.so`, the media/web object control). It runs on Qt 5.15
  with a small shim (`compat/qt511-stdcxx-shim.cpp`) that provides three
  libstdc++ symbols the bundled Qt 5.11 re-exported under the `Qt_5` version.
  Its executable-stack flag is cleared because current glibc refuses to
  `dlopen` such libraries.
- **Faster typing (`fastRepaint`, default on):** in 11.20.0.1520,
  `libhsp.so` (Hancom's Win32 emulation layer) fingerprints a window's whole
  back-buffer image with MD5 when a DC is created and again after painting,
  and repaints only if the two differ. On a 1600x1000 window that is ~30 MB
  hashed per keystroke. 11.20.0.989 had no such check. The comparison has to
  stay (it ends the repaint loop; without it Hcell repaints forever), so
  `compat/patch-libhsp-md5.py` replaces only the fingerprint function with a
  fast 64-bit hash (`compat/libhsp-fingerprint.s`). It verifies the original
  bytes first.

  | 222 keys, 1600x1000 window | CPU/key (Korean) | idle CPU |
  | -------------------------- | ---------------- | -------- |
  | 11.20.0.989                | 47 ms            | ~2%      |
  | 11.20.0.1520 unpatched     | 70 ms            | ~15%     |
  | 11.20.0.1520 `fastRepaint` | 49–51 ms         | ~5%      |

  Hwp renders the same as unpatched (screenshots compared; Hcell, Hshow and
  Hword were also checked before they were removed). The remaining ~45 ms per key is Hwp's own work, also in 989.
- **ibus mode:** keeps the bundled Qt 5.11. Plugins that need Qt modules not in
  the bundle (Wayland, eglfs, webgl, virtual keyboard, MultimediaGstTools) are
  removed.
- The wrapper forces the X11 backend (`QT_QPA_PLATFORM=xcb`) and isolates
  `QT_PLUGIN_PATH`. It sets a full `glibcLocales` archive (`LOCALE_ARCHIVE_2_27`)
  so `ko_KR.UTF-8` works, which keeps Korean file names and the Korean UI.

If typing feels slow, also check that `~/.hnc`, `~/.cache/hnc` and
`~/.config/hnc` belong to you (an earlier Docker setup may have created them
as root). If Hwp can't write its font cache there, Korean input gets about
twice as slow.

Note: the wrapper shell may print `setlocale: LC_ALL: cannot change locale`.
This message comes from bash itself and is harmless; hwp gets the right locale.
