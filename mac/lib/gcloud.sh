# gcloud.sh — everything that talks to Google Cloud.
#
# No account, project name or billing account is written anywhere in this
# repository. All of them come from the operator's own environment, from a flag,
# or from a prompt, and are stored in the state file described in vm.sh, which
# lives outside the repository. That is what makes this reproducible by someone
# other than its author: they run it with their own names and nothing of the
# author's is baked in. The project ID the runbook suggests is a suggestion;
# `up` takes any name and picks another itself if the one asked for is held by
# somebody else.
#
# Every call that creates a billable resource, and every call that deletes
# something, goes through confirm().
#
# Everything else goes through gc(), for the reason written there. That wrapper
# is the whole difference between a driver that reports a problem and a driver
# that appears to have died.

LR_SSH_TIMEOUT="${LR_SSH_TIMEOUT:-600}"
LR_SCP_TIMEOUT="${LR_SCP_TIMEOUT:-900}"
LR_API_TIMEOUT="${LR_API_TIMEOUT:-600}"
LR_LIST_TIMEOUT="${LR_LIST_TIMEOUT:-90}"

# gc LIMIT ARGUMENTS...
#
# Two properties of this CLI will hang an unattended driver rather than fail it.
#
# It asks questions. The most common one is whether to enable an API on a
# project that does not have it — "API [compute.googleapis.com] not enabled on
# project [x]. Would you like to enable and retry?" — and any account that has
# ever held a project without Compute Engine will meet it while the instances on
# that account are being counted. --quiet makes gcloud take the default answer
# instead of asking, which for that question is no, and the command then fails
# with a status the caller can act on.
#
# It asks them on standard error. A caller that discards standard error, as a
# loop over projects naturally does, discards the question as well: the terminal
# shows nothing at all while the process waits on a keystroke that no one knows
# to type. Reading /dev/null closes that door from the other side, so a question
# nobody suppressed reaches end of file instead of waiting.
#
# And a clock on every call, so that a request which neither answers nor fails
# ends as a message rather than as a cursor.
gc() {
  local limit="$1"; shift
  with_timeout "$limit" gcloud "$@" --quiet </dev/null
}

gcloud_check() {
  need gcloud "Install the Google Cloud CLI: https://cloud.google.com/sdk/docs/install"
  if ! gcloud auth list --filter=status:ACTIVE --format='value(account)' 2>/dev/null | grep -q .; then
    die "no active gcloud account. Run: gcloud auth login"
  fi
}

gcloud_account() {
  gcloud auth list --filter=status:ACTIVE --format='value(account)' 2>/dev/null | head -1
}

# ---------------------------------------------------------------------------
# Projects
# ---------------------------------------------------------------------------

# Three answers, and deliberately not four.
#
# Resource Manager refuses to say whether a project ID it will not show you is
# one that belongs to somebody else or one that has never existed. Both come
# back as
#
#     User [...] does not have permission to access projects instance [x]
#     (or it may not exist).
#
# and the parenthesis is the whole of the difference. That is not an accident:
# answering otherwise would turn `describe` into a way of enumerating which IDs
# in a global namespace are taken. An earlier version of this read the words
# 'does not have permission' as evidence of an owner, and so reported a project
# ID containing a Unix timestamp as belonging to a stranger, four times in a
# row, each time inviting the operator to invent another name that would be
# refused for the same reason.
#
# So this does not guess. 'unknown' means unknown, and the only thing that
# settles it is trying to create the project: `projects create` distinguishes an
# ID that is taken from a quota that is exhausted, in as many words, because it
# is allowed to.
gcloud_project_probe() {
  local project="$1" out rc=0 i=1
  while [ "$i" -le 2 ]; do
    rc=0
    out="$(gc 60 projects describe "$project" \
             --format='value(lifecycleState,state)' 2>&1)" || rc=$?
    if [ "$rc" = "0" ]; then
      case "$out" in
        *DELETE_REQUESTED*) printf 'deleted\n' ;;
        *)                  printf 'active\n' ;;
      esac
      return 0
    fi
    i=$((i + 1))
    [ "$i" -le 2 ] || break
    sleep 3
  done
  printf '%s\n' "$out" >>"$LR_LOG" 2>/dev/null || true
  printf 'unknown\n'
}

