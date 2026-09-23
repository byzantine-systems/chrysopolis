# Public C contracts

Only headers consumed by independently built Chrysopolis components belong
here. A3 found no existing C header shared across protection domains, so the
Root's policy header stays private in `src/pd/root/policy/`. BEAM headers
remain private in `src/runtime/` until their owners move in A5. Generated ABI
headers remain build outputs.
