-module(chryso_trace).
-moduledoc """
Shell access to `/dev/chrysotrace`, the wake-path trace built only into
`trace-image` (`src/pd/beam/config/runtime_trace.h`). In any other image
every call returns `{error, enoent}`.

`arm/0` clears and starts the trace, `stop/0` freezes it, and `dump/1`
logs one `TRACE|<TAG>|...` debug event per record, with time in
microseconds since the first record, then a `TRACE|<TAG>|done|records=N|dropped=D`
notice.
""".

-include_lib("kernel/include/logger.hrl").

-export([arm/0, stop/0, dump/1]).

-define(DEV, "/dev/chrysotrace").
-define(HEADER, 32).
-define(RECORD, 32).
%% A full snapshot: the header plus the device's 2047 records, 64 KiB.
-define(MAX, 65536).

-doc "Clear the trace and start recording.".
-spec arm() -> ok | {error, term()}.
arm() ->
    control(<<"1">>).

-doc "Stop recording. The records stay readable.".
-spec stop() -> ok | {error, term()}.
stop() ->
    control(<<"0">>).

-doc "Log every record, decoded, under the upper-cased `Tag`.".
-spec dump(atom()) -> ok | {error, term()}.
dump(Tag) ->
    Name = chryso_test:tag(Tag),
    case snapshot() of
        {ok, Freq, Records, Dropped} ->
            Start =
                case Records of
                    [{T0, _, _, _, _} | _] -> T0;
                    [] -> 0
                end,
            lists:foreach(
                fun({Ticks, Event, Cothread, A, B}) ->
                    ?LOG_DEBUG(
                        "TRACE|~s|us=~b|ev=~s|co=~b|a=~b|b=~b",
                        [
                            Name,
                            (Ticks - Start) * 1000000 div max(1, Freq),
                            event(Event),
                            Cothread,
                            A,
                            B
                        ]
                    )
                end,
                Records
            ),
            ?LOG_NOTICE("TRACE|~s|done|records=~b|dropped=~b", [Name, length(Records), Dropped]);
        {error, _} = Error ->
            Error
    end.

-spec control(binary()) -> ok | {error, term()}.
control(Byte) ->
    case file:open(?DEV, [read, write, raw, binary]) of
        {ok, F} ->
            Result = file:write(F, Byte),
            ok = file:close(F),
            Result;
        {error, _} = Error ->
            Error
    end.

-spec snapshot() ->
    {ok, pos_integer(), [{integer(), integer(), integer(), integer(), integer()}],
        non_neg_integer()}
    | {error, term()}.
snapshot() ->
    case file:open(?DEV, [read, write, raw, binary]) of
        {ok, F} ->
            Result = file:read(F, ?MAX),
            ok = file:close(F),
            decode(Result);
        {error, _} = Error ->
            Error
    end.

-spec decode(term()) ->
    {ok, pos_integer(), [{integer(), integer(), integer(), integer(), integer()}],
        non_neg_integer()}
    | {error, term()}.
decode(
    {ok,
        <<"CHTRACE1", Freq:64/little, Len:32/little, Dropped:32/little, _State:8, _Pad:7/binary,
            Body/binary>>}
) when byte_size(Body) =:= Len * ?RECORD ->
    Records = [
        {Ticks, Event, Cothread, A, B}
     || <<Ticks:64/little, A:64/little-signed, B:64/little-signed, Event:8, Cothread:8/signed,
            _:6/binary>> <= Body
    ],
    {ok, Freq, Records, Dropped};
decode({ok, _}) ->
    {error, bad_snapshot};
decode(Other) ->
    {error, Other}.

%% Names follow runtime_trace_event in runtime_trace_ring.h.
-spec event(non_neg_integer()) -> string().
event(0) -> "futex_wait";
event(1) -> "futex_timeout";
event(2) -> "futex_wake";
event(3) -> "park";
event(4) -> "resume";
event(5) -> "epoll_enter";
event(6) -> "epoll_exit";
event(7) -> "notified";
event(8) -> "drain_rearm";
event(N) -> integer_to_list(N).