gcloud_project_exists() {
  [ "$(gcloud_project_probe "$1")" = "active" ]
}

# Project IDs are six to thirty characters, lower case, starting with a letter
# and not ending with a hyphen. A suffixed candidate has to obey that too, so
# the base is truncated to leave room for the suffix rather than the whole thing
# being rejected for length after the operator has waited for the call.
gcloud_project_candidate() {
  local base="$1" suffix="$2" room
  base="$(printf '%s' "$base" | tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9-' '-')"
  case "$base" in [a-z]*) ;; *) base="lr-$base" ;; esac
  room=$((30 - ${#suffix} - 1))
  [ "${#base}" -le "$room" ] || base="$(printf '%s' "$base" | cut -c1-"$room")"
  base="$(printf '%s' "$base" | sed 's/-*$//')"
  printf '%s-%s\n' "$base" "$suffix"
}

gcloud_project_id_ok() {
  case "$1" in
    [a-z]*[!-]) [ "${#1}" -ge 6 ] && [ "${#1}" -le 30 ] && \
                [ -z "$(printf '%s' "$1" | tr -d 'a-z0-9-')" ] ;;
    *) return 1 ;;
  esac
}

# The projects this account has shut down and not yet lost. Google keeps them
# for thirty days, and for the whole of that time they go on counting against
# the quota that governs how many projects may exist — so an account that has
# deleted everything has not freed anything, and is frequently an account that
# can no longer create a project at all. Restoring one is then the only way
# forward that does not involve waiting a month or asking Google for more quota.
gcloud_restorable_projects() {
  gc 120 projects list --filter='lifecycleState:DELETE_REQUESTED' \
     --format='value(projectId)' 2>>"$LR_LOG" | head -10 || true
}

# Up to five projects this account could use instead. Printed on the failure
# path, because 'choose another name' is not advice when every name has just
# been refused and the operator has no way of knowing which ones are theirs.
gcloud_usable_projects() {
  gc 120 projects list --filter='lifecycleState:ACTIVE' \
     --format='value(projectId)' 2>>"$LR_LOG" | head -5 || true
}

# LR_PROJECT_CHOSEN is what the caller reads back, because it is not necessarily
# what the caller asked for.
LR_PROJECT_CHOSEN=""

