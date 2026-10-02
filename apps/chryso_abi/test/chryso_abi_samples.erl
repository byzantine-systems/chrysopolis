-module(chryso_abi_samples).
-moduledoc "Valid sample pages, mirroring `tools/abi/samples.zig`. Each passes its checker.".

-include("orchestrator_abi.hrl").

-export([page/1, checkers/0, reseal_bank/2]).

-doc "Every generated checker, by name.".
-spec checkers() -> [atom()].
checkers() ->
    [
        root_status_page,
        spec_header,
        spec_bank,
        ctl_command,
        ctl_reply,
        worker_identity,
        worker_status,
        request_journal,
        completion_journal
    ].

-doc "A valid snapshot for the named checker.".
-spec page(atom()) -> binary().
page(root_status_page) ->
    Zero = chryso_abi_codec:root_status_page_from_binary(
        <<0:(?CHRYSO_ROOT_STATUS_PAGE_SIZE * 8)>>
    ),
    #{header := Header, children := [First | Rest]} = Zero,
    Event = #{
        ticks => 1,
        kind => ?CHRYSO_ROOT_EVENT_KIND_BOOT,
        child => 255,
        flags => 0,
        detail => 0,
        a => 0,
        b => 0
    },
    chryso_abi_codec:root_status_page_to_binary(Zero#{
        header := Header#{
            magic := ?CHRYSO_MAGIC_STATUS,
            abi_version := ?CHRYSO_ABI_VERSION,
            child_count := ?CHRYSO_CHILD_COUNT,
            seq := 2,
            root_generation := 1,
            beam_incarnation := 1,
            now_ticks := 9,
            cntfrq := 62500000,
            event_head := 130,
            event_dropped := 2,
            applied_spec_generation := 1
        },
        children := [
            First#{
                state := ?CHRYSO_ROOT_CHILD_WIRE_STATE_LIVE,
                desired := ?CHRYSO_ROOT_DESIRED_RUNNING
            }
            | Rest
        ],
        events := lists:duplicate(?CHRYSO_EVENT_COUNT, Event)
    });
page(Spec) when Spec =:= spec_header; Spec =:= spec_bank ->
    Header = chryso_abi_codec:spec_header_to_binary(#{
        magic => ?CHRYSO_MAGIC_SPEC,
        abi_version => ?CHRYSO_ABI_VERSION,
        active_bank => 0
    }),
    Bank = chryso_abi_codec:seal_spec_bank(
        chryso_abi_codec:spec_bank_to_binary(#{
            generation => 1,
            length => ?CHRYSO_SPEC_BANK_SIZE,
            crc32 => 0,
            record_count => ?CHRYSO_CHILD_COUNT,
            bank_seq => 2,
            budget => lists:duplicate(?CHRYSO_CHILD_COUNT, 3),
            desired => binary:copy(<<?CHRYSO_ROOT_DESIRED_RUNNING>>, ?CHRYSO_CHILD_COUNT)
        })
    ),
    <<Header/binary, Bank/binary, 0:(?CHRYSO_SPEC_BANK_SIZE * 8)>>;
page(ctl_command) ->
    chryso_abi_codec:ctl_command_to_binary(#{
        magic => ?CHRYSO_MAGIC_COMMAND,
        version => ?CHRYSO_ABI_VERSION,
        opcode => ?CHRYSO_PP_OPCODE_HELLO,
        args => [1, 2, 3, 4]
    });
page(ctl_reply) ->
    chryso_abi_codec:ctl_reply_to_binary(#{
        magic => ?CHRYSO_MAGIC_REPLY,
        version => ?CHRYSO_ABI_VERSION,
        result => ?CHRYSO_CTL_RESULT_OK,
        values => [5, 6, 7, 8]
    });
