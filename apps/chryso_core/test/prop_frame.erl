-module(prop_frame).
-moduledoc """
Laws of `chryso_frame`, numbered as in its moduledoc. Observations are
`collect/1` events and `observe/2` verdicts, never reader state.

Damage comes from declarative splice plans, so failures shrink to a small
plan. Noise never holds `@`, since no other console writer prints one.
""".
-include_lib("proper/include/proper.hrl").
-include_lib("kernel/include/logger.hrl").

-export([
    prop_round_trip/0,
    prop_ascii/0,
    prop_chunks/0,
    prop_homomorphism/0,
    prop_noise/0,
    prop_shuffle/0,
    prop_length/0,
    prop_prefix/0,
    prop_complete/0,
    prop_total/0,
    prop_generator/0,
    prop_coverage/0,
    prop_anchors/0
]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(RUN, <<"PROBE.T">>).

%% 1. A run reads back as its records, each value as render/1 of the original.
prop_round_trip() ->
    ?FORALL(
        {Run, Recs, U},
        {run_name(), records(), integer(0, 9)},
        begin
            Events = chryso_frame:collect(encode(Run, Recs, U)),
            Expected = expected(Run, Recs, U),
            ?WHENFAIL(
                ?LOG_ERROR("events=~tp~nexpected=~tp", [Events, Expected]),
                aggregate([label(Recs =:= [], empty_run, nonempty_run)], Events =:= Expected)
            )
        end
    ).

%% 2. Encoded output is printable ASCII plus newline, whatever the values.
prop_ascii() ->
    ?FORALL(
        {Run, Recs},
        {run_name(), records()},
        lists:all(
            fun(C) -> C =:= $\n orelse (C >= 16#21 andalso C =< 16#7E) end,
            binary_to_list(encode(Run, Recs, 0))
        )
    ).

%% 3. Any split of any log reads the same as the whole.
prop_chunks() ->
    ?FORALL(
        {Log, Cuts},
        ?LET(Log, log(), {Log, cuts(Log)}),
        begin
            Chunks = split(Log, Cuts),
            {Rev, S} = lists:foldl(
                fun(C, {Acc, S0}) ->
                    {Es, S1} = chryso_frame:feed(C, S0),
                    {lists:reverse(Es, Acc), S1}
                end,
                {[], chryso_frame:new()},
                Chunks
            ),
            Whole = chryso_frame:collect(Log),
            aggregate(
                [label(split_marker(Log, Cuts), split_marker, other_split)],
                lists:reverse(Rev) ++ chryso_frame:finish(S) =:= Whole
            )
        end
    ).

%% 4. A log that ends between frames concatenates.
prop_homomorphism() ->
    ?FORALL(
        {A, B},
        {clean_log(), log()},
        chryso_frame:collect(<<A/binary, B/binary>>) =:=
            chryso_frame:collect(A) ++ chryso_frame:collect(B)
    ).

%% 5. Noise between frames changes nothing.
prop_noise() ->
    ?FORALL(
        {Run, Recs, Noise},
        {run_name(), records(), list(noise())},
        begin
            Lines = lines(encode(Run, Recs, 0)),
            Noisy = iolist_to_binary(interleave(Lines, Noise)),
            aggregate(
                [label(Noise =/= [], noise, quiet)],
                chryso_frame:collect(Noisy) =:= expected(Run, Recs, 0)
            )
        end
    ).

%% 6. Noise inside a frame body damages that frame: never accepted, never
%% caught only by the CRC, and a line break is always a syntax failure.
prop_shuffle() ->
    ?FORALL(
        {Recs, Plan},
        ?LET(Recs, records1(), {Recs, splice_plan(length(Recs) + 1)}),
        begin
            Clean = encode(?RUN, Recs, 0),
            Log = splice(Clean, Plan),
            Events = chryso_frame:collect(Log),
            Good = [F || {frame, F} <:- chryso_frame:collect(Clean)],
            Accepted = [F || {frame, F} <- Events],
            Reasons = [R || {damaged, _, R} <- Events],
            {_, _, Segment} = Plan,
            Expected =
                case has_line_break(Segment) of
                    true -> [syntax];
                    false -> [syntax, length]
                end,
            ?WHENFAIL(
                ?LOG_ERROR("plan=~tp~nlog=~tp~nevents=~tp", [Plan, Log, Events]),
                aggregate(
                    [label(has_line_break(Segment), crlf_splice, printable_splice) | Reasons],
                    lists:all(fun(F) -> lists:member(F, Good) end, Accepted) andalso
                        length(Reasons) =:= 1 andalso
                        lists:member(hd(Reasons), Expected)
                )
            )
        end
    ).

%% 6, the deterministic case: letters and digits inserted into a value pass
%% every syntax check, so only the declared length can catch them. With the
%% length check removed, the CRC still would, which this property refuses.
prop_length() ->
    ?FORALL(
        {Value, At, Segment},
        {
            non_empty(resize(20, list(range($a, $z)))),
            integer(0, 100),
            non_empty(resize(8, list(oneof([range($a, $z), range($0, $9)]))))
        },
        begin
            Clean = encode(?RUN, [{step, [{name, list_to_binary(Value)}]}], 0),
            {Field, _} = binary:match(Clean, <<"|name=">>),
            Start = Field + byte_size(<<"|name=">>),
            Log = splice_at(Clean, Start + At rem (length(Value) + 1), list_to_binary(Segment)),
            ?WHENFAIL(
                ?LOG_ERROR("log=~tp", [Log]),
                chryso_frame:observe(Log, ?RUN) =:= {fail, {damaged, ?RUN, length}}
            )
        end
    ).

%% 7. A prefix decides nothing, or decides what the whole log decides.
prop_prefix() ->
    ?FORALL(
        {Log, Cut},
        {run_log(), integer(0, 4000)},
        begin
            P = binary:part(Log, 0, min(Cut, byte_size(Log))),
            Short = chryso_frame:observe(P, ?RUN),
            Long = chryso_frame:observe(Log, ?RUN),
            ?WHENFAIL(
                ?LOG_ERROR("prefix=~tp~nshort=~tp~nlong=~tp", [P, Short, Long]),
                aggregate(
                    [label(Short =:= pending, pending_tail, decided)],
                    Short =:= pending orelse Short =:= Long
                )
            )
        end
    ).

%% 8. pass means every frame arrived once and in order. A missing or hidden
%% frame is lost.
prop_complete() ->
    ?FORALL(
        {Recs, Drop, Hide},
        ?LET(Recs, records1(), {Recs, integer(0, length(Recs) - 1), boolean()}),
        begin
            Lines = lines(encode(?RUN, Recs, 0)),
            Victim = lists:nth(Drop + 1, Lines),
            Replacement =
                case Hide of
                    %% Noise inside the marker hides the frame.
                    true ->
                        <<
                            (binary:part(Victim, 0, 3))/binary,
                            "x",
                            (binary:part(Victim, 3, byte_size(Victim) - 3))/binary
                        >>;
                    false ->
                        <<>>
                end,
            Log = iolist_to_binary(replace_nth(Drop + 1, Lines, Replacement)),
            Whole = chryso_frame:observe(encode(?RUN, Recs, 0), ?RUN),
            aggregate(
                [label(Hide, marker_splice, deletion)],
                chryso_frame:observe(Log, ?RUN) =:= {fail, {lost, ?RUN, [Drop]}} andalso
                    element(1, Whole) =:= pass andalso
                    length(element(2, Whole)) =:= length(Recs)
            )
        end
    ).

%% 9. Any bytes at all: no crash, well-formed events, pending only last.
prop_total() ->
    ?FORALL(
        Log,
        oneof([binary(), log(), bit_flipped()]),
        begin
            Events = chryso_frame:collect(Log),
            lists:all(fun well_formed/1, Events) andalso
                not lists:member(pending, lists:droplast([x | Events]))
        end
    ).

%% The generators produce only what encode_run accepts (it raises otherwise).
prop_generator() ->
    ?FORALL(
        {Run, Recs},
        {run_name(), records()},
        is_binary(encode(Run, Recs, 0))
    ).

%% Every class the laws rely on is generated. PropEr reports classes but
%% does not enforce them, so this property does.
prop_coverage() ->
    numtests(
        3,
        ?FORALL(
            Samples,
            vector(400, sample_class()),
            begin
                Missing = classes() -- lists:usort(lists:append(Samples)),
                ?WHENFAIL(?LOG_ERROR("classes never generated: ~p", [Missing]), Missing =:= [])
            end
        )
    ).

%% Hand-worked cases that rule out a degenerate codec.
prop_anchors() ->
    numtests(
        1,
        ?FORALL(
            _,
            exactly(ok),
            begin
                %% Hand-counted: "A|n=0|k=step|x=1" is 16 bytes.
                One =
                    <<"@@CHRYSO|A|n=0|k=step|x=1|len=16|crc=",
                        (crc(<<"A|n=0|k=step|x=1|len=16">>))/binary, "@@\n">>,
                Run = encode(<<"CTL_RECONCILE.FB_POST">>, [{step, [{name, fresh}, {got, 2}]}], 0),
                %% The two fragments the eth driver interleaved into a probe
                %% line in CI, spliced into a value.
                {At, _} = binary:match(Run, <<"fresh">>),
                Damaged = [
                    chryso_frame:observe(splice_at(Run, At + 2, Seg), <<"CTL_RECONCILE.FB_POST">>)
                 || Seg <- [<<"rx=128">>, <<"|oktx=0\r\n">>, <<"rx">>]
                ],
                crc(<<"123456789">>) =:= <<"cbf43926">> andalso
                    encode(<<"A">>, [{step, [{x, 1}]}], 0) =:=
                        <<One/binary, (binary:part(encode(<<"A">>, [], 0), 0, 0))/binary,
                            (line(<<"A">>, 1, <<"verdict">>, <<"frames=1|unexpected=0">>))/binary>> andalso
                    lists:all(
                        fun
                            ({fail, {damaged, <<"CTL_RECONCILE.FB_POST">>, R}}) -> R =/= crc;
                            (_) -> false
                        end,
                        Damaged
                    )
            end
        )
    ).

%% --- generators ------------------------------------------------------------------

run_name() ->
    ?LET(
        Cs,
        non_empty(list(elements("ABZ09_."))),
        list_to_binary(lists:sublist(Cs, 48))
    ).

records() -> resize(6, list(record())).
records1() -> resize(6, non_empty(list(record()))).

record() ->
    {elements([step, row, sample, audit]), resize(4, list({key(), value()}))}.

key() -> elements([name, got, verdict, child, x_1]).

value() ->
    oneof([
        integer(),
        elements([ok, unexpected, 'Weird Atom', '']),
        ?LET(B, resize(12, binary()), B),
        ?LET(S, resize(10, list(range(16#20, 16#7E))), S),
        ?LET(S, resize(6, list(oneof([range(16#A0, 16#FF), range(16#100, 16#2FF)]))), S),
        elements(["a|b", "x@@y", "k=v", "50%", "\r\n", "@@CHRYSO|"]),
        {error, elements([ebusy, enodata])},
        #{a => 1}
    ]).

%% Console text from other writers: printable, CR, LF, and never `@`.
noise() ->
    oneof([
        elements([
            <<"rx=128|oktx=0\r\n">>,
            <<"rx=128">>,
            <<"|tx=0\r\n">>,
            <<"1> ">>,
            <<"chryso_ctl:ctl_script(cmd, 1).\r\n">>,
            <<"ETH|restart|reclaimed rx=3 tx=0\n">>
        ]),
        ?LET(
            Cs,
            resize(16, list(oneof([range(16#20, 16#3F), range(16#41, 16#7E), $\r, $\n]))),
            list_to_binary(Cs)
        )
    ]).

%% Frames and noise only, so the log ends between frames.
clean_log() ->
    ?LET(
        Parts,
        resize(4, list(oneof([?LET({R, Rs}, {run_name(), records()}, encode(R, Rs, 0)), noise()]))),
        iolist_to_binary(Parts)
    ).

%% Anything a console might hold: frames, noise, cut frames, raw bytes.
log() ->
    ?LET(
        Parts,
        resize(5, list(oneof([clean_log(), noise(), cut_frame(), resize(8, binary())]))),
        iolist_to_binary(Parts)
    ).

cut_frame() ->
    ?LET(
        {Bin, Cut},
        {?LET({R, Rs}, {run_name(), records()}, encode(R, Rs, 0)), integer(1, 300)},
        binary:part(Bin, 0, min(Cut, byte_size(Bin)))
    ).

%% A run of ?RUN, mixed with noise, possibly damaged and possibly cut.
run_log() ->
    ?LET(
        {Recs, Noise, Damage},
        {records(), list(noise()), oneof([none, splice_plan(1)])},
        begin
            Run = iolist_to_binary(interleave(lines(encode(?RUN, Recs, 0)), Noise)),
            case Damage of
                none -> Run;
                Plan -> splice(Run, Plan)
            end
        end
    ).

bit_flipped() ->
    ?LET(
        {Bin, At, Bit},
        {clean_log(), integer(0, 4000), integer(0, 7)},
        case byte_size(Bin) of
            0 -> Bin;
            Size -> flip(Bin, At rem Size, Bit)
        end
    ).

%% Split points, some inside a start marker, which random cuts rarely hit.
cuts(Log) ->
    Inside = [S + K || {S, _} <- binary:matches(Log, <<"@@CHRYSO|">>), K <- lists:seq(1, 8)],
    Any = integer(0, max(0, byte_size(Log))),
    list(
        case Inside of
            [] -> Any;
            _ -> frequency([{3, Any}, {1, elements(Inside)}])
        end
    ).

%% Insert a segment inside frame I's body: after the marker, before `@@`.
splice_plan(Frames) ->
    {integer(0, Frames - 1), integer(0, 1000), noise1()}.

noise1() ->
    ?SUCHTHAT(N, noise(), N =/= <<>>).

sample_class() ->
    ?LET(
        {Recs, Plan, {Log, Cuts}},
        {records(), splice_plan(1), ?LET(Log, log(), {Log, cuts(Log)})},
        classes_of(Recs, Plan, Log, Cuts)
    ).

classes() ->
    [
        empty_run,
        unicode,
        reserved,
        near_cap,
        crlf_splice,
        printable_splice,
        split_marker,
        cut_frame
    ].

classes_of(Recs, {_, _, Seg}, Log, Cuts) ->
    Values = [V || {_, Fs} <- Recs, {_, V} <- Fs],
    Rendered = [chryso_frame:render(V) || V <- Values],
    Body = byte_size(encode(?RUN, Recs, 0)),
    [empty_run || Recs =:= []] ++
        [
            unicode
         || lists:any(fun(B) -> lists:any(fun(C) -> C > 16#7E end, binary_to_list(B)) end, Rendered)
        ] ++
        [
            reserved
         || lists:any(
                fun(B) -> binary:match(B, [<<"|">>, <<"@">>, <<"=">>, <<"%">>]) =/= nomatch end,
                Rendered
            )
        ] ++
        [near_cap || Body > 600] ++
        [crlf_splice || has_line_break(Seg)] ++
        [printable_splice || not has_line_break(Seg)] ++
        [split_marker || split_marker(Log, Cuts)] ++
        [
            cut_frame
         || binary:match(Log, <<"@@CHRYSO|">>) =/= nomatch andalso
                lists:last([pending | chryso_frame:collect(Log)]) =:= pending
        ].

%% --- helpers ----------------------------------------------------------------------

label(true, Yes, _No) -> Yes;
label(false, _Yes, No) -> No.

encode(Run, Recs, U) ->
    iolist_to_binary(chryso_frame:encode_run(Run, Recs, U)).

expected(Run, Recs, U) ->
    [
        {frame, #{
            run => Run,
            n => N,
            kind => atom_to_binary(K),
            fields => [{atom_to_binary(Key), chryso_frame:render(V)} || {Key, V} <:- Fs]
        }}
     || {N, {K, Fs}} <:- lists:enumerate(0, Recs)
    ] ++
        [
            {frame, #{
                run => Run,
                n => length(Recs),
                kind => <<"verdict">>,
                fields => [
                    {<<"frames">>, integer_to_binary(length(Recs))},
                    {<<"unexpected">>, integer_to_binary(U)}
                ]
            }}
        ].

lines(Bin) ->
    [<<L/binary, "\n">> || L <- binary:split(Bin, <<"\n">>, [global, trim])].

interleave([L | Ls], [N | Ns]) -> [L, N | interleave(Ls, Ns)];
interleave(Ls, []) -> Ls;
interleave([], Ns) -> Ns.

replace_nth(1, [_ | T], X) -> [X | T];
replace_nth(N, [H | T], X) -> [H | replace_nth(N - 1, T, X)].

%% Insert Segment into frame I's body at a position derived from At.
splice(Log, {I, At, Segment}) ->
    Lines = lines_of(Log),
    Frames = [K || {K, L} <- lists:enumerate(Lines), binary:match(L, <<"@@CHRYSO|">>) =/= nomatch],
    case Frames of
        [] ->
            Log;
        _ ->
            K = lists:nth(I rem length(Frames) + 1, Frames),
            Line = lists:nth(K, Lines),
            {Start, _} = binary:match(Line, <<"@@CHRYSO|">>),
            {End, _} = binary:match(Line, <<"@@">>, [
                {scope, {Start + 9, byte_size(Line) - Start - 9}}
            ]),
            Pos = Start + 9 + At rem (End - Start - 9 + 1),
            Spliced = <<
                (binary:part(Line, 0, Pos))/binary,
                Segment/binary,
                (binary:part(Line, Pos, byte_size(Line) - Pos))/binary
            >>,
            iolist_to_binary(replace_nth(K, Lines, Spliced))
    end.

lines_of(Bin) ->
    [<<L/binary, "\n">> || L <- binary:split(Bin, <<"\n">>, [global])] -- [<<"\n">>].

splice_at(Bin, Pos, Segment) ->
    <<
        (binary:part(Bin, 0, Pos))/binary,
        Segment/binary,
        (binary:part(Bin, Pos, byte_size(Bin) - Pos))/binary
    >>.

split(Bin, Cuts) ->
    Points = lists:usort([C || C <- Cuts, C > 0, C < byte_size(Bin)]),
    split(Bin, 0, Points).

split(Bin, From, []) ->
    [binary:part(Bin, From, byte_size(Bin) - From)];
split(Bin, From, [P | Ps]) ->
    [binary:part(Bin, From, P - From) | split(Bin, P, Ps)].

%% A cut lands strictly inside a start marker.
split_marker(Log, Cuts) ->
    Starts = [S || {S, _} <- binary:matches(Log, <<"@@CHRYSO|">>)],
    lists:any(fun(C) -> lists:any(fun(S) -> C > S andalso C < S + 9 end, Starts) end, Cuts).

has_line_break(Seg) ->
    binary:match(Seg, [<<"\r">>, <<"\n">>]) =/= nomatch.

flip(Bin, At, Bit) ->
    <<Pre:At/binary, B, Post/binary>> = Bin,
    <<Pre/binary, (B bxor (1 bsl Bit)), Post/binary>>.

crc(Bin) ->
    iolist_to_binary(io_lib:format("~8.16.0b", [erlang:crc32(Bin)])).

line(Run, N, Kind, Fields) ->
    Prefix =
        <<Run/binary, "|n=", (integer_to_binary(N))/binary, "|k=", Kind/binary, "|",
            Fields/binary>>,
    Covered = <<Prefix/binary, "|len=", (integer_to_binary(byte_size(Prefix)))/binary>>,
    <<"@@CHRYSO|", Covered/binary, "|crc=", (crc(Covered))/binary, "@@\n">>.

well_formed({frame, #{run := R, n := N, kind := K, fields := Fs}}) ->
    is_binary(R) andalso is_integer(N) andalso is_binary(K) andalso
        lists:all(fun({A, B}) -> is_binary(A) andalso is_binary(B) end, Fs);
well_formed({damaged, G, R}) ->
    is_binary(G) andalso lists:member(R, [oversize, syntax, length, crc]);
well_formed(pending) ->
    true;
well_formed(_) ->
    false.
