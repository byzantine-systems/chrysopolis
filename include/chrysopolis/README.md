# Public C contracts

Only headers consumed by independently built Chrysopolis components belong here.

The generated orchestration contracts share the `<chrysopolis/...>` include namespace but never live here: `tools/abi` writes them into a build output directory, and the `project-structure` check rejects a committed copy.
