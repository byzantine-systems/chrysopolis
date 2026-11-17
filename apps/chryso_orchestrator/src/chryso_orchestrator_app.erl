-module(chryso_orchestrator_app).
-moduledoc "Reads the application environment and starts the tree.".
-behaviour(application).

-export([start/2, stop/1, config/0]).

-define(DEFAULTS, #{
    device => chryso_root_files,
    drivers => [0, 1, 2, 3],
    scan_ms => 250,
    deadline_ms => 1000,
    backoff_ms => {100, 1600},
    settle_ms => 500,
    floor => 1
}).

-doc false.
-spec start(application:start_type(), term()) -> supervisor:startlink_ret().
start(_Type, _Args) ->
    #{device := Device} = Config = config(),
    chryso_orchestrator_sup:start_link(Device, maps:remove(device, Config)).

-doc false.
-spec stop(term()) -> ok.
stop(_State) ->
    _ = persistent_term:erase({chryso_orchestrator, wants}),
    ok.

-doc "The defaults, overridden by the application environment.".
-spec config() -> map().
config() ->
    maps:map(
        fun(Key, Default) -> application:get_env(chryso_orchestrator, Key, Default) end,
        ?DEFAULTS
    ).
