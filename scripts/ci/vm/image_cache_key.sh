#!/usr/bin/env bash
# Prints the deterministic Actions cache key for one Windows Server Core VM image.
# Single source of truth shared by vm-image, vm-test, and crash-selftest; hashes only the
# requested OS's env file so updating one ISO URL does not invalidate the other OS images.
set -euo pipefail

OS_ID="${1:?usage: image_cache_key.sh <os_id>}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
OS_ENV="$SCRIPT_DIR/os/${OS_ID}.env"

if [[ ! "$OS_ID" =~ ^[a-z0-9_-]+$ ]] || [[ ! -f "$OS_ENV" ]]; then
  echo "ERROR: unsupported OS '$OS_ID' (missing $OS_ENV)." >&2
  exit 2
fi

mapfile -t files < <(
  {
    printf '%s\n' \
      "scripts/ci/vm/os/${OS_ID}.env" \
      "scripts/ci/vm/bake_image.sh" \
      "scripts/ci/vm/common.sh" \
      "scripts/install_wlk_ifstest.ps1" \
      "cert/DokanCA.cer"
    (cd "$REPO_ROOT" && find scripts/ci/vm/provisioning -type f | LC_ALL=C sort)
  }
)

digest="$(cd "$REPO_ROOT" && sha256sum "${files[@]}" | sha256sum | cut -d' ' -f1)"
printf 'vm-image-%s-%s\n' "$OS_ID" "$digest"