# Creation only. Billing, APIs, network and firewall are four separate steps
# below, each of which can fail on its own and each of which says so. They used
# to be folded in here, which is how a project with no billing account could be
# reported as ready.
gcloud_create_project() {
  local project="$1" display="${2:-$1}"
  LR_PROJECT_CHOSEN="$project"

  gcloud_project_id_ok "$project" || die "'$project' is not a usable project ID.
    Six to thirty characters, lower case, starting with a letter, made of
    letters, digits and hyphens, and not ending in one. For example:
      bash $SELF go --project linerate-$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')"

  local candidate="$project"
  case "$(gcloud_project_probe "$project")" in
    active)
      ok "project $project exists and is visible to this account"
      return 0 ;;
    deleted)
      # Pending deletion, so nothing can be created in it. Restoring it is tried
      # before a new ID is invented, and in that order deliberately: the project
      # is counted against the quota whether it is restored or not, so restoring
      # costs nothing that has not already been spent, while creating another
      # spends a slot that an account in this state may no longer have.
      warn "project $project is pending deletion"
      info "It counts against this account's project quota either way, so"
      info "restoring it is cheaper than creating another one."
      running "restoring $project"
      if gc "$LR_API_TIMEOUT" projects undelete "$project" >/dev/null 2>>"$LR_LOG"; then
        local i=1
        while [ "$i" -le 20 ]; do
          if [ "$(gcloud_project_probe "$project")" = "active" ]; then
            ok "restored $project"
            warn "shutting a project down disconnects its billing account, and"
            warn "restoring it does not reconnect it. That is done below."
            LR_PROJECT_CHOSEN="$project"
            return 0
          fi
          sleep 6
          i=$((i + 1))
        done
        warn "$project did not come back within two minutes; using a new ID instead"
      else
        warn "could not restore $project; using a new ID instead"
      fi
      candidate="$(gcloud_project_candidate "$project" \
                     "$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')")"
      warn "the ID for this run will be $candidate" ;;
  esac

  confirm "Create Google Cloud project '$candidate' (display name: $display)?"
  local attempt=0 out rc
  while : ; do
    running "creating project $candidate"
    rc=0
    out="$(gc "$LR_API_TIMEOUT" projects create "$candidate" --name="$display" 2>&1)" || rc=$?
    printf '%s\n' "$out" >>"$LR_LOG" 2>/dev/null || true
    if [ "$rc" = "0" ]; then
      LR_PROJECT_CHOSEN="$candidate"
      ok "created project $candidate"
      # A project is not immediately addressable by everything that will be
      # asked of it in the next few seconds.
      sleep 5
      return 0
    fi
    attempt=$((attempt + 1))
    # Only one kind of failure is worth another name. A quota, a permission or
    # an organisation policy says the same thing about every ID there is, and
    # retrying it three times only delays the report and invents two more names
    # that were never the problem.
    case "$out" in
      *"already in use"*|*ALREADY_EXISTS*|*"already exists"*)
        [ "$attempt" -lt 4 ] || die_detail "$out" \
          "four project IDs in a row were already taken, which is improbable
    enough to be a symptom rather than a coincidence. The last message is
    above. Choose an ID of your own:
      bash $SELF go --project linerate-$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n')"
        candidate="$(gcloud_project_candidate "$project" \
                       "$(od -An -N3 -tx1 /dev/urandom | tr -d ' \n')")"
        warn "that ID is already in use; trying $candidate"
        continue ;;
    esac
    local usable restorable
    usable="$(gcloud_usable_projects)"
    if [ -n "$usable" ]; then
      say "" >&2
      printf '    %sprojects this account can already use:%s\n' "$C_DIM" "$C_OFF" >&2
      printf '%s\n' "$usable" | sed 's/^/      /' >&2
      printf '    %sany of them:  bash %s go --project NAME%s\n' "$C_DIM" "$SELF" "$C_OFF" >&2
    fi
    restorable="$(gcloud_restorable_projects)"
    if [ -n "$restorable" ]; then
      say "" >&2
      printf '    %sprojects this account shut down and can still restore:%s\n' "$C_DIM" "$C_OFF" >&2
      printf '%s\n' "$restorable" | sed 's/^/      /' >&2
      printf '    %sthey count against the quota whether restored or not, so%s\n' "$C_DIM" "$C_OFF" >&2
      printf '    %srestoring one costs nothing that has not been spent:%s\n' "$C_DIM" "$C_OFF" >&2
      printf '    %s  bash %s go --project ONE-OF-THOSE%s\n' "$C_DIM" "$SELF" "$C_OFF" >&2
    fi
    die_detail "$out" "could not create the project $candidate.
    A project quota is the usual cause, and deleting projects does not relieve
    it: Google keeps a shut-down project for thirty days and counts it the whole
    time. Restore one of the projects listed above, or ask for more quota at
      https://console.cloud.google.com/iam-admin/quotas"
  done
}

# ---------------------------------------------------------------------------
# Billing
# ---------------------------------------------------------------------------
#
# This is the step the study kept failing at, and it failed silently. Billing
# used to be asked for only when the project did not already exist, on the
# assumption that an existing project is a paid-for one. An existing project
# left over from an earlier attempt is not: it exists, it has no billing account
# and it cannot have Compute Engine enabled, so `services enable` fails, so the
# Compute API has never heard of it, so creating a firewall rule in it answers
#
#     The resource 'projects/...' was not found
#
# which is a message about the project for a condition that has nothing to do
# with the project. Billing is now established for every project, existing or
# new, before anything is asked of Compute Engine.

