# Working from the Mac

One command:

```bash
bash ~/Networks-HPC/linerate/mac/linerate go --project linerate-payload-2026
```

and the steps it is made of, each usable on its own:

```bash
./mac/linerate doctor        # what this Mac has and what it does not
./mac/linerate cloud         # project, billing, APIs, network — creates nothing
./mac/linerate up            # project, two VMs, build on both
./mac/linerate run --quick   # two minutes, checks the whole path
./mac/linerate run --full    # the real sweep
./mac/linerate watch         # re-attach to a sweep already running
./mac/linerate fetch         # bring the dataset back and merge it
./mac/linerate report        # figures, tables and the PDF, here
./mac/linerate status        # what exists and what is billing
./mac/linerate down          # delete the VMs so they stop billing
./mac/linerate purge         # delete every other instance on the account
```

## Before the first run

```bash
brew install --cask google-cloud-sdk
gcloud auth login
```

That is all that is needed. `go` builds the Python environment beside the
repository, repairs the file modes a zip archive loses, clears the quarantine
attribute macOS puts on a download, and finds the billing account itself when
exactly one is attached to the login.

**You choose the project name.** Nothing in this repository names a Google
account, a project or a billing account, which is what makes it reproducible by
someone other than its author. `linerate-payload-2026` is a suggestion the
runbook prints, not a fact about the author's cloud; project IDs are globally
unique, so if that one is taken, `up` adds a random suffix and prints the ID it
actually created. Google will not say which IDs are taken — an ID it refuses to
show you may belong to a stranger or may never have existed, and the refusal is
worded identically — so `up` does not try to tell the two apart. It treats an ID
it cannot see as unknown and settles the question by creating it, which is the
only call that answers straight. What you supply is remembered in `~/.linerate/state.env` —
outside the repository, so it cannot be committed.

## The Mac does not measure

`setup.sh` builds on macOS and the self-test passes there, but the harness
classifies macOS as a `development` host and refuses to write a measurement from
one. Thread affinity on macOS is a hint rather than an instruction, and a
per-packet cycle count from an unpinned thread is worse than no number at all
because it looks like data.

So the split is: the Mac creates, builds, drives, collects and typesets. The VMs
measure.

## What each command actually does

**`go`** — asks one question, then runs `preflight`, `up`, `run --quick`,
`run --full`, `fetch`, `report` and `down` in that order. If any of them fails
it says which, prints the three commands that resume, look at or stop the run,
and offers to delete the instances so that a failure does not quietly keep
billing.

**`cloud`** — reports the four preconditions on the Google side, in the order in
which they depend on each other: the project exists and is not pending deletion,
an open billing account is linked to it, Compute Engine, IAP and OS Login are
enabled on it, and a VPC network called `default` exists. It creates nothing and
charges nothing, and it is the fastest way to find out why a run stopped before
it reached an instance.

**`up`** — checks `gcloud` is authenticated; creates the project if it does not
exist, or restores it when it is pending deletion. Restoring is tried before a
new ID is invented, because a shut-down project goes on counting against the
project quota for the thirty days Google keeps it, so restoring one costs
nothing that has not been spent while creating another spends a slot the account
may not have. Then, as four
separate steps each of which fails on its own rather than being carried past:
links a billing account — for a restored project this is not optional
housekeeping but the repair for a specific consequence of deletion, which
disconnects the billing account and does not reconnect it on restore — retried
five times over two minutes, because a project created or restored moments ago
is not yet known to the billing service — for every project, not only for
one just created, because an existing project is not necessarily a paid-for one
and an unbilled project cannot have Compute Engine enabled; enables the Compute
Engine, IAP and OS Login APIs, of which only the first is fatal; waits for
Compute Engine to actually answer for the project, because enabling an API is
eventually consistent and every call made inside that window fails with a
message about the project rather than about the service; and creates a VPC
network called `default` if the project has none. It then
deletes and remakes any instance of the same name left from an earlier attempt,
rather than reusing a machine whose state nothing recorded;
creates two firewall rules, one for traffic between the nodes and one allowing
ssh from the IAP range only, both scoped to the network tag `linerate` so they
cannot widen access to anything else in the project; deletes every other
instance the account can see, for the reason below; creates `linerate-a` and
`linerate-b` with gVNIC, falling back through `n2-standard-4`,
`n2d-standard-4`, `c2-standard-4` and `t2d-standard-4` when the requested family
has no quota in the zone — every one of them supports gVNIC, both nodes get the same one, and
the one actually used is what the dataset records; waits for `ssh` rather than for the instance to report
RUNNING (an instance is RUNNING well before `sshd` accepts a connection); then
copies the working tree — not a git clone, so uncommitted local changes are what
gets measured — and builds on both.

