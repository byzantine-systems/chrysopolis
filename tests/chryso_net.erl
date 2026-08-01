%% Net-class probes for tcp-smoke and net-restart-smoke.
%%
%% These drive the whole path: ERTS inet_drv -> libc sock.c -> src/runtime/tcp.c
%% -> lwIP -> sDDF net -> virtio-net/slirp. The test driver process is itself
%% the slirp host, so the peer on the other end is a plain Python socket: the
%% guest reaches it by dialling the gateway at 10.0.2.2, and the host reaches
%% the guest through a hostfwd.
%%
%% See chryso_test for why tags arrive as lower-case atoms and are upper-cased
%% here rather than by the caller.
-module(chryso_net).

-export([ping/2, echo_server/1]).

%% Guest -> host: connect through the slirp gateway, send the tag as the
%% payload, and report from the guest side. Asserting from BOTH ends is the
%% point: the host must receive the bytes and the guest must print its own
%% success line, so a half-working path cannot pass.
%%
%% The payload IS the upper-cased tag, which is what lets one function serve
%% both callers: tcp-smoke's chryso_ping sends "CHRYSO_PING" and
%% net-restart-smoke's before/after1/after2 send "BEFORE"/"AFTER1"/"AFTER2",
%% each matching what its host-side listener already asserts.
-spec ping(atom(), inet:port_number()) -> ok.
ping(Tag, Port) ->
    Name = chryso_test:tag(Tag),
    case gen_tcp:connect({10, 0, 2, 2}, Port, [binary, {active, false}], 5000) of
        {ok, Sock} ->
            ok = gen_tcp:send(Sock, list_to_binary(Name)),
            ok = gen_tcp:close(Sock),
            io:format("NET_OK|~s~n", [Name]);
        Err ->
            io:format("NET_ERR|~s ~p~n", [Name, Err])
    end.

%% Host -> guest: a LOOPING echo server, so early half-open probes cannot
%% consume a one-shot acceptor. {packet,raw} because the payload is raw bytes,
%% not line-framed.
-spec echo_server(inet:port_number()) -> pid().
echo_server(Port) ->
    spawn(fun() ->
        Opts = [binary, {packet, raw}, {active, false}, {reuseaddr, true}],
        {ok, Listen} = gen_tcp:listen(Port, Opts),
        io:format("LISTENER_UP~n"),
        accept_loop(Listen)
    end).

%% The hot path (accept -> recv -> send -> close) MUST stay free of io:format:
%% each console write costs about a second under TCG and two of them blow the
%% host's 3s socket timeout. The ECHOED print therefore lands after the close,
%% which is deliberate twice over: it keeps the round trip fast, and its
%% absence is the regression signal for the close-with-in-flight-ACK crash.
-spec accept_loop(gen_tcp:socket()) -> no_return().
accept_loop(Listen) ->
    case gen_tcp:accept(Listen, 30000) of
        {ok, Sock} ->
            case gen_tcp:recv(Sock, 0) of
                {ok, Bin} ->
                    gen_tcp:send(Sock, Bin),
                    gen_tcp:close(Sock),
                    io:format("ECHOED ~p~n", [Bin]);
                RecvErr ->
                    io:format("RECV_ERR ~p~n", [RecvErr])
            end;
        AcceptErr ->
            io:format("ACCEPT_ERR ~p~n", [AcceptErr])
    end,
    accept_loop(Listen).
