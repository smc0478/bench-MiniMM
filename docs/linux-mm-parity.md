# Linux MM 대응표

## 비교 범위

miniMM은 Linux memory-management subsystem을 그대로 이식한 커널이 아니라,
검사 가능한 핵심 상태 전이를 C API로 재현한 사용자 공간 모델이다. 이 문서의
“대응”은 syscall ABI나 내부 자료구조가 같다는 뜻이 아니라, 공개 API에서
관찰하는 VMA·fault·COW·residency 계약을 가능한 범위에서 Linux 의미에 맞췄다는
뜻이다.

비교 기준은 Linux kernel의 [Memory Management concepts][linux-concepts]와
[Memory Management APIs][linux-mm-api], 그리고 Linux man-pages의
[`mmap(2)`][mmap], [`mremap(2)`][mremap], [`mincore(2)`][mincore],
[`mlock(2)`][mlock], [`mprotect(2)`][mprotect], [`msync(2)`][msync],
[`madvise(2)`][madvise]다. miniMM 전용 `EDIT`, note, capability service와
`minimm_mapping_copy()`에는 직접 대응하는 Linux syscall이 없다.

## 현재 맞춘 관찰 계약

| 영역 | miniMM의 현재 계약 | 대응 정도 |
| --- | --- | --- |
| VMA와 sparse page table | 4KiB page, lower canonical user range, 4-level sparse page table, 겹치지 않는 `[start,end)` VMA를 유지한다. compatible adjacent VMA는 보수적으로 합친다. | 핵심 개념 대응. 주소 폭, page 크기와 자료구조는 고정된 모델 값이다. |
| `MAP_FIXED` / `FIXED_NOREPLACE` | fixed는 겹치는 VMA/PTE를 교체하고 noreplace는 `ADDRESS_IN_USE`를 반환한다. | Linux의 관찰 가능한 교체/거부 구분에 대응한다. |
| `MAP_POPULATE` | fixed mapping과 조합할 수 있는 best-effort prefault hint다. 일부 page 준비가 실패해도 mmap과 VMA는 성공하며 나머지는 demand fault한다. | Linux처럼 populate 실패를 mmap transaction rollback으로 취급하지 않는다. |
| maximum protection | `maximum_protection == NONE`이면 mapping 종류와 note 권한에서 VM_MAY-style 상한을 계산한다. private file mapping은 read-only backing이어도 COW write 상한을 가질 수 있다. | Linux VMA의 현재 권한과 향후 허용 권한 분리를 모델링한다. |
| access와 fault | TLB miss 뒤 resident PTE를 찾은 경우 accessed/dirty 갱신과 refill만 하며 fault sequence를 늘리지 않는다. PTE 없음, nonresident page-in과 COW write처럼 resolution이 필요할 때 fault handler 경로를 탄다. 자동 접근과 명시적 fault의 VMA hole/permission denial도 handler 결과로 기록한다. | hardware fault 자체가 아니라 Linux의 page-fault 필요 여부를 명시적 상태 전이로 대응한다. |
| fork COW | VM_MAYWRITE에 해당하도록 maximum protection에 write/edit가 있는 private parent/child PTE만 effective write-protected COW로 바꾸고 첫 write/edit에서 새 4KiB frame으로 분리한다. 영구 read-only private PTE는 frame만 공유한다. child는 old/unlocked로 시작하며 shared-child 로컬 PTE는 clean이다. writable private child는 기존 PTE dirty bit를 유지할 수 있다. shared mapping은 backing/frame을 계속 공유한다. | Linux `is_cow_mapping()` 계열의 잠재 writable private mapping 구분과 child accessed/lock 분리에 대응한다. |
| 같은-space mapping COPY | 하나의 private VMA 전체를 새 주소에 복제하고 설치된 PTE frame을 공유한 뒤 양쪽을 COW로 만든다. destination은 clean, old, unlocked이며 source 상태는 유지한다. | Linux syscall이 아닌 miniMM 확장이다. fork와 같은 page-level COW 불변조건을 사용한다. |
| TLB coherence | generation lookup 없이 unmap, protect, COW, explicit page-out, remap, fork 시 page/range/full invalidation을 명시적으로 수행한다. eviction으로 nonresident가 된 cached translation은 다음 lookup에서 제거한다. | Linux의 shootdown이 필요한 상태 변화는 모델링하지만 CPU별 hardware TLB와 IPI는 없다. |
| residency query | `minimm_mincore()` 결과 bit 0은 resident 의미다. 로컬 PTE가 없어도 note/anonymous-shared backing cache에 resident frame이 있으면 bit 0을 설정한다. | Linux `mincore()`의 bit-0 ABI와 page-cache residency 관찰에 대응한다. |
| rich page query | nonresident PTE는 공개 `present == false`, PFN `MINIMM_PFN_NONE`이고 `translate`는 `NOT_FOUND`다. COW/dirty/lock 같은 software metadata는 유지할 수 있고 resident frame의 global cold 우선순위도 조회한다. | Linux-style present/resident 구분을 synthetic PFN API에 적용한 모델 계약이다. cold는 MiniMM 전용 진단값이다. |
| immutable space inspection | capture 시점의 통계·generation, 주소순 VMA와 설치된 sparse PTE/frame 관계를 값으로 복사한다. snapshot은 원래 space 파괴 뒤에도 독립적으로 읽을 수 있다. | MiniMM 전용 진단 API다. Linux procfs/pagemap ABI 또는 kernel-wide atomic snapshot에 대응하지 않는다. |
| memory locking | `mlock`/`munlock`의 unaligned 범위를 시작은 아래로, 끝은 위로 page-round한다. 길이 0은 no-op이고, lock된 frame은 eviction하지 않는다. 실패한 `mlock`은 이미 바꾼 PTE를 rollback한다. | 기본 range/pin 의미에 대응한다. 실측한 Linux 6.6의 hole 실패 중 prefix lock 진행은 재현하지 않는 transactional 모델이다. VMA lock flag, inheritance policy와 resource limit은 축소됐다. |
| cold advice | `MINIMM_MADV_COLD`는 설치된 로컬 PTE의 accessed bit를 clear하고 resident physical frame을 global cold로 표시한다. PTE가 없는 page를 materialize하지 않으며 locked frame도 표시할 수 있다. | Linux `MADV_COLD`의 reclaim hint와 닮은 관찰 전이만 제공한다. Linux의 page aging/rmap 동작과 동일하지 않다. |
| bounded reclaim와 refault | system 전체에서 cold 우선, 그 뒤 exact-LRU로 최대 target을 동기식 best-effort page-out한다. explicit와 automatic pressure의 scan/reclaim, policy victim의 첫 page-in을 센다. | MiniMM 전용 실험 API와 counter다. Linux reclaim control ABI나 workingset event에 직접 대응하지 않는다. |
| `mremap` | whole-VMA shrink/grow/MAYMOVE와 `MREMAP_FIXED|MAYMOVE` destination 교체를 지원한다. PTE 이동 실패는 원래 source/destination으로 rollback한다. | 자주 쓰는 단일 VMA subset에 대응한다. |
| `MREMAP_DONTUNMAP` | 같은 크기의 private-anonymous VMA에 PTE/lock을 destination으로 옮기고 source VMA를 빈 상태로 남긴다. source의 다음 접근은 zero-fill fault한다. fixed destination 교체도 지원한다. | Linux의 source-fault 동작을 좁은 mapping subset에서 대응한다. |
| zero-length range API | `mprotect`, `msync`, `mincore`, `mlock`, `munlock`, `madvise`의 길이 0은 유효 주소 조건만 검사하는 no-op이다. | Linux 유사 edge contract다. `mmap`과 mapping COPY의 길이 0은 여전히 invalid다. |
| page cache와 file backing | 같은 note object의 mapping/I/O는 page cache를 공유한다. note당 reference-counted file-backing/duplicated FD 하나를 cached frame들이 공유한다. | Linux page-cache alias의 작은 객체 단위 모델이다. inode 전역 coherence는 아니다. |
| eviction과 swap slot | resident 한도에서 unpinned cold frame을 우선하고 나머지는 exact-LRU로 동기식 page-out한다. private frame이 소멸하면 내부 swap slot을 free-list에 반환해 재사용한다. | reclaim/page-in/out 결과는 모델링하지만 Linux reclaim architecture와는 다르다. |
| `brk` VMA | heap grow/shrink가 하나의 compatible VMA를 유지하고 protection을 복원하면 split VMA를 다시 합친다. | 관찰 가능한 VMA range 관리의 단순화된 대응이다. |

