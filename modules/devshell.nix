# Developer surface: the treefmt config (nix fmt + the formatting flake
# check) and the devenv shell with the run-sel4 helper.
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
      # nix fmt + nix flake check (auto-wired by flakeModule)
      treefmt = {
        projectRootFile = "flake.nix";
        # tcp.c is vendored from LionsOS lib/sock/tcp.c with local patches;
        # keep upstream's formatting so the diff stays upstreamable.
        settings.global.excludes = [ "src/runtime/tcp.c" ];
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
          if r != "" then r else builtins.toString ../.;

        packages = with pkgs; [
          erlfmt
          erlang-language-platform
          gnumake
          qemu
          cpio
          # cross-gcc for poking at the PD build by hand
          chryso.targetPkgs.stdenv.cc
        ];

        env = {
          MICROKIT_SDK = "${chryso.microkitSdk}";
          MICROKIT_BOARD = chryso.microkitBoard;
          MICROKIT_CONFIG = chryso.microkitConfig;
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

        languages.zig = {
          enable = true;
        };

        scripts.run-sel4.exec = ''
          set -e
          # The interactive Erlang shell lives in the ERTS-linked image
          # (.#test-image); the default package is a bring-up image with no
          # liberts.a. Build test-image into its own out-link so a stray
          # `nix build` (default package) can't shadow it, and let $1
          # override with an explicit image path.
          if [ -n "''${1:-}" ]; then
            img="$1"
          else
            nix build .#test-image --out-link result-test
            img="result-test/sel4-beam.img"
          fi
          # The FAT volume mounts read-write (ERTS opens /dev/null; the
          # filesystem must accept writes). The store image is read-only, so
          # keep a writable working copy that persists across reboots.
          disk="''${CHRYSO_DISK:-./chrysopolis-disk.img}"
          if [ ! -f "$disk" ]; then
            cp --no-preserve=mode ${config.packages.disk} "$disk"
          fi
          exec qemu-system-aarch64 \
            -machine virt,virtualization=on \
            -cpu cortex-a53 \
            -m size=2G \
            -serial mon:stdio \
            -nographic \
            -device loader,file="$img",addr=0x70000000,cpu-num=0 \
            -global virtio-mmio.force-legacy=false \
            -drive file="$disk",if=none,format=raw,id=hd \
            -device virtio-blk-device,drive=hd,bus=virtio-mmio-bus.1 \
            -device virtio-net-device,netdev=net0,bus=virtio-mmio-bus.0 \
            -netdev user,id=net0,hostfwd=tcp::8080-:8080
        '';

        enterShell = ''
          echo "========================================================="
          echo " Chrysopolis: BEAM on seL4 Microkit ${chryso.microkitVersion}"
          echo " MICROKIT_SDK=$MICROKIT_SDK"
          echo " Execute 'run-sel4' to boot the synthesized image in QEMU"
          echo "========================================================="
        '';
      };
    };
}
