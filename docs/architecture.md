# miniMM 아키텍처

## 목적과 경계

miniMM은 Linux `mm`의 관찰 가능한 핵심 동작을 작은 사용자 공간 모델로
낮춘 구현이다. 페이지 경계, 권한 검사, COW, fault, eviction, VMA 교체 및
TLB 무효화를 테스트하기 쉽게 만드는 것이 목표다.

아래 항목은 현재 구현되어 있다.

- 고정 4KiB page와 48-bit/4-level page table
- host `mmap` resident-byte arena와 파일 기반 page-out/page-in
- shared/private anonymous 및 note mapping
- private mapping, `minimm_space_fork()`와 같은 공간 private VMA 복사의 COW
- immutable VMA snapshot, RCU-style reader 보호, per-space TLB
- VMA와 sparse PTE/frame 관계를 보존하는 immutable public inspection snapshot
- fault/access API와 Linux 유사 range API

다만 이 프로젝트는 커널이 아니며 page register, CPU MMU, 실제 TLB,
signal, process, Linux syscall ABI를 구현하지 않는다. PFN도 실제 물리 frame
번호가 아닌 모델 내부 식별자다.

## 전체 구성

```text
                         minimm_t
              configuration + frame store
                          |
              +-----------+-----------+
              |                       |
        minimm_note_t              minimm_space_t
      fd + shared page cache       one modeled mm_struct
              |                       |
              +---- mapping backing --+
                                      |
                 +--------------------+-------------------+
                 |                    |                   |
          VMA snapshot          4-level page table   per-space TLB
          + RCU-style GC          PTE -> frame        retained frame
                                      |
                         host mmap arena / temp file
```

핵심 객체의 소유 관계는 reference count로 이어진다. space와 note는 system을
retain하고, mapping backing은 note를 retain하며, page table·TLB·page cache는
frame을 retain한다.

선택적인 TCP 서비스 계층은 이 객체 모델 위에만 놓인다.

```text
minimm-client -- framed TCP v1 --> minimm-server
                                      |
                         capability registry + session handles
                                      |
                              minimm_note_t API
                                      |
                         frame cache + temporary files
```

서버는 host 주소를 MiniMM 주소로 map하지 않는다. protocol READ/WRITE/EDIT는
`minimm_note_pread/pwrite/pedit()`를 호출하고, 그 아래의 4KiB frame cache가
host `mmap` arena의 resident slot과 `pread`/`pwrite` 임시 file backing을
사용한다.

## 주소와 페이지 모델

- `MINIMM_PAGE_SIZE`는 항상 4096이고 변경할 수 없다.
- 가상 주소 형식은 12-bit offset과 각 9-bit인 네 단계 index로 구성된다.
- 공개 상수는 48-bit 모델을 나타내지만 현재 매핑 가능한 사용자 범위는
  lower canonical half인 `[0, 2^47)`이다.
- 일반 mapping의 최저 주소는 `0x10000`, 자동 mapping의 초기 탐색 기준은
  `0x100000`, program break의 초기값은 `0x100000000`이다.
- sparse page table은 필요한 중간 node만 만들고 빈 node를 회수한다.
- PTE는 frame, protection, `PRESENT/COW/SHARED/ACCESSED/DIRTY/LOCKED`
  software flag를 가진다.
- 내부 PTE는 frame이 page-out된 뒤에도 COW/dirty/lock metadata와 swap
  association을 유지할 수 있다. 그러나 공개 query에서 `present`는 Linux식으로
  resident translation이 있다는 뜻이다. nonresident PTE는 `present == false`,
  PFN은 `MINIMM_PFN_NONE`이며 `pte_count`에만 남는다. 이 값은 host RSS나 host
  `mincore(2)` 결과가 아니다.
- synthetic PFN은 frame store가 발급하는 내부 ID다. host virtual/physical
  address와 관계가 없고 dereference할 수 없다.

## Frame store, host mmap과 파일 backing

