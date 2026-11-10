# Redwood

A testbed for cryptocurrency consensus, written in C with a GTK3 interface. Create a network of nodes, let them transact, make some of them cheat, and inspect every block, fork and rejection.
 
## Build

Requires a C11 compiler, `make`, `pkg-config` and GTK 3.

```
# Debian / Ubuntu
sudo apt install build-essential pkg-config libgtk-3-dev
# Fedora
sudo dnf install gcc make pkgconf-pkg-config gtk3-devel
# macOS (Homebrew)
brew install gtk+3 pkg-config
# Windows: use MSYS2 (mingw-w64-x86_64-gtk3, mingw-w64-x86_64-toolchain, make, pkgconf)

make          # builds ./redwood (GUI) and ./redwood-cli (headless)
./redwood
```

Developed and tested on Linux with GTK 3.24. The simulation core (`sha256.c`, `sim.c`) has no GUI dependency.

Launch options: `--run` (start immediately), `--coin 0|1|2`, `--page 0-4` (initial tab), `--cheat NODE:MASK` (repeatable).

## The three coins

| Coin | Consensus | Who produces blocks | Fork choice |
|---|---|---|---|
| Sequoia (SEQ) | Proof of Work | Anyone who finds a SHA-256 hash below the target. Real hashing, difficulty retargeted every 10 blocks. | Most cumulative work |
| Cedar (CDR) | Trusted authorities | Nodes flagged as authorities, in round-robin slots, with signed blocks. | Longest chain |
| Aspen (ASP) | Proof of Stake | A stake-weighted lottery per slot, seeded by the parent block hash. Stake is the account balance. | Longest chain |

## Using it

* **Network**: every node, its role, balance, tip, blocks created, how many of those ended up on the main chain, blocks it rejected, and reorgs. Select a node to edit it. Nodes, starting balances, authorities and chain parameters can be changed until the first tick; press Reset to edit them again. Hashpower, activity and cheats can be changed while running.
* **Run / Pause / Step / Reset**, plus a speed slider. The same seed and settings reproduce the same run.
* **Viewpoint**: block and transaction status is shown as the chosen node sees it (main chain, stale fork, rejected, not yet received, or private).
* **Blocks**: every block ever created, including forks, rejected blocks and blocks withheld by cheaters. Select one for its hash, parent, proof, transactions and the balances after it.
* **Transactions**: every payment with its confirmation depth, pending, or replaced (a conflicting payment with the same nonce won).
* **Balances**: balances on the heaviest chain over time.
* **Events**: a running log of rejections, reorgs, attacks and releases.

## Cheating behaviours

All nodes follow the rules by default. Tick any combination per node.

| Cheat | What the node does | Typical outcome |
|---|---|---|
| Selfish mining | Withholds blocks on a private chain and releases them to override honest blocks. | Pays off for PoW miners above roughly one third of the hashpower. |
| History rewrite | Pays a victim, waits for 3 confirmations, then publishes a secret longer fork that pays itself instead. | Succeeds against PoW with a hashpower majority. Against PoS it succeeds with a stake majority, because this model has no finality or checkpoints (a long-range attack). Fails against PoA unless authorities collude. |
| Split double-spend | Sends two payments with the same nonce to two halves of the network. | Only one confirms; the other shows as replaced. |
| Inflate reward | Claims 10x the block reward. | Rejected by every honest node; a PoW cheater wastes its hashpower. |
| Forge proof | Broadcasts blocks with a fake proof of work or a forged signature. | Rejected; logged with the real author. |
| Censor victim | Never includes the victim's transactions. | Other producers include them anyway. |
| Equivocate | Produces two blocks for one slot (PoA/PoS only). | Short forks that resolve at the next block. |

The attack frequency setting controls how often an idle cheater launches a double-spend or rewrite.

## Headless runs

```
./redwood-cli <coin 0|1|2> <ticks> [seed] [node:cheatmask[:hashpower[:balance]] ...]
./redwood-cli 0 8000 1 0:2:1500        # Sequoia, node 0 has a hashpower majority and rewrites history
```

Cheat mask bits: 1 selfish, 2 rewrite, 4 split, 8 inflate, 16 forge, 32 censor, 64 equivocate. The output includes a supply check (supply must equal initial balances plus rewards times height).

## Model and limits

* Time is in ticks. Gossip is a full mesh with configurable latency and jitter; blocks are fetched from the sender when a parent is missing.
* Each block stores the full ledger state after it, so any fork can be validated and compared without replaying history. Validity is computed once per block.
* Signatures are SHA-256(secret key, message) checked through a simulation oracle. A node can only sign with its own key, so it cannot forge another node's signature, but this stands in for public-key cryptography rather than implementing it.
* No fees, no mempool incentives, no network partitions beyond the split double-spend, no slashing or finality gadget for Aspen.
* Limits: 16 nodes, 120,000 blocks and 400,000 transactions per chain; the run halts and logs a message when reached.
