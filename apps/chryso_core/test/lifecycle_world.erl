-module(lifecycle_world).
-moduledoc """
The SUT for `prop_lifecycle`: `chryso_lifecycle` driven against the shipped
`chryso_root`. World state lives in the process dictionary for one run.
Every call is total, so a shrunk command sequence never makes an illegal call.
Ticks are milliseconds (`cntfrq` 1000): Root and the controller share a clock.

Each event returns what it caused: commands emitted, restarts Root accepted
from commands (with the want each was for), commands Root cannot have acted
on (deadline fired with no reply, or `rate_limited`),
whether the controller got a known page, and the drivers it showed gone.
""".

-export([reset/0, fault/1, want/1, scan/0, stale_scan/0, unknown_scan/0]).
-export([deliver/0, lose/0, drop/0, deadline/0, tick/1, restart_ctl/0, flood/1]).
-export([open_wants/0, page/0, controller/0, last_restarts/0, params/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(KEY, '$lifecycle_world').
-define(DRIVERS, [0, 1, 2, 3]).
%% Uneven budgets, so a driver Root gives up on is a few faults away.
-define(BUDGETS, [3, 3, 2, 1]).
-define(RESTART_CHILD, 2).
-define(SYNC, 7).

params() ->
    #{
        drivers => ?DRIVERS,
        deadline_ms => 200,
        backoff_ms => {50, 400},
        settle_ms => 100,
        floor => 0
    }.

reset() ->
    put(?KEY, #{
        root => chryso_root:new(root_config()),
        now => 1000,
        last => #{},
        ctl => chryso_lifecycle:new(params()),
        spec => #{},
        pending => [],
        armed => [],
        sent => #{},
        history => []
    }),
    ok.

%% --- events ---------------------------------------------------------------------

