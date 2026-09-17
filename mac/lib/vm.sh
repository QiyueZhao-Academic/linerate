# vm.sh — instance lifecycle, the state that survives between commands, and the
# starting of anything on a node that has to outlive the ssh session that
# started it.
#
# State lives in ~/.linerate/state.env, outside the repository, so that nothing
# identifying the operator's cloud account can be committed by accident. Each
# command reads it, so `linerate run` does not have to be told again what
# `linerate up` already established.

LR_STATE_DIR="${LR_STATE_DIR:-$HOME/.linerate}"
LR_STATE="$LR_STATE_DIR/state.env"
LR_LOG="${LR_LOG:-$LR_STATE_DIR/driver.log}"

state_dir() {
  mkdir -p "$LR_STATE_DIR"
  chmod 700 "$LR_STATE_DIR" 2>/dev/null || true
  : >> "$LR_LOG" 2>/dev/null || true
}

state_load() {
  [ -f "$LR_STATE" ] && . "$LR_STATE" || true
}

state_save() {
  state_dir
  {
    echo "# linerate state, written by mac/linerate. Safe to delete."
    echo "LR_PROJECT=${LR_PROJECT:-}"
    echo "LR_PROJECT_NAME='${LR_PROJECT_NAME:-}'"
    echo "LR_ZONE=${LR_ZONE:-}"
    echo "LR_MACHINE=${LR_MACHINE:-}"
    echo "LR_NODE_A=${LR_NODE_A:-}"
    echo "LR_NODE_B=${LR_NODE_B:-}"
    echo "LR_IP_A=${LR_IP_A:-}"
    echo "LR_IP_B=${LR_IP_B:-}"
    echo "LR_CREATED=${LR_CREATED:-}"
  } > "$LR_STATE"
  chmod 600 "$LR_STATE"
}

state_clear() { rm -f "$LR_STATE"; }