Every step that creates a billable resource asks first.

**`run`** — runs the self-test on both nodes and stops if either fails; starts
`lr_loadgend` and an `iperf3` server on node A; starts the sweep on node B with
`--peer` and `--local` already filled in; reads the sweep's log until it ends;
stops the generator and clears any network condition the sweep left behind.

**`watch`** — reads the log of a sweep that is already running. The sweep is
detached from the connection that started it, so this can be run, interrupted
and run again without touching the measurement.

**`fetch`** — copies `results/raw` back and merges it. A `results/` left by an
earlier run is moved aside first, because the merge refuses to combine fragments
from different machines and that refusal is correct. The archive itself contains
no dataset: nothing in the tree is a measurement until one is made.

**`report`** — figures, macros, tables and then the PDF, in that order and only
in that order: the macros depend on facts the figures compute while drawing, and
the PDF depends on both. Then `check_wiring.py` and `check_numbers.py`. A
missing LaTeX installation costs the PDF and nothing else; the step warns and
carries on.

**`down`** — deletes both instances after confirming. The project stays;
deleting it is one further command that the output prints.

**`purge`** — deletes every instance the signed-in account can see except this
study's two, in every project it can see. `up` does this as one of its steps.

## Why other instances are deleted

Both nodes are virtual machines on shared physical hardware. A third instance of
your own competes for the same last-level cache, the same memory bandwidth and
the same network, and what it does arrives in this study's data as variance on
whichever factor level was running at the time. The interleaved sweep order
spreads that damage rather than removing it.

The list is printed before anything is deleted, and `--keep-other-vms` skips the
step. It cannot see instances belonging to other tenants of the same physical
host; the anchor measured at both ends of every sweep is what catches those.

## Nothing started here is attached to the session that started it

Three failures used to end runs, and all three are structural rather than
accidental.

An ssh session does not close while a process it started still holds the
channel, so `nohup … &` over `gcloud compute ssh` returns only when the daemon
exits. A driver that waits for that call waits for ever, having already done the
work — the run that stopped after printing the word `started`. Redirecting the
descriptors does not repair it, because they are moved around inside a process
tree that still belongs to the session. `cloud/nodectl.sh` hands the process to
`systemd-run` instead, and a separate session confirms it is up.

Handing it to `systemd-run` is not the same as knowing it started. That command
returns once the transient unit has been accepted and its start job queued, so
its exit status describes the unit and not the process. Reading it as *running*
was the second failure: a unit whose `ExecStart` failed was never noticed, the
`setsid` fallback was never reached because it only ran when `systemd-run`
itself refused, and `--collect` had already removed the unit that could have
said why. `spawn` now verifies by looking for the process, keeps looking for a
few seconds because the interesting failure starts and then dies, copies
`systemctl status` and the journal into the log, and only then falls back — so
the fallback covers a unit that failed as well as one that was refused.

An hour-long sweep behind a single ssh connection is an hour in which a dropped
tunnel destroys the run. The sweep is a transient `systemd` unit writing to a
log on the node, and the driver reads that log in short polls, any one of which
can fail without consequence.

On top of both, every remote call runs under a timeout, so a call that hangs for
a reason nobody predicted becomes a message rather than a cursor that never
comes back.

## Nothing this driver runs can ask a question

`gcloud` asks. The commonest question is whether to enable an API on a project
that does not have it, and it is asked on standard error — which a loop over
projects discards along with the question, leaving a terminal that shows nothing
while the process waits for a keystroke. Every call to `gcloud` here therefore
passes `--quiet`, reads `/dev/null`, and runs under a timeout: the first makes
it take the default answer, the second turns any question nobody suppressed into
end of file, and the third turns a call that neither answers nor fails into a
message.

## If something goes wrong

```bash
./mac/linerate status     # what exists, what is billing, and what the sweep is doing
cat ~/.linerate/driver.log   # the output of the calls the driver makes quietly
```

`status` warns when instances are running. Two `c3-standard-4` instances left
running overnight are not free.

The state file is disposable:

```bash
rm ~/.linerate/state.env    # forget the session; the instances still exist
```

## Non-interactive use

`LR_YES=1` skips every confirmation, including the ones before creating and
deleting instances. `go` sets it for itself after its own single question. Set
it by hand only in scripts — interactively, the prompts are the only thing
between a typo and a billing surprise.

`LR_POLL` sets the seconds between reads of the sweep's log, `LR_SWEEP_TIMEOUT`
the longest a sweep may run before the driver stops watching it, and
`LR_STATE_DIR` where the session state and the driver log are kept.
