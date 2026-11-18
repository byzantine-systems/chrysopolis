-module(chryso_orchestrator_app).
-moduledoc """
Reads the application environment, validates it, and starts the tree. A bad
value fails startup with `{bad_config, Key, Value}` instead of crashing the
controller later.

Law (`prop_config`): the defaults validate, and a config with exactly one bad
key fails naming that key.
""".
-behaviour(application).

-export([start/2, stop/1, config/0, validate/1]).
-export_type([settings/0]).

-define(IS_POS(V), (is_integer(V) andalso V > 0)).
-define(IS_NON_NEG(V), (is_integer(V) andalso V >= 0)).

-define(DEFAULTS, #{
    device => chryso_root_files,
    drivers => [0, 1, 2, 3],
    scan_ms => 250,
    deadline_ms => 1000,
    backoff_ms => {100, 1600},
    settle_ms => 500,
    floor => 1
}).

-doc "A validated configuration: the device module and the controller's config.".
-type settings() :: #{device := module(), controller := chryso_lifecycle_ctl:config()}.

-doc false.
-spec start(application:start_type(), term()) -> supervisor:startlink_ret().
start(_Type, _Args) ->
    case validate(config()) of
        {ok, #{device := Device, controller := Config}} ->
            chryso_orchestrator_sup:start_link(Device, Config);
        {error, _} = Error ->
            Error
    end.

-doc false.
-spec stop(term()) -> ok.
stop(_State) ->
    _ = persistent_term:erase({chryso_orchestrator, wants}),
    ok.

-doc "The defaults, overridden by the application environment. Not yet validated.".
-spec config() -> #{atom() => term()}.
config() ->
    maps:map(
        fun(Key, Default) -> application:get_env(chryso_orchestrator, Key, Default) end,
        ?DEFAULTS
    ).

-doc "Check every key, in a fixed order, and report the first bad one.".
-spec validate(#{atom() => term()}) -> {ok, settings()} | {error, {bad_config, atom(), term()}}.
validate(Config) ->
    Bad = [
        {K, V}
     || K <:- lists:sort(maps:keys(?DEFAULTS)),
        V <- [maps:get(K, Config, missing)],
        not valid(K, V)
    ],
    case Bad of
        [] ->
            {Device, Controller} = maps:take(device, maps:with(maps:keys(?DEFAULTS), Config)),
            {ok, #{device => Device, controller => Controller}};
        [{K, V} | _] ->
            {error, {bad_config, K, V}}
    end.

-spec valid(atom(), term()) -> boolean().
valid(device, V) when is_atom(V) ->
    V =/= missing;
valid(drivers, [_ | _] = V) ->
    lists:all(fun(D) -> ?IS_NON_NEG(D) end, V) andalso length(V) =:= length(lists:usort(V));
valid(backoff_ms, {First, Largest}) when ?IS_POS(First), ?IS_POS(Largest) ->
    First =< Largest;
valid(K, V) when (K =:= scan_ms orelse K =:= deadline_ms), ?IS_POS(V) ->
    true;
valid(K, V) when (K =:= settle_ms orelse K =:= floor), ?IS_NON_NEG(V) ->
    true;
valid(_Key, _Value) ->
    false.
