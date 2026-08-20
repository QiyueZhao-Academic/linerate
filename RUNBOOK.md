# Runbook

What to do, in order, and what to do when a step does not behave. Written for
this machine: a 13-inch MacBook Pro (M1, 2020) under macOS 26.6.1, repository at
`~/Networks-HPC/linerate`, environment directory beside it at
`~/Networks-HPC/linerate-env`.

## The whole study, in five lines

Paste these one at a time, from any directory. Lines 1 and 2 are needed only
once per machine; skip them if Homebrew and the toolchain are already there.

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

brew install cmake openssl@3 && brew install --cask google-cloud-sdk basictex && eval "$(/usr/libexec/path_helper)" && sudo tlmgr update --self && sudo tlmgr install latexmk acmart booktabs preprint

gcloud auth login

mkdir -p ~/Networks-HPC && rm -rf ~/Networks-HPC/linerate && unzip -q ~/Downloads/linerate.zip -d ~/Networks-HPC

bash ~/Networks-HPC/linerate/mac/linerate go --project linerate-payload-2026
# Choose your own ID here: project IDs are globally unique, and yours is never stored in this repository.
```

The last line is the study. It asks one question — a summary of everything it is
about to do — and after you answer it, it does not stop to ask again:

1. checks this Mac and builds the analysis environment in
   `~/Networks-HPC/linerate-env/venv`
2. deletes every other virtual machine on your Google account, so that nothing
   else on the same hardware perturbs the measurement
3. creates the project `linerate-payload-2026` — display name *LineRate
   Payload Crossover* — links your billing account, and creates two brand new
   `c3-standard-4` instances in `europe-west1-b`
4. copies this working tree to both and builds it there
5. runs a two-minute smoke sweep, then the real one
6. brings the dataset back, draws the figures, generates the macros and tables,
   and typesets the PDF
7. deletes the two instances, so they stop billing

About an hour and a quarter, and about a dollar of instance time. The PDF opens
by itself at the end; it is at `~/Networks-HPC/linerate/results/linerate-report.pdf`
and the dataset it was built from is at `results/results.json`.

**Nothing in this archive is a report.** There is no `results/`, no
`report/generated/`, no `report/figures/` and no PDF until a run produces them:
the figures are drawn from the dataset that run measured, every quantity in the
prose is a macro generated from the same file, and the PDF is typeset last, out
of both. So there is never a document in the tree whose numbers came from
somewhere else, and a PDF that exists is one this machine measured.

`bash …/mac/linerate` rather than `./mac/linerate` because a zip archive does
not carry the Unix execute bit. The first thing `go` does is repair that, so
`./mac/linerate …` works from the repository root from then on.

The `rm -rf` before the unzip is deliberate. Unpacking over an older tree keeps
every file the older one had that the new archive does not, which for this
project means a dataset and a PDF measured on a machine that is not yours. The
environment directory `~/Networks-HPC/linerate-env` is outside the repository
and survives, so nothing is downloaded twice.

### The two things you may be asked

**A billing account.** If exactly one open billing account is attached to the
Google login, `go` uses it and says which. If there are several it prints them
and asks once, before anything is created. To choose one in advance, pass it:
`--billing 01EB96-F5914D-A25385`, the ID from `gcloud billing accounts list`.

This is settled for every project, not only for one being created. A project
left over from an earlier attempt exists without necessarily paying for
anything, and an unbilled project cannot have Compute Engine enabled on it —
which used to be discovered three steps later, as a firewall rule failing with
`The resource 'projects/...' was not found`.

**A project ID that is already taken.** Project IDs are globally unique across
Google Cloud, so a stranger may hold `linerate-payload-2026`. `go` adds a short
random suffix, prints the ID it actually created, and carries on. Nothing needs
to be done about it.

Note that Google will not tell anybody which IDs are taken — an ID it refuses to
show you may be a stranger's or may never have existed, and the message is the
same either way. `go` therefore does not try to tell them apart: it treats an ID
it cannot see as unknown and settles the question by creating it, which is the
only operation that gives a straight answer.

### Variations

```bash
bash ~/Networks-HPC/linerate/mac/linerate go --quick-only      # stop after the two-minute smoke run
bash ~/Networks-HPC/linerate/mac/linerate go --keep            # leave the instances running at the end
bash ~/Networks-HPC/linerate/mac/linerate go --keep-other-vms  # do not touch other instances on the account
bash ~/Networks-HPC/linerate/mac/linerate go --zone us-central1-a
```

`--keep` leaves two instances billing. `./mac/linerate status` says whether
anything is still running and `./mac/linerate down` stops it.

## The same thing as separate steps

`go` is these, in order. Each can be run on its own, which is how to carry on
after something failed rather than starting again from the beginning.

```bash
cd ~/Networks-HPC/linerate
./mac/linerate doctor                 # what this Mac has and has not
./mac/linerate cloud                  # project, billing, APIs, network — creates nothing
./mac/linerate up --project linerate-payload-2026
./mac/linerate run --quick            # two minutes, exercises every path
./mac/linerate run --full             # the real sweep, about three quarters of an hour
./mac/linerate watch                  # re-attach to a sweep already running
./mac/linerate fetch                  # bring the dataset back and merge it
./mac/linerate report                 # figures, macros, tables, PDF
./mac/linerate status                 # what exists and what is billing
./mac/linerate down                   # delete the instances
./mac/linerate purge                  # delete every other instance on the account
```

`up` is repeatable: an existing project or firewall rule is reported and
skipped, so re-running it after a failure costs nothing but the time to rebuild.
An existing *instance* is deleted and made again rather than reused — it carries
a half-finished build, an apt state nobody recorded, possibly a daemon still
running and possibly a network condition still attached to its interface, none
of which would be visible in the dataset it went on to produce. A minute of
recreation buys a run that starts from the same place every time;
`--reuse-instances` keeps the old one when that is what you want.

`run` is repeatable in the same way — it stops anything left over on either node
and wipes the node's `results/` before starting, so one sweep produces exactly
one dataset.

`--quick` is a smoke test. Its dataset is marked `quick: true` and no conclusion
should be drawn from it; it exists to confirm every path executes before an hour
is spent.

## Why the instances on your account are deleted

Both nodes are virtual machines on shared physical hardware. A third instance of
your own, running anything at all, competes for the same last-level cache, the
same memory bandwidth and the same network, and what it does arrives in this
study's data as variance on whichever factor level happened to be running at the
time. The interleaved sweep order spreads that damage across factor levels
rather than removing it; removing the neighbours is the only thing that removes
it.

`go` and `up` therefore list every instance the signed-in account can see, in
every project it can see, and delete all of them except this study's two. The
list is printed before anything is deleted. `--keep-other-vms` skips the step,
and `./mac/linerate purge` does it on its own.

This cannot see, and so cannot remove, instances belonging to other tenants on
the same physical host. That is what the anchor measurement at both ends of each
sweep is for: a machine that changed underneath the measurement voids the sweep
instead of being reported.

## The Mac does not measure

The harness classifies macOS as a `development` host and refuses to write a
measurement from one. Thread affinity here is a hint rather than an instruction,
and a per-packet cycle count from an unpinned thread is worse than no number
because it looks like data. So the Mac creates the instances, pushes the working
tree, drives the sweep, collects the dataset and typesets it. The two Ubuntu
24.04 VMs measure, and they compile their own binaries; nothing is
cross-compiled from the M1.

macOS ships LibreSSL headers under OpenSSL's name, which compile and then behave
differently, so `setup.sh` locates Homebrew's `openssl@3` explicitly. Apple's
clang rejects `-march=native`, and CMake falls back to `-mcpu=native` here.
Downloads and build products go to `~/Networks-HPC/linerate-env`, beside the
repository rather than inside it, so a clean checkout stays clean.

To build and self-test here without measuring anything — useful for checking
that the tree is intact — run `./setup.sh`.

## Reading the output of a run

```
==> Self-test on both nodes     must pass on both or the run stops
==> Load generator on node A    detached; a separate session confirms it is up
==> Sweeping on node B          the node's own log, read every 20 seconds
    ==> e1 … controls           each writes results/raw/<name>.json
    ==> merging                 "merged N fragment(s)" plus every unavailable reason