`physical_memory_size / 4096`은 동시에 resident일 수 있는 frame 수를
결정한다. 생성 가능한 frame object 수 자체의 고정 상한은 아니며, 한도를
넘기면 pin되지 않은 cold frame을 우선하고 같은 cold 상태에서는 가장 오래
사용된 resident frame을 고른다. `last_access`가 같으면 frame ID로 순서를
결정하므로 동일 입력의 victim 선택은 결정적이다.

frame store를 만들 때 `physical_memory_size` 크기의 단일 anonymous host
mapping을 `PROT_READ|PROT_WRITE`, `MAP_PRIVATE|MAP_ANONYMOUS`로 만든다. 이를
4KiB slot free-list로 나눠 resident frame byte를 저장한다. `MAP_FIXED`,
`PROT_EXEC`, host `mprotect()`는 사용하지 않는다. arena는 MiniMM virtual
address와 대응하지 않으며 host protection도 모든 slot에서 동일하다.

MiniMM의 VMA/PTE protection, `READ/WRITE/EDIT/EXEC` 검사, fault sequence,
software TLB, accessed/dirty flag, shared alias와 COW는 이 host mapping과
독립적인 명시적 모델 상태다. COW도 host `MAP_PRIVATE` 동작에 맡기지 않고
`minimm_frame_copy()`와 PTE 교체로 4KiB를 직접 복사한다.

익명·private 데이터의 page-out은 `mkstemp()` 후 즉시 unlink한 내부 임시
파일에 `pwrite()`한다. page-in은 먼저 `pread()`한 4KiB staging byte를
확보하고 victim을 page-out한 뒤 빈 arena slot에 복사한다. 따라서 page-in
실패 전에 기존 victim을 불필요하게 버리지 않는다. slot 반환은 MiniMM의
resident 상태 전이이며, 장기간 유지되는 host arena의 RSS 감소를 보장하지
않는다.

note는 별도의 임시 regular file 또는 호출자가 제공한 regular file descriptor의
duplicate를 사용하며 frame cache를 거쳐 `pread()`/`pwrite()`한다. note마다
reference-counted file-backing object 하나와 duplicated FD 하나를 두고 cached
frame들이 이를 공유하므로 page마다 FD를 복제하지 않는다. 외부
descriptor의 truncate를 통제할 수 없고 file mapping 범위 밖 접근은 host
`SIGBUS`가 될 수 있으므로 note file 자체는 직접 `mmap()`하지 않는다.

dirty shared note frame은 note file에 write-back된다. 익명 frame과 COW로
만든 private frame은 내부 page file에 기록되므로 note file을 바꾸지 않는다.
내부 page-out file의 swap slot은 frame이 소멸하면 free-list로 돌아가 다음
private frame이 재사용한다. store는 현재 사용 중인 slot 수와 high-water를
내부 통계로 추적한다.

각 physical frame은 PTE의 `ACCESSED` bit와 별개인 `cold`와
`reclaim_pending` 상태를 가진다. `MINIMM_MADV_COLD`는 호출한 space의 PTE
`ACCESSED`를 지우고 TLB를 무효화하면서 resident frame의 global cold bit를
설정한다. 같은 frame의 shared alias는 모두 cold를 관찰하지만 다른 alias의
local PTE `ACCESSED`는 그대로다. 어느 alias에서든 frame을 다시 사용하면 cold는
지워진다. lock된 frame에도 cold를 표시할 수 있지만 pin이 있는 동안 victim
선택에서는 제외된다.

`minimm_system_reclaim()`은 frame-store lock 아래 최대 target 수를 best-effort로
page-out한다. 한 round는 현재 resident frame을 모두 scan하므로 pinned frame도
scan 수에 들어가며, target을 채우기 위한 다음 round에서 같은 frame을 다시 셀
수 있다. cold 우선순위 뒤에는 exact-LRU를 사용하며 eligible victim이 없으면
오류 대신 짧은 결과로 끝난다. resident 한도 압력의 자동 victim 선택도 같은
policy와 reclaim counter를 사용한다.

