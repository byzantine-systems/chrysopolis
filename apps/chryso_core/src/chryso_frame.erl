-module(chryso_frame).
-moduledoc """
Probe results framed for a console shared with driver output.

Other PDs print to the same UART a byte at a time, so a probe line can arrive
with foreign bytes inserted. A result must arrive whole or be reported
damaged, never half-matched.

`encode_run/3` prints one frame per record, then a verdict frame:

```
@@CHRYSO|<run>|n=<i>|k=<kind>|<key>=<value>...|len=<L>|crc=<8 hex>@@
```

`L` counts the bytes after `@@CHRYSO|` up to `|len=`. The CRC-32 covers the
body through the `len` field. Values are `%XX`-escaped outside `0x21..0x7E`
and for `% | @ =`, so a body never holds CR, LF or `@`. The verdict is
`k=verdict|frames=<N>|unexpected=<U>` at `n=N`.

`step/2` reads one byte:

| state | input | next | events |
|---|---|---|---|
| `{outside, M}` | marker byte `M` | `{outside, M+1}`, inside after the last | |
| `{outside, M}` | other | `{outside, KMP fallback}` | |
| `{inside, B, plain}` | `@` | `{inside, B, at}` | |
| `{inside, B, at}` | `@` | `{outside, 2}` | `frame` or `damaged` |
| `{inside, B, at}` | other | `{inside, B ++ "@" ++ byte, plain}` | |
| `{inside, B, plain}` | other | `{inside, B ++ byte, plain}` | |
| inside, body at the cap | any | `{outside, 0}` | `{damaged, _, oversize}` |

Closing leaves the matcher at 2 because `@@` also starts a marker. A frame
that lost its end closes on the next marker, and the next frame still parses.
A closed body is checked for `syntax`, then `length`, then `crc`. The first
failure is the reason.

Laws, each with its observation, checked by `prop_frame`:

1. **Round trip** [events]. `collect(encode_run(R, Recs, U))` yields one frame
   per record with values equal to `render/1` of the originals, then the
   verdict.
2. **ASCII closure** [bytes]. Output is `0x21..0x7E` plus `\n`.
3. **Chunk invariance** [events]. `feed/2` over any split, then `finish/1`,
   equals `collect/1`.
4. **Homomorphism** [events]. If `A` ends in `{outside, 0}`,
   `collect(A ++ B) = collect(A) ++ collect(B)`.
5. **Noise identity** [events]. Noise without `@` between frames changes
   nothing.
6. **Soundness under shuffle** [frames]. Noise without `@` inside a frame
   damages it and is never accepted. CR, LF and non-ASCII fail `syntax`.
   Other bytes fail `length`, or `syntax` in the `len` and `crc` fields.
   Noise in the marker hides the frame, which law 8 reports.
7. **Prefix stability** [observe]. For a prefix `P` of `L`, `observe(P, R)` is
   `pending` or equals `observe(L, R)`.
8. **Completeness** [observe]. `pass` means frames `0..N-1` arrived once, in
   order. A missing frame fails as `lost`.
9. **Totality** [no crash]. `collect/1` accepts any binary in linear work with
   at most `?MAX_BODY` bytes buffered.

Non-laws. Frames do not commute. A damaged frame's run is a guess. A change
that keeps the length is caught by the CRC alone.

Data. `encode_run/3` input is trusted probe source, and a bad name raises.
`collect/1` and `observe/2` input is untrusted. They create no atoms, return
binaries, cap buffering and do linear work.
""".

-export([render/1, encode_run/3, new/0, step/2, feed/2, finish/1, collect/1, observe/2]).
-export_type([
    record/0, frame/0, reason/0, event/0, state/0, verdict/0, run_name/0
]).

-define(MARKER, <<"@@CHRYSO|">>).
-define(MAX_BODY, 1024).
-define(MAX_RUN, 48).

-doc "A record: its kind and its fields, keys from a closed set in source.".
-type record() :: {atom(), [{atom(), term()}]}.
-doc "A run name, `[A-Z0-9_.]{1,48}`.".
-type run_name() :: binary() | string().
-doc "A collected frame. Keys, kinds and values stay binaries, never atoms.".
-type frame() :: #{
    run := binary(),
    n := non_neg_integer(),
    kind := binary(),
    fields := [{binary(), binary()}]
}.
-doc "Why a closed body was refused, in check order.".
-type reason() :: oversize | syntax | length | crc.
-doc "What the reader saw.".
-type event() :: {frame, frame()} | {damaged, binary(), reason()} | pending.
-doc "The reader: the marker prefix matched outside a frame, or a body.".
-opaque state() :: {outside, 0..8} | {inside, binary(), plain | at}.
-doc "A run's outcome over a log.".
-type verdict() ::
    pending
    | {pass, [frame()], non_neg_integer()}
    | {fail, {damaged, binary(), reason()} | {lost, binary(), [non_neg_integer()]}}.