==> Fetching the dataset        scp, then merge here
==> Building the report         figures, macros, tables, PDF, then the two checks
```

An experiment that prints `did not complete; its fragment records why` is not a
failure of the run. Open the fragment and read `reason`.

The sweep does not run inside the connection that started it. It is a transient
`systemd` unit on node B writing to `~/linerate/results/sweep.log`, and this
terminal reads that log in short polls. Closing the laptop, losing the tunnel or
pressing Ctrl-C stops the *watching*, not the sweep: `./mac/linerate watch`
re-attaches, and `./mac/linerate down` is what actually stops it.

## Where things are, on the nodes

The working tree is at `~/linerate` on both. On node B the fragments accumulate
in `~/linerate/results/raw` and the sweep's own output is in
`~/linerate/results/sweep.log`; on node A the generator's output is in
`/tmp/linerate-loadgend.log`. To go and look:

```bash
source ~/.linerate/state.env && gcloud compute ssh "$LR_NODE_B" --project="$LR_PROJECT" --zone="$LR_ZONE" --tunnel-through-iap --command="bash ~/linerate/cloud/nodectl.sh sweep-poll 1 | head -1; ls -1 ~/linerate/results/raw | wc -l"
```

`~/.linerate/state.env` is written by `up`, is plain `KEY=value`, and holds the
project, zone, instance names and internal addresses and nothing else. It lives
outside the repository so it cannot be committed. `~/.linerate/driver.log` holds
the output of the calls the driver makes quietly, and is where to look when a
step failed without saying much.

`cloud/nodectl.sh` is the node side of all of this and can be driven by hand:

```bash
source ~/.linerate/state.env
gcloud compute ssh "$LR_NODE_A" --project="$LR_PROJECT" --zone="$LR_ZONE" --tunnel-through-iap --command="bash ~/linerate/cloud/nodectl.sh start-generator"
gcloud compute ssh "$LR_NODE_B" --project="$LR_PROJECT" --zone="$LR_ZONE" --tunnel-through-iap --command="bash ~/linerate/cloud/nodectl.sh start-sweep --full --peer=$LR_IP_A --local=$LR_IP_B"
gcloud compute ssh "$LR_NODE_B" --project="$LR_PROJECT" --zone="$LR_ZONE" --tunnel-through-iap --command="bash ~/linerate/cloud/nodectl.sh sweep-poll 1"
gcloud compute ssh "$LR_NODE_A" --project="$LR_PROJECT" --zone="$LR_ZONE" --tunnel-through-iap --command="bash ~/linerate/cloud/nodectl.sh nuke"
```

Each returns immediately. When one of them reports `stopped` or `failed`, the
matching `diagnose-` subcommand prints everything worth reading in one round
trip — the process table, the log, the unit's status and journal, the listening
sockets, and the launcher script that was used:

```bash
gcloud compute ssh "$LR_NODE_A" --project="$LR_PROJECT" --zone="$LR_ZONE" --tunnel-through-iap --command="bash ~/linerate/cloud/nodectl.sh diagnose-generator"
```

`mac/linerate` runs that itself before it gives up, so the output is already on
screen by the time it says the run has stopped. That matters because `go`
deletes the instances immediately afterwards, and an instruction to go and read
a file on a machine that will not exist in thirty seconds is not a diagnosis. The privileged experiments — the virtualisation tiers,
the service chain, the network conditions — need root, and `start-sweep` runs
the sweep as a `systemd` unit, which is root, so `--full` measures them without
anything further being asked for.

## Money

Two `c3-standard-4` instances in `europe-west1-b` bill by the second at roughly
0.2 USD per instance-hour. A complete cycle — create, quick, full, fetch — is on
the order of a dollar; two instances forgotten over a weekend are not. `go`
deletes them at the end, and deletes them too when something fails partway,
after asking. `status` says whether anything is still running, and the project
holds no billable resource once the instances are gone.

## Things that go wrong

### `run` stops after `started`

It cannot any more, and the reason is worth writing down because the obvious
repair is the wrong one.

An ssh session does not close while a process it started still holds the
channel. `nohup … &` over `gcloud compute ssh` therefore returns to the driver
only when the daemon it started exits, which is never. Adding `setsid` and
redirecting the descriptors moves them around inside a process tree that still
belongs to the session, which is why that repair did not hold either.

The process now belongs to `systemd` instead: `cloud/nodectl.sh` starts it with
`systemd-run`, the asking command returns at once, and a *separate* session
checks that it is up. On top of that every remote call in `mac/linerate` runs
under a timeout, so even a call that hangs for a reason nobody predicted ends as
a message rather than as a cursor that never comes back.

### `run` stops at `load generator did not come up`

This was the same failure wearing different clothes, and it is worth separating
from the one above.

`systemd-run` returns as soon as the transient unit has been accepted and its
start job queued — not when `ExecStart` has run. Its exit status therefore says
the unit was well-formed and says nothing about whether the process exists.
Reading that zero as *running* meant a unit that failed to start was never
noticed, the fallback that would have started it by hand was never reached
because it only ran when `systemd-run` itself refused, and `--collect` removed
the failed unit before anyone could ask it why. The symptom was a driver
reporting `stopped log=/tmp/linerate-loadgend.log` and a log file that was
empty, because nothing had ever run long enough to write to it.

`spawn` in `cloud/nodectl.sh` now trusts no mechanism. After every attempt it
looks for the process itself and keeps looking for a few seconds, because the
interesting failure is one that starts and dies a moment later. If the process
is not there it copies `systemctl status` and the unit's journal into the log
and *then* falls back to `setsid`, so the fallback covers a unit that failed as
well as a unit that was refused. `--collect` is gone, the redirection has moved
out of a `StandardOutput=append:` property and into a launcher script — which
also means an operator can run that script by hand and watch the daemon fail in
front of them — and `diagnose-generator` brings the whole picture back in one
round trip.

The daemon itself was part of it. `signal(2)` installs handlers with
`SA_RESTART` on Linux, so a `SIGTERM` arriving while `lr_loadgend` sat in
`accept()` set the stop flag and the kernel then restarted the call: the flag
was never read again. Everything that stopped the daemon had to escalate to
`SIGKILL`, and a daemon killed rather than stopped leaves sockets behind for the
next one to fail to bind. It now installs its handlers with `sigaction` and no
`SA_RESTART`, and — because a signal goes to whichever thread has it unblocked,
which need not be the one in `accept()` — it also gives its listening socket a
receive timeout, so the stop flag is read several times a second no matter who
was signalled.

### It stops at `looking for other instances on this account`

It cannot any more. `gcloud compute instances list` on a project that never had
Compute Engine enabled does not fail; it asks whether to enable the API and
retry, and it asks on standard error, which a loop over projects discards along
with the question. The terminal then shows nothing at all while the process
waits for a keystroke nobody knows to type.

Every call to `gcloud` in this driver now passes `--quiet`, which makes it take
the default answer instead of asking, reads `/dev/null`, so that a question
nobody suppressed reaches end of file rather than waiting, and runs under a
timeout. The step also prints each project as it reaches it, so that a slow
account looks slow rather than stuck.

### `zsh: no such file or directory: ./mac/linerate`

The shell is not in the repository root. `./` means the current directory, and
which folder a Finder window happens to be showing has nothing to do with it.
Use the full path, as the five lines above do:
`bash ~/Networks-HPC/linerate/mac/linerate go`.

### `zsh: permission denied: ./mac/linerate`

The archive stored `mac/linerate` as mode 644, because zip does not carry the
Unix execute bit. `bash ~/Networks-HPC/linerate/mac/linerate go` works anyway,
and repairs the mode on the way past.

### `error: externally-managed-environment`

Homebrew's Python refuses to be installed into. Nothing in the five lines
installs into it: `go` builds a virtual environment at
`~/Networks-HPC/linerate-env/venv` and uses that interpreter for every analysis
step. If you are running `python3 python/make_report.py` by hand, use
`~/Networks-HPC/linerate-env/venv/bin/python3` instead.

### `xcrun: error: invalid active developer path`

The command line tools are missing or were invalidated by a system update:
`xcode-select --install`.

### `latexmk not found` or `acmart.cls not found`

The dataset, the figures, the macros and the tables are all still produced and
only the PDF is missing; the run does not stop. Install the LaTeX line from the
sequence above and run `./mac/linerate report` again — nothing is measured a
second time.

Installing the class used to be necessary and not sufficient. A `latexmk` run
that failed leaves `.fdb_latexmk`, `.aux` and `.fls` describing a document that
was never built, and it believes them next time; the second run therefore failed
too, which reads as a second, different problem. `make_report.py` now discards
that state and builds once more before reporting anything, so installing the
class is enough. By hand the equivalent is `latexmk -C` in `report/`.

### `The resource 'projects/...' was not found` while creating the firewall rule

The project exists; Compute Engine has never heard of it. That happens when the
project is not linked to an open billing account, because an unbilled project
cannot have the Compute Engine API switched on, and every compute call
afterwards answers with a message about the project rather than about the
service.

```bash
./mac/linerate cloud --project linerate-payload-2026
```

says which of the four preconditions — project, billing, APIs, network — is the
one missing, and creates and charges nothing. `go` now establishes billing for
every project, existing or new, before it asks Compute Engine for anything, so
the run that produced this message will get past it once an open billing account
is available:

```bash
gcloud billing accounts list
```

If that prints nothing, there is no billing account on the login and one has to
be created at `https://console.cloud.google.com/billing`. The free trial covers
this study, which costs about a dollar of instance time.

