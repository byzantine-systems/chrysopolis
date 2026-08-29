%% Shared helpers for the QEMU integration tests in tests.nix.
%%
%% Everything in tests/ used to live inside Python string literals passed to
%% machine.send_console(). That put it out of reach of every tool this repo
%% already has: erlfmt is wired into treefmt and ELP is in the dev shell, but
%% neither can see inside a Python string, so a typo surfaced only as a
%% multi-minute wait_console timeout inside a QEMU boot. As real modules the
%% code is formatted, linted, and compiled by erlc -Werror at nix build time.
%%
%% Two properties of the serial console shape every API here.
%%
%% 1. THE CONSOLE ECHOES THE TYPED LINE, and the test driver waits on a regex
%%    over the accumulated log, so an echo can satisfy a wait before the
%%    command has even run. The inline code dodged this by splitting tags into
%%    adjacent Erlang literals ("SERIAL_" "BEFORE|"), which only join once
%%    evaluated. That hack is retired here: the caller types a lower-case tag
%%    atom and the module supplies the upper-case prefix, so the joined tag
%%    exists only in evaluated output and never in the echo. Module source is
%%    never echoed, which is why the literals below can be written whole.
%%
%%    The rule for anything added later: the regex a test waits on must not be
%%    a substring of the line it types. A corollary is that exported functions
%%    return ok rather than anything tag-shaped, so the Eshell's own result
%%    line cannot satisfy a wait either. eval_check/0 is the one deliberate
%%    exception, and says why.
%%
%% 2. ESHELL BINDINGS PERSIST FOR THE WHOLE SESSION, so the inline code needed
%%    a fresh variable name per call (F1, F2, ..., RBEFORE) or an explicit f()
%%    to forget them, otherwise a second `Var = ...` became a match against the
%%    first value and badmatched. Function-local variables have no such
%%    problem, so both workarounds are gone.
%%
%% Everything in tests/ may call kernel and stdlib and nothing else. The FAT
%% disk carries only those two applications plus the Gleam app, and ERTS boots
%% -mode embedded, so an unresolvable remote call is a plain undef with no
%% autoload to rescue it.
-module(chryso_test).

%% Loader, used once per boot by the tests.nix preamble.
-export([loaded/1]).
%% Tag plumbing, shared with the sibling chryso_* modules.
-export([emit/2, tag/1]).
%% Assertions.
-export([eval_check/0, alive/1, restart_pd/1, fault_pd/1]).
-export([serial_before/0, serial_after/0, serial_rx/0]).

-define(EBIN, "/lib/chryso_test/ebin").

%% Load the sibling test modules and report in ONE line whether they all made
%% it. Called with chryso_test itself already loaded by hand: something has to
%% bootstrap the path, and doing it from the shell keeps the module list in
%% tests.nix (generated from the directory listing) as the single source of
%% truth.
%%
%% code:load_file/1 rather than code:ensure_loaded/1: the latter returns
%% {error,embedded} under -mode embedded and would report every module as
%% missing. Reporting failures instead of crashing turns a bad disk or a bad
%% path into an immediate, named error rather than a wait_console timeout at
%% whatever assertion happens to run first.
%%
%% The via= field is not decoration. code:load_file/1 resolves through the code
%% path, which means code_server has to stat the directory added by
%% code:add_patha/1, and whether that works over the fatfs client is the one
%% assumption in this migration that could not be checked on the host. So the
%% fallback below reads the beam directly and loads it as a binary, needing
%% neither the path nor a stat, and the report says which route won. If via=
%% ever reads load_binary, add_patha is not working over fatfs and the tests
%% are still fine.
-spec loaded([module()]) -> ok.
loaded(Mods) ->
    Results = [{M, load(M)} || M <- Mods],
    case [M || {M, error} <- Results] of
        [] ->
            Via =
                case lists:keymember(binary, 2, Results) of
                    true -> "load_binary";
                    false -> "load_file"
                end,
            io:format("TEST_MODULES|ok|mods=~p|via=~s~n", [length(Results), Via]);
        Missing ->
            io:format("TEST_MODULES|missing ~p~n", [Missing])
    end.

-spec load(module()) -> file | binary | error.
load(M) ->
    case code:load_file(M) of
        {module, M} -> file;
        {error, _} -> load_binary(M)
    end.

