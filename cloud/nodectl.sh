#!/usr/bin/env bash
# nodectl.sh — start, watch and stop the long-lived processes on a node.
#
# This file exists because of one property of ssh. A session does not close
# while a process it started still holds the channel, so `nohup … &` issued over
# `gcloud compute ssh` returns to the driver only once the process it started
# exits — which, for a daemon, is never. The driver then waits for ever on a
# command that has already done its work. That is the first failure this study
# stopped at: the word `started` appeared, and nothing followed it.
#
# The repair is not a better redirection. Redirections move descriptors around
# inside a process tree that still belongs to the session; what is needed is for
# the process not to belong to the session at all. `systemd-run` creates a
# transient unit owned by init, whose lifetime has nothing to do with the ssh
# session that asked for it, and the asking command returns at once. Where
# systemd is not reachable the fallback detaches with `setsid` inside a subshell
# whose own three descriptors are replaced first — the same idea with fewer
# guarantees, and still enough that nothing is left holding the channel.
#
# The second failure was subtler and is why spawn() below is written the way it
# is. `systemd-run` returns as soon as the transient unit has been accepted and
# its start job queued, not when ExecStart has run. Its exit status therefore
# says the unit was well-formed; it says nothing about whether the process
# exists. Anything that reads a zero there as `running` will wait for a daemon
# that failed to start, and with `--collect` the unit is garbage-collected
# before anybody can ask it why. So the mechanism is never trusted: after every
# spawn the process itself is looked for, and if it is not there the next
# mechanism is tried and systemd's own account of the failure is copied into the
# log first, so that one file holds the whole story however it was started.
#
# The third reason for this file is the length of a sweep. An hour of
# measurement behind one ssh connection is an hour in which a dropped tunnel
# destroys the run. Here the sweep is detached the moment it starts and writes
# to a log on the node; the driver reads that log in short polls and can lose
# the connection between any two of them without the measurement noticing.
#
# Every subcommand prints one line whose first word is machine-readable, and
# none of them blocks: whatever calls this is holding an ssh session open while
# it waits.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# An unguarded cd here is a quiet one: `cd ""` succeeds and changes nothing, so a
# ROOT that failed to resolve leaves every relative path below pointing at
# whatever directory the ssh session happened to start in, and the first symptom
# is a missing binary rather than a missing tree.
[ -n "$ROOT" ] && cd "$ROOT" || {
  echo "cannot find the repository root from $0" >&2; exit 1; }

GEN_UNIT="linerate-loadgend"
IPERF_UNIT="linerate-iperf3"
SWEEP_UNIT="linerate-sweep"

GEN_LOG="/tmp/linerate-loadgend.log"
IPERF_LOG="/tmp/linerate-iperf3.log"
SWEEP_LOG="$ROOT/results/sweep.log"
SWEEP_RC="$ROOT/results/sweep.rc"
WRAPPER="$ROOT/results/sweep-wrapper.sh"

# How long a process is given to appear after it has been spawned, in seconds.
# A daemon that binds two sockets and enters an accept loop needs a fraction of
# this; the margin is for a node still finishing its first boot.
SPAWN_WAIT="${LR_SPAWN_WAIT:-12}"

usage() {
  cat <<'EOF'
nodectl.sh — the processes on this node that outlive the ssh session

  bash cloud/nodectl.sh start-generator [--port=9099] [--echo-port=9098]
  bash cloud/nodectl.sh generator-status
  bash cloud/nodectl.sh stop-generator
  bash cloud/nodectl.sh diagnose-generator
  bash cloud/nodectl.sh start-iperf3 | iperf3-status | stop-iperf3
  bash cloud/nodectl.sh diagnose-iperf3
  bash cloud/nodectl.sh start-sweep --quick|--full [--peer=IP] [--local=IP]
  bash cloud/nodectl.sh sweep-poll [FROM_LINE]
  bash cloud/nodectl.sh stop-sweep
  bash cloud/nodectl.sh diagnose-sweep
  bash cloud/nodectl.sh netem-clear
  bash cloud/nodectl.sh nuke
EOF
}

# --------------------------------------------------------------------------
# Detaching
# --------------------------------------------------------------------------

# systemd is used when it is actually running and when sudo will not stop to ask
# for a password. Both hold on the instances this study creates; neither is
# assumed, because a cheap check and a working fallback are together less
# trouble than a run that fails on a host nobody predicted.
have_systemd() {
  [ -d /run/systemd/system ] || return 1
  command -v systemd-run >/dev/null 2>&1 || return 1
  command -v systemctl   >/dev/null 2>&1 || return 1
  sudo -n true >/dev/null 2>&1 || return 1
  return 0
}

