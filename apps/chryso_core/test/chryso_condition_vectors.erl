-module(chryso_condition_vectors).
-moduledoc """
What `chryso_condition:classify/2` says for every small Root row, at the
guest's parameters (`settle_ms` 0, `floor` 0, every want held). The testbed's
`faults.condition` must agree on every row, so the model and the shipped
classifier are two statements of one table.

Output is deterministic: no clock, no seed, no node identity.
""".

-export([write/1]).
-export_type([vector/0]).

-define(STATES, [unset, live, stopped, gone, quarantined]).
-define(UPTO, 4).

-doc "One row and what the classifier makes of it.".
-type vector() :: #{
    state := chryso_observe:child_state(),
    lifetime := non_neg_integer(),
    budget := non_neg_integer(),
    condition := chryso_condition:condition(),
    reason := chryso_condition:reason()
}.

-doc "Write `vectors.jsonl` and `manifest.txt` into `Dir`, which must exist.".
-spec write(file:filename()) -> ok.
write(Dir) ->
    Rows = [
        row(State, Lifetime, Budget)
     || State <:- ?STATES, Lifetime <:- lists:seq(0, ?UPTO), Budget <:- lists:seq(0, ?UPTO)
    ],
    ok = file:write_file(filename:join(Dir, "vectors.jsonl"), [[json:encode(R), $\n] || R <:- Rows]),
    Manifest = io_lib:format("chryso-condition-vectors 1~ncount ~b~n", [length(Rows)]),
    ok = file:write_file(filename:join(Dir, "manifest.txt"), Manifest).

-spec row(chryso_observe:child_state(), non_neg_integer(), non_neg_integer()) -> vector().
row(State, Lifetime, Budget) ->
    Row = #{state => State, lifetime => Lifetime, budget => Budget, last_restart => 0},
    Context = #{
        now => 1000000,
        cntfrq => 1000,
        settle_ms => 0,
        floor => 0,
        requested => false,
        held => false
    },
    {Condition, Reason} = chryso_condition:classify(Row, Context),
    #{
        state => State,
        lifetime => Lifetime,
        budget => Budget,
        condition => Condition,
        reason => Reason
    }.
