{
  lib,
  stdenv,
  fetchurl,
  dpkg,
  autoPatchelfHook,
  makeWrapper,
  patchelfUnstable, # --clear-execstack (patchelf >= 0.18)
  python3,
  # runtime libraries
  alsa-lib,
  atk,
  cairo,
  cups,
  curlWithGnuTls,
  dbus,
  expat,
  fontconfig,
  freetype,
  gdk-pixbuf,
  glib,
  glibcLocales,
  gst_all_1,
  gtk3,
  harfbuzzFull,
  libdrm,
  libGL,
  libGLU,
  libpng,
  libpulseaudio,
  libxkbcommon,
  nspr,
  nss,
  pango,
  wayland,
  libx11,
  libxcomposite,
  libxcursor,
  libxdamage,
  libxext,
  libxfixes,
  libxi,
  libxrandr,
  libxrender,
  libxtst,
  libxcb,
  zlib,
  libsForQt5,
  # user options
  scaleFactor ? "1", # QT_SCALE_FACTOR, e.g. "1.2" to zoom
  # "fcitx5": replace the bundled Qt 5.11 with nixpkgs Qt 5.15 + fcitx5-qt plugin.
  # "ibus"  : keep the bundled Qt 5.11, which only ships an ibus input plugin.
  inputMethod ? "fcitx5",
  hwpLocale ? "ko_KR.UTF-8", # fixes Korean file names / Korean UI
  # Binary-patch libhsp.so to fingerprint the window image with a fast hash
  # instead of MD5 on every repaint (the MD5 is most of the CPU time while
  # typing; see compat/patch-libhsp-md5.py). false = keep libhsp.so unchanged.
  fastRepaint ? true,
}:

assert lib.assertOneOf "inputMethod" inputMethod [
  "fcitx5"
  "ibus"
];

let
  appDir = "opt/hnc/hoffice11";
  useSystemQt = inputMethod == "fcitx5";

  # Only Hwp is installed. Files used solely by Hcell (hcl), Hshow (hsl) and
  # Hword (hword): no Hwp binary links them or names them (NEEDED + strings
  # checked), and a traced Hwp session never opens them.
  otherApps = [
    "hcl"
    "hsl"
    "hword"
  ];
  otherAppFiles = [
    "Bin/hcl"
    "Bin/hsl"
    "Bin/hword"
    "Bin/HCell"
    "Bin/HShow"
    "Bin/Hword"
    "Bin/Resource/HCell"
    "Bin/Resource/HShow"
    "Bin/Resource/Hword"
    "Bin/Strings/hcell_*"
    "Bin/Strings/hshow_*"
    "Bin/Strings/hword_*"
    "Bin/Shared/DocTheme"
    "Bin/Shared/HwordFont"
    "Bin/HCellABase.so"
    "Bin/libHCellBase.so"
    "Bin/libHCellBook.so"
    "Bin/libHCellCoreEngine.so"
    "Bin/libHCellDrawObj.so"
    "Bin/libhclunix-qt.so"
    "Bin/libNxImgManDLL.so"
    "Bin/libHwordApp.so" # (needed OpenSSL 1.1)
    "Bin/HwordAction.so"
    "Bin/libShowAnimationRes.so"
    "Bin/HncTransitionGL.so"
    "Bin/libMathEditorNative.so" # Hcell/Hshow equations (needed ICU 63)
    "Shared/CellTemplate"
    "Shared/HwordTemplate"
  ];

  # nixpkgs Qt 5.15 modules replacing the bundled Qt 5.11 ones (fcitx5 mode).
  systemQt = with libsForQt5; [
    qtbase
    qtsvg
    qtmultimedia
    qtdeclarative # Qml, Quick, QuickWidgets
    qtwebchannel
    qtlocation # Positioning
  ];
  qtPluginPkgs = systemQt ++ [
    libsForQt5.qtimageformats
    libsForQt5.fcitx5-qt
  ];
  systemQtPluginPath = lib.concatMapStringsSep ":" (
    p: "${lib.getBin p}/${libsForQt5.qtbase.qtPluginPrefix}"
  ) qtPluginPkgs;

  gstPlugins = with gst_all_1; [
    gst-plugins-base
    gst-plugins-good
    gst-libav
  ];

  # Values passed to QT_IM_MODULE / XMODIFIERS
  imName = if useSystemQt then "fcitx" else "ibus";