### The run reported `APIs enabled` and then failed anyway

It cannot any more. Enabling Compute Engine is now checked rather than
attempted: a failure there ends the run at that line, with the three things that
cause it named in order of how often they are the cause. IAP and OS Login are
still allowed to fail, because a run can be finished without them.

### `Compute Engine reports itself enabled ... but will not answer`

Enabling an API is eventually consistent, and Compute Engine can take a minute
or two to learn about a project it has just been switched on for. The driver
waits up to five minutes for the first compute call to succeed. Beyond that,
running the same command again is the whole remedy.

### `could not create the project`

Only one kind of refusal earns another name. An ID that is already in use gets a
random six-character suffix and another attempt; a quota, a permission or an
organisation policy says the same thing about every ID there is, so it is
reported at the first refusal rather than after two more names that were never
the problem. What Google said is printed above the error, and up to five
projects this account could use instead are listed under it.

A project quota is the usual cause, and deleting projects does not relieve it:
Google keeps a shut-down project for thirty days and counts it the whole time.
The driver lists what can be restored and what is already usable, so the way
through is normally one of those rather than a wait or a quota request:

```bash
bash ~/Networks-HPC/linerate/mac/linerate go --project one-of-those
```

### `does not have permission to access projects instance ... (or it may not exist)`

