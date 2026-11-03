-module(prop_root).
-moduledoc """
Laws of the composed `chryso_root`.

Inputs are generated as a plan and resolved against the model while they
run (command words depend on the current `seq` and counts). The spec page is
a persistent store changed by `write` events. Every refresh reads the page
as it stands, as Root reads the shared page.
""".
-include_lib("proper/include/proper.hrl").
-include_lib("kernel/include/logger.hrl").

-export([prop_noninterference/0, prop_lifetime_within_hard_max/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(BEAM_CHILD, 5).
-define(HARD_MAX, 64).

%% Substitution: replace every accepted restart_child by a direct restart of
%% the same child and delete every other command. Each child's decisions, in
%% order, and the final records and retained generation are unchanged. So
%% commands reach a child only through the shared budget path.
prop_noninterference() ->
    ?FORALL(
        {Mapped, Plan},
        {frequency([{3, true}, {1, false}]), plan()},
        begin
            Full = run(Mapped, Plan),
            Plain = run(Mapped, substitute(Plan, Full)),
            Same =
                decisions(Full) =:= decisions(Plain) andalso
                    final(Full) =:= final(Plain),
            ?WHENFAIL(
                ?LOG_ERROR("full=~p~nplain=~p", [decisions(Full), decisions(Plain)]),
                aggregate(command_kinds(Full), Same)
            )
        end
    ).

%% The cap law through the whole composition: no child's lifetime count
%% exceeds hard_max, whatever specs and commands arrive.
prop_lifetime_within_hard_max() ->
    ?FORALL(
        {Mapped, Plan},
        {boolean(), plan()},
        begin
            #{root := Root} = run(Mapped, Plan),
            lists:all(
                fun({_, #{lifetime := L}}) -> L =< ?HARD_MAX end,
                maps:to_list(chryso_root:records(Root))
            )
        end
    ).

%% --- running a plan -------------------------------------------------------------

config(Mapped) ->
    #{
        mapped => Mapped,
        now => 1000,
        tokens => 4,
        refill => 400,
        hard_max => ?HARD_MAX,
        entry => 16#200000,
        pp => 11,
        beam => ?BEAM_CHILD,
        present => 16#2f,
        drivers => 16#0f,
        compiled => [3, 3, 3, 3, 3, 2],
        generation => 1,
        incarnation => 1,
        seq =>
            case Mapped of
                true -> 2;
                false -> 0
            end,
        abi => 1,
        cntfrq => 62500000
    }.

%% Runs every step. Records each decision and, per step, whether a command
%% was an accepted restart and of which child.
run(Mapped, Plan) ->
    Init = #{
        root => chryso_root:new(config(Mapped)),
        page => absent,
        now => 1000,
        last => none,
        log => [],
        accepted => []
    },
    S = lists:foldl(fun step/2, Init, lists:zip(lists:seq(1, length(Plan)), Plan)),
    S#{log := lists:reverse(maps:get(log, S)), accepted := lists:reverse(maps:get(accepted, S))}.

step({_, {write, Page}}, S) ->
    S#{page := Page};
step({_, {tick, Dt}}, #{now := Now} = S) ->
    S#{now := Now + Dt};
step({_, {fault, Child}}, #{root := Root, page := Page, now := Now, log := Log} = S) ->
    {D, Root1} = chryso_root:fault(Root, Child, Page, Now),
    S#{root := Root1, log := [summary(D) | Log]};
step({I, {cmd, Abstract}}, S) ->
    #{root := Root, page := Page, now := Now, last := Last, log := Log, accepted := Acc} = S,
    {Op, Count, Args} = resolve(Abstract, Root, Last),
    {_Reply, Decided, Root1} = chryso_root:command(Root, Op, Count, Args, Now, Page),
    Log1 =
        case Decided of
            none -> Log;
            _ -> [summary(Decided) | Log]
        end,
    %% A decision means the command reached the restart path, as the C
    %% harness's accepted_restart, even when the child turned out gone.
    Acc1 =
        case Decided of
            #{child := C} -> [{I, C} | Acc];
            none -> Acc
        end,
    S#{root := Root1, last := {Op, Count, Args}, log := Log1, accepted := Acc1}.

%% Command words from the plan's intent and the model's current state.
resolve({restart, Child, fresh}, Root, _Last) ->
    {2, 4, fresh_words(Child, Root)};
resolve({restart, Child, stale}, Root, _Last) ->
    [C, G, Seq, L] = fresh_words(Child, Root),
    {2, 4, [C, G, Seq + 2, L]};
resolve(repeat, _Root, {Op, Count, Args}) ->
    {Op, Count, Args};
resolve(repeat, Root, none) ->
    resolve(hello, Root, none);
resolve(hello, _Root, _) ->
    {1, 4, [1, 0, 0, 0]};
resolve(sync, _Root, _) ->
    {7, 4, [0, 0, 0, 0]};
resolve({malformed, short}, _Root, _) ->
    {7, 3, [0, 0, 0, 0]};
resolve({malformed, unused}, _Root, _) ->
    {7, 4, [0, 0, 0, 1]};
resolve({malformed, opcode}, _Root, _) ->
    {16#7777, 4, [0, 0, 0, 0]};
resolve({slot, Child}, _Root, _) ->
    {3, 4, [Child, 0, 0, 0]}.

fresh_words(Child, Root) ->
    #{records := Records, seq := Seq} = chryso_root:observe(Root),
    #{lifetime := L} = maps:get(Child, Records),
    [Child, 1, Seq, L].

%% Accepted restarts become direct restarts. Other commands vanish.
substitute(Plan, #{accepted := Accepted}) ->
    Index = maps:from_list(Accepted),
    lists:filtermap(
        fun
            ({I, {cmd, _}}) ->
                case maps:find(I, Index) of
                    {ok, Child} -> {true, {fault, Child}};
                    error -> false
                end;
            ({_, Step}) ->
                {true, Step}
        end,
        lists:zip(lists:seq(1, length(Plan)), Plan)
    ).

summary(#{child := C, action := A, record := R}) ->
    {C, A, R}.

decisions(#{log := Log}) ->
    Log.

final(#{root := Root}) ->
    #{records := Records, retained := #{generation := G}} = chryso_root:observe(Root),
    {Records, G}.

command_kinds(#{accepted := Accepted}) ->
    case Accepted of
        [] -> [no_accepted_restart];
        _ -> [accepted_restart]
    end.

%% --- generators ------------------------------------------------------------------

plan() ->
    list(
        frequency([
            {5, {fault, child()}},
            {5, {cmd, command()}},
            {2, {write, page()}},
            {2, {tick, integer(0, 1200)}}
        ])
    ).

child() ->
    elements([0, 1, 2, 3, ?BEAM_CHILD]).

command() ->
    frequency([
        {6, {restart, child(), frequency([{3, fresh}, {1, stale}])}},
        {2, repeat},
        {1, hello},
        {1, sync},
        {1, {malformed, elements([short, unused, opcode])}},
        {1, {slot, child()}}
    ]).

page() ->
    frequency([
        {1, absent},
        {1, header},
        {6, ?LET({A, X, Y}, {integer(0, 1), candidate(), candidate()}, {banks, A, X, Y})}
    ]).

candidate() ->
    frequency([
        {2, empty},
        {1, ?LET(G, integer(0, 5), {rejected, bank_checksum, G})},
        {5,
            ?LET(
                {G, B},
                {integer(1, 5), vector(6, elements([0, 1, 2, 5, ?HARD_MAX + 6]))},
                {bank, G, B, lists:duplicate(6, 1)}
            )}
    ]).
