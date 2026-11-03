-module(chryso_core_cluster_SUITE).
-moduledoc """
The fence law across nodes. A `peer` node hosts a simulated Root
(`chryso_sim_root`); this node sends fenced restart intents to it over
distribution. Replies may be lost (the effect happens, the reply does not
arrive) and the connection may drop between attempts. A caller retries the
same words until it gets a reply.

Law, observed through Snabbkaffe's trace of `sim_effect` and the replies:
every intent to a live driver has exactly one effect, and its final reply is
`ok` or `already_done`; an intent to a gone child has no effect and is
refused.
""".
-include_lib("proper/include/proper.hrl").
-include_lib("snabbkaffe/include/snabbkaffe.hrl").

-export([all/0]).
-export([fenced_intent_has_one_effect/1]).
-compile([nowarn_missing_spec, nowarn_missing_doc]).

all() ->
    [fenced_intent_has_one_effect].

%% CT runs each callback in its own process and peer:start_link links to the
%% caller, so the peer lives exactly as long as the test case.
start_peer() ->
    Ebin = filename:dirname(code:which(chryso_root)),
    {ok, Peer, Node} = peer:start_link(#{
        name => peer:random_name(sim),
        host => "127.0.0.1",
        longnames => true,
        %% A control channel outside distribution, so a dropped connection
        %% does not stop the peer.
        connection => standard_io,
        args => ["-pa", Ebin, "-setcookie", atom_to_list(erlang:get_cookie())]
    }),
    {Peer, Node}.

fenced_intent_has_one_effect(_Config) ->
    {Peer, Node} = start_peer(),
    try
        true = proper:quickcheck(law(Peer, Node), [{numtests, 40}, {to_file, user}])
    after
        peer:stop(Peer)
    end.

law(Peer, Node) ->
    ?FORALL(
        Plan,
        plan(),
        begin
            ok = peer:call(Peer, application, ensure_started, [snabbkaffe]),
            snabbkaffe:start_trace(),
            %% Runs on the collector: makes Node forward its trace here.
            ok = snabbkaffe:forward_trace(Node),
            {ok, _} = peer:call(Peer, chryso_sim_root, start, [config()]),
            try
                Outcomes = [intent(Node, Step) || Step <- Plan],
                Trace = snabbkaffe:collect_trace(200),
                Effects = ?projection(child, ?of_kind(sim_effect, Trace)),
                ?WHENFAIL(
                    ct:pal("plan=~p~noutcomes=~p~neffects=~p", [Plan, Outcomes, Effects]),
                    aggregate(
                        [L || {_, _, #{losses := L}} <- Outcomes],
                        lists:all(fun holds/1, Outcomes) andalso
                            Effects =:= [C || {C, live, _} <- Outcomes]
                    )
                )
            after
                peer:call(Peer, chryso_sim_root, stop, []),
                snabbkaffe:stop()
            end
        end
    ).

%% One intent: observe, then send the same fenced words until a reply
%% arrives, dropping replies and the connection as the plan says.
intent(Node, {Child, Losses, Disconnect}) ->
    #{records := Records, seq := Seq} = chryso_sim_root:observe(Node),
    #{state := State, lifetime := Lifetime} = maps:get(Child, Records),
    Args = [Child, 1, Seq, Lifetime],
    Reply = attempt(Node, Args, Losses, Disconnect),
    {Child, State, #{reply => Reply, losses => length(Losses)}}.

attempt(Node, Args, [Drop | Rest], Disconnect) ->
    try chryso_sim_root:command(Node, 2, Args, #{drop_reply => Drop}) of
        #{result := Result} -> Result
    catch
        exit:{timeout, _} ->
            Disconnect andalso erlang:disconnect_node(Node),
            attempt(Node, Args, Rest, Disconnect)
    end;
attempt(Node, Args, [], Disconnect) ->
    attempt(Node, Args, [false], Disconnect).

holds({_, live, #{reply := R}}) -> R =:= ok orelse R =:= already_done;
holds({_, gone, #{reply := R}}) -> R =:= refused.

config() ->
    #{
        mapped => true,
        now => 1000,
        tokens => 1000,
        refill => 1000,
        hard_max => 64,
        entry => 16#200000,
        pp => 11,
        beam => 5,
        present => 16#2f,
        drivers => 16#0f,
        compiled => [3, 3, 3, 3, 3, 2],
        generation => 1,
        incarnation => 1,
        seq => 2,
        abi => 1,
        cntfrq => 62500000
    }.

%% Intents to drivers, each with up to two lost replies before one that
%% arrives, and sometimes a dropped connection after a loss.
plan() ->
    list({integer(0, 3), list_of_losses(), boolean()}).

list_of_losses() ->
    ?LET(N, integer(0, 2), lists:duplicate(N, true)).
