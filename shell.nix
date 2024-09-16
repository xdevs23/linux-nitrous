{ pkgs ? import <nixpkgs> {} }:

let fhs = pkgs.buildFHSUserEnv {
  name = "linux-env";
  targetPkgs = pkgs: with pkgs; [
      bc
      bison
      ccache
      clang
      elfutils elfutils.dev
      flex
      git
      gnumake
      libelf
      lld
      llvm
      libgcc
      openssl openssl.dev
      perl
      pkgconf
      python3
      util-linux
  ];
  multiPkgs = pkgs: with pkgs; [
  ];
  runScript = "zsh";
  profile = ''
    export LD_LIBRARY_PATH=/usr/lib:/usr/lib32
  '';
};
in pkgs.stdenv.mkDerivation {
  name = "linux-env-shell";
  nativeBuildInputs = [ fhs ];
  shellHook = "exec linux-env";
}