# The two nodes are identical. Node A generates load, node B runs the data plane
# and drives the sweep. Making them identical means a difference between them is
# never an explanation for a result.
#
# An instance of the same name left over from an earlier attempt is deleted and
# made again rather than reused. It carries a half-finished build, an apt state
# nobody recorded, possibly a daemon still running and possibly a network
# condition still attached to its interface — none of which is visible in the
# dataset it would go on to produce. A minute of recreation buys a run that
# starts from the same place every time. LR_REUSE_INSTANCES=1 keeps the old one,
# which is worth having while debugging the driver and worth nothing otherwise.
vm_create() {
  local project="$1" zone="$2" machine="$3" name="$4"
  if gcloud_instance_exists "$project" "$name" "$zone"; then
    if [ "${LR_REUSE_INSTANCES:-0}" = "1" ]; then
      ok "instance $name already exists and is being reused (LR_REUSE_INSTANCES=1)"
      return
    fi
    warn "$name exists from an earlier attempt; deleting it so this run starts clean"
    vm_delete "$project" "$zone" "$name"
  fi
  # A quota of zero for one machine family is not a reason to abandon a study
  # that only needs four vCPUs and a gVNIC. A fresh project frequently has no C3
  # quota in a given region while having plenty of N2, and the difference is a
  # different processor rather than a different experiment: the harness records
  # which one it ran on and every cost is reported in reference cycles, so the
  # substitution changes the platform the study describes and not the study.
  # It is announced rather than silent, and the machine actually used is what
  # goes into the state file and from there into the dataset.
  #
  # gVNIC is requested explicitly throughout. The default virtio-net driver has
  # a different per-packet cost, and a study that reports a fixed cost without
  # saying which driver produced it is not reproducible. Every fallback here is
  # a family that supports gVNIC; e2 is deliberately not among them.
  local candidates="$machine" alt out rc=0
  for alt in c3-standard-4 n2-standard-4 n2d-standard-4 c2-standard-4 t2d-standard-4; do
    [ "$alt" = "$machine" ] || candidates="$candidates $alt"
  done

  local tried="" previous=""
  for alt in $candidates; do
    if [ -n "$previous" ]; then
      warn "$previous was refused in $zone; trying $alt instead"
    fi
    tried="$tried $alt"
    previous="$alt"
    running "creating $name ($alt in $zone)"
    rc=0
    out="$(gc "$LR_API_TIMEOUT" compute instances create "$name" \
      --project="$project" \
      --zone="$zone" \
      --machine-type="$alt" \
      --image-family=ubuntu-2404-lts-amd64 \
      --image-project=ubuntu-os-cloud \
      --boot-disk-size=40GB \
      --boot-disk-type=pd-balanced \
      --network-interface=nic-type=GVNIC,network=default \
      --tags=linerate \
      --metadata=enable-oslogin=TRUE \
      --labels=purpose=linerate-measurement 2>&1)" || rc=$?
    printf '%s\n' "$out" >>"$LR_LOG" 2>/dev/null || true
    if [ "$rc" = "0" ]; then
      LR_MACHINE="$alt"
      ok "created $name ($alt)"
      return 0
    fi
    # Only quota and capacity are worth trying another family for. A permission
    # error, a bad image or a missing network says the same thing about every
    # machine type there is, and retrying it three times only delays the report.
    case "$out" in
      *QUOTA_EXCEEDED*|*quota*|*Quota*|\
      *ZONE_RESOURCE_POOL_EXHAUSTED*|*"does not have enough resources"*|\
      *"currently unavailable"*|*"Invalid value for field 'resource.machineType'"*|\
      *"Machine type with name"*) ;;
      *) break ;;
    esac
  done

  die "could not create $name in $zone. Tried:$tried
    The reason is at the end of $LR_LOG.
    If it named a quota, it named the metric with it: two four-vCPU instances
    need eight vCPUs in one region. Raise it under IAM & Admin → Quotas, or run
    this again in another zone:
      bash $SELF go --zone europe-west4-a
    Do not drop below four vCPUs; the reason is in docs/environment.md."
}

vm_delete() {
  local project="$1" zone="$2" name="$3"
  if ! gcloud_instance_exists "$project" "$name" "$zone"; then
    dim "$name does not exist"
    return
  fi
  running "deleting $name"
  gc "$LR_API_TIMEOUT" compute instances delete "$name" --project="$project" \
      --zone="$zone" >/dev/null \
    || warn "could not delete $name; delete it by hand or it will keep billing"
  ok "deleted $name"
}

# Waiting for ssh rather than for the instance to report RUNNING: an instance is
# RUNNING well before sshd accepts a connection, and a bootstrap that starts too
# early fails in a way that looks like a network problem.
vm_wait_ssh() {
  local project="$1" zone="$2" name="$3"
  local LR_SSH_TIMEOUT=60
  running "waiting for ssh on $name"
  local i=0
  while [ $i -lt 40 ]; do
    if gcloud_ssh "$project" "$zone" "$name" "true" >>"$LR_LOG" 2>&1; then
      ok "$name is reachable"
      return 0
    fi
    sleep 6
    i=$((i + 1))
  done
  die "$name did not accept ssh within four minutes. Look at the serial console:
    gcloud compute instances get-serial-port-output $name --zone=$zone --project=$project"
}