`MINIMM_MINCORE_PRESENT`, `DIRTY`, `LOCKED`, `SHARED`, `COW`, `ACCESSED`처럼
bit 0보다 높은 `mincore` bit는 Linux ABI가 아니라 MiniMM 진단 확장이다.
portable residency 검사처럼 bit 0만 사용하는 code와 내부 상태 검사용 code를
구분해야 한다.

space별 bounded fault trace와 `EXPLICIT`/`ACCESS` origin도 Linux ABI가 아닌
MiniMM 관찰 기능이다. 최근 handler 상태 전이를 결정적으로 재생하기 위한
진단값이며 kernel tracepoint, `perf` event 또는 실제 hardware fault log로
해석하지 않는다.

`minimm_page_info_t.cold`도 MiniMM physical-frame 진단값이다. cold를 설정한
alias의 local PTE `ACCESSED`만 clear되므로 같은 shared frame의 다른 alias는
`cold == true`, `accessed == true`를 동시에 보일 수 있다. lock된 cold frame은
victim scan에는 포함되지만 eviction에서는 건너뛴다. 이는 Linux referenced
bit 회수나 full rmap walk 결과가 아니다.

`minimm_system_reclaim()`의 result와 system reclaim counter는 호출 round의
결정적 모델 관찰값이다. target은 보장이 아닌 상한이고 pinned frame만 남으면
성공한 shortfall이 가능하다. scan은 resident pinned frame을 포함하고 여러
round에서 같은 frame을 다시 셀 수 있다. automatic resident pressure도 같은
scan/reclaim/refault counter에 포함되지만 직접 `MINIMM_MADV_PAGEOUT`은 제외된다.
refault는 policy가 내보낸 frame의 첫 page-in을 한 번 세는 분류일 뿐 Linux
refault distance나 workingset activation 판정이 아니다.