frame store의 `reclaim_scan_count`는 검사한 resident frame, `reclaim_count`는
policy가 내보낸 victim, `refault_count`는 pending victim의 첫 page-in을 누계로
센다. explicit result의 `scanned_count`/`reclaimed_count`는 한 호출 범위의 같은
분류다.

policy가 내보낸 frame은 `reclaim_pending`이 되고 그 frame의 첫 성공 page-in이
pending을 지우며 refault를 한 번 센다. 직접 `MADV_PAGEOUT`은 공통 page-out
저장 경로를 쓰지만 policy reclaim으로 분류하지 않아 reclaim/refault counter에
들어가지 않는다. 이 작은 분류는 refault distance나 workingset 판정을 수행하지
않는다.

pin된 frame만 남아 eviction할 수 없으면 자동 pressure는
`MINIMM_ERROR_BUSY`를 반환한다. explicit reclaim은 성공한 best-effort shortfall로
보고한다.

## Note와 mapping backing

note는 page-aligned 크기, 권한, 중복 소유한 FD, offset별 공유 frame cache를
가진다. 동일한 `minimm_note_t`를 사용하는 mapping과 note I/O는 같은 cache를
통해 데이터를 본다.

`minimm_note_create()`로 만든 temporary note의 `minimm_note_copy()`는 page를
즉시 읽거나 복사하지 않는다. 같은 lineage lock 아래 새 sparse backing과
parent/child link를 만드는 O(1) metadata snapshot이다. child read는 자기
overlay에 없는 page를 immutable ancestor view에서 찾는다. source가 page를
변경하기 전에는 direct child에게 기존 frame을 보존하고, child의 첫 write는
자기 overlay frame을 만든다. lineage reference가 ancestor 수명을 유지하므로
여러 세대 COPY 뒤 caller가 중간 note reference를 놓아도 leaf snapshot은
유효하다.

`minimm_note_open_fd()`로 만든 source는 별도 FD를 통한 외부 writer를 lineage
lock으로 막을 수 없다. 이 source만 COPY 시 모든 page를 새 temporary backing에
쓰는 eager fallback을 사용한다. 이는 COPY 완료 뒤 외부 변경과 destination은
격리하지만 COPY와 동시에 외부 writer가 여러 page를 바꾸는 경우 atomic
whole-file snapshot을 보장하지 않는다. 어떤 source든 mapping이 있으면 mapping
write가 lineage lock에 참여하지 않으므로 COPY는 `BUSY`다. mapped note의 grow는
기존 mapping을 침범하지 않아 허용하지만 shrink는 `BUSY`다. mapping이 없을 때
shrink하는 lineage note는 child가 상속할 잘린 page를 먼저 보존한다. 이 note
snapshot 경로는 아래의 VMA/PTE COW 복사와 별도다.

| mapping 종류 | 최초 fault | write 동작 | fork 이후 |
| --- | --- | --- | --- |
| anonymous private | zero frame | 해당 mapping의 private frame 수정 | 향후 writable PTE는 양쪽 COW, 영구 read-only PTE는 frame만 공유. 아직 없는 page는 각자 zero-fill |
| anonymous shared | backing별 공유 zero frame | alias에 보임 | backing과 frame을 공유 |
| note private | note cache에서 읽음 | 최초 write에 private copy | 향후 writable private PTE는 COW |
| note shared | note cache에서 읽음 | 공유 frame을 수정 | backing과 frame을 공유 |

VMA의 `mapping_cookie`가 immutable range와 mutable backing binding을 연결한다.
VMA가 split되어도 cookie와 조정된 note offset을 유지하며, 같은 cookie를
가진 마지막 range가 사라질 때 binding을 release한다.