%% Path-free fallback: read the beam and hand ERTS the bytes. Note the plain
%% file:read_file/1 rather than anything that lists a directory. The LionsOS
%% libc registers no getdents64 (see the openat shim in src/runtime/bringup.c),
%% so directory listing is unavailable on the guest: never pass `cache` to
%% code:add_patha/2 or code:set_path/2 either, both of those list the directory.
-spec load_binary(module()) -> binary | error.
load_binary(M) ->
    Path = ?EBIN ++ "/" ++ atom_to_list(M) ++ ".beam",
    case file:read_file(Path) of
        {ok, Bin} ->
            case code:load_binary(M, Path, Bin) of
                {module, M} -> binary;
                {error, _} -> error
            end;
        {error, _} ->
            error
    end.

%% Print Tag|Value on one console line. Every assertion in tests/ funnels
%% through here so the log format is uniform and greppable. ~p, not ~w: the
%% inline code used ~p wherever a term could be an error tuple, and the two are
%% identical for the integers and atoms printed here.
-spec emit(string(), term()) -> ok.
emit(Tag, Value) ->
    io:format("~s|~p~n", [Tag, Value]).

%% Upper-case a lower-case tag atom. This is what keeps the typed line free of
%% the string the driver waits for (see the header): the caller types `before`,
%% the log gets `BEFORE`.
-spec tag(atom()) -> string().
tag(Atom) ->
    string:uppercase(atom_to_list(Atom)).

%% shell-smoke: the Eshell evaluates real code. This is the one function that
%% returns its value rather than ok, so the shell still prints a bare `2` and
%% the original assertion holds unchanged; the tagged line is the stronger
%% assertion layered on top, since a bare 2 could come from anywhere in the log.
-spec eval_check() -> integer().
eval_check() ->
    V = 1 + 1,
    emit("SHELL_EVAL", V),
    V.

%% blk-giveup-smoke: the shell still evaluates after the block device has been
%% stopped for good, so losing the disk cost us the disk and nothing else.
-spec alive(atom()) -> ok.
alive(Class) ->
    emit(tag(Class) ++ "_ALIVE", 6 * 7).

%% Ask root to restart a driver PD by class, via the /dev/pd-restart shim in
%% src/runtime/bringup.c. Only present in images built with restartDebug (the
%% shim is gated on a patched .pd_restart_config section), so on a production
%% image the open fails with enoent and this badmatches loudly rather than
%% silently doing nothing.
%%
%% The shim prints PD_RESTART|request before notifying root, and it does so
%% while the OLD instance is still alive, which is what makes that line proof
%% the write reached the shim at all.
-spec restart_pd(serial | timer | blk | eth) -> ok.
restart_pd(Class) ->
    request_pd(atom_to_binary(Class, latin1)).

%% Ask Root to resume the real driver at address 0. The resulting instruction
%% fault is delivered to Root's fault() callback, so this exercises the same
%% detection, budget and recovery path as a production driver bug.
-spec fault_pd(serial | timer | blk | eth) -> ok.
fault_pd(Class) ->
    request_pd(<<"fault:", (atom_to_binary(Class, latin1))/binary>>).

-spec request_pd(binary()) -> ok.
request_pd(Request) ->
    {ok, F} = file:open("/dev/pd-restart", [write, raw]),
    ok = file:write(F, Request),
    ok = file:close(F).

%% serial-restart-smoke, in order: console output works before the restart,
%% output works after it (only true if the restarted driver re-initialised the
%% UART, drained the TX ring and re-armed its IRQ), and input survived too.
%%
%% Three different computations rather than one repeated constant: each line
%% has to prove the shell evaluated it just now, and distinct values make a
%% stale line in the accumulated log obvious.
-spec serial_before() -> ok.
serial_before() ->
    emit("SERIAL_BEFORE", 1 + 1).

-spec serial_after() -> ok.
serial_after() ->
    emit("SERIAL_AFTER", 6 * 7).

%% The AFTER line above already required RX (its command was typed), but a
%% second round trip means a single buffered keystroke cannot carry the test.
%% This is the part that fails when the IRQ ack is missing.
-spec serial_rx() -> ok.
serial_rx() ->
    emit("SERIAL_RX", length([a, b, c])).
