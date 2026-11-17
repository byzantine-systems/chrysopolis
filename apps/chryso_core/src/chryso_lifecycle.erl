-module(chryso_lifecycle).
-moduledoc """
The driver lifecycle controller as a Mealy machine,
`step(Input, State) -> {Effects, State}`. No I/O, no clock: time arrives as
`{tick, NowMs}`. `chryso_orchestrator`'s `gen_statem` interprets it, one
callback per step.

Inputs: `{observed, Knowledge}`, `{want, Child, T}`, `{reply, Ref, Result}`,
`{deadline, Ref}`, `{tick, NowMs}`. Effects: `{command, Ref, Child, Fence}`,
`{arm, Ref, AtMs}`, `{publish, Conditions}`.

One command is in flight at a time. After `ok`, `already_done` or `stale` it
waits for a newer page. After a deadline the next read settles it (an
unchanged page means Root did nothing). After `rate_limited` it backs off.

Laws (`prop_lifecycle`, against `chryso_root`): at most one accepted restart
per want, no command before a known page, none for a driver seen gone,
convergence under fair delivery, and duplicate or older reads change nothing.

Non-law: latency is unbounded, and a lost reply's outcome waits for a read.
""".

-export([new/1, step/2, conditions/1, wants/1, known/1]).
-export_type([params/0, state/0, input/0, effect/0, result/0, conditions/0]).

-doc """
`drivers` are the children the controller watches. `deadline_ms` bounds a
command, `backoff_ms` is the first and the largest wait after Root's rate
limit, and `settle_ms` and `floor` feed `chryso_condition`.
""".
-type params() :: #{
    drivers := [non_neg_integer()],
    deadline_ms := pos_integer(),
    backoff_ms := {pos_integer(), pos_integer()},
    settle_ms := non_neg_integer(),
    floor := non_neg_integer()
}.

-doc "A reply result, as `CtlResult` names it.".
-type result() ::
    ok
    | refused
    | ignored
    | rate_limited
    | bad_arg
    | bad_version
    | internal_error
    | already_done
    | stale
    | unset.

-doc "One input.".
-type input() ::
    {observed, chryso_observe:knowledge()}
    | {want, non_neg_integer(), non_neg_integer()}
    | {reply, pos_integer(), result()}
    | {deadline, pos_integer()}
    | {tick, non_neg_integer()}.

-doc "One effect.".
-type effect() ::
    {command, pos_integer(), non_neg_integer(), chryso_plan:fence()}
    | {arm, pos_integer(), non_neg_integer()}
    | {publish, conditions()}.

-doc "Each watched driver's condition and reason.".
-type conditions() :: #{
    non_neg_integer() => {chryso_condition:condition(), chryso_condition:reason()}
}.

-type inflight() ::
    none | #{ref := pos_integer(), child := non_neg_integer(), fence := chryso_plan:fence()}.

-doc "The controller's state. Rebuilt from Root's page after a restart.".
-opaque state() :: #{
    params := params(),
    known := chryso_observe:knowledge(),
    wants := chryso_plan:wants(),
    %% Child to the planning page's stamp, while it waits for a later read:
    %% `newer` (Root's seq moved) or `any` (a deadline: an unchanged page
    %% proves Root did nothing).
    fenced := #{non_neg_integer() => {chryso_observe:stamp(), newer | any}},
    %% Child to {held until, next backoff}, both in milliseconds.
    hold := #{non_neg_integer() => {non_neg_integer(), pos_integer()}},
    %% Child to the want Root refused for good, and why.
    failed := #{non_neg_integer() => {non_neg_integer(), result()}},
    inflight := inflight(),
    next_ref := pos_integer(),
    now := non_neg_integer(),
    published := conditions()
}.

-doc "A controller that knows nothing and wants nothing.".
-spec new(Params :: params()) -> state().
new(#{drivers := Drivers} = Params) ->
    State = #{
        params => Params,
        known => chryso_observe:unknown(),
        wants => #{},
        fenced => #{},
        hold => #{},
        failed => #{},
        inflight => none,
        next_ref => 1,
        now => 0,
        published => #{}
    },
    State#{published := maps:from_list([{C, {unknown, no_observation}} || C <- Drivers])}.

