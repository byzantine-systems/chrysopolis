"""What an oracle allows: some values, or anything. The fault model allows a
step's outcomes, the crash oracle a file's contents, and both join what two
histories allow with `|`."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class Allowed[T]:
    """The values an observation may take. `anything` admits every value, so
    no sentinel such as None has to mean it."""

    values: frozenset[T] = frozenset()
    anything: bool = False

    @classmethod
    def of(cls, *values: T) -> Allowed[T]:
        return cls(frozenset(values))

    @classmethod
    def everything(cls) -> Allowed[T]:
        return cls(anything=True)

    def __contains__(self, value: object) -> bool:
        return self.anything or value in self.values

    def __or__(self, other: Allowed[T]) -> Allowed[T]:
        return Allowed(self.values | other.values, self.anything or other.anything)

    def only(self) -> T | None:
        """The one value allowed, if exactly one is."""
        if self.anything or len(self.values) != 1:
            return None
        (value,) = self.values
        return value