The parenthesis is the whole message. Resource Manager gives exactly this answer
for a project ID that has never existed and for one owned by somebody else, and
will not distinguish them: answering otherwise would turn `describe` into a way
of enumerating which IDs in a global namespace are taken.

Nothing has to be done about it. `go` reads it as *unknown*, which is what it
means, and settles it by trying to create the project — `projects create` does
say, in as many words, whether an ID is already in use. If it is, a random
six-character suffix is added and the ID actually created is printed.

An earlier revision of this driver read the words *does not have permission* as
evidence of an owner and stopped, inviting a new name that would be refused for
the same reason. A project ID containing a Unix timestamp was reported as
belonging to a stranger. That is fixed; it is recorded here because the message
itself has not changed and will be seen in `~/.linerate/driver.log`.

### `could not create the project` after four names in a row

Four consecutive IDs reported as already in use is improbable enough to be a
symptom. Pass one of your own:

```bash
bash ~/Networks-HPC/linerate/mac/linerate go --project linerate-$RANDOM$RANDOM
```

### `could not link billing`

Three attempts are made before this is reported, and what Google said is printed
above the error rather than left in the log. In order of how often it is the
cause: you can see the billing account but are not an administrator of it, so
you cannot attach a project to it; or the project was created or restored
moments ago and the billing service has not caught up, in which case running the
same command again is the whole remedy; or the account is closed or has no valid
payment method.

