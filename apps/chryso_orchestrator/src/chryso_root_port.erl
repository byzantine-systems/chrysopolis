-module(chryso_root_port).
-moduledoc """
The sole owner of Root's devices. Reads return knowledge, never bytes.
Commands are casts tagged with the controller's reference, and the reply
comes back as `{chryso_reply, Ref, Result}`.

- `/dev/chrysoctl` has one opener, so the port holds it for one command
  only: open, write, read the reply, close. Other callers (`chryso_ctl`
  probes, a SYNC flood) get it between commands.
- A command still queued when its deadline passes is dropped unsent.
- A local error (`ebusy` on open, `eio`, a frame the codec rejects) sends no
  reply. Root may have acted, so the controller's deadline fires and its next
  read settles the outcome.
- Calls block this process, and a PPC blocks the whole TCB, so the controller
  never waits on a command: it only calls `read/2`.
""".
-behaviour(gen_server).

-include("orchestrator_abi.hrl").
-include_lib("kernel/include/logger.hrl").

-export([start_link/1, read/2, command/5, sync/1]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2]).

-define(NAME, ?MODULE).
-define(META, #{domain => [chryso, orchestrator]}).

-doc "Start the port over `Device`, a `chryso_root_device` module.".
-spec start_link(Device :: module()) -> gen_server:start_ret().
start_link(Device) ->
    gen_server:start_link({local, ?NAME}, ?MODULE, Device, []).

-doc "One read of Root's status, as knowledge of `Drivers`. Exits on timeout.".
-spec read(Drivers :: [non_neg_integer()], Timeout :: timeout()) -> chryso_observe:knowledge().
read(Drivers, Timeout) ->
    gen_server:call(?NAME, {read, Drivers}, Timeout).

-doc """
Restart `Child` fenced by `Fence`, unless the monotonic millisecond `At` has
passed when the port gets to it. The reply goes to `ReplyTo`.
""".
-spec command(
    Ref :: pos_integer(),
    Child :: non_neg_integer(),
    Fence :: chryso_plan:fence(),
    At :: integer(),
    ReplyTo :: pid()
) -> ok.
command(Ref, Child, Fence, At, ReplyTo) ->
    gen_server:cast(?NAME, {command, Ref, Child, Fence, At, ReplyTo}).

-doc "Returns once every earlier cast has been handled.".
-spec sync(Timeout :: timeout()) -> ok.
sync(Timeout) ->
    gen_server:call(?NAME, sync, Timeout).

%% --- callbacks ------------------------------------------------------------------

%% Opens nothing: the channel is held per command, and a raw file closes with
%% its owner if the port dies mid-call.
-doc false.
-spec init(module()) -> {ok, map()}.
init(Device) ->
    {ok, #{device => Device}}.

-doc false.
-spec handle_call(term(), gen_server:from(), map()) -> {reply, term(), map()}.
handle_call({read, Drivers}, _From, #{device := Device} = State) ->
    Known =
        case Device:read_status() of
            {ok, Bytes} -> chryso_root_page:knowledge(Bytes, Drivers);
            {error, _} -> chryso_observe:unknown()
        end,
    {reply, Known, State};
handle_call(sync, _From, State) ->
    {reply, ok, State};
handle_call(_Other, _From, State) ->
    {reply, {error, unknown_call}, State}.

-doc false.
-spec handle_cast(term(), map()) -> {noreply, map()}.
handle_cast({command, Ref, Child, Fence, At, ReplyTo}, State) ->
    case erlang:monotonic_time(millisecond) >= At of
        true ->
            ?LOG_INFO(#{what => command_expired, ref => Ref, child => Child}, ?META),
            {noreply, State};
        false ->
            ok = send(Ref, Child, Fence, ReplyTo, State),
            {noreply, State}
    end;
handle_cast(_Other, State) ->
    {noreply, State}.

-doc false.
-spec handle_info(term(), map()) -> {noreply, map()}.
handle_info(_Info, State) ->
    {noreply, State}.

%% --- internals ------------------------------------------------------------------

-spec send(pos_integer(), non_neg_integer(), chryso_plan:fence(), pid(), map()) -> ok.
send(Ref, Child, Fence, ReplyTo, #{device := Device}) ->
    #{root_generation := G, seq := S, lifetime := L} = Fence,
    Command = chryso_abi_codec:ctl_command_to_binary(#{
        magic => ?CHRYSO_MAGIC_COMMAND,
        version => ?CHRYSO_ABI_VERSION,
        opcode => ?CHRYSO_PP_OPCODE_RESTART_CHILD,
        args => [Child, G, S, L]
    }),
    case Device:open_ctl() of
        {ok, H} ->
            Reply = result(Device:call(H, Command)),
            ok = Device:close(H),
            reply(Reply, Ref, Child, ReplyTo);
        {error, Reason} ->
            reply({error, {open, Reason}}, Ref, Child, ReplyTo)
    end.

-spec reply(
    {ok, chryso_lifecycle:result()} | {error, term()}, pos_integer(), non_neg_integer(), pid()
) ->
    ok.
reply({ok, Result}, Ref, Child, ReplyTo) ->
    ?LOG_INFO(#{what => command, ref => Ref, child => Child, result => Result}, ?META),
    ReplyTo ! {chryso_reply, Ref, Result},
    ok;
reply({error, Reason}, Ref, Child, _ReplyTo) ->
    ?LOG_NOTICE(#{what => command_lost, ref => Ref, child => Child, reason => Reason}, ?META),
    ok.

-spec result({ok, binary()} | {error, term()}) -> {ok, chryso_lifecycle:result()} | {error, term()}.
result({ok, Reply}) ->
    case chryso_abi_codec:decode_ctl_reply(Reply) of
        {ok, #{result := Value}} ->
            case chryso_abi_codec:ctl_result_to_atom(Value) of
                {ok, Result} -> {ok, Result};
                error -> {error, {bad_result, Value}}
            end;
        {error, Reject} ->
            {error, Reject}
    end;
result({error, _} = Error) ->
    Error.
