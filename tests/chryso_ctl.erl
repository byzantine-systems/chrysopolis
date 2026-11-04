-module(chryso_ctl).
-moduledoc """
Probes for Root's BEAM devices, `/dev/chrysostat` and `/dev/chrysoctl`
(`src/pd/beam/control/runtime_control_dev.c`).

Records are built and read through the generated `orchestrator_abi.hrl`
offsets, never a hand-written layout, so an ABI change cannot leave these
probes reading the wrong bytes. Each probe prints one framed run
(`chryso_test:emit_run/3`) holding its results as records, which the
testbed reads: a `step` record per check with its verdict, then the run's
verdict frame counting the unexpected ones.

`ctl_script/2` and `latency/3` also audit Root in place. Around the run they
read Root's SYNC tallies and the status page. They then fold every fault
injected and every accepted restart through `chryso_root_policy` (the
shipped model in `apps/chryso_core`) and compare the result with Root's
rows. Each PPC reply is classified the way Root tallies it and compared with
Root's counters. The verdict is an `audit` record with `rows=match`,
`tallies=match` and `gap=G`, where `G` counts ring events lost between the
two reads. A gap never enters the row comparison,
which uses only the probe's own inputs.
""".

-include("orchestrator_abi.hrl").

-export([stat/2, stat_errors/1, ctl_script/2, reconcile/5, latency/3, bench/2]).

-define(STAT, "/dev/chrysostat").
-define(CTL, "/dev/chrysoctl").
-define(PAGE, ?CHRYSO_ROOT_STATUS_PAGE_SIZE).
-define(RECORD, ?CHRYSO_CTL_COMMAND_SIZE).
%% Retries for a torn copy (eagain) before a status read gives up.
-define(STAT_TRIES, 5).

-type page() :: binary().
-type observed() :: #{
    generation := non_neg_integer(),
    incarnation := non_neg_integer(),
    seq := non_neg_integer(),
    count := non_neg_integer()
}.
%% Root's command counters, or the probe's calls in the same classes.
-type tally() :: #{
    accepted := non_neg_integer(),
    limited := non_neg_integer(),
    rejected := non_neg_integer()
}.
-type window() :: #{page := page(), tallies := tally()}.
-type outcome() :: {0 | 1, chryso_frame:record()}.

-doc """
Print `CTL_STAT|<TAG>|incarnation=I|seq=S|count=C` from one status snapshot,
where `count` is `Child`'s lifetime restart count.
""".
-spec stat(atom(), non_neg_integer()) -> ok.
stat(Tag, Child) ->
    Name = chryso_test:tag(Tag),
    Fields =
        case read_page() of
            {ok, Page} ->
                #{incarnation := I, seq := S, count := C} = observe(Page, Child),
                [{incarnation, I}, {seq, S}, {count, C}];
            {error, Reason} ->
                [{error, Reason}]
        end,
    chryso_test:emit_run("CTL_STAT." ++ Name, [{stat, Fields}], 0).

-doc "Exercise the status device's local failures. None involves Root.".
-spec stat_errors(atom()) -> ok.
stat_errors(Tag) ->
    Name = chryso_test:tag(Tag),
    {ok, Short} = file:open(?STAT, [read, raw, binary]),
    U1 = step(short_read, {error, emsgsize}, file:read(Short, ?PAGE - 1)),
    U2 = step(write_readonly, {error, ebadf}, file:write(Short, <<0>>)),
    ok = file:close(Short),
    {ok, Once} = file:open(?STAT, [read, raw, binary]),
    {ok, _} = file:read(Once, ?PAGE),
    U3 = step(second_read, eof, file:read(Once, ?PAGE)),
    ok = file:close(Once),
    U4 = step(open_write, {error, eacces}, file:open(?STAT, [write, raw])),
    done("CTL_STAT_ERR." ++ Name, [U1, U2, U3, U4], []).