To link it by hand instead:
`https://console.cloud.google.com/billing/linkedaccount?project=YOUR-PROJECT`.

### The project is pending deletion

`go` restores it and carries on. It does not stop, and it does not invent a new
ID unless the restore is refused.

Restoring first is deliberate. Google keeps a shut-down project for thirty days
before removing it for good, and counts it against the project quota for the
whole of that time, so restoring one costs no quota that has not already been
spent while creating another spends a slot the account may not have. Deleting
projects to make room does not make room.

Two consequences follow, and `go` handles both:

- Shutting a project down **disconnects its billing account, and restoring the
  project does not reconnect it.** The project comes back unbilled, which is
  exactly the state in which Compute Engine cannot be enabled.
- A project restored a moment ago refuses the billing link for the first minute
  or two. Five attempts are made over about two minutes. Google notes that a
  restored project can take considerably longer than that to become fully
  functional, so if all five are refused, the same command a quarter of an hour
  later is a real remedy rather than a hopeful one.

To see the whole picture before running anything:

```bash
bash ~/Networks-HPC/linerate/mac/linerate cloud
```

It lists every project awaiting deletion, says how many quota slots they are
holding, and creates nothing.

### A quota message when the instances are created

The message names the metric. Two `c3-standard-4` need eight vCPUs in one
region. A quota of zero for one machine family is not fatal: the driver tries
`n2-standard-4`, `n2d-standard-4`, `c2-standard-4` and `t2d-standard-4` in turn, says which one it
fell back to, gives both nodes the same one, and records it in the dataset. Every
family in that list supports gVNIC, which the study depends on; `e2` is
deliberately not among them. If all four are refused, either request the increase
under IAM & Admin → Quotas, or pass a different `--zone`. Do not drop below four vCPUs: the data plane, the io_uring
submission poller and the load generator each want a core, and a smaller
instance makes E5 measure scheduling instead of queueing.

