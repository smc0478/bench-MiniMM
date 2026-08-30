# miniMM 벤치마킹 안내

이 문서는 테스트 또는 외부 harness를 작성할 때 재현 가능한 workload와 측정
기준을 정의한다. 목표는 실제 Linux MM 처리량을 흉내 내는 것이 아니라 메모리
분석 도구가 상태 전이를 얼마나 정확히 찾는지 비교하는 것이다.

## 기본 원칙

각 case는 새 `minimm_t`와 `minimm_space_t`에서 시작하고 다음 정보를 결과에
함께 기록한다.

- miniMM version과 build type
- sanitizer 활성화 여부
- `physical_memory_size`, page 수, TLB entry 수
- workload seed, mapping 수·크기, 반복 횟수, thread 수
- 시작/종료 system·space stats
- 예상 상태와 실제 status/PFN/page flag

timing을 측정한다면 준비와 검증을 제외한 구간을 `clock_gettime()` 같은
monotonic clock으로 감싸고 warm-up, 반복 횟수, median 및 tail percentile을
함께 기록한다. 작은 모델에서는 allocator와 file system 영향이 크므로 한 번의
wall-clock 값보다 상태 전이와 counter delta가 더 중요한 지표다.

## 사용할 수 있는 관찰값

`minimm_system_get_stats()`로 다음 값을 얻는다.

- 생성되어 살아 있는 unique frame 수
- resident frame 수와 configured 한도
- page-in/page-out 누계
- policy victim scan, reclaim 성공과 첫 refault 누계

`minimm_space_get_stats()`로 다음 값을 얻는다.

- VMA/PTE/present/resident 수. `pte_count`는 page-out 뒤 남은 내부 PTE도 세지만
  `present_count`와 `resident_count`는 resident translation만 센다.
- dirty/COW/shared/locked PTE 수
- fault handler sequence
- TLB hit/miss/replacement/invalidation 누계

`minimm_query_page()`, `minimm_mincore()`, `minimm_translate()`는 page별
correctness oracle로 사용한다. `mincore`의 bit 0만 Linux-style residency이고,
로컬 PTE가 없어도 shared backing cache page가 resident이면 설정될 수 있다.
나머지 bit는 MiniMM 진단값이다. nonresident PTE의 query는 `present == false`,
PFN `MINIMM_PFN_NONE`이어야 한다. COW copy 횟수 전용 counter는 없으므로 write
전후 PFN과 COW flag 변화로 확인한다. 짧은 workload의 fault 원인별 수는 시작
전에 `minimm_space_clear_fault_trace()`를 호출하고 종료 뒤
`minimm_space_get_fault_trace()`의 event를 집계한다. 64개를 넘으면
`overwritten_count`가 0이 아니므로 전체 수 집계에는 workload를 나누거나
명시적 handler 결과를 즉시 수집하는 harness를 사용한다.

`minimm_query_page().cold`는 local PTE의 accessed bit가 아니라 physical frame의
global 우선순위다. shared alias는 cold를 같이 보지만 한 alias에
`MINIMM_MADV_COLD`를 적용해도 다른 alias PTE의 accessed bit는 그대로일 수 있다.
locked frame도 cold로 표시할 수 있으므로 `cold == true`를 즉시 evictable이라는
뜻으로 사용하지 않는다.

여러 VMA와 설치된 sparse PTE를 한 번의 관찰 단위로 비교할 때는
`minimm_space_snapshot_capture()`를 사용한다. mapping/page record는 주소순이고
snapshot은 capture 뒤의 live mutation이나 space 파괴와 독립적이므로, 두 시점의
VMA/page-table generation, `mapping_cookie`, `frame_cookie`와 frame mapcount를
관계 oracle로 비교할 수 있다. 단, 이는 MiniMM 값 객체이며 Linux pagemap PFN이나
여러 space를 동시에 고정한 kernel-wide snapshot은 아니다.

추천 파생 지표는 다음과 같다.

```text
TLB hit ratio          = delta_hits / (delta_hits + delta_misses)
faults per access      = delta_fault_sequence / access_count
page-out amplification = delta_page_out / pages_written
resident pressure      = peak_resident / resident_limit
reclaim efficiency     = delta_reclaim_count / delta_reclaim_scan_count
policy refault ratio   = delta_refault_count / delta_reclaim_count
```

