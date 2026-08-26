# Shared toolchain context for every Chrysopolis module: the Microkit SDK,
# the musl-targeted cross suite, the LLVM/clang wrapper the LionsOS reference
# stack builds with, and the zig2nix env. Published to the other perSystem
# modules as the `chryso` module argument (_module.args, the flake-parts
# mechanism for non-derivation values, derivations flow through
# config.packages.* instead).
{ inputs, ... }:
{
  perSystem =
    {
      pkgs,
      system,
      ...
    }:
    let
      # 1. Microkit SDK. The upstream seL4/microkit flake exposes only a
      # dev shell, not the tool or libmicrokit. The
      # prebuilt SDK release (the same mechanism the LionsOS flake uses)
      # ships the statically linked `microkit` tool plus per-board
      # libmicrokit.a, microkit.ld and loader images. The hashes below are
      # the NAR hashes of each release tarball's stripped root, computed
      # directly from the 2.3.0 release rather than copied from LionsOS,
      # which still pins 2.2.0.
      microkitVersion = "2.3.0";
      microkitPlatform =
        {
          aarch64-darwin = "macos-aarch64";
          x86_64-darwin = "macos-x86-64";
          x86_64-linux = "linux-x86-64";
          aarch64-linux = "linux-aarch64";
        }
        .${system} or (throw "Microkit SDK: unsupported system ${system}");

      microkitSdk = pkgs.fetchzip {
        url = "https://github.com/seL4/microkit/releases/download/${microkitVersion}/microkit-sdk-${microkitVersion}-${microkitPlatform}.tar.gz";
        hash =
          {
            aarch64-darwin = "sha256-7WtFeEf/BbcpNLCKtsJDWcypYXHxAAEMElL+p4KsVXc=";
            x86_64-darwin = "sha256-ob07JxvpiG9DqCB5rdEeDlUYPK2X2wqj1MFJjFbge6k=";
            aarch64-linux = "sha256-Chv1S0xxaf2ROQ4OcATLorQ9iaNaiXrCresl+AW0iBI=";
            x86_64-linux = "sha256-95nmS3e2Y0Mu3SC5Yy7lMDabyhEw7vjgwGMJpXAz4Hs=";
          }
          .${system} or (throw "Microkit SDK: no hash for ${system}");
      };

      microkitBoard = "qemu_virt_aarch64";
      microkitConfig = "debug";

      # LLVM/clang toolchain for the LionsOS reference stack (POSIX libc +
      # sDDF drivers). sDDF's own flake builds these with llvmPackages_18
      # and a "clang wrapper": clang locates its bundled freestanding
      # headers (lib/clang/18/include) by walking up from the directory
      # holding the clang binary, but a plain symlinkJoin leaves only a
      # symlink there. Copying the real binary over the symlink restores
      # that search path. We reproduce the wrapper so the driver/libc
      # derivations compile bare-metal exactly as upstream does.
      llvm = pkgs.llvmPackages_18;
      clangComplete = pkgs.symlinkJoin {
        name = "clang-complete";
        paths = llvm.clang-unwrapped.all;
        meta.mainProgram = "clang";
        postBuild = ''
          cp --remove-destination -- ${llvm.clang-unwrapped}/bin/* $out/bin/
        '';
      };
    in
    {
      _module.args.chryso = {
        inherit microkitVersion microkitBoard microkitConfig;
        inherit microkitSdk llvm;
        boardDir = "${microkitSdk}/board/${microkitBoard}/${microkitConfig}";

        # musl-targeted cross suite: musl headers provide the syscall
        # numbers the dispatcher needs today and are the libc the static
        # ERTS build links against later.
        targetPkgs = pkgs.pkgsCross.aarch64-multiplatform-musl;

        # The binaries the sDDF/LionsOS makefiles expect on PATH: clang,
        # ld.lld, llvm-ar/ranlib/objcopy, dtc for the board DTB, perl/which
        # for various .mk rules.
        lionsToolchain = [
          clangComplete
          llvm.lld
          llvm.libllvm
          pkgs.dtc
          pkgs.perl
          pkgs.which
          pkgs.gnumake
        ];

        # One zig2nix env for everything Zig: the root build.zig's package
        # dependencies (BearSSL, see build.zig.zon) and the tools/sdf host
        # tool. deriveLockFile turns a committed build.zig.zon2json-lock into
        # the Nix-fetched package cache `zig build` resolves offline.
        # (Raw `inputs` + system: zig-env is a custom output shape that
        # flake-parts' inputs' transposition does not cover.)
        zigEnv = inputs.zig2nix.zig-env.${system} {
          zig = inputs.zig2nix.packages.${system}."zig-0_15_2";
        };
      };
    };
}
