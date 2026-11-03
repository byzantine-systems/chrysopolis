# tools/testbed: runs scenarios against the Nix-built images under QEMU.
# Python dependencies come only from the pinned nixpkgs.
{
  perSystem =
    { pkgs, config, ... }:
    let
      python = pkgs.python3;
      lib = pkgs.lib;

      # Probe modules the shell loads, derived from the directories as the
      # NixOS-driver checks derive them. chryso_test loads the rest.
      erlModules =
        dir:
        map (lib.removeSuffix ".erl") (
          lib.filter (lib.hasSuffix ".erl") (builtins.attrNames (builtins.readDir dir))
        );
      testModules =
        lib.filter (m: m != "chryso_test") (erlModules ../tests) ++ erlModules ../apps/chryso_core/src;

      images = [
        "default"
        "test-image"
        "restart-image"
        "budget-decay-image"
        "cothread-probe-image"
        "lifecycle-failure-image"
        "spec-commit-image"
        "command-image"
        "fallback-image"
        "fallback-nomap-image"
      ];

      manifest = pkgs.writeText "testbed-manifest.json" (
        builtins.toJSON {
          images = lib.genAttrs images (name: "${config.packages.${name}}");
          disk = "${config.packages.disk}";
          abi = "${../interfaces/generated/system-abi.json}";
          qemu = "${pkgs.qemu}/bin/qemu-system-aarch64";
          qemu_img = "${pkgs.qemu}/bin/qemu-img";
          test_modules = testModules;
        }
      );

      runner = python.withPackages (_: [ config.packages.testbed ]);

      # One derivation per group. Scenarios run concurrently up to the build's
      # cores, and the report lands in $out.
      group =
        name:
        pkgs.runCommand "chrysopolis-testbed-${name}" { nativeBuildInputs = [ runner ]; } ''
          jobs=''${NIX_BUILD_CORES:-1}
          [ "$jobs" -gt 0 ] || jobs=$(nproc)
          chryso-testbed run --manifest ${manifest} --group ${name} --out $out --jobs "$jobs"
        '';
      # Lint, strict types and tests. The Python reader must match every
      # Erlang vector.
      unit = pkgs.stdenvNoCC.mkDerivation {
        name = "chrysopolis-testbed-unit";
        src = pkgs.lib.fileset.toSource {
          root = ../tools/testbed;
          fileset = pkgs.lib.fileset.unions [
            ../tools/testbed/pyproject.toml
            ../tools/testbed/src
            ../tools/testbed/tests
          ];
        };
        nativeBuildInputs = [
          (python.withPackages (p: [
            p.pytest
            p.hypothesis
            p.qemu-qmp
            p.typer
            p.mypy
          ]))
          pkgs.ruff
        ];
        dontConfigure = true;
        dontInstall = true;
        env.CHRYSO_FRAME_VECTORS = "${config.packages.frame-vectors}";
        buildPhase = ''
          runHook preBuild
          export HOME=$TMPDIR PYTHONPATH=$PWD/src
          ruff check --no-cache .
          ruff format --check --no-cache .
          mypy --cache-dir=$TMPDIR/mypy
          python -m pytest
          touch $out
          runHook postBuild
        '';
      };
    in
    {
      packages.testbed-manifest = manifest;

      # nix run .#testbed -- shell test-image, or -- run -k <scenario>.
      apps.testbed = {
        type = "app";
        program = lib.getExe (
          pkgs.writeShellApplication {
            name = "chryso-testbed";
            runtimeInputs = [ runner ];
            text = ''
              export CHRYSO_TESTBED_MANIFEST=''${CHRYSO_TESTBED_MANIFEST:-${manifest}}
              exec chryso-testbed "$@"
            '';
          }
        );
      };

      checks = {
        testbed-unit = unit;
      }
      // lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
        testbed-boot = group "boot";
        testbed-restart = group "restart";
      };

      packages.testbed = python.pkgs.buildPythonPackage {
        pname = "chryso-testbed";
        version = "0.1.0";
        pyproject = true;
        src = pkgs.lib.fileset.toSource {
          root = ../tools/testbed;
          fileset = pkgs.lib.fileset.unions [
            ../tools/testbed/pyproject.toml
            ../tools/testbed/src
          ];
        };
        build-system = [ python.pkgs.setuptools ];
        dependencies = [
          python.pkgs.qemu-qmp
          python.pkgs.typer
        ];
        pythonImportsCheck = [ "chryso_testbed" ];
      };

      # Console-frame vectors from the Erlang reader. Generated twice and
      # compared, so nondeterminism fails the build.
      packages.frame-vectors = pkgs.stdenvNoCC.mkDerivation {
        name = "chrysopolis-frame-vectors";
        src = pkgs.lib.fileset.toSource {
          root = ../apps/chryso_core;
          fileset = pkgs.lib.fileset.unions [
            ../apps/chryso_core/src/chryso_frame.erl
            ../apps/chryso_core/test/chryso_frame_vectors.erl
          ];
        };
        nativeBuildInputs = [ pkgs.beamPackages.erlang ];
        dontConfigure = true;
        buildPhase = ''
          runHook preBuild
          mkdir ebin first second
          erlc -o ebin +deterministic +warnings_as_errors \
            src/chryso_frame.erl test/chryso_frame_vectors.erl
          erl -noshell -pa ebin -eval 'ok = chryso_frame_vectors:write("first"), halt().'
          # Second run from another directory, time zone and umask.
          (cd second && umask 077 && TZ=Pacific/Kiritimati \
            erl -noshell -pa ../ebin -eval 'ok = chryso_frame_vectors:write("."), halt().')
          diff -r first second
          runHook postBuild
        '';
        installPhase = ''
          runHook preInstall
          cp -r first $out
          runHook postInstall
        '';
      };

    };
}
