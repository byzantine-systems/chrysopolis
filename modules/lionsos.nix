# The LionsOS source reconstruction and reference stack:
#   packages.lionsos-src
#     - The LionsOS tree with its (otherwise empty) git
#       submodules populated from pinned flake inputs, plus
#       the Chrysopolis libc patch.
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
    let
      # RNG patch (issue 0.3.0-rng): libc_define_syscall asserts the slot is
      # NULL, so beam_server cannot re-register the getrandom /
      # clock_gettime / openat slots libc_init claims at startup.
      # libc_redefine_syscall() REPLACES a claimed slot and returns the old
      # handler so the RNG shims (src/runtime/bringup.c) can chain to
      # upstream. ~10 lines, an upstream candidate alongside the TCP fixes.
      # Appended (not patched by line number) so it survives lionsos bumps.
      libcRedefineC = pkgs.writeText "libc_redefine_syscall.c" ''

        /* Chrysopolis (issue 0.3.0-rng): replace an already-claimed syscall
         * slot and return the previous handler, so a client can layer a new
         * handler over libc's and chain to it. libc_define_syscall asserts
         * the slot is NULL and so cannot do this. Upstream candidate. */
        muslcsys_syscall_t libc_redefine_syscall(int syscall_num, muslcsys_syscall_t syscall_func) {
            assert(syscall_num >= 0 && syscall_num < ARRAY_SIZE(syscall_table));
            muslcsys_syscall_t old = syscall_table[syscall_num];
            syscall_table[syscall_num] = syscall_func;
            return old;
        }
      '';
      libcRedefineH = pkgs.writeText "libc_redefine_syscall.h" ''

        /* Chrysopolis (issue 0.3.0-rng): see posix.c. Replaces a claimed
         * syscall slot, returning the old handler for chaining. */
        muslcsys_syscall_t libc_redefine_syscall(int syscall_num, muslcsys_syscall_t syscall_func);
      '';
    in
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

          cat ${libcRedefineC} >> $out/lib/libc/posix/posix.c
          cat ${libcRedefineH} >> $out/include/lions/posix/posix.h
        '';

        # The LionsOS reference stack: the POSIX libc.a + headers and the
        # board DTB, built under one make (nix/refstack.mk). musl's libc is an
        # autotools build, so it stays in Nix, ERTS links against libc.a.
        # Everything else (libmicrokitco, the sDDF driver/virtualiser PDs, the
        # beam_server glue) is built by the root build.zig (packages.beam-zig).
        lions-stack = pkgs.stdenvNoCC.mkDerivation {
          name = "lions-stack";
          src = ../nix;
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
