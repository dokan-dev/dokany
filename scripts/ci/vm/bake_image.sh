#!/usr/bin/env bash
# Bakes a test-ready Windows Server Core qcow2 image under QEMU/KVM (unattended install + provisioning).
#
# Usage: bake_image.sh <os_id> <output_dir>
#   os_id: must match scripts/ci/vm/os/<os_id>.env (e.g. server2016, server2019, server2022, server2025)
#
# Output (<output_dir>): base.qcow2 (compacted, uncompressed; actions/cache compresses it with zstd)
# and id_ed25519 (the key authorized for SSH into the guest as Administrator).
set -euo pipefail

usage='usage: bake_image.sh <os_id> <output_dir>'
OS_ID="${1:?$usage}"
OUT_DIR="${2:?$usage}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PROVISIONING_DIR="$SCRIPT_DIR/provisioning"
# shellcheck source=scripts/ci/vm/common.sh
source "$SCRIPT_DIR/common.sh"

OS_ENV="$SCRIPT_DIR/os/${OS_ID}.env"
if [[ ! "$OS_ID" =~ ^[a-z0-9_-]+$ ]] || [[ ! -f "$OS_ENV" ]]; then
  echo "ERROR: unsupported OS '$OS_ID' (missing $OS_ENV). $usage" >&2
  exit 2
fi
# shellcheck source=/dev/null
source "$OS_ENV"
: "${ISO_URL:?ISO_URL not set in $OS_ENV}"
: "${IMAGE_INDEX:?IMAGE_INDEX not set in $OS_ENV}"

EPHEMERAL_WORK_DIR=false
if [[ -n "${RUNNER_TEMP:-}" ]]; then
  WORK_DIR="${RUNNER_TEMP}/vm-bake-${OS_ID}"
  mkdir -p "$WORK_DIR"
else
  WORK_DIR="$(mktemp -d "/tmp/vm-bake-${OS_ID}.XXXXXX")"
  EPHEMERAL_WORK_DIR=true
fi
chmod 700 "$WORK_DIR"
mkdir -p "$OUT_DIR"

# Win32-OpenSSH 10.x (currently published as Preview by PowerShell/Win32-OpenSSH); pinned by SHA-256.
OPENSSH_URL="https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win64.zip"
OPENSSH_SHA256="b67ff8becd203502190ec42ac7787c24c2cd3325ed7700485fe7a1c83d2a7957"

CURL_OPTS=(
  --proto '=https' --tlsv1.2
  --connect-timeout 30 --speed-limit 102400 --speed-time 60 --max-time 900
  --retry 3 --retry-delay 5 --retry-all-errors
  -fsSL
)

TAIL_PID=""
WATCH_PID=""
cleanup() {
  if [[ -n "$TAIL_PID" ]]; then kill "$TAIL_PID" 2>/dev/null || true; fi
  if [[ -n "$WATCH_PID" ]]; then kill "$WATCH_PID" 2>/dev/null || true; fi
  pkill -f "qemu-system-x86_64.*$WORK_DIR/install.qcow2" 2>/dev/null || true
  if [[ -n "$TAIL_PID" ]]; then wait "$TAIL_PID" 2>/dev/null || true; fi
  if [[ -n "$WATCH_PID" ]]; then wait "$WATCH_PID" 2>/dev/null || true; fi
  if [[ "$EPHEMERAL_WORK_DIR" == true ]]; then
    rm -rf "$WORK_DIR"
  fi
}
trap cleanup EXIT

log "Host CPU: $(host_cpu_model); QEMU CPU model: $QEMU_CPU"

log "Downloading Windows evaluation ISO ($OS_ID)"
t0=$(date +%s)
curl "${CURL_OPTS[@]}" -o "$WORK_DIR/windows.iso" "$ISO_URL"
iso_bytes=$(stat -c%s "$WORK_DIR/windows.iso")
iso_magic=$(dd if="$WORK_DIR/windows.iso" bs=1 skip=32769 count=5 2>/dev/null | tr -cd '[:alnum:]' || true)
if (( iso_bytes < 2147483648 )) || [[ "$iso_magic" != "CD001" && "$iso_magic" != "BEA01" ]]; then
  echo "ERROR: downloaded $WORK_DIR/windows.iso does not look like a valid Windows ISO (size=${iso_bytes}B, magic='${iso_magic}')." >&2
  echo "Check whether ISO_URL in $OS_ENV expired or returned an HTML page." >&2
  exit 1