`minimm_space_snapshot_capture()`도 MiniMM 관찰 기능이다. VMA-only 상태와
sparse PTE 생성, fork 뒤 같은 `frame_cookie`를 가진 COW alias, write 뒤의 frame
분리, page-out 뒤 남은 frame identity를 안정적인 값으로 비교하기 위한 것이다.
가상 VMA의 모든 page를 합성하지 않고 설치된 PTE만 주소순으로 복사하며, live
pointer를 보유하지 않아 space 수명과 독립적이다. 이 snapshot의
`frame_cookie`와 `frame_mapping_count`를 `/proc/*/pagemap` PFN, kpagecount 또는
Linux rmap 결과로 해석하면 안 된다.

## Note와 서비스 COPY

note COPY는 Linux syscall 대응 기능이 아니라 MiniMM 저장 객체의 snapshot
연산이다.

- `minimm_note_create()`와 TCP service가 만든 temporary note의 COPY는 호출
  시점에 page byte를 읽지 않는다. sparse destination backing과 parent/child
  lineage만 연결하는 O(1) metadata 연산이다.
- child read는 아직 분리되지 않은 page를 ancestor에서 찾는다. source의 첫
  변경 전에 child가 상속한 기존 page를 보존하고, child 첫 write/edit는 자기
  overlay frame으로 분리한다.
- mapping write는 note lineage lock을 거치지 않으므로 mapped source COPY는
  `MINIMM_ERROR_BUSY`다. mapped note의 grow는 허용하고 shrink는 `BUSY`다.
- `minimm_note_open_fd()` source는 외부 writer가 lineage lock을 우회할 수 있다.
  이 경우에만 COPY 시 모든 page를 새 backing에 쓰는 eager fallback으로
  COPY 완료 뒤의 외부 변경과 destination을 격리한다. COPY와 동시에 외부
  writer가 여러 page를 변경하면 atomic whole-file snapshot은 보장하지 않는다.

주소 공간의 `minimm_mapping_copy()`와 note COPY는 서로 다른 계층이다. 전자는
VMA/PTE frame을 공유하고 write fault에서 분리하며, 후자는 note I/O lineage와
overlay cache에서 page를 분리한다.

## 의도적으로 남은 차이

다음 항목은 이번 대응 범위 밖이며, 지원된 것으로 해석하면 안 된다.

- 실제 syscall 번호/ABI, `errno`, process, CPU MMU, hardware page table/TLB,
  `SIGSEGV`와 `SIGBUS` delivery가 없다. 모든 접근은 MiniMM C API와 status로
  수행된다.
- note/file 크기와 offset은 4KiB 정렬을 요구한다. Linux file mapping의
  partial-EOF zero-fill과 다음 page 접근의 `SIGBUS`를 재현하지 않는다.
- THP, hugetlb, NUMA policy/migration, memory cgroup, overcommit/accounting,
  kernel OOM selection, `userfaultfd`가 없다.
