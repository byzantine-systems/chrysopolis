-module(chryso_root_command).
-moduledoc """
Root's PPC command decisions (`src/pd/root/command.c`): frame checks, the
token bucket, the retry fence and the reply values. The caller performs an
accepted restart and then calls `complete/2`.

The fence is the published status `seq`: a completed command publishes, so
the `seq` it observed never matches again, and only the last completed
command is remembered.

Laws, observed through the reply and the restart action:

- **fenced idempotence**: after `complete/2`, the same opcode and words get
  `already_done` with the stored values and no second action.
- **stale has no effect**: a `stale` reply never carries an action.
- **only accepted restarts act**: an action implies opcode `restart_child`
  and result `ok`.
- **tallies add up**: every framed call adds exactly one to `accepted`,
  `rate_limited` or `rejected`.

Non-law: an *unfenced* restart is not idempotent. HELLO and SYNC are
idempotent only as reads.
""".

-export([new/1, frame/5, decide/6, complete/2, tallies/1, tokens/1]).
-export_type([state/0, view/0, decision/0, result/0]).

-define(WORDS, 4).
-define(CHILD_COUNT, 62).
-define(MASK_BITS, 64).
-define(U32_MAX, 16#FFFFFFFF).

-doc "A reply result, as `CtlResult` names it.".
-type result() ::
    ok
    | refused
    | rate_limited
    | bad_arg
    | bad_version
    | already_done
    | stale.

-doc "Root's state as a command sees it, sampled per call.".
-type view() :: #{
    abi := non_neg_integer(),
    generation := non_neg_integer(),
    incarnation := non_neg_integer(),
    cntfrq := non_neg_integer(),
    seq := non_neg_integer(),
    publishing := boolean(),
    present := non_neg_integer(),
    drivers := non_neg_integer(),
    records := #{non_neg_integer() => chryso_root_policy:record()}
}.

-doc "One decided command.".
-type decision() :: #{
    result := result(),
    action := none | {restart, non_neg_integer()},
    opcode := non_neg_integer(),
    args := [non_neg_integer()],
    values := [non_neg_integer()]
}.

-type class() :: invalid | handshake | barrier | preconditioned | slot.

-doc "Bucket, fence and tallies.".
-opaque state() :: #{
    bucket := chryso_root_bucket:bucket(),
    fence := empty | {completed, non_neg_integer(), [non_neg_integer()], [non_neg_integer()]},
    accepted := non_neg_integer(),
    rate_limited := non_neg_integer(),
    rejected := non_neg_integer()
}.

-doc "An empty fence and zero tallies around `Bucket`.".
-spec new(chryso_root_bucket:bucket()) -> state().
new(Bucket) ->
    #{bucket => Bucket, fence => empty, accepted => 0, rate_limited => 0, rejected => 0}.

-doc """
Transport checks before any word is read: the PPC channel, a declared
nonzero opcode and exactly four words. `ok` to proceed. Otherwise the reply
result, counted as rejected, with no token spent.
""".
-spec frame(state(), non_neg_integer(), non_neg_integer(), non_neg_integer(), non_neg_integer()) ->
    {ok | refused | bad_arg, state()}.
