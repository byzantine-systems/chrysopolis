-module(chryso_abi_vectors_tests).
-moduledoc """
Shared golden vectors. `tools/abi` builds each image from the typed model and
states its verdict; the generated C checkers in `tests/host` and this codec
must both reach it. Reads `$CHRYSO_ABI_GENERATED/vectors/manifest.txt` and
fails, never skips, when it is missing.

Beyond the verdict, every canonical image (all reserved bytes zero) must
survive decode then encode byte for byte, so Erlang places each field where
the model does, and every decoder must agree with its checker.
""".
%% EUnit exports the test functions; they need no spec or doc.
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-include_lib("eunit/include/eunit.hrl").
-include("orchestrator_abi.hrl").

-define(MANIFEST_VERSION, 1).

vectors_test_() ->
    Dir = vector_dir(),
    {ok, Text} = file:read_file(filename:join(Dir, "manifest.txt")),
    [Header | Rows] = binary:split(Text, <<"\n">>, [global, trim]),
    [<<"chryso-abi-vectors">>, Version, Count, Digest] = binary:split(Header, <<" ">>, [global]),
    Vectors = [row(Dir, Row) || Row <- Rows],
    [
        {"manifest version", ?_assertEqual(?MANIFEST_VERSION, binary_to_integer(Version))},
        %% Vectors built from another model must not pass against this codec.
        {"layout digest", ?_assertEqual(?CHRYSO_ABI_LAYOUT_DIGEST, binary_to_integer(Digest, 16))},
        {"no vector skipped", ?_assertEqual(binary_to_integer(Count), length(Vectors))},
        {"crc-32 check value", ?_assertEqual(16#CBF43926, erlang:crc32(<<"123456789">>))}
        | [{binary_to_list(maps:get(name, V)), fun() -> check_vector(V) end} || V <- Vectors]
    ].

vector_dir() ->
    case os:getenv("CHRYSO_ABI_GENERATED") of
        false -> error({missing_env, "CHRYSO_ABI_GENERATED"});
        Generated -> filename:join(Generated, "vectors")
    end.

%% `name checker bank class canonical size`
row(Dir, Row) ->
    [Name, Checker, Bank, Class, Canonical, Size] = binary:split(Row, <<" ">>, [global]),
    true = safe_name(Name),
    {ok, Bytes} = file:read_file(filename:join(Dir, <<Name/binary, ".bin">>)),
    #{
        name => Name,
        checker => checker(Checker),
        bank => binary_to_integer(Bank),
        expect => verdict(Class),
        canonical => canonical(Canonical),
        size => binary_to_integer(Size),
        bytes => Bytes
    }.

%% Names become file names.
safe_name(Name) ->
    Name =/= <<>> andalso
        lists:all(
            fun(C) ->
                (C >= $a andalso C =< $z) orelse (C >= $0 andalso C =< $9) orelse C =:= $_
            end,
            binary_to_list(Name)
        ).

check_vector(
    #{bytes := Bytes, size := Size, checker := Checker, bank := Bank, expect := Expect} = V
) ->
    ?assertEqual(Size, byte_size(Bytes)),
    ?assertEqual(Expect, check(Checker, Bytes, Bank)),
    ?assertEqual(Expect, decoded(Checker, Bytes, Bank)),
    case V of
        #{canonical := true} -> ?assertEqual(Bytes, roundtrip(Checker, Bytes));
        #{canonical := false} -> ok
    end.

%% Manifest words map onto known atoms only; nothing is created from input.
checker(<<"root_status_page">>) -> root_status_page;
checker(<<"spec_header">>) -> spec_header;
checker(<<"spec_bank">>) -> spec_bank;
checker(<<"ctl_command">>) -> ctl_command;
checker(<<"ctl_reply">>) -> ctl_reply;
checker(<<"worker_identity">>) -> worker_identity;
checker(<<"worker_status">>) -> worker_status;
checker(<<"request_journal">>) -> request_journal;
checker(<<"completion_journal">>) -> completion_journal.

verdict(<<"ok">>) -> ok;
verdict(<<"magic">>) -> {error, magic};
verdict(<<"version">>) -> {error, version};
verdict(<<"size">>) -> {error, size};
verdict(<<"checksum">>) -> {error, checksum};
verdict(<<"reserved">>) -> {error, reserved};
verdict(<<"unknown_kind">>) -> {error, unknown_kind};
verdict(<<"range">>) -> {error, range};
verdict(<<"torn">>) -> {error, torn}.

canonical(<<"1">>) -> true;
canonical(<<"0">>) -> false.

check(root_status_page, B, _) -> chryso_abi_codec:check_root_status_page(B);
check(spec_header, B, _) -> chryso_abi_codec:check_spec_header(B);
check(spec_bank, B, Bank) -> chryso_abi_codec:check_spec_bank(B, Bank);
check(ctl_command, B, _) -> chryso_abi_codec:check_ctl_command(B);
check(ctl_reply, B, _) -> chryso_abi_codec:check_ctl_reply(B);
check(worker_identity, B, _) -> chryso_abi_codec:check_worker_identity(B);
check(worker_status, B, _) -> chryso_abi_codec:check_worker_status(B);
check(request_journal, B, _) -> chryso_abi_codec:check_request_journal(B);
check(completion_journal, B, _) -> chryso_abi_codec:check_completion_journal(B).

%% The decoder's verdict: `ok` when it decodes, else its rejection.
decoded(spec_bank, B, Bank) ->
    case chryso_abi_codec:decode_spec_page(B) of
        {ok, #{banks := Banks}} when Bank < length(Banks) -> ok_or(lists:nth(Bank + 1, Banks));
        {ok, _} -> {error, range};
        {error, size} -> {error, size};
        %% A bad header hides the banks; the bank checker alone decides.
        {error, _} -> check(spec_bank, B, Bank)
    end;
decoded(Checker, B, _) ->
    ok_or(decode(Checker, B)).

ok_or({ok, _}) -> ok;
ok_or({error, _} = Error) -> Error.

decode(root_status_page, B) -> chryso_abi_codec:decode_root_status_page(B);
decode(spec_header, B) -> chryso_abi_codec:decode_spec_page(B);
decode(ctl_command, B) -> chryso_abi_codec:decode_ctl_command(B);
decode(ctl_reply, B) -> chryso_abi_codec:decode_ctl_reply(B);
decode(worker_identity, B) -> chryso_abi_codec:decode_worker_identity(B);
decode(worker_status, B) -> chryso_abi_codec:decode_worker_status(B);
decode(request_journal, B) -> chryso_abi_codec:decode_request_journal(B);
decode(completion_journal, B) -> chryso_abi_codec:decode_completion_journal(B).

roundtrip(root_status_page, B) ->
    chryso_abi_codec:root_status_page_to_binary(chryso_abi_codec:root_status_page_from_binary(B));
roundtrip(Spec, B) when Spec =:= spec_header; Spec =:= spec_bank ->
    chryso_abi_codec:spec_page_to_binary(chryso_abi_codec:spec_page_from_binary(B));
roundtrip(ctl_command, B) ->
    chryso_abi_codec:ctl_command_to_binary(chryso_abi_codec:ctl_command_from_binary(B));
roundtrip(ctl_reply, B) ->
    chryso_abi_codec:ctl_reply_to_binary(chryso_abi_codec:ctl_reply_from_binary(B));
roundtrip(worker_identity, B) ->
    chryso_abi_codec:worker_identity_to_binary(chryso_abi_codec:worker_identity_from_binary(B));
roundtrip(worker_status, B) ->
    chryso_abi_codec:worker_status_to_binary(chryso_abi_codec:worker_status_from_binary(B));
roundtrip(Journal, B) when Journal =:= request_journal; Journal =:= completion_journal ->
    chryso_abi_codec:journal_page_to_binary(chryso_abi_codec:journal_page_from_binary(B)).
