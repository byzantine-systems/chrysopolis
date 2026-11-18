-module(chryso_lifecycle_ctl).
-moduledoc """
The interpreter of `chryso_lifecycle`: one `gen_statem` state, `running`, in
which each event becomes `step` inputs and each effect an action. The phases
stay in the pure state, where the laws and the replay check them.

| Event | Step input |
|---|---|
| named timeout `scan` | `{tick, Now}`, then `{observed, K}` from one port read |
| named timeout `{deadline, Ref}` | `{deadline, Ref}` |
| info `{chryso_reply, Ref, Result}` | `{reply, Ref, Result}` |
| call `{want, Child, T}` | `{want, Child, T}` |

| Effect | Action |
|---|---|
| `{command, Ref, Child, Fence}` | `chryso_root_port:command/5` |
| `{arm, Ref, At}` | named timeout `{deadline, Ref}` at `At` |
| `{publish, Conditions}` | kept for `status`, logged |

Time is milliseconds since the controller started. Wants live in
`persistent_term`, so a controller restart keeps them. Every input and its
performed effects go to a `chryso_decisions` log that `replay/1` checks.
""".
-behaviour(
    gen_statem
).

-include_lib("kernel/include/logger.hrl").

-export([start_link/1, status/1, want/3, wanted/1, decisions/1, replay/1, scan/1]).
-export([callback_mode/0, init/1, running/3]).
-export_type([config/0, status/0]).

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

-doc "The conditions, each want's status, and the stamp of the page they come from.".
-type status() :: #{
    conditions := chryso_lifecycle:conditions(),
    wants := #{non_neg_integer() => chryso_lifecycle:want_status()},
    observed :=
        unknown
        | #{
            root_generation := non_neg_integer(),
            beam_incarnation := non_neg_integer(),
            seq := non_neg_integer()
        }
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

-doc "The conditions, each want's status, and the page they come from.".
-spec status(Timeout :: non_neg_integer()) -> status().
status(Timeout) ->
    gen_statem:call(?NAME, status, Timeout).

-doc """
Want `Child` restarted at or after Root tick `T`, or, with `next`, after the
newest page known. Returns the tick used.
""".
-spec want(Child :: non_neg_integer(), T :: non_neg_integer() | next, Timeout :: non_neg_integer()) ->
    {ok, non_neg_integer()} | {error, no_observation | not_watched}.
want(Child, T, Timeout) ->
    gen_statem:call(?NAME, {want, Child, T}, Timeout).

-doc "Per driver, the newest Root tick wanted.".
-spec wanted(Timeout :: non_neg_integer()) -> chryso_plan:wants().
wanted(Timeout) ->
    gen_statem:call(?NAME, wanted, Timeout).

-doc "The decision log, oldest first, and how many entries it dropped.".
-spec decisions(Timeout :: non_neg_integer()) -> {[chryso_decisions:entry()], non_neg_integer()}.
decisions(Timeout) ->
    gen_statem:call(?NAME, decisions, Timeout).

-doc "Replay the decision log through the pure step.".
-spec replay(Timeout :: non_neg_integer()) -> match | {mismatch, pos_integer()}.
replay(Timeout) ->
    gen_statem:call(?NAME, replay, Timeout).

-doc "Read Root now, outside the scan period.".
-spec scan(Timeout :: non_neg_integer()) -> ok.
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
    Reply = #{
        conditions => chryso_lifecycle:conditions(Ctl),
        wants => chryso_lifecycle:wants_status(Ctl),
        observed => Observed
    },
    {keep_state_and_data, [{reply, From, Reply}]};
running({call, From}, {want, Child, T}, Data) ->
    {Reply, Data1, Actions} = add_want(Child, T, Data),
    {keep_state, Data1, [{reply, From, Reply} | Actions]};
running({call, From}, wanted, #{ctl := Ctl}) ->
    {keep_state_and_data, [{reply, From, chryso_lifecycle:wants(Ctl)}]};
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
running(Type, Content, _Data) ->
    ?LOG_WARNING(#{what => unexpected, type => Type, content => Content}, ?META),
    keep_state_and_data.

%% --- internals ------------------------------------------------------------------

%% A read the port cannot answer is unknown, which the merge ignores.
-spec read(data()) -> {data(), [gen_statem:action()]}.
read(#{config := #{drivers := Drivers}} = Data) ->
    Known =
        try
            chryso_root_port:read(Drivers, ?READ_TIMEOUT)
        catch
            %% Only a failed call to the port: a timeout or a port that died.
            exit:{Reason, {gen_server, call, _}} ->
                ?LOG_WARNING(#{what => read_failed, reason => Reason}, ?META),
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
