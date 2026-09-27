from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

PAGE = 4096
INFLIGHT = "inflight"
COMPLETED = "completed"


@dataclass
class Range:
    start: int
    end: int
    seq: int
    generation: int
    state: str
    concurrent_overlap: bool = False


@dataclass
class Shadow:
    value: bytes
    seq: int
    generation: int


class Model:
    """
    Deterministic model of H3-B5's bounded per-pagefile write history.

    The important safety rule is conservative publication:
      * no newer overlapping write may exist;
      * no older overlapping write may still be in flight;
      * no unknown/dropped in-flight write may remain outstanding.
    """

    def __init__(self, history_slots: int = 8):
        self.history_slots = history_slots
        self.history: Dict[str, List[Range]] = {}
        self.floor: Dict[str, int] = {}
        self.dropped_inflight: Dict[str, int] = {}
        self.seq = 1
        self.generation = 1
        self.shadow: Dict[Tuple[str, int], Shadow] = {}

    def begin_write(self, pf: str, start: int, length: int) -> int:
        self.seq += 1
        seq = self.seq
        hist = self.history.setdefault(pf, [])
        self.floor.setdefault(pf, 0)
        self.dropped_inflight.setdefault(pf, 0)

        if len(hist) == self.history_slots:
            dropped = hist.pop(0)
            self.floor[pf] = max(self.floor[pf], dropped.seq)
            if dropped.state == INFLIGHT:
                self.dropped_inflight[pf] += 1

        new_end = start + length
        concurrent = False

        for existing in hist:
            if (
                existing.state == INFLIGHT
                and existing.start < new_end
                and existing.end > start
            ):
                existing.concurrent_overlap = True
                concurrent = True

        hist.append(
            Range(
                start,
                new_end,
                seq,
                self.generation,
                INFLIGHT,
                concurrent,
            )
        )
        return seq

    def _find(self, pf: str, seq: int) -> Optional[Range]:
        for r in self.history.get(pf, []):
            if r.seq == seq:
                return r
        return None

    @staticmethod
    def _overlaps(r: Range, offset: int) -> bool:
        return r.start < offset + PAGE and r.end > offset

    def _mark_complete(self, pf: str, seq: int) -> bool:
        if seq <= self.floor.get(pf, 0):
            # A post-completion arriving after its history record was dropped
            # retires one conservative unknown-write barrier.
            if self.dropped_inflight.get(pf, 0) > 0:
                self.dropped_inflight[pf] -= 1
            return False

        own = self._find(pf, seq)
        if own is None:
            return False

        own.state = COMPLETED
        return True

    def _allows_publish(
        self,
        pf: str,
        offset: int,
        seq: int,
    ) -> Optional[int]:
        if self.dropped_inflight.get(pf, 0) != 0:
            return None

        if seq <= self.floor.get(pf, 0):
            return None

        own = self._find(pf, seq)
        if (
            own is None
            or own.state != COMPLETED
            or own.generation != self.generation
            or own.concurrent_overlap
        ):
            return None

        for r in self.history.get(pf, []):
            if r.seq == seq or not self._overlaps(r, offset):
                continue

            if r.seq > seq:
                return None

            if r.seq < seq and r.state == INFLIGHT:
                return None

        return own.generation

    def _allows_verify(self, pf: str, offset: int, seq: int) -> bool:
        if self.dropped_inflight.get(pf, 0) != 0:
            return False

        if seq <= self.floor.get(pf, 0):
            return False

        own = self._find(pf, seq)
        if (
            own is None
            or own.state != COMPLETED
            or own.generation != self.generation
        ):
            return False

        for r in self.history.get(pf, []):
            if (
                r.seq > seq
                and self._overlaps(r, offset)
            ):
                return False

        return True

    def complete_write(
        self,
        pf: str,
        seq: int,
        pages: Dict[int, bytes],
    ) -> None:
        retained = self._mark_complete(pf, seq)
        if not retained:
            return

        for offset, value in pages.items():
            gen = self._allows_publish(pf, offset, seq)
            if gen is not None:
                self.shadow[(pf, offset)] = Shadow(value, seq, gen)

    def complete_failed_write(self, pf: str, seq: int) -> None:
        # Failed/short writes do not publish data, but their pre-write ranges
        # remain a conservative invalidation boundary.
        self._mark_complete(pf, seq)

    def read(self, pf: str, offset: int, actual: bytes) -> str:
        s = self.shadow.get((pf, offset))
        if s is None or s.generation != self.generation:
            return "untracked"

        if not self._allows_verify(pf, offset, s.seq):
            return "untracked"

        return "match" if actual == s.value else "mismatch"

    def reset(self) -> None:
        self.generation += 1
        self.shadow.clear()


