# Phase-zero baseline

This directory records the contracts and a compact fingerprint of the current Chrysopolis image. CI generates the full baseline JSON from pinned inputs. The fingerprint keeps unexpected changes visible without storing the generated JSON in Git.

The generated baseline contains normalized facts derived from the pinned flake inputs:

- The current runtime ABI and relevant dependency revisions.
- Generated production and restart SDF topology, capacity, and config-blob identities.
- Microkit 2.3.0 TCB, IRQ, and capability facts used by restart supervision.
- Runtime-relevant ELF segments, allocated sections, and symbols.
- Boot image sizes, and the FAT disk's MBR, partition and the hash of every product file on it.

The fingerprint describes the product only, so changing a test never moves it. The test payloads on the disk (`/lib/chryso_test`, `/lib/chryso_core` and `/fault-inflight.bin`), the host-test matrix and the check inventory are left out. `check-contracts` holds every flake check to an entry in `contracts.json` naming what it protects, and the testbed groups observe the serial behavior on a real boot.

Raw QEMU transcripts, timestamps, Nix store prefixes, debug sections, kernel-object physical addresses, and native segment hashes are intentionally excluded. Native segments can contain diagnostic source paths, so a source-only move can change their bytes without changing the ELF layout or runtime behavior.

## Check the baseline

Run the focused comparison with:

```bash
nix build .#checks.x86_64-linux.phase-zero-baseline -L
```

The derivation captures the facts twice, checks that both captures match, then compares the generated JSON with `expected.sha256`. To review a change, generate the current JSON and inspect its contents before updating the fingerprint:

```bash
nix build .#phase-zero-baseline-current -L
(cd result && sha256sum baseline.json)
```

The fingerprint must be recorded with the filename `baseline.json`, as in `expected.sha256`. The previous generated snapshots remain available in Git history for comparison.

The complete verification remains:

```bash
cd tests/host && zig build test --summary all
nix build .#checks.x86_64-linux.runtime-host-tests -L
nix build .#checks.x86_64-linux.production-sdf-gate -L
nix build .#checks.x86_64-linux.restart-topology -L
nix build .#test-image .#default .#disk .#restart-image .#budget-decay-image .#spec-commit-image .#command-image -L
nix flake check -L
```
