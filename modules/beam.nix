# The BEAM-side artifacts:
#   packages.app
#     - The Gleam application compiled to BEAM bytecode.
#   packages.test-modules
#     - tests/*.erl, the console-driving probes the QEMU checks call, built to
#       BEAM bytecode by rebar3 on the host.
#   packages.beam-zig
#     - Every aarch64 cross artifact from the root build.zig (drivers, libmicrokitco,
#       beam_server/beam_test ELFs), plus the elf/beam-test/sddf-drivers/libmicrokitco
#       aliases.
#   packages.romfs
#     - The app bytecode as a cpio archive.
{ inputs, ... }:
{
  perSystem =
    {
      pkgs,
      inputs',
      config,
      chryso,
      ...
    }:
    let
      # The root build's Zig package cache (BearSSL, see build.zig.zon).
      # Symlinked into ZIG_GLOBAL_CACHE_DIR/p in the buildPhase, exactly what
      # zigEnv.package does internally, beam-zig can't use that canned
      # builder because of its custom buildPhase (the ERTS llvm-ar merge).
      beamZigDeps = chryso.zigEnv.deriveLockFile ../build.zig.zon2json-lock {
        inherit (chryso.zigEnv) zig;
        name = "chrysopolis-dependencies";
      };
    in
    {
      packages = {
        # Compile the Gleam app to platform-independent BEAM bytecode.
        app = inputs'.nix-gleam.packages.buildGleamApplication {
          src = ../.;
        };

        # The console-driving test probes (tests/*.erl), built on the HOST.
        # BEAM bytecode is platform-independent, the same reason the FAT disk
        # carries host kernel/stdlib beams (see modules/images.nix), so these
        # load unchanged on the cross-built aarch64 ERTS. buildRebar3 compiles
        # with pkgs.beamPackages.erlang, which is not merely convenient:
        # modules/erts.nix builds the guest emulator from that same package's
        # version and src, so compiler, runtime and OTP libs move together.
        #
        # This derivation exists for the COMPILE GATE as much as for the
        # bytecode. The guest runs ERTS -mode embedded with no compiler
        # application on the disk, so guest-side compilation is not available
        # and a typo used to surface as a wait_console timeout minutes into a
        # QEMU boot. Here it is a build error in seconds.
        #
        # rebar3 rather than a bare erlc call so that ONE file describes the
        # compile: rebar.config carries the flags (see its header for each) and
        # is also the project model ELP discovers natively in the dev shell, so
        # `rebar3 compile` locally and this build agree by construction. An
        # erlc invocation here would need a second, ELP-specific description of
        # the same thing, and the two would drift.
        #
        # buildRebar3 rather than driving rebar3 by hand: it supplies the
        # standard beam hooks (source copy, `rebar3 bare compile`, and the
        # lib/erlang/lib/<app>-<vsn> install layout every other nixpkgs BEAM
        # package uses) and injects `deterministic` via ERL_COMPILER_OPTIONS.
        # beamDeps is empty because the probes use only kernel and stdlib,
        # which is also why this needs no network in the sandbox.
        test-modules = pkgs.beamPackages.buildRebar3 {
          name = "chryso_test";
          version = "0.1.0";
          src = pkgs.lib.fileset.toSource {
            root = ../.;
            fileset = pkgs.lib.fileset.unions [
              ../rebar.config
              ../tests
            ];
          };
          beamDeps = [ ];
        };

        # Every aarch64 cross artifact, built by the one root build.zig:
        # libmicrokitco.a (the cothread runtime), the sDDF driver/virtualiser
        # PDs, and the beam_server PD glue (main.c + bring-up shims) linked
        # against the LionsOS libc.a + libmicrokit. -Dwith-erts also produces
        # beam_test.elf, the same glue with the static ERTS archive linked in:
        # bin/beam_server.elf boots in bring-up mode (console + clock + heap),
        # bin/beam_test.elf hands off to erl_start.
        beam-zig = pkgs.stdenvNoCC.mkDerivation {
          name = "beam-zig";
          src = pkgs.lib.fileset.toSource {
            root = ../.;
            fileset = pkgs.lib.fileset.unions [
              ../build.zig
              ../build.zig.zon
              ../build.zig.zon2json-lock
              ../src/runtime
            ];
          };

          nativeBuildInputs = chryso.lionsToolchain ++ [
            pkgs.bash
            pkgs.util-linux
            inputs'.zig2nix.packages."zig-0_15_2"
          ];
          hardeningDisable = [ "all" ];
          # The Microkit tool patches setvar_vaddr (beam_heap_start) and
          # objcopy updates the config sections, both via the symbol table.
          dontStrip = true;
          dontFixup = true;

          buildPhase = ''
            runHook preBuild
            # The ERTS archives reference each other and libgcc circularly.
            # The Makefile resolved that with `ld --start-group`, the Zig
            # build graph has no group API, so merge them (plus the cross
            # gcc's libgcc.a, which supplies the outline-atomic / 128-bit
            # helpers liberts.a calls) into a single archive whose members
            # lld resolves against one another within the one archive.
            libgccDir=$(dirname "$(find ${chryso.targetPkgs.stdenv.cc.cc}/lib/gcc \
              -name libgcc.a | head -1)")
            {
              echo "create liberts_all.a"
              for a in liberts liberts_internal liberts_internal_r libethread \
                       libz libzstd libepcre libryu micro-openssl; do
                echo "addlib ${config.packages.liberts}/lib/$a.a"
              done
              echo "addlib $libgccDir/libgcc.a"
              echo "save"
              echo "end"
            } | llvm-ar -M
            llvm-ranlib liberts_all.a

            # Alias libc.a off Zig's special library name "c" (-Dlibc-dir).
            mkdir -p libalias
            ln -sf ${config.packages.lions-stack}/lib/libc.a libalias/liblionsc.a

            # build.zig owns the cross target/flags (one resolveTargetQuery).
            # -Dwith-erts builds beam_test.elf too, installArtifact emits
            # $out/{lib,bin}.
            mkdir -p $out
            export ZIG_GLOBAL_CACHE_DIR="$TMPDIR/zig-cache"
            # Zig package deps (BearSSL) from the committed lock, so `zig
            # build` resolves build.zig.zon offline (see beamZigDeps above).
            mkdir -p "$ZIG_GLOBAL_CACHE_DIR"
            ln -s ${beamZigDeps} "$ZIG_GLOBAL_CACHE_DIR"/p
            zig build --prefix $out \
              -Dboard-dir=${chryso.boardDir} \
              -Dboard=${chryso.microkitBoard} \
              -Dsddf=${config.packages.lionsos-src}/dep/sddf \
              -Dlibmicrokitco-src=${inputs.libmicrokitco} \
              -Dlions-libc=${config.packages.lions-stack} \
              -Dlibc-dir="$PWD/libalias" \
              -Dlionsos-src=${config.packages.lionsos-src} \
              -Dwith-erts=true \
              -Dwith-blk=true \
              -Dwith-fs=true \
              -Dwith-net=true \
              -Dwith-crasher=true \
              -Derts-archive-dir="$PWD"

            # Headers a downstream consumer of libmicrokitco.a would need, the
            # beam link above already includes them straight from the source.
            mkdir -p $out/include
            cp ${inputs.libmicrokitco}/libmicrokitco.h $out/include/
            cp -r ${inputs.libmicrokitco}/libhostedqueue $out/include/
            runHook postBuild
          '';

          dontInstall = true;
        };

        # Application bytecode packaged as a flat cpio archive for the
        # simulated virtual flash. Mapping it into the romfs memory
        # region happens when we need to load the BEAM bytecode.
        romfs = pkgs.stdenvNoCC.mkDerivation {
          name = "beam-romfs";
          nativeBuildInputs = [ pkgs.cpio ];
          dontUnpack = true;
          buildCommand = ''
            mkdir -p $out romfs/boot romfs/lib romfs/releases

            find ${config.packages.app} -type d -name ebin | while IFS= read -r dir; do
              app=$(basename "$(dirname "$dir")")
              mkdir -p "romfs/lib/$app/ebin"
              cp "$dir"/* "romfs/lib/$app/ebin/"
            done

            (cd romfs && find . | cpio -o -H odc) > $out/romfs.cpio
          '';
        };

        # Aliases kept so existing `nix build .#<attr>` invocations resolve.
        elf = config.packages.beam-zig;
        beam-test = config.packages.beam-zig;
        sddf-drivers = config.packages.beam-zig;
        libmicrokitco = config.packages.beam-zig;
      };
    };
}
