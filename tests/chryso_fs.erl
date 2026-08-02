%% Block/filesystem probes for blk-restart-smoke and blk-giveup-smoke.
%%
%% Every probe reads a module that is NOT already loaded, so the read is a
%% genuine round trip through fatfs -> blk_virt -> blk_driver rather than a
%% cache hit. code:which/1 only resolves a path string from the code path and
%% does no I/O, so it stays cheap and keeps working even once the block device
%% has been stopped for good; file:read_file/1 is the part that actually
%% touches the disk.
%%
%% Results are reported via catch and element(1, ...) so that success and
%% failure are BOTH printable. After give-up the read MUST fail, and a probe
%% that could only print success would be unable to tell a failure apart from
%% a hang, which is precisely the wedge the give-up path exists to prevent.
%%
%% These modules are loaded into memory before any test damages the disk (see
%% the loader in tests.nix), which is what lets them keep reporting after the
%% device they probe is gone.
-module(chryso_fs).

-export([read_tagged/2, read_status/2, read_inflight/1]).

%% blk-restart-smoke: report the SIZE read, so a successful round trip is
%% proved by a number rather than by the mere absence of an error.
-spec read_tagged(atom(), module()) -> ok.
read_tagged(Tag, Module) ->
    {ok, Bin} = file:read_file(code:which(Module)),
    chryso_test:emit("FS_" ++ chryso_test:tag(Tag), byte_size(Bin)).

%% blk-giveup-smoke: report only the OUTCOME class (ok | error | EXIT), because
%% here both outcomes are expected at different points in the test and the
%% payload is irrelevant. Deliberately not read_tagged/2 with a catch bolted on:
%% the two tests assert opposite things and a shared function would have to
%% return something neither wants.
-spec read_status(atom(), module()) -> ok.
read_status(Tag, Module) ->
    R = (catch file:read_file(code:which(Module))),
    chryso_test:emit("BLKGONE_" ++ chryso_test:tag(Tag), element(1, R)).

%% blk-restart-smoke, scenario 2: start a read and return IMMEDIATELY, so the
%% caller can restart the driver while the request is still outstanding. The
%% reader must come back with something, ok or an error, and must not hang; a
%% silent reader here is the wedge the restart protocol exists to prevent.
-spec read_inflight(module()) -> pid().
read_inflight(Module) ->
    spawn(fun() ->
        R = (catch file:read_file(code:which(Module))),
        chryso_test:emit("FS_INFLIGHT", element(1, R))
    end).