%% --- encoding ----------------------------------------------------------------

-doc """
Render a value as text: integers in decimal, atoms and printable strings as
themselves, binaries as their bytes, anything else as `~0tp`. Total.
""".
-spec render(term()) -> binary().
render(V) when is_integer(V) -> integer_to_binary(V);
render(V) when is_atom(V) -> atom_to_binary(V, utf8);
render(V) when is_binary(V) -> V;
render(V) when is_list(V) ->
    case io_lib:printable_unicode_list(V) of
        true -> unicode:characters_to_binary(V);
        false -> format(V)
    end;
render(V) ->
    format(V).

-doc """
Frame a run: one frame per record, then the verdict carrying the record
count and `Unexpected`. A bad run name, kind or key, or a body over the cap,
is a broken probe invariant and raises `error`.
""".
-spec encode_run(run_name(), [record()], non_neg_integer()) -> iodata().
encode_run(Run0, Records, Unexpected) when is_integer(Unexpected), Unexpected >= 0 ->
    Run = run(Run0),
    Count = length(Records),
    Verdict = {verdict, [{frames, Count}, {unexpected, Unexpected}]},
    [
        frame(Run, N, Record)
     || {N, Record} <:- lists:enumerate(0, Records ++ [Verdict])
    ].

-spec frame(binary(), non_neg_integer(), record()) -> iodata().
frame(Run, N, {Kind, Fields}) when is_atom(Kind), is_list(Fields) ->
    Prefix = iolist_to_binary([
        Run,
        <<"|n=">>,
        integer_to_binary(N),
        <<"|k=">>,
        name(Kind),
        [[$|, key(K), $=, escape(render(V))] || {K, V} <:- Fields]
    ]),
    Sealed = seal(Prefix),
    case byte_size(Sealed) =< ?MAX_BODY of
        true -> [?MARKER, Sealed, <<"@@\n">>];
        false -> error({frame_too_large, Run, N, byte_size(Sealed)})
    end.

%% Prefix plus its length and CRC.
-spec seal(binary()) -> binary().
seal(Prefix) ->
    Covered = <<Prefix/binary, "|len=", (integer_to_binary(byte_size(Prefix)))/binary>>,
    Crc = iolist_to_binary(io_lib:format("~8.16.0b", [erlang:crc32(Covered)])),
    <<Covered/binary, "|crc=", Crc/binary>>.

-spec run(run_name()) -> binary().
run(Run) when is_list(Run) -> run(unicode:characters_to_binary(Run));
run(Run) when is_binary(Run) ->
    case is_run(Run) of
        true -> Run;
        false -> error({bad_run_name, Run})
    end.

-spec key(atom()) -> binary().
key(K) when K =/= n, K =/= k, K =/= len, K =/= crc -> name(K);
key(K) -> error({reserved_key, K}).

-spec name(atom()) -> binary().
name(A) when is_atom(A) ->
    B = atom_to_binary(A, utf8),
    case B =/= <<>> andalso lists:all(fun is_name_byte/1, binary_to_list(B)) of
        true -> B;
        false -> error({bad_name, A})
    end.

-spec escape(binary()) -> binary().
escape(Bin) ->
    <<<<(escape_byte(C))/binary>> || <<C>> <:= Bin>>.

-spec escape_byte(byte()) -> binary().
escape_byte(C) when C >= 16#21, C =< 16#7E, C =/= $%, C =/= $|, C =/= $@, C =/= $= ->
    <<C>>;
escape_byte(C) ->
    iolist_to_binary(io_lib:format("%~2.16.0B", [C])).

-spec format(term()) -> binary().
format(V) ->
    unicode:characters_to_binary(io_lib:format("~0tp", [V])).

%% --- reading -------------------------------------------------------------------

-doc "A reader at the start of a log.".
-spec new() -> state().
new() -> {outside, 0}.

-doc "Read one byte (the Mealy step in the moduledoc table).".
-spec step(byte(), state()) -> {[event()], state()}.
step(C, {outside, M}) ->
    case advance(M, C) of
        9 -> {[], {inside, <<>>, plain}};
        M1 -> {[], {outside, M1}}
    end;
step(_C, {inside, B, _At}) when byte_size(B) >= ?MAX_BODY ->
    {[{damaged, guess(B), oversize}], {outside, 0}};
step($@, {inside, B, plain}) ->
    {[], {inside, B, at}};
step($@, {inside, B, at}) ->
    {[close(B)], {outside, 2}};
