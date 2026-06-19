# Concurrency Control: Design Notes & Options

This document outlines the proposed concurrency control designs and architectural trade-offs for the `datalog` engine to transition beyond the single-lock model.

---

## 1. Current Model
* **Mechanism**: A single reader-writer lock (`pthread_rwlock_t`) encapsulated in the decoupled transaction manager (`s_transaction`).
* **Limitations**: 
  * Writes block all reads database-wide.
  * Reader threads can starve writers under read-heavy workloads.
  * Global lock contention limits scalability to a single core for mutations.

---

## 2. Option A: Subject/Predicate Partitioning (Fine-Grained Locking)
* **Goal**: Divide the database into $N$ partitions/buckets, each with its own localized index structures and reader-writer lock.
* **Mechanism**:
  1. Hashing the subject pointer `s` via a fast constant-time pointer hash: `part_idx = ( (uintptr_t)s * 2654435761U ) % NUM_PARTITIONS`.
  2. Route mutation and index lookup requests to partition `part_idx`.
* **Trade-offs**:
  * **Pros**: Disjoint writes to different subjects execute in parallel on $N$ independent locks, bypassing write contention.
  * **Cons (Query Join Complexity)**: Conjunctive queries and Leapfrog Triejoin (WCOJ) solvers frequently join facts *across* partitions (e.g. `parent(X, Y), parent(Y, Z)`). 
    * If indices are partitioned, a query cursor scanning a variable subject must sequentially scan all $N$ partitions.
    * Join solvers must manage lock acquisition and data assembly across multiple partitions, introducing deadlock risks unless locks are strictly ordered.

---

## 3. Option B: Read-Copy-Update (RCU) & Epoch-Based Reclamation (EBR)
* **Goal**: Support lock-free concurrent traversals for readers while allowing a single writer to update index structures.
* **Mechanism**:
  1. **Lock-Free Read Traversals**: Read-only queries traverse the skiplists and skip-tries without acquiring any locks.
  2. **Copy-on-Write / Atomic Linkage**: The writer thread performs mutations by copying nodes, modifying pointers, and using atomic compare-and-swap (CAS) instructions to link new nodes.
  3. **Epoch-Based Reclamation (EBR)**: Track active reader threads in epoch buckets (e.g., Epoch $E_0, E_1, E_2$). Deleted nodes are placed in a garbage queue and only freed once the epoch counter advances and all threads referencing that epoch exit.
* **Trade-offs**:
  * **Pros**: Readers incur zero locking overhead. Queries run at native single-threaded speeds concurrently with mutations.
  * **Cons**: High implementation complexity. Requires careful memory barriers and memory ordering flags (`memory_order_consume`/`memory_order_acquire`).

---

## 4. Option C: Multi-Version Concurrency Control (MVCC)
* **Goal**: Provide Snapshot Isolation (SI) where queries read a consistent snapshot of the database at a specific transaction start time, even while concurrent writers are committing updates.
* **Mechanism**:
  1. Append a transaction ID (timestamp) to facts or index nodes.
  2. Readers filter facts by comparing the node's creation/deletion timestamp with the reader's transaction start time.
  3. Keep historical versions of nodes until they are garbage-collected.
* **Trade-offs**:
  * **Pros**: Complete isolation between readers and writers. Queries never block.
  * **Cons**: Memory footprint increases due to multi-version nodes, requiring background compaction/vacuuming.
