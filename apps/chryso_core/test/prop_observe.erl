-module(prop_observe).
-moduledoc """
Laws of `chryso_observe`. Pages are generated over small stamp ranges, so ties
and reorderings come up often.

| Planted defect | Caught by |
|---|---|
| `merge` keeping the older page | prop_newest_wins |
| `merge` letting `unknown` win over a page | prop_unknown_is_identity |
| `from_page` accepting a negative field or a foreign state | prop_from_page_rejects_what_is_not_a_page |
""".
-include_lib("proper/include/proper.hrl").

-export([
    prop_merge_is_associative/0,
    prop_merge_is_commutative/0,
    prop_merge_is_idempotent/0,
    prop_unknown_is_identity/0,
    prop_newest_wins/0,
    prop_from_page_accepts_pages/0,
    prop_from_page_rejects_what_is_not_a_page/0
]).
-export([page/0, knowledge/0, known/1]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

prop_merge_is_associative() ->
    ?FORALL(
        {A, B, C},
        {knowledge(), knowledge(), knowledge()},
        merge(A, merge(B, C)) =:= merge(merge(A, B), C)
    ).

prop_merge_is_commutative() ->
    ?FORALL({A, B}, {knowledge(), knowledge()}, merge(A, B) =:= merge(B, A)).

prop_merge_is_idempotent() ->
    ?FORALL(A, knowledge(), merge(A, A) =:= A).

prop_unknown_is_identity() ->
    ?FORALL(
        A,
        knowledge(),
        merge(chryso_observe:unknown(), A) =:= A andalso merge(A, chryso_observe:unknown()) =:= A
    ).

%% Of two pages with different stamps, the merge views the one with the
%% greater {root_generation, seq}, whichever order they arrive in.
prop_newest_wins() ->
    ?FORALL(
        {P, Q},
        {page(), page()},
        begin
            Merged = chryso_observe:view(merge(known(P), known(Q))),
            Newest =
                case stamp_of(P) >= stamp_of(Q) of
                    true -> P;
                    false -> Q
                end,
            Class =
                case stamp_of(P) =:= stamp_of(Q) of
                    true -> tie;
                    false -> ordered
                end,
            aggregate([Class], Class =:= tie orelse Merged =:= Newest)
        end
    ).

prop_from_page_accepts_pages() ->
    ?FORALL(P, page(), element(1, chryso_observe:from_page(P)) =:= ok).

prop_from_page_rejects_what_is_not_a_page() ->
    ?FORALL(
        {P, How},
        {page(), elements([drop_field, negative, foreign_state, bad_row])},
        aggregate([How], chryso_observe:from_page(break(How, P)) =:= {error, invalid})
    ).

%% --- helpers ------------------------------------------------------------------

merge(A, B) ->
    chryso_observe:merge(A, B).

known(P) ->
    {ok, K} = chryso_observe:from_page(P),
    K.

stamp_of(#{root_generation := G, seq := S}) ->
    {G, S}.

break(drop_field, P) ->
    maps:remove(seq, P);
break(negative, P) ->
    P#{seq := -1};
break(foreign_state, #{children := C} = P) ->
    P#{children := C#{0 => (row_template())#{state := running}}};
break(bad_row, #{children := C} = P) ->
    P#{children := C#{1 => #{state => live}}}.

row_template() ->
    #{state => live, lifetime => 0, budget => 3, last_restart => 0}.

%% --- generators ---------------------------------------------------------------

%% Knowledge built through the public constructors as a symbolic expression
%% (proper_symb), so a counterexample shrinks to a smaller expression.
knowledge() ->
    ?LET(Expr, expression(), proper_symb:eval(Expr)).

expression() ->
    ?SIZED(Size, expression(Size)).

expression(0) ->
    frequency([
        {1, {'$call', chryso_observe, unknown, []}},
        {4, ?LET(P, page(), {'$call', ?MODULE, known, [P]})}
    ]);
expression(Size) ->
    frequency([
        {2, expression(0)},
        {1,
            ?LAZY(
                {'$call', chryso_observe, merge, [expression(Size div 2), expression(Size div 2)]}
            )}
    ]).

page() ->
    ?LET(
        {G, S, I, Now, Rows},
        {integer(1, 2), integer(0, 4), integer(1, 3), integer(0, 5000), vector(4, row())},
        #{
            root_generation => G,
            beam_incarnation => I,
            seq => 2 * S,
            now => Now,
            cntfrq => 1000,
            children => maps:from_list(lists:zip(lists:seq(0, 3), Rows))
        }
    ).

row() ->
    ?LET(
        {State, L, B, T},
        {
            frequency([{6, live}, {2, gone}, {1, stopped}, {1, quarantined}]),
            integer(0, 4),
            integer(1, 4),
            integer(0, 5000)
        },
        #{state => State, lifetime => L, budget => B, last_restart => T}
    ).
