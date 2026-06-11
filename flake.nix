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

    # LionsOS, consumed as a flake input. Its flake exposes only a dev
    # shell (no packages output, no liblionos.a), so the file layer vendors
    # the fs protocol definitions from the input's source tree.
    #
    # git+https URLs instead of the github: fetcher because this network
    # blocks api.github.com; plain git over https to github.com works.
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

    # Hermetic resolution of the Zig package tree for tools/sdf. The sdfgen
    # metaprogram pulls transitive Zig deps (dtb.zig, sddf) via its
    # build.zig.zon; zig2nix fetches that lockfile into a Nix-managed zig
    # cache so `zig build` runs offline, rather than vendoring each Zig
    # dependency as its own flake input.
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
    # references and let lionsosSrc populate dep/sddf from inputs.sddf. The
    # sddf flake exposes only devShells; we use its source tree and mirror its
    # llvm/clang toolchain choice (llvmPackages_18) for the driver builds.
    sddf = {
      url = "git+https://github.com/au-ts/sddf?ref=refs/heads/main&rev=d1f5252ea64edab6087552eb4220e23c019c2fbe";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.zig-overlay.follows = "zig-overlay";
      inputs.sdfgen.follows = "sdfgen";
    };

    # libmicrokitco: the LionsOS cooperative cothread library. Another empty
    # lionsos submodule; pinned to the gitlink the locked lionsos references.
    # Needed by the fat fs_server and by the ERTS threading layer (Step 2c,
    # Option D: ERTS helper threads run as cothreads). lionsosSrc populates
    # dep/libmicrokitco from this input.
    libmicrokitco = {
      url = "git+https://github.com/au-ts/libmicrokitco?ref=refs/heads/main&rev=4bf88ee12c19823ff8c6d3122b6a298a5d8147ea";
      flake = false;
    };

    # au-ts musl fork (the "musl for seL4"), the libc that LionsOS's
    # lib/libc/libc.mk builds and the POSIX layer links ERTS against. Pinned to
    # the exact rev the locked lionsos input references as its dep/musllibc
    # gitlink so libc.mk's configure/build matches upstream. lionsosSrc
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

  outputs =
    inputs@{
      self,
      devenv,
      flake-parts,
      nixpkgs,
      ...
    }:
    flake-parts.lib.mkFlake { inherit inputs; } {
      imports = [
        inputs.devenv.flakeModule
        inputs.treefmt-nix.flakeModule
      ];
      systems = nixpkgs.lib.systems.flakeExposed;

      perSystem =
        {
          config,
          self',
          inputs',
          pkgs,
          system,
          ...
        }:
        let
          # 1. Microkit SDK. The upstream seL4/microkit flake exposes only a
          # dev shell, not the tool or libmicrokit. The
          # prebuilt SDK release (the same mechanism the LionsOS flake uses,
          # hashes copied from there) ships the statically linked `microkit`
          # tool plus per-board libmicrokit.a, microkit.ld and loader images.
          microkitVersion = "2.2.0";
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
                aarch64-darwin = "sha256-UZBEwS3vAQqJe6Xj+13smJRS0RYfoc0uCK7hB8ujbvA=";
                x86_64-darwin = "sha256-aE2mYToK2ne9vzw6d3YQDzJvhpnI8IHOR9+VqZxwlfY=";
                aarch64-linux = "sha256-U1hA7Vk/TlSWgV7KiEeG7AkA7t5IR/x89mSE0YHBRNA=";
                x86_64-linux = "sha256-dxPu2Q01qjKhME6Z6kgG4ASDUe12ytZmh5tCtFva/L0=";
              }
              .${system} or (throw "Microkit SDK: no hash for ${system}");
          };

          microkitBoard = "qemu_virt_aarch64";
          microkitConfig = "debug";
          boardDir = "${microkitSdk}/board/${microkitBoard}/${microkitConfig}";

          # musl-targeted cross suite: musl headers provide the syscall
          # numbers the dispatcher needs today and are the libc the static
          # ERTS build links against later.
          targetPkgs = pkgs.pkgsCross.aarch64-multiplatform-musl;

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

          # Reconstruct the LionsOS source tree with its (otherwise empty) git
          # submodules populated from the pinned flake inputs, the hermetic
          # substitute for `git submodule update --init`. Every reference-stack
          # build (lib/libc/libc.mk, the sDDF driver .mk's) runs against this.
          # Preserve file modes (musl's ./configure must stay executable) but
          # add write permission so the empty submodule dirs can be replaced.
          lionsosSrc = pkgs.runCommand "lionsos-src" { } ''
            cp -r ${inputs.lionsos} $out
            chmod -R u+w $out
            rm -rf $out/dep/sddf $out/dep/musllibc $out/dep/libmicrokitco
            cp -r ${inputs.sddf} $out/dep/sddf
            cp -r ${inputs.musllibc} $out/dep/musllibc
            cp -r ${inputs.libmicrokitco} $out/dep/libmicrokitco
            chmod -R u+w $out
          '';

          # The LionsOS reference stack (Step 1): the POSIX libc.a + headers
          # and the sDDF serial/timer driver + virtualiser PDs, built together
          # under one make (nix/refstack.mk) the way the upstream examples do.
          # ERTS links against libc.a; the driver ELFs become PDs in the SDF.
          lionsStack = pkgs.stdenvNoCC.mkDerivation {
            name = "lions-stack";
            src = ./nix;
            nativeBuildInputs = lionsToolchain;
            hardeningDisable = [ "all" ];
            dontStrip = true;
            dontFixup = true;
            # musl's build copies headers out of $(srcdir) and appends to the
            # copies, so the LionsOS tree must be writable (a read-only store
            # path yields "Permission denied" on bits/syscall.h). Stage a
            # writable copy and point LIONSOS at it.
            buildPhase = ''
              runHook preBuild
              cp -r ${lionsosSrc} lions
              chmod -R u+w lions
              make -f refstack.mk \
                LIONSOS=$PWD/lions \
                MICROKIT_SDK=${microkitSdk} \
                MICROKIT_BOARD=${microkitBoard} \
                MICROKIT_CONFIG=${microkitConfig}
              runHook postBuild
            '';
            installPhase = ''
              runHook preInstall
              mkdir -p $out/lib $out/bin
              cp libc/lib/libc.a $out/lib/
              cp libmicrokitco_beam.a $out/lib/libmicrokitco.a
              cp -r libc/include $out/include
              cp ${lionsosSrc}/dep/libmicrokitco/libmicrokitco.h $out/include/
              cp -r ${lionsosSrc}/dep/libmicrokitco/libhostedqueue $out/include/
              cp timer_driver.elf serial_driver.elf \
                 serial_virt_tx.elf serial_virt_rx.elf \
                 blk_driver.elf blk_virt.elf fat.elf $out/bin/
              cp ${microkitBoard}.dtb $out/${microkitBoard}.dtb
              runHook postInstall
            '';
          };

          # 2. Compile the Gleam app to platform-independent BEAM bytecode.
          gleamApp = inputs'.nix-gleam.packages.buildGleamApplication {
            src = ./.;
          };

          # 3. Cross-compiled ERTS emulator, archived as liberts.a.
          #
          # Only the emulator (erts/emulator/) is built; the OTP standard
          # library is not needed until Phase 5 when BEAM bytecode is loaded
          # from romfs. The source tarball is inherited from pkgs.erlang so
          # its hash is already pinned in flake.lock.
          #
          # The preloaded .beam files (init, erlang, erts_internal, …) ship
          # precompiled in the OTP tarball and are converted to C byte-array
          # tables by Perl scripts during the emulator build; no host erlc
          # is required for this step.
          ertsStaticLib = targetPkgs.stdenv.mkDerivation {
            pname = "liberts";
            inherit (pkgs.erlang) version src;

            nativeBuildInputs = [ pkgs.perl ];
            # Build-host compiler, exported as $CC_FOR_BUILD. Needed to build
            # the YCF code generator (see buildPhase): it runs during the
            # cross build, so it must be a native x86 binary.
            depsBuildBuild = [ targetPkgs.buildPackages.stdenv.cc ];

            hardeningDisable = [ "all" ];
            dontStrip = true;

            configurePhase = ''
                            runHook preConfigure
                            export ERL_TOP=$(pwd)

                            # Supply facts OTP cannot determine by running test programs
                            # when cross-compiling for AArch64 LE musl.
                            cat > xcomp.conf <<'XCONF'
              erl_xcomp_bigendian=no
              erl_xcomp_double_middle_byte_order=no
              erl_xcomp_clock_gettime_cpu_time=yes
              erl_xcomp_after_morecore_hook=no
              erl_xcomp_reliable_fpe=no
              erl_xcomp_poll=yes
              erl_xcomp_epoll=yes
              erl_xcomp_timerfd=yes
              erl_xcomp_kqueue=no
              erl_xcomp_putenv_copy=no
              erl_xcomp_msgend_copy=no
              erl_xcomp_getaddrinfo=yes
              erl_xcomp_getnameinfo=yes
              XCONF

                            # The top-level ./configure reads erl_xcomp_* from the
                            # environment; --xcomp-conf is an otp_build-only flag, so we
                            # export the values ourselves instead of passing the file.
                            set -a
                            . ./xcomp.conf
                            set +a

                            ./configure \
                              --host=${targetPkgs.stdenv.hostPlatform.config} \
                              --build=${pkgs.stdenv.hostPlatform.config} \
                              --without-ssl \
                              --without-termcap \
                              --without-wx \
                              --without-javac \
                              --without-odbc \
                              --without-megaco \
                              --disable-dynamic-loading \
                              --disable-jit \
                              --disable-hipe

                            runHook postConfigure
            '';

            buildPhase = ''
              runHook preBuild

              # YCF (Yielding C Fun) rewrites some emulator .c files into
              # yielding form at build time, so it must run on the build host.
              # The OTP build's cross path resolves it via an escript
              # (utils/find_cross_ycf) that needs a bootstrap erlang we do not
              # have; instead we compile YCF natively here and override
              # YCF_EXECUTABLE_PATH so the emulator make uses our host binary.
              ycfsrc=erts/lib_src/yielding_c_fun
              mkdir -p "$TMPDIR/ycf"
              for c in "$ycfsrc"/ycf_*.c; do
                $CC_FOR_BUILD -I"$ycfsrc" -O2 -c "$c" \
                  -o "$TMPDIR/ycf/$(basename "$c" .c).o"
              done
              $CC_FOR_BUILD -O2 -o "$TMPDIR/ycf/yielding_c_fun" \
                "$TMPDIR/ycf"/*.o

              # TARGET defaults to config.guess (the build triple); for a
              # cross build it must name the host dir configure generated
              # under erts/emulator/.
              make -j$NIX_BUILD_CORES -C erts/emulator \
                TARGET=${targetPkgs.stdenv.hostPlatform.config} \
                YCF_EXECUTABLE_PATH="$TMPDIR/ycf/yielding_c_fun"

              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/lib $out/include/erts

              target=${targetPkgs.stdenv.hostPlatform.config}

              # OTP's own build already archives the emulator core into
              # libbeam.a from $(PRELOAD_OBJ) + $(OBJS): the preloaded BEAM
              # modules plus every emulator object including erl_init.o
              # (which defines erl_start). Crucially it omits erl_main.o, so
              # there is no main() to clash with Microkit's init() entry.
              # That is exactly the liberts.a we want.
              cp bin/$target/libbeam.a $out/lib/liberts.a
              $RANLIB $out/lib/liberts.a

              # Ship the emulator's bundled dependency archives (pcre, ryu,
              # zstd, zlib, micro-openssl, ethread, …) so the Phase 4.2 link
              # against liberts.a can resolve their symbols. Names are unique;
              # -n guards against any incidental basename collision.
              find . -path "*/$target/*" -name '*.a' ! -name 'libbeam.a' \
                -exec cp -n {} $out/lib/ \;

              # Generated + public emulator headers for erl_start()'s
              # declaration and any erl_*.h the translation layer includes.
              [ -d erts/include ] && cp -r erts/include/. $out/include/erts/ || true
              runHook postInstall
            '';
          };

          # 4. The beam_server PD glue (main.c + bring-up shims) linked against
          # the LionsOS libc.a and libmicrokit with the same clang/lld
          # toolchain as the drivers. erl_start is weak, so until liberts.a
          # joins the link the PD boots in bring-up mode (console + clock +
          # heap over the real sDDF drivers).
          beamMicrokitElf = pkgs.stdenvNoCC.mkDerivation {
            name = "beam-server-elf";
            src = ./src/runtime;

            nativeBuildInputs = lionsToolchain;
            hardeningDisable = [ "all" ];
            # The Microkit tool patches setvar_vaddr (beam_heap_start) and
            # objcopy updates the config sections, both via the symbol table.
            dontStrip = true;
            dontFixup = true;

            buildPhase = ''
              runHook preBuild
              make CC=clang LD=ld.lld \
                BOARD_DIR=${boardDir} \
                LIONS_LIBC=${lionsStack} \
                SDDF=${lionsosSrc}/dep/sddf \
                LIONSOS_SRC=${lionsosSrc}
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp beam_server.elf $out/bin/
              runHook postInstall
            '';
          };

          # beam_server with the static ERTS archive linked in. Not part of
          # the default chain; building it surfaces the libc/runtime symbols
          # ERTS's boot path needs (trace-driven Phase 4 development).
          beamTestElf = pkgs.stdenvNoCC.mkDerivation {
            name = "beam-test-elf";
            src = ./src/runtime;

            nativeBuildInputs = lionsToolchain ++ [
              pkgs.bash
              pkgs.util-linux
            ];
            hardeningDisable = [ "all" ];
            dontStrip = true;
            dontFixup = true;

            buildPhase = ''
              runHook preBuild
              # Generate boot_data.c with embedded boot files
              bash ${./tools/gen-boot-data.sh} boot_data.c ${pkgs.erlang}/lib/erlang \
                ${pkgs.erlang}/lib/erlang/releases/28/start_clean.boot

              # Compile boot_data.c
              clang -target aarch64-none-elf -mcpu=cortex-a53 -mstrict-align \
                -ffreestanding -g -O2 -Wall \
                -I${boardDir}/include \
                -I${lionsStack}/include \
                -c boot_data.c -o boot_data.o

              # The cross gcc's libgcc.a supplies the runtime helpers liberts.a
              # references (outline atomics, 128-bit arithmetic, ...).
              libgccDir=$(dirname "$(find ${targetPkgs.stdenv.cc.cc}/lib/gcc \
                -name libgcc.a | head -1)")
              make beam_test.elf CC=clang LD=ld.lld \
                BOARD_DIR=${boardDir} \
                LIONS_LIBC=${lionsStack} \
                SDDF=${lionsosSrc}/dep/sddf \
                LIONSOS_SRC=${lionsosSrc} \
                LIBERTS_DIR=${ertsStaticLib}/lib \
                LIBGCC_DIR="$libgccDir" \
                BOOT_DATA_OBJ=boot_data.o
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              mkdir -p $out/bin
              cp beam_test.elf $out/bin/
              runHook postInstall
            '';
          };

          # 4. Application bytecode packaged as a flat cpio archive for the
          # simulated virtual flash. Mapping it into the romfs
          # memory region happens in Phase 5.
          romfsImage = pkgs.stdenvNoCC.mkDerivation {
            name = "beam-romfs";
            nativeBuildInputs = [ pkgs.cpio ];
            dontUnpack = true;
            buildCommand = ''
              mkdir -p $out romfs/boot romfs/lib romfs/releases

              find ${gleamApp} -type d -name ebin | while IFS= read -r dir; do
                app=$(basename "$(dirname "$dir")")
                mkdir -p "romfs/lib/$app/ebin"
                cp "$dir"/* "romfs/lib/$app/ebin/"
              done

              (cd romfs && find . | cpio -o -H odc) > $out/romfs.cpio
            '';
          };

          # FAT disk image the fat fs_server serves to beam_server. Carries a
          # minimal OTP release (kernel + stdlib + the clean boot script) under
          # the -root layout ERTS expects, plus the device files ERTS opens and
          # the Gleam app's BEAM. Populated with mtools, then wrapped in an MBR
          # partition table (the blk virtualiser reads partition 0). BEAM
          # bytecode is platform-independent, so the host pkgs.erlang .beam
          # files load directly on the cross-built aarch64 ERTS.
          fatDisk =
            let
              otp = "${pkgs.erlang}/lib/erlang";
            in
            pkgs.runCommand "chrysopolis-fat.img"
              {
                nativeBuildInputs = [
                  pkgs.dosfstools
                  pkgs.mtools
                  pkgs.util-linux
                ];
              }
              ''
                otp=${otp}
                rel=$(ls $otp/releases | grep -E '^[0-9]+$' | head -1)

                # Build a populated FAT32 partition image.
                part=part.fat
                truncate -s 96M $part
                mkfs.fat -F 32 -n CHRYSO $part
                export MTOOLS_SKIP_CHECK=1

                mmd -i $part ::/dev ::/bin ::/lib ::/releases "::/releases/$rel"

                for app in $otp/lib/kernel-* $otp/lib/stdlib-*; do
                  base=$(basename "$app")
                  mmd -i $part "::/lib/$base" "::/lib/$base/ebin"
                  mcopy -i $part "$app"/ebin/*.beam "::/lib/$base/ebin/"
                done

                # The Gleam application's compiled modules.
                find ${gleamApp} -type d -name ebin | while IFS= read -r dir; do
                  app=$(basename "$(dirname "$dir")")
                  mmd -i $part "::/lib/$app" "::/lib/$app/ebin" 2>/dev/null || true
                  mcopy -i $part "$dir"/*.beam "::/lib/$app/ebin/" 2>/dev/null || true
                done

                # Boot script: the clean boot (kernel + stdlib) as start.boot.
                cp "$otp/releases/$rel/start_clean.boot" start.boot
                mcopy -i $part start.boot "::/releases/$rel/start.boot"

                # Device files ERTS opens (empty: reads EOF, /dev/null sink).
                : > empty
                mcopy -i $part empty ::/dev/null
                mcopy -i $part empty ::/dev/zero
                mcopy -i $part empty ::/dev/urandom

                # Wrap the FAT image in an MBR table; partition 1 (the blk
                # virt's partition 0) starts at sector 2048 (1 MiB aligned).
                off=2048
                truncate -s 100M $out
                echo "label: dos
                start=$off, type=c" | sfdisk $out
                dd if=$part of=$out bs=512 seek=$off conv=notrunc status=none
              '';

          # 5. Generate the Microkit system description with sdfgen instead
          # of hand-writing it. The generator (tools/sdf) builds the topology
          # with the LionsOS sdfgen Zig library and renders system.sdf, so
          # adding PDs/regions/channels later is a code change, not XML
          # surgery. zig-overlay supplies the toolchain; the locked sdfgen
          # input supplies the library.
          # Build the gen-sdf metaprogram with zig2nix: it reads tools/sdf's
          # build.zig.zon + committed build.zig.zon2json-lock and fetches the
          # sdfgen/dtb/sddf Zig packages through Nix, so `zig build` runs
          # offline. A host tool (we run it at build time to emit the SDF).
          zigSdfTool =
            let
              zigEnv = inputs.zig2nix.zig-env.${system} {
                zig = inputs.zig2nix.packages.${system}."zig-0_15_2";
              };
            in
            zigEnv.package {
              src = ./tools/sdf;
            };

          # Run gen-sdf to emit system.sdf and the per-PD config .data blobs.
          # It parses the board DTB (from lionsStack) and probes the sDDF
          # source tree for driver metadata.
          systemSdf = pkgs.runCommand "chrysopolis-system-sdf" { } ''
            mkdir -p $out
            ${zigSdfTool}/bin/gen-sdf \
              ${lionsStack}/${microkitBoard}.dtb \
              ${lionsosSrc}/dep/sddf \
              $out
          '';

          # 6. Synthesize the final bootable seL4 system image. Gather every
          # PD ELF into the search path, embed the per-PD config blobs the
          # metaprogram emitted into the matching ELF sections (the LionsOS
          # examples' objcopy step), then run the Microkit tool.
          # Synthesize a bootable image from a beam_server ELF: gather every
          # PD ELF into the search path, embed the per-PD config blobs the
          # metaprogram emitted into the matching ELF sections (the LionsOS
          # examples' objcopy step), then run the Microkit tool. beamElf is
          # copied in as beam_server.elf (the name the generated SDF uses), so
          # the same topology serves both the bring-up and ERTS-linked PDs.
          mkSel4Image =
            imgName: beamElf:
            pkgs.stdenvNoCC.mkDerivation {
              name = imgName;
              dontUnpack = true;
              nativeBuildInputs = [ llvm.libllvm ];
              buildCommand = ''
                set -ex  # Exit on error, print commands
                mkdir -p $out build
                cp ${beamElf} build/beam_server.elf
                cp ${lionsStack}/bin/*.elf build/
                chmod -R u+w build

                cfg=${systemSdf}
                oc() { llvm-objcopy --update-section "$1"="$cfg/$2" "build/$3"; }
                oc .device_resources     serial_driver_device_resources.data serial_driver.elf
                oc .serial_driver_config serial_driver_config.data           serial_driver.elf
                oc .serial_virt_tx_config serial_virt_tx.data                serial_virt_tx.elf
                oc .serial_virt_rx_config serial_virt_rx.data                serial_virt_rx.elf
                oc .device_resources     timer_driver_device_resources.data  timer_driver.elf
                oc .serial_client_config serial_client_beam_server.data      beam_server.elf
                oc .timer_client_config  timer_client_beam_server.data       beam_server.elf

                # Block subsystem + FAT fs_server: DISABLED for Option C
                # oc .device_resources     blk_driver_device_resources.data    blk_driver.elf
                # oc .blk_driver_config    blk_driver.data                     blk_driver.elf
                # oc .blk_virt_config      blk_virt.data                       blk_virt.elf
                # oc .blk_client_config    blk_client_fatfs.data               fat.elf
                # oc .fs_server_config     fs_server_fatfs.data                fat.elf
                # beam_server's fs client config (the fs protocol to fatfs).
                # oc .fs_client_config     fs_client_beam_server.data          beam_server.elf

                ${microkitSdk}/bin/microkit $cfg/system.sdf \
                  --search-path build \
                  --board ${microkitBoard} \
                  --config ${microkitConfig} \
                  -o $out/sel4-beam.img \
                  -r $out/report.txt
              '';
            };

          # Bring-up image (console + clock + heap, no ERTS).
          sel4SystemImage = mkSel4Image "sel4-beam-image" "${beamMicrokitElf}/bin/beam_server.elf";

          # ERTS-linked image: the same PD topology with liberts.a linked in,
          # so beam_server's init() hands off to erl_start (Phase 4 boot).
          sel4TestImage = mkSel4Image "sel4-beam-test-image" "${beamTestElf}/bin/beam_test.elf";
        in
        {
          packages = {
            default = sel4SystemImage;
            test-image = sel4TestImage;
            elf = beamMicrokitElf;
            beam-test = beamTestElf;
            app = gleamApp;
            romfs = romfsImage;
            disk = fatDisk;
            liberts = ertsStaticLib;
            # LionsOS reference stack (Plan B): the POSIX libc.a + sDDF
            # serial/timer driver PDs, built together by nix/refstack.mk.
            lions-stack = lionsStack;
            lionsos-src = lionsosSrc;
            sdf = systemSdf;
          };

          # Hermetic QEMU integration tests, run by `nix flake check` and CI.
          # Each check boots an image headless under emulation and asserts on
          # the serial trace, pinning a phase's exit criterion as an automated
          # gate rather than a manual step. (Linux only: aarch64 system
          # emulation under TCG; gated off on Darwin builders.)
          checks = pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
            boot-smoke =
              pkgs.runCommand "boot-smoke"
                {
                  nativeBuildInputs = [ pkgs.qemu ];
                }
                ''
                  echo "Booting ${sel4TestImage.name} under QEMU..."
                  timeout 120 qemu-system-aarch64 \
                    -machine virt,virtualization=on \
                    -cpu cortex-a53 \
                    -m size=2G \
                    -display none -monitor none \
                    -serial file:boot.log \
                    -device loader,file=${sel4TestImage}/sel4-beam.img,addr=0x70000000,cpu-num=0 \
                    || true

                  echo "=== serial log ==="
                  cat boot.log

                  # Exit criteria, asserted as gates.
                  fail=0
                  check() {
                    if grep -qF "$1" boot.log; then
                      echo "PASS: $1"
                    else
                      echo "FAIL (missing): $1"
                      fail=1
                    fi
                  }
                  # Step 1: beam_server boots on the LionsOS reference stack.
                  check "beam_server up on the LionsOS reference stack."
                  check "monotonic clock via sDDF timer:"
                  # Step 2: ERTS (liberts.a) is linked and launched.
                  check "Handing off to ERTS core loop..."

                  [ $fail -eq 0 ] || { echo "boot-smoke: assertions failed"; exit 1; }
                  touch $out
                '';
          };

          # nix fmt + nix flake check (auto-wired by flakeModule)
          treefmt = {
            projectRootFile = "flake.nix";
            programs.clang-format.enable = true;
            programs.erlfmt.enable = true;
            programs.gleam.enable = true;
            programs.nixfmt.enable = true;
            programs.zig.enable = true;
          };

          devenv.shells.default = {
            devenv.root =
              let
                r = builtins.getEnv "PWD";
              in
              if r != "" then r else builtins.toString ./.;

            packages = with pkgs; [
              erlfmt
              erlang-language-platform
              gnumake
              qemu
              cpio
              # cross-gcc for poking at the PD build by hand
              targetPkgs.stdenv.cc
            ];

            env = {
              MICROKIT_SDK = "${microkitSdk}";
              MICROKIT_BOARD = microkitBoard;
              MICROKIT_CONFIG = microkitConfig;
              LIONSOS_SRC = "${inputs.lionsos}";
            };

            languages.c = {
              enable = true;
            };

            languages.erlang = {
              enable = true;
            };

            languages.gleam = {
              enable = true;
            };

            scripts.run-sel4.exec = ''
              set -e
              img="''${1:-result/sel4-beam.img}"
              if [ ! -f "$img" ]; then
                nix build
              fi
              exec qemu-system-aarch64 \
                -machine virt,virtualization=on \
                -cpu cortex-a53 \
                -m size=2G \
                -serial mon:stdio \
                -nographic \
                -device loader,file="$img",addr=0x70000000,cpu-num=0
            '';

            enterShell = ''
              echo "========================================================="
              echo " Chrysopolis: BEAM on seL4 Microkit ${microkitVersion}"
              echo " MICROKIT_SDK=$MICROKIT_SDK"
              echo " Execute 'run-sel4' to boot the synthesized image in QEMU"
              echo "========================================================="
            '';
          };
        };
    };
}
