-module(chryso_root_bucket).
-moduledoc """
Root's command token bucket (`root_command_init` and `refill_step` in
`src/pd/root/command.c`). Its refill machine is `running`, then `off` for
good after an unavailable or backwards clock sample. `off` still spends the
tokens already held.

Laws, observed through `take/3` and `tokens/1`:

- **bounded**: tokens never exceed the capacity.
- **absorbing**: once `off`, the bucket never refills.
- **rate**: over an interval of `E` ticks while running, at most
  `capacity + E div interval` takes succeed.
""".

-export([new/4, take/3, tokens/1]).
-export_type([bucket/0]).

-doc "A bucket and its refill machine.".
-opaque bucket() :: #{
    refill := running | off,
    capacity := non_neg_integer(),
    tokens := non_neg_integer(),
    interval := non_neg_integer(),
    last := non_neg_integer()
}.

-doc """
A full bucket of `Capacity` tokens refilled evenly over `RefillTicks`.
Capacity 0, `RefillTicks` 0 or an invalid clock starts it `off`.
""".
-spec new(non_neg_integer(), non_neg_integer(), non_neg_integer(), boolean()) -> bucket().
new(Capacity, RefillTicks, Now, NowValid) ->
    Base = #{refill => off, capacity => Capacity, tokens => Capacity, interval => 0, last => Now},
    case Capacity =/= 0 andalso RefillTicks =/= 0 andalso NowValid of
        true ->
            %% Round up so tokens never arrive faster than the configured rate.
            Interval = RefillTicks div Capacity + min(1, RefillTicks rem Capacity),
            Base#{refill := running, interval := Interval};
        false ->
            Base
    end.

-doc "Refill from one clock sample, then spend one token if any is held.".
-spec take(non_neg_integer(), boolean(), bucket()) -> {ok | limited, bucket()}.
take(Now, NowValid, Bucket) ->
    case refill(Now, NowValid, Bucket) of
        #{tokens := 0} = B -> {limited, B};
        #{tokens := T} = B -> {ok, B#{tokens := T - 1}}
    end.

-doc "Tokens currently held.".
-spec tokens(bucket()) -> non_neg_integer().
tokens(#{tokens := T}) ->
    T.

-spec refill(non_neg_integer(), boolean(), bucket()) -> bucket().
refill(_Now, _Valid, #{refill := off} = B) ->
    B;
refill(Now, Valid, #{last := Last} = B) when not Valid; Now < Last ->
    B#{refill := off};
refill(Now, _Valid, #{capacity := Cap, tokens := T, interval := I, last := Last} = B) ->
    Due = (Now - Last) div I,
    Room = Cap - T,
    if
        Due =:= 0 -> B;
        Due >= Room -> B#{tokens := Cap, last := Now};
        true -> B#{tokens := T + Due, last := Last + Due * I}
    end.
