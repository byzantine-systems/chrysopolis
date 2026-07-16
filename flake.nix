{
  description = "Chrysopolis: Erlang/BEAM (and Gleam) on seL4 Microkit + LionsOS";

  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";

    devenv = {
      url = "github:cachix/devenv";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    flake-parts.url = "github:hercules-ci/flake-parts";

    treefmt-nix.url = "github:numtide/treefmt-nix";

    nix-gleam = {
      url = "github:arnarg/nix-gleam";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    nix2container = {
      url = "github:nlewo/nix2container";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    mk-shell-bin.url = "github:rrbutani/nix-mk-shell-bin";

    # LionsOS, consumed as a flake input. Its flake exposes only a dev
    # shell (no packages output, no liblionos.a), so the file layer vendors
    # the fs protocol definitions from the input's source tree.
    #
    # git+https URLs instead of the github: fetcher because this network
    # blocks api.github.com. Plain git over https to github.com works.
    lionsos = {
      url = "git+https://github.com/au-ts/lionsos";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.zig-overlay.follows = "zig-overlay";
      inputs.sdfgen.follows = "sdfgen";
    };

    zig-overlay = {
      url = "git+https://github.com/mitchellh/zig-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
      inputs.flake-compat.follows = "flake-compat";
    };

    # Hermetic resolution of Zig package trees (the root build.zig's BearSSL
    # dep and tools/sdf's sdfgen dep). zig2nix fetches the committed
    # build.zig.zon2json-lock files into a Nix-managed zig cache so
    # `zig build` runs offline, rather than vendoring each Zig dependency
    # as its own flake input.
    zig2nix = {
      url = "github:Cloudef/zig2nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };

    sdfgen = {
      url = "git+https://github.com/au-ts/microkit_sdf_gen?ref=refs/tags/0.28.1";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.zig-overlay.follows = "zig-overlay";
    };

    # sDDF (seL4 Device Driver Framework), consumed as a flake. The lionsos
    # flake input ships an EMPTY dep/sddf submodule (Nix does not recurse
    # submodules), so the reference-stack libc + drivers have no sDDF to build
    # against. We pin sDDF to the exact gitlink the locked lionsos rev
    # references and let lionsos-src populate dep/sddf from inputs.sddf. The
    # sddf flake exposes only devShells. We use its source tree and mirror its
    # llvm/clang toolchain choice (llvmPackages_18) for the driver builds.
    sddf = {
      url = "git+https://github.com/au-ts/sddf?ref=refs/heads/main&rev=d1f5252ea64edab6087552eb4220e23c019c2fbe";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.zig-overlay.follows = "zig-overlay";
      inputs.sdfgen.follows = "sdfgen";
    };

    # libmicrokitco: the LionsOS cooperative cothread library. Another empty
    # lionsos submodule, pinned to the gitlink the locked lionsos references.
    # Needed by the fat fs_server and by the ERTS threading layer.
    # - ERTS helper threads run as cothreads).
    # - lionsos-src populates dep/libmicrokitco from this input.
    libmicrokitco = {
      url = "git+https://github.com/au-ts/libmicrokitco?ref=refs/heads/main&rev=4bf88ee12c19823ff8c6d3122b6a298a5d8147ea";
      flake = false;
    };

    # au-ts musl fork (the "musl for seL4"), the libc that LionsOS's
    # lib/libc/libc.mk builds and the POSIX layer links ERTS against. Pinned to
    # the exact rev the locked lionsos input references as its dep/musllibc
    # gitlink so libc.mk's configure/build matches upstream. lionsos-src
    # populates dep/musllibc from this input.
    musllibc = {
      url = "git+https://github.com/au-ts/musllibc?ref=sel4&rev=ee77eeceaeabe97b5a1aed454683e9d39ee6c591";
      flake = false;
    };

    systems = {
      url = "git+https://github.com/nix-systems/default";
      flake = false;
    };

    flake-compat = {
      url = "git+https://github.com/edolstra/flake-compat";
      flake = false;
    };
  };

  # The build pipeline lives in modules/, one flake-parts module per
  # concern. Modules share derivations through config.packages.* and
  # non-derivation context (board names, toolchains, the zig2nix env)
  # through the `chryso` module argument defined in toolchain.nix:
  #
  #   toolchain.nix  Microkit SDK, cross/LLVM toolchains, zig2nix env
  #   lionsos.nix    LionsOS source reconstruction (+ libc patch), musl libc.a
  #   erts.nix       the cross-compiled ERTS emulator archive (liberts.a)
  #   beam.nix       root build.zig artifacts, the Gleam app, romfs
  #   images.nix     gen-sdf, the FAT disk, the bootable seL4 images
  #   checks.nix     the QEMU integration tests (tests.nix)
  #   devshell.nix   treefmt + the devenv shell (run-sel4)
  outputs =
    inputs@{
      flake-parts,
      nixpkgs,
      ...
    }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [
        inputs.devenv.flakeModule
        inputs.treefmt-nix.flakeModule
        ./modules/toolchain.nix
        ./modules/lionsos.nix
        ./modules/erts.nix
        ./modules/beam.nix
        ./modules/images.nix
        ./modules/checks.nix
        ./modules/devshell.nix
      ];
      systems = nixpkgs.lib.systems.flakeExposed;
    };
}
