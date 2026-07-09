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
          nativeBuildInputs = [ chryso.llvm.libllvm ];
          buildCommand = ''
            set -ex  # Exit on error, print commands
            mkdir -p $out build
            cp ${beamElf} build/beam_server.elf
            # Driver/virtualiser PDs from the root build.zig (beamZig), the
            # serial/timer client PDs (beam_server) come from beamElf above.
            cp ${config.packages.beam-zig}/bin/serial_driver.elf \
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

            cfg=${config.packages.sdf}
            oc() { llvm-objcopy --update-section "$1"="$cfg/$2" "build/$3"; }
            oc .device_resources     serial_driver_device_resources.data serial_driver.elf
            oc .serial_driver_config serial_driver_config.data           serial_driver.elf
            oc .serial_virt_tx_config serial_virt_tx.data                serial_virt_tx.elf
            oc .serial_virt_rx_config serial_virt_rx.data                serial_virt_rx.elf
            oc .device_resources     timer_driver_device_resources.data  timer_driver.elf
            oc .serial_client_config serial_client_beam_server.data      beam_server.elf
            oc .timer_client_config  timer_client_beam_server.data       beam_server.elf

            # Block subsystem: driver device resources + driver/virt configs.
            oc .device_resources     blk_driver_device_resources.data    blk_driver.elf
            oc .blk_driver_config    blk_driver.data                     blk_driver.elf
            oc .blk_virt_config      blk_virt.data                       blk_virt.elf

            # FAT fs_server: fatfs is the blk client (partition 0) and the fs
            # server, beam_server is the fs client (libc fs path dormant until
            # the memfs cutover).
            oc .blk_client_config    blk_client_fatfs.data               fat.elf
            oc .fs_server_config     fs_server_fatfs.data                fat.elf
            oc .fs_client_config     fs_client_beam_server.data          beam_server.elf

            # Network subsystem: driver device resources + driver/virt/copy
            # configs.
            oc .device_resources     eth_driver_device_resources.data    eth_driver.elf
            oc .net_driver_config    net_driver.data                     eth_driver.elf
            oc .net_virt_rx_config   net_virt_rx.data                    net_virt_rx.elf
            oc .net_virt_tx_config   net_virt_tx.data                    net_virt_tx.elf
            oc .net_copy_config      net_copy_net_copy.data              net_copy.elf

            # Socket client: beam_server links the lwIP stack + LionsOS
            # socket backend, so it now carries the net client config and
            # the lib_sddf_lwip (pbuf pool) config it reads at sddf_lwip_init.
            oc .net_client_config    net_client_beam_server.data           beam_server.elf
            oc .lib_sddf_lwip_config lib_sddf_lwip_config_beam_server.data  beam_server.elf

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
        default = mkSel4Image "sel4-beam-image" "${config.packages.beam-zig}/bin/beam_server.elf";

        # ERTS-linked image: the same PD topology with liberts.a linked in,
        # so beam_server's init() hands off to erl_start.
        test-image = mkSel4Image "sel4-beam-test-image" "${config.packages.beam-zig}/bin/beam_test.elf";
      };
    };
}
