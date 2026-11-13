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
  fastScroll  = true;         # false = keep Hwp's own mouse-wheel scrolling
  hidpiUi     = true;         # false = no high-DPI toolbar / menu fixes
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
- **Fast, partial repaints (`fastRepaint`, default on):** `libhsp.so`
  (Hancom's Win32 emulation layer) keeps a back-buffer image per window. In
  11.20.0.1520 it MD5-hashes the *whole* image when a DC is created and again
  when it is released, and if the hashes differ calls `QWidget::update()`,
  which repaints the *whole* window (blend, clear and X11 upload of ~15 MB on
  a 2540x1500 window). Both happen several times per keystroke. The
  "unchanged -> no update" check must stay because it ends libhsp's repaint
  loop.

  `compat/patch-libhsp.py` makes the fingerprint function tail-call
  `hwp_fast_fingerprint` in `compat/libhwp-fastpatch.cpp`, built into
  `Bin/libhwp-fastpatch.so`. To do that it reuses the function's private
  `QImage::sizeInBytes()` import (renamed with `patchelf`) and adds the
  library as NEEDED. It verifies the original bytes first. The new code keeps
  a copy of the window image plus a hash per row. On DC release it hashes the
  rows (XXH3, up to 4 threads), finds the changed spans, and calls
  `QWidget::update(region)` for the changed rectangles only.

  Key press -> first repaint latency, Korean input via fcitx5, measured with
  XTest + XDamage (median / p90):

  | window                     | 1600x1000     | 2540x1500       |
  | -------------------------- | ------------- | --------------- |
  | 11.20.0.1520 unpatched     | 122 / 189 ms  | 230 / 256 ms    |
  | 11.20.0.989 (old package)  | 29 / 35 ms    | 71 / 80 ms      |
  | **11.20.0.1520 `fastRepaint`** | **15 / 21 ms** | **19 / 28 ms** |

  Idle CPU: ~2.5% (unpatched ~13–15%). The screen was checked against Hwp's
  back buffer after scripted sessions (typing, paging, menus, a dialog, window
  resize, `.hwp` and `.hwpx` files): no pixel differs. Runtime switches:
  `HWP_REPAINT=hash` (whole-window updates, with XXH3 instead of MD5),
  `HWP_FASTPATCH_THREADS=1` (less CPU while typing, ~28 ms instead of ~20 ms
  latency at 2540x1500), `HWP_FASTPATCH_DUMP=DIR` (debug dumps),
  `HWP_FASTPATCH_DEBUG=1` (log scroll/wheel/diff decisions).
- **Faster mouse-wheel scrolling (`fastScroll`, default on):** Hwp scrolled
  slowly, and kept scrolling for seconds after a quick spin (also in
  11.20.0.989), because:
  - its wheel handler scrolls in `SPI_GETWHEELSCROLLLINES` (3) separate steps
    per notch, and every step costs ~100+ ms of Hwp's own UI work (layout,
    style sheets, XML, font and picture look-ups), whatever is repainted;
  - libhsp's `ScrollWindow()` does nothing and its `UpdateWindow()` repaints
    the whole window, so each step also re-renders the whole view;
  - each notch is processed in full before the next one, so notches queue up.

  Fixes: `compat/patch-hwp.py` makes the wheel handler call
  `CHwpView::OnScroll` once with `notches × lines` (same distance, one step).
  With `fastRepaint`, `libhwp-fastpatch.cpp` implements `ScrollWindow()` for the
  document view (moves the back buffer and the screen with
  `QWidget::scroll()`, so only the exposed strip is painted), gives such
  windows Win32 `UpdateWindow()` semantics (paint only the invalid region),
  and merges wheel notches that queued up while Hwp was busy into one step.

  | 2540x1500 window, `.hwp` with pictures | wheel notch: first paint / settled | 20 notches in 0.5 s: done after the last |
  | -------------------------------------- | ---------------------------------- | ---------------------------------------- |
  | before                                 | 83 / 535 ms                        | 8.1 s                                    |
  | `fastScroll`                           | 84 / 188 ms                        | 0.25 s                                   |

  Distance per notch is unchanged (screenshots after 4 notches are identical;
  merged notches can differ by 1 px of rounding). The screen matched Hwp's
  back buffer after wheel, PageDown, arrow-key, scroll-bar and typing steps
  (both variants). `HWP_FASTSCROLL=0` turns the library part off at runtime.
- **High-DPI UI (`hidpiUi`, default on, needs `fastRepaint`):** Hwp sizes UI
  text by the screen DPI (`Xft.dpi`), but its formatting toolbar (새 문서,
  불러오기, 저장하기, ..., 글자 크기) uses fixed 16 px icons and box widths. On a
  2560x1600 screen at 172 DPI the icons were tiny next to the text, the font
  size box drew "pt" over the number, and the recent files in the 파일 menu
  were cut to half height (the list reports its height only after the menu
  was laid out).
  - `compat/patch-ui.py` points the toolbar at Hancom's own 32 px icons (the
    ribbon's `lg_*.png`) where they exist.
  - The library sets the toolbar icon size to 16 × round(DPI / 96) (32 px at
    144–191 DPI, unchanged at 96 DPI; `HWP_TOOLBAR_ICON_SIZE=N` overrides).
    Icons without a large version (new, open, save, undo, redo, text colour)
    are enlarged with Scale2x, a pixel-art scaler that keeps edges sharp
    instead of blurring them like `QT_SCALE_FACTOR` does. Fixed-width toolbar
    boxes are widened to fit their text.
  - It re-sizes the recent-file row of the 파일 menu once the list knows its
    height.

  `HWP_UIFIX=0` turns the runtime part off.
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
