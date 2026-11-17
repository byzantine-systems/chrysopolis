-module(chryso_lifecycle_ctl).
-moduledoc """
The interpreter of `chryso_lifecycle`. Each event becomes one or two `step`
inputs, and each effect becomes an action:

| Effect | Action |
|---|---|
| `{command, Ref, Child, Fence}` | `chryso_root_port:command/5`, with the deadline of the matching `arm` |
| `{arm, Ref, At}` | a named timeout `{deadline, Ref}` at `At` |
| `{publish, Conditions}` | stored for `status`, and logged |

The scan is a named timeout too, and a full read is the correctness path.
Named timeouts, because the scan and each command's deadline run at once and a
`state_timeout` is a single timer. Time is milliseconds since the controller
started, so the machine never sees a negative monotonic time.

There is one gen_statem state, `running`. The lifecycle's phases stay in the
pure state, where the laws and the replay check them, not in a second copy
here.

Wants are kept in `persistent_term`, so a controller restarted by the port's
crash keeps them, and a BEAM restart forgets them. Every input and the
effects performed for it go to a `chryso_decisions` log, which `replay/0`
checks against the pure step.
""".
-behaviour(gen_statem).

-include_lib("kernel/include/logger.hrl").

-export([start_link/1, status/1, want/3, decisions/1, replay/1, scan/1]).
-export([callback_mode/0, init/1, running/3]).
-export_type([config/0]).

-define(NAME, ?MODULE).
-define(WANTS, {chryso_orchestrator, wants}).
-define(META, #{domain => [chryso, orchestrator]}).
-define(READ_TIMEOUT, 2000).
-define(RING, 64).

-doc "The lifecycle parameters plus the scan period.".
-type config() :: #{
    drivers := [non_neg_integer()],
    deadline_ms := pos_integer(),
    backoff_ms := {pos_integer(), pos_integer()},
    settle_ms := non_neg_integer(),
    floor := non_neg_integer(),
    scan_ms := pos_integer()
}.

-type data() :: #{
    config := config(),
    origin := integer(),
    ctl := chryso_lifecycle:state(),
    ring := chryso_decisions:ring()
}.

-doc "Start the controller.".
-spec start_link(Config :: config()) -> gen_statem:start_ret().
start_link(Config) ->
    gen_statem:start_link({local, ?NAME}, ?MODULE, Config, []).

-doc "The conditions and the page they come from.".
-spec status(Timeout :: timeout()) -> map().
status(Timeout) ->
    gen_statem:call(?NAME, status, Timeout).

-doc """
Want `Child` restarted at or after Root tick `T`, or, with `next`, after the
newest page known. Returns the tick used.
""".
-spec want(Child :: non_neg_integer(), T :: non_neg_integer() | next, Timeout :: timeout()) ->
    {ok, non_neg_integer()} | {error, no_observation | not_watched}.
want(Child, T, Timeout) ->
    gen_statem:call(?NAME, {want, Child, T}, Timeout).

-doc "The decision log, oldest first, and how many entries it dropped.".
-spec decisions(Timeout :: timeout()) -> {[chryso_decisions:entry()], non_neg_integer()}.
decisions(Timeout) ->
    gen_statem:call(?NAME, decisions, Timeout).

-doc "Replay the decision log through the pure step.".
-spec replay(Timeout :: timeout()) -> match | {mismatch, pos_integer()}.
replay(Timeout) ->
    gen_statem:call(?NAME, replay, Timeout).

-doc "Read Root now, outside the scan period.".
-spec scan(Timeout :: timeout()) -> ok.
scan(Timeout) ->
    gen_statem:call(?NAME, scan, Timeout).

%% --- callbacks ------------------------------------------------------------------

-doc false.
-spec callback_mode() -> state_functions.
callback_mode() ->
    state_functions.

