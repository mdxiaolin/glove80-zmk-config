{ pkgs ? import <nixpkgs> {}
, firmware ? import ../src {}
}:

let
  config = ./.;
  sat_module = "${../zmk-extra/sat-window-cycle}";

  glove80_left = firmware.zmk.override {
    board = "glove80_lh";
    keymap = "${config}/glove80.keymap";
    kconfig = "${config}/glove80.conf";
    extraModules = [ sat_module ];
  };

  glove80_right = firmware.zmk.override {
    board = "glove80_rh";
    keymap = "${config}/glove80.keymap";
    kconfig = "${config}/glove80.conf";
    extraModules = [ sat_module ];
  };
in firmware.combine_uf2 glove80_left glove80_right