`minimm_mapping_copy()`는 source와 같은 backing을 retain한 새 binding을 만들고
새 `mapping_cookie`로 destination VMA에 연결한다. destination은 주소와 cookie
외에는 source의 note offset, protection, maximum protection, private 속성을
그대로 보존한다. shared backing을 새 private alias로 바꾸는 연산은 아니므로
shared VMA와 전체 VMA가 아닌 source range는 지원하지 않는다.

## VMA: Maple-inspired immutable tree

VMA 구현은 Linux Maple Tree 자체가 아니다. 그 아이디어를 단순화한
fanout 16 다단계 range index다.

- snapshot은 정렬된 `[start, end)` range 배열과 `max_end`를 가진 fanout 16
  검색 tree로 구성된다.
- publication 후 snapshot은 절대 수정하지 않는다.
- insert/remove/protect는 새 배열과 tree를 만들며 필요하면 VMA를 split한다.
- updater는 `space->lock` 아래 새 snapshot을 atomic publish한다.
- `minimm_mapping_query()` reader는 RCU-style read section에서 snapshot을
  lock 없이 조회한다.
- 이전 snapshot은 space별 RCU domain에서 reader 전체를 세는 단일 counter가
  0이 된 뒤 회수한다.

이는 epoch별 CPU RCU나 Linux Maple Tree의 in-place balancing을 재현하지
않는다. update 비용보다 명확한 lifetime과 실패 원자성을 우선한 구조다.

## Public immutable inspection snapshot

내부 `minimm_vma_snapshot_t`는 VMA index publication을 위한 구현 객체이고,
공개 `minimm_space_snapshot_t`는 학습·검사용 값 객체다. 두 snapshot은 이름이
비슷하지만 수명과 목적이 다르다. public capture는 live tree, PTE 또는 frame
pointer를 밖으로 노출하지 않고 다음 값을 새 저장소에 복사한다.

- capture 시점의 `minimm_space_stats_t`, VMA generation과 page-table generation
- 시작 주소순의 모든 VMA를 `minimm_mapping_info_t` record로 복사한 배열
- page 주소순의 설치된 sparse PTE만 복사한
  `minimm_space_snapshot_page_t` 배열

page record는 cold를 포함한 `minimm_page_info_t`를 중첩하고 VMA를 찾는
`mapping_cookie`,
physical frame 관계를 비교하는 `frame_cookie`, capture 시점의
`frame_mapping_count`를 함께 가진다. `frame_cookie`는 live pointer나 host PFN이
아니라 frame object의 값 기반 identity다. 같은 COW frame을 공유하는 parent와
child record는 같은 cookie를 보이고, child write로 frame이 분리되면 달라진다.
page-out은 frame object를 없애지 않으므로 nested page의 `present`와 `resident`가
false가 되어도 cookie는 유지된다.

capture는 VMA 전체의 가상 page 범위를 펼치지 않는다. VMA만 있고 아직 fault하지
않은 page는 page 배열에 없으므로 비용과 저장량은 VMA 수와 실제 sparse PTE 수에
비례한다. getter는 보관된 scalar, stats와 주소순 record만 copy-out하며 live
space로 되돌아가지 않는다. 따라서 capture 성공 뒤 space가 변하거나 파괴돼도
snapshot은 그대로 읽고 별도로 destroy할 수 있다. 오래된 snapshot 역시 새
fault, COW 또는 page-out에 의해 갱신되지 않는다.

이 일관성은 한 MiniMM space에서 복사한 관찰값의 불변성을 뜻한다. 여러 space,
공유 frame store와 실제 kernel MM 전체를 동시에 정지시키는 global atomic
snapshot이 아니며 Linux procfs나 pagemap encoding도 구현하지 않는다.

## Page fault와 COW