step(C, {inside, B, at}) ->
    {[], {inside, <<B/binary, $@, C>>, plain}};
step(C, {inside, B, plain}) ->
    {[], {inside, <<B/binary, C>>, plain}}.

-doc "Read a chunk of a log. Chunks may split anywhere (law 3).".
-spec feed(binary(), state()) -> {[event()], state()}.
feed(Bin, S0) ->
    {Rev, S} = feed(Bin, S0, []),
    {lists:reverse(Rev), S}.

-spec feed(binary(), state(), [event()]) -> {[event()], state()}.
feed(<<C, Rest/binary>>, S0, Acc) ->
    case step(C, S0) of
        {[], S} -> feed(Rest, S, Acc);
        {[E], S} -> feed(Rest, S, [E | Acc])
    end;
feed(<<>>, S, Acc) ->
    {Acc, S}.

-doc "End of the log seen so far: an open frame is `pending`, never damaged.".
-spec finish(state()) -> [event()].
finish({inside, _, _}) -> [pending];
finish({outside, _}) -> [].

-doc "Every event in a log, in log order.".
-spec collect(binary()) -> [event()].
collect(Log) when is_binary(Log) ->
    {Events, S} = feed(Log, new()),
    Events ++ finish(S).

-doc """
A run's outcome. Walks the events in order and decides at the first damaged
frame or at the run's verdict. Until then it is `pending` (law 7). Frames of
other runs are ignored.
""".
-spec observe(binary(), run_name()) -> verdict().
observe(Log, Run) ->
    decide(collect(Log), run(Run), []).

-spec decide([event()], binary(), [frame()]) -> verdict().
decide([], _Run, _Seen) ->
    pending;
decide([pending | _], _Run, _Seen) ->
    pending;
decide([{damaged, Guess, Reason} | _], _Run, _Seen) ->
    {fail, {damaged, Guess, Reason}};
