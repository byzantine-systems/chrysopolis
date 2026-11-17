-module(chryso_root_page).
-moduledoc """
A status page's bytes as `chryso_observe` knowledge of the watched drivers.
A page the codec rejects (torn, short, bad magic) or a row with no name is
`unknown`, and `unknown` is the merge identity, so a bad read loses nothing.

Law (`prop_root_page`): a valid page decodes to the rows it was encoded from.
""".

-export([knowledge/2]).

-doc "What `Bytes` says about `Drivers`, or `unknown`.".
-spec knowledge(Bytes :: binary(), Drivers :: [non_neg_integer()]) -> chryso_observe:knowledge().
knowledge(Bytes, Drivers) ->
    case chryso_abi_codec:decode_root_status_page(Bytes) of
        {ok, #{header := Header, children := Rows}} -> from_rows(Header, Rows, Drivers);
        {error, _} -> chryso_observe:unknown()
    end.

-spec from_rows(map(), [map()], [non_neg_integer()]) -> chryso_observe:knowledge().
from_rows(Header, Rows, Drivers) ->
    #{
        root_generation := G,
        beam_incarnation := I,
        seq := S,
        now_ticks := Now,
        cntfrq := F
    } = Header,
    Children = [{C, row(lists:nth(C + 1, Rows))} || C <- Drivers, C < length(Rows)],
    Page = #{
        root_generation => G,
        beam_incarnation => I,
        seq => S,
        now => Now,
        cntfrq => F,
        children => maps:from_list(Children)
    },
    case chryso_observe:from_page(Page) of
        {ok, Known} -> Known;
        {error, invalid} -> chryso_observe:unknown()
    end.

%% A row the codec cannot name stays a bad row, so from_page refuses the page.
-spec row(map()) -> map().
row(#{
    state := Wire,
    lifetime_count := L,
    effective_budget := B,
    last_restart_ticks := T
}) ->
    case chryso_abi_codec:root_child_wire_state_to_atom(Wire) of
        {ok, State} -> #{state => State, lifetime => L, budget => B, last_restart => T};
        error -> #{}
    end.
