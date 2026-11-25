# Shared helpers for scripts/ci/vm/bake_image.sh and scripts/ci/vm/run_vm_tests.sh.
# shellcheck shell=bash

QEMU_CPU="${QEMU_CPU:-host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic,hv_time}"

log() { echo "=== [$(date -u +%T)] $* ==="; }

host_cpu_model() {
  # Avoid SIGPIPE under pipefail when lscpu emits multiple Model name lines on hybrid CPUs.
  lscpu | sed -n 's/^Model name:[[:space:]]*//p; T; q'
}
