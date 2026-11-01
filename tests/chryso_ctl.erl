-module(chryso_ctl).
-moduledoc """
Probes for Root's BEAM devices, `/dev/chrysostat` and `/dev/chrysoctl`
(`src/pd/beam/control/runtime_control_dev.c`).

Records are built and read through the generated `orchestrator_abi.hrl`
offsets, never a hand-written layout, so an ABI change cannot leave these
probes reading the wrong bytes. Each step prints
`<PREFIX>|<step>|<result>|ok` or `...|unexpected`, then
`<PREFIX>|done|unexpected=N`, which `run_probe` in `tests.nix` asserts on.
""".

-include("orchestrator_abi.hrl").

-export([stat/2, stat_errors/1, ctl_script/2, reconcile/5, latency/3]).

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

-doc """
Print `CTL_STAT|<TAG>|incarnation=I|seq=S|count=C` from one status snapshot,
where `count` is `Child`'s lifetime restart count.
""".
-spec stat(atom(), non_neg_integer()) -> ok.
stat(Tag, Child) ->
    case read_page() of
        {ok, Page} ->
            #{incarnation := I, seq := S, count := C} = observe(Page, Child),
            io:format(
                "CTL_STAT|~s|incarnation=~b|seq=~b|count=~b~n",
                [chryso_test:tag(Tag), I, S, C]
            );
        {error, Reason} ->
            io:format("CTL_STAT|~s|error=~p~n", [chryso_test:tag(Tag), Reason])
    end.

-doc "Exercise the status device's local failures; none involves Root.".
-spec stat_errors(atom()) -> ok.
stat_errors(Tag) ->
    Prefix = "CTL_STAT_ERR|" ++ chryso_test:tag(Tag),
    {ok, Short} = file:open(?STAT, [read, raw, binary]),
    U1 = step(Prefix, short_read, {error, emsgsize}, file:read(Short, ?PAGE - 1)),
    U2 = step(Prefix, write_readonly, {error, ebadf}, file:write(Short, <<0>>)),
    ok = file:close(Short),
    {ok, Once} = file:open(?STAT, [read, raw, binary]),
    {ok, _} = file:read(Once, ?PAGE),
    U3 = step(Prefix, second_read, eof, file:read(Once, ?PAGE)),
    ok = file:close(Once),
    U4 = step(Prefix, open_write, {error, eacces}, file:open(?STAT, [write, raw])),
    done(Prefix, [U1, U2, U3, U4]).

-doc """
Drive the command device against Root, restarting `Child` once.

Local rejects are decided before any PPC; Root's typed outcomes arrive in
replies. Ends by closing with a reply pending and checking that the next
opener cannot see it.
""".
-spec ctl_script(atom(), non_neg_integer()) -> ok.
ctl_script(Tag, Child) ->
    Prefix = "CTL|" ++ chryso_test:tag(Tag),
    {ok, F} = file:open(?CTL, [read, write, raw, binary]),
    U1 = step(Prefix, second_open, {error, ebusy}, file:open(?CTL, [read, write, raw, binary])),
    U2 = step(Prefix, read_idle, {error, enodata}, file:read(F, ?RECORD)),
    U3 = step(
        Prefix,
        hello,
        {?CHRYSO_CTL_RESULT_OK, ?CHRYSO_ABI_VERSION},
        case call(F, command(?CHRYSO_PP_OPCODE_HELLO, [?CHRYSO_ABI_VERSION, 0, 0, 0])) of
            {ok, Result, [Version | _]} -> {Result, Version};
            Other -> Other
        end
    ),
    U4 = step(Prefix, short_write, {error, emsgsize}, file:write(F, <<0:(8 * (?RECORD - 1))>>)),
    U5 = step(
        Prefix,
        bad_magic,
        {error, ebadmsg},
        file:write(F, put_bin(command(?CHRYSO_PP_OPCODE_HELLO, [0, 0, 0, 0]), 0, <<"XXXXXXXX">>))
    ),
    U6 = step(Prefix, bad_opcode, {error, ebadmsg}, file:write(F, command(99, [0, 0, 0, 0]))),
    {ok, Before} = read_page(),
    #{generation := Gen, seq := Seq, count := Count} = observe(Before, Child),
    U7 = step(
        Prefix,
        stale,
        ?CHRYSO_CTL_RESULT_STALE,
        result(call(F, command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, Seq + 2, Count])))
    ),
    {ok, Fresh} = read_page(),
    #{seq := Seq1, count := Count1} = observe(Fresh, Child),
    Restart = command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, Seq1, Count1]),
    U8 = step(
        Prefix,
        restart,
        {?CHRYSO_CTL_RESULT_OK, Count1 + 1},
        case call(F, Restart) of
            {ok, Result1, [_, _, Lifetime, _]} -> {Result1, Lifetime};
            Other1 -> Other1
        end
    ),
    U9 = step(Prefix, retry, ?CHRYSO_CTL_RESULT_ALREADY_DONE, result(call(F, Restart))),
    ok = file:write(F, command(?CHRYSO_PP_OPCODE_SYNC, [0, 0, 0, 0])),
    ok = file:close(F),
    {ok, G} = file:open(?CTL, [read, write, raw, binary]),
    U10 = step(Prefix, dropped_reply, {error, enodata}, file:read(G, ?RECORD)),
    ok = file:close(G),
    done(Prefix, [U1, U2, U3, U4, U5, U6, U7, U8, U9, U10]).

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
    Prefix = "CTL_RECONCILE|" ++ chryso_test:tag(Tag),
    {ok, Page} = read_page(),
    #{generation := Gen, incarnation := I, count := Count} = observe(Page, Child),
    U1 = step(Prefix, incarnation_advanced, true, I > OldIncarnation),
    U2 = step(
        Prefix,
        applied_spec,
        SpecGen,
        field(Page, ?CHRYSO_ROOT_STATUS_HEADER_APPLIED_SPEC_GENERATION_OFFSET, 64)
    ),
    {ok, F} = file:open(?CTL, [read, write, raw, binary]),
    U3 = step(
        Prefix,
        old_seq,
        ?CHRYSO_CTL_RESULT_STALE,
        result(call(F, command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, OldSeq, Count])))
    ),
    {ok, Fresh} = read_page(),
    #{seq := Seq, count := Count1} = observe(Fresh, Child),
    U4 = step(
        Prefix,
        fresh,
        {?CHRYSO_CTL_RESULT_OK, Count1 + 1},
        case call(F, command(?CHRYSO_PP_OPCODE_RESTART_CHILD, [Child, Gen, Seq, Count1])) of
            {ok, Result, [_, _, Lifetime, _]} -> {Result, Lifetime};
            Other -> Other
        end
    ),
    ok = file:close(F),
    done(Prefix, [U1, U2, U3, U4]).

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
    Flood = start_flood(Mode),
    Missing = lists:sum([
        sample(Name, N, Class, Child)
     || {N, {Class, Child}} <- lists:enumerate(Samples)
    ]),
    stop_flood(Name, Flood),
    io:format("LAT|~s|done|samples=~b|missing=~b~n", [Name, length(Samples), Missing]).