fault(C) ->
    W = #{root := Root, now := Now, last := Last} = get(?KEY),
    {D, Root1} = chryso_root:fault(Root, C, absent, Now),
    put(?KEY, W#{root := Root1, last := resumed(D, Now, Last)}),
    caused([], []).

want(C) ->
    W = #{now := Now, spec := Spec} = get(?KEY),
    put(?KEY, W#{spec := Spec#{C => max(Now, maps:get(C, Spec, 0))}}),
    feed([{want, C, Now}], #{}).

scan() ->
    Page = page(),
    W = get(?KEY),
    put(?KEY, W#{history := [Page | maps:get(history, W)]}),
    feed([{observed, Page}], #{known => true, gone => gone_on(Page)}).

stale_scan() ->
    case maps:get(history, get(?KEY)) of
        [] -> caused([], []);
        History -> feed([{observed, lists:last(History)}], #{known => true})
    end.

unknown_scan() ->
    feed([{observed, chryso_observe:unknown()}], #{}).

deliver() ->
    perform(reply).

lose() ->
    perform(lost).

drop() ->
    W = get(?KEY),
    case maps:get(pending, W) of
        [] -> ok;
        [_ | Rest] -> put(?KEY, W#{pending := Rest})
    end,
    caused([], []).

deadline() ->
    W = get(?KEY),
    case maps:get(armed, W) of
        [] ->
            caused([], []);
        [Ref | Rest] ->
            put(?KEY, W#{armed := Rest}),
            Late = [maps:get(Ref, maps:get(sent, W))],
            feed([{deadline, Ref}], #{resendable => Late})
    end.

tick(Dt) ->
    W = #{now := Now} = get(?KEY),
    put(?KEY, W#{now := Now + Dt}),
    feed([{tick, Now + Dt}], #{}).

%% The controller and its port die with their in-flight work and the reads the
%% port held. The committed wants survive.
restart_ctl() ->
    W = #{spec := Spec, now := Now} = get(?KEY),
    put(?KEY, W#{ctl := chryso_lifecycle:new(params()), pending := [], armed := [], history := []}),
    feed([{tick, Now} | [{want, C, T} || C := T <- Spec]], #{}).

flood(N) ->
    lists:foreach(
        fun(_) ->
            W = #{root := Root, now := Now} = get(?KEY),
            {_, _, Root1} = chryso_root:command(Root, ?SYNC, 4, [0, 0, 0, 0], Now, none),
            put(?KEY, W#{root := Root1})
        end,
        lists:seq(1, N)
    ),
    caused([], []).

%% --- after a run ---------------------------------------------------------------

%% Fair rounds (time passes, every deadline fires, Root is read, every command
%% is delivered), then the wants of drivers Root still runs that do not hold.
open_wants() ->
    lists:foreach(
        fun(_) ->
            _ = tick(500),
            [_ = deadline() || _ <- maps:get(armed, get(?KEY))],
            _ = scan(),
            deliver_all()
        end,
        lists:seq(1, 12)
    ),
    #{root := Root, last := Last, spec := Spec} = get(?KEY),
    Records = chryso_root:records(Root),
    [
        {C, T}
     || C := T <- Spec,
        maps:get(state, maps:get(C, Records)) =:= live,
        maps:get(C, Last, 0) < T
    ].

controller() ->
    maps:get(ctl, get(?KEY)).

last_restarts() ->
    maps:get(last, get(?KEY)).

%% Root's page as the orchestrator's adapter would decode it.
page() ->
    #{root := Root, now := Now, last := Last} = get(?KEY),
    #{records := Records, seq := Seq} = chryso_root:observe(Root),
    Row = fun(C) ->
        #{state := S, lifetime := L} = maps:get(C, Records),
        #{
            state => S,
            lifetime => L,
            budget => lists:nth(C + 1, ?BUDGETS),
            last_restart => maps:get(C, Last, 0)
        }
    end,
    {ok, K} = chryso_observe:from_page(#{
        root_generation => 1,
        beam_incarnation => 1,
        seq => Seq,
        now => Now,
        cntfrq => 1000,
        children => maps:from_list([{C, Row(C)} || C <- ?DRIVERS])
    }),
    K.

%% --- internals -----------------------------------------------------------------

deliver_all() ->
    case maps:get(pending, get(?KEY)) of
        [] ->
            ok;
        _ ->
            _ = deliver(),
            deliver_all()
    end.

%% Send the oldest pending command to Root, then deliver or lose its reply.
perform(Mode) ->
    W = get(?KEY),
    case maps:get(pending, W) of
        [] ->
            caused([], []);
        [{Ref, C, Fence, Want} | Rest] ->
            #{root := Root, now := Now, last := Last} = W,
            #{root_generation := G, seq := S, lifetime := L} = Fence,
            {Reply, D, Root1} = chryso_root:command(
                Root, ?RESTART_CHILD, 4, [C, G, S, L], Now, absent
            ),
            Accepted =
                case D of
                    #{action := resume} -> [{C, Want}];
                    _ -> []
                end,
            put(?KEY, W#{pending := Rest, root := Root1, last := resumed(D, Now, Last)}),
            case Mode of
                reply ->
                    Result = maps:get(result, Reply),
                    Limited = [{C, Fence} || Result =:= rate_limited],
                    add_accepted(Accepted, feed([{reply, Ref, Result}], #{resendable => Limited}));
                lost ->
                    caused([], Accepted)
            end
    end.

%% Feed the controller, route its effects, and report what the event caused.
feed(Inputs, Extra) ->
    W = #{ctl := Ctl} = get(?KEY),
    {Effects, Ctl1} = lists:foldl(
        fun(Input, {Acc, S}) ->
            {E, S1} = chryso_lifecycle:step(Input, S),
            {Acc ++ E, S1}
        end,
        {[], Ctl},
        Inputs
    ),
    #{spec := Spec, pending := P, armed := A} = W,
    New = [{Ref, C, F, maps:get(C, Spec, 0)} || {command, Ref, C, F} <- Effects],
    Armed = [Ref || {arm, Ref, _} <- Effects],
    Sent = maps:merge(maps:get(sent, W), maps:from_list([{Ref, {C, F}} || {Ref, C, F, _} <- New])),
    put(?KEY, W#{ctl := Ctl1, pending := P ++ New, armed := A ++ Armed, sent := Sent}),
    maps:merge(caused([{C, F} || {_, C, F, _} <- New], []), Extra).

caused(Emitted, Accepted) ->
    #{emitted => Emitted, accepted => Accepted, known => false, gone => [], resendable => []}.

add_accepted(Accepted, #{accepted := A} = Caused) ->
    Caused#{accepted := Accepted ++ A}.

resumed(#{action := resume, child := C}, Now, Last) -> Last#{C => Now};
resumed(_, _Now, Last) -> Last.

gone_on(K) ->
    [C || C <- ?DRIVERS, maps:get(state, chryso_observe:child(K, C)) =:= gone].

root_config() ->
    #{
        mapped => true,
        now => 1000,
        tokens => 4,
        refill => 400,
        hard_max => 64,
        entry => 16#200000,
        pp => 11,
        beam => 5,
        present => 16#2f,
        drivers => 16#0f,
        compiled => ?BUDGETS ++ [3, 2],
        generation => 1,
        incarnation => 1,
        seq => 2,
        abi => 1,
        cntfrq => 1000
    }.