shared alias를 여러 PTE가 가리키면 space resident 수는 alias별로 세지만 system
resident 수는 unique frame별로 센다. 두 값을 직접 동일시하지 않는다.

## 기능·성능 workload

| 시나리오 | 구성 | 확인할 결과 |
| --- | --- | --- |
| demand-zero 순차 접근 | 큰 anonymous private range를 page마다 읽기 | zero-fill, PFN 생성, fault/page-in 증가 |
| 강제 eviction | resident 한도보다 큰 working set을 순회 | resident 한도 유지, page-out/page-in, 데이터 보존 |
| TLB locality | 같은 page 반복 후 TLB index가 충돌하는 page 교차 | hit ratio와 replacement 변화 |
| VMA churn | 작은 VMA map, 부분 protect, 부분 unmap 반복 | 겹침 없음, split 수, lookup 결과, update 비용 |
| shared anonymous fork | shared mapping 뒤 fork하고 양쪽에서 접근 | 같은 PFN과 write visibility |
| shared note | 같은 note를 여러 space에 `MAP_SHARED` | alias visibility, `msync` 후 FD byte |
| private note COW | 같은 note를 `MAP_PRIVATE`로 읽은 뒤 한쪽 write | 최초 read PFN 공유 가능, write 후 PFN 분리, file 불변 |
| fork COW | faulted private page를 fork 후 parent/child write | write 전 PFN 동일, 이후 분리와 byte 격리 |
| populate best effort | 작은 resident 한도와 pinned victim에서 fixed+populate | mmap 성공과 VMA 유지, 준비된 page만 resident, 나머지는 후속 fault |
| lock pressure | working set 일부를 mlock하고 나머지를 page-in/out | pin 보존, `BUSY`, unlock 후 회복 |
| cold-priority working set | hot/cold/locked page를 섞고 bounded reclaim 뒤 victim refault | cold unlocked 우선, best-effort shortfall, scan/reclaim/refault delta |
| remap 이동 | 인접 blocker를 둔 뒤 MAYMOVE 확장 | 기존 PFN/byte 보존, 이전 주소 제거 |
| fixed remap | source와 destination 양쪽에 데이터를 둔 뒤 FIXED 이동 | destination 교체, source byte/PTE 이동, 실패 rollback |
| remap DONTUNMAP | 같은 크기 private-anonymous VMA를 이동 | destination byte/lock 보존, source VMA 유지와 zero refault |
| inspection snapshot churn | fault/protect/fork/COW/page-out 사이마다 capture | 주소순 sparse record, generation 변화, 과거 값 불변과 frame-cookie 관계 |
| sparse note COPY | fault되지 않은 큰 temporary note를 여러 세대 COPY | COPY 직후 frame/resident/page-in 불변, 첫 변경 page만 분리 |
| 원격 shared note | 두 client가 같은 capability를 OPEN하고 교차 I/O | visibility, handle 권한, chunk framing |
| protocol pressure | 작은 negotiated payload와 여러 동시 client | chunk 완료 byte, quota, clean shutdown |

순차·random workload는 동일 seed의 address trace를 미리 만들고 두 detector에
같은 trace를 제공한다. model의 direct-mapped TLB index는 virtual page number
modulo TLB capacity이므로 의도적인 conflict trace도 만들 수 있다.

## 취약점 탐지기용 workload 아이디어

다음 case는 허용/거부 상태와 rollback 불변조건을 oracle로 사용하는 추가
후보다.

### 권한과 범위

- read-only VMA에 write/edit fault를 발생시키고 `PERMISSION`을 기대한다.
- `WRITE`만 있는 VMA에서 write는 성공하고 edit은 실패하는지 확인한다.
- `maximum_protection`보다 높은 `mprotect`가 상태를 바꾸지 않는지 확인한다.
- 두 page 접근에서 두 번째 page만 unmap하거나 권한을 낮춰 정확한 partial
  `out_completed`를 검증한다.
- user limit 근처의 length overflow, 잘못된 page 정렬, note 범위 초과를 입력해
  OOB 접근 없이 거부되는지 검사한다. Linux 유사 range API의 0 length는 no-op,
  `mlock`/`munlock`의 unaligned range는 양쪽 page 경계로 round되는지 별도로
  검사한다.

### COW와 alias 격리