### `ssh` never connects

`up` enables `iap.googleapis.com` and opens tcp:22 from the IAP range to the
tagged instances, so this should not happen. If it does, read the serial
console:

```bash
source ~/.linerate/state.env && gcloud compute instances get-serial-port-output "$LR_NODE_B" --zone="$LR_ZONE" --project="$LR_PROJECT"
```

### `lost contact with linerate-b for twenty consecutive polls`

The tunnel is down; the sweep is not. It is detached and still running.
`./mac/linerate watch` re-attaches to it and prints the log from the beginning.

### The build fails on a node

`~/.linerate/driver.log` has the output. Then go and look, and run by hand the
two steps `up` runs:

```bash
source ~/.linerate/state.env && gcloud compute ssh "$LR_NODE_B" --project="$LR_PROJECT" --zone="$LR_ZONE" --tunnel-through-iap --command="cd ~/linerate && sudo bash cloud/bootstrap.sh && bash setup.sh"
```

### The self-test fails

Nothing measured from this build would mean anything, so the run stops. Run
`./build/lr_selftest` on the node directly; each check prints pass or fail with
what it was checking. A cipher cross-check failure means the build produced
wrong ciphertext — report the compiler and flags, not a performance number.

### `E1: anchor measurement failed`

The sweep re-spawns itself to vary the hardware-crypto mask, and the child
produced nothing. Usually the binary path: run from the repository root, not
from `build/`. Test the child directly:

```bash
./build/lr_bench e1-unit --cipher=aes-256-gcm --payload=512 --packets=8000
```

### `SWEEP VOID` after the anchor check

The anchor moved more than ten per cent between the start and the end. The
machine changed underneath the measurement — a neighbouring tenant, a thermal
event, a governor change. Re-run. If it happens repeatedly, the host is too busy
to measure on; `tools/doctor.py` says which knobs are not set.

### `hardware-crypto mask did not take effect in the child process`

The capability variable did not reach libcrypto. Check the architecture is one
the mask exists for (x86-64 or `aarch64`) and that the child sees it:

```bash
OPENSSL_ia32cap=~0x200000200000000:~0x60000000000 openssl speed -evp aes-256-gcm -seconds 1
```

Bulk AES should fall by more than an order of magnitude. If it does not, the
mask is not supported by this build of OpenSSL and E1's masked arm records
itself as unavailable rather than reporting a null result as a finding.

### `merge refuses: fragments came from more than one machine`

Correct behaviour, and `fetch` avoids it: a `results/` left by an earlier run is
moved to `results-previous-<date>` before the new fragments land, and the
archive itself contains none. Fragments carry the environment that produced
them, and mixing two machines' fragments makes every cross-experiment comparison
wrong invisibly.
Re-run the sweep on one machine, or pass `--allow-mixed` if the mixture is
deliberate — the merged file then records `mixed_sources: true`.

### `E5 … interpretable: false`

The load generator shares a core with the data plane, so the delays are
scheduling latency rather than queueing delay. The two-node topology
`./mac/linerate run` sets up avoids this; seeing it there means the generator
did not come up on node A, and `/tmp/linerate-loadgend.log` on that node says
why. E9 refuses to fit on such a dataset, which is why the admission macros
render as em dashes.

### `sqpoll_contends_for_cpu: true`

Fewer than two usable cores, so the io_uring submission poller competes with the
data plane rather than bypassing anything. The difference between `posix` and
`uring-sqpoll` bounds nothing on such a host and the report says so. Use an
instance with at least four vCPUs — the default already does.