page(worker_identity) ->
    Header = chryso_abi_codec:worker_identity_header_to_binary(#{
        magic => ?CHRYSO_MAGIC_WORKER_IDENTITY,
        abi_version => ?CHRYSO_ABI_VERSION,
        slot => 3,
        class => 0,
        generation => 7,
        workload_ref => 11,
        desired_phase => ?CHRYSO_SLOT_PHASE_READY,
        completion_ack => 4
    }),
    pad(Header, ?CHRYSO_WORKER_IDENTITY_SIZE);
page(worker_status) ->
    Header = chryso_abi_codec:worker_status_header_to_binary(#{
        magic => ?CHRYSO_MAGIC_WORKER_STATUS,
        abi_version => ?CHRYSO_ABI_VERSION,
        slot => 3,
        class => 0,
        generation => 7,
        phase => ?CHRYSO_SLOT_PHASE_READY,
        health => ?CHRYSO_WORKER_HEALTH_HEALTHY,
        request_ack => 4,
        heartbeat_ticks => 100,
        accepted_count => 4,
        completed_count => 3,
        queue_depth => 1,
        failed_count => 0,
        status_seq => 6
    }),
    pad(Header, ?CHRYSO_WORKER_STATUS_SIZE);
page(request_journal) ->
    journal(?CHRYSO_MAGIC_REQUEST, ?CHRYSO_REQUEST_KIND_WORK);
page(completion_journal) ->
    journal(?CHRYSO_MAGIC_COMPLETION, ?CHRYSO_COMPLETION_KIND_WORK).

%% Sequences 6..20 are live, so the ring has wrapped; sequence 20 carries a full payload.
journal(Magic, Kind) ->
    Header = chryso_abi_codec:journal_header_to_binary(#{
        magic => Magic,
        abi_version => ?CHRYSO_ABI_VERSION,
        slot => 3,
        generation => 7,
        published_seq => 20,
        capacity => ?CHRYSO_JOURNAL_CAPACITY,
        entry_size => ?CHRYSO_JOURNAL_ENTRY_SIZE
    }),
    Slots = maps:from_list([
        {(Seq - 1) rem ?CHRYSO_JOURNAL_CAPACITY, entry(Seq, Kind)}
     || Seq <- lists:seq(6, 20)
    ]),
    Entries = <<
        <<(maps:get(Slot, Slots))/binary>>
     || Slot <- lists:seq(0, ?CHRYSO_JOURNAL_CAPACITY - 1)
    >>,
    <<Header/binary, Entries/binary>>.

entry(Seq, Kind) ->
    Length =
        case Seq of
            20 -> ?CHRYSO_JOURNAL_PAYLOAD_SIZE;
            _ -> Seq * 13 rem ?CHRYSO_JOURNAL_PAYLOAD_SIZE
        end,
    Payload = <<
        (binary:copy(<<Seq>>, Length))/binary, 0:((?CHRYSO_JOURNAL_PAYLOAD_SIZE - Length) * 8)
    >>,
    chryso_abi_codec:journal_entry_to_binary(#{
        generation => 7,
        sequence => Seq,
        request_id => Seq,
        workload_id => 1,
        deadline_ticks => 0,
        kind => Kind,
        status => 0,
        payload_length => Length,
        flags => 0,
        payload => Payload
    }).

pad(Bin, Size) ->
    <<Bin/binary, 0:((Size - byte_size(Bin)) * 8)>>.

-doc "Recomputes bank `Index`'s CRC after a test edits it.".
-spec reseal_bank(binary(), 0 | 1) -> binary().
reseal_bank(Page, Index) ->
    Offset = ?CHRYSO_SPEC_PAGE_BANKS_OFFSET + Index * ?CHRYSO_SPEC_BANK_SIZE,
    <<Pre:Offset/binary, Bank:?CHRYSO_SPEC_BANK_SIZE/binary, Post/binary>> = Page,
    <<Pre/binary, (chryso_abi_codec:seal_spec_bank(Bank))/binary, Post/binary>>.
