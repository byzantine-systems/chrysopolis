-module(chryso_abi_codec_tests).
-moduledoc """
Decoder, encoder and conversion behaviour of the generated codec. Verdicts,
precedence and window edges are held by the shared golden vectors in
`chryso_abi_vectors_tests`.
""".
%% EUnit exports the test functions; they need no spec or doc.
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-include_lib("eunit/include/eunit.hrl").
-include("orchestrator_abi.hrl").

non_binary_input_is_size_test() ->
    ?assertEqual({error, size}, chryso_abi_codec:check_ctl_command(not_a_binary)),
    ?assertEqual({error, size}, chryso_abi_codec:check_spec_bank(<<0:512>>, 9)).

decode_status_returns_live_events_test() ->
    {ok, #{header := Header, events := Events}} =
        chryso_abi_codec:decode_root_status_page(chryso_abi_samples:page(root_status_page)),
    ?assertEqual(1, maps:get(applied_spec_generation, Header)),
    ?assertEqual(?CHRYSO_EVENT_COUNT, length(Events)).

decode_journal_returns_entries_in_sequence_order_test() ->
    {ok, #{entries := Entries}} =
        chryso_abi_codec:decode_request_journal(chryso_abi_samples:page(request_journal)),
    ?assertEqual(lists:seq(6, 20), [maps:get(sequence, E) || E <- Entries]),
    Newest = lists:last(Entries),
    ?assertEqual(?CHRYSO_JOURNAL_PAYLOAD_SIZE, byte_size(maps:get(payload, Newest))),
    ?assertEqual(78, byte_size(maps:get(payload, hd(Entries)))).

spec_page_reports_banks_independently_test() ->
    Page = chryso_abi_samples:page(spec_header),
    ?assertMatch(
        {ok, #{banks := [{ok, #{generation := 1}}, {error, torn}]}},
        chryso_abi_codec:decode_spec_page(Page)
    ).

%% Breaks the encoder's contract on purpose: the runtime check, not the spec, is under test.
-dialyzer({nowarn_function, encoders_reject_out_of_range_values_test/0}).
encoders_reject_out_of_range_values_test() ->
    Command = #{magic => ?CHRYSO_MAGIC_COMMAND, version => 1, opcode => 1, args => [0, 0, 0, 0]},
    ?assertError(badarg, chryso_abi_codec:ctl_command_to_binary(Command#{version := 16#10000})),
    ?assertError(badarg, chryso_abi_codec:ctl_command_to_binary(Command#{args := [0, 0, 0]})),
    ?assertError(badarg, chryso_abi_codec:ctl_command_to_binary(Command#{magic := <<"SHORT">>})).

enum_conversions_test() ->
    ?assertEqual({ok, hello}, chryso_abi_codec:pp_opcode_to_atom(?CHRYSO_PP_OPCODE_HELLO)),
    ?assertEqual(error, chryso_abi_codec:completion_kind_to_atom(16#8000)),
    ?assertEqual(16#8001, chryso_abi_codec:completion_kind_value(work)).
