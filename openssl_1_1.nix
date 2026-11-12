# Minimal OpenSSL 1.1.1 (EOL, removed from nixpkgs) for Hancom Hword.
# libHwordApp.so links libssl.so.1.1/libcrypto.so.1.1 and only uses local
# crypto primitives (EVP AES, SHA1/SHA512) for document encryption; no TLS.
# It is a private dependency of hancom-office and is not exported.
{
  lib,
  stdenv,
  fetchurl,
  perl,
}:

stdenv.mkDerivation rec {
  pname = "openssl";
  version = "1.1.1w";

  src = fetchurl {
    url = "https://www.openssl.org/source/openssl-${version}.tar.gz";
    hash = "sha256-zzCYlQy02FOtlcCEHx+cbT3BAtzPys1SHZOSUgi3asg=";
  };

  nativeBuildInputs = [ perl ];

  postPatch = ''
    patchShebangs Configure config
  '';

  configurePhase = ''
    runHook preConfigure
    ./Configure linux-x86_64 shared no-tests \
      --prefix=$out --libdir=lib --openssldir=/etc/ssl
    runHook postConfigure
  '';

  enableParallelBuilding = true;

  installTargets = [ "install_sw" ];

  postInstall = ''
    rm -rf $out/bin $out/include $out/lib/*.a $out/lib/pkgconfig
  '';

  meta = {
    description = "OpenSSL 1.1.1 runtime libraries (private to hancom-office)";
    homepage = "https://www.openssl.org/";
    license = lib.licenses.openssl;
    platforms = lib.platforms.linux;
  };
}