in
stdenv.mkDerivation {
  pname = "hancom-hwp";
  version = "11.20.0.1520";

  src = fetchurl {
    name = "hoffice_11.20.0.1520_amd64.deb";
    url = "https://www.bada-ie.com/board/download/?code=all&uid_code=6333";
    # The mirror only serves the file when the request comes from its board page.
    curlOptsList = [
      "-H"
      "Referer: https://www.bada-ie.com/board/view/?code=all&uid=4366"
    ];
    hash = "sha256-HssvgukVtJcG0fX20gb4vUqThP2ivVZ5jJQEaGX+VzA=";
  };

  nativeBuildInputs = [
    dpkg
    autoPatchelfHook
    makeWrapper
    patchelfUnstable
  ]
  ++ lib.optional fastRepaint python3;

  buildInputs = [
    alsa-lib
    atk
    cairo
    cups
    curlWithGnuTls
    dbus
    expat
    fontconfig
    freetype
    gdk-pixbuf
    glib
    gtk3
    harfbuzzFull
    libdrm
    libGL
    libGLU
    libpng
    libpulseaudio
    libxkbcommon
    nspr
    nss
    pango
    stdenv.cc.cc.lib
    wayland
    zlib
    libx11
    libxcomposite
    libxcursor
    libxdamage
    libxext
    libxfixes
    libxi
    libxrandr
    libxrender
    libxtst
    libxcb
  ]
  ++ (with gst_all_1; [
    gstreamer
    gst-plugins-base
  ])
  # Hwp only uses public Qt5 API (symbol versions Qt_5 / Qt_5.11), so they
  # run fine on the newer, ABI-compatible Qt 5.15 from nixpkgs.
  ++ lib.optionals useSystemQt systemQt;

  # Bundled ICU 56, ffmpeg 3.4, QtWebEngine 5.11 (and all of Qt 5.11 in ibus
  # mode) live in Bin/qt/lib.
  appendRunpaths = [ "${placeholder "out"}/${appDir}/Bin/qt/lib" ];

  unpackPhase = ''
    runHook preUnpack
    dpkg-deb -x $src .
    runHook postUnpack
  '';

  dontConfigure = true;
  dontBuild = !useSystemQt;
  buildPhase = ''
    runHook preBuild
    $CXX -shared -fPIC -O2 -o libqt511-stdcxx-shim.so \
      -Wl,--version-script=${./compat/qt511-stdcxx-shim.map} \
      ${./compat/qt511-stdcxx-shim.cpp}
    runHook postBuild
  '';
  dontStrip = true;
  dontWrapQtApps = true;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/opt $out/bin
    cp -r opt/hnc $out/opt/
    cp -r usr/share $out/share

    # Hwp only: drop Hcell, Hshow and Hword.
    for f in ${lib.concatMapStringsSep " " (f: "$out/${appDir}/${f}") otherAppFiles}; do
      [ -e "$f" ] || { echo "missing: $f"; exit 1; }
      rm -rf "$f"
    done
    for app in ${toString otherApps}; do
      rm $out/share/applications/hoffice11-$app.desktop \
         $out/share/mime/packages/hoffice11-$app.xml
      find $out/share/icons -name "hoffice11-$app*" -delete
    done
  ''
  + lib.optionalString fastRepaint ''
    python3 ${./compat/patch-libhsp-md5.py} $out/${appDir}/Bin/libhsp.so
  ''
  + ''

    qtdir=$out/${appDir}/Bin/qt
  ''
  + (
    if useSystemQt then
      ''
        # Remove the bundled Qt 5.11 libraries and plugins; nixpkgs Qt 5.15 is
        # used. QtWebEngine 5 is no longer in nixpkgs, so the bundled one stays
        # (it needs only two Qt private symbols, both still in Qt 5.15). It is
        # used by Hmc.so (media/web objects in documents).
        find $qtdir/lib -maxdepth 1 -name 'libQt5*' ! -name 'libQt5WebEngine*' -delete
        rm -rf $qtdir/plugins
        # Provides std::__throw_out_of_range_fmt@Qt_5, which QtWebEngine 5.11
        # imports from the (removed) bundled Qt 5.11; see compat/.
        install -m755 libqt511-stdcxx-shim.so $qtdir/lib/
        ${lib.getExe patchelfUnstable} --add-needed libqt511-stdcxx-shim.so \
          $qtdir/lib/libQt5WebEngineCore.so.5.11.3
        plugins="${systemQtPluginPath}"
      ''
    else
      ''
        # Drop Qt plugins whose Qt modules (Wayland, EglFS, WebSockets,
        # VirtualKeyboard, MultimediaGstTools) are not bundled; the apps run on
        # xcb only. The media player plugin (Hancom's vaapi-fixed gsttools) stays.
        rm -rf $qtdir/plugins/wayland-* \
               $qtdir/plugins/platforms/libqwayland-* \
               $qtdir/plugins/platforms/libqeglfs.so \
               $qtdir/plugins/platforms/libqwebgl.so \
               $qtdir/plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so \
               $qtdir/plugins/mediaservice/libgst{audiodecoder,camerabin,mediacapture}.so
        plugins="$qtdir/plugins"
      ''
  )
  + ''
    # QtWebEngineCore is marked as needing an executable stack, which current
    # glibc refuses for dlopen()ed libraries (Hmc.so pulls it in that way).
    ${lib.getExe patchelfUnstable} --clear-execstack $qtdir/lib/libQt5WebEngineCore.so.5.11.3

    # Debian's libcurl-gnutls.so.4 soname (no versioned symbols are required).
    # libXcursor is also on the wrapper's LD_LIBRARY_PATH: the bundled Qt 5.11
    # xcb plugin dlopen()s it.
    mkdir -p $out/${appDir}/Bin/compat
    ln -s ${lib.getLib curlWithGnuTls}/lib/libcurl.so.4 \
      $out/${appDir}/Bin/compat/libcurl-gnutls.so.4

    makeWrapper $out/${appDir}/Bin/hwp $out/bin/hwp \
        --prefix LD_LIBRARY_PATH : "$out/${appDir}/Bin/compat:${lib.getLib libxcursor}/lib" \
        --set QT_PLUGIN_PATH "$plugins" \
        --set QT_QPA_PLATFORM xcb \
        --unset QT_QPA_PLATFORMTHEME \
        --unset QT_STYLE_OVERRIDE \
        --unset QML2_IMPORT_PATH \
        --set QT_IM_MODULE ${imName} \
        --set XMODIFIERS "@im=${imName}" \
        --set GST_PLUGIN_SYSTEM_PATH_1_0 "${
          lib.makeSearchPathOutput "lib" "lib/gstreamer-1.0" gstPlugins
        }" \
        --set-default QT_SCALE_FACTOR ${scaleFactor} \
        --set LOCALE_ARCHIVE_2_27 ${glibcLocales}/lib/locale/locale-archive \
        --set LOCALE_ARCHIVE ${glibcLocales}/lib/locale/locale-archive \
        --set-default QTCOMPOSE ${libx11}/share/X11/locale \
        --set-default LC_ALL ${hwpLocale} \
        --set-default LANG ${hwpLocale}

    substituteInPlace $out/share/applications/hoffice11-hwp.desktop \
      --replace-fail "Exec=/${appDir}/Bin/hwp" "Exec=$out/bin/hwp"

    runHook postInstall
  '';

  meta = {
    description = "Hancom Office Hwp 2022 Beta (Hangul word processor) for Linux";
    homepage = "https://www.hancom.com/";
    license = lib.licenses.unfree;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = [ "x86_64-linux" ];
    mainProgram = "hwp";
  };
}