fi
echo "windows.iso: $(du -h "$WORK_DIR/windows.iso" | cut -f1) in $(( $(date +%s) - t0 ))s, magic $iso_magic, sha256 $(sha256sum "$WORK_DIR/windows.iso" | cut -d' ' -f1)"

log "Downloading Win32-OpenSSH"
curl "${CURL_OPTS[@]}" -o "$WORK_DIR/OpenSSH-Win64.zip" "$OPENSSH_URL"
echo "$OPENSSH_SHA256  $WORK_DIR/OpenSSH-Win64.zip" | sha256sum --check --quiet

log "Building seed ISO"
SEED_DIR="$WORK_DIR/seed"
rm -rf "$SEED_DIR"
mkdir -p "$SEED_DIR"
rm -f "$OUT_DIR/id_ed25519" "$OUT_DIR/id_ed25519.pub"
ssh-keygen -q -t ed25519 -N "" -C "dokany-ci" -f "$OUT_DIR/id_ed25519"
cp "$OUT_DIR/id_ed25519.pub" "$SEED_DIR/authorized_keys"
rm -f "$OUT_DIR/id_ed25519.pub"
cp "$WORK_DIR/OpenSSH-Win64.zip" "$REPO_ROOT/cert/DokanCA.cer" "$REPO_ROOT/scripts/install_wlk_ifstest.ps1" \
   "$PROVISIONING_DIR/provision_guest.ps1" "$SEED_DIR/"
sed "s/@IMAGE_INDEX@/$IMAGE_INDEX/" "$PROVISIONING_DIR/autounattend.xml" > "$SEED_DIR/autounattend.xml"
sed 's/\r$//; s/$/\r/' "$PROVISIONING_DIR/run_provisioning.cmd" > "$SEED_DIR/run_provisioning.cmd"
xmllint --noout "$SEED_DIR/autounattend.xml"
genisoimage -quiet -J -r -V DOKANSEED -o "$WORK_DIR/seed.iso" "$SEED_DIR"

SERIAL_LOG="$WORK_DIR/serial.log"
DIAG_DIR="$WORK_DIR/diag"
MON_SOCK="$WORK_DIR/monitor.sock"
rm -rf "$DIAG_DIR"
mkdir -p "$DIAG_DIR"

stop_bake_qemu() {
  printf 'quit\n' | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null 2>&1 || true
  sleep 2
  pkill -f "qemu-system-x86_64.*$WORK_DIR/install.qcow2" 2>/dev/null || true
}

# Screenshot every 2 minutes (uploaded on failure). Before provisioning starts (empty serial.log),
# 4 identical screenshots in a row (6-8 minutes) indicate a nested-KVM boot-logo stall; once
# provisioning starts, 5 consecutive ticks (10 minutes) with no new serial output indicate a hang.
watch_install() {
  local i=0 last="" same=0 sum ppm last_serial_size=0 serial_same=0 cur_size sleep_pid=""
  trap '[[ -n "$sleep_pid" ]] && kill "$sleep_pid" 2>/dev/null || true; exit 0' TERM INT
  while true; do
    sleep 120 >/dev/null 2>&1 &
    sleep_pid=$!
    wait "$sleep_pid" || break
    sleep_pid=""
    i=$((i + 1))
    ppm="$(printf '%s/screen-%03d.ppm' "$DIAG_DIR" "$i")"
    printf 'screendump %s\n' "$ppm" | socat - "UNIX-CONNECT:$MON_SOCK" >/dev/null 2>&1 || continue
    sleep 2
    if [[ -s "$SERIAL_LOG" ]]; then
      cur_size=$(stat -c%s "$SERIAL_LOG" 2>/dev/null || echo 0)
      if (( cur_size == last_serial_size )); then
        serial_same=$((serial_same + 1))
      else
        serial_same=0
      fi
      last_serial_size=$cur_size
      if (( serial_same >= 5 )); then
        echo "Provisioning serial log unchanged for 10 minutes; stopping QEMU." >&2
        touch "$WORK_DIR/install-stalled"
        stop_bake_qemu
        return
      fi
      continue
    fi
    [[ -f "$ppm" ]] || continue
    sum=$(md5sum "$ppm" 2>/dev/null | cut -d' ' -f1 || true)
    if [[ -n "$sum" && "$sum" == "$last" ]]; then same=$((same + 1)); else same=0; fi
    last="$sum"
    if (( same >= 3 )); then
      echo "Installer screen unchanged across 4 screenshots (6-8 minutes); stopping QEMU." >&2
      touch "$WORK_DIR/install-stalled"
      stop_bake_qemu
      return
    fi
  done
}

