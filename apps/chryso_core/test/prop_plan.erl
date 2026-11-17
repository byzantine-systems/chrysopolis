-module(prop_plan).
-moduledoc """
Laws of `chryso_plan`.

| Planted defect | Caught by |
|---|---|
| `next` planning from `unknown` with a zero fence | prop_unknown_plans_nothing |
| `next` addressing a stopped or gone child | prop_never_addresses_a_child_that_is_not_live |
| `next` planning a want that already holds (`=<` for `<`) | prop_fixpoint |
| the fence taken from the want instead of the page | prop_fence_is_the_page |
""".
-include_lib("proper/include/proper.hrl").

-export([
    prop_then_is_associative/0,
    prop_none_is_identity/0,
    prop_unknown_plans_nothing/0,
    prop_never_addresses_a_child_that_is_not_live/0,
    prop_fixpoint/0,
    prop_bounded/0,
    prop_fence_is_the_page/0,
    prop_blocked_children_are_left_alone/0
]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

prop_then_is_associative() ->
    ?FORALL(
        {A, B, C},
        {plan(), plan(), plan()},
        cmds(then(A, then(B, C))) =:= cmds(then(then(A, B), C))
    ).

prop_none_is_identity() ->
    ?FORALL(
        A,
        plan(),
        cmds(then(chryso_plan:none(), A)) =:= cmds(A) andalso
            cmds(then(A, chryso_plan:none())) =:= cmds(A)
    ).

prop_unknown_plans_nothing() ->
    ?FORALL(
        W,
        wants(),
        cmds(chryso_plan:next(chryso_observe:unknown(), W, sets:new())) =:= []
    ).

prop_never_addresses_a_child_that_is_not_live() ->
    ?FORALL(
        {K, W},
        {prop_observe:knowledge(), wants()},
        lists:all(
            fun({restart, C, _}) -> state_of(K, C) =:= live end,
            cmds(chryso_plan:next(K, W, sets:new()))
        )
    ).

%% When every want holds on the page, there is nothing to do.
prop_fixpoint() ->
    ?FORALL(
        P,
        prop_observe:page(),
        begin
            #{children := Children} = P,
            Held = maps:map(fun(_, #{last_restart := T}) -> T end, Children),
            cmds(chryso_plan:next(known(P), Held, sets:new())) =:= []
        end
    ).

prop_bounded() ->
    ?FORALL(
        {K, W},
        {prop_observe:knowledge(), wants()},
        begin
            N = length(cmds(chryso_plan:next(K, W, sets:new()))),
            aggregate([N], N =< 1)
        end
    ).

prop_fence_is_the_page() ->
    ?FORALL(
        {P, W},
        {prop_observe:page(), wants()},
        lists:all(
            fun({restart, C, Fence}) ->
                #{root_generation := G, seq := S, children := #{C := #{lifetime := L}}} = P,
                Fence =:= #{root_generation => G, seq => S, lifetime => L}
            end,
            cmds(chryso_plan:next(known(P), W, sets:new()))
        )
    ).

prop_blocked_children_are_left_alone() ->
    ?FORALL(
        {K, W, Blocked},
        {prop_observe:knowledge(), wants(), list(integer(0, 3))},
        begin
            Set = sets:from_list(Blocked),
            lists:all(
                fun({restart, C, _}) -> not sets:is_element(C, Set) end,
                cmds(chryso_plan:next(K, W, Set))
            )
        end
    ).

%% --- helpers ------------------------------------------------------------------

cmds(P) ->
    chryso_plan:commands(P).

then(A, B) ->
    chryso_plan:then(A, B).

known(P) ->
    {ok, K} = chryso_observe:from_page(P),
    K.

state_of(K, C) ->
    case chryso_observe:child(K, C) of
        unknown -> unknown;
        #{state := S} -> S
    end.

%% --- generators ---------------------------------------------------------------

wants() ->
    ?LET(Pairs, list({integer(0, 3), integer(0, 6000)}), maps:from_list(Pairs)).

plan() ->
    ?LET(
        Steps,
        list({integer(0, 3), integer(1, 2), integer(0, 8), integer(0, 4)}),
        lists:foldl(
            fun({C, G, S, L}, Acc) ->
                then(Acc, chryso_plan:restart(C, #{root_generation => G, seq => S, lifetime => L}))
            end,
            chryso_plan:none(),
            Steps
        )
    ).
