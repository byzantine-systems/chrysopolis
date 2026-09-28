-module(chryso_abi_prop_tests).
-moduledoc "PropEr properties over the generated codec, run from EUnit.".
%% EUnit and PropEr export the test and property functions; they need no spec or doc.
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-include_lib("proper/include/proper.hrl").
-include_lib("eunit/include/eunit.hrl").
-include("orchestrator_abi.hrl").

-define(NUMTESTS, 300).

%% Record size by name, from the generated header.
records() ->
    [
        {root_status_header, ?CHRYSO_ROOT_STATUS_HEADER_SIZE},
        {root_child_status, ?CHRYSO_ROOT_CHILD_STATUS_SIZE},
        {root_event, ?CHRYSO_ROOT_EVENT_SIZE},
        {spec_header, ?CHRYSO_SPEC_HEADER_SIZE},
        {spec_bank, ?CHRYSO_SPEC_BANK_SIZE},
        {ctl_command, ?CHRYSO_CTL_COMMAND_SIZE},
        {ctl_reply, ?CHRYSO_CTL_REPLY_SIZE},
        {worker_identity_header, ?CHRYSO_WORKER_IDENTITY_HEADER_SIZE},
        {worker_status_header, ?CHRYSO_WORKER_STATUS_HEADER_SIZE},
        {journal_header, ?CHRYSO_JOURNAL_HEADER_SIZE},
        {journal_entry, ?CHRYSO_JOURNAL_ENTRY_SIZE}
    ].

codec(Record, Direction) ->
    Name = list_to_atom(atom_to_list(Record) ++ Direction),
    fun chryso_abi_codec:Name/1.

%% Decoding then encoding any bytes, then decoding again, reaches the same map:
%% every non-reserved field lands where the decoder read it.
prop_roundtrip() ->
    ?FORALL(
        {Record, Size},
        oneof(records()),
        ?FORALL(
            Bin,
            binary(Size),
            begin
                From = codec(Record, "_from_binary"),
                To = codec(Record, "_to_binary"),
                Map = From(Bin),
                From(To(Map)) =:= Map
            end
        )
    ).

check(spec_bank, Bin) ->
    chryso_abi_codec:check_spec_bank(Bin, 0);
check(Checker, Bin) ->
    Name = list_to_atom("check_" ++ atom_to_list(Checker)),
    chryso_abi_codec:Name(Bin).

is_verdict(ok) ->
    true;
is_verdict({error, Class}) ->
    lists:member(Class, [size, magic, version, torn, checksum, reserved, unknown_kind, range]);
is_verdict(_) ->
    false.

%% Checkers are total: any binary, of any size, yields a verdict and never raises.
prop_total() ->
    ?FORALL(
        {Checker, Bin},
        {oneof(chryso_abi_samples:checkers()), binary()},
        is_verdict(check(Checker, Bin))
    ).

%% Right-sized noise reaches the rule checks instead of stopping at size.
prop_total_right_size() ->
    ?FORALL(
        Checker,
        oneof(chryso_abi_samples:checkers()),
        ?FORALL(
            Bin,
            binary(byte_size(chryso_abi_samples:page(Checker))),
            is_verdict(check(Checker, Bin))
        )
    ).

%% Reserved spans of each valid page, as {Offset, Length}.
reserved_spans(ctl_command) ->
    [{?CHRYSO_CTL_COMMAND_RESERVED0_OFFSET, 4}, {?CHRYSO_CTL_COMMAND_RESERVED_OFFSET, 16}];
reserved_spans(worker_status) ->
    [
        {?CHRYSO_WORKER_STATUS_HEADER_RESERVED0_OFFSET, 4},
        {?CHRYSO_WORKER_STATUS_RESERVED_TAIL_OFFSET,
            ?CHRYSO_WORKER_STATUS_SIZE - ?CHRYSO_WORKER_STATUS_RESERVED_TAIL_OFFSET}
    ];
reserved_spans(root_status_page) ->
    [
        {?CHRYSO_ROOT_STATUS_HEADER_RESERVED0_OFFSET, 4},
        {?CHRYSO_ROOT_STATUS_PAGE_RESERVED_TAIL_OFFSET,
            ?CHRYSO_ROOT_STATUS_PAGE_SIZE - ?CHRYSO_ROOT_STATUS_PAGE_RESERVED_TAIL_OFFSET}
    ];
reserved_spans(spec_bank) ->
    Tail = ?CHRYSO_SPEC_PAGE_BANKS_OFFSET + ?CHRYSO_SPEC_BANK_RESERVED_TAIL_OFFSET,
    [{Tail, ?CHRYSO_SPEC_BANK_SIZE - ?CHRYSO_SPEC_BANK_RESERVED_TAIL_OFFSET}].

%% One nonzero byte in a reserved span of a valid page is exactly `reserved`
%% (after resealing the bank, since its CRC covers the tail).
prop_reserved_byte() ->
    ?FORALL(
        {Checker, Pick, Byte},
        {
            oneof([ctl_command, worker_status, root_status_page, spec_bank]),
            non_neg_integer(),
            range(1, 255)
        },
        begin
            Spans = reserved_spans(Checker),
            {Offset, Length} = lists:nth(Pick rem length(Spans) + 1, Spans),
            At = Offset + Pick rem Length,
            <<Pre:At/binary, _, Post/binary>> = chryso_abi_samples:page(Checker),
            Page0 = <<Pre/binary, Byte, Post/binary>>,
            Page =
                case Checker of
                    spec_bank -> chryso_abi_samples:reseal_bank(Page0, 0);
                    _ -> Page0
                end,
            check(Checker, Page) =:= {error, reserved}
        end
    ).

properties_test_() ->
    {timeout, 120, [
        {
            atom_to_list(Name),
            ?_assert(
                proper:quickcheck(?MODULE:Name(), [
                    {numtests, ?NUMTESTS}, {to_file, user}, long_result
                ]) =:= true
            )
        }
     || Name <- [prop_roundtrip, prop_total, prop_total_right_size, prop_reserved_byte]
    ]}.