-doc """
Drive the command device against Root, restarting `Child` once.

Local rejects are decided before any PPC. Root's typed outcomes arrive in
replies. Ends by closing with a reply pending and checking that the next
opener cannot see it.
""".
-spec ctl_script(atom(), non_neg_integer()) -> ok.
ctl_script(Tag, Child) ->
    Name = chryso_test:tag(Tag),
    Window = audit_open(),
    {ok, F} = file:open(?CTL, [read, write, raw, binary]),
    U1 = step(second_open, {error, ebusy}, file:open(?CTL, [read, write, raw, binary])),
    U2 = step(read_idle, {error, enodata}, file:read(F, ?RECORD)),
    Hello = call(F, command(?CHRYSO_PP_OPCODE_HELLO, [?CHRYSO_ABI_VERSION, 0, 0, 0])),
    U3 = step(
        hello,
        {?CHRYSO_CTL_RESULT_OK, ?CHRYSO_ABI_VERSION},
        case Hello of
            {ok, Result, [Version | _]} -> {Result, Version};
            Other -> Other
        end
    ),
    U4 = step(short_write, {error, emsgsize}, file:write(F, <<0:(8 * (?RECORD - 1))>>)),
    U5 = step(
        bad_magic,
        {error, ebadmsg},
        file:write(F, put_bin(command(?CHRYSO_PP_OPCODE_HELLO, [0, 0, 0, 0]), 0, <<"XXXXXXXX">>))
    ),
    U6 = step(bad_opcode, {error, ebadmsg}, file:write(F, command(99, [0, 0, 0, 0]))),
    {ok, Before} = read_page(),
    #{generation := Gen, seq := Seq, count := Count} = observe(Before, Child),
    Stale = call(F, command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, Seq + 2, Count])),
    U7 = step(stale, ?CHRYSO_CTL_RESULT_STALE, result(Stale)),
    {ok, Fresh} = read_page(),
    #{seq := Seq1, count := Count1} = observe(Fresh, Child),
    Restart = command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, Seq1, Count1]),
    First = call(F, Restart),
    chryso_test:settle_driver(),
    U8 = step(
        restart,
        {?CHRYSO_CTL_RESULT_OK, Count1 + 1},
        case First of
            {ok, Result1, [_, _, Lifetime, _]} -> {Result1, Lifetime};
            Other1 -> Other1
        end
    ),
    Retry = call(F, Restart),
    U9 = step(retry, ?CHRYSO_CTL_RESULT_ALREADY_DONE, result(Retry)),
    ok = file:write(F, command(?CHRYSO_PP_OPCODE_SYNC, [0, 0, 0, 0])),
    ok = file:close(F),
    {ok, G} = file:open(?CTL, [read, write, raw, binary]),
    U10 = step(dropped_reply, {error, enodata}, file:read(G, ?RECORD)),
    ok = file:close(G),
    Steps = [Child || result(First) =:= ?CHRYSO_CTL_RESULT_OK],
    Calls = lists:foldl(fun count_call/2, zero(), [Hello, Stale, First, Retry]),
    %% The SYNC whose reply close dropped reached Root. Its class is unknown.
    Audit = audit(Window, Steps, Calls, 1),
    done("CTL." ++ Name, [U1, U2, U3, U4, U5, U6, U7, U8, U9, U10], Audit).

-doc """
After a BEAM restart, check that the new BEAM observes Root before it
commands: the incarnation advanced past `OldIncarnation`, the applied spec
generation is `SpecGen` (the committed spec survived the restart rather than
reading as absent), a restart carrying the previous BEAM's `OldSeq` is
`stale`, and one built from a fresh observation succeeds.
""".
-spec reconcile(atom(), non_neg_integer(), non_neg_integer(), non_neg_integer(), non_neg_integer()) ->
    ok.
