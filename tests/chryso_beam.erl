%% beam-restart-smoke: prove that the BEAM PD comes back from a crash as a
%% genuinely fresh VM, not as the old one resumed.
%%
%% The C side already reports the two halves that only it can see: root's
%% ROOT|fault / ROOT|restart lines, and beam_server's BEAM|boot|generation=N|
%% bss-counter=1 (a counter outside the restored memory that climbs, paired with
%% one inside it that must not). What those cannot show is whether ERTS ITSELF
%% came back clean, because a PD whose memory was only partly reset could still
%% reach an Eshell while carrying the dead VM's process table.
%%
%% So this module plants state that is unmistakably VM-level and then asks for
%% it again after the restart. Two independent kinds, because they die for
%% different reasons: a registered process lives in the process registry, and a
%% persistent_term lives in the literal area. If either survived, the reset did
%% not do its job.
%%
%% persistent_term is safe to call here despite the kernel-and-stdlib-only rule
%% in chryso_test's header: it is an ERTS preloaded module, compiled into the
%% emulator rather than loaded off the FAT disk, so -mode embedded cannot fail
%% to resolve it.
-module(chryso_beam).

-export([mark/0, check/1]).
-export([stop_vm/0, halt_vm/1]).

-define(NAME, chryso_restart_mark).
-define(PTERM, chryso_restart_pterm).

%% Plant the marks. The registered process just sleeps: nothing ever sends to
%% it, its only job is to exist under a name that whereis/1 can be asked about
%% later.
-spec mark() -> ok.
mark() ->
    Pid = spawn(fun() -> receive
        after infinity -> ok
        end end),
    true = register(?NAME, Pid),
    ok = persistent_term:put(?PTERM, marked),
    chryso_test:emit("BEAM_MARK", set).

%% Report whether either mark is still here, as ONE line so a partial survival
%% cannot be read as a pass. Tagged by the caller (before/after) so the two
%% checks in a run are distinguishable in the accumulated log, and so the
%% pattern the driver waits on is never a substring of the line it typed.
%%
%% present is emitted when EITHER mark survived, since either one surviving
%% falsifies the same claim.
-spec check(atom()) -> ok.
check(Which) ->
    Registered = erlang:whereis(?NAME) =/= undefined,
    Stored = persistent_term:get(?PTERM, missing) =/= missing,
    Verdict =
        case Registered orelse Stored of
            true -> present;
            false -> absent
        end,
    chryso_test:emit("BEAM_MARK_" ++ chryso_test:tag(Which), Verdict).

%% Shut the VM down the way an operator would. init:stop/0 runs the ordinary
%% OTP shutdown and ends in erlang:halt(0), which reaches the exit/exit_group
%% shim in src/runtime/bringup.c and becomes a restart request to root.
%%
%% Returns ok immediately; the shutdown is asynchronous, so the test waits on
%% the console rather than on this call.
-spec stop_vm() -> ok.
stop_vm() ->
    init:stop().

%% The same path with a non-zero status, which is what proves the exit code
%% reaches root: the shim faults at a reserved address whose low byte is the
%% code, so the code shows up in root's fault log as mr1.
-spec halt_vm(integer()) -> no_return().
halt_vm(Code) ->
    erlang:halt(Code).
