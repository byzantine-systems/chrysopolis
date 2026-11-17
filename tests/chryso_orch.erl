-module(chryso_orch).
-moduledoc """
Probes for `apps/chryso_orchestrator`, one per testbed step: `want/3`,
`rewant/2`, `look/2`, and `replay/1` for the in-situ audit. Each starts the
application on first use and emits one framed run after its work, so a frame
never crosses a restart it asked for. Results are data in the frame, never a
crash. Waits poll Root a bounded number of times, since Root publishes no
completion notification.
""".

-export([want/3, rewant/2, look/2, replay/1, ensure/0]).

-define(APPS, [
    {chryso_core, "/lib/chryso_core/ebin"},
    {chryso_abi, "/lib/chryso_abi/ebin"},
    {chryso_orchestrator, "/lib/chryso_orchestrator/ebin"}
]).
-define(ENV, [
    {settle_ms, 0},
    {floor, 0},
    {scan_ms, 250},
    {deadline_ms, 1000}
]).
%% Waits read up to ROUNDS times, PAUSE ms apart. QUIET reads let a command
%% the reads caused run before the probe reports. A flood ends by itself after
%% FLOOD_MS even if nothing stops it.
-define(ROUNDS, 40).
-define(FLOOD_MS, 10000).
-define(QUIET, 4).
-define(PAUSE, 100).

-doc """
Ask for one restart of `Child` after the page read now. With `Flood`, a
`chryso_orch_flood` is rate limited by Root first, runs while the want is
made, and is stopped before the want settles. `outcome` is `held`, `failed`,
`gone`, `timeout`, or the orchestrator's error. `flood_limited` counts the
flood's rate-limited rounds. `asked` is how many commands Root counted
during the want, not counting the probe's own SYNCs (and the flood's, when
flooded): Root's own evidence of whether it was asked at all.
""".
-spec want(atom(), non_neg_integer(), boolean()) -> ok.
want(Tag, Child, Flood) ->
    ok = ensure(),
    ok = chryso_orchestrator:scan(),
    {Before, _} = tallies(),
    Flooder = flood(Flood),
    Requested = chryso_orchestrator:want(Child),
    ok = read_for(?QUIET),
    #{limited := Limited} = Rounds = unflood(Flooder),
    Outcome =
        case Requested of
            {ok, _Tick} -> poll(fun() -> settled(Child) end, ?ROUNDS, timeout);
            {error, Reason} -> Reason
        end,
    {After, Retries} = tallies(),
    %% The delta holds one of the two measuring SYNCs, whichever way Root
    %% counts the call it is replying to, plus the retries of the second one
    %% and every flood round that reached Root.
    Asked = After - Before - 1 - Retries - reached(Rounds),
    emit("ORCH_WANT.", Tag, [
        {want, [{child, Child}, {outcome, Outcome}, {flood_limited, Limited}, {asked, Asked}]}
    ]).

-doc """
Apply again the newest want made for `Child` since the application started,
word for word. `applied` is `false` when there was none, `true`, or the
orchestrator's error.
""".
-spec rewant(atom(), non_neg_integer()) -> ok.
rewant(Tag, Child) ->
    ok = ensure(),
    Applied =
        case maps:find(Child, chryso_orchestrator:wanted()) of
            error ->
                false;
            {ok, Tick} ->
                case chryso_orchestrator:want(Child, Tick) of
                    {ok, _} -> true;
                    {error, Reason} -> Reason
                end
        end,
    ok = read_for(?QUIET),
    emit("ORCH_REWANT.", Tag, [{rewant, [{child, Child}, {applied, Applied}]}]).

-doc "Read Root, then report the orchestrator's condition for `Child`.".
-spec look(atom(), non_neg_integer()) -> ok.
look(Tag, Child) ->
    ok = ensure(),
    ok = chryso_orchestrator:scan(),
    #{conditions := #{Child := {Condition, Reason}}} = chryso_orchestrator:status(),
    emit("ORCH_LOOK.", Tag, [
        {look, [{child, Child}, {condition, Condition}, {reason, Reason}]}
    ]).

-doc """
Replay the decision log through the pure step. `verdict` is `match` or
`mismatch` (at `index`), `dropped` counts entries the bound let go, and
`limited` counts the `rate_limited` replies the log still holds.
""".
-spec replay(atom()) -> ok.
replay(Tag) ->
    ok = ensure(),
    {Entries, Dropped} = chryso_orchestrator:decisions(),
    {Verdict, Index} =
        case chryso_orchestrator:replay() of
            match -> {match, 0};
            {mismatch, I} -> {mismatch, I}
        end,
    Limited = length([R || {{reply, _, rate_limited} = R, _} <- Entries]),
    emit("ORCH_REPLAY.", Tag, [
        {replay, [{verdict, Verdict}, {index, Index}, {dropped, Dropped}, {limited, Limited}]}
    ]).

