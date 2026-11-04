-module(chryso_sim_root).
-moduledoc """
Test-only simulated Root: a `gen_server` around the pure `chryso_root` model,
so a host node (a `peer`, or later a braid node) can stand in for a guest's
Root. `command/4` can drop its reply to model a lost PPC reply. The caller
then sees a timeout although the effect happened.

Trace points (Snabbkaffe): `sim_effect` for every command that reached the
restart path, with the child and the action Root decided.
""".
-behaviour(gen_server).

-include_lib("snabbkaffe/include/snabbkaffe.hrl").

-export([start/1, stop/0, observe/1, command/4]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2]).

-define(NAME, ?MODULE).

-doc "Start the simulated Root on this node, unlinked so it outlives the starting call.".
-spec start(chryso_root:config()) -> {ok, pid()} | {error, term()}.
start(Config) ->
    gen_server:start({local, ?NAME}, ?MODULE, Config, []).

-doc "Stop the simulated Root on this node.".
-spec stop() -> ok.
stop() ->
    gen_server:stop(?NAME).

-doc "Root's observable state on `Node`: records, retained policy, `seq`.".
-spec observe(node()) -> map().
observe(Node) ->
    gen_server:call({?NAME, Node}, observe, 1000).

-doc """
One command to the simulated Root on `Node`. With `drop_reply`, the effect
happens but no reply is sent, so the call exits with a timeout.
""".
-spec command(node(), non_neg_integer(), [non_neg_integer()], #{drop_reply := boolean()}) ->
    chryso_root:reply().
command(Node, Op, Args, #{drop_reply := Drop}) ->
    gen_server:call({?NAME, Node}, {command, Op, Args, Drop}, 300).

-doc false.
-spec init(chryso_root:config()) -> {ok, chryso_root:root()}.
init(Config) ->
    {ok, chryso_root:new(Config)}.

-doc false.
-spec handle_call(term(), gen_server:from(), chryso_root:root()) ->
    {reply, term(), chryso_root:root()} | {noreply, chryso_root:root()}.
handle_call(observe, _From, Root) ->
    {reply, chryso_root:observe(Root), Root};
handle_call({command, Op, Args, Drop}, _From, Root) ->
    {Reply, Decided, Root1} = chryso_root:command(Root, Op, 4, Args, 1000, absent),
    case Decided of
        #{child := Child, action := Action} ->
            ?tp(sim_effect, #{child => Child, action => Action});
        none ->
            ok
    end,
    case Drop of
        true -> {noreply, Root1};
        false -> {reply, Reply, Root1}
    end;
handle_call(_Unknown, _From, Root) ->
    {reply, {error, unsupported}, Root}.

-doc false.
-spec handle_cast(term(), chryso_root:root()) -> {noreply, chryso_root:root()}.
handle_cast(_Msg, Root) ->
    {noreply, Root}.

-doc false.
-spec handle_info(term(), chryso_root:root()) -> {noreply, chryso_root:root()}.
handle_info(_Info, Root) ->
    {noreply, Root}.
