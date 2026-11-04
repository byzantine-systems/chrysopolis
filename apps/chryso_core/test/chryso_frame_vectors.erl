-module(chryso_frame_vectors).
-moduledoc """
Golden vectors for the console frame reader. Each vector is a log
(`<name>.log`) and what `chryso_frame` makes of it (`<name>.expect.json`,
events plus the verdict for `V.RUN`). The testbed's Python reader must match
every one.

Output is deterministic. A fixed seed, no clock, no node identity.
""".

-export([write/1]).

-define(RUN, <<"V.RUN">>).
-define(SEED, {16#c4, 16#a7, 16#e5}).

-doc "Write every vector into `Dir`, which must exist and be empty.".
-spec write(file:filename()) -> ok.
write(Dir) ->
    S0 = rand:seed_s(exsss, ?SEED),
    {Generated, _} = lists:mapfoldl(fun generated/2, S0, lists:seq(1, 48)),
    Vectors = fixed() ++ Generated,
    Names = [Name || {Name, _} <:- Vectors],
    [] = Names -- lists:usort(Names),
    [ok = write_vector(Dir, Name, Log) || {Name, Log} <:- Vectors],
    Manifest = [
        <<"chryso-frame-vectors 1\n">>,
        [[Name, $\n] || Name <:- Names],
        io_lib:format("count ~b~n", [length(Names)])
    ],
    ok = file:write_file(filename:join(Dir, "manifest.txt"), Manifest).

-spec write_vector(file:filename(), binary(), binary()) -> ok.
write_vector(Dir, Name, Log) ->
    Expect = #{
        events => [event(E) || E <:- chryso_frame:collect(Log)],
        observe => verdict(chryso_frame:observe(Log, ?RUN))
    },
    ok = file:write_file(filename:join(Dir, <<Name/binary, ".log">>), Log),
    ok = file:write_file(
        filename:join(Dir, <<Name/binary, ".expect.json">>), [json:encode(Expect), $\n]
    ).

-spec event(chryso_frame:event()) -> json:encode_value().
event({frame, #{run := R, n := N, kind := K, fields := Fs}}) ->
    #{frame => #{run => R, n => N, kind => K, fields => [[Key, V] || {Key, V} <:- Fs]}};
event({damaged, Guess, Reason}) ->
    #{damaged => #{run => Guess, reason => Reason}};
event(pending) ->
    pending.

-spec verdict(chryso_frame:verdict()) -> json:encode_value().
verdict(pending) ->
    pending;
verdict({pass, Frames, U}) ->
    #{pass => #{frames => length(Frames), unexpected => U}};
verdict({fail, {damaged, Guess, Reason}}) ->
    #{fail => #{kind => damaged, run => Guess, reason => Reason}};
verdict({fail, {lost, Run, Missing}}) ->
    #{fail => #{kind => lost, run => Run, missing => Missing}}.

%% --- hand-picked cases -------------------------------------------------------------

-spec fixed() -> [{binary(), binary()}].
fixed() ->
    Run = run([{step, [{name, fresh}, {got, {error, ebusy}}, {verdict, ok}]}, {row, [{child, 3}]}]),
    %% Inside the value "fresh", so a splice there damages a field.
    {Fresh, _} = binary:match(Run, <<"fresh">>),
    At = Fresh + 2,
    Crc = crc_at(Run),
    [
        {<<"empty">>, <<>>},
        {<<"noise_only">>, <<"1> chryso_ctl:stat('boot', 1).\r\nrx=128|oktx=0\r\n">>},
        {<<"good">>, Run},
        {<<"good_noisy">>, interleave(Run, [<<"rx=128">>, <<"|tx=0\r\n">>, <<"1> ">>])},
        {<<"splice_crlf">>, splice(Run, At, <<"|oktx=0\r\n">>)},
        {<<"splice_plain">>, splice(Run, At, <<"rx">>)},
        {<<"splice_pr81">>, splice(Run, At, <<"rx=128">>)},
        {<<"splice_len">>, splice(Run, len_at(Run), <<"1">>)},
        {<<"splice_crc">>, splice(Run, Crc, <<"0">>)},
        {<<"splice_marker">>, splice(Run, 3, <<"x">>)},
        {<<"crc_flipped">>, flip_hex(Run, Crc)},
        {<<"bit_flip">>, flip_bit(Run, At, 2)},
        {<<"deleted">>, delete_line(Run, 1)},
        {<<"pending_tail">>, binary:part(Run, 0, byte_size(Run) - 10)},
        {<<"split_marker_tail">>, <<"noise @@CHR">>},
        {<<"truncated_then_next">>, <<(binary:part(Run, 0, 40))/binary, Run/binary>>},
        {<<"oversize">>, <<"@@CHRYSO|V.RUN|", (binary:copy(<<"a">>, 1100))/binary, "@@\n">>},
        {<<"other_run_only">>, iolist_to_binary(chryso_frame:encode_run(<<"OTHER">>, [], 0))},
        {<<"non_ascii">>, splice(Run, At, <<16#C3, 16#A9>>)},
        {<<"verdict_mismatch">>, fake_verdict()}
    ].

%% A verdict frame that claims two frames when its n says one.
-spec fake_verdict() -> binary().
fake_verdict() ->
    Body = <<"V.RUN|n=1|k=verdict|frames=2|unexpected=0">>,
    Covered = <<Body/binary, "|len=", (integer_to_binary(byte_size(Body)))/binary>>,
    Crc = iolist_to_binary(io_lib:format("~8.16.0b", [erlang:crc32(Covered)])),
    Zero = run([]),
    <<"@@CHRYSO|", Covered/binary, "|crc=", Crc/binary, "@@\n", Zero/binary>>.

%% --- seeded cases ------------------------------------------------------------------

-spec generated(pos_integer(), rand:state()) -> {{binary(), binary()}, rand:state()}.
generated(I, S0) ->
    {Recs, S1} = records(S0),
    {Kind, S2} = pick([clean, noisy, spliced, cut], S1),
    Run = run(Recs),
    {Log, S3} =
        case Kind of
            clean ->
                {Run, S2};
            noisy ->
                {Noise, S} = noise_list(S2),
                {interleave(Run, Noise), S};
            spliced ->
                {Pos, Sa} = rand:uniform_s(byte_size(Run) - 1, S2),
                {Seg, Sb} = noise(Sa),
                {splice(Run, Pos, Seg), Sb};
            cut ->
                {Pos, Sa} = rand:uniform_s(byte_size(Run), S2),
                {binary:part(Run, 0, Pos), Sa}
        end,
    Name = iolist_to_binary(io_lib:format("seed_~3..0b_~s", [I, Kind])),
    {{Name, Log}, S3}.

-spec records(rand:state()) -> {[chryso_frame:record()], rand:state()}.
records(S0) ->
    {N, S1} = rand:uniform_s(5, S0),
    lists:mapfoldl(fun(_, S) -> record(S) end, S1, lists:seq(1, N - 1)).

-spec record(rand:state()) -> {chryso_frame:record(), rand:state()}.
record(S0) ->
    {Kind, S1} = pick([step, row, sample, audit], S0),
    {N, S2} = rand:uniform_s(4, S1),
    {Fields, S3} = lists:mapfoldl(
        fun(_, S) ->
            {K, Sa} = pick([name, got, child, x_1], S),
            {V, Sb} = value(Sa),
            {{K, V}, Sb}
        end,
        S2,
        lists:seq(1, N - 1)
    ),
    {{Kind, Fields}, S3}.

%% Values whose rendering is valid UTF-8, so the expectations are valid JSON.
-spec value(rand:state()) -> {term(), rand:state()}.
value(S0) ->
    {Choice, S1} = rand:uniform_s(6, S0),
    {I, S2} = rand:uniform_s(100000, S1),
    Value =
        case Choice of
            1 -> I - 50000;
            2 -> ok;
            3 -> {error, ebusy};
            4 -> "a|b@c=d%e";
            5 -> [16#E9, 16#2603];
            6 -> #{child => I}
        end,
    {Value, S2}.

-spec noise_list(rand:state()) -> {[binary()], rand:state()}.
noise_list(S0) ->
    {N, S1} = rand:uniform_s(4, S0),
    lists:mapfoldl(fun(_, S) -> noise(S) end, S1, lists:seq(1, N)).

-spec noise(rand:state()) -> {binary(), rand:state()}.
noise(S) ->
    pick(
        [
            <<"rx=128|oktx=0\r\n">>,
            <<"rx=128">>,
            <<"|tx=0\r\n">>,
            <<"1> ">>,
            <<"ETH|up\n">>,
            <<"zz">>
        ],
        S
    ).

-spec pick([T, ...], rand:state()) -> {T, rand:state()}.
pick(List, S0) ->
    {I, S1} = rand:uniform_s(length(List), S0),
    {lists:nth(I, List), S1}.

%% --- edits -------------------------------------------------------------------------

-spec run([chryso_frame:record()]) -> binary().
run(Recs) ->
    iolist_to_binary(chryso_frame:encode_run(?RUN, Recs, 0)).

-spec interleave(binary(), [binary()]) -> binary().
interleave(Run, Noise) ->
    Lines = [<<L/binary, "\n">> || L <- binary:split(Run, <<"\n">>, [global, trim])],
    iolist_to_binary(zip_noise(Lines, Noise)).

-spec zip_noise([binary()], [binary()]) -> iodata().
zip_noise([L | Ls], [N | Ns]) -> [L, N | zip_noise(Ls, Ns)];
zip_noise(Ls, []) -> Ls;
zip_noise([], Ns) -> Ns.

-spec splice(binary(), non_neg_integer(), binary()) -> binary().
splice(Bin, Pos, Seg) ->
    <<
        (binary:part(Bin, 0, Pos))/binary,
        Seg/binary,
        (binary:part(Bin, Pos, byte_size(Bin) - Pos))/binary
    >>.

-spec delete_line(binary(), pos_integer()) -> binary().
delete_line(Bin, K) ->
    Lines = binary:split(Bin, <<"\n">>, [global, trim]),
    iolist_to_binary([[L, $\n] || {I, L} <:- lists:enumerate(Lines), I =/= K]).

-spec flip_bit(binary(), non_neg_integer(), 0..7) -> binary().
flip_bit(Bin, At, Bit) ->
    <<Pre:At/binary, B, Post/binary>> = Bin,
    <<Pre/binary, (B bxor (1 bsl Bit)), Post/binary>>.

%% Replace the first crc digit with a different hex digit.
-spec flip_hex(binary(), non_neg_integer()) -> binary().
flip_hex(Bin, At) ->
    <<Pre:At/binary, D, Post/binary>> = Bin,
    New =
        case D of
            $0 -> $1;
            _ -> $0
        end,
    <<Pre/binary, New, Post/binary>>.

-spec crc_at(binary()) -> non_neg_integer().
crc_at(Bin) ->
    {Pos, Len} = binary:match(Bin, <<"|crc=">>),
    Pos + Len.

-spec len_at(binary()) -> non_neg_integer().
len_at(Bin) ->
    {Pos, Len} = binary:match(Bin, <<"|len=">>),
    Pos + Len.