gcloud_billing_enabled() {
  gc 90 billing projects describe "$1" --format='value(billingEnabled)' 2>>"$LR_LOG" \
    | tr -d '[:space:]' | grep -qi '^true$'
}

# Both halves of the only question worth asking before a run starts: is there a
# project, and will anything in it be allowed to cost money. Either answer being
# no means a billing account has to be found now.
gcloud_project_billed() {
  gcloud_project_exists "$1" && gcloud_billing_enabled "$1"
}

gcloud_open_billing_accounts() {
  gc 120 billing accounts list --filter='open=true' --format='value(name)' \
    2>>"$LR_LOG" | sed 's|billingAccounts/||' | grep . || true
}

# The billing account ID on standard output, or nothing at all. Progress and
# prompts go to standard error, so that a caller capturing the answer still sees
# what is happening.
gcloud_resolve_billing() {
  local given="${1:-}" accounts count
  if [ -n "$given" ]; then
    printf '%s\n' "${given#billingAccounts/}"
    return 0
  fi
  accounts="$(gcloud_open_billing_accounts)"
  count="$(printf '%s' "$accounts" | grep -c . || true)"
  case "$count" in
    0) return 0 ;;
    1) printf '%s\n' "$accounts"; return 0 ;;
  esac
  # `go` undertakes to ask nothing after its own confirmation, and a run that
  # stops forty minutes in to ask which account to charge breaks that for the
  # sake of a choice between accounts that all belong to the same person.
  if [ "${LR_YES:-0}" = "1" ] || [ ! -t 0 ]; then
    warn "this login holds $count open billing accounts; using the first" >&2
    printf '%s\n' "$accounts" | head -1
    return 0
  fi
  {
    say ""
    info "This login holds more than one open billing account:"
  } >&2
  gc 120 billing accounts list --filter='open=true' \
     --format='table(name.basename(),displayName,open)' >&2 || true
  printf '    Billing account ID to link: ' >&2
  local answer=""
  read -r answer </dev/tty || true
  printf '%s\n' "${answer#billingAccounts/}"
}

gcloud_ensure_billing() {
  local project="$1" billing="${2:-}"
  if gcloud_billing_enabled "$project"; then
    ok "billing is enabled on $project"
    return 0
  fi
  warn "$project has no billing account, so Compute Engine cannot be enabled on it"
  billing="$(gcloud_resolve_billing "$billing")"
  [ -n "$billing" ] || die "no open billing account is available to this login, and
    two virtual machines cannot be created without one.

    To see what you have:      gcloud billing accounts list
    To create one (the free trial is enough for this study, which costs about a
    dollar of instance time):
      https://console.cloud.google.com/billing

    Then run this again, or name the account directly:
      bash $SELF go --project $project --billing XXXXXX-XXXXXX-XXXXXX"

  confirm "Link billing account $billing to $project? The project can then incur charges."
  # Five attempts over about two minutes, because the commonest reason for a
  # refusal here is time rather than permission. Shutting a project down
  # disconnects its billing account and restoring the project does not reconnect
  # it, so a restored project arrives at this line needing exactly this call —
  # and refuses it for the first minute or so while the restore settles. A
  # single attempt reads that as a permission problem and says so, which sends
  # the operator to look at an access control page where nothing is wrong.
  local i=1 out rc linked=0
  while [ "$i" -le 5 ]; do
    running "linking billing account $billing to $project (attempt $i of 5)"
    rc=0
    out="$(gc "$LR_API_TIMEOUT" billing projects link "$project" \
             --billing-account="$billing" 2>&1)" || rc=$?
    printf '%s\n' "$out" >>"$LR_LOG" 2>/dev/null || true
    if [ "$rc" = "0" ]; then linked=1; break; fi
    [ "$i" -lt 5 ] || break
    sleep $((i * 12))
    i=$((i + 1))
  done
  if [ "$linked" != "1" ]; then
    die_detail "$out" "could not link billing account $billing to $project.
    In order of how often it is the cause:
      - you can see the billing account but are not an administrator of it, so
        you cannot attach a project to it. Ask whoever is.
      - the project was created or restored moments ago and the billing service
        has not caught up. Five attempts over two minutes have already been
        made; Google warns that a restored project can take considerably longer
        than that to become fully functional, so running the same command again
        in a quarter of an hour is a real remedy rather than a hopeful one.
      - the account is closed or has no valid payment method:
          https://console.cloud.google.com/billing/$billing
    To link it by hand instead:
      https://console.cloud.google.com/billing/linkedaccount?project=$project"
  fi

  # The link call returning zero is not the same as billing being on. Reading it
  # back costs three seconds and turns a wrong assumption into a fact.
  i=1
  while [ "$i" -le 10 ]; do
    if gcloud_billing_enabled "$project"; then
      ok "billing account $billing linked to $project"
      return 0
    fi
    sleep 3
    i=$((i + 1))
  done
  die "billing account $billing was linked to $project but the project still
    reports itself as not billed. Look at
      https://console.cloud.google.com/billing/linkedaccount?project=$project"
}

