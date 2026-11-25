#!/usr/bin/env bash
# Boots a baked Windows Server Core image under QEMU/KVM, installs Dokan with Driver Verifier armed,
# runs one test shard, and collects logs, fstest scorecards and crash dumps.
#
# Usage: run_vm_tests.sh <image_dir> <payload_dir> <results_dir> [shard] [simulate]
#   shard:    all | shard1 | shard2 (validated by run_guest_tests.ps1; default: all)
#   simulate: none | bugcheck | hang (default: none; crash-capture self-test)
#
# <payload_dir> must contain DokanSetup.exe (plus fsx.exe and fstest/dokan_fstest.exe when simulate=none).
#
# Environment:
#   GUEST_TEST_TIMEOUT    Seconds before a running shard is declared hung (default: 1800; 300 for bugcheck, 120 for hang).
#   SSH_PORT              Loopback port forwarded to guest SSH (default: 2222; override for concurrent local runs).
#   QEMU_CPU              Override the QEMU -cpu model (for investigations).
#   ALLOW_PATCHGUARD_0X109  If 'true', retry once on known nested-KVM PatchGuard 0x109 (type 0x115) and warn
#                           instead of failing if it reproduces (dump is still uploaded for kd.exe verification).
#
# Guest network: user-mode NAT with restrict=on (no outbound access); only SSH is forwarded to localhost.
# A hung guest gets an NMI (the image sets CrashControl\NMICrashDump=1), turning the hang into MEMORY.DMP.
set -euo pipefail

usage='usage: run_vm_tests.sh <image_dir> <payload_dir> <results_dir> [shard] [none|bugcheck|hang]'
IMAGE_DIR="${1:?$usage}"
PAYLOAD_DIR="${2:?$usage}"
RESULTS_DIR="${3:?$usage}"
SHARD="${4:-all}"
SIMULATE="${5:-none}"
if [[ ! "$SHARD" =~ ^[a-zA-Z0-9_-]+$ ]]; then
  echo "ERROR: invalid shard '$SHARD'. $usage" >&2
  exit 2
fi
case "$SIMULATE" in none|bugcheck|hang) ;; *) echo "ERROR: invalid simulate mode '$SIMULATE'. $usage" >&2; exit 2 ;; esac

if [[ "$SIMULATE" == "hang" ]]; then
  GUEST_TEST_TIMEOUT="${GUEST_TEST_TIMEOUT:-120}"
elif [[ "$SIMULATE" == "bugcheck" ]]; then
  GUEST_TEST_TIMEOUT="${GUEST_TEST_TIMEOUT:-300}"
else
  GUEST_TEST_TIMEOUT="${GUEST_TEST_TIMEOUT:-1800}"
fi
SSH_PORT="${SSH_PORT:-2222}"
ALLOW_PATCHGUARD_0X109="${ALLOW_PATCHGUARD_0X109:-false}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# shellcheck source=scripts/ci/vm/common.sh
source "$SCRIPT_DIR/common.sh"

WORK_DIR="$(mktemp -d "${RUNNER_TEMP:-/tmp}/vm-test.XXXXXX")"
chmod 700 "$WORK_DIR"
mkdir -p "$RESULTS_DIR"

BASE_IMG="$IMAGE_DIR/base.qcow2"
SSH_KEY="$IMAGE_DIR/id_ed25519"
OVERLAY_IMG="$WORK_DIR/overlay.qcow2"
MON_SOCK="$WORK_DIR/monitor.sock"
SERIAL_LOG="$RESULTS_DIR/serial.log"
BUGCHECK_LOG="$WORK_DIR/bugcheck_events.txt"
chmod 600 "$SSH_KEY"
: > "$SERIAL_LOG"

log "Host CPU: $(host_cpu_model); QEMU CPU model: $QEMU_CPU"