class AliasModel(Model):
    """Multiple FILE_OBJECT-like handles mapped to canonical pagefile IDs."""

    def __init__(self, history_slots: int = 8):
        super().__init__(history_slots)
        self.aliases: Dict[str, str] = {}

    def alias(self, handle: str, paging_file: str) -> None:
        self.aliases[handle] = paging_file

    def begin_write_handle(
        self,
        handle: str,
        start: int,
        length: int,
    ) -> int:
        return self.begin_write(self.aliases[handle], start, length)

    def complete_write_handle(
        self,
        handle: str,
        seq: int,
        pages: Dict[int, bytes],
    ) -> None:
        self.complete_write(self.aliases[handle], seq, pages)

    def read_handle(self, handle: str, offset: int, actual: bytes) -> str:
        return self.read(self.aliases[handle], offset, actual)


def page(ch: int) -> bytes:
    return bytes([ch]) * PAGE


def main() -> None:
    # 1. Basic write -> read match.
    m = Model()
    s1 = m.begin_write("C", 0, PAGE)
    m.complete_write("C", s1, {0: page(1)})
    assert m.read("C", 0, page(1)) == "match"

    # 2. Non-overlapping newer write must not invalidate an old sample.
    s2 = m.begin_write("C", 8 * PAGE, PAGE)
    m.complete_write("C", s2, {8 * PAGE: page(2)})
    assert m.read("C", 0, page(1)) == "match"

    # 3. Newer overlapping write immediately invalidates old sample.
    s3 = m.begin_write("C", 0, PAGE)
    assert m.read("C", 0, page(1)) == "untracked"

    # 4. Older completion after newer overlapping pre-write cannot republish.
    m.complete_write("C", s1, {0: page(1)})
    assert m.read("C", 0, page(1)) == "untracked"

    # 5. Newest completed overlapping write becomes a valid sample once the
    # earlier writer is no longer in flight.
    m.complete_write("C", s3, {0: page(3)})
    assert m.read("C", 0, page(3)) == "match"

    # 6. A later non-overlap write preserves that match.
    s4 = m.begin_write("C", 20 * PAGE, PAGE)
    m.complete_write("C", s4, {20 * PAGE: page(4)})
    assert m.read("C", 0, page(3)) == "match"

    # 7. Failed newer overlap conservatively invalidates old sample.
    failed = m.begin_write("C", 0, PAGE)
    m.complete_failed_write("C", failed)
    assert m.read("C", 0, page(3)) == "untracked"

    # 8. Different pagefiles are independent.
    d1 = m.begin_write("D", 0, PAGE)
    m.complete_write("D", d1, {0: page(7)})
    assert m.read("D", 0, page(7)) == "match"

    # 9. Reset invalidates previous-generation samples.
    m.reset()
    assert m.read("D", 0, page(7)) == "untracked"

    # 10. Completed history overflow never permits a stale match.
    n = Model(history_slots=4)
    a = n.begin_write("C", 0, PAGE)
    n.complete_write("C", a, {0: page(9)})
    for i in range(10):
        x = n.begin_write("C", (i + 10) * PAGE, PAGE)
        n.complete_write("C", x, {(i + 10) * PAGE: page(i)})
    assert n.read("C", 0, page(9)) == "untracked"

    # 11. Large newer write invalidates a sampled offset anywhere in its range.
    q = Model()
    z1 = q.begin_write("C", 10 * PAGE, PAGE)
    q.complete_write("C", z1, {10 * PAGE: page(5)})
    z2 = q.begin_write("C", 0, 100 * PAGE)
    assert q.read("C", 10 * PAGE, page(5)) == "untracked"
    q.complete_write(
        "C",
        z2,
        {
            0: page(6),
            PAGE: page(6),
            2 * PAGE: page(6),
            3 * PAGE: page(6),
        },
    )
    assert q.read("C", 10 * PAGE, page(5)) == "untracked"

    # 12. Out-of-order overlapping completions are deliberately untracked.
    # Newer completes while older is still in flight -> newer cannot publish.
    # Older then completes after a newer overlap exists -> older cannot publish.
    r = Model()
    old = r.begin_write("C", 0, PAGE)
    new = r.begin_write("C", 0, PAGE)
    r.complete_write("C", new, {0: page(2)})
    assert r.read("C", 0, page(2)) == "untracked"
    r.complete_write("C", old, {0: page(1)})
    assert r.read("C", 0, page(1)) == "untracked"
    assert r.read("C", 0, page(2)) == "untracked"

    # A later serialized write restores useful tracking.
    stable = r.begin_write("C", 0, PAGE)
    r.complete_write("C", stable, {0: page(3)})
    assert r.read("C", 0, page(3)) == "match"

    # 13. The inverse callback order is also untracked.  The old write
    # completes first, but both writes overlapped while in flight, so callback
    # ordering cannot be used as proof of final storage order.
    r2 = Model()
    old2 = r2.begin_write("C", 0, PAGE)
    new2 = r2.begin_write("C", 0, PAGE)
    r2.complete_write("C", old2, {0: page(1)})
    r2.complete_write("C", new2, {0: page(2)})
    assert r2.read("C", 0, page(1)) == "untracked"
    assert r2.read("C", 0, page(2)) == "untracked"

    # 14. Multiple FILE_OBJECT aliases for one pagefile share write history.
    aliases = AliasModel()
    aliases.alias("C-object-1", "C-pagefile")
    aliases.alias("C-object-2", "C-pagefile")
    ca = aliases.begin_write_handle("C-object-1", 0, PAGE)
    aliases.complete_write_handle("C-object-1", ca, {0: page(1)})
    cb = aliases.begin_write_handle("C-object-2", 0, PAGE)
    assert aliases.read_handle("C-object-1", 0, page(1)) == "untracked"
    aliases.complete_write_handle("C-object-2", cb, {0: page(2)})
    assert aliases.read_handle("C-object-1", 0, page(2)) == "match"

    # 15. Non-overlap alias writes preserve unrelated samples.
    keep = aliases.begin_write_handle("C-object-1", 8 * PAGE, PAGE)
    aliases.complete_write_handle(
        "C-object-1",
        keep,
        {8 * PAGE: page(3)},
    )
    other = aliases.begin_write_handle("C-object-2", 20 * PAGE, PAGE)
    aliases.complete_write_handle(
        "C-object-2",
        other,
        {20 * PAGE: page(4)},
    )
    assert aliases.read_handle(
        "C-object-1",
        8 * PAGE,
        page(3),
    ) == "match"

    # 16. Aliases on different canonical identities remain independent.
    aliases.alias("D-object-1", "D-pagefile")
    d = aliases.begin_write_handle("D-object-1", 0, PAGE)
    aliases.complete_write_handle("D-object-1", d, {0: page(9)})
    assert aliases.read_handle("D-object-1", 0, page(9)) == "match"
    assert aliases.read_handle("C-object-1", 0, page(2)) == "match"

    # 17. If an in-flight record falls out of the bounded ring, all publication
    # is blocked until that unknown write finally completes.
    h = Model(history_slots=4)
    very_old = h.begin_write("C", 0, PAGE)
    for i in range(4):
        seq = h.begin_write("C", (10 + i) * PAGE, PAGE)
        h.complete_write("C", seq, {(10 + i) * PAGE: page(i + 1)})
    assert h.dropped_inflight["C"] == 1

    blocked = h.begin_write("C", 50 * PAGE, PAGE)
    h.complete_write("C", blocked, {50 * PAGE: page(8)})
    assert h.read("C", 50 * PAGE, page(8)) == "untracked"

    # The late completion retires the unknown-write barrier but cannot publish
    # because its own history record is gone.
    h.complete_write("C", very_old, {0: page(7)})
    assert h.dropped_inflight["C"] == 0
    assert h.read("C", 0, page(7)) == "untracked"

    after_barrier = h.begin_write("C", 60 * PAGE, PAGE)
    h.complete_write("C", after_barrier, {60 * PAGE: page(6)})
    assert h.read("C", 60 * PAGE, page(6)) == "match"

    # 18. A dropped in-flight write on C must not block independent D tracking.
    d2 = h.begin_write("D", 0, PAGE)
    h.complete_write("D", d2, {0: page(5)})
    assert h.read("D", 0, page(5)) == "match"

    print("H3B_HISTORY_MODEL=PASS")
    print("cases=18")


if __name__ == "__main__":
    main()
