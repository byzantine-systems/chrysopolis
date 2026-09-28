-module(chryso_abi_codec_tests).
-moduledoc """
One case per rejection class for every checker, mirroring the verdict table in
`tools/abi/checks.zig`, plus decoder, encoder and window edges.
""".
%% EUnit exports the test functions; they need no spec or doc.
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-include_lib("eunit/include/eunit.hrl").
-include("orchestrator_abi.hrl").

%% {Name, Checker, [{Offset, Byte}], Reseal, Bank, Expected}
cases() ->
    Events = ?CHRYSO_ROOT_STATUS_PAGE_EVENTS_OFFSET,
    Children = ?CHRYSO_ROOT_STATUS_PAGE_CHILDREN_OFFSET,
    Bank0 = ?CHRYSO_SPEC_PAGE_BANKS_OFFSET,
    Bank1 = Bank0 + ?CHRYSO_SPEC_BANK_SIZE,
    Entries = ?CHRYSO_JOURNAL_PAGE_ENTRIES_OFFSET,
    Newest = Entries + 4 * ?CHRYSO_JOURNAL_ENTRY_SIZE,
    [
        {"status ok", root_status_page, [], false, 0, ok},
        {"status magic", root_status_page, [{0, 0}], false, 0, {error, magic}},
        {"status version", root_status_page, [{?CHRYSO_ROOT_STATUS_HEADER_ABI_VERSION_OFFSET, 2}],
            false, 0, {error, version}},
        {"odd seq", root_status_page, [{?CHRYSO_ROOT_STATUS_HEADER_SEQ_OFFSET, 3}], false, 0,
            {error, torn}},
        {"unpublished", root_status_page, [{?CHRYSO_ROOT_STATUS_HEADER_SEQ_OFFSET, 0}], false, 0,
            {error, torn}},
        {"magic outranks torn", root_status_page,
            [{?CHRYSO_ROOT_STATUS_HEADER_SEQ_OFFSET, 3}, {7, 0}], false, 0, {error, magic}},
        {"header reserved", root_status_page, [{?CHRYSO_ROOT_STATUS_HEADER_RESERVED0_OFFSET, 1}],
            false, 0, {error, reserved}},
        {"row reserved", root_status_page,
            [
                {
                    Children + 3 * ?CHRYSO_ROOT_CHILD_STATUS_SIZE +
                        ?CHRYSO_ROOT_CHILD_STATUS_RESERVED_OFFSET,
                    1
                }
            ],
            false, 0, {error, reserved}},
        {"status tail", root_status_page, [{?CHRYSO_ROOT_STATUS_PAGE_SIZE - 1, 1}], false, 0,
            {error, reserved}},
        {"unknown child state", root_status_page, [{Children, 9}], false, 0, {error, unknown_kind}},
        {"unset live event", root_status_page, [{Events + ?CHRYSO_ROOT_EVENT_KIND_OFFSET, 0}],
            false, 0, {error, unknown_kind}},
        {"event outside window", root_status_page,
            [
                {?CHRYSO_ROOT_STATUS_HEADER_EVENT_HEAD_OFFSET, 3},
                {Events + 10 * ?CHRYSO_ROOT_EVENT_SIZE + ?CHRYSO_ROOT_EVENT_KIND_OFFSET, 0}
            ],
            false, 0, ok},
        {"applied bank", root_status_page, [{?CHRYSO_ROOT_STATUS_HEADER_APPLIED_BANK_OFFSET, 2}],
            false, 0, {error, range}},
        {"child count", root_status_page, [{?CHRYSO_ROOT_STATUS_HEADER_CHILD_COUNT_OFFSET, 61}],
            false, 0, {error, range}},
        {"event child", root_status_page, [{Events + ?CHRYSO_ROOT_EVENT_CHILD_OFFSET, 62}], false,
            0, {error, range}},
        {"spec header ok", spec_header, [], false, 0, ok},
        {"active bank", spec_header, [{?CHRYSO_SPEC_HEADER_ACTIVE_BANK_OFFSET, 2}], false, 0,
            {error, range}},
        {"spec header reserved", spec_header, [{?CHRYSO_SPEC_HEADER_RESERVED_OFFSET, 1}], false, 0,
            {error, reserved}},
        {"bank ok", spec_bank, [], false, 0, ok},
        {"bank selector", spec_bank, [], false, 2, {error, range}},
        {"inactive bank unpublished", spec_bank, [], false, 1, {error, torn}},
        {"odd inactive bank leaves bank 0", spec_bank,
            [{Bank1 + ?CHRYSO_SPEC_BANK_BANK_SEQ_OFFSET, 5}], false, 0, ok},
        {"bank crc", spec_bank, [{Bank0 + ?CHRYSO_SPEC_BANK_CRC32_OFFSET, 0}], false, 0,
            {error, checksum}},
        {"checksum outranks reserved", spec_bank,
            [{Bank0 + ?CHRYSO_SPEC_BANK_RESERVED_TAIL_OFFSET, 1}], false, 0, {error, checksum}},
        {"sealed bank reserved", spec_bank, [{Bank0 + ?CHRYSO_SPEC_BANK_RESERVED_TAIL_OFFSET, 1}],
            true, 0, {error, reserved}},
        {"bank seq excluded from crc", spec_bank, [{Bank0 + ?CHRYSO_SPEC_BANK_BANK_SEQ_OFFSET, 4}],
            false, 0, ok},
        {"clamped values pass", spec_bank,
            [
                {Bank0 + ?CHRYSO_SPEC_BANK_BUDGET_OFFSET + 3, 16#ff},
                {Bank0 + ?CHRYSO_SPEC_BANK_DESIRED_OFFSET, 9}
            ],
            true, 0, ok},
        {"bank length", spec_bank, [{Bank0 + ?CHRYSO_SPEC_BANK_LENGTH_OFFSET, 0}], true, 0,
            {error, range}},
        {"ctl ok", ctl_command, [], false, 0, ok},
        {"ctl version", ctl_command, [{?CHRYSO_CTL_COMMAND_VERSION_OFFSET, 2}], false, 0,
            {error, version}},
        {"ctl unset opcode", ctl_command, [{?CHRYSO_CTL_COMMAND_OPCODE_OFFSET, 0}], false, 0,
            {error, unknown_kind}},
        {"ctl unknown opcode", ctl_command, [{?CHRYSO_CTL_COMMAND_OPCODE_OFFSET, 8}], false, 0,
            {error, unknown_kind}},
        {"reply ok", ctl_reply, [], false, 0, ok},
        {"reply reserved", ctl_reply, [{?CHRYSO_CTL_REPLY_RESERVED_OFFSET, 1}], false, 0,
            {error, reserved}},
        {"identity ok", worker_identity, [], false, 0, ok},
        {"identity generation", worker_identity,
            [{?CHRYSO_WORKER_IDENTITY_HEADER_GENERATION_OFFSET, 0}], false, 0, {error, torn}},
        {"identity phase", worker_identity,
            [{?CHRYSO_WORKER_IDENTITY_HEADER_DESIRED_PHASE_OFFSET, 0}], false, 0,
            {error, unknown_kind}},
        {"worker status ok", worker_status, [], false, 0, ok},
        {"worker status odd seq", worker_status,
            [{?CHRYSO_WORKER_STATUS_HEADER_STATUS_SEQ_OFFSET, 7}], false, 0, {error, torn}},
        {"worker health", worker_status, [{?CHRYSO_WORKER_STATUS_HEADER_HEALTH_OFFSET, 3}], false,
            0, {error, unknown_kind}},
        {"request ok", request_journal, [], false, 0, ok},
        {"completion ok with status OK", completion_journal, [], false, 0, ok},
        {"journal generation", request_journal, [{?CHRYSO_JOURNAL_HEADER_GENERATION_OFFSET, 0}],
            false, 0, {error, torn}},
        {"stale entry sequence", request_journal,
            [{Entries + ?CHRYSO_JOURNAL_ENTRY_SEQUENCE_OFFSET, 1}], false, 0, {error, torn}},
        {"entry flags", request_journal, [{Entries + ?CHRYSO_JOURNAL_ENTRY_FLAGS_OFFSET, 1}], false,
            0, {error, reserved}},
        {"request status", request_journal, [{Entries + ?CHRYSO_JOURNAL_ENTRY_STATUS_OFFSET, 1}],
            false, 0, {error, reserved}},
        {"byte after payload", request_journal,
            [{Entries + ?CHRYSO_JOURNAL_ENTRY_PAYLOAD_OFFSET + 150, 1}], false, 0,
            {error, reserved}},
        {"shortened payload exposes its tail", request_journal,
            [{Newest + ?CHRYSO_JOURNAL_ENTRY_PAYLOAD_LENGTH_OFFSET, 191}], false, 0,
            {error, reserved}},
        {"request kind", request_journal, [{Entries + ?CHRYSO_JOURNAL_ENTRY_KIND_OFFSET, 0}], false,
            0, {error, unknown_kind}},
        {"completion kind gap", completion_journal,
            [{Entries + ?CHRYSO_JOURNAL_ENTRY_KIND_OFFSET, 0}], false, 0, {error, unknown_kind}},
        {"overlong payload", request_journal,
            [{Entries + ?CHRYSO_JOURNAL_ENTRY_PAYLOAD_LENGTH_OFFSET, 193}], false, 0,
            {error, range}},
        {"journal capacity", completion_journal, [{?CHRYSO_JOURNAL_HEADER_CAPACITY_OFFSET, 16}],
            false, 0, {error, range}}
    ].

verdict_test_() ->
    [
        {Name, fun() ->
            ?assertEqual(Expected, check(Checker, patch(Checker, Patches, Reseal), Bank))
        end}
     || {Name, Checker, Patches, Reseal, Bank, Expected} <- cases()
    ].

patch(Checker, Patches, Reseal) ->
    Page = lists:foldl(
        fun({Offset, Byte}, Acc) ->
            <<Pre:Offset/binary, _, Post/binary>> = Acc,
            <<Pre/binary, Byte, Post/binary>>
        end,
        chryso_abi_samples:page(Checker),
        Patches
    ),
    case Reseal of
        true -> chryso_abi_samples:reseal_bank(Page, 0);
        false -> Page
    end.

check(spec_bank, Page, Bank) ->
    chryso_abi_codec:check_spec_bank(Page, Bank);
check(Checker, Page, _Bank) ->
    Name = list_to_atom("check_" ++ atom_to_list(Checker)),
    chryso_abi_codec:Name(Page).

wrong_size_is_size_first_test() ->
    ?assertEqual({error, size}, chryso_abi_codec:check_spec_bank(<<0:512>>, 9)),
    ?assertEqual({error, size}, chryso_abi_codec:check_ctl_command(not_a_binary)),
    [
        ?assertEqual({error, size}, check(C, <<(chryso_abi_samples:page(C))/binary, 0>>, 0))
     || C <- chryso_abi_samples:checkers()
    ].

crc_check_value_test() ->
    ?assertEqual(16#CBF43926, erlang:crc32(<<"123456789">>)).

digest_test() ->
    ?assertEqual(?CHRYSO_ABI_LAYOUT_DIGEST, chryso_abi_codec:layout_digest()).

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

journal_window_at_the_maximum_sequence_test() ->
    Max = 16#FFFFFFFFFFFFFFFF,
    Page0 = chryso_abi_samples:page(request_journal),
    Header = chryso_abi_codec:journal_header_from_binary(
        binary:part(Page0, 0, ?CHRYSO_JOURNAL_HEADER_SIZE)
    ),
    Slots = maps:from_list([
        {
            (Seq - 1) rem ?CHRYSO_JOURNAL_CAPACITY,
            chryso_abi_codec:journal_entry_to_binary(#{
                generation => 7,
                sequence => Seq,
                request_id => 0,
                workload_id => 0,
                deadline_ticks => 0,
                kind => ?CHRYSO_REQUEST_KIND_DRAIN,
                status => 0,
                payload_length => 0,
                flags => 0,
                payload => <<0:(?CHRYSO_JOURNAL_PAYLOAD_SIZE * 8)>>
            })
        }
     || Seq <- lists:seq(Max - ?CHRYSO_JOURNAL_CAPACITY + 1, Max)
    ]),
    Page = <<
        (chryso_abi_codec:journal_header_to_binary(Header#{published_seq := Max}))/binary,
        <<<<(maps:get(S, Slots))/binary>> || S <- lists:seq(0, ?CHRYSO_JOURNAL_CAPACITY - 1)>>/binary
    >>,
    ?assertEqual(ok, chryso_abi_codec:check_request_journal(Page)).

spec_page_reports_banks_independently_test() ->
    Page = chryso_abi_samples:page(spec_header),
    ?assertMatch(
        {ok, #{banks := [{ok, #{generation := 1}}, {error, torn}]}},
        chryso_abi_codec:decode_spec_page(Page)
    ).

encoders_reject_out_of_range_values_test() ->
    Command = #{magic => ?CHRYSO_MAGIC_COMMAND, version => 1, opcode => 1, args => [0, 0, 0, 0]},
    ?assertError(badarg, chryso_abi_codec:ctl_command_to_binary(Command#{version := 16#10000})),
    ?assertError(badarg, chryso_abi_codec:ctl_command_to_binary(Command#{args := [0, 0, 0]})),
    ?assertError(badarg, chryso_abi_codec:ctl_command_to_binary(Command#{magic := <<"SHORT">>})).

enum_conversions_test() ->
    ?assertEqual({ok, hello}, chryso_abi_codec:pp_opcode_to_atom(?CHRYSO_PP_OPCODE_HELLO)),
    ?assertEqual(error, chryso_abi_codec:completion_kind_to_atom(16#8000)),
    ?assertEqual(16#8001, chryso_abi_codec:completion_kind_value(work)).
