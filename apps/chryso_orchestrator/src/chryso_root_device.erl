-module(chryso_root_device).
-moduledoc """
How `chryso_root_port` reaches Root: one status page per read, one command
and its reply per call. `chryso_root_files` is the guest's implementation over
`/dev/chrysostat` and `/dev/chrysoctl`. Tests put a fake in the `device` key.
""".

-export_type([handle/0]).

-doc "An open command channel.".
-type handle() :: term().

-doc "One whole status page.".
-callback read_status() -> {ok, binary()} | {error, term()}.

-doc "Open the command channel. Root allows one opener.".
-callback open_ctl() -> {ok, handle()} | {error, term()}.

-doc "Send one command record and read its reply record. Blocks the caller.".
-callback call(handle(), binary()) -> {ok, binary()} | {error, term()}.

-doc "Close the channel, dropping a pending reply.".
-callback close(handle()) -> ok.
