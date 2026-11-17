-module(chryso_status_bytes).
-moduledoc """
Test helper: a `chryso_observe` page as the bytes Root would publish, through
the codec, like `apps/chryso_abi/test/chryso_abi_samples.erl`. Children absent
from the page are zero rows (`unset`). No events are published.
""".

-include("orchestrator_abi.hrl").

-export([encode/1, tear/1]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

encode(#{
    root_generation := G,
    beam_incarnation := I,
    seq := S,
    now := Now,
    cntfrq := F,
    children := Children
}) ->
    Zero = chryso_abi_codec:root_status_page_from_binary(
        <<0:(?CHRYSO_ROOT_STATUS_PAGE_SIZE * 8)>>
    ),
    #{header := Header, children := Rows} = Zero,
    Rows1 = [row(maps:get(C, Children, none), Row) || {C, Row} <- lists:enumerate(0, Rows)],
    chryso_abi_codec:root_status_page_to_binary(Zero#{
        header := Header#{
            magic := ?CHRYSO_MAGIC_STATUS,
            abi_version := ?CHRYSO_ABI_VERSION,
            child_count := ?CHRYSO_CHILD_COUNT,
            seq := S,
            root_generation := G,
            beam_incarnation := I,
            now_ticks := Now,
            cntfrq := F
        },
        children := Rows1
    }).

%% The same page caught mid-publication: an odd seq.
tear(Bytes) ->
    <<Head:16/binary, Seq:64/little, Rest/binary>> = Bytes,
    <<Head/binary, (Seq bor 1):64/little, Rest/binary>>.

row(none, Zero) ->
    Zero;
row(#{state := State, lifetime := L, budget := B, last_restart := T}, Zero) ->
    Zero#{
        state := chryso_abi_codec:root_child_wire_state_value(State),
        desired := ?CHRYSO_ROOT_DESIRED_RUNNING,
        lifetime_count := L,
        effective_budget := B,
        last_restart_ticks := T
    }.
