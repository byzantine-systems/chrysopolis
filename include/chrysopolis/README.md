# Public C contracts

Only headers consumed by independently built Chrysopolis components belong
here. A3 found no existing C header shared across protection domains, so the
current Root and BEAM headers stay private in `src/runtime/` until their
owners move in A4 and A5. Generated ABI headers remain build outputs.
