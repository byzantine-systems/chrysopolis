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

      # The comparisons behind abi-stale and orchestrator-abi, shared with
      # abi-negative so it plants failures in the code the gates run.
      sameBytes = a: b: "cmp ${a} ${b}";
      sameTree = a: b: "diff -r --no-dereference ${a} ${b}";
      generateOrchestratorAbi = dir: ''
        ${config.packages.abi-tool}/bin/gen-orchestrator-abi c ${dir}/c
        ${config.packages.abi-tool}/bin/gen-orchestrator-abi erlang ${dir}/erlang
        ${config.packages.abi-tool}/bin/gen-orchestrator-abi vectors ${dir}/vectors
      '';
      # apps/chryso_abi's rebar3 deps, exported by the rebar3_nix plugin
      # (`rebar3 as test nix lock -o rebar-deps.nix`), so nothing comes from Hex at build time.
      chrysoAbiDeps = import ../apps/chryso_abi/rebar-deps.nix {
        inherit (pkgs.beamPackages) fetchHex;
        inherit (pkgs) fetchgit fetchFromGitHub;
        builder = pkgs.beamPackages.buildRebar3;
      };
      individualVmNames = [
        "boot-smoke"
        "shell-smoke"
        "tcp-smoke"
        "restart-smoke"
        "serial-restart-smoke"
        "serial-fault-smoke"
        "timer-restart-smoke"
        "timer-fault-smoke"
        "blk-restart-smoke"
        "blk-fault-smoke"
        "net-restart-smoke"
        "net-fault-smoke"
      ];
      vmTests = import ../tests.nix {
        inherit pkgs;
        sel4SystemImage = config.packages.default;
        sel4TestImage = config.packages.test-image;
        sel4RestartImage = config.packages.restart-image;
        sel4BudgetDecayImage = config.packages.budget-decay-image;
        sel4LifecycleFailureImage = config.packages.lifecycle-failure-image;
        sel4ThreadProbeImage = config.packages.cothread-probe-image;
        fatDisk = config.packages.disk;
      };
      vmScenarioPackages = builtins.listToAttrs (
        map (name: {
          name = "vm-scenario-${name}";
          value = vmTests.${name};
        }) individualVmNames
      );

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
                ../interfaces/orchestrator_abi.zig
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
              cd tools/abi
              zig build test --summary all
              zig build --prefix $out -Doptimize=ReleaseSafe
              runHook postBuild
            '';
            dontInstall = true;
          };
        # Both projections of the orchestration ABI, C headers under c/ and the
        # Erlang codec under erlang/, plus the golden vectors both must agree
        # on under vectors/. All three carry the same layout digest. A second
        # run from another directory, into a relative path, with a different
        # time zone, locale and umask must produce the same tree.
        orchestrator-abi =
          pkgs.runCommand "chrysopolis-orchestrator-abi" { abiFresh = config.checks.abi-stale; }
            ''
              ${generateOrchestratorAbi "$out"}
              mkdir -p "$TMPDIR/again"
              (
                cd "$TMPDIR/again"
                umask 077
                export TZ=XYZ-14 LC_ALL=C.UTF-8
                ${generateOrchestratorAbi "tree"}
              )
              ${sameTree "$out" "$TMPDIR/again/tree"}
              c=$(sed -n 's/.*chryso_abi_layout_digest = 0x\([0-9a-f]*\)u;.*/\1/p' $out/c/chrysopolis/orchestrator_abi.h)
              erl=$(sed -n 's/.*CHRYSO_ABI_LAYOUT_DIGEST, 16#\([0-9a-f]*\)).*/\1/p' $out/erlang/include/orchestrator_abi.hrl)
              vec=$(sed -n '1s/^chryso-abi-vectors [0-9]* [0-9]* \([0-9a-f]*\)$/\1/p' $out/vectors/manifest.txt)
              if [ -z "$c" ] || [ "$c" != "$erl" ] || [ "$c" != "$vec" ]; then
                echo "layout digest differs: C '$c', Erlang '$erl', vectors '$vec'" >&2
                exit 1
              fi
            '';
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
      }
      // vmScenarioPackages;

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
              # Scanned for generated ABI files that must not be committed.
              ../tools
              ../tests
              (pkgs.lib.fileset.maybeMissing ../apps)
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
          ${config.packages.abi-tool}/bin/gen-abi first-system.json first-orchestrator.json
          ${config.packages.abi-tool}/bin/gen-abi second-system.json second-orchestrator.json
          ${sameBytes "first-system.json" "second-system.json"}
          ${sameBytes "first-orchestrator.json" "second-orchestrator.json"}
          ${sameBytes "first-system.json" "${../interfaces/generated/system-abi.json}"}
          touch $out
        '';
        # The typed contracts, generators and every negative fixture under
        # tools/abi: the package fails when `zig build test` does.
        abi-tool = config.packages.abi-tool;

        # Proves the ABI gates can fail. Each half runs a positive control, then
        # plants one difference: a changed value in a copy of the committed
        # system JSON (formatting kept, so only the value differs), and one
        # flipped byte in a copy of the generated tree.
        abi-negative = pkgs.runCommand "chrysopolis-abi-negative" { } ''
          ${config.packages.abi-tool}/bin/gen-abi system.json orchestrator.json
          cp ${../interfaces/generated/system-abi.json} committed.json
          ${sameBytes "system.json" "committed.json"}
          ${pkgs.python3}/bin/python - <<'EOF'
          import pathlib, re
          text = pathlib.Path("committed.json").read_text()
          stale, count = re.subn(r'("driver_budget":)([0-9]+)', lambda m: m[1] + str(int(m[2]) + 1), text)
          assert count == 1, count
          pathlib.Path("stale.json").write_text(stale)
          EOF
          if ${sameBytes "system.json" "stale.json"}; then
            echo "abi-negative: a stale system projection passed" >&2
            exit 1
          fi

          cp -r ${config.packages.orchestrator-abi} tree
          chmod -R u+w tree
          ${sameTree "${config.packages.orchestrator-abi}" "tree"}
          bin=$(find tree/vectors -name '*.bin' | sort | head -n 1)
          ${pkgs.python3}/bin/python -c 'import sys; p = sys.argv[1]; b = bytearray(open(p, "rb").read()); b[0] ^= 1; open(p, "wb").write(b)' "$bin"
          if ${sameTree "${config.packages.orchestrator-abi}" "tree"} >/dev/null; then
            echo "abi-negative: a changed generated tree passed" >&2
            exit 1
          fi
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

        # EUnit, PropEr and the shared golden vectors over the generated Erlang
        # codec. PropEr comes from rebar-deps.nix; the Hex-only plugin and
        # test-profile deps are dropped from the sandbox copy of rebar.config
        # so rebar3 never reaches the network. Every erl_opts flag is kept.
        # Dialyzer stays a local command.
        abi-erlang = pkgs.stdenvNoCC.mkDerivation {
          name = "chrysopolis-abi-erlang";
          src = pkgs.lib.fileset.toSource {
            root = ../apps/chryso_abi;
            fileset = pkgs.lib.fileset.unions [
              ../apps/chryso_abi/rebar.config
              ../apps/chryso_abi/rebar.config.script
              ../apps/chryso_abi/src
              ../apps/chryso_abi/test
            ];
          };
          nativeBuildInputs = [
            pkgs.beamPackages.erlang
            pkgs.beamPackages.rebar3
          ];
          dontConfigure = true;
          dontInstall = true;
          buildPhase = ''
            runHook preBuild
            export HOME=$TMPDIR
            export CHRYSO_ABI_GENERATED=${config.packages.orchestrator-abi}
            export ERL_LIBS=${chrysoAbiDeps.proper}/lib/erlang/lib
            erl -noshell -eval '
              {ok, Terms} = file:consult("rebar.config"),
              Kept = [T || T <- Terms, not lists:member(element(1, T), [plugins, profiles])],
              ok = file:write_file("rebar.config", [io_lib:format("~tp.~n", [T]) || T <- Kept]),
              halt().'
            rebar3 eunit
            touch $out
            runHook postBuild
          '';
        };

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
            # Root control plane: both regions in every image, with the exact
            # one-writer rights and setvar symbols from the ABI.
            require_line "$sdf" '<memory_region name="root_status" size="${hex runtimeAbi.control.status.size}"'
            require_line "$sdf" '<memory_region name="orchestrator_spec" size="${hex runtimeAbi.control.spec.size}"'
            require_line "$sdf" '<map mr="root_status" vaddr="${hex runtimeAbi.control.status.root_vaddr}" perms="rw" setvar_vaddr="${runtimeAbi.control.status.root_setvar}" />'
            require_line "$sdf" '<map mr="root_status" vaddr="${hex runtimeAbi.control.status.beam_vaddr}" perms="r" setvar_vaddr="${runtimeAbi.control.status.beam_setvar}" />'
            require_line "$sdf" '<map mr="orchestrator_spec" vaddr="${hex runtimeAbi.control.spec.beam_vaddr}" perms="rw" setvar_vaddr="${runtimeAbi.control.spec.beam_setvar}" />'
            require_line "$sdf" '<map mr="orchestrator_spec" vaddr="${hex runtimeAbi.control.spec.root_vaddr}" perms="r" setvar_vaddr="${runtimeAbi.control.spec.root_setvar}" />'
            require_channel "$sdf" 'pd="beam_server" id="${toString runtimeAbi.control.pp_channel.beam}" notify="false" pp="true"' \
                                   'pd="root" id="${toString runtimeAbi.control.pp_channel.root}" notify="false"'
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

          # root <-> beam_server channels: exactly one is allowed, the
          # protected procedure call, and it must carry the ABI's pinned ids
          # with pp on the beam end. Anything else between the two PDs is a
          # notification edge: beam_server could poke root's notified(), which
          # production must keep unreachable (the restart/fault-injection
          # channels are the restart image's test affordance, and a stray
          # notification would be exactly that leak).
          ppc_seen=0
          while IFS= read -r block; do
            if grep -q 'pp="true"' <<<"$block"; then
              ppc_seen=$((ppc_seen + 1))
              grep -q "pd=\"beam_server\" id=\"${toString runtimeAbi.control.pp_channel.beam}\"" <<<"$block" || {
                  echo "production PPC channel does not use ABI beam id ${toString runtimeAbi.control.pp_channel.beam}" >&2
                  exit 1
                }
              grep -q "pd=\"root\" id=\"${toString runtimeAbi.control.pp_channel.root}\"" <<<"$block" || {
                  echo "production PPC channel does not use ABI root id ${toString runtimeAbi.control.pp_channel.root}" >&2
                  exit 1
                }
            else
              echo "production SDF has a root <-> beam_server channel without pp (notification leak):" >&2
              printf '%s\n' "$block" >&2
              exit 1
            fi
          done < <(channel_pair root beam_server "$sdf")
          if [ "$ppc_seen" -ne 1 ]; then
            echo "production SDF has $ppc_seen root <-> beam_server pp channels, expected exactly 1" >&2
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
          # system-abi.json. Check every assembled image: production SDF users
          # include the bring-up, ERTS, cothread-probe and config-failure
          # images; both restart images add the crasher and debug channels.
          restart-topology = pkgs.runCommand "chrysopolis-restart-topology" { } ''
            ${checkTopology "production" config.packages.default config.packages.sdf}
            ${checkTopology "production" config.packages.test-image config.packages.sdf}
            ${checkTopology "production" config.packages.cothread-probe-image config.packages.sdf}
            ${checkTopology "production" config.packages.lifecycle-failure-image config.packages.sdf}
            ${checkTopology "restart" config.packages.restart-image config.packages.sdf-restart}
            ${checkTopology "restart" config.packages.budget-decay-image config.packages.sdf-restart}
            touch $out
          '';

          # Proves the control-plane half of restart-topology can fail. Takes
          # the production report and SDF, passes them through the checker
          # untouched (the control), then plants one difference per case: a
          # wrong or deleted PPC badge, a signaling or PPC cap in Root's CNode,
          # a drifted fault badge, a one-writer rights flip, a cache-attribute
          # alias mismatch and a moved control window. Each planted case must
          # be rejected; a vacuous mutation aborts the fixture generator.
          restart-topology-negative = pkgs.runCommand "chrysopolis-restart-topology-negative" { } ''
            base_endpoint=$(awk '$1 == "#define" && $2 == "BASE_ENDPOINT_CAP" { print $3; exit }' \
              ${chryso.boardDir}/include/microkit.h)
            pp_beam=${toString runtimeAbi.control.pp_channel.beam}
            pp_root=${toString runtimeAbi.control.pp_channel.root}
            ppc_badge=$(printf '0x%x' $((0x8000000000000000 | pp_root)))
            fault_badge=$(printf '0x%x' $((0x4000000000000000 | ${toString runtimeAbi.children.beam})))
            ${pkgs.python3}/bin/python ${../nix/mutate-topology-fixtures.py} \
              ${config.packages.default}/report.txt ${config.packages.sdf}/system.sdf \
              $TMPDIR/fixtures \
              --ppc-slot $((base_endpoint + pp_beam)) \
              --ppc-badge "$ppc_badge" \
              --fault-badge "$fault_badge"
            ${checkTopology "production" "$TMPDIR/fixtures/control" "$TMPDIR/fixtures/control"}
            for case_dir in $TMPDIR/fixtures/fail-*; do
              if ${pkgs.lib.getExe config.packages.check-restart-topology} \
                   "$case_dir/report.txt" "$case_dir/system.sdf" \
                   ${chryso.boardDir}/include/microkit.h \
                   ${../interfaces/generated/system-abi.json} production \
                   >/dev/null 2>"$TMPDIR/err"; then
                echo "restart-topology-negative: $(basename "$case_dir") passed a planted failure" >&2
                exit 1
              fi
              case $(basename "$case_dir") in
                fail-ppc-badge) expected='PPC badge' ;;
                fail-ppc-cap-removed) expected='does not hold exactly 2 ep_root cap' ;;
                fail-root-signal) expected='signaling cap to beam_server' ;;
                fail-ppc-in-root) expected='carries the PPC badge' ;;
                fail-second-ppc-caller) expected='unauthorized PPC cap to Root' ;;
                fail-fault-badge) expected='no fault-badged ep_root cap' ;;
                fail-status-write | fail-spec-write) expected='maps .* with perms' ;;
                fail-alias-attribute) expected='sets a cache attribute' ;;
                fail-vaddr) expected='maps root_status at' ;;
                *) echo "restart-topology-negative: unexpected fixture $case_dir" >&2; exit 1 ;;
              esac
              if ! grep -Eq "$expected" "$TMPDIR/err"; then
                echo "restart-topology-negative: $(basename "$case_dir") failed for the wrong reason:" >&2
                cat "$TMPDIR/err" >&2
                exit 1
              fi
            done
            touch $out
          '';

          # The checked artifact and behavior reference. The JSON is generated
          # during the build; its compact fingerprint is updated only after
          # reviewing changes to the captured facts.
          phase-zero-baseline = pkgs.runCommand "chrysopolis-phase-zero-baseline" { } ''
            cd ${config.packages.phase-zero-baseline-current}
            sha256sum -c ${../baselines/phase-zero/expected.sha256}
            cp ${config.packages.phase-zero-baseline-current}/baseline.json $out
          '';
        }
        // builtins.removeAttrs vmTests individualVmNames
      );
    };
}