# Anything started through systemd-run runs as root, so anything that stops it
# has to be able to signal a root process. Falling back to an unprivileged kill
# keeps this working on a host where sudo is not available and the fallback
# spawn path was used.
as_root() {
  if sudo -n true >/dev/null 2>&1; then sudo -n "$@"; else "$@"; fi
}

unit_active() { systemctl is-active --quiet "$1" 2>/dev/null; }

unit_stop() {
  if have_systemd; then
    sudo -n systemctl stop "$1" >/dev/null 2>&1 || true
    # Without this a unit left in the failed state keeps its name, and the next
    # systemd-run for the same name is refused. That refusal used to arrive as
    # a start that reported nothing and produced nothing.
    sudo -n systemctl reset-failed "$1" >/dev/null 2>&1 || true
  fi
}

kill_exact() { as_root pkill -x "$1" >/dev/null 2>&1 || true; }
kill_match() { as_root pkill -f "$1" >/dev/null 2>&1 || true; }

# A process that has exited but has not been reaped still answers to pgrep. On a
# host whose init reaps promptly that window is too short to see; on one that
# does not, a stop that worked reports itself as a failure and the driver stops
# a run that was fine. The state is therefore read rather than the existence of
# the entry.
first_alive() {   # pgrep arguments
  local p st
  for p in $(pgrep "$@" 2>/dev/null); do
    st="$(ps -o stat= -p "$p" 2>/dev/null | head -1)"
    case "$st" in Z*|"") continue ;; esac
    echo "$p"
    return 0
  done
  return 1
}

# wait_alive SECONDS pgrep-arguments...
#
# The interesting failure is a process that starts and dies a second later, and
# the only way to tell it from one that is still starting is to look more than
# once.
wait_alive() {
  local limit="$1"; shift
  local i=0
  while : ; do
    first_alive "$@" >/dev/null && return 0
    i=$((i + 1))
    [ "$i" -ge "$limit" ] && return 1
    sleep 1
  done
}

# A file both root and the login user can append to. The two spawn mechanisms
# run as different users, and a log left owned by root from a previous attempt
# is a log the fallback cannot write — which turns a legible failure into an
# empty file, which is exactly what this whole file is trying to prevent.
reset_log() {   # path
  as_root rm -f "$1" >/dev/null 2>&1 || true
  rm -f "$1" >/dev/null 2>&1 || true
  : > "$1" 2>/dev/null || true
  chmod 0666 "$1" 2>/dev/null || true
}

# write_launcher SCRIPT LOG COMMAND...
#
# The command is written to a script rather than passed as a command line. That
# buys three things. The redirection is then identical whichever mechanism
# starts it, so a failure cannot be an artefact of how it was started. It needs
# no `StandardOutput=append:` property, so it does not need a systemd new enough
# to have one. And an operator debugging a node can run the launcher by hand and
# watch the daemon fail in front of them, which is the fastest way to the answer
# and was not previously possible.
write_launcher() {
  local script="$1" log="$2"; shift 2
  {
    printf '#!/usr/bin/env bash\n'
    printf '# Written by cloud/nodectl.sh. Safe to run by hand; safe to delete.\n'
    printf 'exec >>%q 2>&1\n' "$log"
    printf 'cd %q || { echo "cannot enter %q"; exit 1; }\n' "$ROOT" "$ROOT"
    printf 'printf -- "=== %%s starting\\n" "$(date -u +%%Y-%%m-%%dT%%H:%%M:%%SZ)"\n'
    printf 'exec'
    printf ' %q' "$@"
    printf '\n'
  } > "$script" 2>/dev/null || return 1
  chmod 0755 "$script" 2>/dev/null || return 1
  return 0
}

