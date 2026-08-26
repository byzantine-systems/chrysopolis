# System assembly:
#   packages.sdf
#     - The generated Microkit system description + per-PD config
#       blobs (gen-sdf, tools/sdf).
#   packages.disk
#     - The FAT disk image the fs_server serves to ERTS.
#   packages.default
#     - The bring-up image (console + clock + heap, no ERTS).
#   packages.test-image
#     - the ERTS-linked image (erl_start handoff).
{
  perSystem =
    {
      pkgs,
      config,
      chryso,
      ...
    }:
    let
      # Generate the Microkit system description with sdfgen instead
      # of hand-writing it. The generator (tools/sdf) builds the topology
      # with the LionsOS sdfgen Zig library and renders system.sdf, so
      # adding PDs/regions/channels later is a code change, not XML
      # surgery. zig-overlay supplies the toolchain, the locked sdfgen
      # input supplies the library.
      # Built with zig2nix's canned builder: it reads tools/sdf's
      # build.zig.zon + committed build.zig.zon2json-lock and fetches the
      # sdfgen/dtb/sddf Zig packages through Nix, so `zig build` runs
      # offline. A host tool (we run it at build time to emit the SDF).
      zigSdfTool = chryso.zigEnv.package {
        src = ../tools/sdf;
      };

      # Which gen-sdf config blob lands in which ELF section, as data rather
      # than as twenty near-identical shell lines. The objcopy loop below is
      # generated from it, which keeps the mapping readable and greppable and
      # makes an addition one line instead of one line plus a column-aligned
      # edit.
      #
      # Worth knowing when this list changes: sdfgen writes the blob and an
      # sDDF/LionsOS C header declares the struct the PD reads it into. If they
      # disagree, a blob LARGER than its section is a hard objcopy error (that
      # is how the sdfgen 0.35.0 bump surfaced: net_virt_rx grew 3168 -> 27744
      # while the consuming sDDF was still at the old pin), but a smaller or
      # merely reordered one is silent and the PD reads plausible garbage at
      # init. The structural defence is keeping sdfgen, sDDF and LionsOS on
      # versions that pin each other (0.35.0 / 0.7.0 / 0.4.0 do), so bump them
      # as a set rather than individually.
      #
      # `elf` is the name in build/, so "beam_server.elf" is whichever ELF the
      # mkSel4Image caller passed as beamElf (beam_server for the ERTS image,
      # beam_test for bring-up).
      configSections = [
        # Serial and timer: driver device resources, driver/virt configs, and
        # the beam_server client configs.
        {
          section = ".device_resources";
          blob = "serial_driver_device_resources.data";
          elf = "serial_driver.elf";
        }
        {
          section = ".serial_driver_config";
          blob = "serial_driver_config.data";
          elf = "serial_driver.elf";
        }
        {
          section = ".serial_virt_tx_config";
          blob = "serial_virt_tx.data";
          elf = "serial_virt_tx.elf";
        }
        {
          section = ".serial_virt_rx_config";
          blob = "serial_virt_rx.data";
          elf = "serial_virt_rx.elf";
        }
        {
          section = ".device_resources";
          blob = "timer_driver_device_resources.data";
          elf = "timer_driver.elf";
        }
        {
          section = ".serial_client_config";
          blob = "serial_client_beam_server.data";
          elf = "beam_server.elf";
        }
        {
          section = ".timer_client_config";
          blob = "timer_client_beam_server.data";
          elf = "beam_server.elf";
        }

        # Block subsystem: driver device resources + driver/virt configs.
        {
          section = ".device_resources";
          blob = "blk_driver_device_resources.data";
          elf = "blk_driver.elf";
        }
        {
          section = ".blk_driver_config";
          blob = "blk_driver.data";
          elf = "blk_driver.elf";
        }
        {
          section = ".blk_virt_config";
          blob = "blk_virt.data";
          elf = "blk_virt.elf";
        }

        # FAT fs_server: fatfs is the blk client (partition 0) and the fs
        # server, beam_server is the fs client (libc fs path dormant until the
        # memfs cutover). gen-sdf also emits a 21st blob, fat_config.data, which
        # is zero bytes and has no section: the fatfs PD takes its real
        # configuration from the two below plus a compile-time fat_config.h, so
        # the 21-vs-20 count is expected rather than a dropped blob.
        {
          section = ".blk_client_config";
          blob = "blk_client_fatfs.data";
          elf = "fat.elf";
        }
        {
          section = ".fs_server_config";
          blob = "fs_server_fatfs.data";
          elf = "fat.elf";
        }
        {
          section = ".fs_client_config";
          blob = "fs_client_beam_server.data";
          elf = "beam_server.elf";
        }

        # Network subsystem: driver device resources + driver/virt/copy configs.
        {
          section = ".device_resources";
          blob = "eth_driver_device_resources.data";
          elf = "eth_driver.elf";
        }
        {
          section = ".net_driver_config";
          blob = "net_driver.data";
          elf = "eth_driver.elf";
        }
        {
          section = ".net_virt_rx_config";
          blob = "net_virt_rx.data";
          elf = "net_virt_rx.elf";
        }
        {
          section = ".net_virt_tx_config";
          blob = "net_virt_tx.data";
          elf = "net_virt_tx.elf";
        }
        {
          section = ".net_copy_config";
          blob = "net_copy_net_copy.data";
          elf = "net_copy.elf";
        }

        # Socket client: beam_server links the lwIP stack + LionsOS socket
        # backend, so it carries the net client config and the lib_sddf_lwip
        # (pbuf pool) config it reads at sddf_lwip_init.
        {
          section = ".net_client_config";
          blob = "net_client_beam_server.data";
          elf = "beam_server.elf";
        }
        {
          section = ".lib_sddf_lwip_config";
          blob = "lib_sddf_lwip_config_beam_server.data";
          elf = "beam_server.elf";
        }
      ];

      # Synthesize a bootable image from a beam_server ELF: gather every
      # PD ELF into the search path, embed the per-PD config blobs the
      # metaprogram emitted into the matching ELF sections (the LionsOS
      # examples' objcopy step), then run the Microkit tool. beamElf is
      # copied in as beam_server.elf (the name the generated SDF uses), so
      # the same topology serves both the bring-up and ERTS-linked PDs.
      mkSel4Image =
        {
          imgName,
          beamElf,
          # Which generated SDF to synthesise against (default topology, or the
          # --with-crasher variant for the restart test).
          sdf ? config.packages.sdf,
          # Extra PD ELFs to stage into the search path (e.g. crasher.elf).
          # These carry no per-PD config blobs, so they are just copied in.
          extraElfs ? [ ],
          # Enable the test-only /dev/pd-restart trigger by patching the
          # beam_server -> root debug channel ids into beam_server.elf. Only
          # meaningful alongside an sdf generated with --with-restart-debug, and
          # left false everywhere else so production images cannot notify root.
          restartDebug ? false,
        }:
        pkgs.stdenvNoCC.mkDerivation {
          name = imgName;
          dontUnpack = true;
          nativeBuildInputs = [ chryso.llvm.libllvm ];
          buildCommand = ''
            set -ex  # Exit on error, print commands
            mkdir -p $out build
            cp ${beamElf} build/beam_server.elf
            ${pkgs.lib.concatMapStringsSep "\n" (e: "cp ${e} build/") extraElfs}
            # Driver/virtualiser PDs from the root build.zig (beamZig), the
            # serial/timer client PDs (beam_server) come from beamElf above.
            cp ${config.packages.beam-zig}/bin/root.elf \
               ${config.packages.beam-zig}/bin/serial_driver.elf \
               ${config.packages.beam-zig}/bin/timer_driver.elf \
               ${config.packages.beam-zig}/bin/serial_virt_tx.elf \
               ${config.packages.beam-zig}/bin/serial_virt_rx.elf \
               ${config.packages.beam-zig}/bin/blk_driver.elf \
               ${config.packages.beam-zig}/bin/blk_virt.elf \
               ${config.packages.beam-zig}/bin/eth_driver.elf \
               ${config.packages.beam-zig}/bin/net_virt_rx.elf \
               ${config.packages.beam-zig}/bin/net_virt_tx.elf \
               ${config.packages.beam-zig}/bin/net_copy.elf \
               ${config.packages.beam-zig}/bin/fat.elf build/
            chmod -R u+w build

            cfg=${sdf}
            oc() { llvm-objcopy --update-section "$1"="$cfg/$2" "build/$3"; }

            # Restart entry points for the Root PD: the child ELF's e_entry
            # (== _start) is a property of the board's microkit.ld, not a
            # universal constant, so derive it from the linked child ELFs rather
            # than hardcoding it. Assert it is uniform across the restartable
            # children (they share microkit.ld, so it must be), then patch it
            # into root.elf's .restart_config section (root.c reads it there).
            # Every child of root is checked, not just a representative pair:
            # root restarts them all to the same address, so a divergent entry
            # anywhere means a silent restart into garbage. beam_server is
            # included because it is a child of root too, and any extra staged
            # PD ELFs (the crasher) are children as well.
            entry_of() { llvm-readelf -h "build/$1" | sed -n 's/.*Entry point address: *//p'; }
            # First match only: a second line would turn the caller's $(( ))
            # into a syntax error rather than an obvious "ambiguous symbol".
            sym_of() {
              llvm-nm "build/$1" \
                | awk -v s="$2" '$3 == s && !found { print "0x" $1; found = 1 }
                                 END { exit(found ? 0 : 1) }'
            }
            # Emit a value as an 8-byte little-endian blob, appended to $1.
            emit_u64() {
              local out=$1 v=$(( $2 )) i byte
              for i in 0 1 2 3 4 5 6 7; do
                byte=$(( (v >> (i * 8)) & 0xff ))
                printf "\\$(printf '%03o' "$byte")" >> "$out"
              done
            }

            ref_elf=serial_driver.elf
            se=$(entry_of "$ref_elf")
            for child in timer_driver.elf blk_driver.elf eth_driver.elf beam_server.elf \
                         ${pkgs.lib.concatMapStringsSep " " (e: builtins.baseNameOf e) extraElfs}; do
              ce=$(entry_of "$child")
              if [ "$se" != "$ce" ]; then
                echo "restart-entry: child ELF entry points differ" \
                     "($ref_elf=$se $child=$ce); the Root PD assumes a uniform" \
                     "per-board entry" >&2
                exit 1
              fi
            done

            # beam_server is resumed at its own _reset symbol, NOT at e_entry.
            # A Microkit restart re-zeroes no memory, and beam_server carries
            # tens of megabytes of ERTS/libc state that has to be pristine
            # before the emulator can boot again, so _reset restores its
            # writable segment first and only then enters the normal boot (see
            # src/runtime/restart.c). Resolve the symbol rather than hardcoding
            # it: unlike _start it has no fixed address.
            if ! beam_reset=$(sym_of beam_server.elf _reset); then
              echo "restart-entry: beam_server.elf exports no _reset symbol;" \
                   "src/runtime/restart.c must be linked into the beam glue" >&2
              exit 1
            fi

            # Capacity gate for the restart snapshot. restart.c copies
            # [__init_array_start, _bss) into the beam_snapshot region at first
            # boot; the region and the offsets within it are fixed constants
            # shared with tools/sdf/system.zig, so assert HERE that the ELF
            # actually linked still fits. Growing ERTS or libc past the region
            # then fails the build instead of corrupting memory at runtime.
            # Offsets mirror BEAM_DATA_OFF / BEAM_SNAPSHOT_SIZE in restart.c.
            snapshot_size=$((0x80000))
            data_off=$((0x6000))
            data_capacity=$((snapshot_size - data_off))
            # Checked explicitly rather than left to fail inside the $(( ))
            # below: an unresolved symbol yields an empty string there, and
            # "syntax error in expression" says nothing about which linker
            # symbol microkit.ld stopped providing.
            for sym in __init_array_start _bss; do
              sym_of beam_server.elf "$sym" > /dev/null || {
                echo "restart-snapshot: beam_server.elf has no $sym symbol;" \
                     "src/runtime/restart.c reads the writable-segment bounds" \
                     "from the board's microkit.ld" >&2
                exit 1
              }
            done
            seg_start=$(sym_of beam_server.elf __init_array_start)
            bss_start=$(sym_of beam_server.elf _bss)
            data_len=$(( bss_start - seg_start ))
            if [ "$data_len" -gt "$data_capacity" ]; then
              echo "restart-snapshot: beam_server's writable data is $data_len bytes," \
                   "which exceeds the $data_capacity byte snapshot data area;" \
                   "raise BEAM_SNAPSHOT_SIZE in src/runtime/restart.c AND the" \
                   "beam_snapshot region size in tools/sdf/system.zig" >&2
              exit 1
            fi
            echo "restart-snapshot: data=$data_len/$data_capacity bytes"

            : > restart_entry.bin
            emit_u64 restart_entry.bin "$se"          # shared child _start
            emit_u64 restart_entry.bin "$beam_reset"  # beam_server _reset
            llvm-objcopy --update-section .restart_config=restart_entry.bin build/root.elf

            ${pkgs.lib.optionalString restartDebug ''
              # Test-only /dev/pd-restart trigger: patch the beam_server-side
              # channel ids of the beam_server -> root debug-restart channels
              # into beam_server.elf's .pd_restart_config (src/runtime/bringup.c
              # reads them there). One byte per driver class, in the order
              # serial, timer, blk, eth, matching BOTH the pinned ids in
              # tools/sdf/system.zig and the pd_restart_channels[] array.
              #
              # The shim is compiled into every beam_server (production images
              # reuse the same ELF), so this patch is what ENABLES it: without
              # it the array keeps its 0xff "not wired" initialiser and
              # /dev/pd-restart reports ENOENT. That is why the ids live in data
              # rather than behind a build flag.
              printf '\072\073\074\075' > pd_restart_channels.bin  # 58 59 60 61
              llvm-objcopy --update-section .pd_restart_config=pd_restart_channels.bin build/beam_server.elf
            ''}
            # Generated from the configSections list above.
            ${pkgs.lib.concatMapStringsSep "\n            " (
              c: "oc ${c.section} ${c.blob} ${c.elf}"
            ) configSections}

            ${chryso.microkitSdk}/bin/microkit $cfg/system.sdf \
              --search-path build \
              --board ${chryso.microkitBoard} \
              --config ${chryso.microkitConfig} \
              -o $out/sel4-beam.img \
              -r $out/report.txt
          '';
        };
    in
    {
      packages = {
        # Run gen-sdf to emit system.sdf and the per-PD config .data blobs.
        # It parses the board DTB (from lions-stack) and probes the sDDF
        # source tree for driver metadata.
        sdf = pkgs.runCommand "chrysopolis-system-sdf" { } ''
          mkdir -p $out
          ${zigSdfTool}/bin/gen-sdf \
            ${config.packages.lions-stack}/${chryso.microkitBoard}.dtb \
            ${config.packages.lionsos-src}/dep/sddf \
            $out
        '';

        # Same topology plus the test-only restart scaffolding:
        #   --with-crasher       the faulting child of root (fault DETECTION),
        #   --with-restart-debug the beam_server -> root channels that let a test
        #                        restart a HEALTHY driver (fault RECOVERY).
        # Both live in this one variant so the restart tests need a single extra
        # image rather than one per concern.
        sdf-restart = pkgs.runCommand "chrysopolis-system-sdf-restart" { } ''
          mkdir -p $out
          ${zigSdfTool}/bin/gen-sdf \
            ${config.packages.lions-stack}/${chryso.microkitBoard}.dtb \
            ${config.packages.lionsos-src}/dep/sddf \
            $out \
            --with-crasher \
            --with-restart-debug
        '';

        # FAT disk image the fat fs_server serves to beam_server. Carries a
        # minimal OTP release (kernel + stdlib + the clean boot script) under
        # the -root layout ERTS expects, plus the device files ERTS opens and
        # the Gleam app's BEAM. Populated with mtools, then wrapped in an MBR
        # partition table (the blk virtualiser reads partition 0). BEAM
        # bytecode is platform-independent, so the host pkgs.erlang .beam
        # files load directly on the cross-built aarch64 ERTS.
        disk =
          let
            otp = "${pkgs.beamPackages.erlang}/lib/erlang";
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
              find ${config.packages.app} -type d -name ebin | while IFS= read -r dir; do
                app=$(basename "$(dirname "$dir")")
                mmd -i $part "::/lib/$app" "::/lib/$app/ebin" 2>/dev/null || true
                mcopy -i $part "$dir"/*.beam "::/lib/$app/ebin/" 2>/dev/null || true
              done

              # The console-driving test probes (tests/*.erl, built by rebar3 in
              # modules/beam.nix). Nothing ever starts this application: ERTS
              # boots -mode embedded and start_clean's path covers only kernel
              # and stdlib, so these beams are inert unless a test script asks
              # for them by name (see tests.nix's loader preamble). They ride
              # the one shared disk rather than a test-only variant because,
              # unlike the debug-restart channels and crasher PD that
              # production-sdf-gate keeps out of the shipped topology, bytecode
              # nothing loads grants no capability to anything.
              #
              # Landed UNVERSIONED even though buildRebar3 emits the usual
              # <app>-<vsn> directory: the guest path is hard-coded in the
              # loader (code:add_patha) and in chryso_test itself, and a version
              # bump should not have to be chased through both.
              mmd -i $part ::/lib/chryso_test ::/lib/chryso_test/ebin
              mcopy -i $part \
                ${config.packages.test-modules}/lib/erlang/lib/chryso_test-*/ebin/* \
                ::/lib/chryso_test/ebin/

              # Boot script: the clean boot (kernel + stdlib) as start.boot.
              cp "$otp/releases/$rel/start_clean.boot" start.boot
              mcopy -i $part start.boot "::/releases/$rel/start.boot"

              # Device files ERTS opens (empty: reads EOF, /dev/null sink).
              # NOT /dev/urandom or /dev/random: bringup.c's openat shim backs
              # those with the DRBG (an empty FAT file would just read EOF).
              : > empty
              mcopy -i $part empty ::/dev/null
              mcopy -i $part empty ::/dev/zero

              # Wrap the FAT image in an MBR table, partition 1 (the blk
              # virt's partition 0) starts at sector 2048 (1 MiB aligned).
              off=2048
              truncate -s 100M $out
              echo "label: dos
              start=$off, type=c" | sfdisk $out
              dd if=$part of=$out bs=512 seek=$off conv=notrunc status=none
            '';

        # Bring-up image (console + clock + heap, no ERTS).
        default = mkSel4Image {
          imgName = "sel4-beam-image";
          beamElf = "${config.packages.beam-zig}/bin/beam_server.elf";
        };

        # ERTS-linked image: the same PD topology with liberts.a linked in,
        # so beam_server's init() hands off to erl_start.
        test-image = mkSel4Image {
          imgName = "sel4-beam-test-image";
          beamElf = "${config.packages.beam-zig}/bin/beam_test.elf";
        };

        # ERTS image plus the crasher child of root: used only by the
        # restart-smoke test to exercise fault -> restart -> give-up while the
        # system boots through to the Eshell.
        restart-image = mkSel4Image {
          imgName = "sel4-beam-restart-image";
          beamElf = "${config.packages.beam-zig}/bin/beam_test.elf";
          sdf = config.packages.sdf-restart;
          extraElfs = [ "${config.packages.beam-zig}/bin/crasher.elf" ];
          restartDebug = true;
        };
      };
    };
}
