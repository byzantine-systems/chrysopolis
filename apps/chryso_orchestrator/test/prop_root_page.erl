-module(prop_root_page).
-moduledoc """
Laws of `chryso_root_page`: a valid page decodes to the rows it was encoded
from, and anything the codec rejects is `unknown`.

| Planted defect | Caught by |
|---|---|
| `budget` and `lifetime` swapped | prop_decodes_what_was_encoded |
| a torn page decoded anyway | prop_rejected_is_unknown |
""".
-include_lib("proper/include/proper.hrl").

-export([prop_decodes_what_was_encoded/0, prop_rejected_is_unknown/0]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

-define(DRIVERS, [0, 1, 2, 3]).

prop_decodes_what_was_encoded() ->
    ?FORALL(
        Page,
        page(),
        begin
            Got = chryso_observe:view(
                chryso_root_page:knowledge(chryso_status_bytes:encode(Page), ?DRIVERS)
            ),
            States = lists:usort([S || #{state := S} <- maps:values(maps:get(children, Page))]),
            aggregate(States, Got =:= Page)
        end
    ).

prop_rejected_is_unknown() ->
    ?FORALL(
        {Page, How},
        {page(), elements([torn, short, magic, version])},
        begin
            Bytes = break(How, chryso_status_bytes:encode(Page)),
            aggregate(
                [How], chryso_root_page:knowledge(Bytes, ?DRIVERS) =:= chryso_observe:unknown()
            )
        end
    ).

break(torn, Bytes) ->
    chryso_status_bytes:tear(Bytes);
break(short, Bytes) ->
    binary:part(Bytes, 0, byte_size(Bytes) - 1);
break(magic, <<_:8, Rest/binary>>) ->
    <<"X", Rest/binary>>;
break(version, <<Magic:8/binary, _:32, Rest/binary>>) ->
    <<Magic/binary, 2:32/little, Rest/binary>>.

%% Ranges reach past 32 bits where the wire field is 64 bits wide.
page() ->
    ?LET(
        {G, I, S, Now, F, Rows},
        {
            integer(1, 1 bsl 40),
            integer(1, 1 bsl 40),
            integer(1, 1 bsl 40),
            integer(0, 1 bsl 62),
            integer(1, 1 bsl 33),
            vector(length(?DRIVERS), row())
        },
        #{
            root_generation => G,
            beam_incarnation => I,
            seq => 2 * S,
            now => Now,
            cntfrq => F,
            children => maps:from_list(lists:zip(?DRIVERS, Rows))
        }
    ).

row() ->
    ?LET(
        {State, L, B, T},
        {
            elements([live, stopped, gone, quarantined]),
            integer(0, 1 bsl 32 - 1),
            integer(0, 1 bsl 32 - 1),
            integer(0, 1 bsl 62)
        },
        #{state => State, lifetime => L, budget => B, last_restart => T}
    ).