# Push the repository and build it on the node. A tar over the gcloud tunnel
# rather than a git clone, so that uncommitted local changes are what gets
# measured — which is what an operator iterating on the code actually wants.
vm_push_and_build() {
  local project="$1" zone="$2" name="$3" repo="$4"
  local LR_SSH_TIMEOUT="${LR_BUILD_TIMEOUT:-2400}"

  running "copying the repository to $name"
  # Anything left running from an earlier session holds files in the tree that
  # is about to be replaced, and a sweep that ran as root leaves a results
  # directory the login user cannot remove.
  gcloud_ssh "$project" "$zone" "$name" \
    "[ -f ~/linerate/cloud/nodectl.sh ] && bash ~/linerate/cloud/nodectl.sh nuke >/dev/null 2>&1; \
     sudo -n rm -rf ~/linerate >/dev/null 2>&1 || rm -rf ~/linerate; mkdir -p ~/linerate" \
    >>"$LR_LOG" 2>&1 || true

  # macOS puts its extended attributes into a tar as ._ files, which arrive on
  # the node as a second copy of every source file with a name the build ignores
  # and a reader does not.
  COPYFILE_DISABLE=1 tar --exclude='.git' --exclude='build' --exclude='results' \
      --exclude='results-*' --exclude='*.o' \
      -czf /tmp/linerate-src.tgz -C "$(dirname "$repo")" "$(basename "$repo")"
  gcloud_scp_to "$project" "$zone" "$name" /tmp/linerate-src.tgz "/tmp/linerate-src.tgz" \
    >>"$LR_LOG" 2>&1 || die "could not copy the source to $name"
  gcloud_ssh "$project" "$zone" "$name" \
    "mkdir -p ~/linerate && tar xzf /tmp/linerate-src.tgz -C ~/linerate --strip-components=1" \
    >>"$LR_LOG" 2>&1 || die "could not unpack the source on $name"
  ok "source copied"

  running "bootstrapping and building on $name (a few minutes the first time)"
  gcloud_ssh "$project" "$zone" "$name" \
    "cd ~/linerate && sudo -n bash cloud/bootstrap.sh && bash setup.sh" \
    || die "the build failed on $name. Go and look at it with:
    gcloud compute ssh $name --zone=$zone --project=$project --tunnel-through-iap"

  # A build that produced no binaries fails later, in the middle of a sweep,
  # with a message about a missing file rather than about a build.
  gcloud_ssh "$project" "$zone" "$name" \
    "test -x ~/linerate/build/lr_bench && test -x ~/linerate/build/lr_loadgend" \
    >>"$LR_LOG" 2>&1 \
    || die "the build on $name produced no binaries"
  ok "built on $name"
}

# ---------------------------------------------------------------------------
# Starting something that has to outlive the session that starts it
# ---------------------------------------------------------------------------

# The start call is allowed to fail, and is allowed to hang: it is
# fire-and-forget by construction, and whether the thing is running is
# established by a second, separate session that cannot be affected by whatever
# happened to the first one. That is the whole repair for a driver that used to
# stop after the word 'started' — see the comment at the top of
# cloud/nodectl.sh for why the first session can hang at all.
#
# The diagnosis is fetched here rather than described to the operator. Everything
# worth reading lives on an instance that this driver deletes within the minute,
# and an instruction to go and read a file on a machine that is about to stop
# existing is not a diagnosis. One extra ssh round trip on the failure path buys
# the whole story on screen while the machine that has it is still alive.
vm_daemon_start() {
  local project="$1" zone="$2" name="$3" label="$4" start_cmd="$5" status_cmd="$6"
  local diag_cmd="${7:-}"
  running "starting the $label on $name"
  gcloud_ssh_detached "$project" "$zone" "$name" "$start_cmd" || true
  local i=1 out=""
  while [ "$i" -le 6 ]; do
    out="$(gcloud_ssh_out "$project" "$zone" "$name" "$status_cmd" 2>/dev/null || true)"
    case "$out" in
      running*) ok "$label on $name: ${out#running }"; return 0 ;;
    esac
    sleep 5
    i=$((i + 1))
  done
  warn "$label did not come up on $name: ${out:-no answer}"
  if [ -n "$diag_cmd" ]; then
    say ""
    info "What $name says about it:"
    gcloud_ssh_out "$project" "$zone" "$name" "$diag_cmd" 2>/dev/null \
      | sed 's/^/      /' || warn "could not reach $name for a diagnosis"
    say ""
  fi
  return 1
}
