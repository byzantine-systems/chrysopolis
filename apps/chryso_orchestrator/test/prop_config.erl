-module(prop_config).
-moduledoc """
Law of `chryso_orchestrator_app:validate/1`: the defaults validate, and a
config with exactly one bad key fails naming that key.

| Planted defect | Caught by |
|---|---|
| `backoff_ms` accepted with `First > Largest` | prop_one_bad_key_is_named |
""".
-include_lib("proper/include/proper.hrl").

-export([prop_defaults_validate/0, prop_one_bad_key_is_named/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

prop_defaults_validate() ->
    ?FORALL(
        _,
        integer(),
        element(1, chryso_orchestrator_app:validate(chryso_orchestrator_app:config())) =:= ok
    ).

prop_one_bad_key_is_named() ->
    ?FORALL(
        {Key, Bad},
        ?LET(K, elements(keys()), {K, bad(K)}),
        begin
            Config = (chryso_orchestrator_app:config())#{Key := Bad},
            aggregate(
                [Key],
                chryso_orchestrator_app:validate(Config) =:= {error, {bad_config, Key, Bad}}
            )
        end
    ).

keys() ->
    [device, drivers, scan_ms, deadline_ms, backoff_ms, settle_ms, floor].

%% A value outside the key's domain, at a boundary where one exists.
bad(device) -> elements([missing, 42, "chryso_root_files"]);
bad(drivers) -> elements([[], [1, 1], [-1], not_a_list]);
bad(backoff_ms) -> ?LET({A, B}, {integer(2, 10), integer(1, 1)}, elements([{A, B}, {0, 5}, 100]));
bad(K) when K =:= scan_ms; K =:= deadline_ms -> elements([0, -1, ten]);
bad(K) when K =:= settle_ms; K =:= floor -> elements([-1, half]).
