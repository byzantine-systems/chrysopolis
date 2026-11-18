-module(chryso_controller_sup).
-moduledoc """
The controllers, `one_for_one`: each is independent and reads Root afresh
when it restarts. Today that is the driver lifecycle. Slot controllers join
here later.
""".
-behaviour(supervisor).

-export([start_link/1, init/1]).

-doc "Start the controllers with `Config`.".
-spec start_link(Config :: chryso_lifecycle_ctl:config()) -> supervisor:startlink_ret().
start_link(Config) ->
    supervisor:start_link({local, ?MODULE}, ?MODULE, Config).

-doc false.
-spec init(chryso_lifecycle_ctl:config()) ->
    {ok, {supervisor:sup_flags(), [supervisor:child_spec()]}}.
init(Config) ->
    Flags = #{strategy => one_for_one, intensity => 5, period => 10},
    Children = [
        #{
            id => chryso_lifecycle_ctl,
            start => {chryso_lifecycle_ctl, start_link, [Config]},
            shutdown => 2000
        }
    ],
    {ok, {Flags, Children}}.
