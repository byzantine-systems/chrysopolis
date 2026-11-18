-module(chryso_decisions).
-moduledoc """
The controller's decision log: the last `Limit` inputs it fed
`chryso_lifecycle:step/2`, each with the effects the shell performed and the
state after it. `Base` is the state before the oldest kept entry.

Law (`prop_decisions`): `replay/1` folds `step` over the inputs from `Base`
and gets the performed effects back, so a shell that acts on its own (a
resend `step` never asked for) is caught on the guest.
""".

-export([new/2, log/4, entries/1, dropped/1, replay/1]).
-export_type([ring/0, entry/0]).

-doc "One input and the effects the shell performed for it.".
-type entry() :: {chryso_lifecycle:input(), [chryso_lifecycle:effect()]}.

-doc "A bounded log.".
-opaque ring() :: #{
    limit := pos_integer(),
    base := chryso_lifecycle:state(),
    entries := queue:queue({entry(), chryso_lifecycle:state()}),
    count := non_neg_integer(),
    dropped := non_neg_integer()
}.

-doc "An empty log starting from `State`.".
-spec new(Limit :: pos_integer(), State :: chryso_lifecycle:state()) -> ring().
new(Limit, State) ->
    #{limit => Limit, base => State, entries => queue:new(), count => 0, dropped => 0}.

-doc "Append one step. When full, the oldest entry goes and `Base` moves past it.".
-spec log(
    Input :: chryso_lifecycle:input(),
    Performed :: [chryso_lifecycle:effect()],
    After :: chryso_lifecycle:state(),
    Ring :: ring()
) -> ring().
log(Input, Performed, After, #{limit := Limit, entries := Q, count := N} = Ring) when N < Limit ->
    Ring#{entries := queue:in({{Input, Performed}, After}, Q), count := N + 1};
log(Input, Performed, After, #{entries := Q, dropped := D} = Ring) ->
    {{value, {_Oldest, Base}}, Q1} = queue:out(Q),
    Ring#{base := Base, entries := queue:in({{Input, Performed}, After}, Q1), dropped := D + 1}.

-doc "The kept entries, oldest first.".
-spec entries(Ring :: ring()) -> [entry()].
entries(#{entries := Q}) ->
    [Entry || {Entry, _After} <- queue:to_list(Q)].

-doc "How many entries the bound has dropped.".
-spec dropped(Ring :: ring()) -> non_neg_integer().
dropped(#{dropped := D}) ->
    D.

-doc """
`match` when stepping from `Base` reproduces every performed effect list and
the final state, else the 1-based index of the first entry that differs.
""".
-spec replay(Ring :: ring()) -> match | {mismatch, pos_integer()}.
replay(#{base := Base, entries := Q}) ->
    replay(queue:to_list(Q), Base, 1).

-spec replay([{entry(), chryso_lifecycle:state()}], chryso_lifecycle:state(), pos_integer()) ->
    match | {mismatch, pos_integer()}.
replay([], _State, _Index) ->
    match;
replay([{{Input, Performed}, After} | Rest], State, Index) ->
    case chryso_lifecycle:step(Input, State) of
        {Performed, After} -> replay(Rest, After, Index + 1);
        _ -> {mismatch, Index}
    end.
