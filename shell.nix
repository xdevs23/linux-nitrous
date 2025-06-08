{ pkgs ? import <nixpkgs> {} }:

let fhs = pkgs.buildFHSEnv {
  name = "linux-env";
  targetPkgs = pkgs: with pkgs; [
      bc
      bison
      ccache
      llvmPackages_20.clang-unwrapped
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