# spawn UNIT LOG PROBE COMMAND...
#
# PROBE is the pgrep expression that says the thing is up — "-x lr_loadgend" for
# a daemon whose process name is its own, "-f sweep-wrapper.sh" for a script run
# by an interpreter whose name says nothing. It is deliberately word-split at
# every use. Prints how the process was detached, or `none`, and returns
# non-zero when nothing came up. Both mechanisms are verified rather than
# trusted; see the note at the top of this file for why the systemd one in
# particular cannot be.
spawn() {
  local unit="$1" log="$2" probe="$3"; shift 3
  local script="/tmp/$unit-launch.sh"

  reset_log "$log"
  as_root rm -f "$script" >/dev/null 2>&1 || true
  rm -f "$script" >/dev/null 2>&1 || true
  if ! write_launcher "$script" "$log" "$@"; then
    echo "none"
    return 1
  fi

  unit_stop "$unit"

  if have_systemd; then
    # No --collect: a unit that fails is worth more than a unit that is tidy.
    # unit_stop above has already cleared any corpse of the same name.
    # The redirect belongs to this shell, not to sudo, and that is deliberate:
    # what is being captured is systemd-run's own refusal, and the log is a file
    # the login user owns. The unit's output goes to the journal.
    # shellcheck disable=SC2086,SC2024
    if sudo -n systemd-run --unit="$unit" /bin/bash "$script" >>"$log" 2>&1; then
      if wait_alive "$SPAWN_WAIT" $probe; then
        echo "systemd"
        return 0
      fi
      {
        echo "--- systemd accepted the unit and nothing matching '$probe' came up"
        sudo -n systemctl status "$unit" --no-pager -l 2>&1 | sed 's/^/    /'
        sudo -n journalctl -u "$unit" --no-pager -n 40 2>&1 | sed 's/^/    /'
        echo "--- falling back to setsid"
      } >>"$log" 2>&1 || true
      unit_stop "$unit"
    else
      echo "--- systemd-run refused the unit; falling back to setsid" >>"$log" 2>&1 || true
    fi
  fi

  # No systemd, or systemd could not run it: detach by hand. The subshell's own
  # descriptors are replaced before it forks, so neither it nor its child holds
  # anything belonging to the ssh session that is running this script.
  ( setsid /bin/bash "$script" </dev/null >/dev/null 2>&1 & ) </dev/null >/dev/null 2>&1
  # shellcheck disable=SC2086
  if wait_alive "$SPAWN_WAIT" $probe; then
    echo "setsid"
    return 0
  fi
  echo "none"
  return 1
}

# diagnose UNIT LOG PROBE PORTS
#
# Everything an operator would go and look at by hand, in one round trip. It is
# one round trip on purpose: by the time this is wanted the driver is often
# about to delete the instance, and an instruction to go and read a file on a
# machine that will not exist in thirty seconds is not a diagnosis.
diagnose() {
  local unit="$1" log="$2" probe="$3" ports="${4:-}"
  echo "--- process"
  # shellcheck disable=SC2086
  if first_alive $probe >/dev/null; then
    # shellcheck disable=SC2086
    pgrep -a $probe 2>/dev/null | sed 's/^/    /'
  else
    echo "    nothing matching 'pgrep $probe' is running"
  fi
  echo "--- $log"
  if [ -s "$log" ]; then
    as_root tail -n 60 "$log" 2>/dev/null | sed 's/^/    /'
  else
    echo "    empty: nothing was written, so nothing ran long enough to write"
  fi
  if have_systemd; then
    echo "--- systemctl status $unit"
    sudo -n systemctl status "$unit" --no-pager -l 2>&1 | head -20 | sed 's/^/    /'
    echo "--- journalctl -u $unit"
    sudo -n journalctl -u "$unit" --no-pager -n 20 2>&1 | sed 's/^/    /'
  else
    echo "--- systemd is not available on this node; the setsid path was used"
  fi
  if [ -n "$ports" ]; then
    echo "--- listeners on $ports"
    if command -v ss >/dev/null 2>&1; then
      as_root ss -lntup 2>/dev/null | grep -E "$ports" | sed 's/^/    /' || echo "    none"
    else
      echo "    ss is not installed"
    fi
  fi
  echo "--- launcher"
  if [ -f "/tmp/$unit-launch.sh" ]; then
    sed 's/^/    /' "/tmp/$unit-launch.sh"
  else
    echo "    /tmp/$unit-launch.sh was never written"
  fi
  return 0
}

# --------------------------------------------------------------------------
# The load generator
# --------------------------------------------------------------------------

PORT=9099
ECHO_PORT=9098
for a in "$@"; do
  case "$a" in
    --port=*)      PORT="${a#*=}" ;;
    --echo-port=*) ECHO_PORT="${a#*=}" ;;
  esac
done

generator_status() {
  local how="${1:-}" pid
  pid="$(first_alive -x lr_loadgend || true)"
  if [ -n "$pid" ]; then
    echo "running pid=$pid port=$PORT via=${how:-already}"
    return 0
  fi
  echo "stopped ${how:+via=$how }log=$GEN_LOG"
  return 1
}

