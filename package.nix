{
  lib,
  stdenv,
  fetchurl,
  dpkg,
  autoPatchelfHook,
  makeWrapper,
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
  gtk3,
  harfbuzzFull,
  libdrm,
  libGL,
  libGLU,
  libxkbcommon,
  nss,
  pango,
  libx11,
  libxcomposite,
  libxext,
  libxi,
  libxrender,
  libxcb,
  zlib,
  libsForQt5,
  # user options
  scaleFactor ? "1", # QT_SCALE_FACTOR, e.g. "1.2" to zoom
  # "fcitx5": replace the bundled Qt 5.11 with nixpkgs Qt 5.15 + fcitx5-qt plugin.
  # "ibus"  : keep the bundled Qt 5.11, which only ships an ibus input plugin.
  inputMethod ? "fcitx5",
  hwpLocale ? "ko_KR.UTF-8", # fixes Korean file names / Korean UI
}:

assert lib.assertOneOf "inputMethod" inputMethod [
  "fcitx5"
  "ibus"
];

let
  appDir = "opt/hnc/hoffice11";
  useSystemQt = inputMethod == "fcitx5";

  qtPkgs = with libsForQt5; [
    qtbase
    qtsvg
    qtimageformats
    fcitx5-qt
  ];
  systemQtPluginPath = lib.concatMapStringsSep ":" (
    p: "${lib.getBin p}/${libsForQt5.qtbase.qtPluginPrefix}"
  ) qtPkgs;

  # Values passed to QT_IM_MODULE / XMODIFIERS
  imName = if useSystemQt then "fcitx" else "ibus";
in
stdenv.mkDerivation {
  pname = "hancom-hwp";
  version = "11.20.0.989";

  src = fetchurl {
    url = "https://cdn.hancom.com/pds/hnc/DOWN/gooroom/hoffice_hwp_2020_amd64.deb";
    # The Hancom CDN refuses requests without this header.
    curlOptsList = [
      "-H"
      "Referer: https://www.hancom.com/cs_center"
    ];
    hash = "sha256-3CNLTYgTw57kaG8JGVsWDpmP7Nh5jJHX3QVIcj+5ma4=";
  };

  nativeBuildInputs = [
    dpkg
    autoPatchelfHook
    makeWrapper
  ];

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
    libxkbcommon
    nss
    pango
    stdenv.cc.cc.lib
    zlib
    libx11
    libxcomposite
    libxext
    libxi
    libxrender
    libxcb
  ]
  # hwp only uses public Qt5 API (symbol versions Qt_5 / Qt_5.11), so it runs
  # fine on the newer, ABI-compatible Qt 5.15 from nixpkgs.
  ++ lib.optionals useSystemQt (with libsForQt5; [
    qtbase
    qtsvg
  ]);

  # Bundled ICU 56 (and Qt 5.11 in ibus mode) live in Bin/qt/lib.
  appendRunpaths = [ "${placeholder "out"}/${appDir}/Bin/qt/lib" ];

  unpackPhase = ''
    runHook preUnpack
    dpkg-deb -x $src .
    runHook postUnpack
  '';

  dontConfigure = true;
  dontBuild = true;
  dontStrip = true;
  dontWrapQtApps = true;

  installPhase = ''
    runHook preInstall

    mkdir -p $out/opt $out/bin
    cp -r opt/hnc $out/opt/
    cp -r usr/share $out/share

    qtdir=$out/${appDir}/Bin/qt
  ''
  + (
    if useSystemQt then
      ''
        # Remove the bundled Qt 5.11 libraries and plugins; nixpkgs Qt 5.15 is used.
        rm -f  $qtdir/lib/libQt5*
        rm -rf $qtdir/plugins
        plugins="${systemQtPluginPath}"
      ''
    else
      ''
        # Drop Qt plugins whose Qt modules (Wayland, Quick, Qml, EglFS,
        # WebSockets) are not bundled; the app runs on xcb only.
        rm -rf $qtdir/plugins/wayland-* \
               $qtdir/plugins/platforms/libqwayland-* \
               $qtdir/plugins/platforms/libqeglfs.so \
               $qtdir/plugins/platforms/libqwebgl.so \
               $qtdir/plugins/platforminputcontexts/libqtvirtualkeyboardplugin.so
        plugins="$qtdir/plugins"
      ''
  )
  + ''
    # Debian's libcurl-gnutls.so.4 soname (no versioned symbols are required).
    mkdir -p $out/${appDir}/Bin/compat
    ln -s ${lib.getLib curlWithGnuTls}/lib/libcurl.so.4 \
      $out/${appDir}/Bin/compat/libcurl-gnutls.so.4

    makeWrapper $out/${appDir}/Bin/hwp $out/bin/hwp \
      --prefix LD_LIBRARY_PATH : "$out/${appDir}/Bin/compat" \
      --set QT_PLUGIN_PATH "$plugins" \
      --set QT_QPA_PLATFORM xcb \
      --unset QT_QPA_PLATFORMTHEME \
      --unset QT_STYLE_OVERRIDE \
      --unset QML2_IMPORT_PATH \
      --prefix XDG_DATA_DIRS : "${glib.getSchemaDataDirPath gtk3}" \
      --set QT_IM_MODULE ${imName} \
      --set XMODIFIERS "@im=${imName}" \
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
    description = "Hancom Office Hwp 2020 (Hangul word processor) for Linux";
    homepage = "https://www.hancom.com/";
    license = lib.licenses.unfree;
    sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
    platforms = [ "x86_64-linux" ];
    mainProgram = "hwp";
  };
}