자동 access 경로는 주소 공간 lock 아래 먼저 VMA 권한과 direct-mapped TLB를
검사한다. TLB miss여도 resident PTE가 있고 COW/권한 fault가 필요하지 않으면
page-table walk로 `ACCESSED`와 필요 시 `DIRTY`를 갱신하고 TLB를 다시 채운다.
이 refill은 fault sequence를 증가시키지 않는다. PTE가 없거나 frame이
nonresident인 경우와 COW write/edit처럼 실제 resolution이 필요한 경우에만
다음 fault 경로로 들어간다. 일반 access의 VMA hole/permission 위반도 handler가
`UNMAPPED`/`PERMISSION` denial로 분류하고 fault sequence와 trace에 기록한다.

1. 주소와 단일 access 종류를 검증하고 fault sequence를 증가시킨다.
2. VMA와 현재 protection을 확인한다.
3. mapping cookie로 backing을 찾는다.
4. PTE가 없으면 zero frame 또는 note frame을 가져와 PTE를 설치한다.
5. frame이 non-resident이면 file에서 page-in한다.
6. private COW page의 write/edit fault이면 effective write protection을 해제할
   새 4KiB frame을 복사해 PTE를 교체한다.
7. accessed/dirty flag를 갱신하고 해당 TLB entry를 다시 채운다.

handler의 단일 완료 지점은 확정된 status와 함께 fault 정보를 space별 64-entry
ring에 append한다. space mutex와 같은 순서를 사용하므로 snapshot은 해당
space에서 오래된 event부터 일관되게 보인다. 호출자가 제공한 callback을 lock
안에서 실행하지 않고 고정 저장소를 사용해 reentrancy와 관찰용 allocation
실패를 피한다. 자동 access와 명시적 handler는 `origin`으로 구분한다. ring
overwrite와 clear는 lifetime fault sequence에 영향을 주지 않는다.

COW write는 frame mapcount가 1이어도 항상 새 frame을 복사한다. 다른 space가
동시에 alias를 추가할 수 있는 환경에서 unsafe in-place reuse를 피하기 위한
결정이며 Linux의 exclusive-page reuse 성능 최적화는 재현하지 않는다.

`minimm_read()`, `minimm_write()`, `minimm_edit()`는 page 경계를 나누어 이
경로를 자동 호출한다. 여러 page 접근 중 뒤쪽 page에서 실패하면 앞쪽
page까지 처리된 byte 수를 `out_completed`로 돌려준다.

`MAP_POPULATE`, `mlock`, `MINIMM_MADV_WILLNEED`는 메모리 접근을 흉내 내지 않는
내부 clean materialization 경로를 사용한다. 이 경로는 PTE와 resident frame만
준비하고
`ACCESSED/DIRTY`, COW 분리, fault sequence, TLB fill을 발생시키지 않는다.
따라서 write-only private-note page도 실제 첫 write 전까지 clean COW 상태를
유지한다. `MAP_POPULATE`는 fixed mapping에도 쓸 수 있는 best-effort hint여서
중간 materialization 실패가 이미 publish한 VMA를 rollback하거나 mmap 실패로
바뀌지 않는다. `mlock`은 이와 달리 요청 범위 pin에 실패하면 새 PTE/pin을
rollback한다.

`minimm_space_fork()`는 VMA와 backing binding을 복제하고 설치된 PTE frame을
공유한다. shared VMA는 `SHARED`로 남고, maximum protection에 `WRITE`/`EDIT`가
있는 private VMA의 parent/child PTE만 effective write-protected `COW`가 된다.
영구 read-only private VMA는 같은 frame을 공유하되 COW flag를 만들지 않는다.
모든 child PTE는 old/unlocked 상태로 시작해 `ACCESSED`, `LOCKED`를 상속하지
않는다. shared child의 로컬 PTE는 `DIRTY`와 `COW`도 지우지만 writable private
child는 COW snapshot의 기존 PTE dirty bit를 유지할 수 있고 공유 frame 자체의
dirty 상태도 별도로 보일 수 있다. 각 space의 page table과 TLB는 독립적이다.