- fork 직후 private PFN은 같고 write 뒤 달라지는지 검사한다.
- parent write가 child/private note/file byte를 오염시키지 않는지 검사한다.
- shared mapping은 반대로 동일 frame과 변경 visibility를 유지하는지 검사한다.
- COW page를 page-out/page-in한 뒤에도 private byte가 유지되는지 검사한다.
- temporary note COPY 직후 page/frame counter가 크기에 비례해 늘지 않고,
  source·child의 첫 write와 resize가 서로의 snapshot byte를 바꾸지 않는지
  검사한다. 여러 세대 COPY 뒤 ancestor caller reference를 놓은 case도 포함한다.
- 외부 `open_fd` source COPY는 eager fallback 뒤 별도 FD write가 destination을
  바꾸지 않는지 검사한다. COPY와 동시에 외부 writer를 돌린 결과는 atomic
  whole-file snapshot oracle로 사용하지 않는다.

### stale translation과 metadata

- TLB를 채운 뒤 `mprotect`, `munmap`, `mremap`, fork COW 전환을 수행하고 이전
  권한·주소로 접근할 수 없는지 검사한다.
- VMA를 반복 split/remove하면서 `mapping_query`가 hole이나 잘못된 note
  offset을 반환하지 않는지 확인한다.
- reader thread의 `mapping_query`와 updater thread의 map/protect/unmap을
  교차해 immutable snapshot 회수 중 use-after-free가 없는지 검사한다.
  단, space destroy는 모든 worker가 join한 뒤 수행한다.

### 실패 원자성과 자원 압력

- 모든 resident frame을 pin한 상태에서 일반 fault는 `BUSY`가 되고 기존 byte가
  유지되는지 검사한다. 같은 조건의 `MAP_POPULATE`는 best-effort이므로 mmap이
  성공하고 VMA가 남되 준비하지 못한 page의 후속 access가 `BUSY`인지 본다.
- fixed-noreplace 충돌은 기존 VMA를 덮지 않고, fixed+populate는 destination
  교체가 prefault 성공 여부와 독립적으로 commit되는지 본다.
- 작은 resident limit에서 dirty shared/private page를 반복 축출해 write-back
  대상이 note file과 내부 page file로 올바르게 분리되는지 검사한다.
- 생성/해제를 반복하고 종료 시 frame/VMA/PTE 수가 기대값으로 돌아오는지
  확인한다.

### 수명 오용을 분리한 negative suite

API 계약은 destroy/release와 같은 handle의 동시 사용을 허용하지 않는다.
이 계약 자체를 어기는 use-after-release case를 detector 평가에 쓰고 싶다면
정상 conformance suite와 분리하고 “caller misuse”로 표시한다. library의
동시성 결함과 지원하지 않는 호출 계약을 같은 finding으로 계산하지 않는다.

외부 FD를 직접 바꾼 뒤 이미 cache된 note에서 최신 byte를 기대하는 case도
현재 알려진 coherence 제한이다. detector 오탐 평가에는 사용할 수 있지만
보안 결함 oracle로 분류하면 안 된다.

### 원격 서비스 negative suite

- HELLO 전 다른 opcode, 0 request ID, 잘못된 magic/header size, reserved bit,
  negotiated 상한을 넘는 payload를 보내 framing 거부와 연결 종료를 확인한다.
- read-only handle로 WRITE/EDIT, 최대 권한을 넘는 OPEN, DELETE right가 없는
  capability의 UNLINK를 시도하고 다음 PING도 정상인지 확인한다.
- capability를 UNLINK한 뒤 새 OPEN은 실패하지만 기존 두 handle의 데이터는
  유지되는지 확인한다.
- `max_clients`, `max_notes`, 단일/전체 note byte limit과 연결당 256 handle
  상한에서 allocation 및 reference count가 원상 복구되는지 본다.
- 같은 shared note의 서로 다른 범위를 여러 client가 반복 write하고 종료와
  동시에 socket을 drain해 deadlock, UAF, quota underflow를 검사한다.

v1 capability는 계정이나 ACL이 아닌 bearer secret이고 token 하나가 생성 시
최대 권한을 다시 얻을 수 있다. 제한된 권한을 요청한 OPEN handle을 별도
delegation credential로 간주하는 detector oracle을 만들면 안 된다. 네트워크
공격면의 상세 경계는 [서비스 안내](service.md)를 따른다.

## 검증 profile

일반 strict build와 test:

