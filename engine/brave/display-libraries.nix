{ pkgs }:
let
  lib = pkgs.lib;
in {
  gtk = assert lib.assertMsg (pkgs.gtk4.version == "4.22.4") "Review Gnosis display-state patch for new GTK"; pkgs.gtk4.overrideAttrs (old: {
    patches = (old.patches or []) ++ [
      ../../patches/gtk4-x11-ignore-stale-selection-clear.patch
      ../../patches/gtk4-wayland-follow-fastest-intersecting-output.patch
      ../../patches/gtk4-gnosis-display-state.patch
    ];
  });
  wlroots = assert lib.assertMsg (pkgs.wlroots_0_19.version == "0.19.3") "Review Gnosis output-color patch for new wlroots"; pkgs.wlroots_0_19.overrideAttrs (old: {
    patches = (old.patches or []) ++ [ ../../patches/wlroots-gnosis-output-color.patch ];
  });
}
