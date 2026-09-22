# Phase-zero baseline

This directory records the facts that define the current Chrysopolis image. It is not a second
test suite. It collects the contracts and outputs of the existing checks into one deterministic,
reviewable comparison.

`expected-v1.json` contains normalized facts derived from the pinned flake inputs:

- the current runtime ABI and relevant dependency revisions;
- generated production and restart SDF topology, capacity, and config-blob identities;
- Microkit 2.3.0 TCB, IRQ, and capability facts used by restart supervision;
- runtime-relevant ELF segments, allocated sections, and symbols;
- boot image sizes and the FAT disk hash;
- host-test suites and build variants;
- every flake check and the artifact or serial behavior it protects.

Raw QEMU transcripts, timestamps, Nix store prefixes, debug sections, kernel-object physical
addresses, and native segment hashes are intentionally excluded. Native segments can contain
diagnostic source paths, so a source-only move can change their bytes without changing the ELF
layout or runtime behavior. `contracts.json` records required and forbidden serial markers
instead. The QEMU checks remain responsible for observing those markers on a real boot.

## Check the baseline

Run the focused comparison with:

```bash
nix build .#checks.x86_64-linux.phase-zero-baseline -L
```

The derivation captures the current facts twice and first proves that capture is deterministic.
It then performs a formatted JSON diff against `expected-v1.json`, so a mismatch identifies the
changed field. Generate the current candidate without accepting it with:

```bash
nix build .#phase-zero-baseline-current -L
diff -u baselines/phase-zero/expected-v1.json result/baseline.json
```

The complete verification remains:

```bash
cd tests/host && zig build test --summary all
nix build .#checks.x86_64-linux.runtime-host-tests -L
nix build .#checks.x86_64-linux.production-sdf-gate -L
nix build .#checks.x86_64-linux.restart-topology -L
nix build .#test-image .#default .#disk .#restart-image -L
nix flake check -L
```

## Updating the expected file

Do not update the expected file merely to make the check pass. Any difference in topology,
capability allocation, ELF layout, image size, or observed behavior must be understood first.
An update is valid when correcting the capture procedure or accepting an intentional architecture
change. In either case, inspect the field-level diff and record the reason in the change that
updates the baseline.
