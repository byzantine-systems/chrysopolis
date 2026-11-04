-module(prop_root_spec).
-moduledoc """
Laws of `chryso_root_spec`, one property each. Observation: the retained
policy and the refresh result.

Histories come from two generators: independent pages as the codec
classifies them (broad), and reads of a persistent two-bank page driven by
writer actions (commit, a commit that reuses a generation, a partial write,
corruption of the active bank, a writer restart that loses its counter), which is what makes unchanged, regressed and
conflicting banks reachable.
""".
-include_lib("proper/include/proper.hrl").
-include_lib("kernel/include/logger.hrl").

-export([
    prop_rejection_identity/0,
    prop_generation_monotone/0,
    prop_idempotent/0,
    prop_no_merge/0,
    prop_report_once/0,
    prop_generator_reaches_classes/0,
    prop_applied_budget_is_capped/0
]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(HARD_MAX, 64).
-define(CHILDREN, 6).

%% A refresh that applies nothing leaves the retained policy unchanged.
prop_rejection_identity() ->
    ?FORALL(
        {History, Page},
        history_and_next(),
        begin
            S0 = fold(History),
            {#{outcome := Outcome}, S1} = chryso_root_spec:consume(Page, S0),
            collect(
                Outcome,
                Outcome =:= applied orelse
                    chryso_root_spec:retained(S1) =:= chryso_root_spec:retained(S0)
            )
        end
    ).

%% The retained generation never decreases across any page sequence.
prop_generation_monotone() ->
    ?FORALL(
        Pages,
        history(),
        begin
            {Gens, _} = lists:mapfoldl(
                fun(P, S) ->
                    {_, S1} = chryso_root_spec:consume(P, S),
                    {generation(S1), S1}
                end,
                chryso_root_spec:new(),
                Pages
            ),
            Gens =:= lists:sort(Gens)
        end
    ).

%% Reading the same page twice applies at most once.
prop_idempotent() ->
    ?FORALL(
        {History, Page},
        {history(), page()},
        begin
            {_, S1} = chryso_root_spec:consume(Page, fold(History)),
            {#{outcome := Second}, S2} = chryso_root_spec:consume(Page, S1),
            Second =/= applied andalso
                chryso_root_spec:retained(S2) =:= chryso_root_spec:retained(S1)
        end
    ).

%% An equal generation with different content is a conflict, never a merge.
prop_no_merge() ->
    ?FORALL(
        {Gen, Budgets, Changed, Active},
        {integer(1, 5), budgets(), integer(0, ?CHILDREN - 1), integer(0, 1)},
        begin
            Desired = lists:duplicate(?CHILDREN, 1),
            Applied = {banks, Active, {bank, Gen, Budgets, Desired}, empty},
            {#{outcome := applied}, S1} = chryso_root_spec:consume(Applied, chryso_root_spec:new()),
            Other = bump(Changed, Budgets),
            {Result, S2} =
                chryso_root_spec:consume({banks, Active, {bank, Gen, Other, Desired}, empty}, S1),
            Result =:= #{outcome => kept, reason => conflict, record => true} andalso
                chryso_root_spec:retained(S2) =:= chryso_root_spec:retained(S1)
        end
    ).

%% A refresh that applied nothing, seen again, reports nothing new. After an
%% apply the same page may report a different reason (an active bank that
%% was a conflict becomes regressed), so the law is stated for non-applying
%% refreshes only.
prop_report_once() ->
    ?FORALL(
        {History, Page},
        {history(), page()},
        begin
            {#{outcome := Outcome, reason := Reason}, S1} =
                chryso_root_spec:consume(Page, fold(History)),
            {#{record := Again}, _} = chryso_root_spec:consume(Page, S1),
            collect(Reason, Outcome =:= applied orelse Reason =:= unset orelse not Again)
        end
    ).

%% Generator self-check: across a batch of histories every outcome and the
%% semantic rejections occur, so the laws above are not vacuous.
prop_generator_reaches_classes() ->
    ?FORALL(
        Histories,
        %% Explicit lengths: a class needs a bank seen again, which PropEr's
        %% size budget rarely allows inside a vector.
        vector(50, long_writer_history()),
        begin
            Seen = lists:usort(lists:flatmap(fun classes/1, Histories)),
            Required = [absent, applied, unchanged, kept, conflict, regressed, header],
            ?WHENFAIL(
                ?LOG_ERROR("missing classes: ~p", [Required -- Seen]),
                Required -- Seen =:= []
            )
        end
    ).

%% Anchor for the cap law through the spec: an applied entry above
%% hard_max yields hard_max. An entry of 0 yields the compiled budget.
prop_applied_budget_is_capped() ->
    ?FORALL(
        {Entry, Compiled},
        {oneof([0, integer(1, 6), ?HARD_MAX + 6]), integer(1, 6)},
        begin
            Budgets = [Entry | lists:duplicate(?CHILDREN - 1, 0)],
            Page = {banks, 0, {bank, 1, Budgets, lists:duplicate(?CHILDREN, 1)}, empty},
            {_, S} = chryso_root_spec:consume(Page, chryso_root_spec:new()),
            Want =
                case Entry of
                    0 -> Compiled;
                    E -> min(E, ?HARD_MAX)
                end,
            chryso_root_spec:budget(S, 0, Compiled, ?HARD_MAX) =:= Want
        end
    ).

%% --- helpers ------------------------------------------------------------------

fold(Pages) ->
    lists:foldl(
        fun(P, S) -> element(2, chryso_root_spec:consume(P, S)) end,
        chryso_root_spec:new(),
        Pages
    ).

generation(S) ->
    maps:get(generation, chryso_root_spec:retained(S)).

classes(Pages) ->
    {Classes, _} = lists:mapfoldl(
        fun(P, S) ->
            {#{outcome := O, reason := R}, S1} = chryso_root_spec:consume(P, S),
            {[O, R], S1}
        end,
        chryso_root_spec:new(),
        Pages
    ),
    lists:append(Classes).

bump(I, Budgets) ->
    {Before, [B | After]} = lists:split(I, Budgets),
    Before ++ [(B + 1) rem (?HARD_MAX + 7) | After].

%% --- generators ----------------------------------------------------------------

history() ->
    oneof([list(page()), writer_history()]).

%% A history and the next page: a fresh one, or the last page read again.
history_and_next() ->
    ?LET(
        {History, Fresh, Again},
        {history(), page(), boolean()},
        case {Again, History} of
            {true, [_ | _]} -> {History, lists:last(History)};
            _ -> {History, Fresh}
        end
    ).

%% Reads of one persistent page after each writer action. Generations come
%% from the writer, so repeats, older banks and reused generations occur.
writer_history() ->
    ?LET(Actions, list(writer_action()), pages(Actions)).

long_writer_history() ->
    ?LET(Len, integer(10, 30), ?LET(Actions, vector(Len, writer_action()), pages(Actions))).

writer_action() ->
    frequency([
        {4, {commit, budgets()}},
        {1, {reuse_generation, budgets()}},
        {1, partial},
        %% A restarted writer that lost its generation counter: its next
        %% commit is older than what Root retained.
        {1, restart_writer},
        {1, corrupt_active},
        {1, unmap},
        {1, bad_header}
    ]).

pages(Actions) ->
    Init = #{active => 0, banks => {empty, empty}, gen => 0, view => ok},
    {Pages, _} = lists:mapfoldl(
        fun(A, W) ->
            W1 = write(A, W),
            {read(W1), W1}
        end,
        Init,
        Actions
    ),
    Pages.

write({commit, B}, #{gen := G} = W) ->
    seal(G + 1, B, W#{gen := G + 1});
write({reuse_generation, B}, #{gen := G} = W) ->
    seal(max(G, 1), B, W#{gen := max(G, 1)});
write(partial, #{gen := G} = W) ->
    put_bank(1 - maps:get(active, W), {rejected, bank_torn, 0}, W#{gen := G, view := ok});
write(corrupt_active, #{active := A} = W) ->
    put_bank(A, {rejected, bank_checksum, 0}, W#{view := ok});
write(restart_writer, W) ->
    W#{gen := 0};
write(unmap, W) ->
    W#{view := absent};
write(bad_header, W) ->
    W#{view := header}.

%% The writer fills the inactive bank, then flips active_bank.
seal(Gen, Budgets, #{active := A} = W) ->
    Bank = {bank, Gen, Budgets, lists:duplicate(?CHILDREN, 1)},
    put_bank(1 - A, Bank, W#{active := 1 - A, view := ok}).

put_bank(0, Bank, #{banks := {_, B1}} = W) -> W#{banks := {Bank, B1}};
put_bank(1, Bank, #{banks := {B0, _}} = W) -> W#{banks := {B0, Bank}}.

read(#{view := absent}) -> absent;
read(#{view := header}) -> header;
read(#{active := 0, banks := {B0, B1}}) -> {banks, 0, B0, B1};
read(#{active := 1, banks := {B0, B1}}) -> {banks, 1, B1, B0}.

page() ->
    frequency([
        {1, absent},
        {1, header},
        {8, ?LET({A, X, Y}, {integer(0, 1), candidate(), candidate()}, {banks, A, X, Y})}
    ]).

candidate() ->
    frequency([
        {2, empty},
        {2, ?LET({R, G}, {codec_reason(), integer(0, 5)}, {rejected, R, G})},
        {6, ?LET({G, B, D}, {integer(1, 5), budgets(), desired()}, {bank, G, B, D})}
    ]).

codec_reason() ->
    elements([bank_torn, bank_checksum, bank_malformed, undeclared_child]).

budgets() ->
    vector(?CHILDREN, elements([0, 1, 2, 3, 5, ?HARD_MAX + 6])).

desired() ->
    vector(?CHILDREN, frequency([{9, 1}, {1, 0}])).
