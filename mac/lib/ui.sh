# ui.sh — output, prompting, and the two things a driver needs to survive a
# cloud: a clock on every remote call, and a confirmation gate on anything
# billable or destructive.
#
# Everything the operator sees goes through here so that a run reads the same
# whichever subcommand produced it, and so that no destructive or billable
# action can happen without a visible prompt.

set -euo pipefail

# The absolute path of the driver. Every command these files print for the
# operator to run is printed with it, because the relative form is correct only
# from inside the repository and the messages that matter are the ones printed
# after something has failed — at which point an instruction that depends on the
# working directory adds 'no such file or directory' to the failure it was meant
# to repair. Defined here as well as in the driver so that a library is never
# left depending on whoever sourced it.
: "${SELF:=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." 2>/dev/null && pwd)/linerate}"

if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
  C_DIM=$'\033[2m'; C_BOLD=$'\033[1m'; C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'; C_YELLOW=$'\033[33m'; C_OFF=$'\033[0m'
else
  C_DIM=''; C_BOLD=''; C_RED=''; C_GREEN=''; C_YELLOW=''; C_OFF=''
fi

say()  { printf '%s\n' "$*"; }
step() { printf '\n%s==>%s %s%s%s\n' "$C_BOLD" "$C_OFF" "$C_BOLD" "$*" "$C_OFF"; }
info() { printf '    %s\n' "$*"; }
dim()  { printf '%s    %s%s\n' "$C_DIM" "$*" "$C_OFF"; }
ok()   { printf '    %s✓%s %s\n' "$C_GREEN" "$C_OFF" "$*"; }
warn() { printf '    %s!%s %s\n' "$C_YELLOW" "$C_OFF" "$*"; }
die()  { printf '\n%serror:%s %s\n' "$C_RED" "$C_OFF" "$*" >&2; exit 1; }

# The same, preceded by what the cloud actually said. An operator who has to
# open a log file to find out what happened has already lost the thing the log
# was for — and the driver knows the message, because it captured it in order to
# decide that this was a failure at all.
die_detail() {
  local detail="$1" body; shift
  # The `|| true` is not decoration. Under `set -o pipefail` a detail made
  # entirely of blank lines gives grep nothing to print and a status of one,
  # which under `set -e` would end the shell here — swallowing the very message
  # this function exists to deliver.
  body="$(printf '%s\n' "$detail" | grep -v '^[[:space:]]*$' | tail -8 || true)"
  if [ -n "$body" ]; then
    printf '\n%swhat Google Cloud said:%s\n' "$C_DIM" "$C_OFF" >&2
    printf '%s\n' "$body" | sed 's/^/    /' >&2
  fi
  die "$@"
}

# A spinner would hide the tool's own output, which is exactly what an operator
# needs when a cloud call fails. Long steps announce themselves instead.
running() { printf '    %s… %s%s\n' "$C_DIM" "$*" "$C_OFF"; }

# Ask before anything that costs money or destroys state. LR_YES=1 skips the
# prompt: that is what `go` sets after asking once for the whole run, and what
# scripting uses. Nothing else does.
confirm() {
  local prompt="$1"
  if [ "${LR_YES:-0}" = "1" ]; then
    info "$prompt — proceeding (LR_YES is set)"
    return 0
  fi
  # Without a terminal there is nobody to ask, and the answer to a question
  # nobody can hear is no. Saying which question it was, and how to answer it in
  # advance, is the difference between that and a run that cancels itself for no
  # stated reason under nohup, in a pipeline, or from an editor's task runner.
  if [ ! -c /dev/tty ] || ! : </dev/tty 2>/dev/null; then
    die "this needs an answer and there is no terminal to ask at:
      $prompt
    Run it from a terminal, or answer every such question in advance with:
      LR_YES=1 bash $SELF ..."
  fi
  printf '    %s [y/N] ' "$prompt"
  local answer=""
  read -r answer </dev/tty || true
  case "$answer" in
    y|Y|yes|YES) return 0 ;;
    *) die "cancelled" ;;
  esac
}

# The same question, for a step the run can continue without. Declining returns
# non-zero rather than ending the run, so the caller can say what carrying on
# without it costs.
confirm_soft() {
  local prompt="$1"
  if [ "${LR_YES:-0}" = "1" ]; then
    info "$prompt — proceeding (LR_YES is set)"
    return 0
  fi
  printf '    %s [y/N] ' "$prompt"
  local answer=""
  read -r answer </dev/tty || true
  case "$answer" in
    y|Y|yes|YES) return 0 ;;
    *) return 1 ;;
  esac
}

# Default yes, for the question asked after something has already gone wrong,
# where the safe answer and the cheap answer are the same one.
confirm_default_yes() {
  local prompt="$1"
  [ -t 0 ] || return 0
  printf '    %s [Y/n] ' "$prompt"
  local answer=""
  read -r answer </dev/tty || true
  case "$answer" in
    n|N|no|NO) return 1 ;;
    *) return 0 ;;
  esac
}

need() {
  command -v "$1" >/dev/null 2>&1 || die "$1 is not installed. $2"
}

# Run a command under a wall-clock limit, and return 124 when the limit is what
# ended it.
#
# macOS has no timeout(1), and every remote call in this driver is one that can
# hang rather than fail: an ssh session whose channel something still holds, an
# IAP tunnel that stops moving bytes, an API call waiting on a lock. A driver
# without a clock on those calls is one an operator has to sit and watch in
# order to notice that nothing is happening — which is the state this study was
# in when it stopped after printing the word 'started'.
#
# The poll starts fine and coarsens. A whole second of waiting charged to every
# call that returns promptly is forty seconds spent doing nothing across a run
# that makes forty of them; a call still going after five seconds, on the other
# hand, will not finish within a millisecond of any particular poll, so there is
# nothing to be gained by asking that often. BSD sleep takes a fraction and GNU
# sleep takes a fraction, but a shell built around neither may not, so the
# fraction is established by trying it once rather than assumed.
if sleep 0.2 >/dev/null 2>&1; then LR_NAP="0.2"; else LR_NAP="1"; fi

with_timeout() {
  local limit="$1"; shift
  "$@" &
  local pid=$! rc=0 polls=0 nap="$LR_NAP" deadline
  deadline=$(( $(date +%s) + limit ))
  while kill -0 "$pid" 2>/dev/null; do
    if [ "$(date +%s)" -ge "$deadline" ]; then
      kill -TERM "$pid" 2>/dev/null || true
      sleep 2
      kill -KILL "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
      return 124
    fi
    sleep "$nap"
    polls=$((polls + 1))
    [ "$polls" -lt 25 ] || nap="1"
  done
  wait "$pid" || rc=$?
  return "$rc"
}

# Seconds, rendered the way an operator reads them.
elapsed() {
  local s="$1"
  if [ "$s" -lt 60 ]; then printf '%ds' "$s"
  else printf '%dm%02ds' "$((s / 60))" "$((s % 60))"; fi
}