validate_payload() {
  if [[ ! -f "$PAYLOAD_DIR/DokanSetup.exe" ]]; then
    echo "ERROR: payload is missing $PAYLOAD_DIR/DokanSetup.exe." >&2
    exit 1
  fi
  if [[ "$SIMULATE" == "none" ]]; then
    if [[ ! -f "$PAYLOAD_DIR/fsx.exe" || ! -f "$PAYLOAD_DIR/fstest/dokan_fstest.exe" ]]; then
      echo "ERROR: payload is incomplete (need DokanSetup.exe, fsx.exe, fstest/dokan_fstest.exe under $PAYLOAD_DIR)." >&2
      exit 1
    fi
  fi
  local symlinks
  symlinks="$(find "$PAYLOAD_DIR" -type l 2>/dev/null | head -n 5 || true)"
  if [[ -n "$symlinks" ]]; then
    echo "ERROR: refusing to stage symlinks found under $PAYLOAD_DIR:" >&2
    echo "$symlinks" >&2
    exit 1
  fi
}

QEMU_PID=""
stop_qemu() {
  if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
    kill "$QEMU_PID" 2>/dev/null || true
    for _ in $(seq 1 10); do
      kill -0 "$QEMU_PID" 2>/dev/null || break
      sleep 1
    done
    if kill -0 "$QEMU_PID" 2>/dev/null; then
      kill -9 "$QEMU_PID" 2>/dev/null || true
    fi
    wait "$QEMU_PID" 2>/dev/null || true
  fi
  QEMU_PID=""
}

