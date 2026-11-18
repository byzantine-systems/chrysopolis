-module(chryso_orch_flood).
-moduledoc """
SYNC on `/dev/chrysoctl`: one call (`sync/0`, which also reads Root's command
tallies) or a bounded flood that drains Root's command bucket. Each call
opens the device, sends one SYNC and closes it, so the orchestrator's port
gets the device between calls.

The worker is a two-state loop, `flooding` then `done`, left when the stop
flag is set or its time budget runs out, so it ends by itself. Control and
observation never pass through its mailbox: `tally/1` reads shared
`counters`, and `stop/1` sets an `atomics` flag and waits on a monitor.
""".

-include("orchestrator_abi.hrl").

-export([start/1, tally/1, stop/1, sync/0]).
-export_type([flood/0, tally/0]).

-define(CTL, "/dev/chrysoctl").
-define(STOP_WAIT, 5000).
%% Counter slots.
-define(SENT, 1).
-define(LIMITED, 2).
-define(BUSY, 3).
-define(OTHER, 4).

-doc "A running or finished flood.".
-opaque flood() :: #{
    pid := pid(), counts := counters:counters_ref(), stop := atomics:atomics_ref()
}.

-doc "Rounds by what Root answered, or `busy` when the device was held.".
-type tally() :: #{
    sent := non_neg_integer(),
    limited := non_neg_integer(),
    busy := non_neg_integer(),
    other := non_neg_integer()
}.

-doc """
One SYNC. Root's reply carries its tallies of every command it has counted:
accepted, rate limited, and rejected. `busy` when another opener holds the
device.
""".
-spec sync() ->
    {ok, ok | rate_limited, #{
        accepted := non_neg_integer(), limited := non_neg_integer(), rejected := non_neg_integer()
    }}
    | busy
    | failed.
sync() ->
    case file:open(?CTL, [read, write, raw, binary]) of
        {ok, F} ->
            Result = call(F),
            ok = file:close(F),
            Result;
        {error, ebusy} ->
            busy;
        {error, _} ->
            failed
    end.

-doc "Flood for at most `Ms` milliseconds, linked to the caller.".
-spec start(pos_integer()) -> flood().
start(Ms) ->
    Counts = counters:new(4, []),
    Stop = atomics:new(1, []),
    Deadline = erlang:monotonic_time(millisecond) + Ms,
    Pid = spawn_link(fun() -> flooding(Counts, Stop, Deadline) end),
    #{pid => Pid, counts => Counts, stop => Stop}.

-doc "The counts so far, read without messaging the worker.".
-spec tally(flood()) -> tally().
tally(#{counts := C}) ->
    #{
        sent => counters:get(C, ?SENT),
        limited => counters:get(C, ?LIMITED),
        busy => counters:get(C, ?BUSY),
        other => counters:get(C, ?OTHER)
    }.

-doc "Stop the flood, wait for the worker to end, and return the final counts.".
-spec stop(flood()) -> tally().
stop(#{pid := Pid, stop := Stop} = Flood) ->
    Ref = monitor(process, Pid),
    ok = atomics:put(Stop, 1, 1),
    receive
        {'DOWN', Ref, process, Pid, _} -> ok
    after ?STOP_WAIT -> error(flood_did_not_stop)
    end,
    tally(Flood).

%% --- the worker ------------------------------------------------------------------

-spec flooding(counters:counters_ref(), atomics:atomics_ref(), integer()) -> ok.
flooding(Counts, Stop, Deadline) ->
    case atomics:get(Stop, 1) =:= 1 orelse erlang:monotonic_time(millisecond) >= Deadline of
        true ->
            ok;
        false ->
            ok = counters:add(Counts, slot(round()), 1),
            flooding(Counts, Stop, Deadline)
    end.

-spec slot(busy | rate_limited | answered | failed) -> pos_integer().
slot(busy) -> ?BUSY;
slot(rate_limited) -> ?LIMITED;
slot(answered) -> ?SENT;
slot(failed) -> ?OTHER.

-spec round() -> busy | rate_limited | answered | failed.
round() ->
    case sync() of
        {ok, rate_limited, _} -> rate_limited;
        {ok, ok, _} -> answered;
        Other -> Other
    end.

-spec call(file:io_device()) ->
    {ok, ok | rate_limited, #{
        accepted := non_neg_integer(), limited := non_neg_integer(), rejected := non_neg_integer()
    }}
    | failed.
call(F) ->
    Command = chryso_abi_codec:ctl_command_to_binary(#{
        magic => ?CHRYSO_MAGIC_COMMAND,
        version => ?CHRYSO_ABI_VERSION,
        opcode => ?CHRYSO_PP_OPCODE_SYNC,
        args => [0, 0, 0, 0]
    }),
    maybe
        ok ?= file:write(F, Command),
        {ok, Reply} ?= file:read(F, ?CHRYSO_CTL_COMMAND_SIZE),
        {ok, #{result := Value, values := [_Seq, A, L, R]}} ?=
            chryso_abi_codec:decode_ctl_reply(Reply),
        {ok, Result} ?= chryso_abi_codec:ctl_result_to_atom(Value),
        true ?= Result =:= ok orelse Result =:= rate_limited,
        {ok, Result, #{accepted => A, limited => L, rejected => R}}
    else
        _ -> failed
    end.
