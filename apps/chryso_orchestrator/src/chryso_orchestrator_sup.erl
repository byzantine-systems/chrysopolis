-module(chryso_orchestrator_sup).
-moduledoc """
The top of the tree, `rest_for_one`:

1. `chryso_root_port`, the one owner of Root's devices.
2. `chryso_controller_sup`, every controller that commands through it.

A port crash restarts every controller, each starting again from `unknown`.
A controller crash spends its own supervisor's budget and leaves the port and
its open channel alone.
""".
-behaviour(supervisor).

-export([start_link/2, init/1]).

-doc "Start the tree over `Device` with the controllers' `Config`.".
-spec start_link(Device :: module(), Config :: chryso_lifecycle_ctl:config()) ->
    supervisor:startlink_ret().
start_link(Device, Config) ->
    supervisor:start_link({local, ?MODULE}, ?MODULE, {Device, Config}).

-doc false.
-spec init({module(), chryso_lifecycle_ctl:config()}) ->
    {ok, {supervisor:sup_flags(), [supervisor:child_spec()]}}.
init({Device, Config}) ->
    Flags = #{strategy => rest_for_one, intensity => 3, period => 10},
    Children = [
        #{
            id => chryso_root_port,
            start => {chryso_root_port, start_link, [Device]},
            shutdown => 2000
        },
        #{
            id => chryso_controller_sup,
            start => {chryso_controller_sup, start_link, [Config]},
            type => supervisor,
            shutdown => infinity
        }
    ],
    {ok, {Flags, Children}}.