# ---------------------------------------------------------------------------
# APIs
# ---------------------------------------------------------------------------

gcloud_api_enabled() {
  gc 90 services list --enabled --project="$1" \
     --filter="config.name=$2" --format='value(config.name)' 2>>"$LR_LOG" \
    | grep -q .
}

# Enabling an API is a long-running operation behind a synchronous-looking call,
# and it is rate limited. One refusal means nothing; three mean something. The
# last word is a read rather than the status of the last write, because `enable`
# reports a failure for an operation that in fact completed often enough to be
# worth the extra call.
gcloud_enable_one_api() {
  local project="$1" api="$2" i=1
  if gcloud_api_enabled "$project" "$api"; then return 0; fi
  while [ "$i" -le 3 ]; do
    if gc "$LR_API_TIMEOUT" services enable "$api" --project="$project" \
         >/dev/null 2>>"$LR_LOG"; then
      return 0
    fi
    [ "$i" -lt 3 ] || break
    sleep $((i * 10))
    i=$((i + 1))
  done
  gcloud_api_enabled "$project" "$api"
}

# Compute Engine is not optional and is no longer treated as though it were.
# Every ssh in this driver goes through the IAP tunnel, so that one is close to
# mandatory as well; OS Login is a convenience. Only the first is fatal, and it
# is fatal here rather than four steps later.
gcloud_enable_apis() {
  local project="$1" api
  for api in compute.googleapis.com iap.googleapis.com oslogin.googleapis.com; do
    running "enabling $api"
    if gcloud_enable_one_api "$project" "$api"; then
      ok "$api enabled"
    elif [ "$api" = "compute.googleapis.com" ]; then
      die "could not enable Compute Engine on $project, so no virtual machine
    can be created there. The reason is at the end of $LR_LOG. In order of how
    often it is the cause:
      - the project is not linked to an open billing account;
      - this login is not an owner or editor of the project;
      - an organisation policy forbids the service.
    What the project says about itself:
      gcloud billing projects describe $project
      gcloud services list --enabled --project=$project"
    else
      warn "could not enable $api; carrying on"
      [ "$api" != "iap.googleapis.com" ] || \
        warn "without it the ssh tunnel may be refused later"
    fi
  done
}

