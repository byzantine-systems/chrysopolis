%% Entropy probe for rng-smoke.
%%
%% rng-smoke boots the image TWICE and asserts every value here differs between
%% the two boots, which is what proves the jitter-seeded HMAC-DRBG is really
%% seeded rather than replaying a fixed sequence.
%%
%% All three values are printed on ONE console line because writes cost about a
%% second each under TCG. They are tagged and space-separated so each is
%% unambiguous to the extractor even though they share a line.
-module(chryso_rng).

-export([sample/0]).

%% The bounded file:read(Fd, 8) is not an optimisation, it is a requirement:
%% /dev/urandom is synthesised by the openat shim in src/runtime/bringup.c and
%% NEVER returns EOF, so file:read_file/1 would loop forever. Reading it at all
%% is the point of including it here, since that exercises the shim rather than
%% just ERTS's own RNG.
-spec sample() -> ok.
sample() ->
    {ok, Fd} = file:open("/dev/urandom", [read, binary, raw]),
    {ok, Urandom} = file:read(Fd, 8),
    ok = file:close(Fd),
    io:format(
        "RNG_BYTES|~w RNG_REF|~p RNG_URANDOM|~w~n",
        [rand:bytes(8), erlang:make_ref(), Urandom]
    ).
