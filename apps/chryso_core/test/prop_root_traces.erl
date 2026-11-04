-module(prop_root_traces).
-moduledoc """
Differential law: for every run in the corpus traced from Root's C units
(`tests/host/root/suite_root_fallback.c --traces`, packaged as
`.#root-traces`), `chryso_root` produces the same decisions and replies.
`CHRYSO_ROOT_TRACES` names the corpus. The page reaches the model as the
codec classified it, so this checks the semantics, not the byte checks.

The corpus is finite, so one case replays all of it and reports every
divergence. Trace text is matched against a closed vocabulary. Nothing read
from a file becomes an atom.
""".
-include_lib("proper/include/proper.hrl").
-include_lib("kernel/include/logger.hrl").

-export([prop_model_agrees_with_c/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

prop_model_agrees_with_c() ->
    ?FORALL(
        Dir,
        exactly(corpus_dir()),
        begin
            Files = corpus(Dir),
            Diverged = [{filename:basename(F), E} || F <- Files, {error, E} <- [replay_safe(F)]],
            ?WHENFAIL(
                ?LOG_ERROR("~b of ~b traces diverged:~n~p", [
                    length(Diverged), length(Files), Diverged
                ]),
                Files =/= [] andalso Diverged =:= []
            )
        end
    ).

corpus_dir() ->
    case os:getenv("CHRYSO_ROOT_TRACES") of
        false -> error(chryso_root_traces_unset);
        Dir -> Dir
    end.

corpus(Dir) ->
    Files = filelib:wildcard(filename:join(Dir, "trace_*.txt")),
    {ok, Manifest} = file:read_file(filename:join(Dir, "manifest.txt")),
    [_, <<"count ", Count/binary>> | _] = binary:split(Manifest, <<"\n">>, [global]),
    binary_to_integer(Count) =:= length(Files) orelse error({manifest_count, Count, length(Files)}),
    Files.

replay_safe(File) ->
    try replay(File) of
        ok -> ok
    catch
        error:Reason -> {error, Reason}
    end.

replay(File) ->
    {ok, Bin} = file:read_file(File),
    Lines = [L || L <- binary:split(Bin, <<"\n">>, [global]), L =/= <<>>],
    Numbered = lists:zip(lists:seq(1, length(Lines)), Lines),
    [{_, Init} | Rest] = Numbered,
    run(chryso_root:new(config(fields(Init))), Rest, File).

run(_Root, [{_, <<"end">>}], _File) ->
    ok;
run(Root, [{_, <<"tick ", _/binary>>} | Rest], File) ->
    run(Root, Rest, File);
run(
    Root,
    [{N, <<"fault ", F/binary>>}, {_, <<"spec ", S/binary>>}, {DN, <<"decide ", D/binary>>} | Rest],
    File
) ->
    #{<<"child">> := Child, <<"now">> := Now} = fields(F),
    {Decided, Root1} = chryso_root:fault(Root, int(Child), page(S), int(Now)),
    check(File, N, DN, decide_line(D), decided(Decided)),
    run(Root1, Rest, File);
run(Root, [{N, <<"cmd ", C/binary>>} | Rest0], File) ->
    #{<<"op">> := Op, <<"count">> := Count, <<"now">> := Now, <<"args">> := Args} = fields(C),
    {Page, ExpectDecide, Rest1} =
        case Rest0 of
            [{_, <<"spec ", S/binary>>}, {DN, <<"decide ", D/binary>>} | R] ->
                {page(S), {DN, decide_line(D)}, R};
            _ ->
                {none, none, Rest0}
        end,
    {Reply, Decided, Root1} =
        chryso_root:command(Root, int(Op), int(Count), ints(Args), int(Now), Page),
    case {ExpectDecide, Decided} of
        {none, none} -> ok;
        {{DN1, Want}, #{}} -> check(File, N, DN1, Want, decided(Decided));
        _ -> error({File, N, decide_mismatch, ExpectDecide, Decided})
    end,
    [{RN, <<"reply ", R1/binary>>} | Rest2] = Rest1,
    check(File, N, RN, reply_line(R1), reply(Reply)),
    run(Root1, Rest2, File);
run(_Root, [{N, Line} | _], File) ->
    error({File, N, unexpected_line, Line}).

check(_File, _N, _LineNo, Same, Same) ->
    ok;
check(File, N, LineNo, Want, Got) ->
    error(
        {trace_divergence, filename:basename(File), {input_line, N}, {output_line, LineNo},
            {c, Want}, {model, Got}}
    ).

%% --- the model's outputs in the trace's words ---------------------------------

decided(#{action := Action, record := Rec, spec := Spec, retained := Ret, seq := Seq}) ->
    #{state := State, lifetime := Lifetime} = Rec,
    #{outcome := Outcome, reason := Reason, record := Recorded} = Spec,
    #{generation := Gen, budgets := Budgets} = Ret,
    {Action, State, Lifetime, Outcome, Reason, Recorded, Gen, pad(Budgets), Seq}.

reply(#{result := Result, values := Values} = R) ->
    {Result, Values, maps:get(tokens, R, none)}.

pad(Budgets) ->
    Budgets ++ lists:duplicate(6 - length(Budgets), 0).

%% --- the C's outputs ---------------------------------------------------------

decide_line(Bin) ->
    #{
        <<"action">> := A,
        <<"state">> := S,
        <<"lifetime">> := L,
        <<"outcome">> := O,
        <<"reason">> := R,
        <<"record">> := Rec,
        <<"retained">> := G,
        <<"budgets">> := B,
        <<"seq">> := Seq
    } = fields(Bin),
    {
        action(A),
        state(S),
        int(L),
        outcome(O),
        reason(int(R)),
        Rec =:= <<"1">>,
        int(G),
        ints(B),
        int(Seq)
    }.

reply_line(Bin) ->
    F = fields(Bin),
    Tokens =
        case F of
            #{<<"tokens">> := T} -> int(T);
            #{} -> none
        end,
    {result(maps:get(<<"result">>, F)), ints(maps:get(<<"values">>, F)), Tokens}.

%% --- inputs -------------------------------------------------------------------

config(F) ->
    #{
        mapped => maps:get(<<"mapped">>, F) =:= <<"1">>,
        now => int(maps:get(<<"now">>, F)),
        tokens => int(maps:get(<<"tokens">>, F)),
        refill => int(maps:get(<<"refill">>, F)),
        hard_max => int(maps:get(<<"hard_max">>, F)),
        entry => int(maps:get(<<"entry">>, F)),
        pp => int(maps:get(<<"pp">>, F)),
        beam => int(maps:get(<<"beam">>, F)),
        present => int(maps:get(<<"present">>, F)),
        drivers => int(maps:get(<<"drivers">>, F)),
        compiled => ints(maps:get(<<"compiled">>, F)),
        generation => int(maps:get(<<"generation">>, F)),
        incarnation => int(maps:get(<<"incarnation">>, F)),
        seq => int(maps:get(<<"seq">>, F)),
        abi => int(maps:get(<<"abi">>, F)),
        cntfrq => int(maps:get(<<"cntfrq">>, F))
    }.

page(<<"absent">>) ->
    absent;
page(<<"header">>) ->
    header;
page(<<"bank", Active:1/binary, " ", Rest/binary>>) ->
    [A, O] = binary:split(Rest, <<" ">>),
    {banks, int(Active), candidate(A), candidate(O)}.

candidate(<<"empty">>) ->
    empty;
candidate(<<"rej/", Rest/binary>>) ->
    [Reason, Gen] = binary:split(Rest, <<"/">>),
    {rejected, reason_name(Reason), int(Gen)};
candidate(<<"bank/", Rest/binary>>) ->
    [Gen, Budgets, Desired] = binary:split(Rest, <<"/">>, [global]),
    {bank, int(Gen), ints(Budgets), ints(Desired)}.

fields(Bin) ->
    maps:from_list([
        list_to_tuple(binary:split(KV, <<"=">>))
     || KV <- binary:split(Bin, <<" ">>, [global]), binary:match(KV, <<"=">>) =/= nomatch
    ]).

int(B) -> binary_to_integer(B).

ints(B) -> [int(X) || X <- binary:split(B, <<",">>, [global])].

%% --- the closed vocabulary ----------------------------------------------------

action(<<"resume">>) -> resume;
action(<<"giveup_budget">>) -> giveup_budget;
action(<<"giveup_no_entry">>) -> giveup_no_entry;
action(<<"ignore_gone">>) -> ignore_gone.

state(<<"live">>) -> live;
state(<<"gone">>) -> gone.

outcome(<<"absent">>) -> absent;
outcome(<<"applied">>) -> applied;
outcome(<<"unchanged">>) -> unchanged;
outcome(<<"kept">>) -> kept.

result(<<"ok">>) -> ok;
result(<<"refused">>) -> refused;
result(<<"rate_limited">>) -> rate_limited;
result(<<"bad_arg">>) -> bad_arg;
result(<<"bad_version">>) -> bad_version;
result(<<"already_done">>) -> already_done;
result(<<"stale">>) -> stale.

%% RootSpecReject in interfaces/orchestrator_abi.zig.
reason(0) -> unset;
reason(1) -> header;
reason(2) -> bank_torn;
reason(3) -> bank_checksum;
reason(4) -> bank_malformed;
reason(5) -> undeclared_child;
reason(6) -> regressed;
reason(7) -> conflict;
reason(8) -> unstable_copy.

reason_name(<<"bank_torn">>) -> bank_torn;
reason_name(<<"bank_checksum">>) -> bank_checksum;
reason_name(<<"bank_malformed">>) -> bank_malformed;
reason_name(<<"undeclared_child">>) -> undeclared_child.
