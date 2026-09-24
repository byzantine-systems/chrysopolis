# Hermetic QEMU integration tests, run by `nix flake check` and CI.
# NixOS-test-driver based (see tests.nix): each check boots a
# seL4 image headless under emulation via driver.create_machine and
# asserts on the serial trace / drives TCP peers from the test
# script, pinning a phase's exit criterion as an automated gate.
#
# Pure (non-QEMU) checks also validate the generated ABI and assert that
# restart-test affordances stay out of the production topology.
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
      runtimeAbi = builtins.fromJSON (builtins.readFile ../interfaces/generated/system-abi.json);
      drivers = runtimeAbi.drivers;
      abiToolDeps = chryso.zigEnv.deriveLockFile ../tools/abi/build.zig.zon2json-lock {
        inherit (chryso.zigEnv) zig;
        name = "chrysopolis-abi-dependencies";
      };
      hex = value: "0x${pkgs.lib.toHexString value}";

      # The behavioral baseline documents every check visible in the final
      # flake output, including treefmt, which is contributed by devshell.nix.
      # Only the attribute names are forced here. Check derivation values are
      # not evaluated, avoiding a dependency from the baseline capture back to
      # the checks whose contracts it inventories.
      phaseZeroCheckNames = pkgs.writeText "chrysopolis-phase-zero-check-names.json" (
        builtins.toJSON (
          pkgs.lib.subtractLists [ "abi-stale" "project-structure" ] (builtins.attrNames config.checks)
        )
      );

      capturePhaseZeroBaseline = output: ''
        ${pkgs.python3}/bin/python ${../nix/capture-phase-zero-baseline.py} \
          --abi ${../interfaces/generated/system-abi.json} \
          --flake-lock ${../flake.lock} \
          --host-build ${../tests/host/build.zig} \
          --contracts ${../baselines/phase-zero/contracts.json} \
          --check-names ${phaseZeroCheckNames} \
          --production-sdf ${config.packages.sdf} \
          --restart-sdf ${config.packages.sdf-restart} \
          --beam-zig ${config.packages.beam-zig} \
          --production-image ${config.packages.default} \
          --test-image ${config.packages.test-image} \
          --restart-image ${config.packages.restart-image} \
          --disk ${config.packages.disk} \
          --readelf ${chryso.llvm.libllvm}/bin/llvm-readelf \
          --microkit-version ${chryso.microkitVersion} \
          --microkit-board ${chryso.microkitBoard} \
          --microkit-config ${chryso.microkitConfig} \
          --zig-version 0.15.2 \
          --llvm-version ${chryso.llvm.release_version} \
          --otp-version ${pkgs.beamPackages.erlang.version} \
          --output ${output}
      '';

      # Runs one image's report.txt through the topology checker, against the
      # SDF that image was synthesised from.
      checkTopology = mode: image: sdf: ''
        ${pkgs.lib.getExe config.packages.check-restart-topology} \
          ${image}/report.txt ${sdf}/system.sdf \
          ${chryso.boardDir}/include/microkit.h ${../interfaces/generated/system-abi.json} \
          ${mode}
      '';
    in
    {
      # The report.txt parser behind the restart-topology check, exposed so it
      # can be run by hand against a modified report or SDF.
      packages = {
        abi-tool =
          let
            source = pkgs.lib.fileset.toSource {
              root = ../.;
              fileset = pkgs.lib.fileset.unions [
                ../tools/abi
                ../interfaces/system_abi.zig
              ];
            };
          in
          pkgs.stdenvNoCC.mkDerivation {
            name = "chrysopolis-abi-tool";
            src = source;
            nativeBuildInputs = [ inputs'.zig2nix.packages."zig-0_15_2" ];
            buildPhase = ''
              runHook preBuild
              export ZIG_GLOBAL_CACHE_DIR="$TMPDIR/zig-cache"
              mkdir -p "$ZIG_GLOBAL_CACHE_DIR"
              ln -s ${abiToolDeps} "$ZIG_GLOBAL_CACHE_DIR"/p
              zig test interfaces/system_abi.zig
              cd tools/abi
              zig build --prefix $out -Doptimize=ReleaseSafe
              runHook postBuild
            '';
            dontInstall = true;
          };
        check-restart-topology = pkgs.writeShellApplication {
          name = "check-restart-topology";
          runtimeInputs = [
            pkgs.gawk
            pkgs.gnugrep
            pkgs.jq
            pkgs.coreutils
          ];
          text = builtins.readFile ../nix/check-restart-topology.sh;
        };
      }
      // pkgs.lib.optionalAttrs pkgs.stdenv.isLinux {
        # A reviewable snapshot of all phase-zero facts. Generate it twice in
        # one derivation and compare the results so accidental iteration or
        # path ordering cannot produce a baseline that changes between runs.
        phase-zero-baseline-current = pkgs.runCommand "chrysopolis-phase-zero-baseline-current" { } ''
          mkdir -p $out
          ${capturePhaseZeroBaseline "$out/current-a.json"}
          ${capturePhaseZeroBaseline "$out/current-b.json"}
          cmp "$out/current-a.json" "$out/current-b.json"
          mv "$out/current-a.json" "$out/baseline.json"
          rm "$out/current-b.json"
        '';
      };

      checks = {
        # Dependency direction, explicit first-party source membership,
        # public header isolation and the vendored TCP formatting boundary.
        project-structure = pkgs.stdenvNoCC.mkDerivation {
          name = "chrysopolis-project-structure";
          src = pkgs.lib.fileset.toSource {
            root = ../.;
            fileset = pkgs.lib.fileset.unions [
              ../build.zig
              ../build.zig.zon
              ../build
              ../src
              ../include
              ../interfaces
              ../modules
              ../nix/check-project-structure.py
              ../nix/test-project-structure.py
            ];
          };
          nativeBuildInputs = [
            pkgs.python3
            chryso.llvm.clang
          ];
          dontConfigure = true;
          dontInstall = true;
          buildPhase = ''
            runHook preBuild
            python nix/test-project-structure.py
            python nix/check-project-structure.py .
            touch $out
            runHook postBuild
          '';
        };
        abi-stale = pkgs.runCommand "chrysopolis-abi-stale" { } ''
          ${config.packages.abi-tool}/bin/gen-system-abi first.json
          ${config.packages.abi-tool}/bin/gen-system-abi second.json
          cmp first.json second.json
          cmp first.json ${../interfaces/generated/system-abi.json}
          touch $out
        '';
        # Compile gate for the console-driving probes in tests/. Exposed as a
        # named check, not left as a transitive dependency of .#disk, so that a
        # typo in an .erl file fails in seconds instead of behind a multi-minute
        # cross build, and so it gates on darwin too, where the QEMU checks are
        # skipped. rebar.config sets warnings_as_errors, so a warning fails here.
        test-modules = config.packages.test-modules;

        # Host tests for the pure runtime logic: ID and range checks, timeout
        # arithmetic, ownership rollback and state transitions that the QEMU
        # checks cannot drive into their invalid or boundary cases. Seconds to
        # run and platform-independent, like test-modules above.
        runtime-host-tests = config.packages.runtime-host-tests;

        # The Zig parser rejects malformed, overlapping and out-of-range ABI
        # values while producing both SDF variants. This check then verifies
        # that the rendered topology and config-blob set reflect those values.
        # C layout and ELF section sizes are checked by c23-diagnostic and the
        # image builders respectively.
        abi-contract = pkgs.runCommand "chrysopolis-abi-contract" { } ''
          production=${config.packages.sdf}
          restart=${config.packages.sdf-restart}

          require_line() {
            local file=$1 text=$2
            grep -Fq "$text" "$file" || {
              echo "ABI contract missing from $file: $text" >&2
              exit 1
            }
          }
          require_channel() {
            local file=$1 a=$2 b=$3
            awk -v a="$a" -v b="$b" '
              /<channel>/   { inside = 1; block = "" }
              inside        { block = block $0 }
              /<\/channel>/ { inside = 0
                               if (index(block, a) && index(block, b)) found = 1 }
              END           { exit(found ? 0 : 1) }
            ' "$file" || {
              echo "ABI channel missing from $file: $a <-> $b" >&2
              exit 1
            }
          }

          for sdf in "$production/system.sdf" "$restart/system.sdf"; do
            require_line "$sdf" '<memory_region name="beam_heap" size="${hex runtimeAbi.memory.heap.size}"'
            require_line "$sdf" '<memory_region name="beam_snapshot" size="${hex runtimeAbi.memory.snapshot.size}"'
            require_line "$sdf" '<map mr="beam_heap" vaddr="${hex runtimeAbi.memory.heap.vaddr}" perms="rw" setvar_vaddr="${runtimeAbi.memory.heap.setvar}" />'
            require_line "$sdf" '<map mr="beam_snapshot" vaddr="${hex runtimeAbi.memory.snapshot.vaddr}" perms="rw" setvar_vaddr="${runtimeAbi.memory.snapshot.setvar}" />'
            require_line "$sdf" '<protection_domain name="beam_server" id="${toString runtimeAbi.children.beam}"'
            ${pkgs.lib.concatMapStringsSep "\n            " (driver: ''
              require_line "$sdf" '<protection_domain name="${driver.name}_driver" id="${toString driver.child}"'
            '') drivers}
            require_channel "$sdf" 'pd="root" id="${toString runtimeAbi.giveup.root_blk_channel}"' \
                                   'pd="blk_virt" id="${toString runtimeAbi.giveup.blk_virt_channel}"'
          done

          require_line "$restart/system.sdf" '<protection_domain name="crasher" id="${toString runtimeAbi.children.crasher}"'
          ${pkgs.lib.concatMapStringsSep "\n          " (driver: ''
            require_channel "$restart/system.sdf" \
              'pd="root" id="${toString driver.root_debug_channel}"' \
              'pd="beam_server" id="${toString driver.beam_debug_channel}"'
            require_channel "$restart/system.sdf" \
              'pd="root" id="${toString driver.root_fault_channel}"' \
              'pd="beam_server" id="${toString driver.beam_fault_channel}"'
          '') drivers}

          for output in "$production" "$restart"; do
            ${pkgs.lib.concatMapStringsSep "\n            " (mapping: ''
              test -f "$output/${mapping.blob}" || {
                echo "ABI config blob missing from $output: ${mapping.blob}" >&2
                exit 1
              }
            '') runtimeAbi.config_sections}
          done

          touch $out
        '';

        # Regression guard for the restart-test gating, kept separate from the
        # QEMU checks because it is a pure grep over the generated system
        # description: seconds to run, and it gates on every platform.
        #
        # The debug restart/fault-injection channels and the deliberately
        # faulting crasher PD are test-only affordances. If either leaked into
        # the production topology, beam_server could restart or fault a driver
        # at will, and a PD whose whole purpose is to fault would be in the
        # shipped system. Both are gated behind gen-sdf flags that only the
        # restart SDF passes, so this asserts the gate actually holds rather
        # than trusting it.
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
            echo "production SDF has a root <-> beam_server channel (restart/fault injection leak)" >&2
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
        {
          c23-diagnostic = config.packages.beam-zig-diagnostic;

          # Root's restart topology as the Microkit tool actually built it:
          # which PDs fault to Root, their entry and priority, Root's TCB caps
          # and the notification caps between Root and its peers, read from
          # report.txt and cross-checked against the generated SDF and
          # system-abi.json. Both images, because the restart image adds the
          # crasher and the debug channels and production must have neither.
          restart-topology = pkgs.runCommand "chrysopolis-restart-topology" { } ''
            ${checkTopology "production" config.packages.default config.packages.sdf}
            ${checkTopology "restart" config.packages.restart-image config.packages.sdf-restart}
            touch $out
          '';

          # The checked artifact and behavior reference. A mismatch prints a
          # field-level unified diff. The expected file is intentionally not
          # regenerated inside this check: updating it is a reviewed decision.
          phase-zero-baseline = pkgs.runCommand "chrysopolis-phase-zero-baseline" { } ''
            diff -u \
              ${../baselines/phase-zero/expected-v1.json} \
              ${config.packages.phase-zero-baseline-current}/baseline.json
            cp ${config.packages.phase-zero-baseline-current}/baseline.json $out
          '';
        }
        // import ../tests.nix {
          inherit pkgs;
          sel4SystemImage = config.packages.default;
          sel4TestImage = config.packages.test-image;
          sel4RestartImage = config.packages.restart-image;
          sel4LifecycleFailureImage = config.packages.lifecycle-failure-image;
          sel4ThreadProbeImage = config.packages.cothread-probe-image;
          fatDisk = config.packages.disk;
        }
      );
    };
}
