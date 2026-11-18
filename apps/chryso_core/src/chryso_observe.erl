-module(chryso_observe).
-moduledoc """
What the orchestrator knows about Root: `unknown`, or the newest status page
seen, stamped `{root_generation, seq}`. Input is a page `chryso_abi_codec`
decoded, with states as atoms.

Laws, through `view/1`:

- **semilattice**: `merge` is associative, commutative and idempotent.
- **identity**: `unknown()` is its identity, so a failed read loses nothing.
- **newest wins**: the merge of two pages is the one with the greater stamp.

Non-laws: a newer `beam_incarnation` is a different reader, not a newer page.
Two pages with one stamp but different bytes are a Root fault, not a tie
(`root_status_observe` treats them so). `merge` still picks one, by term
order, so it stays total and deterministic, but neither choice is a claim.
""".

-export([unknown/0, from_page/1, merge/2, view/1, child/2, stamp/1]).
-export_type([page/0, child_row/0, knowledge/0, stamp/0, child_state/0]).

%% non_neg_integer() as a guard: types are not checked at run time, and there
%% is no guard BIF for it.
-define(IS_NON_NEG(X), (is_integer(X) andalso X >= 0)).

-doc "A child's state, as `RootChildWireState` names it.".
-type child_state() :: unset | live | stopped | gone | quarantined.

-doc "One child row, reduced to what decisions read.".
-type child_row() :: #{
    state := child_state(),
    lifetime := non_neg_integer(),
    budget := non_neg_integer(),
    last_restart := non_neg_integer()
}.

-doc """
A decoded status page. `children` holds the rows of the children the caller
cares about, keyed by child id. Ticks are raw counter reads, `cntfrq` per
second.
""".
-type page() :: #{
    root_generation := non_neg_integer(),
    beam_incarnation := non_neg_integer(),
    seq := non_neg_integer(),
    now := non_neg_integer(),
    cntfrq := non_neg_integer(),
    children := #{non_neg_integer() => child_row()}
}.

-doc "Root's generation and the page's published `seq`.".
-type stamp() :: {non_neg_integer(), non_neg_integer()}.

-doc "Nothing known, or the newest page seen.".
-opaque knowledge() :: unknown | {known, stamp(), page()}.

-doc "Knowing nothing: the merge identity.".
-spec unknown() -> knowledge().
%% Constructors: unknown/0 and from_page/1. Nothing else builds a knowledge().
unknown() ->
    unknown.

-doc """
Knowledge of exactly one page, or `{error, invalid}` when the term is not a
`page/0`: a missing or negative field, or a child state outside
`child_state/0`. The caller keeps what it knew, since an invalid read is
`unknown` and `unknown` is the merge identity.
""".
-spec from_page(Page :: term()) -> {ok, knowledge()} | {error, invalid}.
from_page(
    #{
        root_generation := G,
        beam_incarnation := I,
        seq := S,
        now := Now,
        cntfrq := F,
        children := Children
    } = Page
) when
    ?IS_NON_NEG(G),
    ?IS_NON_NEG(I),
    ?IS_NON_NEG(S),
    ?IS_NON_NEG(Now),
    ?IS_NON_NEG(F),
    is_map(Children)
->
    case lists:all(fun valid_child/1, maps:to_list(Children)) of
        true -> {ok, {known, {G, S}, Page}};
        false -> {error, invalid}
    end;
from_page(_) ->
    {error, invalid}.

-spec valid_child({term(), term()}) -> boolean().
valid_child(
    {Child, #{state := State, lifetime := L, budget := B, last_restart := T}}
) when ?IS_NON_NEG(Child), ?IS_NON_NEG(L), ?IS_NON_NEG(B), ?IS_NON_NEG(T) ->
    lists:member(State, [unset, live, stopped, gone, quarantined]);
valid_child(_) ->
    false.

-doc "The newer of two pieces of knowledge.".
-spec merge(A :: knowledge(), B :: knowledge()) -> knowledge().
merge(unknown, K) ->
    K;
merge(K, unknown) ->
    K;
merge({known, _, _} = A, {known, _, _} = B) ->
    max(A, B).

-doc "The page known, or `unknown`.".
-spec view(Knowledge :: knowledge()) -> unknown | page().
view(unknown) ->
    unknown;
view({known, _, Page}) ->
    Page.

-doc "One child's row, or `unknown` when nothing is known or the page lacks it.".
-spec child(Knowledge :: knowledge(), Child :: non_neg_integer()) -> unknown | child_row().
child(unknown, _Child) ->
    unknown;
child({known, _, #{children := Children}}, Child) ->
    maps:get(Child, Children, unknown).

-doc "The stamp known, or `unknown`.".
-spec stamp(Knowledge :: knowledge()) -> unknown | stamp().
stamp(unknown) ->
    unknown;
stamp({known, Stamp, _}) ->
    Stamp.