frame(#{rejected := R} = State, Channel, PpChannel, _Label, _Count) when Channel =/= PpChannel ->
    {refused, State#{rejected := R + 1}};
frame(#{rejected := R} = State, _Channel, _PpChannel, Label, Count) ->
    case Count =:= ?WORDS andalso class(Label) =/= invalid of
        true -> {ok, State};
        false -> {bad_arg, State#{rejected := R + 1}}
    end.

-doc """
Decide a framed command at clock sample `Now`. `Args` holds exactly the four
words `frame/5` admitted. A different length is a caller bug.
""".
-spec decide(state(), view(), non_neg_integer(), [non_neg_integer()], non_neg_integer(), boolean()) ->
    {decision(), state()}.
decide(State, View, Opcode, Args, Now, NowValid) ->
    D0 = #{result => ok, action => none, opcode => Opcode, args => Args, values => zeros()},
    case malformed(Opcode, Args) of
        true ->
            reply(D0#{result := bad_arg}, State);
        false ->
            #{bucket := Bucket} = State,
            case chryso_root_bucket:take(Now, NowValid, Bucket) of
                {limited, B} ->
                    reply(D0#{result := rate_limited}, State#{bucket := B});
                {ok, B} ->
                    by_class(class(Opcode), D0, State#{bucket := B}, View)
            end
    end.

-doc "Remember a performed action's reply, so an identical retry is `already_done`.".
-spec complete(state(), decision()) -> state().
complete(State, #{action := none}) ->
    State;
complete(State, #{opcode := Op, args := Args, values := Values}) ->
    State#{fence := {completed, Op, Args, Values}}.

-doc "`{accepted, rate_limited, rejected}`, as SYNC reports them.".
-spec tallies(state()) -> {non_neg_integer(), non_neg_integer(), non_neg_integer()}.
tallies(#{accepted := A, rate_limited := L, rejected := R}) ->
    {A, L, R}.

-doc "Tokens currently held.".
-spec tokens(state()) -> non_neg_integer().
tokens(#{bucket := B}) ->
    chryso_root_bucket:tokens(B).

-spec by_class(class(), decision(), state(), view()) -> {decision(), state()}.
by_class(handshake, #{args := [Version | _]} = D, State, View) ->
    #{abi := Abi, generation := G, incarnation := I, cntfrq := F} = View,
    Result =
        case Version =:= Abi of
            true -> ok;
            false -> bad_version
        end,
    reply(D#{result := Result, values := [Abi, G, I, F]}, State);
by_class(barrier, D, State, #{publishing := false}) ->
    reply(D#{result := refused}, State);
by_class(barrier, D, State, #{seq := Seq}) ->
    %% Counted first so the reply includes this SYNC.
    State1 = tally(ok, State),
    #{accepted := A, rate_limited := L, rejected := R} = State1,
    {D#{result := ok, values := [Seq, A, L, R]}, State1};
by_class(preconditioned, D, State, View) ->
    reply(restart(D, State, View), State);
by_class(slot, D, State, _View) ->
    reply(D#{result := refused}, State).

-spec restart(decision(), state(), view()) -> decision().
restart(#{opcode := Op, args := Args} = D, #{fence := {completed, Op, Args, Values}}, _View) ->
    D#{result := already_done, values := Values};
restart(#{args := [Raw, Gen, Seq, Count]} = D, _State, View) ->
    #{present := Present, drivers := Drivers, records := Records} = View,
    case Raw =< ?U32_MAX andalso valid_child(Raw, Present) of
        false ->
            D#{result := bad_arg};
        true ->
            #{state := RecState, lifetime := Lifetime} = maps:get(Raw, Records),
            #{publishing := Publishing, generation := RootGen, seq := RootSeq} = View,
            Usable = is_driver(Raw, Drivers) andalso RecState =:= live andalso Publishing,
            Fresh = Gen =:= RootGen andalso Seq =:= RootSeq andalso Count =:= Lifetime,
            case {Usable, Fresh} of
                {false, _} -> D#{result := refused};
                {true, false} -> D#{result := stale, values := [RootGen, RootSeq, Lifetime, 0]};
                {true, true} -> D#{result := ok, action := {restart, Raw}}
            end
    end.

-spec reply(decision(), state()) -> {decision(), state()}.
reply(#{result := Result} = D, State) ->
    {D, tally(Result, State)}.

-spec tally(result(), state()) -> state().
tally(Result, #{accepted := A} = S) when Result =:= ok; Result =:= already_done ->
    S#{accepted := A + 1};
tally(rate_limited, #{rate_limited := L} = S) ->
    S#{rate_limited := L + 1};
tally(_, #{rejected := R} = S) ->
    S#{rejected := R + 1}.

%% Opcode classes and the words each leaves zero (bit i = word i).
-spec class(non_neg_integer()) -> class().
class(1) -> handshake;
class(2) -> preconditioned;
class(Op) when Op >= 3, Op =< 6 -> slot;
class(7) -> barrier;
class(_) -> invalid.

-spec unused(non_neg_integer()) -> non_neg_integer().
unused(1) -> 2#1110;
unused(7) -> 2#1111;
unused(_) -> 0.

-spec malformed(non_neg_integer(), [non_neg_integer()]) -> boolean().
malformed(Opcode, Args) ->
    class(Opcode) =:= invalid orelse
        lists:any(
            fun({I, W}) -> (unused(Opcode) bsr I) band 1 =:= 1 andalso W =/= 0 end,
            lists:zip(lists:seq(0, ?WORDS - 1), Args)
        ).

-spec valid_child(non_neg_integer(), non_neg_integer()) -> boolean().
valid_child(Raw, Present) ->
    Raw < ?CHILD_COUNT andalso Raw < ?MASK_BITS andalso (Present bsr Raw) band 1 =:= 1.

-spec is_driver(non_neg_integer(), non_neg_integer()) -> boolean().
is_driver(Child, Drivers) ->
    (Drivers bsr Child) band 1 =:= 1.

-spec zeros() -> [non_neg_integer()].
zeros() ->
    [0, 0, 0, 0].
