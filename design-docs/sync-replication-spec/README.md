# Valkey sync replication: Lean proofs

Lean 4 proofs for the synchronous replication design
(`design-docs/sync-replication.md`, proposed in
[valkey#5286](https://github.com/valkey-io/valkey/pull/5286)).

This branch has only the Lean part. The TLA+ models these proofs encode
(`spec/Core.tla`, `spec/CoreAck.tla`, `spec/CoreRecon.tla`,
`spec/CoreRestart.tla`), the TLC results and the findings are on the
[`sync-repl-formal-spec`](https://github.com/jjuleslasarte/valkey/tree/sync-repl-formal-spec)
branch.

## What is proven

With the proposed fixes for F1 (reconcile against the inherited log), F2
(fence held replies by primaryship) and F5 (a restarted primary rebuilds its
sync set), for any number of nodes, log length, configuration changes and
restarts, and every min-sync-replicas value:
- no acknowledged write is lost (`RestartProof.ackSafety`);
- every authoritative node holds every committed and acknowledged write
  (`RestartProof.safety`, `RestartProof.ackedHeld`);
- with min-sync-replicas >= 1, every committed write is on at least two
  authoritative nodes (`RestartProof.commit_two_copies`).

The one assumption is the design's survival condition: whenever a node
restarts without its data, another authoritative node keeps its data.
`ReconProof`, `AckProof` and `CoreProof` are derived from `RestartProof` by
embedding the smaller models into it. `Lease/` proves the lease timing rules
(F11, F12). `XCheck/` checks the Lean step functions against TLC's full state
graphs. Not covered: forced failover.

All theorems use only Lean's standard axioms (`propext`,
`Classical.choice`, `Quot.sound`), with no `sorry`. See
[lean/LEAN.md](lean/LEAN.md) for every theorem, its assumptions and how to
reproduce it.

## Build

Lean 4.35, core library only. `tools/lean.sh` runs the toolchain in a
container; unpack the toolchain into `tools/elan/` first.

```
cd lean
../tools/lean.sh lake build
../tools/lean.sh lake env lean Check.lean   # prints the axioms
```

The cross-checks (`lake exe xcheck`, `lake exe xcheck_ack`) need TLC state
graphs generated from the spec branch; `lean/LEAN.md` gives the commands.