cleanup() {
  stop_qemu
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

start_qemu() {
  stop_qemu
  rm -f "$OVERLAY_IMG" "$MON_SOCK"
  # Throw-away copy-on-write overlay: the cached base image is never modified.
  qemu-img create -q -f qcow2 -b "$BASE_IMG" -F qcow2 "$OVERLAY_IMG"
  qemu-system-x86_64 \
    -enable-kvm \
    -machine q35,accel=kvm \
    -cpu "$QEMU_CPU" \
    -smp 4 \
    -m 6G \
    -display none \
    -vga std \
    -monitor "unix:$MON_SOCK,server,nowait" \
    -serial "file:$SERIAL_LOG" \
    -device ahci,id=ahci \
    -drive "file=$OVERLAY_IMG,format=qcow2,if=none,id=hd0,cache=writeback" \
    -device ide-hd,drive=hd0,bus=ahci.0 \
    -netdev "user,id=net0,restrict=on,hostfwd=tcp:127.0.0.1:${SSH_PORT}-:22" \
    -device e1000e,netdev=net0 &
  QEMU_PID=$!
}

qemu_monitor() {
  printf '%s\n' "$@" | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null 2>&1 || true
}

SSH_OPTS=(-F /dev/null -i "$SSH_KEY" -o IdentitiesOnly=yes -o ForwardAgent=no -o ForwardX11=no
          -o ClearAllForwardings=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o BatchMode=yes -o ConnectTimeout=15 -o ServerAliveInterval=15 -o ServerAliveCountMax=4
          -p "$SSH_PORT")
SCP_OPTS=(-F /dev/null -i "$SSH_KEY" -o IdentitiesOnly=yes -o ForwardAgent=no -o ForwardX11=no
          -o ClearAllForwardings=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
          -o BatchMode=yes -o ConnectTimeout=10 -P "$SSH_PORT")
# Commands are PowerShell executed by the guest's sshd, so they are meant to be passed through verbatim.
# shellcheck disable=SC2029
guest_ssh() { ssh "${SSH_OPTS[@]}" Administrator@127.0.0.1 "$@"; }

get_guest_boot_time() {
  guest_ssh '(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToString("o")' 2>/dev/null | tr -d '\r\n'
}

wait_for_ssh() {
  local max_wait="$1" prev_boot="${2:-}" start attempt=0 err="" cur_boot=""
  start=$(date +%s)
  while true; do
    attempt=$((attempt + 1))
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
      echo "ERROR: QEMU exited while waiting for SSH" >&2
      return 1
    fi
    if [[ -n "$prev_boot" ]]; then
      if cur_boot=$(get_guest_boot_time) && [[ -n "$cur_boot" && "$cur_boot" != "$prev_boot" ]]; then
        return 0
      fi
      err="waiting for reboot (boot time still '${cur_boot:-unreachable}')"
    else
      if err=$(guest_ssh 'Write-Output ready' 2>&1); then
        return 0
      fi
    fi
    if (( attempt % 10 == 1 )); then
      echo "[wait_for_ssh +$(( $(date +%s) - start ))s] $(echo "$err" | tr '\n' ' ' | cut -c1-200)"
    fi
    if (( $(date +%s) - start >= max_wait )); then
      echo "ERROR: no SSH after ${max_wait}s; saving screenshot." >&2
      qemu_monitor "screendump $RESULTS_DIR/no-ssh-screen.ppm"
      return 1
    fi
    sleep 3
  done
}

# Stage the guest's C:\ci tree: the installer, run_guest_tests.ps1, the samples\ harnesses as in
# the repository (with fsx.exe where they expect it), and the dokan_fstest runner + baselines.
stage_guest_files() {
  local stage="$WORK_DIR/guest-ci"
  rm -rf "$stage"
  mkdir -p "$stage/repo/samples/fstools/src/fsx" "$stage/repo/scripts"
  cp "$PAYLOAD_DIR/DokanSetup.exe" "$stage/"
  if [[ -f "$PAYLOAD_DIR/fsx.exe" ]]; then
    cp "$PAYLOAD_DIR/fsx.exe" "$stage/repo/samples/fstools/src/fsx/"
  fi
  if [[ -d "$PAYLOAD_DIR/fstest" ]]; then
    cp -r "$PAYLOAD_DIR/fstest" "$stage/"
  fi
  cp "$SCRIPT_DIR/run_guest_tests.ps1" "$stage/"
  cp "$REPO_ROOT/samples/memfs_test.ps1" "$REPO_ROOT/samples/mirror_test.ps1" "$stage/repo/samples/"
  cp "$REPO_ROOT/scripts/run_ifstest.ps1" "$REPO_ROOT/scripts/fstest_common.ps1" "$stage/repo/scripts/"
  guest_ssh 'Remove-Item -Recurse -Force C:\ci -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force -Path C:\ci | Out-Null'
  scp "${SCP_OPTS[@]}" -r "$stage"/* "Administrator@127.0.0.1:C:/ci/"
}

NMI_INJECTED=false
inject_nmi() {
  log "Guest is unresponsive: saving screenshot and injecting NMI (BugCheck 0x80 -> MEMORY.DMP)"
  qemu_monitor "screendump $RESULTS_DIR/hang-screen.ppm" "nmi"
  NMI_INJECTED=true
}

DUMP_COLLECTED=false
# Brings the guest back after a crash or hang if needed, then copies logs, scorecards and MEMORY.DMP.
collect_results() {
  local test_rc="$1" prev_boot="$2" guest_up=false expect_crash=0
  if [[ $test_rc -eq 124 ]]; then
    echo "Test shard exceeded ${GUEST_TEST_TIMEOUT}s."
    inject_nmi
    expect_crash=1
    sleep 10
    if wait_for_ssh 480 "$prev_boot"; then guest_up=true; fi
  elif [[ $test_rc -ne 0 ]]; then
    # SSH can drop because of a real BugCheck: give Windows time to write the dump and reboot.
    sleep 8
    if [[ $test_rc -eq 255 ]]; then
      expect_crash=1
    fi
    if wait_for_ssh 180; then
      guest_up=true
    elif kill -0 "$QEMU_PID" 2>/dev/null; then
      inject_nmi
      expect_crash=1
      sleep 10
      if wait_for_ssh 480 "$prev_boot"; then guest_up=true; fi
    fi
  else
    if wait_for_ssh 60; then guest_up=true; fi
  fi

  if [[ "$guest_up" != true ]]; then
    echo "ERROR: guest did not come back; no logs or dump can be collected." >&2
    return 1
  fi

  local dump_rc=0
  : > "$BUGCHECK_LOG"
  # After a crash, wait (up to ~45s) for SaveDump to finish copying C:\Windows\MEMORY.DMP from pagefile.sys.
  # Never break early when a crash/NMI is expected or while MEMORY.DMP is still growing.
  # shellcheck disable=SC2016 # PowerShell variables, expanded by the guest.
  guest_ssh "
    \$expectCrash = ($expect_crash -ne 0)
    \$dump = 'C:\Windows\MEMORY.DMP'
    for (\$i = 0; \$i -lt 15; \$i++) {
      \$crashed = Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001; ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting' } -MaxEvents 1 -ErrorAction SilentlyContinue
      \$hasDump = Test-Path -LiteralPath \$dump
      if (-not \$expectCrash -and -not \$crashed -and -not \$hasDump -and \$i -ge 2) { break }
      if (\$hasDump) {
        \$size = (Get-Item -LiteralPath \$dump).Length; Start-Sleep -Seconds 3
        if (\$size -gt 0 -and \$size -eq (Get-Item -LiteralPath \$dump).Length) { break }
      } else { Start-Sleep -Seconds 3 }
    }
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 1001; ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting' } -MaxEvents 3 -ErrorAction SilentlyContinue |
      Format-List TimeCreated, Message
    if (Test-Path -LiteralPath \$dump) { exit 42 }
    exit 0
  " > "$BUGCHECK_LOG" || dump_rc=$?
  cat "$BUGCHECK_LOG"

  scp "${SCP_OPTS[@]}" "Administrator@127.0.0.1:C:/ci/dokan_install*.log" "$RESULTS_DIR/" 2>/dev/null || true
  scp "${SCP_OPTS[@]}" "Administrator@127.0.0.1:C:/ci/guest_tests.log" "$RESULTS_DIR/" 2>/dev/null || true
  scp "${SCP_OPTS[@]}" -r "Administrator@127.0.0.1:C:/ci/fstest-results" "$RESULTS_DIR/" 2>/dev/null || true

  if [[ $dump_rc -eq 42 ]]; then
    log "Collecting C:\\Windows\\MEMORY.DMP"
    rm -f "$WORK_DIR/MEMORY.DMP" "$RESULTS_DIR/MEMORY.DMP.7z"
    if scp "${SCP_OPTS[@]}" "Administrator@127.0.0.1:C:/Windows/MEMORY.DMP" "$WORK_DIR/MEMORY.DMP" \
        && 7z a -t7z -mx=3 "$RESULTS_DIR/MEMORY.DMP.7z" "$WORK_DIR/MEMORY.DMP" >/dev/null; then
      rm -f "$WORK_DIR/MEMORY.DMP"
      echo "MEMORY.DMP.7z: $(du -h "$RESULTS_DIR/MEMORY.DMP.7z" | cut -f1) (NMI injected: $NMI_INJECTED)"
      DUMP_COLLECTED=true
    else
      rm -f "$WORK_DIR/MEMORY.DMP" "$RESULTS_DIR/MEMORY.DMP.7z"
      echo "ERROR: failed to collect or compress C:\\Windows\\MEMORY.DMP." >&2
    fi
  elif [[ "$NMI_INJECTED" == true ]]; then
    echo "ERROR: an NMI was injected but Windows wrote no MEMORY.DMP." >&2
  fi

  # Best-effort graceful shutdown with a 15s cap (overlay is throw-away and artifacts are already copied).
  guest_ssh 'Stop-Computer -Force' >/dev/null 2>&1 || true
  for _ in $(seq 1 15); do
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 1
  done
  stop_qemu

  [[ $dump_rc -eq 0 && "$DUMP_COLLECTED" != true && "$NMI_INJECTED" != true ]]
}

is_known_patchguard_0x109() {
  [[ "$DUMP_COLLECTED" == true ]] \
    && grep -qiE '0x0*109\b' "$BUGCHECK_LOG" \
    && grep -qiE '0x0*115\b' "$BUGCHECK_LOG"
}

validate_payload

max_attempts=1
if [[ "$SIMULATE" == "none" && "$ALLOW_PATCHGUARD_0X109" == "true" ]]; then
  max_attempts=2
fi

attempt=1
boot_secs=0
boot_retries=0
test_secs=0
test_rc=0
collect_rc=0
patchguard_tolerated=false

while true; do
  NMI_INJECTED=false
  DUMP_COLLECTED=false
  boot_retries=0
  rm -rf "$RESULTS_DIR/fstest-results" "$RESULTS_DIR/MEMORY.DMP.7z" \
    "$RESULTS_DIR/no-ssh-screen.ppm" "$RESULTS_DIR/hang-screen.ppm" \
    "$RESULTS_DIR/guest_tests.log" "$RESULTS_DIR"/dokan_install*.log

  log "Booting guest (shard=$SHARD, simulate=$SIMULATE, attempt=$attempt/$max_attempts)"
  t_boot_total=$(date +%s)
  start_qemu
  # Under nested KVM Windows occasionally stalls on the boot logo; retry once with a fresh overlay.
  if ! wait_for_ssh 90; then
    boot_retries=1
    echo "WARNING: guest not reachable after 90s; retrying boot once with a fresh overlay." >&2
    start_qemu
    wait_for_ssh 150
    rm -f "$RESULTS_DIR/no-ssh-screen.ppm"
  fi
  boot_secs=$(( $(date +%s) - t_boot_total ))
  log "Guest reachable over SSH after ${boot_secs}s (boot retries: ${boot_retries})"

  pre_boot_time="$(get_guest_boot_time || true)"
  stage_guest_files

  guest_cmd="& C:\\ci\\run_guest_tests.ps1 -Shard $SHARD"
  case "$SIMULATE" in
    bugcheck) guest_cmd+=" -SimulateBugCheck" ;;
    hang) guest_cmd+=" -SimulateHang" ;;
    none|*) ;;
  esac

  log "Running: $guest_cmd (timeout ${GUEST_TEST_TIMEOUT}s)"
  t_test=$(date +%s)
  set +e
  timeout "$GUEST_TEST_TIMEOUT" ssh "${SSH_OPTS[@]}" Administrator@127.0.0.1 "$guest_cmd"
  test_rc=$?
  set -e
  test_secs=$(( $(date +%s) - t_test ))
  log "Guest tests exited with code $test_rc after ${test_secs}s"

  collect_rc=0
  collect_results "$test_rc" "$pre_boot_time" || collect_rc=$?

  if [[ "$SIMULATE" == "none" && ( $test_rc -ne 0 || $collect_rc -ne 0 ) && "$ALLOW_PATCHGUARD_0X109" == "true" ]]; then
    if is_known_patchguard_0x109; then
      if (( attempt < max_attempts )); then
        echo "::warning::Known nested-KVM PatchGuard 0x109 (type 0x115) hit on attempt $attempt; retrying once on a fresh overlay."
        attempt=$((attempt + 1))
        continue
      fi
      echo "::warning::Known nested-KVM PatchGuard 0x109 (type 0x115) reproduced on attempt $attempt; dump uploaded for kd.exe verification."
      patchguard_tolerated=true
    fi
  fi
  break
done

cat <<EOF
=== Summary ===
Boot to SSH:           ${boot_secs}s (retries: ${boot_retries})
Tests:                 ${test_secs}s (exit code $test_rc, attempts: $attempt)
NMI injected:          $NMI_INJECTED
Crash dump collected:  $DUMP_COLLECTED
PatchGuard tolerated:  $patchguard_tolerated
EOF

if [[ "$SIMULATE" != "none" ]]; then
  # Self-test: success means a dump was produced and collected.
  [[ "$DUMP_COLLECTED" == true ]]
  exit $?
fi
if [[ "$patchguard_tolerated" == true ]]; then
  exit 0
fi
[[ $test_rc -eq 0 && $collect_rc -eq 0 ]]
