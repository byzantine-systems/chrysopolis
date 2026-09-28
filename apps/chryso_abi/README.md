# chryso_abi

Erlang side of the orchestration ABI: `chryso_abi_codec` and `orchestrator_abi.hrl`, generated
by `tools/abi` from `interfaces/orchestrator_abi.zig`. It is a library application with no
processes. B4's reconciler depends on it (Nix `beamDeps`, or `ERL_LIBS` locally).

It lives under `apps/` rather than beside the Gleam sources in `src/`, which belong to the
Gleam payload and the C runtime adapter.

The codec is never committed. Generate it, then point rebar3 at it:

```bash
export CHRYSO_ABI_GENERATED=$(nix build --no-link --print-out-paths .#orchestrator-abi)/erlang
rebar3 eunit          # EUnit cases plus PropEr properties
rebar3 dialyzer
```

Every checker returns `ok` or `{error, Class}`, with the same classes and precedence as the C
checkers in `<chrysopolis/*.h>`. Decoders take a snapshot the C side already copied under its
seqlock; they cannot observe shared memory changing.

Dependencies are pinned for Nix in `rebar-deps.nix` by the rebar3_nix plugin. After changing
`deps` or the test profile, regenerate it (the test profile is included because the Nix check
runs the property tests):

```bash
rebar3 as test nix lock -o rebar-deps.nix
```
