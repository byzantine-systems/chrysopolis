-module(chryso_orch).
-moduledoc """
Probes for `apps/chryso_orchestrator`: the testbed's `Want`, `Rewant` and
`Look` steps. Each starts the application on first use, does its work, and
only then emits one framed run, so a frame never crosses a restart it asked
for (a want of the serial driver restarts the console path itself). Root's own
`ROOT|ctl-restart` line, which does not pass through that driver, is the
authority for what a want did.

Results are data in the frame, never a crash: an error from the orchestrator
is reported for the testbed to judge. Waits poll the status page, a bounded
number of times, since Root publishes no completion notification.
""".

-export([want/2, rewant/2, look/2, ensure/0]).

-define(ABI_EBIN, "/lib/chryso_abi/ebin").
-define(ORCH_EBIN, "/lib/chryso_orchestrator/ebin").
-define(ENV, [
    {settle_ms, 0},
    {floor, 0},
    {scan_ms, 250},
    {deadline_ms, 1000}
]).
%% A want waits up to ROUNDS reads, PAUSE ms apart. A rewant reads QUIET times.
-define(ROUNDS, 40).
-define(QUIET, 4).
-define(PAUSE, 100).

-doc """
Ask for one restart of `Child` after the page read now, then read until the
want holds or Root gave the driver up. `outcome` is `held`, `gone`,
`timeout`, or the orchestrator's error.
""".
-spec want(atom(), non_neg_integer()) -> ok.
want(Tag, Child) ->
    ok = ensure(),
    ok = chryso_orchestrator:scan(),
    Outcome =
        case chryso_orchestrator:want(Child) of
            {ok, _Tick} -> poll(fun() -> settled(Child) end, ?ROUNDS, timeout);
            {error, Reason} -> Reason
        end,
    emit("ORCH_WANT.", Tag, [{want, [{child, Child}, {outcome, Outcome}]}]).

-doc """
Apply again the newest want made for `Child` since the application started,
the same Root tick word for word, then read a few times. `applied` is
`false` when there was none, `true` when it was applied, or the
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
    {Condition, Reason} = condition(Child),
    emit("ORCH_LOOK.", Tag, [
        {look, [{child, Child}, {condition, Condition}, {reason, Reason}]}
    ]).

-doc """
Start the application if it is not running. ERTS boots `-mode embedded`, so
every module the `.app` lists, and the codec, is loaded by name first. The
orchestrator's own log domain is kept off the console, where a notice could
cut into a frame. Idempotent.
""".
-spec ensure() -> ok.
ensure() ->
    true = code:add_patha(?ABI_EBIN),
    true = code:add_patha(?ORCH_EBIN),
    ok = loaded(application:load(chryso_orchestrator)),
    {ok, Modules} = application:get_key(chryso_orchestrator, modules),
    lists:foreach(fun load/1, [chryso_abi_codec | Modules]),
    ok = quiet_domain(),
    lists:foreach(fun({K, V}) -> ok = application:set_env(chryso_orchestrator, K, V) end, ?ENV),
    {ok, _Started} = application:ensure_all_started(chryso_orchestrator),
    ok.

%% --- internals ------------------------------------------------------------------

%% Embedded mode loads nothing on demand, and code:ensure_loaded/1 refuses
%% there with {error, embedded}. Only code:load_file/1 loads by name.
-spec load(module()) -> ok.
load(M) ->
    case code:is_loaded(M) of
        {file, _} ->
            ok;
        false ->
            {module, M} = code:load_file(M),
            ok
    end.

-spec loaded(ok | {error, term()}) -> ok.
loaded(ok) -> ok;
loaded({error, {already_loaded, chryso_orchestrator}}) -> ok.

-spec quiet_domain() -> ok.
quiet_domain() ->
    Filter = {fun logger_filters:domain/2, {stop, sub, [chryso, orchestrator]}},
    case logger:add_primary_filter(chryso_orch_quiet, Filter) of
        ok -> ok;
        {error, {already_exist, chryso_orch_quiet}} -> ok
    end.

-spec condition(non_neg_integer()) -> {chryso_condition:condition(), chryso_condition:reason()}.
condition(Child) ->
    #{conditions := Conditions} = chryso_orchestrator:status(),
    maps:get(Child, Conditions).

%% A want holds once the driver is neither waiting for its restart nor
%% unknown: an unknown page is not evidence either way.
-spec settled(non_neg_integer()) -> held | gone | continue.
settled(Child) ->
    case condition(Child) of
        {gone, _} -> gone;
        {restarting, _} -> continue;
        {unknown, _} -> continue;
        _ -> held
    end.

%% Read Root, ask `Done`, and pause, at most `Rounds` times. `Done` returns
%% `continue` to go on or the result to stop with. `Exhausted` is the result
%% when the rounds run out.
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

%% Read Root `Rounds` times, a pause apart, so any command the reads cause
%% has run before the caller reports.
-spec read_for(non_neg_integer()) -> ok.
read_for(Rounds) ->
    poll(fun() -> continue end, Rounds, ok).

-spec emit(string(), atom(), [chryso_frame:record()]) -> ok.
emit(Prefix, Tag, Records) ->
    chryso_test:emit_run(Prefix ++ chryso_test:tag(Tag), Records, 0).