decide([{frame, #{run := Run, kind := <<"verdict">>} = F} | _], Run, Seen) ->
    verdict(F, Run, lists:reverse(Seen));
decide([{frame, #{run := Run} = F} | Rest], Run, Seen) ->
    decide(Rest, Run, [F | Seen]);
decide([{frame, _Other} | Rest], Run, Seen) ->
    decide(Rest, Run, Seen).

-spec verdict(frame(), binary(), [frame()]) -> verdict().
verdict(#{n := N, fields := Fields}, Run, Frames) ->
    Expected = lists:seq(0, N - 1),
    Got = [I || #{n := I} <:- Frames],
    case {count(Fields, <<"frames">>), count(Fields, <<"unexpected">>)} of
        {{ok, N}, {ok, U}} when Got =:= Expected -> {pass, Frames, U};
        {{ok, N}, {ok, _}} -> {fail, {lost, Run, Expected -- Got}};
        %% A verdict whose count disagrees with its n, or is unreadable,
        %% vouches for nothing.
        {{ok, _}, {ok, _}} -> {fail, {lost, Run, Expected}};
        {error, _} -> {fail, {lost, Run, Expected}};
        {{ok, _}, error} -> {fail, {lost, Run, Expected}}
    end.

-spec count([{binary(), binary()}], binary()) -> {ok, non_neg_integer()} | error.
count(Fields, Key) ->
    case lists:keyfind(Key, 1, Fields) of
        {Key, V} -> digits(V);
        false -> error
    end.

%% --- closed bodies ---------------------------------------------------------------

-spec close(binary()) -> event().
close(Body) ->
    case parse(Body) of
        {ok, Frame} -> {frame, Frame};
        {error, Reason} -> {damaged, guess(Body), Reason}
    end.

%% syntax, then length, then crc. The first failure is the reason.
-spec parse(binary()) -> {ok, frame()} | {error, reason()}.
parse(Body) ->
    maybe
        true ?= lists:all(fun(C) -> C >= 16#21 andalso C =< 16#7E end, binary_to_list(Body)),
        [Run, <<"n=", N0/binary>>, <<"k=", Kind/binary>> | Rest] ?=
            binary:split(Body, <<"|">>, [
                global
            ]),
        [<<"crc=", Crc/binary>>, <<"len=", Len0/binary>> | RevFields] ?= lists:reverse(Rest),
        true ?= is_run(Run) andalso is_name(Kind) andalso is_hex8(Crc),
        {ok, N} ?= digits(N0),
        {ok, Len} ?= digits(Len0),
        {ok, Fields} ?= fields(lists:reverse(RevFields), []),
        Covered = byte_size(Body) - byte_size(<<"|crc=">>) - 8,
        Prefix = binary:part(Body, 0, Covered - byte_size(<<"|len=">>) - byte_size(Len0)),
        {length, true} ?= {length, byte_size(Prefix) =:= Len},
        {crc, true} ?= {crc, hex8(erlang:crc32(binary:part(Body, 0, Covered))) =:= Crc},
        {ok, #{run => Run, n => N, kind => Kind, fields => Fields}}
    else
        {length, false} -> {error, length};
        {crc, false} -> {error, crc};
        %% A byte check or field grammar refused it, or a field split into
        %% the wrong shape.
        false -> {error, syntax};
        error -> {error, syntax};
        Shape when is_list(Shape) -> {error, syntax}
    end.

-spec fields([binary()], [{binary(), binary()}]) -> {ok, [{binary(), binary()}]} | error.
fields([], Acc) ->
    {ok, lists:reverse(Acc)};
fields([Field | Rest], Acc) ->
    maybe
        [K, V] ?= binary:split(Field, <<"=">>),
        true ?= is_name(K),
        {ok, Value} ?= unescape(V),
        fields(Rest, [{K, Value} | Acc])
    else
        [_NoEquals] -> error;
        false -> error;
        error -> error
    end.

-spec unescape(binary()) -> {ok, binary()} | error.
unescape(V) -> unescape(V, <<>>).

-spec unescape(binary(), binary()) -> {ok, binary()} | error.
unescape(<<>>, Acc) ->
    {ok, Acc};
unescape(<<$%, H, L, Rest/binary>>, Acc) ->
    maybe
        {ok, Hi} ?= hex(H),
        {ok, Lo} ?= hex(L),
        unescape(Rest, <<Acc/binary, (Hi * 16 + Lo)>>)
    else
        error -> error
    end;
unescape(<<C, _/binary>>, _Acc) when C =:= $%; C =:= $@; C =:= $= ->
    error;
unescape(<<C, Rest/binary>>, Acc) ->
    unescape(Rest, <<Acc/binary, C>>).

-spec hex(byte()) -> {ok, 0..15} | error.
hex(C) when C >= $0, C =< $9 -> {ok, C - $0};
hex(C) when C >= $A, C =< $F -> {ok, C - $A + 10};
hex(C) when is_integer(C) -> error.

%% Decimal without a sign or a redundant leading zero.
-spec digits(binary()) -> {ok, non_neg_integer()} | error.
digits(<<"0">>) ->
    {ok, 0};
digits(<<C, _/binary>> = B) when C >= $1, C =< $9, byte_size(B) =< 10 ->
    case lists:all(fun(D) -> D >= $0 andalso D =< $9 end, binary_to_list(B)) of
        true -> {ok, binary_to_integer(B)};
        false -> error
    end;
digits(B) when is_binary(B) ->
    error.

-spec hex8(non_neg_integer()) -> binary().
hex8(X) -> iolist_to_binary(io_lib:format("~8.16.0b", [X])).

-spec is_hex8(binary()) -> boolean().
is_hex8(B) ->
    byte_size(B) =:= 8 andalso
        lists:all(
            fun(C) -> (C >= $0 andalso C =< $9) orelse (C >= $a andalso C =< $f) end,
            binary_to_list(B)
        ).

-spec is_run(binary()) -> boolean().
is_run(B) ->
    byte_size(B) >= 1 andalso byte_size(B) =< ?MAX_RUN andalso
        lists:all(
            fun(C) ->
                (C >= $A andalso C =< $Z) orelse (C >= $0 andalso C =< $9) orelse C =:= $_ orelse
                    C =:= $.
            end,
            binary_to_list(B)
        ).

-spec is_name(binary()) -> boolean().
is_name(B) ->
    B =/= <<>> andalso lists:all(fun is_name_byte/1, binary_to_list(B)).

-spec is_name_byte(byte()) -> boolean().
is_name_byte(C) -> (C >= $a andalso C =< $z) orelse (C >= $0 andalso C =< $9) orelse C =:= $_.

%% The damaged frame's run, when its first field still reads as one.
-spec guess(binary()) -> binary().
guess(Body) ->
    [First | _] = binary:split(Body, <<"|">>),
    case is_run(First) of
        true -> First;
        false -> <<"?">>
    end.

%% KMP fallback: the longest marker prefix that is a suffix of the matched
%% prefix plus C. Brute force over nine bytes keeps it obviously right.
-spec advance(0..8, byte()) -> 0..9.
advance(M, C) ->
    Seen = <<(binary:part(?MARKER, 0, M))/binary, C>>,
    hd([K || K <- lists:seq(min(M + 1, 9), 0, -1), suffix(Seen, K)]).

-spec suffix(binary(), 0..9) -> boolean().
suffix(Seen, K) ->
    binary:part(Seen, byte_size(Seen) - K, K) =:= binary:part(?MARKER, 0, K).
