{ pkgs, brave ? pkgs.brave }:
let
  pin = builtins.fromJSON (builtins.readFile ./patches/binary-native-ui.json);
  scrollbarPin = builtins.fromJSON (builtins.readFile ./patches/binary-native-scrollbars.json);
  installPin = builtins.fromJSON (builtins.readFile ./patches/native-install-ui.json);
  config = pkgs.writeText "gnosis-native-install-config.h" (pkgs.lib.concatStringsSep "\n"
    (pkgs.lib.mapAttrsToList (key: value:
      "#define UI_${pkgs.lib.toUpper key} " +
      (if pkgs.lib.hasSuffix "_rva" key then "${value}UL" else builtins.toJSON value)
    ) ((builtins.removeAttrs installPin [ "schema" ]) // {
      native_scrollbar_binary_sha256 = scrollbarPin.output_sha256;
    })));
  debugPatch = id: name:
    let
      patch = pkgs.lib.findFirst (item: item.function == name) (throw "Missing pinned native UI patch: ${name}") pin.patches;
      length = builtins.div (builtins.stringLength patch.replacement) 2;
      bytes = pkgs.lib.concatMapStringsSep "," (index: "0x" + builtins.substring (index * 2) 2 patch.expected_prefix) (pkgs.lib.range 0 (length - 1));
    in "{\"${id}\",${toString patch.virtual_address}UL,{${bytes}},${toString length}}";
  debugConfig = pkgs.writeText "gnosis-native-debug-config.h" (''
    struct DebugOptionalPatch { const char *id; uintptr_t offset; unsigned char original[3]; size_t length; };
    static const struct DebugOptionalPatch debug_optional_patches[] = {
      ${debugPatch "zoom-osd" "ZoomBubbleCoordinator::Show"},
      ${debugPatch "script-dialogs" "BrowserWebContentsDelegate::GetJavaScriptDialogManager"},
      ${debugPatch "infobars" "InfoBarContainerView::CalculatePreferredSize"}
    };
  '');
  nativeInstallShim = pkgs.stdenv.mkDerivation {
    pname = "gnosis-native-install-ui";
    version = brave.version;
    dontUnpack = true;
    nativeBuildInputs = [ pkgs.pkg-config ];
    buildInputs = [ pkgs.glib pkgs.json-glib pkgs.dbus ];
    doCheck = true;
    checkPhase = ''
      $CC -Wall -Wextra ${../tests/brave-portal-parent-test.c} -I${./.} \
        $(pkg-config --cflags --libs glib-2.0 dbus-1) -ldl -o portal-parent-test
      ./portal-parent-test
    '';
    buildPhase = ''
      cat ${config} > gnosis-native-install-config.h
      echo >> gnosis-native-install-config.h
      cat ${debugConfig} >> gnosis-native-install-config.h
      cp ${./native-authenticator-ui.inc} native-authenticator-ui.inc
      cp ${./native-find.inc} native-find.inc
      cp ${./native-external-ui.inc} native-external-ui.inc
      cp ${./native-close.inc} native-close.inc
      $CC -shared -fPIC -O2 -Wall -Wextra ${./native-install-ui.c} ${./native-notifications.c} -I. \
        $(pkg-config --cflags --libs glib-2.0 json-glib-1.0 dbus-1) -ldl -o native-install-ui.so
    '';
    installPhase = "install -Dm755 native-install-ui.so $out/lib/native-install-ui.so";
  };
  stockScrollbars = pkgs.runCommand "gnosis-brave-native-ui-${brave.version}" {
    nativeBuildInputs = [ pkgs.python3 ];
    inherit (brave) version meta;
  } ''
    python3 ${../tools/patch-brave-binary.py} ${brave} "$out" \
      --manifest ${./patches/binary-native-ui.json} \
      --native-install-shim ${nativeInstallShim}/lib/native-install-ui.so
  '';
in
assert pkgs.lib.assertMsg (brave.version == pin.brave_version)
  "Gnosis native UI binary patch: unsupported Brave version; review the binary pin before upgrading.";
assert pkgs.lib.assertMsg (scrollbarPin.input_sha256 == pin.output_sha256)
  "Native scrollbar patch must apply after the pinned native UI hooks.";
pkgs.runCommand "gnosis-brave-native-scrollbars-${brave.version}" {
  nativeBuildInputs = [ pkgs.python3 ];
  inherit (brave) version meta;
  passthru = { inherit nativeInstallShim stockScrollbars; };
} ''
  python3 ${../tools/patch-brave-binary.py} ${stockScrollbars} "$out" \
    --manifest ${./patches/binary-native-scrollbars.json}
''