reconcile(Tag, Child, OldIncarnation, OldSeq, SpecGen) ->
    Name = chryso_test:tag(Tag),
    {ok, Page} = read_page(),
    #{generation := Gen, incarnation := I, count := Count} = observe(Page, Child),
    U1 = step(incarnation_advanced, true, I > OldIncarnation),
    U2 = step(
        applied_spec,
        SpecGen,
        field(Page, ?CHRYSO_ROOT_STATUS_HEADER_APPLIED_SPEC_GENERATION_OFFSET, 64)
    ),
    {ok, F} = file:open(?CTL, [read, write, raw, binary]),
    U3 = step(
        old_seq,
        ?CHRYSO_CTL_RESULT_STALE,
        result(call(F, command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, OldSeq, Count])))
    ),
    {ok, Fresh} = read_page(),
    #{seq := Seq, count := Count1} = observe(Fresh, Child),
    Restarted =
        case call(F, command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, Seq, Count1])) of
            {ok, Result, [_, _, Lifetime, _]} -> {Result, Lifetime};
            Other -> Other
        end,
    chryso_test:settle_driver(),
    U4 = step(fresh, {?CHRYSO_CTL_RESULT_OK, Count1 + 1}, Restarted),
    ok = file:close(F),
    done("CTL_RECONCILE." ++ Name, [U1, U2, U3, U4], []).

-doc """
Fault each `{Class, Child}` driver once through `/dev/pd-restart` and read
Root's own timestamps for it from the status page: the injection, the
fault's arrival in `fault()`, the restart decision and the publication.
Prints one `LAT|<TAG>|<n>|...` line per sample, in counter ticks with the
counter frequency, then `LAT|<TAG>|done|samples=N|missing=M`.

With `Mode = flood`, a second process keeps `/dev/chrysoctl` busy with SYNC
commands for the whole run (most come back `rate_limited`), so every fault
lands while the BEAM is issuing PPCs as fast as it can.
""".
-spec latency(atom(), baseline | flood, [{atom(), non_neg_integer()}]) -> ok.
latency(Tag, Mode, Samples) ->
    Name = chryso_test:tag(Tag),
    Window = audit_open(),
    Flood = start_flood(Mode),
    Taken = [sample(N, Class, Child) || {N, {Class, Child}} <:- lists:enumerate(Samples)],
    Missing = lists:sum([M || {M, _} <:- Taken]),
    {Calls, Flooded} = stop_flood(Flood),
    %% Every injection reaches Root's fault path, even one whose events the
    %% probe missed, so each is one policy step.
    Audit = audit(Window, [Child || {_, Child} <:- Samples], Calls, 0),
    Done = {done, [{samples, length(Samples)}, {missing, Missing}]},
    chryso_test:emit_run(
        "LAT." ++ Name, [R || {_, R} <:- Taken] ++ Flooded ++ Audit ++ [Done], Missing
    ).

-doc """
Time `N` runs each of `urandom` (dirty-IO handoff only), `stat` (adds a page
copy) and `sync` (adds a Root PPC). Prints a `BENCH|<TAG>|vm|...` line, then
`BENCH|<TAG>|op=..|n=..|min_us=..|median_us=..|max_us=..|per_s=..` per op.
""".
-spec bench(atom(), pos_integer()) -> ok.
bench(Tag, N) when is_integer(N), N > 0 ->
    Name = chryso_test:tag(Tag),
    Vm = [
        {otp, erlang:system_info(otp_release)},
        {schedulers, erlang:system_info(schedulers_online)},
        {dirty_cpu, erlang:system_info(dirty_cpu_schedulers_online)},
        {dirty_io, erlang:system_info(dirty_io_schedulers)}
    ],
    {ok, Random} = file:open("/dev/urandom", [read, raw, binary]),
    Urandom = time_op(urandom, N, fun() -> {ok, <<_>>} = file:read(Random, 1) end),
    ok = file:close(Random),
    Stat = time_op(stat, N, fun() -> {ok, _} = read_page() end),
    {ok, F} = file:open(?CTL, [read, write, raw, binary]),
    Sync = command(?CHRYSO_PP_OPCODE_SYNC, [0, 0, 0, 0]),
    Synced = time_op(sync, N, fun() -> {ok, _, _} = call(F, Sync) end),
    ok = file:close(F),
    chryso_test:emit_run("BENCH." ++ Name, [{vm, Vm}, Urandom, Stat, Synced], 0).

