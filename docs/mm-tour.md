# MiniMM MM tour

`minimm-mm-tour`는 TCP note 서비스나 host syscall을 거치지 않고 MiniMM의
address-space API를 직접 실행하는 작은 self-checking 실습이다. 출력은 실제
PFN 번호보다 `same`, `split`, `none` 관계, frame cookie와 fault resolution을
중심으로 보여 주므로 allocator 세부 구현이 바뀌어도 같은 의미를 유지한다.

## 빌드와 실행

```sh
cmake -S . -B build/dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIMM_WARNINGS_AS_ERRORS=ON
cmake --build build/dev --parallel
./build/dev/examples/minimm-mm-tour all
```

`MINIMM_BUILD_EXAMPLES=OFF`로 구성한 build에는 실행 파일이 만들어지지 않는다.
각 시나리오는 관찰값을 출력한 뒤 byte와 PFN 관계, resident 상한을 검사한다.
계약이 어긋나면 설명만 계속하지 않고 0이 아닌 종료 코드로 끝난다.

## 1. fault cycle

```sh
./build/dev/examples/minimm-mm-tour fault
```

실행 전에 다음 흐름을 예상한다.

```text
VMA only, no PTE
  -> first read: NOT_PRESENT / ZERO_FILLED
  -> PAGEOUT: PTE remains, resident=false, PFN=none
  -> read: NOT_PRESENT / PAGE_IN
  -> original byte is preserved
```

최초 anonymous 접근의 zero-fill과 이미 byte를 가진 nonresident frame의
page-in은 서로 다른 resolution이다. `minimm_query_page()`와
`minimm_translate()`는 이 전이를 스스로 만들지 않으며 현재 상태만 관찰한다.

tour는 access 전후에 space fault trace를 읽는다. `origin=access`는
`minimm_read/write/edit()`가 handler를 호출했다는 뜻이고 `origin=explicit`은
사용자가 `minimm_handle_page_fault()`를 직접 호출했다는 뜻이다. resident TLB
hit와 resident PTE refill은 fault event가 아니다.

## 2. fork COW

```sh
./build/dev/examples/minimm-mm-tour cow
```

parent가 만든 private anonymous page를 fault한 뒤 space를 fork한다. fork 직후
두 PTE는 같은 frame을 가리키며 effective write protection과 COW bit를 가진다.
child 첫 write는 `COW / COW_COPIED` fault로 새 frame을 만들고 다음을 확인한다.

- parent와 child의 resident PFN 관계가 `same`에서 `split`으로 바뀐다.
- child의 새 byte가 parent byte를 바꾸지 않는다.
- child PTE의 COW bit는 사라지고 writable protection이 복원된다.

이는 process와 `fork(2)`를 구현했다는 뜻이 아니다. 하나의 `mm_struct`에
대응하는 MiniMM space를 복제하고 page-level COW 전이만 실행한 것이다.

## 3. lock pressure와 reclaim

```sh
./build/dev/examples/minimm-mm-tour reclaim
```

resident 한도를 한 page로 둔 뒤 page A를 materialize하고 `mlock`한다. page B의
첫 접근은 내보낼 수 있는 victim이 없어 `MINIMM_ERROR_BUSY`가 되어야 한다.
page A를 `munlock`하고 같은 접근을 반복하면 A가 page-out되고 B가 resident가
된다. 마지막으로 A를 다시 읽어 저장 byte가 보존됐는지 확인한다.

이 시나리오는 Linux reclaim daemon이나 MGLRU를 흉내 내지 않는다. MiniMM의
작은 동기식 cold-priority exact-LRU, pin/unevictable 조건, file-backed
page-out/page-in을 결정적인 입력으로 관찰한다.

## 4. immutable inspection

```sh
./build/dev/examples/minimm-mm-tour inspect
```

이 시나리오는 point query를 이어 붙이지 않고
`minimm_space_snapshot_capture()`로 각 시점의 VMA, sparse PTE와 frame 관계를
값으로 보존한다. 실행 전에 다음 흐름을 예상한다.