`minimm_mapping_copy()`도 설치된 private PTE의 frame을 공유하지만, 새 space 대신
같은 VMA snapshot과 page table에 destination range를 추가한다. maximum
protection에 `WRITE`/`EDIT`가 있는 source/destination PTE만 모두 `COW`로
표시하며, 영구 read-only PTE는 `fork`와 같이 COW 없이 frame만 공유한다.
destination hint가 유효하고 비어 있으면 우선 사용하고, 그렇지 않으면 자동
gap으로 대체하며 기존 VMA를 덮어쓰지 않는다. PTE가 없는 page는 그대로 두어
copy 자체가 fault나 page-in을 만들지 않는다. source PTE의 access/dirty/lock
상태는 유지하지만 destination은 `ACCESSED`, `DIRTY`, `LOCKED`, `SHARED`를 지운
clean/old/unlocked private alias로 시작한다.
page-table의 sparse PTE만 수집하므로 비용은 VMA의 가상 page 수가 아니라 실제
space PTE 수와 source에 설치된 PTE 수에 비례한다.

새 snapshot, binding과 destination PTE를 먼저 준비한 뒤 source copy-state
전환과 snapshot publication을 commit한다. source 전환 전 준비가 실패하면
destination PTE와 임시 객체를 모두 회수하며 source PTE flag는 바뀌지 않는다.
정상 입력에서 source 속성 전환은 allocation 없이 기존 PTE만 갱신한다.

`minimm_mremap()`은 정확히 하나의 전체 VMA를 shrink/grow/move한다.
`MREMAP_FIXED|MAYMOVE`는 destination mapping을 교체하면서 source PTE를 옮기고,
준비 실패 시 원래 source/destination PTE를 복구한다. 같은 크기의 private
anonymous VMA에는 `MREMAP_DONTUNMAP|MAYMOVE`도 지원한다. 이 경우 PTE와 lock
상태는 destination으로 이동하고 source VMA는 PTE 없이 남아 다음 access에서
zero-fill fault한다. 이는 Linux의 현대 `mremap` 전체가 아니라 의도적으로 좁힌
subset이다.

## Software TLB

각 space는 설정된 entry 수의 direct-mapped TLB를 하나 가진다. index는
virtual page number에 의해 정해지며 entry는 retained frame reference,
effective protection과 PTE flag를 보관한다.

generation 비교는 사용하지 않는다. unmap, protect, COW 교체, explicit
`MADV_PAGEOUT`, `MADV_COLD`, range 이동, fork의 parent COW 전환처럼 translation
의미가 바뀌는 지점에서 관련 page/range 또는 전체 TLB를 명시적으로 무효화한다. 다른
접근의 eviction으로 nonresident frame이 남은 entry는 다음 lookup에서 제거한다.
한 access의 최초 lookup은 hit 또는 miss 중
하나만 집계하며 page-table refill 자체를 추가 hit로 세지 않는다. hit, miss,
replacement, invalidation 누적값은 public stats API로 읽을 수 있다.

## 권한 모델

VMA와 PTE는 `READ`, `WRITE`, `EDIT`, `EXEC`를 구분한다. `EDIT`는 노트 앱의
명시적인 편집 경로를 모델링하기 위한 추가 권한이며 항상 `WRITE`와 함께
설정해야 한다. 일반 `minimm_write()`는 `WRITE`만 요구하지만
`minimm_edit()`는 `WRITE|EDIT`를 모두 요구한다. `EXEC`는 execute fault의
허가 여부를 모델링할 뿐 실제 코드를 실행하지 않는다.

각 VMA에는 현재 protection과 `maximum_protection`이 있다. `mprotect`는
maximum을 넘을 수 없다. note에도 `READ/WRITE/EDIT/SHARE/RESIZE` 권한이
별도로 있으며 mapping의 최대 protection과 sharing 요청을 제한한다. private
note write는 backing을 바꾸지 않는 COW이므로 `READ` note도 write maximum을
가질 수 있지만 shared write에는 note `WRITE`가 필요하다. `EDIT` maximum에는
mapping 종류와 관계없이 note `EDIT`가 필요하다.