-spec time_op(atom(), pos_integer(), fun(() -> term())) -> chryso_frame:record().
time_op(Op, N, Fun) ->
    Start = erlang:monotonic_time(microsecond),
    Samples = lists:sort([time_once(Fun) || _ <- lists:seq(1, N)]),
    Elapsed = max(1, erlang:monotonic_time(microsecond) - Start),
    Fields = [
        {op, Op},
        {runs, N},
        {min_us, hd(Samples)},
        {median_us, lists:nth((N + 1) div 2, Samples)},
        {max_us, lists:last(Samples)},
        {per_s, N * 1000000 div Elapsed}
    ],
    {op, Fields}.

-spec time_once(fun(() -> term())) -> non_neg_integer().
time_once(Fun) ->
    T0 = erlang:monotonic_time(microsecond),
    _ = Fun(),
    erlang:monotonic_time(microsecond) - T0.

%% One fault and its four timestamps, or 1 and a missing record when Root's
%% events never showed it.
-spec sample(pos_integer(), atom(), non_neg_integer()) -> outcome().
sample(N, Class, Child) ->
    {ok, Before} = read_page(),
    Head = field(Before, ?CHRYSO_ROOT_STATUS_HEADER_EVENT_HEAD_OFFSET, 64),
    ok = chryso_test:fault_pd(Class),
    case await_restart(Child, Head, 200) of
        {ok, Page, #{inject := T0, fault := T1, decided := T2}} ->
            Published = field(Page, ?CHRYSO_ROOT_STATUS_HEADER_NOW_TICKS_OFFSET, 64),
            Freq = field(Page, ?CHRYSO_ROOT_STATUS_HEADER_CNTFRQ_OFFSET, 64),
            %% Let the restarted driver settle before the next fault.
            timer:sleep(200),
            {0,
                {sample, [
                    {sample, N},
                    {child, Child},
                    {inject, T0},
                    {fault, T1},
                    {decided, T2},
                    {published, Published},
                    {cntfrq, Freq}
                ]}};
        {error, Reason} ->
            {1, {sample, [{sample, N}, {child, Child}, {missing, Reason}]}}
    end.

%% Polls the page until Root's restart of Child after event Head appears.
-spec await_restart(non_neg_integer(), non_neg_integer(), non_neg_integer()) ->
    {ok, page(), #{atom() => non_neg_integer()}} | {error, term()}.
await_restart(_Child, _Head, 0) ->
    {error, timeout};
await_restart(Child, Head, Tries) ->
    case read_page() of
        {ok, Page} ->
            case stamps(Page, Child, Head) of
                {ok, Stamps} ->
                    {ok, Page, Stamps};
                none ->
                    timer:sleep(10),
                    await_restart(Child, Head, Tries - 1)
            end;
        {error, _} ->
            timer:sleep(10),
            await_restart(Child, Head, Tries - 1)
    end.

%% The injection, fault and restart events for Child since Head, in order.
-spec stamps(page(), non_neg_integer(), non_neg_integer()) ->
    {ok, #{atom() => non_neg_integer()}} | none.
stamps(Page, Child, Head) ->
    Now = field(Page, ?CHRYSO_ROOT_STATUS_HEADER_EVENT_HEAD_OFFSET, 64),
    Events = [event(Page, I) || I <- lists:seq(max(Head, Now - ?CHRYSO_EVENT_COUNT), Now - 1)],
    Mine = [E || #{child := C} = E <- Events, C =:= Child],
    Inject = [
        T
     || #{
            kind := ?CHRYSO_ROOT_EVENT_KIND_CONTROL,
            detail := ?CHRYSO_ROOT_CONTROL_KIND_FAULT_INJECT,
            ticks := T
        } <- Mine
    ],
    Fault = [T || #{kind := ?CHRYSO_ROOT_EVENT_KIND_FAULT, ticks := T} <- Mine],
    Restart = [T || #{kind := ?CHRYSO_ROOT_EVENT_KIND_RESTART, ticks := T} <- Mine],
    case {Inject, Fault, Restart} of
        {[T0 | _], [T1 | _], [T2 | _]} -> {ok, #{inject => T0, fault => T1, decided => T2}};
        _ -> none
    end.

-spec event(page(), non_neg_integer()) -> #{atom() => non_neg_integer()}.
event(Page, Index) ->
    Base =
        ?CHRYSO_ROOT_STATUS_PAGE_EVENTS_OFFSET +
            (Index rem ?CHRYSO_EVENT_COUNT) * ?CHRYSO_ROOT_EVENT_SIZE,
    #{
        ticks => field(Page, Base + ?CHRYSO_ROOT_EVENT_TICKS_OFFSET, 64),
        kind => field(Page, Base + ?CHRYSO_ROOT_EVENT_KIND_OFFSET, 8),
        child => field(Page, Base + ?CHRYSO_ROOT_EVENT_CHILD_OFFSET, 8),
        detail => field(Page, Base + ?CHRYSO_ROOT_EVENT_DETAIL_OFFSET, 32)
    }.

%% The flooding process for Mode = flood, or none.
-spec start_flood(baseline | flood) -> none | {pid(), reference()}.
start_flood(baseline) ->
    none;
start_flood(flood) ->
    Parent = self(),
    Ref = make_ref(),
    Pid = spawn_link(fun() ->
        {ok, F} = file:open(?CTL, [read, write, raw, binary]),
        Parent ! {flooding, Ref},
        flood(F, Ref, Parent, zero())
    end),
    receive
        {flooding, Ref} -> {Pid, Ref}
    after 5000 -> error(flood_did_not_start)
    end.

-spec flood(file:io_device(), reference(), pid(), tally()) -> ok.
flood(F, Ref, Parent, Calls) ->
    receive
        {stop, Ref} ->
            ok = file:close(F),
            Parent ! {flooded, Ref, Calls},
            ok
    after 0 ->
        Got = call(F, command(?CHRYSO_PP_OPCODE_SYNC, [0, 0, 0, 0])),
        flood(F, Ref, Parent, count_call(Got, Calls))
    end.

%% The flood's calls by Root's tally classes and its record. A baseline run
%% has neither.
-spec stop_flood(none | {pid(), reference()}) -> {tally(), [chryso_frame:record()]}.
stop_flood(none) ->
    {zero(), []};
stop_flood({Pid, Ref}) ->
    Pid ! {stop, Ref},
    receive
        {flooded, Ref, #{limited := Limited} = Calls} ->
            {Calls, [{flood, [{calls, total(Calls)}, {rate_limited, Limited}]}]}
    after 5000 -> error(flood_did_not_stop)
    end.

%% --- in-situ audit ---------------------------------------------------------

%% Root's tallies, then the page, before the audited run.
-spec audit_open() -> {ok, window()} | {error, term()}.
audit_open() ->
    maybe
        {ok, Tallies, _Own} ?= sync_tallies(),
        {ok, Page} ?= read_page(),
        {ok, #{page => Page, tallies => Tallies}}
    end.

%% Close the window and return the verdict as records. Steps holds one child
%% per policy step the run caused, Calls the run's PPCs by class, and Unknown
%% the PPCs whose reply the run never read.
-spec audit(
    {ok, window()} | {error, term()}, [non_neg_integer()], tally(), non_neg_integer()
) -> [chryso_frame:record()].
audit({error, Reason}, _Steps, _Calls, _Unknown) ->
    [{audit, [{error, {open, Reason}}]}];
audit({ok, #{page := P0, tallies := T0}}, Steps, Calls, Unknown) ->
    Closed =
        maybe
            {ok, Tallies, Spent} ?= sync_tallies(),
            {ok, Page} ?= read_page(),
            {ok, Tallies, Spent, Page}
        end,
    case Closed of
        {ok, T1, Own, P1} ->
            Rows = row_mismatches(P0, P1, Steps),
            Delta = maps:map(fun(K, V) -> V - maps:get(K, T0) end, T1),
            Known = maps:map(fun(K, V) -> V + maps:get(K, Own) end, Calls),
            Fields = [
                {rows, verdict(Rows =:= [] andalso same_vm(P0, P1))},
                {steps, length(Steps)},
                {accepted, maps:get(accepted, Delta)},
                {limited, maps:get(limited, Delta)},
                {rejected, maps:get(rejected, Delta)},
                {tallies, verdict(tallies_agree(Delta, Known, Unknown))},
                {gap, gap(P0, P1)}
            ],
            [{row, [{child, C}, {model, M}, {root, R}]} || {C, M, R} <:- Rows] ++
                [{audit, Fields}];
        {error, Reason} ->
            [{audit, [{error, Reason}]}]
    end.

%% Root's counters through one SYNC on a fresh opener, retrying while the
%% bucket is empty. Own counts those calls, since Root counts them too.
-spec sync_tallies() -> {ok, tally(), tally()} | {error, term()}.
sync_tallies() ->
    {ok, F} = file:open(?CTL, [read, write, raw, binary]),
    Result = sync_tallies(F, 20, zero()),
    ok = file:close(F),
    Result.

-spec sync_tallies(file:io_device(), non_neg_integer(), tally()) ->
    {ok, tally(), tally()} | {error, term()}.
sync_tallies(_F, 0, _Own) ->
    {error, rate_limited};
sync_tallies(F, Tries, Own) ->
    case call(F, command(?CHRYSO_PP_OPCODE_SYNC, [0, 0, 0, 0])) of
        {ok, ?CHRYSO_CTL_RESULT_OK, [_Seq, A, L, R]} = Got ->
            {ok, #{accepted => A, limited => L, rejected => R}, count_call(Got, Own)};
        {ok, ?CHRYSO_CTL_RESULT_RATE_LIMITED, _} = Got ->
            timer:sleep(100),
            sync_tallies(F, Tries - 1, count_call(Got, Own));
        Other ->
            {error, {sync, Other}}
    end.

%% Root's tally rule (command.c): ok and already_done are accepted,
%% rate_limited is limited, every other reply is rejected. A call with no
%% reply is not counted here.
-spec count_call(term(), tally()) -> tally().
count_call({ok, Result, _}, #{accepted := A, limited := L, rejected := R} = T) ->
    if
        Result =:= ?CHRYSO_CTL_RESULT_OK; Result =:= ?CHRYSO_CTL_RESULT_ALREADY_DONE ->
            T#{accepted := A + 1};
        Result =:= ?CHRYSO_CTL_RESULT_RATE_LIMITED ->
            T#{limited := L + 1};
        true ->
            T#{rejected := R + 1}
    end;
count_call(_NoReply, T) ->
    T.

-spec zero() -> tally().
zero() ->
    #{accepted => 0, limited => 0, rejected => 0}.

-spec total(tally()) -> non_neg_integer().
total(#{accepted := A, limited := L, rejected := R}) ->
    A + L + R.

%% Each known class is covered, and the unread replies account for the rest.
-spec tallies_agree(tally(), tally(), non_neg_integer()) -> boolean().
tallies_agree(Delta, Known, Unknown) ->
    lists:all(fun(K) -> maps:get(K, Delta) >= maps:get(K, Known) end, maps:keys(Known)) andalso
        total(Delta) =:= total(Known) + Unknown.

%% Rows where the model, started from the first page's row and stepped once
%% per entry in Steps, disagrees with the second page's row.
-spec row_mismatches(page(), page(), [non_neg_integer()]) ->
    [{non_neg_integer(), term(), term()}].
row_mismatches(P0, P1, Steps) ->
    Count = min(field(P0, ?CHRYSO_ROOT_STATUS_HEADER_CHILD_COUNT_OFFSET, 32), ?CHRYSO_CHILD_COUNT),
    [
        {C, Model, Root}
     || C <- lists:seq(0, Count - 1),
        {Model, Root} <- [{model(P0, C, Steps), record(P1, C)}],
        Model =/= Root
    ].

-spec model(page(), non_neg_integer(), [non_neg_integer()]) -> term().
model(Page, Child, Steps) ->
    Budget = field(Page, row(Child) + ?CHRYSO_ROOT_CHILD_STATUS_EFFECTIVE_BUDGET_OFFSET, 32),
    Mine = [S || S <- Steps, S =:= Child],
    case record(Page, Child) of
        #{} = Record ->
            %% Entry 1: every audited image carries a restart entry.
            lists:foldl(
                fun(_, R) ->
                    {_Action, R1} = chryso_root_policy:step(R, Budget, 1),
                    R1
                end,
                Record,
                Mine
            );
        Raw when Mine =:= [] ->
            Raw;
        Raw ->
            {unmodelled, Raw}
    end.

%% A row as the model's record, or its raw words when the model has no
%% counterpart for its state.
-spec record(page(), non_neg_integer()) -> chryso_root_policy:record() | {raw, term()}.
record(Page, Child) ->
    Lifetime = field(Page, row(Child) + ?CHRYSO_ROOT_CHILD_STATUS_LIFETIME_COUNT_OFFSET, 32),
    case field(Page, row(Child) + ?CHRYSO_ROOT_CHILD_STATUS_STATE_OFFSET, 8) of
        ?CHRYSO_ROOT_CHILD_WIRE_STATE_LIVE -> #{state => live, lifetime => Lifetime};
        ?CHRYSO_ROOT_CHILD_WIRE_STATE_GONE -> #{state => gone, lifetime => Lifetime};
        State -> {raw, {State, Lifetime}}
    end.

-spec row(non_neg_integer()) -> non_neg_integer().
row(Child) ->
    ?CHRYSO_ROOT_STATUS_PAGE_CHILDREN_OFFSET + Child * ?CHRYSO_ROOT_CHILD_STATUS_SIZE.

%% Both pages come from one Root generation and one BEAM.
-spec same_vm(page(), page()) -> boolean().
same_vm(P0, P1) ->
    Words = [
        ?CHRYSO_ROOT_STATUS_HEADER_ROOT_GENERATION_OFFSET,
        ?CHRYSO_ROOT_STATUS_HEADER_BEAM_INCARNATION_OFFSET
    ],
    [field(P0, O, 64) || O <- Words] =:= [field(P1, O, 64) || O <- Words].

%% Events written between the two reads that the ring no longer holds.
-spec gap(page(), page()) -> non_neg_integer().
gap(P0, P1) ->
    Head0 = field(P0, ?CHRYSO_ROOT_STATUS_HEADER_EVENT_HEAD_OFFSET, 64),
    Head1 = field(P1, ?CHRYSO_ROOT_STATUS_HEADER_EVENT_HEAD_OFFSET, 64),
    max(0, Head1 - Head0 - ?CHRYSO_EVENT_COUNT).

-spec verdict(boolean()) -> string().
verdict(true) -> "match";
verdict(false) -> "mismatch".

%% Print one step's verdict and return it as a record, with 1 when it was
%% unexpected.
-spec step(atom(), term(), term()) -> outcome().
step(Name, Expected, Got) ->
    {Verdict, Unexpected} =
        case Got of
            Expected -> {ok, 0};
            _ -> {unexpected, 1}
        end,
    {Unexpected, {step, [{name, Name}, {got, Got}, {verdict, Verdict}]}}.

%% The probe's framed run, with its unexpected steps counted in the verdict.
-spec done(string(), [outcome()], [chryso_frame:record()]) -> ok.
done(Run, Steps, Extra) ->
    Unexpected = lists:sum([U || {U, _} <:- Steps]),
    chryso_test:emit_run(Run, [R || {_, R} <:- Steps] ++ Extra, Unexpected).

-spec read_page() -> {ok, page()} | {error, term()}.
read_page() ->
    read_page(?STAT_TRIES).

-spec read_page(non_neg_integer()) -> {ok, page()} | {error, term()}.
read_page(0) ->
    {error, eagain};
read_page(Tries) ->
    case file:open(?STAT, [read, raw, binary]) of
        {ok, F} ->
            Result = file:read(F, ?PAGE),
            ok = file:close(F),
            classify_page(Result, Tries);
        {error, _} = Error ->
            Error
    end.

-spec classify_page(term(), non_neg_integer()) -> {ok, page()} | {error, term()}.
classify_page(Result, Tries) ->
    case Result of
        {ok, <<Magic:8/binary, _/binary>> = Page} when
            Magic =:= ?CHRYSO_MAGIC_STATUS, byte_size(Page) =:= ?PAGE
        ->
            {ok, Page};
        {error, eagain} ->
            read_page(Tries - 1);
        {ok, _} ->
            {error, bad_page};
        {error, _} = Error ->
            Error;
        eof ->
            {error, eof}
    end.

-spec observe(page(), non_neg_integer()) -> observed().
observe(Page, Child) ->
    Row = ?CHRYSO_ROOT_STATUS_PAGE_CHILDREN_OFFSET + Child * ?CHRYSO_ROOT_CHILD_STATUS_SIZE,
    #{
        generation => field(Page, ?CHRYSO_ROOT_STATUS_HEADER_ROOT_GENERATION_OFFSET, 64),
        incarnation => field(Page, ?CHRYSO_ROOT_STATUS_HEADER_BEAM_INCARNATION_OFFSET, 64),
        seq => field(Page, ?CHRYSO_ROOT_STATUS_HEADER_SEQ_OFFSET, 64),
        count => field(Page, Row + ?CHRYSO_ROOT_CHILD_STATUS_LIFETIME_COUNT_OFFSET, 32)
    }.

-spec command(non_neg_integer(), [non_neg_integer()]) -> binary().
command(Opcode, [A, B, C, D]) ->
    Args = ?CHRYSO_CTL_COMMAND_ARGS_OFFSET,
    lists:foldl(
        fun({Offset, Bits, Value}, Bin) -> set_field(Bin, Offset, Bits, Value) end,
        put_bin(<<0:(8 * ?RECORD)>>, ?CHRYSO_CTL_COMMAND_MAGIC_OFFSET, ?CHRYSO_MAGIC_COMMAND),
        [
            {?CHRYSO_CTL_COMMAND_VERSION_OFFSET, 16, ?CHRYSO_ABI_VERSION},
            {?CHRYSO_CTL_COMMAND_OPCODE_OFFSET, 16, Opcode},
            {Args, 64, A},
            {Args + 8, 64, B},
            {Args + 16, 64, C},
            {Args + 24, 64, D}
        ]
    ).

%% One command and its reply: {ok, Result, Values} or the failing return.
-spec call(file:io_device(), binary()) ->
    {ok, non_neg_integer(), [non_neg_integer()]} | term().
call(F, Command) ->
    case file:write(F, Command) of
        ok ->
            case file:read(F, ?RECORD) of
                {ok, <<Magic:8/binary, _/binary>> = Reply} when Magic =:= ?CHRYSO_MAGIC_REPLY ->
                    Values = ?CHRYSO_CTL_REPLY_VALUES_OFFSET,
                    {ok, field(Reply, ?CHRYSO_CTL_REPLY_RESULT_OFFSET, 16), [
                        field(Reply, Values + 8 * I, 64)
                     || I <- lists:seq(0, 3)
                    ]};
                Other ->
                    Other
            end;
        Error ->
            Error
    end.

-spec result(term()) -> term().
result({ok, Result, _}) -> Result;
result(Other) -> Other.

-spec field(binary(), non_neg_integer(), pos_integer()) -> non_neg_integer().
field(Bin, Offset, Bits) ->
    <<_:Offset/binary, Value:Bits/little, _/binary>> = Bin,
    Value.

-spec set_field(binary(), non_neg_integer(), pos_integer(), non_neg_integer()) -> binary().
set_field(Bin, Offset, Bits, Value) ->
    <<Pre:Offset/binary, _:Bits, Post/binary>> = Bin,
    <<Pre/binary, Value:Bits/little, Post/binary>>.

-spec put_bin(binary(), non_neg_integer(), binary()) -> binary().
put_bin(Bin, Offset, Field) ->
    Len = byte_size(Field),
    <<Pre:Offset/binary, _:Len/binary, Post/binary>> = Bin,
    <<Pre/binary, Field/binary, Post/binary>>.