# An API that reports itself enabled is not an API that will answer. Compute
# Engine takes up to a minute or two to learn about a project it has just been
# switched on for, and every call made inside that window fails with the same
# 'resource not found' message as a project that genuinely does not exist. This
# waits for a Compute call to succeed rather than letting the firewall rule be
# the thing that discovers it.
gcloud_wait_compute_ready() {
  local project="$1" i=1 limit="${LR_COMPUTE_READY_TRIES:-30}"
  if gc 60 compute networks list --project="$project" --format='value(name)' \
       >/dev/null 2>>"$LR_LOG"; then
    ok "Compute Engine answers for $project"
    return 0
  fi
  running "waiting for Compute Engine to answer for $project (up to five minutes)"
  while [ "$i" -le "$limit" ]; do
    sleep 10
    if gc 60 compute networks list --project="$project" --format='value(name)' \
         >/dev/null 2>>"$LR_LOG"; then
      ok "Compute Engine answers for $project"
      return 0
    fi
    i=$((i + 1))
  done
  die "Compute Engine reports itself enabled on $project but will not answer for
    it. This is normally a propagation delay and clears within a few minutes;
    the same command run again is the whole remedy:
      bash $SELF go --project $project
    If it persists, the reason is at the end of $LR_LOG."
}

# ---------------------------------------------------------------------------
# Network
# ---------------------------------------------------------------------------

# Both instances are created on the network called 'default'. Every project used
# to have one; a project created under an organisation policy that suppresses
# automatic networks does not, and the failure that produces arrives as an
# instance that cannot be created for a reason that names a network.
gcloud_ensure_network() {
  local project="$1"
  if gc 90 compute networks describe default --project="$project" \
       --format='value(name)' >/dev/null 2>>"$LR_LOG"; then
    ok "VPC network 'default' exists"
    return 0
  fi
  warn "this project has no VPC network called 'default'"
  running "creating an auto-mode network 'default'"
  gc "$LR_API_TIMEOUT" compute networks create default --project="$project" \
     --subnet-mode=auto >/dev/null 2>>"$LR_LOG" \
    || die "could not create a VPC network in $project. The reason is at the end
    of $LR_LOG. An organisation policy is the usual cause; create the network by
    hand, or use a project that already has one."
  # Subnet creation trails network creation by a few seconds.
  sleep 10
  ok "created VPC network 'default'"
}

# ---------------------------------------------------------------------------
# Firewall
# ---------------------------------------------------------------------------

# Rules scoped to the network tag these instances carry, so they cannot widen
# access to anything else in the project. Traffic between the two nodes is
# allowed on the measurement ports; ssh is allowed only from the address range
# Identity-Aware Proxy forwards from.
#
# Creation is retried rather than fatal on the first refusal. A rule created by
# a run that was interrupted between the create and the describe answers
# 'already exists', which is a success with a non-zero status.
gcloud_firewall_rule() {
  local project="$1" name="$2"; shift 2
  if gc 90 compute firewall-rules describe "$name" --project="$project" \
       >/dev/null 2>&1; then
    ok "firewall rule $name exists"
    return 0
  fi
  running "creating firewall rule $name"
  local i=1 out rc
  while [ "$i" -le 3 ]; do
    rc=0
    out="$(gc "$LR_API_TIMEOUT" compute firewall-rules create "$name" \
             --project="$project" "$@" 2>&1)" || rc=$?
    printf '%s\n' "$out" >>"$LR_LOG" 2>/dev/null || true
    if [ "$rc" = "0" ]; then
      ok "firewall rule $name created"
      return 0
    fi
    case "$out" in
      *alreadyExists*|*"already exists"*)
        ok "firewall rule $name already exists"
        return 0 ;;
    esac
    [ "$i" -lt 3 ] || break
    sleep $((i * 10))
    i=$((i + 1))
  done
  return 1
}

gcloud_firewall() {
  local project="$1" subnet="$2"
  gcloud_firewall_rule "$project" linerate-internal \
    --network=default --direction=INGRESS --action=ALLOW \
    --rules=tcp:9099,udp:1024-65535,tcp:5201,udp:5201,udp:51820,icmp \
    --source-ranges="$subnet" --target-tags=linerate \
    || die "could not create the firewall rule linerate-internal in $project.
    The reason is at the end of $LR_LOG. Without it the two nodes cannot reach
    each other and nothing can be measured."
  ok "node-to-node traffic allowed, scoped to tag 'linerate' and source $subnet"

  gcloud_firewall_rule "$project" linerate-iap-ssh \
    --network=default --direction=INGRESS --action=ALLOW \
    --rules=tcp:22 --source-ranges=35.235.240.0/20 --target-tags=linerate \
    || warn "could not create linerate-iap-ssh; the network's own rules may already allow it"
}