start_generator() {
  if [ ! -x "$ROOT/build/lr_loadgend" ]; then
    echo "stopped reason=missing-binary $ROOT/build/lr_loadgend; setup.sh has not run here"
    return 1
  fi
  stop_generator >/dev/null 2>&1 || true
  local how
  how="$(spawn "$GEN_UNIT" "$GEN_LOG" "-x lr_loadgend" "$ROOT/build/lr_loadgend" \
           "--port=$PORT" "--echo-port=$ECHO_PORT")"
  generator_status "$how"
}

# A daemon that ignores SIGTERM would be killed and reported as still running,
# so the escalation is explicit: ask, wait, insist, and only then complain.
stop_generator() {
  unit_stop "$GEN_UNIT"
  kill_exact lr_loadgend
  local i=0
  while [ "$i" -lt 5 ]; do
    first_alive -x lr_loadgend >/dev/null || { echo "stopped"; return 0; }
    sleep 1
    i=$((i + 1))
  done
  as_root pkill -KILL -x lr_loadgend >/dev/null 2>&1 || true
  sleep 1
  if first_alive -x lr_loadgend >/dev/null; then echo "still-running"; return 1; fi
  echo "stopped"
  return 0
}

# --------------------------------------------------------------------------
# The iperf3 server
#
# E6 measures the same link with no cryptography at all, which needs a server on
# the far node. Without one that reference records itself unavailable and the
# report prints an em dash where the upper bound on the link should be.
# --------------------------------------------------------------------------

start_iperf3() {
  command -v iperf3 >/dev/null 2>&1 || { echo "stopped reason=absent iperf3 is not installed"; return 1; }
  stop_iperf3 >/dev/null 2>&1 || true
  local how
  how="$(spawn "$IPERF_UNIT" "$IPERF_LOG" "-x iperf3" "$(command -v iperf3)" -s)"
  iperf3_status "$how"
}

iperf3_status() {
  local how="${1:-}" pid
  pid="$(first_alive -x iperf3 || true)"
  if [ -n "$pid" ]; then
    echo "running pid=$pid port=5201 via=${how:-already}"
    return 0
  fi
  echo "stopped ${how:+via=$how }log=$IPERF_LOG"
  return 1
}

stop_iperf3() {
  unit_stop "$IPERF_UNIT"
  kill_exact iperf3
  sleep 1
  as_root pkill -KILL -x iperf3 >/dev/null 2>&1 || true
  echo "stopped"
  return 0
}

# --------------------------------------------------------------------------
# The sweep
# --------------------------------------------------------------------------

# The sweep is wrapped rather than run directly, so that three things happen
# whatever it does: its exit status lands where the driver can read it, the
# dataset ends up owned by the login user even though the sweep needed root for
# the tiers, and the log ends with a line that says the run is over instead of
# simply stopping.
write_wrapper() {   # mode peer local
  local mode="$1" peer="$2" local_addr="$3" uid gid
  uid="$(id -u)"; gid="$(id -g)"
  mkdir -p "$ROOT/results"
  cat > "$WRAPPER" <<WRAP
#!/usr/bin/env bash
cd "$ROOT"
rc=0
bash run.sh $mode $peer $local_addr || rc=\$?
chown -R $uid:$gid "$ROOT/results" 2>/dev/null || true
echo "\$rc" > "$SWEEP_RC"
chown $uid:$gid "$SWEEP_RC" 2>/dev/null || true
echo "==> sweep finished with status \$rc"
exit \$rc
WRAP
  chmod 0755 "$WRAPPER"
}

sweep_running() {
  if have_systemd && unit_active "$SWEEP_UNIT"; then return 0; fi
  first_alive -f "sweep-wrapper.sh" >/dev/null && return 0
  return 1
}