-doc """
Start the application and its declared dependencies if they are not running.
Embedded mode loads nothing on demand, so each application's listed modules
are loaded by name first. The orchestrator's log domain is kept off the
console handler, where it could cut into a frame. Idempotent.
""".
-spec ensure() -> ok.
ensure() ->
    lists:foreach(fun load_app/1, ?APPS),
    ok = quiet_domain(),
    lists:foreach(fun({K, V}) -> ok = application:set_env(chryso_orchestrator, K, V) end, ?ENV),
    {ok, _Started} = application:ensure_all_started(chryso_orchestrator),
    ok.

%% --- internals ------------------------------------------------------------------

%% code:ensure_loaded/1 refuses in embedded mode. Only load_file/1 loads.
-spec load(module()) -> ok.
load(M) ->
    case code:is_loaded(M) of
        {file, _} ->
            ok;
        false ->
            {module, M} = code:load_file(M),
            ok
    end.

-spec load_app({atom(), file:filename()}) -> ok.
load_app({App, Ebin}) ->
    true = code:add_patha(Ebin),
    case application:load(App) of
        ok -> ok;
        {error, {already_loaded, App}} -> ok
    end,
    {ok, Modules} = application:get_key(App, modules),
    lists:foreach(fun load/1, Modules).

%% A filter on the console handler only, so another handler would still get
%% the orchestrator's events.
-spec quiet_domain() -> ok.
quiet_domain() ->
    Filter = {fun logger_filters:domain/2, {stop, sub, [chryso, orchestrator]}},
    case logger:add_handler_filter(default, chryso_orch_quiet, Filter) of
        ok -> ok;
        {error, {already_exist, chryso_orch_quiet}} -> ok
    end.

%% A flood that Root has already rate limited, or none.
-spec flood(boolean()) -> none | chryso_orch_flood:flood().
flood(false) ->
    none;
flood(true) ->
    Flood = chryso_orch_flood:start(?FLOOD_MS),
    _ = poll(fun() -> limited(Flood) end, ?ROUNDS, false),
    Flood.

-spec limited(chryso_orch_flood:flood()) -> true | continue.
limited(Flood) ->
    case chryso_orch_flood:tally(Flood) of
        #{limited := 0} -> continue;
        #{} -> true
    end.

-spec unflood(none | chryso_orch_flood:flood()) -> chryso_orch_flood:tally().
unflood(none) ->
    #{sent => 0, limited => 0, busy => 0, other => 0};
unflood(Flood) ->
    chryso_orch_flood:stop(Flood).

%% Flood rounds Root answered, so counted.
-spec reached(chryso_orch_flood:tally()) -> non_neg_integer().
reached(#{sent := Sent, limited := Limited}) ->
    Sent + Limited.

%% Every command Root has counted, through one SYNC, and how many of the
%% probe's tries Root rate limited first. A held device is retried.
-spec tallies() -> {non_neg_integer(), non_neg_integer()}.
tallies() ->
    tallies(?ROUNDS, 0).

-spec tallies(non_neg_integer(), non_neg_integer()) -> {non_neg_integer(), non_neg_integer()}.
tallies(0, _Retries) ->
    error(tallies_unavailable);
tallies(Tries, Retries) ->
    case chryso_orch_flood:sync() of
        {ok, ok, #{accepted := A, limited := L, rejected := R}} ->
            {A + L + R, Retries};
        {ok, rate_limited, _} ->
            timer:sleep(?PAUSE),
            tallies(Tries - 1, Retries + 1);
        busy ->
            timer:sleep(?PAUSE),
            tallies(Tries - 1, Retries);
        failed ->
            error(tallies_failed)
    end.

%% The want's status, as the orchestrator reports it. Gone ends the wait too:
%% a want of a gone driver can never hold.
-spec settled(non_neg_integer()) -> held | failed | gone | continue.
settled(Child) ->
    #{conditions := Conditions, wants := Wants} = chryso_orchestrator:status(),
    case {maps:get(Child, Conditions), maps:get(Child, Wants, open)} of
        {{gone, _}, _} -> gone;
        {_, held} -> held;
        {_, failed} -> failed;
        {_, open} -> continue
    end.

%% Read Root, ask `Done`, and pause, at most `Rounds` times. `Done` returns
%% `continue` or the result to stop with, and `Exhausted` is the result when
%% the rounds run out.
-spec poll(fun(() -> continue | Result), non_neg_integer(), Result) -> Result.
poll(_Done, 0, Exhausted) ->
    Exhausted;
poll(Done, Rounds, Exhausted) ->
    ok = chryso_orchestrator:scan(),
    case Done() of
        continue ->
            timer:sleep(?PAUSE),
            poll(Done, Rounds - 1, Exhausted);
        Result ->
            Result
    end.

-spec read_for(non_neg_integer()) -> ok.
read_for(Rounds) ->
    poll(fun() -> continue end, Rounds, ok).

-spec emit(string(), atom(), [chryso_frame:record()]) -> ok.
emit(Prefix, Tag, Records) ->
    chryso_test:emit_run(Prefix ++ chryso_test:tag(Tag), Records, 0).