## 동시성과 lock 순서

space를 변경하거나 PTE/frame에 접근하는 대부분의 public API는
`space->lock`으로 직렬화된다. note cache와 backing cache, frame store는 각각
자체 mutex를 사용한다. 구현과 확장 코드는 다음 바깥쪽→안쪽 순서를
유지해야 한다.

```text
brk_lock -> space lock -> (mapping-backing lock 또는 note lock)
                         -> frame-store lock
```

- fork는 parent의 `brk_lock` 다음 `space->lock`을 잡는다.
- RCU retire queue의 내부 lock은 publication 과정에서 잠깐 사용되며 reclaim
  callback은 그 lock을 놓은 뒤 실행된다.
- VMA RCU-style 보호는 snapshot 수명만 보장한다. space/note handle 수명이나
  page table의 무잠금 접근을 보장하지 않는다.
- public inspection capture는 `space->lock` 아래 VMA/PTE를 복사하고 frame
  관찰에는 기존 순서대로 frame-store lock을 안쪽에서 사용한다. capture가
  반환한 값 객체는 어느 live lock이나 space/frame reference도 보유하지 않는다.
- system-wide bounded reclaim은 VMA/PTE를 수정하지 않고 frame-store lock만
  잡는다. nonresident 여부는 기존 PTE query/TLB residency 검증에서 관찰한다.
- API 호출과 같은 handle의 `minimm_space_destroy()` 또는 마지막
  `minimm_note_release()`를 동시에 실행해서는 안 된다. caller가 join이나
  상위 mutex로 수명을 동기화해야 한다.

서비스는 accept thread와 active client별 worker 하나를 사용한다. 한 client
연결에서는 request를 순서대로 하나씩 처리하고 client library도 연결 mutex로
호출을 직렬화한다. 서버 registry mutex는 capability lookup/link/unlink와
registry reference만 보호한다. note record의 mutex는 서로 다른 연결이 같은
note를 읽고 쓰거나 resize할 때 core note 호출과 quota 크기 갱신을 직렬화한다.
registry mutex를 잡은 채 file I/O나 note operation을 실행하지 않는다.

stop 경로는 listener를 깨우고 accept thread를 join한 뒤 active socket을
shutdown한다. condition variable로 worker가 handle reference를 모두 놓을
때까지 기다린 다음 registry reference를 해제하므로 server object보다 note
record가 늦게 살아남지 않는다. 이 lifetime 경계는 kernel RCU가 아니라
mutex, atomic reference count와 thread join으로 구현한다.

`minimm_destroy()`는 system을 closing 상태로 바꾸고 caller의 reference를
놓는다. 이미 존재하는 space와 note가 system/frame store를 retain하므로
그 객체들이 release될 때까지 내부 자원은 살아 있지만, destroy한 system
handle로 새 객체를 만들거나 다시 접근하면 안 된다.

## 모듈 경계

```text
include/minimm/minimm.h  public types, constants, functions
include/minimm/protocol.h stable wire constants and payload sizes
include/minimm/server.h  embedded server lifecycle and limits
include/minimm/client.h  remote note client API
src/mm.c                 system configuration and lifetime
src/space.c              address-space lifetime
src/frame.c              host mmap resident arena, temp backing, page I/O
src/note.c               note FD, rights, shared page cache
src/mapping_backing.c    anon/note shared-private backing bindings
src/vma_tree.c           immutable fanout-16 VMA snapshots
src/page_table.c         sparse four-level page table
src/tlb.c                per-space direct-mapped software TLB
src/fault.c              demand paging and COW fault resolution
src/access.c             checked cross-page read/write/edit
src/snapshot.c           immutable public space inspection copies
src/mapping.c            mmap/munmap/mprotect/mremap/copy/query
src/fork.c               address-space clone and COW setup
src/brk.c                brk/sbrk model
src/memory_api.c         msync/mincore/mlock/munlock/madvise
src/stats.c              system, space, TLB statistics
src/rcu.c                conservative user-space RCU-style domain
src/protocol.c           endian-safe framing and exact socket I/O
src/page_remap.c         transient file-page remap service model
src/mseal_merge.c        transient mseal range-merge service model
src/mglru_reparent.c     transient MGLRU generation-accounting service model
src/server.c             TCP lifecycle, registry, session dispatch
src/client.c             handshake, framing, chunked remote note calls
cmd/                     one-shot server/client command-line tools
examples/                executable core-MM learning scenarios
tests/                   invariant and integration tests
```

