# Shared helpers for scripts/ci/vm/bake_image.sh and scripts/ci/vm/run_vm_tests.sh.
# shellcheck shell=bash

# Omit hv_time: under nested AMD SVM (kvm_amd on Azure Hyper-V EPYC runners), exposing
# hv_time triggers an intermittent early-boot AP #2 LAPIC TPR=0xf0 (HIGH_LEVEL) deadlock
# in KiSystemStartup / KiIpiStallOnPacketTargets.
QEMU_CPU="${QEMU_CPU:-host,hv_relaxed,hv_spinlocks=0x1fff,hv_vapic}"

log() { echo "=== [$(date -u +%T)] $* ==="; }

host_cpu_model() {
  # Avoid SIGPIPE under pipefail when lscpu emits multiple Model name lines on hybrid CPUs.
  lscpu | sed -n 's/^Model name:[[:space:]]*//p; T; q'
}
