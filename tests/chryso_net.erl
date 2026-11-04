-module(chryso_net).
-moduledoc """
Net-class probes for tcp-smoke and net-restart-smoke.

These drive the whole path: ERTS inet_drv -> libc sock.c ->
`src/pd/beam/io/tcp.c` -> lwIP -> sDDF net -> virtio-net/slirp. The test
driver process is itself the slirp host, so the peer on the other end is a
plain Python socket: the guest reaches it by dialling the gateway at
10.0.2.2, and the host reaches the guest through a hostfwd.

See `chryso_test` for why tags arrive as lower-case atoms and are upper-cased
here rather than by the caller.
""".

-include_lib("kernel/include/logger.hrl").

-export([ping/2, echo_server/1]).

-doc """
Guest -> host: connect through the slirp gateway, send the tag as the
payload, and report from the guest side. Asserting from both ends is the
point: the host must receive the bytes and the guest must print its own
success line, so a half-working path cannot pass.

The payload is the upper-cased tag, which is what lets one function serve
both callers: tcp-smoke's `chryso_ping` sends `CHRYSO_PING` and
net-restart-smoke's `before`/`after1`/`after2` send `BEFORE`/`AFTER1`/`AFTER2`,
each matching what its host-side listener already asserts.
""".
-spec ping(atom(), inet:port_number()) -> ok.
ping(Tag, Port) ->
    Name = chryso_test:tag(Tag),
    case gen_tcp:connect({10, 0, 2, 2}, Port, [binary, {active, false}], 5000) of
        {ok, Sock} ->
            ok = gen_tcp:send(Sock, list_to_binary(Name)),
            ok = gen_tcp:close(Sock),
            ?LOG_NOTICE("NET_OK|~s", [Name]);
        Err ->
            ?LOG_ERROR("NET_ERR|~s ~p", [Name, Err])
    end.

-doc """
Host -> guest: a looping echo server, so early half-open probes cannot
consume a one-shot acceptor. `{packet, raw}` because the payload is raw
bytes, not line-framed.
""".
-spec echo_server(inet:port_number()) -> pid().
echo_server(Port) ->
    spawn(fun() ->
        Opts = [binary, {packet, raw}, {active, false}, {reuseaddr, true}],
        {ok, Listen} = gen_tcp:listen(Port, Opts),
        ?LOG_NOTICE("LISTENER_UP"),
        accept_loop(Listen)
    end).

%% The hot path (accept -> recv -> send -> close) MUST stay free of logging.
%% Each console write costs about a second under TCG and two of them blow the
%% host's 3s socket timeout. The ECHOED event therefore lands after the close,
%% which keeps the round trip fast, and its absence is the regression signal
%% for the close-with-in-flight-ACK crash.
-spec accept_loop(gen_tcp:socket()) -> no_return().
accept_loop(Listen) ->
    case gen_tcp:accept(Listen, 30000) of
        {ok, Sock} ->
            case gen_tcp:recv(Sock, 0) of
                {ok, Bin} ->
                    gen_tcp:send(Sock, Bin),
                    gen_tcp:close(Sock),
                    ?LOG_NOTICE("ECHOED ~p", [Bin]);
                RecvErr ->
                    ?LOG_ERROR("RECV_ERR ~p", [RecvErr])
            end;
        AcceptErr ->
            ?LOG_ERROR("ACCEPT_ERR ~p", [AcceptErr])
    end,
    accept_loop(Listen).
