-module(prop_root_policy).
-moduledoc """
Laws of `chryso_root_policy`, one property each. Observation: the action and
the record `step/3` returns.
""".
-include_lib("proper/include/proper.hrl").

-export([
    prop_absorption/0,
    prop_monotone/0,
    prop_budget_identity_and_cap/0,
    prop_exactly_budget_restarts/0
]).
%% PropEr calls these by name. Specs would only restate the generators.
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(HARD_MAX, 64).

%% gone absorbs: any budget and entry, no change, ignore_gone.
prop_absorption() ->
    ?FORALL(
        {Lifetime, Budget, Entry},
        {lifetime(), budget(), entry()},
        begin
            Gone = #{state => gone, lifetime => Lifetime},
            chryso_root_policy:step(Gone, Budget, Entry) =:= {ignore_gone, Gone}
        end
    ).

%% The lifetime count never decreases and grows by one exactly on resume.
prop_monotone() ->
    ?FORALL(
        {Record, Budget, Entry},
        {record(), budget(), entry()},
        begin
            {Action, #{lifetime := After}} = chryso_root_policy:step(Record, Budget, Entry),
            #{lifetime := Before} = Record,
            Grew = After - Before,
            collect(
                Action,
                Grew =:= expected_growth(Action) andalso After >= Before
            )
        end
    ).

%% No spec is the identity. A spec entry of 0 means compiled. Otherwise
%% min(spec, hard_max), which may exceed the compiled budget.
prop_budget_identity_and_cap() ->
    ?FORALL(
        {Spec, Compiled},
        {oneof([none, 0, budget()]), budget()},
        begin
            Got = chryso_root_policy:budget(Spec, Compiled, ?HARD_MAX),
            Want =
                case Spec of
                    none -> Compiled;
                    0 -> Compiled;
                    S -> min(S, ?HARD_MAX)
                end,
            collect(budget_class(Spec, Compiled), Got =:= Want)
        end
    ).

%% Anchor for the laws above: from a fresh record under budget B, exactly B
%% faults resume and the next one gives up, after which the child is gone.
prop_exactly_budget_restarts() ->
    ?FORALL(
        Budget,
        integer(0, 8),
        begin
            {Actions, _} = lists:mapfoldl(
                fun(_, R) -> chryso_root_policy:step(R, Budget, 1) end,
                chryso_root_policy:new(),
                lists:seq(1, Budget + 2)
            ),
            Actions =:= lists:duplicate(Budget, resume) ++ [giveup_budget, ignore_gone]
        end
    ).

budget_class(none, _) -> no_spec;
budget_class(0, _) -> zero_entry;
budget_class(S, _) when S > ?HARD_MAX -> capped;
budget_class(S, C) when S > C -> raised;
budget_class(_, _) -> lowered_or_equal.

expected_growth(resume) -> 1;
expected_growth(_) -> 0.

%% Budgets around the boundaries: 0, small, the hard maximum and above it.
budget() ->
    frequency([{1, 0}, {6, integer(1, 6)}, {1, ?HARD_MAX}, {1, ?HARD_MAX + 6}]).

lifetime() ->
    frequency([{1, 0}, {6, integer(1, 8)}, {1, ?HARD_MAX}]).

entry() ->
    frequency([{1, 0}, {9, integer(1, 16#FFFFFF)}]).

record() ->
    ?LET(
        {State, Lifetime},
        {frequency([{4, live}, {1, gone}]), lifetime()},
        #{state => State, lifetime => Lifetime}
    ).
