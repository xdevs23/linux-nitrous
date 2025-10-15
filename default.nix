{
  pkgs,
  lib,
  config,
  ...
}:

{
  options = {
    linux-nitrous = {
      processorFamily = lib.mkOption {
        type = lib.types.nullOr (
          lib.types.enum [
            # Intel
            "nocona"
            "core2"
            "penryn"
            "bonnell"
            "atom"
            "silvermont"
            "slm"
            "goldmont"
            "goldmont-plus"
            "tremont"
            "nehalem"
            "corei7"
            "westmere"
            "sandybridge"
            "corei7-avx"
            "ivybridge"
            "core-avx-i"
            "haswell"
            "core-avx2"
            "broadwell"
            "skylake"
            "skylake-avx512"
            "skx"
            "cascadelake"
            "cooperlake"
            "cannonlake"
            "icelake-client"
            "rocketlake"
            "icelake-server"
            "tigerlake"
            "sapphirerapids"
            "alderlake"
            "raptorlake"
            "meteorlake"
            "arrowlake"
            "arrowlake-s"
            "lunarlake"
            "gracemont"
            "pantherlake"
            "sierraforest"
            "grandridge"
            "graniterapids"
            "graniterapids-d"
            "emeraldrapids"
            "clearwaterforest"
            "diamondrapids"
            # AMD
            "knl"
            "knm"
            "k8"
            "athlon64"
            "athlon-fx"
            "opteron"
            "k8-sse3"
            "athlon64-sse3"
            "opteron-sse3"
            "amdfam10"
            "barcelona"
            "btver1"
            "btver2"
            "bdver1"
            "bdver2"
            "bdver3"
            "bdver4"
            "znver1"
            "znver2"
            "znver3"
            "znver4"
            "znver5"
            # Generic
            "x86-64"
            "x86-64-v2"
            "x86-64-v3"
            "x86-64-v4"
          ]
        );
        default = null;
      };
      enableDrmXe = lib.mkEnableOption "Intel Xe support";
    };
  };
  config = {
    boot.kernelPackages = lib.mkOverride 80 (
      let
        version = "6.17.3-1";
        linuxVersion = lib.head (lib.splitString "-" version);
        suffix = "nitrous";
        llvm = pkgs.unstable.llvmPackages_20;
        linux_nitrous_pkg =
          { fetchurl, buildLinux, ... }@args:
          buildLinux (
            args
            // rec {
              inherit version;
              pname = "linux-${suffix}";
              modDirVersion = lib.versions.pad 3 "${linuxVersion}-${suffix}";
              stdenv = pkgs.unstable.overrideCC llvm.stdenv (
                llvm.stdenv.cc.override { inherit (llvm) bintools; }
              );
              nativeBuildInputs = [ llvm.lld ];
              extraMakeFlags = [
                "LLVM=1"
                "LD=${llvm.lld}/bin/ld.lld"
                "CC=${lib.getExe llvm.clang-unwrapped}"
              ]
              ++ (
                let
                  processorFamily = config.linux-nitrous.processorFamily;
                in
                if processorFamily != null then
                  [
                    "KCPPFLAGS=-march=${processorFamily}"
                    "KCFLAGS=-march=${processorFamily}"
                    "KRUSTFLAGS=-Ctarget-cpu=${processorFamily}"
                  ]
                else
                  [ ]
              );

              src = fetchurl {
                url = "https://gitlab.com/xdevs23/linux-nitrous/-/archive/v${version}/linux-nitrous-v${version}.tar.gz";
                hash = "sha256-iy1aM2p0gPHO3T/ZtVj76pEToTYBt/3jGe8BFE+9cpI=";
              };

              structuredExtraConfig = with lib.kernel; {
                LOCALVERSION_AUTO = no;
                NTSYNC = yes;
                #LATENCYTOP = no;
                BCACHEFS_FS = module;
                DRM_XE = if config.linux-nitrous.enableDrmXe then module else no;
                #SCHED_CLASS_EXT = lib.mkForce no;
                PREEMPT_VOLUNTARY = lib.mkForce no;
                PREEMPT_LAZY = yes;
                CC_OPTIMIZE_FOR_PERFORMANCE_O3 = yes;
                KVM = yes;
                #LTO_CLANG_THIN = yes;
                USB_FEW_INIT_RETRIES = yes;
                ANDROID_BINDER_IPC = yes;
                ANDROID_BINDERFS = yes;
                FS_SYSCTL_PROTECTED_SYMLINKS = yes;
                FS_SYSCTL_PROTECTED_HARDLINKS = yes;
              };

              kernelPatches = [
                {
                  name = "nitrous-config";
                  patch = null;
                  extraConfig = ''
                    LOCALVERSION -${suffix}
                    FS_SYSCTL_PROTECTED_FIFOS 2
                    FS_SYSCTL_PROTECTED_REGULAR 2
                    KPTR_RESTRICT 1
                  '';
                }
              ];

              extraMeta.branch = lib.versions.majorMinor version;
            }
            // (args.argsOverride or { })
          );
        linux_nitrous = pkgs.callPackage linux_nitrous_pkg { };
      in
      pkgs.recurseIntoAttrs (
        (pkgs.unstable.linuxPackagesFor linux_nitrous).extend (
          lpfinal: lpprev: {
            ryzen-smu = lpprev.ryzen-smu.overrideAttrs (
              oldAttrs:
              let
                newSrc = oldAttrs.src.override {
                  owner = "amkillam";
                  repo = "ryzen_smu";
                  rev = "172c316f53ac8f066afd7cb9e1da517084273368";
                  hash = "sha256-U2UMWY7XgLXOpNgl2OsFBRvZSC4/qLa9rzJxFOpZ830=";
                };
                monitor-cpu = llvm.stdenv.mkDerivation {
                  pname = "monitor-cpu";
                  inherit (oldAttrs) version src;
                  makeFlags = [
                    "-C userspace"
                    "CC=${lib.getExe llvm.clang}"
                  ];
                  installPhase = ''
                    runHook preInstall
                    install userspace/monitor_cpu -Dm755 -t $out/bin
                    runHook postInstall
                  '';
                };
              in
              {
                src = newSrc;
                stdenv = pkgs.unstable.overrideCC llvm.stdenv (
                  llvm.stdenv.cc.override { inherit (llvm) bintools; }
                );
                nativeBuildInputs = (oldAttrs.nativeBuildInputs or [ ]) ++ [ llvm.lld ];
                makeFlags = (oldAttrs.makeFlags or [ ]) ++ [
                  "CC=${lib.getExe llvm.clang-unwrapped}"
                  "CXX=${lib.getExe llvm.clang-unwrapped}"
                  "LD=${llvm.lld}/bin/ld.lld"
                ];
                installPhase = ''
                  runHook preInstall
                  install ryzen_smu.ko -Dm444 -t $out/lib/modules/${lpprev.kernel.modDirVersion}/kernel/drivers/ryzen_smu
                  install ${monitor-cpu}/bin/monitor_cpu -Dm755 -t $out/bin
                  runHook postInstall
                '';
              }
            );
          }
        )
      )
    );
  };
}