### `check_numbers.py` reports a literal in the prose

Someone typed a measured number into a section instead of using a macro. Add the
quantity to `build_macros()` in `python/lr/report.py` and reference it. If the
numeral is structural rather than measured — a count of factor levels, an RFC
number — add it to `ALLOWED_LITERALS` with the reason.

### `check_wiring.py` reports a missing macro or fragment

Something was renamed on one side of a boundary. The message names both sides.
This is the check that exists because the failure mode is silent: a missing key
reads as zero and the report prints a plausible wrong number.

## First run, on one Linux machine

```bash
./setup.sh                            # configure, build, self-test
python3 tools/doctor.py               # what will and will not run here
./run.sh --quick
./run.sh
```

`sudo ./run.sh --full` adds the tiers, the chain and the network conditions.
`make help` lists the same operations as targets.

## Running one experiment

```bash
./build/lr_bench --help
./build/lr_bench e3 --out=results/raw --reps=11 --verbose
python3 tools/merge_results.py --raw results/raw --out results/results.json
python3 python/make_report.py
```

## Two nodes by hand

On the generating node:

```bash
./build/lr_loadgend --port=9099
```

On the measuring node:

```bash
./run.sh --full --peer=<generator IP> --local=<this node's IP>
```

Both addresses must be on the interface being measured, not loopback. The
harness records `nic_driver`, so a run over the wrong interface is visible
afterwards.

## Cleaning up

```bash
./mac/linerate down                      # the instances, and the billing
make clean                               # the build
make distclean                           # the build, the results, the report
rm -f ~/.linerate/state.env              # the session
gcloud projects delete <your project>    # only if you want the project gone too
```

`distclean` removes `results/`, so archive anything worth keeping first.

## What changed in this revision

The run that produced the log this revision was made from stopped at
`load generator did not come up on linerate-a`, with an empty
`/tmp/linerate-loadgend.log`. Seventeen files changed; the reasoning behind each
is in the file it changed, and this is the index.

| File | What was wrong |
| --- | --- |
| `cloud/nodectl.sh` | `systemd-run` returns when a unit is accepted, not when its process runs. A zero was read as *running*, the `setsid` fallback only covered a unit that was *refused*, and `--collect` discarded the evidence. Every spawn is now verified, falls through on failure, and records why. |
| `src/apps/lr_loadgend.cpp` | `signal(2)` sets `SA_RESTART`, so `SIGTERM` never interrupted `accept()`; `listen()` was unchecked; a failed echo bind was silent; a unit that could not start reported zero packets rather than a reason. |
| `cloud/remote-sweep.sh` | The core loop rewrote `e5_latency.json` after the netem conditions had been folded into it, so a full sweep reached the analysis with one condition instead of five and E9 declined to fit. |
| `python/make_report.py` | A failed `latexmk` leaves state it believes next time, so installing the missing class was not enough to make the next run work. |
| `tools/merge_results.py` | A fragment written while `environment.json` was absent looked like a second machine, and the merge refused with a confident wrong diagnosis. |
| `tools/doctor.py` | Reported no interfaces on a host without `iproute2`, while the harness on the same host reported `eth0`. |
| `tools/check_numbers.py` | Two whitelist entries were duplicate dict keys, so their stated reasons were silently discarded. |
| `mac/linerate`, `mac/lib/vm.sh` | The failure message named a log on an instance the next prompt deleted. The diagnosis is now fetched and printed while the node still exists, and `start-sweep`'s answer is read rather than discarded. |
| `include/lr/ctrl.hpp`, `src/core/ctrl.cpp`, `src/bench/bench.cpp`, `src/bench/exp_e5_latency.cpp` | Carry the generator's reason for a unit that produced nothing, so a wrong cipher name stops reading as a measured zero. |
| `RUNBOOK.md`, `docs/environment.md`, `docs/limitations.md`, `docs/mac-workflow.md` | Descriptions of mechanisms that had changed. |

Verified on x86-64 Linux: clean `setup.sh`, self-test, `run.sh --quick`, merge,
figures, macros, PDF, `check_wiring.py`, `check_numbers.py` (113 macros). The
start path was exercised with the control port deliberately occupied, which now
puts `bind 9099: Address already in use` in the log and on the operator's screen
instead of leaving both empty.
