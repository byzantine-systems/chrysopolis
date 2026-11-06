"""Hypothesis profiles for the fuzz (`-p chryso_testbed.fuzzing`): `explicit`
replays the pinned examples, `nightly` keeps the example database on CI.
"""

from __future__ import annotations

import pytest
from hypothesis import Phase, settings
from hypothesis.database import DirectoryBasedExampleDatabase

from chryso_testbed import log

settings.register_profile("explicit", phases=[Phase.explicit])
settings.register_profile(
    "nightly", database=DirectoryBasedExampleDatabase(".hypothesis/examples"), print_blob=True
)


def pytest_configure(config: pytest.Config) -> None:
    log.configure()