## 현재 한계

- Linux syscall 번호/ABI, `errno`, signal(`SIGSEGV`/`SIGBUS`), process
  scheduling, CPU MMU, hardware page table/TLB를 제공하지 않는다.
- lower canonical user range만 사용하며 page 크기는 4KiB로 고정이다.
- THP, hugetlb, NUMA 정책·migration, memcg, overcommit/accounting, Linux OOM,
  `userfaultfd`를 구현하지 않는다.
- reverse mapping은 frame mapcount만 추적하며 Linux rmap처럼 모든 alias를
  역순회하지 않는다.
- reclaim/writeback은 cold 우선 exact-LRU와 동기식 file I/O의 결정적 실험이다.
  Linux active/inactive list, refault distance 기반 workingset 판정, kswapd,
  background writeback 또는 MGLRU가 아니다.
- VMA update는 tree 일부가 아니라 snapshot 전체를 다시 만든다. Linux Maple
  Tree 자체, kernel RCU/lockless page-table walk, per-VMA lock을 구현하지 않는다.
- public space snapshot은 값 기반 inspection copy다. Linux `/proc/*/maps`,
  `/proc/*/pagemap` ABI나 여러 address space를 아우르는 kernel-wide atomic
  snapshot을 제공하지 않는다.
- `RLIMIT_AS`, `RLIMIT_DATA`, `RLIMIT_MEMLOCK` 같은 process limit을 모델링하지
  않는다.
- `MINIMM_MREMAP_FIXED`는 지원하지만 `mremap` source는 정확히 하나의 전체
  VMA여야 한다. `old_length == 0` clone과 여러 VMA remap은 지원하지 않는다.
- `MINIMM_MREMAP_DONTUNMAP`은 같은 크기의 private-anonymous VMA subset만
  지원한다.
- `minimm_mapping_copy()`는 같은 space의 private VMA 전체만 받으며 shared
  VMA와 부분 range 복사는 지원하지 않는다.
- `MINIMM_MAP_POPULATE`는 fixed 조합에서도 허용되지만 best-effort이며 실패한
  page 수를 별도 결과로 보고하지 않는다.
- mapping이 하나라도 붙은 note는 COPY와 shrink할 수 없지만 grow는 허용한다.
- page cache는 note/backing object 단위다. 동일 inode를 연 서로 다른 note
  object를 합치는 전역 per-inode cache와 외부 FD writer에 대한 자동
  invalidation/coherence를 제공하지 않는다.
- note 크기와 file mapping 끝은 4KiB 정렬을 요구해 Linux의 partial-EOF
  zero-fill 및 그 다음 page `SIGBUS` 동작을 재현하지 않는다.
- `MINIMM_MADV_NORMAL/RANDOM/SEQUENTIAL`은 현재 검증 후 no-op이다.
- 실패한 `mlock`은 새 PTE를 제거하고 기존 PTE의 frame, protection, software
  flag를 복구하지만, 그 과정에서 page-in된 기존 frame이나 backing cache는
  resident 상태로 남을 수 있다.

API별 세부 계약은 [api.md](api.md), Linux와의 비교는
[linux-mm-parity.md](linux-mm-parity.md)에 정리한다.
