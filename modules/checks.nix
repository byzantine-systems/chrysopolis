# Hermetic QEMU integration tests, run by `nix flake check` and CI.
# NixOS-test-driver based (see tests.nix): each check boots a
# seL4 image headless under emulation via driver.create_machine and
# asserts on the serial trace / drives TCP peers from the test
# script, pinning a phase's exit criterion as an automated gate.
{
  perSystem =
    { pkgs, config, ... }:
    {
      checks = pkgs.lib.optionalAttrs pkgs.stdenv.isLinux (
        import ../tests.nix {
          inherit pkgs;
          sel4SystemImage = config.packages.default;
          sel4TestImage = config.packages.test-image;
          fatDisk = config.packages.disk;
        }
      );
    };
}
