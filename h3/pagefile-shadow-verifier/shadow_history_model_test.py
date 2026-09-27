from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

PAGE = 4096

@dataclass
class Range:
    start: int
    end: int
    seq: int
    generation: int

@dataclass
class Shadow:
    value: bytes
    seq: int
    generation: int

class Model:
    def __init__(self, history_slots: int = 8):
        self.history_slots = history_slots
        self.history: Dict[str, List[Range]] = {}
        self.floor: Dict[str, int] = {}
        self.seq = 1
        self.generation = 1
        self.shadow: Dict[Tuple[str, int], Shadow] = {}

    def begin_write(self, pf: str, start: int, length: int) -> int:
        self.seq += 1
        seq = self.seq
        hist = self.history.setdefault(pf, [])
        self.floor.setdefault(pf, 0)
        if len(hist) == self.history_slots:
            dropped = hist.pop(0)
            self.floor[pf] = max(self.floor[pf], dropped.seq)
        hist.append(Range(start, start + length, seq, self.generation))
        return seq

    def _allows(self, pf: str, offset: int, seq: int) -> Optional[int]:
        if seq <= self.floor.get(pf, 0):
            return None
        hist = self.history.get(pf, [])
        own = None
        page_end = offset + PAGE
        for r in hist:
            if r.seq == seq:
                own = r
            elif r.seq > seq and r.start < page_end and r.end > offset:
                return None
        if own is None or own.generation != self.generation:
            return None
        return own.generation

    def complete_write(self, pf: str, seq: int, pages: Dict[int, bytes]) -> None:
        for offset, value in pages.items():
            gen = self._allows(pf, offset, seq)
            if gen is not None:
                self.shadow[(pf, offset)] = Shadow(value, seq, gen)

    def read(self, pf: str, offset: int, actual: bytes) -> str:
        s = self.shadow.get((pf, offset))
        if s is None or s.generation != self.generation:
            return "untracked"
        if self._allows(pf, offset, s.seq) is None:
            return "untracked"
        return "match" if actual == s.value else "mismatch"

    def reset(self) -> None:
        self.generation += 1
        self.shadow.clear()


def page(ch: int) -> bytes:
    return bytes([ch]) * PAGE


def main() -> None:
    # 1. Basic write -> read match.
    m = Model()
    s1 = m.begin_write("C", 0, PAGE)
    m.complete_write("C", s1, {0: page(1)})
    assert m.read("C", 0, page(1)) == "match"

    # 2. Non-overlapping newer write must not invalidate old sample.
    s2 = m.begin_write("C", 8 * PAGE, PAGE)
    m.complete_write("C", s2, {8 * PAGE: page(2)})
    assert m.read("C", 0, page(1)) == "match"

    # 3. Newer overlapping write immediately invalidates older sample,
    # even before the newer write completes.
    s3 = m.begin_write("C", 0, PAGE)
    assert m.read("C", 0, page(1)) == "untracked"

    # 4. Older completion arriving after newer overlapping pre-write cannot republish.
    m.complete_write("C", s1, {0: page(1)})
    assert m.read("C", 0, page(1)) == "untracked"

    # 5. Newest completed overlapping write becomes authoritative sample.
    m.complete_write("C", s3, {0: page(3)})
    assert m.read("C", 0, page(3)) == "match"

    # 6. A later non-overlap write still preserves that match.
    s4 = m.begin_write("C", 20 * PAGE, PAGE)
    m.complete_write("C", s4, {20 * PAGE: page(4)})
    assert m.read("C", 0, page(3)) == "match"

    # 7. Failed/abandoned newer overlapping write conservatively invalidates.
    _failed = m.begin_write("C", 0, PAGE)
    assert m.read("C", 0, page(3)) == "untracked"

    # 8. Different pagefiles are independent.
    d1 = m.begin_write("D", 0, PAGE)
    m.complete_write("D", d1, {0: page(7)})
    assert m.read("D", 0, page(7)) == "match"

    # 9. Reset invalidates previous-generation samples.
    m.reset()
    assert m.read("D", 0, page(7)) == "untracked"

    # 10. History overflow never permits a stale match.
    n = Model(history_slots=4)
    a = n.begin_write("C", 0, PAGE)
    n.complete_write("C", a, {0: page(9)})
    for i in range(10):
        x = n.begin_write("C", (i + 10) * PAGE, PAGE)
        n.complete_write("C", x, {(i + 10) * PAGE: page(i)})
    assert n.read("C", 0, page(9)) == "untracked"

    # 11. Large newer write invalidates an old sampled offset anywhere in its range,
    # without needing per-page invalidation work.
    q = Model()
    z1 = q.begin_write("C", 10 * PAGE, PAGE)
    q.complete_write("C", z1, {10 * PAGE: page(5)})
    z2 = q.begin_write("C", 0, 100 * PAGE)
    assert q.read("C", 10 * PAGE, page(5)) == "untracked"
    q.complete_write("C", z2, {0: page(6), PAGE: page(6), 2 * PAGE: page(6), 3 * PAGE: page(6)})
    assert q.read("C", 10 * PAGE, page(5)) == "untracked"

    # 12. Out-of-order completion of overlapping writes stays conservative.
    r = Model()
    old = r.begin_write("C", 0, PAGE)
    new = r.begin_write("C", 0, PAGE)
    r.complete_write("C", new, {0: page(2)})
    r.complete_write("C", old, {0: page(1)})
    assert r.read("C", 0, page(2)) == "match"
    assert r.read("C", 0, page(1)) == "mismatch"

    print("H3B_HISTORY_MODEL=PASS")
    print("cases=12")


if __name__ == "__main__":
    main()
