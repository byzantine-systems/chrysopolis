# Docs

## Testing Suite

A group's output holds `report.json`, `junit.xml`, each scenario's `observations.json`, and each machine's `console.log` (colour stripped) and `console.raw`.

Core function:

- **`boot-smoke`**: Headless boot of the ERTS image, asserts `beam_server` init, the sDDF monotonic clock, ERTS handoff, the FAT `MBR partitioning detected`, `Eshell`, and no PD faults.
- **`cothread-smoke`**: Boots a diagnostic bring-up image and checks stack-allocation cleanup, pthread handles, TLS, locks, once, condition deadlines, and cothread slot exhaustion and reuse inside `beam_server`.
- **`socket-smoke`**: Boots the bring-up image (no ERTS) and asserts the linked lwIP stack gets a DHCP lease and `socket()/bind()/listen()/connect()` succeed from C.
- **`lifecycle-config-failure-smoke`**: Boots a test-only image with malformed serial configuration and proves beam_server logs the contract failure and parks before exposing partial state.
- **`shell-smoke`**: Drives the interactive Erlang shell over the serial console and asserts that arithmetic evaluates.
- **`tcp-smoke`**: Drives the shell to prove `gen_tcp` both directions: a host client echoes off a guest listener, and the guest connects out to a host listener.
- **`rng-smoke`**: Boots the ERTS image twice and asserts the RNG fingerprint, `rand:bytes/1` and `erlang:make_ref/0` all differ across boots (see *Entropy* below).

Crash and restart:

- **`restart-smoke`**: Root catches a deliberately faulting child, restarts it until the budget is spent, then gives up.
- **`budget-decay-smoke`**: A test Root uses the AArch64 counter to refill a fault window while retaining a hard lifetime restart ceiling.
- **`beam-restart-smoke`**: An ERTS exit (`init:stop()`, then `erlang:halt(3)`) restarts `beam_server` to a fresh `1>`, carrying the exit code to Root, with none of the previous VM's state surviving.
- **`serial-restart-smoke`**, **`timer-restart-smoke`**, **`blk-restart-smoke`**, **`net-restart-smoke`**: A healthy driver of each class is restarted on request and its subsystem must keep working.
- **`serial-fault-smoke`**, **`timer-fault-smoke`**, **`blk-fault-smoke`**, **`net-fault-smoke`**: Each real driver is forced to take a genuine seL4 fault, Root catches and restarts it, and its subsystem must recover. The block case includes a client request in flight.
- **`blk-giveup-smoke`**: Genuine block-driver faults spend the entire restart budget, Root stops the driver for good, and the system must degrade rather than wedge.

Root's control plane and the orchestrator:

- **`orch-status-smoke`**, **`orch-command-smoke`**, **`orch-spec-smoke`**: The BEAM reads Root's status page, sends it fenced, rate-limited commands, and commits a restart policy that survives a BEAM restart, all through `/dev/chrysostat` and `/dev/chrysoctl`.
- **`orch-driver-smoke`**: The OTP orchestrator (`apps/chryso_orchestrator`) wants, looks at and restarts drivers while they fault and run out of budget. Every outcome is judged by the testbed's fault model, the orchestrator's decision log must replay, and Root's event ring must record exactly the commands it sent.
- **`orch-restart-smoke`**: An image patched for it boots the orchestrator release (`orch.boot`) with no shell command. BEAM halts never make Root repeat a command, and near the BEAM's restart budget beam_server boots the plain shell instead (safe mode).
- **`orch-fallback-smoke`**: Root's fault handling keeps its latency bound under a command flood, and keeps restarting drivers after it gives up on the BEAM.

Pure and generated-artifact gates include:

- **`runtime-host-tests`** runs every pure runtime suite with Debug/UBSan and ReleaseFast.
- **`abi-contract`** proves both generated SDF variants represent the declared ABI and emit every configured data blob.
- **`production-sdf-gate`** asserts test-only restart affordances stay out of the shipped topology.
- **`restart-topology`** checks Root's children, entries, priorities, fault endpoints, and relevant capability slots against the SDF and ABI.
- **`test-modules`** compiles the guest-side probes in `tests/`.
- **`c23-diagnostic`** compiles first-party C with stricter diagnostics and warnings as errors.
- **`treefmt`** checks Nix, Gleam, Erlang, C, and Zig formatting.
- **`phase-zero-baseline`** regenerates normalized SDF, Microkit report, ELF, image, disk and toolchain facts about the product, with no test facts, and checks their fingerprint against [`baselines/phase-zero`](baselines/phase-zero).
- **`check-contracts`** requires an entry in `baselines/phase-zero/contracts.json` for every flake check, naming what it protects.