start_sweep() {
  local mode="--quick" peer="" local_addr=""
  for a in "$@"; do
    case "$a" in
      --quick)   mode="--quick" ;;
      --full)    mode="--full" ;;
      --peer=*)  peer="$a" ;;
      --local=*) local_addr="$a" ;;
    esac
  done
  if [ ! -x "$ROOT/build/lr_bench" ]; then
    echo "failed reason=missing-binary $ROOT/build/lr_bench; setup.sh has not run here"
    return 1
  fi
  if sweep_running; then echo "already-running"; return 0; fi

  # One sweep, one dataset. Fragments left by a previous run in the same
  # directory would be merged with these, and a quick smoke run folded into a
  # full sweep is a dataset whose provenance nobody can reconstruct afterwards.
  as_root rm -rf "$ROOT/results" >/dev/null 2>&1 || true
  rm -rf "$ROOT/results" >/dev/null 2>&1 || true
  mkdir -p "$ROOT/results/raw"

  write_wrapper "$mode" "$peer" "$local_addr"
  : > "$SWEEP_LOG"
  chmod 0666 "$SWEEP_LOG" 2>/dev/null || true
  local how
  # The probe is the wrapper's path, not an interpreter's name: `pgrep -x bash`
  # would match this very script's shell.
  how="$(spawn "$SWEEP_UNIT" "$SWEEP_LOG" "-f sweep-wrapper.sh" /bin/bash "$WRAPPER")"
  if [ "$how" = "none" ]; then
    echo "failed reason=would-not-start mode=$mode log=$SWEEP_LOG"
    return 1
  fi
  echo "started via=$how mode=$mode log=$SWEEP_LOG"
  return 0
}

# sweep-poll FROM
#
# The first line is the status; everything after it is the log from line FROM
# on. One round trip rather than two, because each one costs an ssh connection
# and the driver makes this call every few seconds for the length of a sweep.
sweep_poll() {
  local from="${1:-1}" rc="" state="running"
  case "$from" in ''|*[!0-9]*) from=1 ;; esac
  [ -f "$SWEEP_RC" ] && rc="$(tr -d '[:space:]' < "$SWEEP_RC" 2>/dev/null)"
  if [ -n "$rc" ]; then
    state="finished rc=$rc"
  elif ! sweep_running; then
    # No status file and no process. The sweep died without being able to say
    # so, which needs a different response from the operator than a non-zero
    # exit and is therefore reported as its own state.
    state="vanished"
  fi
  # The line count travels with the status so the driver can advance its cursor
  # by what the log holds rather than by what it managed to print. Trailing
  # blank lines do not survive a shell's command substitution, and a cursor
  # advanced by the printed count would send them again on every poll.
  local total=0
  [ -f "$SWEEP_LOG" ] && total="$(wc -l < "$SWEEP_LOG" 2>/dev/null | tr -d ' ')"
  echo "STATUS $state lines=$total"
  [ -f "$SWEEP_LOG" ] && tail -n "+$from" "$SWEEP_LOG" 2>/dev/null
  return 0
}

stop_sweep() {
  unit_stop "$SWEEP_UNIT"
  kill_match "sweep-wrapper.sh"
  kill_exact lr_bench
  echo "stopped"
  return 0
}

# A network condition outlives the process that set it. A sweep that dies
# between `netem.sh delay 10` and `netem.sh clear` leaves every later
# measurement on this host — and every ssh connection to it — behind that
# condition, so the driver clears it whatever the sweep did.
netem_clear() {
  if [ -f "$ROOT/cloud/netem.sh" ] && sudo -n true >/dev/null 2>&1; then
    sudo -n bash "$ROOT/cloud/netem.sh" clear >/dev/null 2>&1 || true
  fi
  echo "cleared"
  return 0
}

# --------------------------------------------------------------------------

case "${1:-}" in
  start-generator)    shift; start_generator "$@" ;;
  generator-status)   generator_status ;;
  stop-generator)     stop_generator ;;
  diagnose-generator) diagnose "$GEN_UNIT" "$GEN_LOG" "-x lr_loadgend" ":$PORT|:$ECHO_PORT" ;;
  start-iperf3)       start_iperf3 ;;
  iperf3-status)      iperf3_status ;;
  stop-iperf3)        stop_iperf3 ;;
  diagnose-iperf3)    diagnose "$IPERF_UNIT" "$IPERF_LOG" "-x iperf3" ":5201" ;;
  start-sweep)        shift; start_sweep "$@" ;;
  sweep-poll)         shift; sweep_poll "${1:-1}" ;;
  stop-sweep)         stop_sweep ;;
  diagnose-sweep)     diagnose "$SWEEP_UNIT" "$SWEEP_LOG" "-f sweep-wrapper.sh" "" ;;
  netem-clear)        netem_clear ;;
  nuke)               stop_sweep >/dev/null; stop_generator >/dev/null
                      stop_iperf3 >/dev/null; netem_clear >/dev/null
                      echo "stopped" ;;
  *)                  usage; exit 64 ;;
esac
