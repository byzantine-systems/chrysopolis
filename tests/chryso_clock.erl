%% Timer-class probes for timer-restart-smoke.
%%
%% Two distinct things get measured here and only the second has teeth. The
%% monotonic clock READS correctly almost for free: it is the ARM generic
%% timer's CNTPCT, a hardware counter the driver only reads, so a restart
%% cannot perturb it. A NEW timeout still FIRING is the real test, because the
%% driver's init() re-fills timeouts[] with UINT64_MAX and discards every
%% timeout armed before the restart, leaving the client waiting on a
%% notification that will never arrive.
%%
%% Reaching now_tagged/1 at all after a restart also proves the restarted timer
%% answers PPCs again: erlang:monotonic_time/0 bottoms out in
%% sddf_timer_time_now, which is an seL4_Call.
%%
%% See chryso_test for why tags arrive as lower-case atoms and are upper-cased
%% here rather than by the caller.
-module(chryso_clock).

-export([now_tagged/1, sleep_check/1]).

%% Print the raw monotonic reading; the driver-side assertion (that it ADVANCED
%% across a restart) is the test script's job, not the probe's.
-spec now_tagged(atom()) -> ok.
now_tagged(Tag) ->
    chryso_test:emit("CLOCK_" ++ chryso_test:tag(Tag), erlang:monotonic_time()).

%% Assert a timeout still fires by measuring around it. Reports the elapsed
%% milliseconds rather than a verdict so that the ">= 500" oracle stays in the
%% test script where the other assertions live, and so a short sleep is
%% distinguishable from a missing line in the log.
-spec sleep_check(atom()) -> ok.
sleep_check(Tag) ->
    T0 = erlang:monotonic_time(millisecond),
    timer:sleep(500),
    Elapsed = erlang:monotonic_time(millisecond) - T0,
    chryso_test:emit(chryso_test:tag(Tag), Elapsed).
