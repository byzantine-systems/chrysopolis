-module(chryso_root).
-moduledoc """
Root as one Mealy machine: the product of `chryso_root_policy` (per child),
`chryso_root_spec` and `chryso_root_command`, composed the way Root's
`main.c` composes the C units: refresh the spec, plan, commit, publish.

This is the BEAM's model of what Root does. It is checked three ways:
PropEr laws (`prop_root*`), replay of traces from the C units
(`prop_root_traces`), and in-situ against Root's status rows
(`chryso_ctl`'s audit, on the guest).

Inputs are faults and framed commands. The spec page arrives as the codec
classifies it (`t:chryso_root_spec:page/0`), read at each refresh. Outputs
are the decision for a refresh and the reply for a command.

Law, observed through one driver's decisions and record:

- **noninterference**: deleting every BEAM-side input (commands other than
  an accepted `restart_child` of that driver, and refreshes that apply
  nothing) leaves the driver's decisions and record unchanged.
""".

-export([new/1, fault/4, command/6, observe/1, records/1]).
-export_type([config/0, root/0, decided/0, reply/0]).

-define(KIND_RESTART, 3).
-define(KIND_GIVEUP, 5).

-doc "The image constants a Root starts from.".
-type config() :: #{
    mapped := boolean(),
    now := non_neg_integer(),
    tokens := non_neg_integer(),
    refill := non_neg_integer(),
    hard_max := pos_integer(),
    entry := non_neg_integer(),
    pp := non_neg_integer(),
    beam := non_neg_integer(),
    present := non_neg_integer(),
    drivers := non_neg_integer(),
    compiled := [non_neg_integer()],
    generation := non_neg_integer(),
    incarnation := non_neg_integer(),
    seq := non_neg_integer(),
    abi := non_neg_integer(),
    cntfrq := non_neg_integer()
}.

-doc "One refresh-and-decide, as Root publishes it.".
-type decided() :: #{
    child := non_neg_integer(),
    action := chryso_root_policy:action(),
    record := chryso_root_policy:record(),
    spec := chryso_root_spec:result(),
    retained := chryso_root_spec:policy(),
    seq := non_neg_integer()
}.

-doc "One command's reply. `tokens` is absent when the frame was refused.".
-type reply() :: #{
    result := chryso_root_command:result(),
    values := [non_neg_integer()],
    tokens => non_neg_integer()
}.

-doc "Root's whole modelled state.".
-opaque root() :: #{
    config := config(),
    records := #{non_neg_integer() => chryso_root_policy:record()},
    spec := chryso_root_spec:state(),
    command := chryso_root_command:state(),
    seq := non_neg_integer(),
    incarnation := non_neg_integer()
}.

-doc "Root at boot: every child live, the compiled policy, a full bucket.".
-spec new(config()) -> root().
new(#{now := Now, tokens := Tokens, refill := Refill, compiled := Compiled, seq := Seq} = Config) ->
    #{incarnation := Incarnation} = Config,
    Children = lists:seq(0, length(Compiled) - 1),
    #{
        config => Config,
        records => maps:from_list([{C, chryso_root_policy:new()} || C <- Children]),
        spec => chryso_root_spec:new(),
        command => chryso_root_command:new(chryso_root_bucket:new(Tokens, Refill, Now, true)),
        seq => Seq,
        incarnation => Incarnation
    }.

-doc "A child faulted: refresh the spec from `Page`, decide, publish.".
-spec fault(root(), non_neg_integer(), chryso_root_spec:page(), non_neg_integer()) ->
    {decided(), root()}.
fault(Root, Child, Page, _Now) ->
    decide(Root, Child, Page).

