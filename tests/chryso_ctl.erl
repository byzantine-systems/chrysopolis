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

-export([stat/2, stat_errors/1, ctl_script/2]).

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
    {ok, F} = file:open(?STAT, [read, raw, binary]),
    Result = file:read(F, ?PAGE),
    ok = file:close(F),
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
