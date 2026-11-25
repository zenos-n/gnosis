{ pkgs }:
let displayLibraries = import ./display-libraries.nix { inherit pkgs; };
in pkgs.stdenv.mkDerivation {
  pname = "gnosis-compositor-probe";
  version = "0.0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.pkg-config pkgs.wrapGAppsHook4 pkgs.wayland-scanner ];
  buildInputs = [ displayLibraries.gtk pkgs.libadwaita pkgs.json-glib pkgs.libxml2 displayLibraries.wlroots pkgs.wayland pkgs.wayland-protocols pkgs.libxkbcommon pkgs.libdrm pkgs.pixman pkgs.libglvnd ];
  dontConfigure = true;
  buildPhase = ''
    export PKG_CONFIG_PATH="${displayLibraries.gtk.dev}/lib/pkgconfig:''${PKG_CONFIG_PATH-}"
    test "$(pkg-config --variable=libdir gtk4)" = "${pkgs.lib.getLib displayLibraries.gtk}/lib"
    wayland-scanner server-header ${pkgs.wayland-protocols}/share/wayland-protocols/staging/color-management/color-management-v1.xml color-management-v1-protocol.h
    wayland-scanner server-header ${pkgs.wayland-protocols}/share/wayland-protocols/staging/cursor-shape/cursor-shape-v1.xml cursor-shape-v1-protocol.h
    wayland-scanner server-header ${pkgs.wayland-protocols}/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml xdg-shell-protocol.h
    wayland-scanner server-header ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml xdg-decoration-unstable-v1-protocol.h
    wayland-scanner server-header ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml pointer-constraints-unstable-v1-protocol.h
    wayland-scanner client-header ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml pointer-constraints-client.h
    wayland-scanner private-code ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml pointer-constraints-code.c
    wayland-scanner client-header ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml relative-pointer-client.h
    wayland-scanner private-code ${pkgs.wayland-protocols}/share/wayland-protocols/unstable/relative-pointer/relative-pointer-unstable-v1.xml relative-pointer-code.c
    $CC -std=gnu11 -O2 -Wall -Wextra -I. compositor.c wayland-source.c outer-pointer.c clipboard-bridge.c profile.c compositor-probe.c pointer-constraints-code.c relative-pointer-code.c -o gnosis-compositor-probe \
      $(pkg-config --cflags --libs gtk4-wayland libadwaita-1 json-glib-1.0 gio-unix-2.0 libxml-2.0 wlroots-0.19 wayland-server wayland-client xkbcommon libdrm pixman-1)
  '';
  installPhase = ''
    mkdir -p $out/bin
    cp gnosis-compositor-probe $out/bin/
  '';
  preFixup = ''
    gappsWrapperArgs+=(--set GNOSIS_BRAVE_EXECUTABLE ${pkgs.brave}/bin/brave)
  '';
  meta.mainProgram = "gnosis-compositor-probe";
}
