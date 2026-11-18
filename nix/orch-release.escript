#!/usr/bin/env escript
%% orch.boot and orch.config for the disk (modules/images.nix).
%%
%% Usage: orch-release.escript OUT APP_DIR...
%% Each APP_DIR holds ebin/<app>.app. kernel and stdlib come from this OTP, the
%% one ERTS is cross-built from, so the .rel names the guest's versions.
%%
%% systools orders loading and starting from the .app files. Every
%% application starts permanent: if the orchestrator's top supervisor gives
%% up, init halts the VM, Root counts a BEAM restart, and the boot selector
%% falls back to start.boot near the budget.
%%
%% The disk keeps the chryso applications in unversioned /lib/<app>/ebin, so
%% their script paths are rewritten before the binary boot file is made, and
%% the result is checked against that layout. Any failure logs one report and
%% exits 1. Outputs land in a fresh Nix build directory that is registered
%% only on success, so a partial write is never seen.
-mode(compile).

-include_lib("kernel/include/logger.hrl").

main(Args) ->
    argparse:run(Args, cli(), #{progname => "orch-release"}).

cli() ->
    #{
        arguments => [
            #{name => out, type => string, help => "output directory"},
            #{
                name => apps,
                type => string,
                nargs => nonempty_list,
                help => "application directories"
            }
        ],
        handler => fun(#{out := Out, apps := Apps}) -> finish(run(Out, Apps)) end
    }.

run(Out, AppDirs) ->
    try
        build(Out, AppDirs)
    catch
        Class:Reason:Stack -> {error, {crashed, {Class, Reason, Stack}}}
    end.

finish(ok) ->
    ok;
