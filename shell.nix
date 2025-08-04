{ pkgs ? import <nixpkgs> {}, lib ? pkgs.lib }:

let fhs = let
  llvm = pkgs.llvmPackages_20;
  stdenv = pkgs.overrideCC llvm.stdenv (llvm.stdenv.cc.override { inherit (llvm) bintools; });
  in pkgs.buildFHSEnv {
  name = "linux-env";
  targetPkgs = pkgs: with pkgs; [
      bc
      bison
      ccache
      llvmPackages_20.clang
      elfutils elfutils.dev
      flex
      git
      gnumake
      libelf
      lld_20
      llvm_20
      libgcc
      ncurses ncurses.dev
      openssl openssl.dev
      perl
      pkgconf
      python3
      util-linux
      stdenv
  ];
  multiPkgs = pkgs: with pkgs; [
  ];
  runScript = "zsh";
  profile = ''
    export LD_LIBRARY_PATH=/usr/lib:/usr/lib32
    export LLVM=1
    export LD=${llvm.lld}/bin/ld.lld
    export CC=${lib.getExe llvm.clang-unwrapped}
  '';
};
in pkgs.stdenv.mkDerivation {
  name = "linux-env-shell";
  nativeBuildInputs = [ fhs ];
  shellHook = "exec linux-env";
}
