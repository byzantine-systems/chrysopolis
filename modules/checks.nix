# Hermetic QEMU integration tests, run by `nix flake check` and CI.
# NixOS-test-driver based (see tests.nix): each check boots a
# seL4 image headless under emulation via driver.create_machine and
# asserts on the serial trace / drives TCP peers from the test
# script, pinning a phase's exit criterion as an automated gate.
#
# Plus one pure (non-QEMU) check, production-sdf-gate, asserting the
# restart-test affordances stay out of the production topology.
{
  perSystem =
    { pkgs, config, ... }:
    {
      checks = {
        # Compile gate for the console-driving probes in tests/. Exposed as a
        # named check, not left as a transitive dependency of .#disk, so that a
        # typo in an .erl file fails in seconds instead of behind a multi-minute
        # cross build, and so it gates on darwin too, where the QEMU checks are
        # skipped. rebar.config sets warnings_as_errors, so a warning fails here.
        test-modules = config.packages.test-modules;

        # Regression guard for the restart-test gating, kept separate from the
        # QEMU checks because it is a pure grep over the generated system
        # description: seconds to run, and it gates on every platform.
        #
        # The debug-restart channels and the deliberately-faulting crasher PD are
        # test-only affordances. If either leaked into the production topology,
        # beam_server could ask root to restart a driver at will, and a PD whose
        # whole purpose is to fault would be in the shipped system. Both are
        # gated behind gen-sdf flags that only the restart SDF passes, so this
        # asserts the gate actually holds rather than trusting it.
        production-sdf-gate = pkgs.runCommand "chrysopolis-production-sdf-gate" { } ''
          sdf=${config.packages.sdf}/system.sdf

          # The debug-restart channels are root <-> beam_server. Root legitimately
          # has ONE production channel (root -> blk_virt, the give-up
          # notification), so the gate can no longer be "root has no channels";
          # it has to name the pairing that must not exist.
          #
          # Matched per <channel> element rather than per line: the two ends are
          # on separate lines, so a file-wide grep for each PD name would also
          # match root's give-up channel and beam_server's unrelated ones and
          # report a pairing that does not exist.
          channel_pair() {
            awk -v a="pd=\"$1\"" -v b="pd=\"$2\"" '
              /<channel>/       { inside = 1; block = "" }
              inside            { block = block $0 }
              /<\/channel>/     { inside = 0
                                  if (index(block, a) && index(block, b)) {
                                    print block; found = 1
                                  } }
              END               { exit(found ? 0 : 1) }
            ' "$3"
          }

          if channel_pair root beam_server "$sdf" >&2; then
            echo "production SDF has a root <-> beam_server channel (debug-restart leak)" >&2
            exit 1
          fi

          # The give-up channel is important in production: without it a
          # permanently stopped blk_driver leaves blk_virt holding every
          # outstanding request forever. Asserted positively so a refactor cannot
          # quietly drop it and leave only the negative checks passing.
          if ! channel_pair root blk_virt "$sdf" > /dev/null; then
            echo "production SDF is missing the root -> blk_virt give-up channel" >&2
            exit 1
          fi

          # The crasher PD must not be instantiated. Its ELF may exist in the
          # build output (one shared beam-zig derivation builds it for the
          # restart image); what must not happen is the SDF referencing it.
          if grep -q 'crasher' "$sdf"; then
            echo "production SDF references the crasher PD:" >&2
            grep -n 'crasher' "$sdf" >&2
            exit 1
          fi

          # beam_server must be a CHILD of root, asserted positively for the
          # same reason as the give-up channel above. Its whole restart path
          # depends on this one structural fact: Microkit delivers a PD's fault
          # to its PARENT, so a top-level beam_server sends its faults to the
          # monitor, root never sees them, and an ERTS crash goes back to being
          # terminal. Nothing else in the build would notice, because every
          # other check passes on a system that simply never restarts.
          #
          # Matched on the nesting rather than on a name: the child element sits
          # inside root's, so the test is that beam_server's tag appears at a
          # greater depth than root's. Real depth counting, not "any close tag
          # ends root": root has several children, and the first of THEM to
          # close would otherwise end the search before beam_server was reached
          # if it were ever reordered later in the list.
          awk '
            /<protection_domain / {
              depth++
              if ($0 ~ /name="root"/) { root_depth = depth }
              else if (root_depth && depth > root_depth && $0 ~ /name="beam_server"/) { found = 1 }
              next
            }
            /<\/protection_domain>/ {
              if (depth == root_depth) { root_depth = 0 }
              depth--
            }
            END { exit(found ? 0 : 1) }
          ' "$sdf" || {
            echo "production SDF does not nest beam_server inside root:" \
                 "its faults would go to the monitor and it would never be restarted" >&2
            exit 1
          }

          # Sanity: fail loudly if the SDF is empty or unparseable, so the
          # greps above cannot pass vacuously.
          grep -q '<protection_domain name="beam_server"' "$sdf" \
            || { echo "production SDF looks malformed (no beam_server PD)" >&2; exit 1; }

          touch $out
        '';

      }
      // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux (
        import ../tests.nix {
          inherit pkgs;
          sel4SystemImage = config.packages.default;
          sel4TestImage = config.packages.test-image;
          sel4RestartImage = config.packages.restart-image;
          fatDisk = config.packages.disk;
        }
      );
    };
}
