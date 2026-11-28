{ pkgs, brave ? pkgs.brave }:
let
  lib = pkgs.lib;
  system = "x86_64-linux";
  displayLibraries = import ./engine/brave/display-libraries.nix { inherit pkgs; };
  gnosisGtk = displayLibraries.gtk;
  gnosisWlroots = displayLibraries.wlroots;
  pixbufLoaders = pkgs.runCommand "gnosis-pixbuf-loaders.cache" {} ''
    cat ${pkgs.librsvg}/${pkgs.gdk-pixbuf.binaryDir}/loaders.cache \
      ${pkgs.webp-pixbuf-loader}/${pkgs.gdk-pixbuf.binaryDir}/webp-loaders.cache > "$out"
  '';
  uiLibraries = with pkgs; [ gtk4 libadwaita json-glib libepoxy fontconfig freetype libsodium libsecret libsoup_3 qrencode libdatachannel ];
  chromiumLibraries = with pkgs; [
    stdenv.cc.cc.lib glib nss nspr dbus at-spi2-core systemd
    cups libdrm libxkbcommon libGL libgbm expat alsa-lib pango cairo
    libX11 libXcomposite libXdamage libXext libXfixes libXrandr libxcb
  ];
  buildTools = with pkgs; [ cmake ninja pkg-config sysprof nodejs ];
  rebrandCheck = pkgs.stdenv.mkDerivation {
    pname = "gnosis-rebrand-check";
    version = "1";
    dontUnpack = true;
    nativeBuildInputs = [ pkgs.pkg-config pkgs.dbus ];
    buildInputs = [ pkgs.glib ];
    buildPhase = ''
      $CC -Wall -Wextra ${./engine/tests/rebrand-test.c} ${./lib/gnosis-rebrand.c} -I${./lib} \
        $(pkg-config --cflags --libs gio-2.0) -o rebrand-test
    '';
    doCheck = true;
    checkPhase = "./rebrand-test";
    installPhase = "mkdir -p $out; touch $out/passed";
  };
  cefSdk = pkgs.stdenv.mkDerivation {
    pname = "gnosis-cef-sdk";
    version = "150.0.22";
    src = pkgs.fetchurl {
      url = "https://cef-builds.spotifycdn.com/cef_binary_150.0.22+gc1a097e+chromium-150.0.7871.256_linux64_minimal.tar.bz2";
      hash = "sha256-3Rtcw6xu/mWex+jpsGd7pOQyFqL+/K/Vq8k0CDfjLZY=";
    };
    nativeBuildInputs = [ pkgs.autoPatchelfHook ];
    buildInputs = chromiumLibraries;
    dontConfigure = true;
    dontBuild = true;
    installPhase = ''
      runHook preInstall
      mkdir -p "$out"
      cp -r . "$out/"
      # Chromium's bundled loader does not search NixOS's graphics-driver path.
      rm "$out/Release/libvulkan.so.1"
      ln -s ${pkgs.vulkan-loader}/lib/libvulkan.so.1 "$out/Release/libvulkan.so.1"
      runHook postInstall
    '';
    meta = {
      description = "Pinned stable Chromium Embedded Framework SDK";
      homepage = "https://github.com/chromiumembedded/cef";
      license = lib.licenses.bsd3;
      platforms = [ system ];
    };
  };
  rrweb = pkgs.stdenvNoCC.mkDerivation {
    pname = "gnosis-rrweb";
    version = "2.2.2";
    src = pkgs.fetchurl {
      url = "https://registry.npmjs.org/rrweb/-/rrweb-2.2.2.tgz";
      hash = "sha512-LUvR1mbcTTaNkn5OiR0r2neuH86quaQsmucT07N4Yd7YFdxPtr9qMQ2e//1FkbWKa1L3JGmPIQtgv83VPJe3mQ==";
    };
    dontBuild = true;
    installPhase = "mkdir -p $out; cp dist/rrweb.umd.min.cjs dist/style.min.css $out/; mkdir -p $out/share/licenses/rrweb; cp ${./third-party/rrweb/LICENSE} $out/share/licenses/rrweb/LICENSE";
  };
  ublock = pkgs.stdenvNoCC.mkDerivation rec {
    pname = "ublock-origin";
    version = "1.76.4";
    src = pkgs.fetchurl {
      url = "https://github.com/gorhill/uBlock/releases/download/${version}/uBlock0_${version}.chromium.zip";
      hash = "sha256-Sxsn7YLnNNUIrm0lWTPDNPYvJW6KiuzpJyS4JoKOPV8=";
    };
    nativeBuildInputs = [ pkgs.unzip pkgs.python3 ];
    doCheck = true;
    checkPhase = ''
      python3 - <<'PY'
      import json
      with open('manifest.json') as source:
          manifest = json.load(source)
      assert manifest['name'] == 'uBlock Origin' and manifest['manifest_version'] == 2
      assert manifest['version'] == '${version}'
      # GHSA-cmqw-9r2w-6wv2 plus the subsequent security fixes.
      assert tuple(map(int, manifest['version'].split('.'))) >= (1, 76, 4)
      PY
    '';
    dontConfigure = true;
    dontBuild = true;
    installPhase = "mkdir -p $out; cp -r . $out/";
    meta.license = lib.licenses.gpl3Plus;
  };
  keepassxcBrowser = pkgs.stdenvNoCC.mkDerivation {
    pname = "gnosis-keepassxc-browser";
    version = "1.10.4.1";
    src = pkgs.fetchurl {
      url = "https://github.com/keepassxreboot/keepassxc-browser/releases/download/1.10.4.1/keepassxc-browser_1.10.4.1_chromium.zip";
      hash = "sha256-PM2mEqlR3UeItCTNQa3O2DKSLU5ILqPuA8YBM5D0WEI=";
    };
    nativeBuildInputs = [ pkgs.unzip pkgs.python3 ];
    unpackPhase = "unzip -q $src";
    patchPhase = "python3 ${./engine/tools/prepare-keepassxc.py} . ${./engine/data/keepassxc}";
    nativeCheckInputs = [ pkgs.nodejs ];
    doCheck = true;
    checkPhase = ''
      node ${./engine/tests/keepassxc-observer-test.js} .
      node ${./engine/tests/keepassxc-service-test.js} ${./engine/data/keepassxc/service.js} ${./engine/data/keepassxc/legacy-settings.js}
      node ${./engine/tests/keepassxc-pairing-test.js} ${./engine/data/keepassxc/pairing.js}
    '';
    dontConfigure = true;
    dontBuild = true;
    installPhase = "mkdir -p $out; cp -r . $out/";
    meta.license = lib.licenses.gpl3;
  };
  package = pkgs.epiphany.overrideAttrs (old: {
    pname = "gnosis";
    version = "0.1.0";
    src = lib.cleanSourceWith {
      src = lib.cleanSource ./.;
      filter = path: type: !(builtins.elem (baseNameOf path) [ "GNOSIS-TODO.md" "BRAVE-MIGRATION.md" ]);
    };
    patches = [];
    nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.cmake pkgs.wayland-scanner rebrandCheck ];
    buildInputs = map (input:
      if input == pkgs.gtk4.dev then gnosisGtk.dev
      else if input == pkgs.gtk4 then gnosisGtk
      else input
    ) old.buildInputs ++ [ pkgs.libpwquality pkgs.libepoxy gnosisWlroots pkgs.wayland pkgs.wayland-protocols pkgs.libxkbcommon pkgs.libdrm pkgs.libglvnd pkgs.pixman pkgs.qrencode pkgs.libsodium pkgs.libdatachannel ] ++ chromiumLibraries;
    preConfigure = (old.preConfigure or "") + ''
      export PKG_CONFIG_PATH="${gnosisGtk.dev}/lib/pkgconfig:''${PKG_CONFIG_PATH-}"
      test "$(pkg-config --variable=libdir gtk4)" = "${lib.getLib gnosisGtk}/lib"
      export NIX_LDFLAGS="-rpath ${lib.getLib gnosisGtk}/lib ''${NIX_LDFLAGS-}"
    '';
    mesonFlags = [ "-Dgranite=disabled" "-Dunit_tests=disabled" "-Dman-pages=disabled" "-Dcef_root=${cefSdk}" "-Dcef_runtime=linked" "-Dublock_dir=${ublock}" "-Dkeepassxc_dir=${keepassxcBrowser}" "-Drrweb_dir=${rrweb}" ];
    preFixup = (old.preFixup or "") + ''
      gappsWrapperArgs+=(--prefix PATH : ${lib.makeBinPath [ pkgs.dconf ]})
      gappsWrapperArgs+=(--set GDK_PIXBUF_MODULE_FILE ${pixbufLoaders})
      gappsWrapperArgs+=(--set GNOSIS_ENGINE brave)
      gappsWrapperArgs+=(--set-default GNOSIS_SURFACE wayland)
      gappsWrapperArgs+=(--set-default GNOSIS_BRAVE_EXECUTABLE ${brave}/bin/brave)
      ${lib.optionalString (brave ? stockScrollbars) "gappsWrapperArgs+=(--set-default GNOSIS_BRAVE_STOCK_SCROLLBARS_EXECUTABLE ${brave.stockScrollbars}/bin/brave)"}
      gappsWrapperArgs+=(--set-default GNOSIS_BRAVE_VERSION ${brave.version})
      gappsWrapperArgs+=(--set GNOSIS_NOTIFICATION_ICON "$out/share/icons/hicolor/scalable/apps/com.neg-zero.Gnosis.svg")
      ${lib.optionalString (brave ? nativeInstallShim) "gappsWrapperArgs+=(--set-default GNOSIS_NATIVE_UI_BRIDGE 1)"}
    '';
    passthru = (old.passthru or {}) // { inherit cefSdk ublock gnosisGtk keepassxcBrowser rebrandCheck; };
    meta = old.meta // {
      description = "GNOME Web shell with a composited Brave web-content backend";
      homepage = "https://github.com/doromiert/gnosis";
      mainProgram = "gnosis";
    };
  });
in package
