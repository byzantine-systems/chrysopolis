"""The fault model's condition table against the shipped classifier.

`faults.condition` and `chryso_condition:classify/2` are two statements of
one table. `packages.condition-vectors` prints what the Erlang classifier says
for every Root row with lifetime and budget up to 4, at the guest's parameters.
The domain is small and finite, so the model is checked against that
independent oracle on every row, not a sample.

| Planted defect | Caught by |
|---|---|
| the table maps `stopped` to `ready` | test_the_model_agrees_with_chryso_condition |
| the budget floor off by one | test_the_model_agrees_with_chryso_condition |
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import get_args

import pytest
from pydantic import BaseModel, ConfigDict, Field

from chryso_testbed.faults import RowState, condition
from chryso_testbed.outcomes import Condition, ConditionName, Reason

HEADER = "chryso-condition-vectors 1"


class Vector(BaseModel):
    """One row the classifier printed, validated at the boundary."""

    model_config = ConfigDict(frozen=True, extra="forbid", strict=True)

    state: RowState
    lifetime: int = Field(ge=0)
    budget: int = Field(ge=0)
    condition: ConditionName
    reason: Reason

    @property
    def id(self) -> str:
        return f"{self.state}-{self.lifetime}-of-{self.budget}"


def vectors() -> Path:
    root = os.environ.get("CHRYSO_CONDITION_VECTORS")
    if root is None:
        pytest.fail("CHRYSO_CONDITION_VECTORS is not set")
    return Path(root)


def read() -> list[Vector]:
    lines = (vectors() / "vectors.jsonl").read_text().splitlines()
    return [Vector.model_validate_json(line) for line in lines]


ROWS = read() if "CHRYSO_CONDITION_VECTORS" in os.environ else []


@pytest.mark.parametrize("row", [pytest.param(r, id=r.id) for r in ROWS])
def test_the_model_agrees_with_chryso_condition(row: Vector) -> None:
    assert condition(row.state, row.lifetime, row.budget) == Condition(row.condition, row.reason)


def test_every_row_was_read_and_every_state_is_there() -> None:
    """The parametrized law is vacuous on an empty or partial file."""
    header, count = (vectors() / "manifest.txt").read_text().splitlines()
    rows = read()
    assert header == HEADER
    assert count == f"count {len(rows)}"
    assert {r.state for r in rows} == set(get_args(RowState.__value__))