-doc """
A PPC command. `Page` is what a refresh would read, used only when the
command reaches the restart path. Pass `none` when no refresh follows.
Returns the reply and, for an accepted restart, the decision it caused.
""".
-spec command(
    root(),
    non_neg_integer(),
    non_neg_integer(),
    [non_neg_integer()],
    non_neg_integer(),
    chryso_root_spec:page() | none
) -> {reply(), none | decided(), root()}.
command(#{config := #{pp := Pp}, command := Cmd} = Root, Op, Count, Args, Now, Page) ->
    case chryso_root_command:frame(Cmd, Pp, Pp, Op, Count) of
        {ok, Cmd1} ->
            {D, Cmd2} = chryso_root_command:decide(Cmd1, view(Root), Op, Args, Now, true),
            act(D, Root#{command := Cmd2}, Page);
        {Refused, Cmd1} ->
            {#{result => Refused, values => [0, 0, 0, 0]}, none, Root#{command := Cmd1}}
    end.

-doc "Per-child records, the retained policy and the published `seq`.".
-spec observe(root()) ->
    #{
        records := #{non_neg_integer() => chryso_root_policy:record()},
        retained := chryso_root_spec:policy(),
        seq := non_neg_integer()
    }.
observe(#{records := Records, spec := Spec, seq := Seq}) ->
    #{records => Records, retained => chryso_root_spec:retained(Spec), seq => Seq}.

-doc "Per-child records.".
-spec records(root()) -> #{non_neg_integer() => chryso_root_policy:record()}.
records(#{records := Records}) ->
    Records.

-spec act(chryso_root_command:decision(), root(), chryso_root_spec:page() | none) ->
    {reply(), none | decided(), root()}.
act(#{action := none, result := Result, values := Values}, #{command := Cmd} = Root, _Page) ->
    {#{result => Result, values => Values, tokens => chryso_root_command:tokens(Cmd)}, none, Root};
act(#{action := {restart, _}}, _Root, none) ->
    error(missing_spec_view);
act(#{action := {restart, Child}, values := [_, V1, _, _]} = D, Root, Page) ->
    {Decided, Root1} = decide(Root, Child, Page),
    #{command := Cmd} = Root1,
    case Decided of
        #{action := ignore_gone} ->
            Reply = #{result => refused, values => maps:get(values, D)},
            {Reply#{tokens => chryso_root_command:tokens(Cmd)}, Decided, Root1};
        #{action := Action, record := #{lifetime := Lifetime}, seq := Seq} ->
            Kind =
                case Action of
                    resume -> ?KIND_RESTART;
                    _ -> ?KIND_GIVEUP
                end,
            Values = [Kind, V1, Lifetime, Seq],
            Cmd1 = chryso_root_command:complete(Cmd, D#{values := Values}),
            Reply = #{result => ok, values => Values, tokens => chryso_root_command:tokens(Cmd1)},
            {Reply, Decided, Root1#{command := Cmd1}}
    end.

-spec decide(root(), non_neg_integer(), chryso_root_spec:page()) -> {decided(), root()}.
decide(Root, Child, Page) ->
    #{config := Config, records := Records, spec := Spec, seq := Seq} = Root,
    #{hard_max := HardMax, entry := Entry, compiled := Compiled, mapped := Mapped} = Config,
    #{beam := Beam} = Config,
    #{incarnation := Incarnation} = Root,
    {SpecResult, Spec1} = chryso_root_spec:consume(Page, Spec),
    Budget = chryso_root_spec:budget(Spec1, Child, lists:nth(Child + 1, Compiled), HardMax),
    {Action, Record} = chryso_root_policy:step(maps:get(Child, Records), Budget, Entry),
    %% Every decision publishes while the status page is mapped.
    Seq1 =
        case Mapped of
            true -> Seq + 2;
            false -> Seq
        end,
    Decided = #{
        child => Child,
        action => Action,
        record => Record,
        spec => SpecResult,
        retained => chryso_root_spec:retained(Spec1),
        seq => Seq1
    },
    %% A resumed BEAM is a new BEAM incarnation.
    Incarnation1 =
        case {Child, Action} of
            {Beam, resume} -> Incarnation + 1;
            _ -> Incarnation
        end,
    {Decided, Root#{
        records := Records#{Child := Record},
        spec := Spec1,
        seq := Seq1,
        incarnation := Incarnation1
    }}.

-spec view(root()) -> chryso_root_command:view().
view(#{config := Config, records := Records, seq := Seq, incarnation := Incarnation}) ->
    #{
        abi => maps:get(abi, Config),
        generation => maps:get(generation, Config),
        incarnation => Incarnation,
        cntfrq => maps:get(cntfrq, Config),
        seq => Seq,
        publishing => maps:get(mapped, Config),
        present => maps:get(present, Config),
        drivers => maps:get(drivers, Config),
        records => Records
    }.
