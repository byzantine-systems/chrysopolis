-module(chryso_root_files).
-moduledoc """
`chryso_root_device` over the guest's virtual files (`AGENTS.md`, "BEAM
devices"). Each status open yields one validated page and then EOF. The
command file takes one 64-byte record and returns one. The write is the PPC,
which blocks the whole beam_server TCB until Root replies.
""".
-behaviour(chryso_root_device).

-export([read_status/0, open_ctl/0, call/2, close/1]).

-define(STAT, "/dev/chrysostat").
-define(CTL, "/dev/chrysoctl").
-define(PAGE, 16384).
-define(RECORD, 64).

-doc "One status page.".
-spec read_status() -> {ok, binary()} | {error, term()}.
read_status() ->
    case file:open(?STAT, [read, raw, binary]) of
        {ok, F} ->
            Result = file:read(F, ?PAGE),
            ok = file:close(F),
            case Result of
                {ok, Page} -> {ok, Page};
                eof -> {error, eof};
                {error, _} = Error -> Error
            end;
        {error, _} = Error ->
            Error
    end.

-doc "Open the command file.".
-spec open_ctl() -> {ok, file:io_device()} | {error, term()}.
open_ctl() ->
    file:open(?CTL, [read, write, raw, binary]).

-doc "One command and its reply.".
-spec call(file:io_device(), binary()) -> {ok, binary()} | {error, term()}.
call(F, Command) ->
    case file:write(F, Command) of
        ok ->
            case file:read(F, ?RECORD) of
                {ok, Reply} -> {ok, Reply};
                eof -> {error, eof};
                {error, _} = Error -> Error
            end;
        {error, _} = Error ->
            Error
    end.

-doc "Close the command file.".
-spec close(file:io_device()) -> ok.
close(F) ->
    _ = file:close(F),
    ok.