-doc false.
-spec init(config()) -> {ok, running, data(), [gen_statem:action()]}.
init(Config) ->
    Params = maps:with([drivers, deadline_ms, backoff_ms, settle_ms, floor], Config),
    Ctl = chryso_lifecycle:new(Params),
    Data0 = #{
        config => Config,
        origin => erlang:monotonic_time(millisecond),
        ctl => Ctl,
        ring => chryso_decisions:new(?RING, Ctl)
    },
    %% Committed wants come back before any read, so nothing is planned for
    %% them until a page is known.
    Wants = [{want, C, T} || C := T <- persistent_term:get(?WANTS, #{})],
    {Data, Actions} = feed([{tick, 0} | lists:sort(Wants)], Data0),
    {ok, running, Data, [{{timeout, scan}, 0, scan} | Actions]}.

-doc false.
-spec running(gen_statem:event_type(), term(), data()) ->
    gen_statem:event_handler_result(running).
running({timeout, scan}, scan, #{config := #{scan_ms := Period}} = Data) ->
    {Data1, Actions} = read(Data),
    {keep_state, Data1, [{{timeout, scan}, Period, scan} | Actions]};
running({timeout, {deadline, Ref}}, Ref, Data) ->
    {Data1, Actions} = feed([{deadline, Ref}], Data),
    {keep_state, Data1, Actions};
running(info, {chryso_reply, Ref, Result}, Data) ->
    {Data1, Actions} = feed([{reply, Ref, Result}], Data),
    {keep_state, Data1, Actions};
running({call, From}, status, #{ctl := Ctl}) ->
    Observed =
        case chryso_observe:view(chryso_lifecycle:known(Ctl)) of
            unknown ->
                unknown;
            #{root_generation := G, beam_incarnation := I, seq := S} ->
                #{root_generation => G, beam_incarnation => I, seq => S}
        end,
    Reply = #{conditions => chryso_lifecycle:conditions(Ctl), observed => Observed},
    {keep_state_and_data, [{reply, From, Reply}]};
running({call, From}, {want, Child, T}, Data) ->
    {Reply, Data1, Actions} = add_want(Child, T, Data),
    {keep_state, Data1, [{reply, From, Reply} | Actions]};
running({call, From}, decisions, #{ring := Ring}) ->
    Reply = {chryso_decisions:entries(Ring), chryso_decisions:dropped(Ring)},
    {keep_state_and_data, [{reply, From, Reply}]};
running({call, From}, replay, #{ring := Ring}) ->
    {keep_state_and_data, [{reply, From, chryso_decisions:replay(Ring)}]};
running({call, From}, scan, Data) ->
    {Data1, Actions} = read(Data),
    {keep_state, Data1, [{reply, From, ok} | Actions]};
running({call, From}, _Other, _Data) ->
    {keep_state_and_data, [{reply, From, {error, unknown_call}}]};
running(_Type, _Content, _Data) ->
    keep_state_and_data.

%% --- internals ------------------------------------------------------------------

%% A read the port cannot answer is unknown, which the merge ignores.
-spec read(data()) -> {data(), [gen_statem:action()]}.
read(#{config := #{drivers := Drivers}} = Data) ->
    Known =
        try
            chryso_root_port:read(Drivers, ?READ_TIMEOUT)
        catch
            exit:Reason ->
                ?LOG_NOTICE(#{what => read_failed, reason => Reason}, ?META),
                chryso_observe:unknown()
        end,
    feed([{tick, now(Data)}, {observed, Known}], Data).

-spec add_want(non_neg_integer(), non_neg_integer() | next, data()) ->
    {{ok, non_neg_integer()} | {error, no_observation | not_watched}, data(), [gen_statem:action()]}.
add_want(Child, T, #{config := #{drivers := Drivers}, ctl := Ctl} = Data) ->
    Stamp =
        case {T, chryso_observe:view(chryso_lifecycle:known(Ctl))} of
            {next, unknown} -> {error, no_observation};
            {next, #{now := Now}} -> {ok, Now + 1};
            {Tick, _} -> {ok, Tick}
        end,
    case {lists:member(Child, Drivers), Stamp} of
        {false, _} ->
            {{error, not_watched}, Data, []};
        {true, {error, _} = Error} ->
            {Error, Data, []};
        {true, {ok, Tick1} = Ok} ->
            Wants = persistent_term:get(?WANTS, #{}),
            persistent_term:put(?WANTS, Wants#{Child => max(Tick1, maps:get(Child, Wants, 0))}),
            {Data1, Actions} = feed([{want, Child, Tick1}], Data),
            {Ok, Data1, Actions}
    end.

%% Step each input, perform its effects and log both.
-spec feed([chryso_lifecycle:input()], data()) -> {data(), [gen_statem:action()]}.
feed(Inputs, Data) ->
    lists:foldl(
        fun(Input, {#{ctl := Ctl, ring := Ring} = D, Acc}) ->
            {Effects, Ctl1} = chryso_lifecycle:step(Input, Ctl),
            {Performed, Actions} = perform(Effects, D),
            {
                D#{ctl := Ctl1, ring := chryso_decisions:log(Input, Performed, Ctl1, Ring)},
                Acc ++ Actions
            }
        end,
        {Data, []},
        Inputs
    ).

-spec perform([chryso_lifecycle:effect()], data()) ->
    {[chryso_lifecycle:effect()], [gen_statem:action()]}.
perform(Effects, #{origin := Origin}) ->
    Deadlines = #{Ref => At || {arm, Ref, At} <- Effects},
    Actions = lists:flatmap(
        fun
            ({command, Ref, Child, Fence}) ->
                At = Origin + maps:get(Ref, Deadlines),
                ok = chryso_root_port:command(Ref, Child, Fence, At, self()),
                [];
            ({arm, Ref, At}) ->
                [{{timeout, {deadline, Ref}}, Origin + At, Ref, [{abs, true}]}];
            ({publish, Conditions}) ->
                ?LOG_INFO(#{what => conditions, conditions => Conditions}, ?META),
                []
        end,
        Effects
    ),
    {Effects, Actions}.

-spec now(data()) -> non_neg_integer().
now(#{origin := Origin}) ->
    max(0, erlang:monotonic_time(millisecond) - Origin).
