# The LionsOS source reconstruction and reference stack:
#   packages.lionsos-src
#     - The LionsOS tree with its (otherwise empty) git
#       submodules populated from pinned flake inputs.
#       Chrysopolis's changes to LionsOS and sDDF are commits
#       on the byzantine-systems forks' chryso branches.
#   packages.lions-stack
#     - The POSIX libc.a + headers and the board DTB,
#       built by nix/refstack.mk.
{ inputs, ... }:
{
  perSystem =
    {
      pkgs,
      config,
      chryso,
      ...
    }:
    {
      packages = {
        # Reconstruct the LionsOS source tree with its (otherwise empty) git
        # submodules populated from the pinned flake inputs, the hermetic
        # substitute for `git submodule update --init`. Every reference-stack
        # build (lib/libc/libc.mk, the sDDF driver .mk's) runs against this.
        # Preserve file modes (musl's ./configure must stay executable) but
        # add write permission so the empty submodule dirs can be replaced.
        lionsos-src = pkgs.runCommand "lionsos-src" { } ''
          cp -r ${inputs.lionsos} $out
          chmod -R u+w $out
          rm -rf $out/dep/sddf $out/dep/musllibc $out/dep/libmicrokitco
          cp -r ${inputs.sddf} $out/dep/sddf
          cp -r ${inputs.musllibc} $out/dep/musllibc
          cp -r ${inputs.libmicrokitco} $out/dep/libmicrokitco
          chmod -R u+w $out
        '';

        # The LionsOS reference stack: the POSIX libc.a + headers and the
        # board DTB, built under one make (nix/refstack.mk). musl's libc is an
        # autotools build, so it stays in Nix, ERTS links against libc.a.
        # Everything else (libmicrokitco, the sDDF driver/virtualiser PDs, the
        # beam_server glue) is built by the root build.zig (packages.beam-zig).
        lions-stack = pkgs.stdenvNoCC.mkDerivation {
          name = "lions-stack";
          # refstack.mk reads only the external trees passed below; keeping
          # other nix/ files out stops unrelated edits from rebuilding libc.
          # The name stays "nix": libc embeds its /build/<name> source paths.
          src = builtins.path {
            name = "nix";
            path = ../nix;
            filter = path: _type: baseNameOf path == "refstack.mk";
          };
          nativeBuildInputs = chryso.lionsToolchain;
          hardeningDisable = [ "all" ];
          dontStrip = true;
          dontFixup = true;
          # musl's build copies headers out of $(srcdir) and appends to the
          # copies, so the LionsOS tree must be writable (a read-only store
          # path yields "Permission denied" on bits/syscall.h). Stage a
          # writable copy and point LIONSOS at it.
          buildPhase = ''
            runHook preBuild
            cp -r ${config.packages.lionsos-src} lions
            chmod -R u+w lions
            make -f refstack.mk \
              LIONSOS=$PWD/lions \
              MICROKIT_SDK=${chryso.microkitSdk} \
              MICROKIT_BOARD=${chryso.microkitBoard} \
              MICROKIT_CONFIG=${chryso.microkitConfig}
            runHook postBuild
          '';
          installPhase = ''
            runHook preInstall
            mkdir -p $out/lib
            cp libc/lib/libc.a $out/lib/
            cp -r libc/include $out/include
            # The sDDF drivers and libmicrokitco are built by the root
            # build.zig now, this derivation provides only the musl libc.a +
            # headers and the board DTB.
            cp ${chryso.microkitBoard}.dtb $out/${chryso.microkitBoard}.dtb
            runHook postInstall
          '';
        };
      };
    };
}
