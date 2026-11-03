# tools/testbed: runs scenarios against the Nix-built images under QEMU.
# Python dependencies come only from the pinned nixpkgs.
{
  perSystem =
    { pkgs, config, ... }:
    let
      python = pkgs.python3;
    in
    {
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
        dependencies = [ python.pkgs.qemu-qmp ];
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

      # Lint, strict types and tests. The Python reader must match every
      # Erlang vector.
      checks.testbed-unit = pkgs.stdenvNoCC.mkDerivation {
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
    };
}