%% One fault and its four timestamps; 1 when Root's events never showed it.
-spec sample(string(), pos_integer(), atom(), non_neg_integer()) -> 0 | 1.
sample(Name, N, Class, Child) ->
    {ok, Before} = read_page(),
    Head = field(Before, ?CHRYSO_ROOT_STATUS_HEADER_EVENT_HEAD_OFFSET, 64),
    ok = chryso_test:fault_pd(Class),
    case await_restart(Child, Head, 200) of
        {ok, Page, #{inject := T0, fault := T1, decided := T2}} ->
            io:format(
                "LAT|~s|~b|child=~b|inject=~b|fault=~b|decided=~b|published=~b|cntfrq=~b~n",
                [
                    Name,
                    N,
                    Child,
                    T0,
                    T1,
                    T2,
                    field(Page, ?CHRYSO_ROOT_STATUS_HEADER_NOW_TICKS_OFFSET, 64),
                    field(Page, ?CHRYSO_ROOT_STATUS_HEADER_CNTFRQ_OFFSET, 64)
                ]
            ),
            %% Let the restarted driver settle before the next fault.
            timer:sleep(200),
            0;
        {error, Reason} ->
            io:format("LAT|~s|~b|child=~b|missing=~p~n", [Name, N, Child, Reason]),
            1
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
        flood(F, Ref, Parent, 0, 0)
    end),
    receive
        {flooding, Ref} -> {Pid, Ref}
    after 5000 -> error(flood_did_not_start)
    end.

-spec flood(file:io_device(), reference(), pid(), non_neg_integer(), non_neg_integer()) -> ok.
flood(F, Ref, Parent, Calls, Limited) ->
    receive
        {stop, Ref} ->
            ok = file:close(F),
            Parent ! {flooded, Ref, Calls, Limited},
            ok
    after 0 ->
        Got = result(call(F, command(?CHRYSO_PP_OPCODE_SYNC, [0, 0, 0, 0]))),
        Limited1 =
            case Got of
                ?CHRYSO_CTL_RESULT_RATE_LIMITED -> Limited + 1;
                _ -> Limited
            end,
        flood(F, Ref, Parent, Calls + 1, Limited1)
    end.

-spec stop_flood(string(), none | {pid(), reference()}) -> ok.
stop_flood(_Name, none) ->
    ok;
stop_flood(Name, {Pid, Ref}) ->
    Pid ! {stop, Ref},
    receive
        {flooded, Ref, Calls, Limited} ->
            io:format("LAT|~s|flood|calls=~b|rate_limited=~b~n", [Name, Calls, Limited])
    after 5000 -> error(flood_did_not_stop)
    end.

%% Print one step's verdict; 1 when it was unexpected, for done/2.
-spec step(string(), atom(), term(), term()) -> 0 | 1.
step(Prefix, Name, Expected, Got) ->
    {Verdict, Unexpected} =
        case Got of
            Expected -> {"ok", 0};
            _ -> {"unexpected", 1}
        end,
    io:format("~s|~s|~0p|~s~n", [Prefix, Name, Got, Verdict]),
    Unexpected.

%% The line run_probe in tests.nix waits on.
-spec done(string(), [0 | 1]) -> ok.
done(Prefix, Unexpected) ->
    io:format("~s|done|unexpected=~b~n", [Prefix, lists:sum(Unexpected)]).

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
