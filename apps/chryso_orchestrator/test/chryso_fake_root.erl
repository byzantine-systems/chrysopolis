-module(chryso_fake_root).
-moduledoc """
Test device: the shipped `chryso_root` model behind the `chryso_root_device`
callbacks, its view published as real status bytes. Ticks are microseconds
(`cntfrq` 1 MHz), so a want stamped `now + 1` never shares a tick with the
restart that satisfies it. Root's bucket refills one token per 1000 ticks, so
it never limits these tests. Controls fault a driver, tear every read while on, drop the next
reply after Root acted, and report every restart Root accepted from a command.
""".
-behaviour(chryso_root_device).
-behaviour(gen_server).

-include("orchestrator_abi.hrl").

-export([read_status/0, open_ctl/0, call/2, close/1]).
-export([start/0, stop/0, fault/1, tear/1, drop_next/0, accepted/0, budgets/0, last/0, states/0]).
-export([handle_info/2]).
-export([init/1, handle_call/3, handle_cast/2]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(NAME, ?MODULE).
%% Uneven, so the last driver is gone after two faults.
-define(BUDGETS, [3, 3, 2, 1]).
-define(DRIVERS, [0, 1, 2, 3]).

budgets() -> ?BUDGETS.

start() ->
    gen_server:start({local, ?NAME}, ?MODULE, [], []).

stop() ->
    gen_server:stop(?NAME).

fault(C) -> gen_server:call(?NAME, {fault, C}).
tear(On) -> gen_server:call(?NAME, {tear, On}).
drop_next() -> gen_server:call(?NAME, drop_next).
accepted() -> gen_server:call(?NAME, accepted).
%% Each driver's last restart tick, from a fault or a command.
last() -> gen_server:call(?NAME, last).
states() -> gen_server:call(?NAME, states).

%% --- chryso_root_device ---------------------------------------------------------

read_status() -> gen_server:call(?NAME, read).
open_ctl() -> {ok, make_ref()}.
call(_Handle, Command) -> gen_server:call(?NAME, {command, Command}).
close(_Handle) -> ok.

%% --- gen_server -----------------------------------------------------------------

init([]) ->
    Origin = erlang:monotonic_time(microsecond) - 1000,
    {ok, #{
        root => chryso_root:new(config(1000)),
        origin => Origin,
        last => #{},
        tear => false,
        drop => false,
        accepted => []
    }}.

handle_call(read, _From, #{tear := true} = S) ->
    {reply, {ok, chryso_status_bytes:tear(bytes(S))}, S};
handle_call(read, _From, S) ->
    {reply, {ok, bytes(S)}, S};
handle_call({fault, C}, _From, #{root := Root, last := Last} = S) ->
    Now = now(S),
    {D, Root1} = chryso_root:fault(Root, C, absent, Now),
    {reply, maps:get(action, D), S#{root := Root1, last := resumed(D, Now, Last)}};
handle_call({command, Bin}, _From, S) ->
    {ok, #{opcode := Op, args := Args}} = chryso_abi_codec:decode_ctl_command(Bin),
    #{root := Root, last := Last, accepted := Acc, drop := Drop} = S,
    Now = now(S),
    {#{result := R, values := V}, D, Root1} = chryso_root:command(Root, Op, 4, Args, Now, absent),
    Acc1 =
        case D of
            #{action := resume, child := C} -> [{C, Now} | Acc];
            _ -> Acc
        end,
    S1 = S#{root := Root1, last := resumed(D, Now, Last), accepted := Acc1, drop := false},
    Reply =
        case Drop of
            true ->
                {error, eio};
            false ->
                {ok,
                    chryso_abi_codec:ctl_reply_to_binary(#{
                        magic => ?CHRYSO_MAGIC_REPLY,
                        version => ?CHRYSO_ABI_VERSION,
                        result => chryso_abi_codec:ctl_result_value(R),
                        values => V
                    })}
        end,
    {reply, Reply, S1};
handle_call({tear, On}, _From, S) ->
    {reply, ok, S#{tear := On}};
handle_call(drop_next, _From, S) ->
    {reply, ok, S#{drop := true}};
handle_call(accepted, _From, #{accepted := Acc} = S) ->
    {reply, lists:reverse(Acc), S};
handle_call(last, _From, #{last := Last} = S) ->
    {reply, Last, S};
handle_call(states, _From, #{root := Root} = S) ->
    #{records := Records} = chryso_root:observe(Root),
    {reply, #{C => maps:get(state, maps:get(C, Records)) || C <- ?DRIVERS}, S}.

handle_cast(_Msg, S) ->
    {noreply, S}.

handle_info(_Msg, S) ->
    {noreply, S}.

%% --- internals ------------------------------------------------------------------

now(#{origin := Origin}) ->
    erlang:monotonic_time(microsecond) - Origin.

bytes(#{root := Root, last := Last} = S) ->
    #{records := Records, seq := Seq} = chryso_root:observe(Root),
    Row = fun(C) ->
        #{state := State, lifetime := L} = maps:get(C, Records),
        #{
            state => State,
            lifetime => L,
            budget => lists:nth(C + 1, ?BUDGETS),
            last_restart => maps:get(C, Last, 0)
        }
    end,
    chryso_status_bytes:encode(#{
        root_generation => 1,
        beam_incarnation => 1,
        seq => Seq,
        now => now(S),
        cntfrq => 1000000,
        children => maps:from_list([{C, Row(C)} || C <- ?DRIVERS])
    }).

resumed(#{action := resume, child := C}, Now, Last) -> Last#{C => Now};
resumed(_, _Now, Last) -> Last.

config(Now) ->
    #{
        mapped => true,
        now => Now,
        tokens => 32,
        refill => 1000,
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
        cntfrq => 1000000
    }.
