"""Hypothesis profiles for the fuzz, a pytest plugin loaded with `-p chryso_testbed.fuzzing`.

`explicit` runs only the pinned `@example`s, cheap enough for every PR.
`nightly` keeps an example database on disk even on CI, where Hypothesis
otherwise turns it off, so a failure found one night replays first the next.
"""

from __future__ import annotations

from hypothesis import Phase, settings
from hypothesis.database import DirectoryBasedExampleDatabase

settings.register_profile("explicit", phases=[Phase.explicit])
settings.register_profile(
    "nightly", database=DirectoryBasedExampleDatabase(".hypothesis/examples"), print_blob=True
)