# ---------------------------------------------------------------------------
# Instances that are not ours
# ---------------------------------------------------------------------------

# Every instance the signed-in account can see, in every project it can see,
# except the two this study is about to use. Printed as project,name,zone,status
# on standard output; progress goes to standard error, so that a caller
# capturing the list still sees how far it has got.
#
# A neighbouring instance is not a neutral bystander. It competes for the same
# physical host, the same last-level cache and the same network, and what it
# does lands in this study's data as variance on whichever factor level happened
# to be running at the time. The interleaved sweep order spreads that damage
# rather than repairing it. Removing the neighbours is the repair.
#
# Most accounts hold at least one project that never had Compute Engine enabled.
# Asking such a project for its instances fails, which is the right answer and
# is not worth reporting: it is skipped, with the reason in the driver log.
gcloud_foreign_instances() {
  local keep_project="$1" keep_a="$2" keep_b="$3"
  local projects total i=0 p name zone status
  projects="$(gc 120 projects list --filter='lifecycleState:ACTIVE' \
                --format='value(projectId)' 2>>"$LR_LOG" || true)"
  total="$(printf '%s\n' "$projects" | grep -c . || true)"
  printf '    %s… %s project(s) to look at%s\n' "$C_DIM" "$total" "$C_OFF" >&2
  for p in $projects; do
    i=$((i + 1))
    printf '    %s… [%s/%s] %s%s\n' "$C_DIM" "$i" "$total" "$p" "$C_OFF" >&2
    while IFS=, read -r name zone status; do
      [ -n "$name" ] || continue
      if [ "$p" = "$keep_project" ] && \
         { [ "$name" = "$keep_a" ] || [ "$name" = "$keep_b" ]; }; then
        continue
      fi
      printf '%s,%s,%s,%s\n' "$p" "$name" "$zone" "$status"
    done <<EOF
$(gc "$LR_LIST_TIMEOUT" compute instances list --project="$p" \
    --format='csv[no-heading](name,zone.basename(),status)' 2>>"$LR_LOG" || true)
EOF
  done
}

gcloud_purge_foreign_instances() {
  local keep_project="$1" keep_a="$2" keep_b="$3"
  local list p n z s
  running "looking for other instances on this account"
  list="$(gcloud_foreign_instances "$keep_project" "$keep_a" "$keep_b")"
  if [ -z "$list" ]; then
    ok "no other instances exist on this account"
    return 0
  fi
  warn "these instances belong to this account and are not part of this study:"
  while IFS=, read -r p n z s; do
    [ -n "$n" ] || continue
    info "    $n  ($z, $s)  in project $p"
  done <<EOF
$list
EOF
  info "They will be deleted. Anything on their disks is lost with them."
  if ! confirm_soft "Delete every instance listed above?"; then
    warn "left in place. They share hardware with the measurement, so whatever"
    warn "they do arrives in this study's data as variance."
    return 0
  fi
  while IFS=, read -r p n z s; do
    [ -n "$n" ] || continue
    running "deleting $n in $p"
    if gc "$LR_API_TIMEOUT" compute instances delete "$n" --project="$p" --zone="$z" \
         >/dev/null 2>>"$LR_LOG"; then
      ok "deleted $n"
    else
      warn "could not delete $n in $p; delete it by hand or it keeps billing"
    fi
  done <<EOF
$list
EOF
  return 0
}

# ---------------------------------------------------------------------------
# Instances that are ours
# ---------------------------------------------------------------------------

gcloud_instance_exists() {
  gc 90 compute instances describe "$2" --project="$1" --zone="$3" \
    --format='value(name)' >/dev/null 2>&1
}

