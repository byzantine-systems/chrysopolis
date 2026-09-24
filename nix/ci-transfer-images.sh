#!/usr/bin/env bash
# Transfer the image outputs and their store references between CI jobs.
# The full Nix store is too large to archive in the runner's root filesystem.
set -euo pipefail

if [[ $# != 2 ]]; then
  echo "usage: ci-transfer-images.sh export|import DIRECTORY" >&2
  exit 2
fi

mode=$1
transfer_dir=$2

case "$mode" in
  export)
    mkdir -p "$transfer_dir"
    nix build \
      .#test-image \
      .#default \
      .#disk \
      .#restart-image \
      .#budget-decay-image \
      .#cothread-probe-image \
      .#lifecycle-failure-image \
      --no-link --print-out-paths --print-build-logs > "$transfer_dir/outputs.txt"
    mapfile -t outputs < "$transfer_dir/outputs.txt"
    if [[ ${#outputs[@]} != 7 ]]; then
      echo "expected seven image and disk outputs, got ${#outputs[@]}" >&2
      exit 1
    fi
    nix-store --query --requisites "${outputs[@]}" > "$transfer_dir/closure.txt"
    mapfile -t closure < "$transfer_dir/closure.txt"
    nix-store --export "${closure[@]}" > "$transfer_dir/images.nar"
    (
      cd "$transfer_dir"
      sha256sum images.nar > images.nar.sha256
    )
    du -h "$transfer_dir/images.nar"
    ;;
  import)
    (
      cd "$transfer_dir"
      sha256sum -c images.nar.sha256
    )
    # The artifact comes from this workflow's image job and has no Nix key.
    # setup-nix marks the runner as trusted so this local import is allowed.
    nix-store --option require-sigs false --import < "$transfer_dir/images.nar"
    mapfile -t outputs < "$transfer_dir/outputs.txt"
    if [[ ${#outputs[@]} != 7 ]]; then
      echo "expected seven image and disk outputs, got ${#outputs[@]}" >&2
      exit 1
    fi
    for output in "${outputs[@]}"; do
      nix-store --check-validity "$output"
    done
    ;;
  *)
    echo "unknown mode: $mode" >&2
    exit 2
    ;;
esac