```text
VMA only: mapping_count > 0, page_count = 0
  -> one page access: sparse page_count grows, untouched VMA page is absent
  -> fork COW: parent/child page records share one frame_cookie
  -> child write: child frame_cookie splits from the parent
  -> child PAGEOUT: resident=false, present=false, frame_cookie is unchanged
  -> live spaces change or are destroyed: older snapshots remain unchanged
```

tour는 각 snapshot의 VMA/page-table generation과 count를 출력하고,
`minimm_space_snapshot_get_mapping()`과
`minimm_space_snapshot_get_page()`가 주소순 record를 돌려주는지 검사한다. page
record 안의 `page`는 기존 `minimm_page_info_t` 관찰값이고,
`mapping_cookie`는 copied VMA, `frame_cookie`는 COW/page-out 전후의 frame
identity, `frame_mapping_count`는 capture 시점의 PTE alias 수를 나타낸다.

page 배열은 VMA의 모든 가상 page가 아니라 실제 설치된 sparse PTE만 가진다.
snapshot은 live pointer를 보유하지 않는 독립 값 객체이므로 이후 COW나 page-out이
오래된 결과를 바꾸지 않고, 원래 space를 destroy한 뒤에도 getter로 읽은 다음
`minimm_space_snapshot_destroy()`로 해제할 수 있다.

이는 Linux `/proc/*/maps`나 `/proc/*/pagemap` 출력의 재현이 아니다. 또한 여러
space와 shared frame store를 동시에 정지시킨 kernel-wide atomic snapshot도
아니며, 한 MiniMM space에서 capture한 교육·검사용 관찰값이다.

## 5. cold-priority working set

```sh
./build/dev/examples/minimm-mm-tour working-set
```

resident slot 세 개에 A/B/C를 순서대로 쓰고 A를 lock한다. 이어 A와 B에
`MINIMM_MADV_COLD`를 적용하면 두 physical frame은 cold가 되지만 A는 pin 때문에
victim이 될 수 없다. 첫 `minimm_system_reclaim(mm, 1, &result)`는 resident 세
frame을 scan하고 cold이면서 unlocked인 B 하나를 내보낸다.

다음 `reclaim(mm, 2, ...)`는 남은 A/C를 scan해 hot C만 내보낸 뒤, pinned A만
남은 다음 round에서 A를 다시 scan하고 성공한 best-effort shortfall로 끝난다.
따라서 이 호출의 결과는 `scanned_count=3`, `reclaimed_count=1`이다. scan 수는
unique frame 수가 아니며 pin된 frame도 round마다 다시 포함될 수 있다.

```text
A=cold+locked, B=cold, C=hot
  -> reclaim(1): scan=3, reclaim=1, victim=B
  -> reclaim(2): scan=3, reclaim=1, victim=C, locked shortfall
  -> read B: PAGE_IN, same byte/PFN, first policy refault=1
  -> read hot B again: refault remains 1
  -> unlock A: cold stays set
  -> reclaim(1): scan=2, reclaim=1, victim=A
```

최종 self-check는 frame 3개 중 resident 1개, page-in 4, page-out 3,
reclaim scan 8, reclaim 3, refault 1을 확인한다. `MINIMM_MADV_COLD`는 대상 local
PTE의 accessed bit를 clear하지만 cold는 physical frame 상태라 shared alias도
같이 관찰한다. locked frame도 cold가 될 수 있으며 eviction만 skip된다.

이 시나리오는 결정적인 cold-priority exact-LRU 실험이다. Linux
active/inactive list, refault distance, workingset 판정, kswapd, MGLRU 또는
full rmap을 구현하거나 예측하지 않는다. 직접 `MINIMM_MADV_PAGEOUT`은
page-out/in counter에는 들어가지만 이 policy의 reclaim/refault counter에는
들어가지 않는다.

## 자동 검사

다섯 시나리오는 CTest의 `education` label로도 등록된다. working-set만 고르면
test 이름은 `minimm.education.working_set`이다.

```sh
ctest --test-dir build/dev -L education --output-on-failure
```

더 큰 workload를 만들 때는 [벤치마킹 안내](benchmarking.md)의 상태·counter
기준을 사용하고, 실제 Linux와 의도적으로 다른 부분은
[Linux MM 대응표](linux-mm-parity.md)에서 먼저 확인한다.