gcloud_internal_ip() {
  gc 90 compute instances describe "$2" --project="$1" --zone="$3" \
    --format='value(networkInterfaces[0].networkIP)' 2>/dev/null
}

# ---------------------------------------------------------------------------
# ssh and scp
# ---------------------------------------------------------------------------

# --quiet stops gcloud asking whether to generate an ssh key the first time it
# connects, a question that arrives in the middle of a step and stops it dead.
# --verbosity=error drops the tunnel's advice about installing NumPy, which was
# three quarters of the output of a run. -n gives ssh /dev/null for its own
# standard input, so a remote command can never consume what the operator is
# typing at the driver, and nothing on the far side inherits a descriptor that
# leads back to this terminal.
gcloud_ssh() {
  local project="$1" zone="$2" name="$3"; shift 3
  with_timeout "$LR_SSH_TIMEOUT" \
    gcloud compute ssh "$name" --project="$project" --zone="$zone" \
      --tunnel-through-iap --quiet --verbosity=error --command="$*" \
      -- -n -o ConnectTimeout=20 -o ServerAliveInterval=15 \
         -o ServerAliveCountMax=6 -o StrictHostKeyChecking=no -o LogLevel=ERROR \
    </dev/null
}

# The same call, with the remote command's standard output captured rather than
# printed, and its standard error kept apart so that a status line stays a
# status line. Returns the remote status; prints whatever the remote printed.
gcloud_ssh_out() {
  local tmp err rc=0
  tmp="$(mktemp "${TMPDIR:-/tmp}/linerate.XXXXXX")"
  err="$tmp.err"
  gcloud_ssh "$@" >"$tmp" 2>"$err" || rc=$?
  cat "$tmp"
  if [ "$rc" -ne 0 ] && [ -s "$err" ]; then
    sed 's/^/        /' "$err" >&2
  fi
  rm -f "$tmp" "$err"
  return "$rc"
}

# A call whose output is uninteresting and whose failure is not fatal: starting
# something that a later, separate session will verify. Anything it printed goes
# to the log rather than to the operator.
gcloud_ssh_detached() {
  local project="$1" zone="$2" name="$3"; shift 3
  local limit="${LR_START_TIMEOUT:-120}"
  with_timeout "$limit" \
    gcloud compute ssh "$name" --project="$project" --zone="$zone" \
      --tunnel-through-iap --quiet --verbosity=error --command="$*" \
      -- -n -o ConnectTimeout=20 -o ServerAliveInterval=15 \
         -o ServerAliveCountMax=6 -o StrictHostKeyChecking=no -o LogLevel=ERROR \
    </dev/null >>"$LR_LOG" 2>&1
}

# Transient tunnel failures are common enough that a single attempt is not
# evidence of anything. Three are.
gcloud_ssh_retry() {
  local tries="$1"; shift
  local i=1 rc=0
  while [ "$i" -le "$tries" ]; do
    rc=0
    gcloud_ssh "$@" || rc=$?
    if [ "$rc" -eq 0 ]; then return 0; fi
    if [ "$i" -lt "$tries" ]; then
      warn "attempt $i of $tries failed (status $rc); retrying"
      sleep 8
    fi
    i=$((i + 1))
  done
  return "$rc"
}

gcloud_scp_from() {
  local project="$1" zone="$2" name="$3" remote="$4" local_path="$5"
  with_timeout "$LR_SCP_TIMEOUT" \
    gcloud compute scp --project="$project" --zone="$zone" --tunnel-through-iap \
      --quiet --verbosity=error --recurse "$name:$remote" "$local_path" </dev/null
}

gcloud_scp_to() {
  local project="$1" zone="$2" name="$3" local_path="$4" remote="$5"
  with_timeout "$LR_SCP_TIMEOUT" \
    gcloud compute scp --project="$project" --zone="$zone" --tunnel-through-iap \
      --quiet --verbosity=error --recurse "$local_path" "$name:$remote" </dev/null
}