```sh
cmake -S . -B build/dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIMM_WARNINGS_AS_ERRORS=ON
cmake --build build/dev --parallel
ctest --test-dir build/dev --output-on-failure
```

메모리·정수 UB 검증 profile:

```sh
cmake -S . -B build/sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIMM_ENABLE_ASAN=ON \
  -DMINIMM_ENABLE_UBSAN=ON \
  -DMINIMM_WARNINGS_AS_ERRORS=ON
cmake --build build/sanitize --parallel
ASAN_OPTIONS=detect_leaks=0 \
UBSAN_OPTIONS=halt_on_error=1 \
ctest --test-dir build/sanitize --output-on-failure
```

`detect_leaks=0`은 LeakSanitizer가 동작하지 않는 ptrace 기반 sandbox를 위한
설정이다. 일반 환경에서는 이 항목을 제거해 leak 검사도 별도로 수행한다.

ASan/UBSan finding, nonzero process status, CTest failure, oracle mismatch를
각각 별도 결과 열로 기록한다. sanitizer가 느리게 만든 wall-clock 수치는
비-sanitized 성능 결과와 직접 비교하지 않는다.

## 해석할 때의 주의점

- miniMM은 host `mmap`을 resident-byte arena 할당에 사용하지만 MiniMM 주소를
  host 주소로 map하지 않고 hardware MMU도 모델에 사용하지 않는다. 따라서
  Linux page-fault latency 또는 kernel TLB 성능의 대용물이 아니다.
- space 접근은 보수적인 mutex로 직렬화되고 VMA update는 snapshot 전체를
  재구축한다. 처리량보다 상태 전이의 재현성을 우선한다.
- temporary backing의 실제 지연은 host file system과 `/tmp` 설정에 영향을
  받는다.
- anonymous arena를 처음 touch할 때 생길 수 있는 host fault는 저장 구현의
  부수 효과이며 MiniMM `fault_sequence`에 포함되지 않는다. 반대로 MiniMM
  fault는 VMA/PTE/COW 상태 전이이므로 host fault counter와 동일하지 않다.
- `fault_sequence`는 handler invocation 수이고 `page_in_count`에는 zero frame
  materialization도 포함된다. `MAP_POPULATE`, `mlock`, `MADV_WILLNEED`의 clean
  materialization은 fault sequence에는 포함되지 않지만 page-in 수에는
  반영될 수 있다. 지표 이름만 보고 disk fault 수로 해석하지 않는다.
- `reclaim_scan_count`는 unique frame 수가 아니다. explicit target을 채우는 각
  round와 automatic pressure가 현재 resident frame을 다시 검사하며 pinned
  frame도 포함한다. `minimm_reclaim_result_t.scanned_count`도 같은 round 합계다.
- `reclaim_count`는 explicit bounded reclaim과 resident pressure가 policy로
  내보낸 frame을 세고, `refault_count`는 그 frame의 pending 상태 뒤 첫 page-in만
  센다. 직접 `MADV_PAGEOUT`과 그 후 page-in은 page-out/in에는 들어가지만 이 두
  counter에는 들어가지 않는다. 측정 구간 전에 생긴 pending refault가 다음
  구간에 나타날 수 있으므로 ratio를 비교할 때 workload phase를 맞춘다.
- 이 cold/exact-LRU 값은 Linux active/inactive list, refault distance 또는
  workingset/MGLRU 판정 결과가 아니다.
- fault trace는 space별 최근 64개 handler 결과만 보관한다. cross-space의 전역
  시간순서, background reclaim event나 host kernel fault를 나타내지 않는다.
- detector 비교 시 동일 compiler, CMake 옵션, config, seed, trace, 반복 횟수를
  사용한다.

host mapping의 사용 경계를 확인하려면 raw syscall 이름을 검색한다.
`mmap`/`munmap`은 `src/frame.c`의 resident arena 수명에만 나타나야 하고,
MiniMM 권한을 host page protection으로 위임하는 raw `mprotect`는 없어야 한다.
`minimm_mmap` 같은 접두사 API는 MiniMM 모델 함수이므로 정확한 host symbol과
구분한다.

```sh
rg '#include <sys/mman.h>|(^|[^[:alnum:]_])(mmap|munmap|mprotect)\(' src
nm -u build/dev/libminimm.a |
  rg '(^|[[:space:]])(mmap|munmap|mprotect)$'
```
