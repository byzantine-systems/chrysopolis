-module(chryso_fs).
-moduledoc """
Block/filesystem probes for blk-restart-smoke and blk-giveup-smoke.

Every probe reads a module that is not already loaded, so the read is a
genuine round trip through fatfs -> blk_virt -> blk_driver rather than a
cache hit. `code:which/1` only resolves a path string from the code path and
does no I/O, so it stays cheap and keeps working even once the block device
has been stopped for good. `file:read_file/1` is the part that actually
touches the disk.

Results are reported via `catch` and `element(1, ...)` so that success and
failure are both printable. After give-up the read must fail, and a probe
that could only print success would be unable to tell a failure apart from
a hang, which is precisely the wedge the give-up path exists to prevent.

These modules are loaded into memory before any test damages the disk (see
`load_test_modules` in tools/testbed), which is what lets them keep
reporting after the device they probe is gone.
""".

-export([
    read_tagged/2,
    read_status/2,
    read_inflight/1,
    read_during_fault/0,
    write_status/1,
    read_outcome/2,
    read_back/1,
    put/3,
    get/2
]).

-doc """
blk-restart-smoke: report the size read, so a successful round trip is
proved by a number rather than by the mere absence of an error.
""".
-spec read_tagged(atom(), module()) -> ok.
read_tagged(Tag, Module) ->
    {ok, Bin} = file:read_file(code:which(Module)),
    chryso_test:emit("FS_" ++ chryso_test:tag(Tag), byte_size(Bin)).

-doc """
blk-giveup-smoke: report only the outcome class (`ok | error | 'EXIT'`),
because here both outcomes are expected at different points in the test and
the payload is irrelevant. Deliberately not `read_tagged/2` with a catch
bolted on: the two tests assert opposite things and a shared function would
have to return something neither wants.
""".
-spec read_status(atom(), module()) -> ok.
read_status(Tag, Module) ->
    R = (catch file:read_file(code:which(Module))),
    chryso_test:emit("BLKGONE_" ++ chryso_test:tag(Tag), element(1, R)).

-doc """
blk-restart-smoke, scenario 2: start a read and return immediately, so the
caller can restart the driver while the request is still outstanding. The
reader must come back with something, ok or an error, and must not hang. A
silent reader here is the wedge the restart protocol exists to prevent.
""".
-spec read_inflight(module()) -> pid().
read_inflight(Module) ->
    spawn(fun() ->
        R = (catch file:read_file(code:which(Module))),
        chryso_test:emit("FS_INFLIGHT", element(1, R))
    end).

-doc """
blk-fault-smoke: remove the serial-shell delay between starting the client
read and asking Root to fault blk_driver. The reader wakes a fault worker
immediately before entering `file:read_file/1`. That worker waits a bounded
100 ms so the async transfer is established before it writes the test-only
fault command.

The integration test requires the genuine fault and restart records to
precede `FS_INFLIGHT`, proving the client call receives its answer only after
the fault rather than completing before the injection command is processed.
""".
-spec read_during_fault() -> pid().
read_during_fault() ->
    Fault = spawn(fun() ->
        receive
            fault ->
                %% Let the reader enter the async file path before waking Root.
                %% The 16 MiB transfer remains active well beyond this delay
                %% under QEMU, while a bounded timer avoids an uncoordinated
                %% race between two immediately runnable BEAM processes.
                timer:sleep(100),
                chryso_test:fault_pd(blk)
        end
    end),
    spawn(fun() ->
        Fault ! fault,
        R = (catch file:read_file("/fault-inflight.bin")),
        chryso_test:emit("FS_INFLIGHT", element(1, R))
    end).

-define(PROBE_FILE, "/eio-probe.bin").

