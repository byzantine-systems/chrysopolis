# chryso_core

The orchestrator's pure semantics. Today it holds the BEAM's executable model of Root's B3 algebra. It is a library application with no processes, no I/O and no clock: time is an input.

| Module | What it models |
|---|---|
| `chryso_root_policy` | one child's restart decision (`src/pd/root/restart_plan.c`) |
| `chryso_root_spec` | which committed spec bank Root retains (`src/pd/root/spec.c`) |
| `chryso_root_bucket` | the command token bucket's refill machine |
| `chryso_root_command` | frame checks, the retry fence and tallies (`src/pd/root/command.c`) |
| `chryso_root` | their product, composed the way Root's `main.c` composes the C units |

Each module states its laws in its `-doc`. It depends on nothing, `chryso_abi` included, and takes inputs as decoded maps.

The model is checked three ways:

- **Laws:** PropEr properties in `test/prop_*.erl`.
- **Traces from the C units:** `prop_root_traces` replays the traces from `nix build .#root-traces` (`tests/host/root/suite_root_fallback.c --traces`), read from `$CHRYSO_ROOT_TRACES`.
- **In situ:** `tests/chryso_ctl.erl` folds a QEMU run's faults and accepted restarts through `chryso_root_policy` and compares the result with Root's status rows. `packages.core-modules` puts these modules on the guest disk.

`test/chryso_core_cluster_SUITE.erl` is a Common Test suite. It checks the fence law across a `peer` node that hosts a simulated Root (`test/chryso_sim_root.erl`), with lost replies and dropped connections, observed through Snabbkaffe.

```bash
nix build .#checks.x86_64-linux.core-erlang -L   # everything above, plus Dialyzer and xref
chryso-core-rebar3 dialyzer                      # from the dev shell
```

Test dependencies are pinned for Nix in `rebar-deps.nix` by the rebar3_nix plugin. After changing the test profile, regenerate it:

```bash
rebar3 as test nix lock -o rebar-deps.nix
```