install_windows() {
  : > "$SERIAL_LOG"
  rm -f "$MON_SOCK" "$WORK_DIR/install-stalled"
  tail -f "$SERIAL_LOG" &
  TAIL_PID=$!
  watch_install &
  WATCH_PID=$!
  local rc=0
  # 20 minute cap per attempt so 2 attempts + ISO download + compaction stay inside the 60m job timeout.
  timeout 1200 qemu-system-x86_64 \
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
    -drive "file=$WORK_DIR/install.qcow2,format=qcow2,if=none,id=hd0,cache=unsafe,discard=unmap" \
    -device ide-hd,drive=hd0,bus=ahci.0,rotation_rate=1 \
    -drive "file=$WORK_DIR/windows.iso,media=cdrom,if=none,id=cd0" \
    -device ide-cd,drive=cd0,bus=ahci.1 \
    -drive "file=$WORK_DIR/seed.iso,media=cdrom,if=none,id=cd1" \
    -device ide-cd,drive=cd1,bus=ahci.2 \
    -netdev user,id=net0 \
    -device e1000e,netdev=net0 \
    -boot order=dc || rc=$?
  kill "$TAIL_PID" "$WATCH_PID" 2>/dev/null || true
  wait "$TAIL_PID" "$WATCH_PID" 2>/dev/null || true
  TAIL_PID=""
  WATCH_PID=""
  return "$rc"
}

for attempt in 1 2; do
  log "Installing and provisioning Windows ($OS_ID) under QEMU/KVM (attempt $attempt)"
  # Keep disk creation outside the ||-guarded function so set -e aborts immediately if it fails.
  rm -f "$WORK_DIR/install.qcow2"
  qemu-img create -q -f qcow2 "$WORK_DIR/install.qcow2" 20G
  t0=$(date +%s)
  qemu_rc=0
  install_windows || qemu_rc=$?
  echo "QEMU exited with code $qemu_rc after $(( $(date +%s) - t0 ))s"
  if [[ $qemu_rc -eq 0 ]] && grep -q PROVISIONING_SUCCEEDED "$SERIAL_LOG"; then
    break
  fi
  if [[ $attempt -eq 1 ]] && { [[ -f "$WORK_DIR/install-stalled" ]] || [[ $qemu_rc -eq 124 ]]; }; then
    echo "WARNING: installer stalled or timed out; retrying with a fresh disk." >&2
    rm -rf "$WORK_DIR/diag-attempt1"
    mv "$DIAG_DIR" "$WORK_DIR/diag-attempt1"
    mkdir -p "$DIAG_DIR"
    continue
  fi
  echo "ERROR: install/provisioning failed (QEMU exit code $qemu_rc, no PROVISIONING_SUCCEEDED marker)." >&2
  exit 1
done

log "Compacting image"
rm -f "$WORK_DIR/windows.iso" "$WORK_DIR/seed.iso"
# Drops the TRIMmed free space and the pagefile cleared at shutdown. No extra compression here:
# actions/cache already compresses with 'zstd --long=30' and decompresses inline on restore.
qemu-img convert -O qcow2 "$WORK_DIR/install.qcow2" "$OUT_DIR/base.qcow2"
rm -f "$WORK_DIR/install.qcow2"
ls -lh "$OUT_DIR/base.qcow2"