-doc "Take one input.".
-spec step(Input :: input(), State :: state()) -> {[effect()], state()}.
step({observed, Knowledge}, #{known := Known} = State) ->
    settle(observe(State#{known := chryso_observe:merge(Known, Knowledge)}));
step({want, Child, T}, #{params := #{drivers := Drivers}, wants := Wants} = State) ->
    case lists:member(Child, Drivers) of
        true -> settle(State#{wants := Wants#{Child => max(T, maps:get(Child, Wants, 0))}});
        false -> {[], State}
    end;
step({reply, Ref, Result}, #{inflight := #{ref := Ref, child := Child} = Flight} = State) when
    Result =:= ok; Result =:= already_done; Result =:= stale
->
    %% Root's seq is past the planning page: it restarted, or something else
    %% changed. Either way a newer page is coming. Wait for it.
    settle(replied(Result, Child, State#{inflight := none, fenced := fence(Flight, newer, State)}));
step({reply, Ref, Result}, #{inflight := #{ref := Ref, child := Child}} = State) ->
    %% No effect and nothing new to publish: rate limited (wait out the
    %% backoff) or refused for good.
    settle(replied(Result, Child, State#{inflight := none}));
step({reply, _Late, _Result}, State) ->
    %% A late reply to a command already given up on: its outcome is read off
    %% the next page, never taken from here.
    {[], State};
step({deadline, Ref}, #{inflight := #{ref := Ref} = Flight} = State) ->
    %% Unknown outcome. The next read settles it: a newer page if Root acted,
    %% the same page if not, and then an identical resend is safe because
    %% Root's fence makes it `already_done` had Root acted after all.
    settle(State#{inflight := none, fenced := fence(Flight, any, State)});
step({deadline, _Stale}, State) ->
    {[], State};
step({tick, Now}, #{now := Then} = State) ->
    settle(State#{now := max(Now, Then)}).

-doc "Each watched driver's condition, as last published.".
-spec conditions(State :: state()) -> conditions().
conditions(#{published := Published}) ->
    Published.

-doc "What the controller knows about Root.".
-spec known(State :: state()) -> chryso_observe:knowledge().
known(#{known := Known}) ->
    Known.

-doc "The wants the controller holds.".
-spec wants(State :: state()) -> chryso_plan:wants().
wants(#{wants := Wants}) ->
    Wants.

%% --- internals ------------------------------------------------------------------

%% Lift the waits this read settles. Only reads run this: a gone child stays
%% gone because Root keeps it gone and only the newest page is known.
-spec observe(state()) -> state().
observe(#{known := Known, fenced := Fenced} = State) ->
    case chryso_observe:stamp(Known) of
        unknown ->
            State;
        Stamp ->
            Waiting = fun
                (_, {F, newer}) -> F >= Stamp;
                (_, {F, any}) -> F > Stamp
            end,
            State#{fenced := maps:filter(Waiting, Fenced)}
    end.

-spec fence(
    #{child := non_neg_integer(), fence := chryso_plan:fence(), _ => _}, newer | any, state()
) ->
    #{non_neg_integer() => {chryso_observe:stamp(), newer | any}}.
fence(#{child := Child, fence := #{root_generation := G, seq := S}}, Kind, #{fenced := Fenced}) ->
    Fenced#{Child => {{G, S}, Kind}}.

-spec replied(result(), non_neg_integer(), state()) -> state().
replied(Result, Child, #{hold := Hold} = State) when Result =:= ok; Result =:= already_done ->
    State#{hold := maps:remove(Child, Hold)};
replied(rate_limited, Child, #{hold := Hold, now := Now, params := Params} = State) ->
    #{backoff_ms := {First, Largest}} = Params,
    Wait =
        case maps:find(Child, Hold) of
            {ok, {_, Next}} -> Next;
            error -> First
        end,
    State#{hold := Hold#{Child => {Now + Wait, min(2 * Wait, Largest)}}};
replied(stale, _Child, State) ->
    State;
replied(Result, Child, #{failed := Failed, wants := Wants} = State) ->
    State#{failed := Failed#{Child => {maps:get(Child, Wants, 0), Result}}}.

%% Plan when nothing is in flight, then publish conditions that changed.
-spec settle(state()) -> {[effect()], state()}.
settle(State) ->
    {Commands, State1} = plan(State),
    publish(State1, Commands).

-spec plan(state()) -> {[effect()], state()}.
plan(#{inflight := #{}} = State) ->
    {[], State};
plan(#{known := Known, wants := Wants, next_ref := Ref, now := Now, params := Params} = State) ->
    case chryso_plan:commands(chryso_plan:next(Known, Wants, blocked(State))) of
        [] ->
            {[], State};
        [{restart, Child, Fence}] ->
            #{deadline_ms := Deadline} = Params,
            Flight = #{ref => Ref, child => Child, fence => Fence},
            Effects = [{command, Ref, Child, Fence}, {arm, Ref, Now + Deadline}],
            {Effects, State#{inflight := Flight, next_ref := Ref + 1}}
    end.

%% Children the plan must not address now.
-spec blocked(state()) -> sets:set(non_neg_integer()).
blocked(#{fenced := Fenced, hold := Hold, failed := Failed, wants := Wants, now := Now}) ->
    Held = [C || C := {Until, _} <- Hold, Until > Now],
    GaveUp = [C || C := {Want, _} <- Failed, maps:get(C, Wants, 0) =< Want],
    sets:from_list(maps:keys(Fenced) ++ Held ++ GaveUp).

-spec publish(state(), [effect()]) -> {[effect()], state()}.
publish(#{published := Published} = State, Commands) ->
    case current(State) of
        Published -> {Commands, State};
        Conditions -> {Commands ++ [{publish, Conditions}], State#{published := Conditions}}
    end.

-spec current(state()) -> conditions().
current(#{known := Known, params := #{drivers := Drivers}} = State) ->
    Page = chryso_observe:view(Known),
    maps:from_list([
        {C, chryso_condition:classify(chryso_observe:child(Known, C), context(C, Page, State))}
     || C <- Drivers
    ]).

-spec context(non_neg_integer(), unknown | chryso_observe:page(), state()) ->
    chryso_condition:context().
context(Child, Page, State) ->
    #{params := Params, known := Known, hold := Hold, wants := Wants, now := Now} = State,
    #{settle_ms := Settle, floor := Floor} = Params,
    {Ticks, Freq} =
        case Page of
            unknown -> {0, 0};
            #{now := T, cntfrq := F} -> {T, F}
        end,
    Row = chryso_observe:child(Known, Child),
    Requested =
        case {maps:find(Child, Wants), Row} of
            {{ok, T0}, #{last_restart := Last}} -> Last < T0 andalso not is_failed(Child, State);
            _ -> false
        end,
    #{
        now => Ticks,
        cntfrq => Freq,
        settle_ms => Settle,
        floor => Floor,
        requested => Requested,
        held =>
            case maps:find(Child, Hold) of
                {ok, {Until, _}} -> Until > Now;
                error -> false
            end
    }.

-spec is_failed(non_neg_integer(), state()) -> boolean().
is_failed(Child, #{failed := Failed, wants := Wants}) ->
    case maps:find(Child, Failed) of
        {ok, {Want, _}} -> maps:get(Child, Wants, 0) =< Want;
        error -> false
    end.
