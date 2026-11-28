{
  description = "Gnosis — Epiphany with a Chromium engine";
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  outputs = { self, nixpkgs }: let
    system = "x86_64-linux";
    pkgs = import nixpkgs { inherit system; config.allowUnfree = true; };
    nativeBrave = import ./engine/brave/native-ui.nix { inherit pkgs; };
    package = import ./package.nix { inherit pkgs; brave = nativeBrave; };
    compositorProbe = import ./engine/brave/compositor-probe.nix { inherit pkgs; };
    braveSeries = builtins.fromJSON (builtins.readFile ./engine/brave/patches/series.json);
    bravePatchChecker = pkgs.writeShellApplication {
      name = "check-brave-patches";
      runtimeInputs = [ pkgs.python3 pkgs.patch pkgs.cacert ];
      text = ''
        export SSL_CERT_FILE=${pkgs.cacert}/etc/ssl/certs/ca-bundle.crt
        if [ "$#" -eq 0 ]; then set -- --fetch; fi
        exec python3 ${./engine/tools/check-brave-patches.py} --patches ${./engine/brave/patches} "$@"
      '';
    };
    bravePatchInputs = pkgs.linkFarm "gnosis-brave-patch-inputs" ([ {
      name = "brave/package.json";
      path = pkgs.fetchurl {
        url = "https://raw.githubusercontent.com/brave/brave-core/v${braveSeries.brave_version}/package.json";
        sha256 = braveSeries.metadata_sha256;
      };
    } ] ++ pkgs.lib.mapAttrsToList (name: sha256: {
      inherit name;
      path = pkgs.fetchurl {
        url = "https://raw.githubusercontent.com/chromium/chromium/${braveSeries.chromium_version}/${name}";
        inherit sha256;
      };
    }) braveSeries.sources);
  in {
    packages.${system} = { default = package; gnosis = package; compositor-probe = compositorProbe; cef-sdk = package.cefSdk; brave-native-ui = nativeBrave; };
    apps.${system} = {
      default = { type = "app"; program = "${package}/bin/gnosis"; };
      mcp = { type = "app"; program = "${package}/bin/gnosis-mcp"; };
      compositor-probe = { type = "app"; program = "${compositorProbe}/bin/gnosis-compositor-probe"; };
      check-brave-patches = { type = "app"; program = "${bravePatchChecker}/bin/check-brave-patches"; };
    };
    checks.${system} = {
      rebrand = package.rebrandCheck;
      brave-patches = assert pkgs.lib.assertMsg
        (pkgs.brave.version == braveSeries.brave_version)
        "Brave version drift: run check-brave-patches --fetch --brave-version <new-version> --upgrade and review the pin.";
        pkgs.runCommand "gnosis-brave-patches" { nativeBuildInputs = [ pkgs.python3 pkgs.patch ]; } ''
          python3 ${./engine/tools/check-brave-patches.py} ${bravePatchInputs} \
            --patches ${./engine/brave/patches} --report "$out"
        '';
    };
    devShells.${system}.default = pkgs.mkShell {
      inputsFrom = [ package ];
      packages = with pkgs; [ cmake ninja meson pkg-config python3 nodejs xorg-server imagemagick gdb xdotool ];
      CEF_ROOT = package.cefSdk;
      UBLOCK_DIR = package.ublock;
      KEEPASSXC_DIR = package.keepassxcBrowser;
      LD_LIBRARY_PATH = pkgs.lib.makeLibraryPath package.cefSdk.buildInputs;
    };
  };
}