- frame mapcount는 추적하지만 Linux rmap처럼 frame에서 모든 VMA/PTE alias를
  찾아가는 full reverse mapping은 없다. 따라서 shared alias 전체의 software
  dirty bit를 한 번에 정리하는 등의 동작은 제한된다.
- page cache는 note/backing object 단위다. 동일 inode를 여러 note로 열었을 때
  합쳐지는 전역 per-inode cache와 외부 FD I/O/truncate coherence가 없다.
- eviction과 writeback은 호출 thread의 동기식 cold 우선 exact-LRU 경로다.
  Linux active/inactive list, refault distance·workingset 판정, kswapd,
  background flusher, writeback throttling과 MGLRU가 없다.
- VMA index는 Maple-inspired immutable fanout tree다. 실제 Linux Maple Tree의
  node algorithm, kernel RCU, lockless page-table walk와 per-VMA lock이 아니다.
- `RLIMIT_AS`, `RLIMIT_DATA`, `RLIMIT_MEMLOCK`, memory-policy inheritance 같은
  process/VMA resource 정책이 없다.
- 실측한 Linux 6.6의 hole-spanning range syscall은 `ENOMEM`을 반환하면서 앞선 VMA의
  protection/lock 상태나 mapped 조각의 advice 효과를 일부 남길 수 있다.
  MiniMM은 `mprotect`, `mincore`, `mlock`, `munlock`, `madvise`에서 전체 mapped
  range와 필요한 reclaimability를 먼저 검증하므로 hole/사전 검증 실패에는 부분
  VMA/PTE 변경을 남기지 않는다. `mlock`의 뒤쪽 page 준비 실패도 이미 변경한
  로컬 PTE/lock 상태는 rollback하지만 shared backing frame의 page-in/residency
  효과는 남을 수 있다. 따라서 실패 진행 상태까지 Linux oracle로 사용할 수 없다.
  다만 `WILLNEED`/`PAGEOUT` 실행 중 page-in, writeback 또는 경쟁으로 생긴 runtime
  실패는 앞선 page의 효과를 남길 수 있다. `msync`도 mapped 조각을 sync한 뒤
  hole을 `NOT_FOUND`로 보고한다.
- `mremap`은 정확히 하나의 전체 VMA만 받는다. `old_length == 0` shareable clone,
  여러 VMA에 걸친 range, Linux가 허용하는 더 넓은 `DONTUNMAP` mapping 종류를
  지원하지 않는다.
- COW write는 경쟁 안전성을 우선해 frame mapcount가 1이어도 항상 새 4KiB
  frame을 복사한다. Linux가 exclusive anonymous page를 제자리 재사용하는
  최적화는 모델링하지 않았으므로 COW allocation/성능을 Linux와 비교하면 안
  된다.
- shared zero page, architecture-specific PTE bit, soft-dirty, NUMA hinting fault,
  KSM, migration entry와 swap PTE encoding을 재현하지 않는다.
- synthetic PFN은 resident frame object ID다. page-out/page-in과 allocator
  동작을 실제 Linux physical PFN 변화로 해석할 수 없다.
- immutable space inspection은 capture한 MiniMM 값의 수명과 비교 가능성만
  보장한다. 여러 process/mm, shared frame과 kernel subsystem 전체에 걸친
  global atomic snapshot이나 Linux `/proc/*/maps`·`pagemap` byte format은
  제공하지 않는다.

따라서 miniMM은 Linux MM의 교육·검증용 executable model로는 쓸 수 있지만,
Linux kernel의 correctness 또는 성능 대체 oracle은 아니다. API별 정확한 입력,
rollback과 수명 계약은 [API 안내](api.md), 내부 상태 구조는
[아키텍처](architecture.md)를 따른다.

[linux-concepts]: https://cdn.kernel.org/doc/html/latest/admin-guide/mm/concepts.html
[linux-mm-api]: https://www.kernel.org/doc/html/latest/core-api/mm-api.html
[mmap]: https://man7.org/linux/man-pages/man2/mmap.2.html
[mremap]: https://man7.org/linux/man-pages/man2/mremap.2.html
[mincore]: https://man7.org/linux/man-pages/man2/mincore.2.html
[mlock]: https://man7.org/linux/man-pages/man2/mlock.2.html
[mprotect]: https://man7.org/linux/man-pages/man2/mprotect.2.html
[msync]: https://man7.org/linux/man-pages/man2/msync.2.html
[madvise]: https://man7.org/linux/man-pages/man2/madvise.2.html
