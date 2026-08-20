# linerate

## 📄 [Full report (PDF)](./REPORT.pdf)
Successful local run on macOS

Measuring the per-packet cost of an encrypted UDP data plane, and the payload
size at which that cost stops being about the packet and starts being about the
bytes.

---

## Résumé

Un point de terminaison de tunnel chiffré paie deux coûts par paquet. L'un ne
dépend pas de la taille : les appels système, l'analyse de l'en-tête, la fenêtre
anti-rejeu, la dérivation du nonce, l'initialisation du chiffrement. L'autre est
proportionnel à la charge utile : le chiffrement et le déchiffrement en masse, et
le trafic mémoire qui les alimente. En notant le premier `a` cycles et le second
`b` cycles par octet :

```
C(s) = a + b·s          s* = a / b
```

`s*` est la taille de charge utile où les deux moitiés s'équilibrent. En dessous,
un paquet coûte surtout d'être un paquet ; au-dessus, surtout de transporter ces
octets. C'est la grandeur que cette étude rapporte, parce que `a` et `b` varient
tous deux avec la fréquence d'horloge : leur rapport, lui, se transporte d'une
machine à l'autre, ce que ni l'un ni l'autre ne fait seul.

**Ce qui est mesuré.** Un plan de données qui déchiffre puis rechiffre — la
topologie réelle d'un point de terminaison reliant deux associations de
sécurité. Quatre facteurs sont variés : le chiffrement (quatre niveaux), la
présence des instructions cryptographiques matérielles, le transport (quatre
niveaux dont les taux d'appels système diffèrent par construction), et la taille
de la charge utile.

**Deux implémentations de chaque chiffrement.** AES-256-GCM et
ChaCha20-Poly1305 sont mesurés à la fois via OpenSSL et via des implémentations
écrites pour cette étude : AES-NI avec un GHASH PCLMULQDQ et une boucle interne
fusionnée en une seule passe sur x86-64, AES ARMv8 avec PMULL sur `aarch64`,
ChaCha20 vectorisé en AVX2 ou NEON. L'auto-test vérifie les deux contre OpenSSL
octet par octet sur trente longueurs, dans les deux sens, avant toute mesure.
Disposer d'une seconde implémentation est ce qui sépare « ce chiffrement coûte
*x* » de « la version de ce chiffrement dans cette bibliothèque coûte *x* ».

**Le même raisonnement à trois autres frontières.** À la frontière du noyau, un
chemin de soumission qui n'émet aucun appel système mesure directement ce que les
courbes de traitement par lots ne pouvaient que borner. À la frontière
d'isolation, chaque bac à sable supplémentaire ajoute un coût fixe et aucun coût
par octet, si bien que le prix du chaînage de fonctions réseau virtualisées
retombe entièrement sur les petits paquets. Au bus, le coût fixe de lancement et
le terme de transfert déterminent ensemble une taille de lot en dessous de
laquelle un accélérateur ne peut pas gagner, quelle que soit la vitesse de son
noyau de calcul.

**Discipline expérimentale.** Onze réplicats par point, médiane et intervalle par
bootstrap, ordre de balayage entrelacé et graine enregistrée, point d'ancrage
mesuré avant et après chaque balayage — un balayage dont l'ancrage a dérivé est
déclaré nul plutôt que rapporté. Chaque contrôle porte une prédiction écrite
*avant* la mesure : un contrôle qui ne peut pas échouer est un ornement. Une
machine incapable de tenir une mesure n'a pas le droit d'en écrire une, et cette
règle est appliquée dans le code.

**Intégrité du rapport.** Aucun rapport n'est livré avec cette archive : ni
`results/`, ni figures, ni PDF n'existent avant qu'une exécution ne les
produise. Aucun nombre mesuré n'apparaît littéralement dans la prose : chaque
grandeur est une macro engendrée depuis `results.json`.
`tools/check_numbers.py` refuse de valider si un chiffre nu apparaît là où une
macro devrait être, et recalcule un échantillon de macros depuis le jeu de
données. Chaque expérience écrit son fragment même lorsqu'elle n'a pas pu
s'exécuter, avec la raison ; le jeu de données est donc toujours complet et le
rapport indique ce qui manquait au lieu de laisser un trou.

**Reproduire.** Une seule commande, depuis un Mac : `./mac/linerate go
--project <votre-identifiant>`. Elle vérifie la machine locale, supprime les
autres instances du compte Google — une machine virtuelle voisine partage le
cache et la bande passante mémoire, et ce qu'elle fait se retrouve ici sous
forme de variance —, crée le projet et deux instances, compile sur les deux,
exécute un balayage de deux minutes puis le balayage complet, rapatrie le jeu de
données, compose le rapport et détruit les instances. Elle pose une seule
question, au début. Le détail est dans `RUNBOOK.md`.

**Le seul prérequis que cette étude ne peut pas inventer** est un compte de
facturation Google ouvert. Un projet peut exister sans en avoir un ; un projet
sans facturation ne peut pas activer Compute Engine, et tout appel ultérieur
répond alors `The resource 'projects/...' was not found` — un message qui parle
du projet pour une condition qui ne le concerne pas. La facturation est donc
établie pour tout projet, existant ou neuf, avant qu'on ne demande quoi que ce
soit à Compute Engine. Pour voir ce dont vous disposez, sans rien créer :

```bash
gcloud billing accounts list          # les comptes de facturation du login
./mac/linerate cloud                  # projet, facturation, API, réseau
```

`cloud` ne crée rien et ne facture rien : il indique laquelle des quatre
conditions manque. Il n'y a pas non plus d'identifiant de projet à trouver :
Google refuse de dire quels identifiants de son espace de noms global sont pris
— un identifiant qu'il refuse de montrer peut appartenir à un tiers ou n'avoir
jamais existé, et le refus est formulé de la même manière dans les deux cas.
`go` traite donc un identifiant invisible comme inconnu et tranche en le créant,
en ajoutant un suffixe aléatoire s'il s'avère déjà pris.

**Un projet arrêté n'est pas un projet libéré.** Google le conserve trente jours
et le compte pendant tout ce temps dans le quota qui limite le nombre de projets,
si bien que supprimer d'anciens projets pour faire de la place n'en fait pas.
`go` restaure donc un projet arrêté au lieu de le remplacer, et rétablit le
compte de facturation que l'arrêt avait déconnecté — l'arrêt le déconnecte, la
restauration ne le rétablit pas. Le crédit d'essai gratuit suffit — l'étude coûte environ un
dollar de temps d'instance.

---

## Getting started

### On a Mac, driving two cloud VMs

```bash
brew install --cask google-cloud-sdk && gcloud auth login
./mac/linerate go --project your-project-id
```

One command: it checks the Mac, deletes the other instances on the account so
that nothing else on the same hardware perturbs the measurement, creates the
project and two VMs, builds on both, runs a two-minute smoke sweep and then the
real one, brings the dataset back, typesets the report, and deletes the
instances so they stop billing. It asks once, at the start, and does not stop to
ask again. About an hour and a quarter.

The steps it is made of remain separate, because a run that fails halfway should
resume from the step that failed:

```bash
./mac/linerate cloud              # project, billing, APIs, network — creates nothing
./mac/linerate up                 # creates the project and two VMs, builds on both
./mac/linerate run --quick        # two minutes, exercises the whole path
./mac/linerate run --full         # the real sweep
./mac/linerate watch              # re-attach to a sweep already running
./mac/linerate fetch
./mac/linerate report
./mac/linerate down               # deletes the VMs so they stop billing
```

**You choose the project name.** Nothing in this repository identifies a Google
account, project or billing account; what you supply is kept in
`~/.linerate/state.env`, outside the repository, so it cannot be committed.

**You do not have to find a free project ID.** Google will not say which IDs in
its global namespace are taken; an ID it refuses to show you may belong to a
stranger or may never have existed, and the refusal reads the same either way.
`go` treats an unseen ID as unknown and settles it by creating it, adding a
random suffix and printing the result if that ID turns out to be in use.

**A project you shut down is not a project you have freed.** Google keeps it for
thirty days and counts it against the quota that limits how many projects may
exist, so deleting old projects to make room does not make room. `go` therefore
restores a shut-down project rather than replacing it, and relinks the billing
account that shutting it down disconnected.

**One precondition cannot be invented:** an open billing account. A project can
exist without one — a project left over from an earlier attempt usually does —
and an unbilled project cannot have Compute Engine enabled on it, after which
every compute call answers `The resource 'projects/...' was not found`, a
message about the project for a condition that has nothing to do with it.
Billing is therefore established for every project, existing or new, before
anything is asked of Compute Engine. `./mac/linerate cloud` reports which of the
four preconditions — project, billing, APIs, network — is the one missing, and
creates and charges nothing.

The sweep is detached from the connection that starts it — a transient `systemd`
unit on the node, read back in short polls — so a dropped tunnel costs a
reconnection rather than an hour of measurement, and every remote call runs
under a timeout, so a call that hangs becomes a message rather than a driver
that never returns.

The Mac never measures. `setup.sh` builds there and the self-test passes, but the
harness classifies macOS as a `development` host and refuses to write a
measurement from one — thread affinity on macOS is a hint rather than an
instruction, and a cycle count from an unpinned thread is worse than no number
because it looks like data.

### On a single Linux machine

```bash
./setup.sh
python3 tools/doctor.py           # what this host can and cannot measure
./run.sh --quick                  # two minutes
./run.sh                          # the core experiments
```

Only OpenSSL 3 is required. Everything else — liburing, MPI, CUDA, iperf3,
WireGuard, Docker, gVisor, `tc` — removes one experiment when absent, and the
dataset records which and why.

---

## What each experiment measures

| | question | needs |
|---|---|---|
| **E1** | `C(s) = a + b·s` and `s*`, for four ciphers with hardware crypto present and masked | nothing beyond OpenSSL |
| **E2** | strong and weak scaling of the shard pool, in-node and across nodes | OpenMP; MPI for the cross-node half |
| **E3** | what the system-call boundary contributes to `a`, measured rather than extrapolated | liburing |
| **E4** | the memory roof: sustained bandwidth divided by passes per payload byte | nothing |
| **E5** | tail latency against offered load, open-loop, across the knee | a peer node to be meaningful |
| **E6** | `openssl speed`, iperf3 and WireGuard as independent references | iperf3, WireGuard, a peer |
| **E7** | host / namespace / container / gVisor, and the cost per hop of a service chain | Docker, gVisor, root |
| **E8** | a GPU offload's fixed cost and where the bus stops it winning | CUDA |
| **E9** | can tail latency be predicted well enough to admit traffic? Against a static threshold, not against nothing | scikit-learn, several netem conditions |
| **controls** | five checks, each with a prediction recorded before it ran | nothing |

---

## Layout

```
include/lr/        headers: aead, proto, transport, worker, clock, stats, json,
                   platform, ctrl, simd_crypto
src/core/          the implementation, including simd_crypto.c — AES-256-GCM and
                   ChaCha20-Poly1305 written from scratch with runtime dispatch
                   across AES-NI/PCLMULQDQ, ARMv8 AES/PMULL, AVX2, NEON and
                   portable C
src/apps/          lr_selftest, lr_sink, lr_gen, lr_loadgend
src/bench/         the experiments and the sweep scaffolding
src/mpi/           the cross-node half of E2
src/cuda/          the GPU branch, built only where a toolkit exists
python/lr/         model (the fits), figures, report (macros and tables),
                   admission (E9)
tools/             doctor, merge_results, emit_fragment, merge_netem,
                   check_wiring, check_numbers
mac/               the driver — go, up, run, watch, fetch, report, down,
                   purge — and its gcloud/VM libraries
cloud/             bootstrap, nodectl (everything on a node that has to outlive
                   the ssh session that started it), remote-sweep, baselines,
                   wireguard, netem, isolate, virtualization, sfc, pin,
                   kernel-cmdline.md
report/            the LaTeX source; generated/ and figures/ do not exist until
                   a run has measured what goes in them
docs/              methodology, environment, schema, limitations, mac-workflow
```

Downloads and build environment go to `../linerate-env`, beside the repository
rather than inside it, so a clean checkout stays clean.

---

## The rules this project runs on

**A host that cannot hold a measurement may not write one.** Three classes —
`development`, `constrained`, `measurement` — enforced in code. A constrained
host measures and attaches its caveats to every dataset it writes.

**Every experiment writes a fragment, even when it cannot run.** With
`available: false` and a reason. So the dataset is always complete, the report
renders "not available, because X" instead of a missing figure, and a reader can
distinguish an experiment that was skipped from one nobody attempted.

**No report ships with this archive.** There is no dataset, no figure and no PDF
in the tree until a run produces them, so there is never a document here whose
numbers came from somewhere else. The figures are drawn from the dataset the run
measured, the macros are generated from the same file, and the PDF is typeset
last, out of both.

**No measured number is written by hand.** Every quantity in the report's prose
is a macro generated from `results.json`. `tools/check_numbers.py` fails when a
bare numeral appears where a macro belongs, and independently re-derives macros
from the dataset — including checking that every `s*` equals its own `a/b`.

**Controls carry their predictions.** Written before the measurement, stored
beside the outcome. One of them failed during development because its threshold
tested the wrong quantity; the control was rewritten rather than the threshold
loosened, and that is the whole point of having them.

**Correctness before timing.** The self-test checks both hand-written cipher
implementations against OpenSSL byte for byte, the replay window, the rekey
schedule, the handshake, every transport and the clock. `run.sh` stops if it
fails.

**Failed predictions are reported.** Where a result contradicted what the design
expected, the report says so rather than quietly dropping the prediction.

---

## Reading further

- `docs/methodology.md` — why each experimental choice went the way it did, and
  what would have gone wrong otherwise
- `docs/environment.md` — the two-node setup, why it has that shape, what
  `cloud/bootstrap.sh` tunes and why
- `docs/schema.md` — the contract between the harness and the analysis
- `docs/limitations.md` — what this study does not establish
- `docs/mac-workflow.md` — the driver in detail: what each command does, and
  why nothing it starts on a node is attached to the session that started it
- `cloud/kernel-cmdline.md` — kernel isolation, and why it is not sufficient on
  a public cloud

---

## Licence

See `LICENSE`.