finish({error, {Step, Reason}}) ->
    %% A handler's output type cannot change in place, so the report goes to
    %% a stderr handler that replaces the console one.
    ok = logger:remove_handler(default),
    ok = logger:add_handler(stderr, logger_std_h, #{config => #{type => standard_error}}),
    ?LOG_ERROR(#{event => orch_release_failed, step => Step, reason => Reason}),
    %% The handler is asynchronous, and halting first can drop the report.
    ok = logger_std_h:filesync(stderr),
    halt(1).

build(Out, AppDirs) ->
    Rel = filename:join(Out, "orch"),
    ConfigFile = filename:join(Out, "orch.config"),
    maybe
        ok ?= step(outdir, filelib:ensure_path(Out)),
        {ok, Apps} ?= step(apps, apps(AppDirs)),
        ok ?= step(closure, closure(Apps)),
        ok ?= step(rel, write_term(Rel ++ ".rel", release(Apps))),
        ok ?= step(make_script, make_script(Rel, AppDirs)),
        {ok, Script} ?= step(script, consult_one(Rel ++ ".script")),
        Rewritten = rewrite(Script, Apps),
        ok ?= step(layout, check_script(Rewritten, Apps)),
        ok ?= step(script, write_term(Rel ++ ".script", Rewritten)),
        ok ?= step(boot, systools:script2boot(Rel)),
        {ok, Config} ?= step(config, config()),
        ok ?= step(config, write_term(ConfigFile, Config)),
        {ok, Written} ?= step(config, consult_one(ConfigFile)),
        step(config, check_config(Written))
    end.

step(_Name, ok) -> ok;
step(_Name, {ok, _} = Ok) -> Ok;
step(Name, error) -> {error, {Name, error}};
step(Name, {error, Reason}) -> {error, {Name, Reason}}.

%% {Name, Vsn, Deps} for each application directory, from its one .app file.
apps(Dirs) ->
    collect([app_of(Dir) || Dir <- Dirs], []).

collect([], Acc) -> {ok, lists:reverse(Acc)};
collect([{ok, App} | Rest], Acc) -> collect(Rest, [App | Acc]);
collect([{error, _} = Error | _], _Acc) -> Error.

app_of(Dir) ->
    maybe
        [AppFile] ?= filelib:wildcard(filename:join([Dir, "ebin", "*.app"])),
        {ok, [{application, Name, Keys}]} ?= file:consult(AppFile),
        Vsn = proplists:get_value(vsn, Keys),
        true ?= is_list(Vsn) andalso Vsn =/= [] andalso io_lib:printable_latin1_list(Vsn),
        {ok, {Name, Vsn, proplists:get_value(applications, Keys, [])}}
    else
        _ -> {error, {bad_app_dir, Dir}}
    end.

%% The orchestrator is present and every declared dependency is supplied.
closure(Apps) ->
    Names = [kernel, stdlib | [Name || {Name, _, _} <- Apps]],
    Missing = lists:usort([Dep || {_, _, Deps} <- Apps, Dep <- Deps, not lists:member(Dep, Names)]),
    case {lists:member(chryso_orchestrator, Names), Missing} of
        {true, []} -> ok;
        {false, _} -> {error, no_chryso_orchestrator};
        {true, _} -> {error, {missing_dependencies, Missing}}
    end.

release(Apps) ->
    Erts = erlang:system_info(version),
    {_, OrchVsn, _} = lists:keyfind(chryso_orchestrator, 1, Apps),
    Id = OrchVsn ++ "+otp-" ++ erlang:system_info(otp_release) ++ "+erts-" ++ Erts,
    Base = [{App, otp_vsn(App)} || App <- [kernel, stdlib]],
    {release, {"chrysopolis-orch", Id}, {erts, Erts}, Base ++ [{N, V} || {N, V, _} <- Apps]}.

otp_vsn(App) ->
    _ = application:load(App),
    {ok, Vsn} = application:get_key(App, vsn),
    Vsn.

%% With silent, make_script returns {ok, Module, Warnings} or
%% {error, Module, Error}. warnings_as_errors makes any warning an error.
make_script(Rel, AppDirs) ->
    Paths = [filename:join(Dir, "ebin") || Dir <- AppDirs],
    Options = [
        {path, Paths}, {outdir, filename:dirname(Rel)}, silent, warnings_as_errors, no_warn_sasl
    ],
    case systools:make_script(Rel, Options) of
        {ok, _Module, []} -> ok;
        {ok, Module, Warnings} -> {error, {warnings, Module:format_warning(Warnings)}};
        {error, Module, Reason} -> {error, Module:format_error(Reason)}
    end.

%% "$ROOT/lib/<app>-<vsn>/ebin" becomes "$ROOT/lib/<app>/ebin" for the
%% chryso applications. kernel and stdlib keep their versioned directories.
rewrite({script, Id, Instructions}, Apps) ->
    {script, Id, [instruction(I, Apps) || I <- Instructions]}.

instruction({path, Dirs}, Apps) -> {path, [unversioned(D, Apps) || D <- Dirs]};
instruction(Other, _Apps) -> Other.

unversioned(Dir, Apps) ->
    Rewrite = fun({Name, Vsn, _Deps}, D) ->
        App = atom_to_list(Name),
        string:replace(D, "/lib/" ++ App ++ "-" ++ Vsn ++ "/", "/lib/" ++ App ++ "/", all)
    end,
    unicode:characters_to_list(lists:foldl(Rewrite, Dir, Apps)).

%% Every path is kernel's or stdlib's versioned directory or a chryso
%% application's unversioned one, and the orchestrator starts permanent. The
%% script repeats paths (one {path, _} per phase), so they compare as sets.
check_script({script, _Id, Instructions}, Apps) ->
    Allowed = lists:usort(
        [otp_dir(App) || App <- [kernel, stdlib]] ++
            ["$ROOT/lib/" ++ atom_to_list(Name) ++ "/ebin" || {Name, _, _} <- Apps]
    ),
    Paths = lists:usort(lists:append([Dirs || {path, Dirs} <- Instructions])),
    Start = {apply, {application, start_boot, [chryso_orchestrator, permanent]}},
    case {ordsets:subtract(Paths, Allowed), lists:member(Start, Instructions)} of
        {[], true} -> ok;
        {Stray, true} -> {error, {unexpected_paths, Stray}};
        {_, false} -> {error, {missing, Start}}
    end.

otp_dir(App) ->
    "$ROOT/lib/" ++ atom_to_list(App) ++ "-" ++ otp_vsn(App) ++ "/ebin".

%% Boot-time configuration: the console handler drops the orchestrator's log
%% domain, which could cut into a probe's frame. A handler filter, not a
%% primary one, so another handler would still see those events.
%%
%% Declaring the default handler replaces it (remove, then add), and an added
%% handler's filters default to []. Kernel's default handler has its own
%% (remote_gl, the [otp] domain, no_domain, filter_default stop), which keep
%% progress and SASL reports off the console. They are taken from this OTP's
%% running default handler, the guest's, and kept in order after ours.
config() ->
    Quiet = {chryso_orch_quiet, {fun logger_filters:domain/2, {stop, sub, [chryso, orchestrator]}}},
    case logger:get_handler_config(default) of
        {ok, #{filters := Filters, filter_default := Default}} ->
            Handler = #{filter_default => Default, filters => [Quiet | Filters]},
            {ok, [{kernel, [{logger, [{handler, default, logger_std_h, Handler}]}]}]};
        Other ->
            {error, {no_default_handler, Other}}
    end.

%% The file as init reads it: one term whose filter funs resolve. Logger
%% removes a handler whose filter crashes, so an unresolvable fun would
%% silently drop the console.
check_config([{kernel, [{logger, [{handler, default, logger_std_h, #{filters := Filters}}]}]}]) ->
    case [Id || {Id, {Fun, _Arg}} <- Filters, not resolves(Fun)] of
        [] -> ok;
        Unresolved -> {error, {unresolved_filters, Unresolved}}
    end;
check_config(Other) ->
    {error, {unexpected_config, Other}}.

resolves(Fun) when is_function(Fun) ->
    {module, M} = erlang:fun_info(Fun, module),
    {name, F} = erlang:fun_info(Fun, name),
    {arity, A} = erlang:fun_info(Fun, arity),
    code:ensure_loaded(M) =:= {module, M} andalso erlang:function_exported(M, F, A);
resolves(_) ->
    false.

%% k prints map keys in term order. Without it a small map's atom keys follow
%% atom creation order, which varies between runs, and the file is not
%% reproducible.
write_term(Path, Term) ->
    file:write_file(Path, io_lib:format("~tkp.~n", [Term])).

consult_one(Path) ->
    case file:consult(Path) of
        {ok, [Term]} -> {ok, Term};
        {ok, Terms} -> {error, {expected_one_term, length(Terms)}};
        {error, _} = Error -> Error
    end.