-doc """
blk-eio-smoke and the disk fuzz: write 64 KiB, sync and close, reporting each
step. 64 KiB spans whole sectors, so FatFs writes them during the write
itself and a disk error belongs to that call. `file:write_file/2` would hide
a failed close, so each step is reported on its own.
""".
-spec write_status(atom()) -> ok.
write_status(Tag) ->
    Outcome =
        case file:open(?PROBE_FILE, [write, raw, binary]) of
            {ok, Fd} ->
                Written = file:write(Fd, payload()),
                Synced = file:sync(Fd),
                {Written, Synced, file:close(Fd)};
            {error, _} = Error ->
                Error
        end,
    chryso_test:emit("FS_WRITE_" ++ chryso_test:tag(Tag), Outcome).

-doc """
The disk fuzz: read the probe file back and report `same` when it holds
exactly what `write_status/1` wrote, `differs` when it does not, or
`{error, Reason}`.
""".
-spec read_back(atom()) -> ok.
read_back(Tag) ->
    Outcome =
        case file:read_file(?PROBE_FILE) of
            {ok, Bin} -> back(Bin);
            {error, _} = Error -> Error
        end,
    chryso_test:emit("FS_BACK_" ++ chryso_test:tag(Tag), Outcome).

-doc """
blk-eio-smoke and the disk fuzz: read a module's file and report
`{ok, Size, Hash}` or `{error, Reason}`. The hash lets the caller check that
a fault elsewhere left existing data unchanged, and a refused read is a
result, not a crashed probe.
""".
-spec read_outcome(atom(), module()) -> ok.
read_outcome(Tag, Module) ->
    Outcome =
        case file:read_file(code:which(Module)) of
            {ok, Bin} -> {ok, byte_size(Bin), erlang:phash2(Bin)};
            {error, _} = Error -> Error
        end,
    chryso_test:emit("FS_READ_" ++ chryso_test:tag(Tag), Outcome).

-define(FUZZ_SIZE, 8192).

-doc """
The system fuzz: write file `File` as 8 KiB of the byte `Seed`, then sync
and close, and report `FS_PUT_<TAG>|{Write, Sync, Close}` or
`{error, Reason}`. Below one FatFs cluster, so a disk error may surface at
any of the three steps.
""".
-spec put(atom(), 0..255, byte()) -> ok.
put(Tag, File, Seed) ->
    Outcome =
        case catch file:open(fuzz_path(File), [write, raw, binary]) of
            {ok, Fd} ->
                Written = file:write(Fd, binary:copy(<<Seed>>, ?FUZZ_SIZE)),
                Synced = file:sync(Fd),
                {Written, Synced, file:close(Fd)};
            {error, _} = Error ->
                Error;
            {'EXIT', _} ->
                {error, exit}
        end,
    chryso_test:emit("FS_PUT_" ++ chryso_test:tag(Tag), Outcome).

-doc """
The system fuzz: read file `File` back and report `{ok, Seed}` when it is
whole and every byte is `Seed`, `differs` when it is not, or
`{error, Reason}`.
""".
-spec get(atom(), 0..255) -> ok.
get(Tag, File) ->
    Outcome =
        case catch file:read_file(fuzz_path(File)) of
            {ok, <<Seed, _/binary>> = Bin} when byte_size(Bin) =:= ?FUZZ_SIZE ->
                case binary:copy(<<Seed>>, ?FUZZ_SIZE) of
                    Bin -> {ok, Seed};
                    _ -> differs
                end;
            {ok, _} ->
                differs;
            {error, _} = Error ->
                Error;
            {'EXIT', _} ->
                {error, exit}
        end,
    chryso_test:emit("FS_GET_" ++ chryso_test:tag(Tag), Outcome).

-spec fuzz_path(0..255) -> string().
fuzz_path(File) ->
    "/fuzz" ++ integer_to_list(File) ++ ".bin".

-spec back(binary()) -> same | differs.
back(Bin) ->
    case Bin =:= payload() of
        true -> same;
        false -> differs
    end.

-spec payload() -> binary().
payload() ->
    binary:copy(<<"chryso">>, 65536 div 6).
