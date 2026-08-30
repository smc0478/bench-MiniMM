# miniMM 공개 API 안내

MM 모델 공개 API의 기준 헤더는 `include/minimm/minimm.h`다. 원격 서비스는
`server.h`, `client.h`, wire 상수는 `protocol.h`에 있다. 이 문서는 현재
구현의 사용 규칙을 요약하며 Linux syscall과의 ABI 호환성을 약속하지 않는다.

## 기본 상수와 설정

| 항목 | 값 또는 의미 |
| --- | --- |
| `MINIMM_PAGE_SIZE` | 4096 byte, 변경 불가 |
| `MINIMM_PAGE_TABLE_LEVELS` | 4 |
| `MINIMM_VIRTUAL_ADDRESS_BITS` | 48-bit 모델 |
| `MINIMM_USER_ADDRESS_LIMIT` | `2^47`, 유효 주소는 이 값 미만 |
| `MINIMM_ADDRESS_AUTO` | 주소를 자동 선택하라는 sentinel |
| `MINIMM_PFN_NONE` | 아직 PTE/frame이 없다는 sentinel |

`minimm_config_default()`는 64MiB resident budget, 4096-byte page, 64-entry
TLB를 반환한다. `minimm_create()`는 page size가 정확히 4096이고 물리 메모리
크기가 그 배수이며 TLB entry 수가 0보다 큰 설정만 받는다. 여기서 물리
메모리 크기는 host RAM 예약량이 아니라 동시에 resident일 수 있는 모델
frame byte의 한도다.

모든 함수는 `minimm_status_t`를 반환하며 `MINIMM_OK`만 성공이다.
`minimm_status_string()`으로 짧은 설명을 얻을 수 있다.

| 주요 상태 | 의미 |
| --- | --- |
| `MINIMM_ERROR_INVALID_ARGUMENT` | 포인터, 정렬, 길이, flag 조합 또는 범위가 잘못됨 |
| `MINIMM_ERROR_OUT_OF_MEMORY` | metadata/frame allocation 실패 |
| `MINIMM_ERROR_NO_SPACE` | 주소, ID, backing 공간을 확보할 수 없음 |
| `MINIMM_ERROR_IO` | backing file I/O 실패 |
| `MINIMM_ERROR_BUSY` | closing 객체, pin pressure, mapping 중 note copy/축소 resize 등 |
| `MINIMM_ERROR_NOT_FOUND` | VMA/PTE가 없음 |
| `MINIMM_ERROR_PERMISSION` | protection, maximum 또는 note right 위반 |
| `MINIMM_ERROR_ADDRESS_IN_USE` | 겹치는 fixed/확장 범위 |
| `MINIMM_ERROR_UNSUPPORTED` | 알려진 API이지만 현재 모델이 지원하지 않는 조합 |

## 객체 수명

```text
minimm_create
  +-- minimm_space_create / minimm_space_fork
  +-- minimm_note_create / minimm_note_open_fd

space mapping ----retains----> backing ----retains----> note
space/note --------retain-----------------------------> system
```

- `minimm_destroy()`는 새 child 생성을 막고 system owner reference를 놓는다.
  기존 space/note가 있으면 실제 frame store 해제는 지연된다.
- mapping은 note를 retain하므로 성공한 mapping 뒤 caller가 자신의 note
  reference를 release해도 mapping은 유효하다.
- 같은 handle을 사용하는 API와 `minimm_space_destroy()` 또는 마지막
  `minimm_note_release()`를 race시키면 안 된다.
- retain된 note reference 하나마다 정확히 한 번 release해야 한다.

## 권한

### VMA protection

| flag | 허용하는 동작 |
| --- | --- |
| `MINIMM_PROT_READ` | `minimm_read`, read fault |
| `MINIMM_PROT_WRITE` | `minimm_write`, write fault |
| `MINIMM_PROT_EDIT` | `minimm_edit`, edit fault. 반드시 `WRITE`와 함께 지정 |
| `MINIMM_PROT_EXEC` | execute fault 검사. 실제 코드 실행 API는 없음 |

`protection`은 현재 권한이고 `maximum_protection`은 이후 `mprotect`로 올릴 수
있는 VM_MAY-style 상한이다. maximum을 `MINIMM_PROT_NONE`으로 두면 현재
protection만 고정하는 것이 아니라 mapping 종류와 note 권한으로 기본 상한을
계산한다. anonymous mapping은 모든 protection을 올릴 수 있고, private note
mapping은 backing이 read-only여도 private COW write를 허용할 수 있다. shared
note의 write 상한과 모든 note mapping의 edit 상한에는 대응하는 note right가
필요하다. 현재 protection은 항상 계산된 maximum의 부분집합이어야 한다.

### Note rights

note right는 `READ`, `WRITE`, `EDIT`, `SHARE`, `RESIZE`다. `EDIT`는
`WRITE`와 함께 부여해야 한다. note mapping은 항상 `READ` right를 요구하고,
shared mapping은 `SHARE`를 요구한다. shared mapping의 write 상한에는 `WRITE`,
모든 note mapping의 edit 상한에는 `EDIT`가 필요하다. private note mapping의
write는 file을 갱신하지 않는 COW이므로 note `WRITE` 없이도 허용할 수 있다.

## System과 통계 API

| API | 동작 |
| --- | --- |
| `minimm_create`, `minimm_destroy` | system/frame store 생성과 owner 수명 종료 |
| `minimm_physical_memory_size` | configured resident byte 한도 |
| `minimm_page_size`, `minimm_page_count` | 고정 page 크기와 resident page 한도 |
| `minimm_version` | CMake project version 문자열 |
| `minimm_system_get_stats` | frame 수, resident 수·한도, page-in/out과 reclaim scan/success/refault 누계 |
| `minimm_system_reclaim` | cold 우선 exact-LRU로 최대 `target_pages`를 best-effort page-out하고 scan/reclaim 결과 반환 |
| `minimm_space_get_stats` | VMA/PTE 상태와 fault/TLB 누계 |
| `minimm_space_flush_tlb` | 해당 space TLB 전체 무효화 |

system의 resident/frame 수는 unique frame 기준이다. space의 PTE/resident 수는
PTE 기준이므로 shared frame이 여러 주소에 매핑되면 각 PTE가 집계된다. 공개
`present_count`는 Linux-style 관찰을 위해 resident PTE만 세고, page-out 뒤에도
남는 내부 PTE 수는 `pte_count`로 확인한다.
`fault_sequence`는 fault handler 호출 횟수이며 실패한 fault만 세는 값은
아니다. `MAP_POPULATE`와 `mlock`의 내부 clean materialization은 access fault가
아니므로 이 sequence에 포함되지 않는다. 최초 zero materialization도 system
page-in 누계에 포함된다.

`minimm_system_reclaim()`은 non-null `minimm_reclaim_result_t`를 요구한다.
`target_pages == 0`은 `scanned_count == 0`, `reclaimed_count == 0`인 성공한
no-op이다. 양수 target은 성공 보장이 아니라 상한이다. 각 round에서 현재
resident frame을 모두 검사해 cold frame을 먼저, 같은 cold 상태에서는
`last_access`가 가장 오래된 frame을 고른다. pin되거나 짧은 access pin을 가진
frame도 scan 수에는 포함하지만 victim에서는 제외한다. eligible frame이
부족하면 `MINIMM_OK`와 target보다 작은 `reclaimed_count`를 반환한다. page-out
I/O가 실패하면 그 status와 실패 전까지의 누적 결과를 반환한다. target을
채우기 위해 round를 반복하므로 같은 pinned frame이 `scanned_count`에 여러 번
포함될 수 있으며, 이 값은 unique frame 수가 아니다.

system의 `reclaim_scan_count`는 명시적 reclaim과 resident 한도 압력에서 victim을
찾으며 검사한 resident frame 수, `reclaim_count`는 그 경로가 실제 page-out한
frame 수다. policy reclaim 뒤 nonresident frame에 남는 pending 표시는 그 frame의
첫 성공 page-in에서 한 번만 `refault_count`를 증가시키고 지워진다. 이미
resident인 frame을 다시 읽는 동작은 refault가 아니다. 직접
`MINIMM_MADV_PAGEOUT`한 frame은 `page_out_count`/후속 `page_in_count`에는
반영되지만 reclaim pending을 만들지 않아 reclaim/refault counter에서는
제외된다. 이 누계 counter는 `UINT64_MAX`에서 포화한다.

space의 dirty/locked 집계와 `minimm_query_page()`는 공유 frame 자체의
dirty/pin 상태도 반영할 수 있다. 반면 `MINIMM_MINCORE_LOCKED`는 해당 PTE가
소유한 lock bit만 나타낸다. 따라서 다른 alias가 같은 frame을 lock한 동안
query와 mincore의 lock 관찰값이 다를 수 있다.

## Immutable space inspection snapshot

`minimm_space_snapshot_t`는 한 space의 관찰값을 capture 시점에 소유권 있는
값으로 복사해 두는 불변 객체다. 현재 space를 반복해서 point-query하는 대신
VMA topology, sparse PTE와 frame 관계, 통계를 한 묶음으로 비교할 때 사용한다.

| API | 동작 |
| --- | --- |
| `minimm_space_snapshot_capture` | 현재 VMA, sparse PTE/frame 관찰값과 space 통계를 새 snapshot으로 copy-out |
| `minimm_space_snapshot_destroy` | snapshot이 소유한 copy를 해제 |
| `minimm_space_snapshot_mapping_count` | 복사된 VMA record 수를 `size_t`로 반환 |
| `minimm_space_snapshot_page_count` | 복사된 sparse PTE record 수를 `size_t`로 반환 |
| `minimm_space_snapshot_vma_generation` | capture한 VMA snapshot generation을 `uint64_t`로 반환 |
| `minimm_space_snapshot_page_table_generation` | capture한 page-table generation을 `uint64_t`로 반환 |
| `minimm_space_snapshot_get_stats` | capture 시점의 `minimm_space_stats_t`를 copy-out |
| `minimm_space_snapshot_get_mapping` | index의 `minimm_mapping_info_t`를 주소순으로 copy-out |
| `minimm_space_snapshot_get_page` | index의 `minimm_space_snapshot_page_t`를 page 주소순으로 copy-out |

scalar getter는 null snapshot에 0을 반환한다. value-copy getter는 null 입력을
`MINIMM_ERROR_INVALID_ARGUMENT`, 배열 범위 밖 index를 `MINIMM_ERROR_NOT_FOUND`로
보고한다. `minimm_space_snapshot_destroy(NULL)`은 no-op이다. generation은 같은
space의 capture 사이에서 VMA publication과 PTE mutation을 비교하는 model
counter이며, wall-clock이나 서로 다른 space 사이의 전역 순서가 아니다. frame
residency처럼 page table을 바꾸지 않는 전이는 generation만 보지 말고 page
record도 비교해야 한다.

mapping 배열은 VMA 시작 주소순이고 page 배열은 실제 설치된 sparse PTE만 page
주소순으로 가진다. 따라서 큰 VMA의 아직 fault하지 않은 page를 열거하거나 PTE로
만들지 않으며, `mapping_count`와 `page_count`는 서로 독립적이다. VMA-only
상태에서는 mapping count가 양수여도 page count가 0일 수 있다.

`minimm_space_snapshot_page_t`는 기존 `minimm_page_info_t page`를 중첩해
protection, present/resident, dirty/accessed, COW/shared/locked/cold 상태를
보존하고, 다음 관계값을 함께 둔다.

- `mapping_cookie`는 record의 page가 속한 copied VMA와 연결된다.
- `frame_cookie`는 snapshot용 frame object identity다. resident page의
  synthetic PFN과 달리 page-out 뒤에도 같은 frame이면 유지되며, COW split은
  서로 다른 값으로 보인다.
- `frame_mapping_count`는 capture 시점에 그 physical frame을 가리킨 PTE 수다.

snapshot은 space, page table 또는 frame에 대한 live pointer를 공개하거나
보유하지 않는다. capture가 성공하면 원래 space를 변경하거나
`minimm_space_destroy()`한 뒤에도 getter와 destroy를 사용할 수 있다. 이후
fault, fork, COW, page-out은 기존 snapshot 값을 바꾸지 않으므로 서로 다른
시점의 generation, count와 record를 직접 비교할 수 있다. capture 자체는
fault, page-in, TLB fill 또는 accessed/dirty 갱신을 만들지 않는다.

이 기능은 값 기반 MiniMM inspection API다. Linux `/proc/*/maps`,
`/proc/*/pagemap`의 형식·권한·PFN ABI가 아니고, 여러 space와 frame store를
포함한 kernel 전역 상태를 한 순간에 정지시키는 global atomic snapshot도
아니다. 특히 shared frame에서 다른 space의 동시 접근으로 바뀌는 frame 상태를
Linux rmap 전체의 원자적 view로 해석하면 안 된다.

## Note API

| API | 동작과 조건 |
| --- | --- |
| `minimm_note_create` | page-aligned 크기의 unlink된 임시 file note 생성 |
| `minimm_note_copy` | mapping이 없는 `READ` source의 같은 크기 독립 snapshot 생성. temporary source는 lazy page COW |
| `minimm_note_open_fd` | page-aligned regular FD를 내부 `CLOEXEC` descriptor로 보유하여 note 생성 |
| `minimm_note_retain/release` | 명시적 reference 관리 |
| `minimm_note_id/size/rights` | 불변 ID, 현재 크기, 부여된 권한 조회 |
| `minimm_note_resize` | page-aligned resize. mapping 중 확장은 허용하고 축소는 `BUSY` |
| `minimm_note_pread/pwrite/pedit` | note cache를 통한 범위 I/O |
| `minimm_note_flush` | dirty shared frame write-back 후 `fsync` |

note 크기는 0을 포함한 4KiB 배수여야 한다. `open_fd`는 page-aligned 크기의
regular file만 받는다. backing page-in이 필요하므로 access mode는 `O_RDONLY`나
`O_RDWR`여야 하고 write/edit/resize right에는 `O_RDWR` FD가 필요하다.
`O_PATH` FD는 실제 page I/O가 불가능하므로 거부한다. Linux에서 `O_APPEND`는
`pwrite()`의 지정 offset도 무시하므로 append-mode FD에는 write/edit right를
부여할 수 없으며 호출자 FD의 `O_APPEND`를 임의로 지우지도 않는다. Linux에서
write right를 부여할 때는 검증용 duplicate를 `/proc/self/fd/<fd>`를 통해
`O_RDWR | O_CLOEXEC`로 다시 열고 device/inode가 같은지 확인한다. 이렇게 얻은
독립 open-file-description은 성공한 open 뒤 호출자가 원본 FD에
`F_SETFL(O_APPEND)`를 적용해도 note의 positioned writeback에 전파되지 않는다.
unlink된 regular file도 원본 FD와 procfs link가 남아 있고 재open 권한이 있으면
지원한다. 단, 재open은 현재 credential과 inode permission을 다시 검사하므로
권한이 사라졌거나 `/proc/self/fd`를 사용할 수 없으면 원본 descriptor를 공유하는
대신 해당 errno에 대응하는 오류로 `minimm_note_open_fd()`가 실패한다. Linux 외
플랫폼에서는 내부 `CLOEXEC` duplicate를 보유한다.
호출자의 원본 FD 소유권은 이전되지 않는다. I/O 범위는 note 크기 안에
완전히 들어와야 하며, 중간 오류가 나면 `out_completed`에 완료 byte 수가
남는다.

`minimm_note_copy(source, rights, out_note)`는 source에 `READ` right가 있고
mapping이 없을 때, 같은 COW lineage를 사용하는 note I/O와 resize를
직렬화한다. temporary source는 이 lock 경계의 크기와 내용을 snapshot한다.
mapping이 붙어 있으면 mapping write가 lineage lock에 참여하지 않아 원자적
snapshot을 보장할 수 없으므로 `MINIMM_ERROR_BUSY`다. destination은 지정한
`rights`를 가진 새 note이며 성공 뒤 어느 한쪽의 쓰기, 편집 또는 크기 변경도
다른 쪽에 반영되지 않는다. 외부 FD source의 동시 writer 계약은 아래와 같다.

`minimm_note_create()`로 만든 temporary source는 COPY 때 page byte를 복사하지
않는다. 새 sparse backing과 parent/child lineage만 연결하는 O(1) metadata
snapshot이며, child read는 아직 분리되지 않은 page를 ancestor에서 찾는다.
source가 그 page를 처음 변경하기 전에는 child용 기존 byte를 보존하고, child의
첫 write/edit는 자기 overlay frame으로 분리한다. 여러 세대의 COPY와 ancestor
reference release 뒤에도 lineage reference가 snapshot 수명을 유지한다.

반면 `minimm_note_open_fd()`로 연 외부 source에는 lineage lock을 우회하는 외부
writer가 있을 수 있다. 이 경우에만 COPY 시 전체 page를 destination backing에
쓰는 eager fallback을 사용해 COPY 완료 뒤의 외부 변경과 destination을
격리한다. 외부 writer가 COPY와 동시에 여러 page를 바꾸면 page별 read/cached
view가 섞일 수 있어 atomic whole-file snapshot은 보장하지 않는다. 성공한
`out_note` reference의 소유권은 caller에게 있고 일반 note와 같이
`minimm_note_release()`해야 한다.

mapping이 붙은 note의 확장 resize는 기존 mapping 범위를 바꾸지 않고 새 구간을
zero-fill하므로 허용한다. 기존 mapping이 file 끝 밖을 가리킬 수 있는 축소는
`MINIMM_ERROR_BUSY`다. mapping이 없을 때 lineage의 어느 note를 줄여도 child가
상속한 잘린 page를 먼저 보존하므로 source와 snapshot의 크기·byte는 독립적이다.

동일한 note object의 cache를 통하지 않은 외부 FD 변경은 자동 invalidate되지
않는다. 같은 file을 별도 note object로 여러 번 열어도 cache coherence를
보장하지 않는다. 외부 변경을 관찰해야 한다면 mapping과 handle을 종료한 뒤
note를 다시 여는 방식으로 경계를 명시해야 한다.

## 원격 Note API

`minimm_server_*` API는 하나의 `minimm_t`와 TCP listener를 소유하고 note를
연결별 opaque handle로 노출한다. `minimm_client_*` API는 mandatory HELLO를
수행한 뒤 한 연결의 요청을 mutex로 직렬화한다. public handle 값은 그 handle을
발급한 `minimm_client_t`에서만 유효하며 다른 연결의 같은 숫자와 관계없다.

| API | 동작 |
| --- | --- |
| `minimm_server_config_default` | loopback, port 7331과 자원 상한을 가진 기본 설정 |
| `minimm_server_create/start/bound_port` | 서비스와 listener 생성, port 0이면 ephemeral port 선택 |
| `minimm_server_stop/destroy` | listener와 client socket을 닫고 worker를 drain한 뒤 공유 registry 해제 |
| `minimm_client_connect/disconnect` | TCP 연결, HELLO와 negotiated limit 설정, 연결 수명 종료 |
| `minimm_client_ping` | nonce 왕복으로 연결 확인 |
| `minimm_client_note_create/open/close` | note 생성, capability로 열기, 연결별 handle 닫기 |
| `minimm_client_note_copy` | `READ` handle의 독립 snapshot을 새 record와 capability로 생성 |
| `minimm_client_note_stat/read/write/edit` | handle metadata와 범위 I/O |
| `minimm_client_note_preview` | 한 page의 private view에 입력을 적용하고 폐기 |
| `minimm_client_note_stack_expand` | 한 page의 transient private stack marker 적용과 폐기 |
| `minimm_client_note_remap_page` | transient shared view의 file page remap protection 조회 |
| `minimm_client_note_mseal_merge` | transient mseal merge의 page·range metadata 조회 |
| `minimm_client_note_mglru_reparent` | transient MGLRU reparent accounting metadata 조회 |
| `minimm_client_note_rmap_unmap` | transient rmap unmap batch metadata 조회 |
| `minimm_client_note_uffd_move` | transient userfaultfd move metadata 조회 |
| `minimm_client_note_hugetlb_reserve` | transient hugetlb reservation metadata 조회 |
| `minimm_client_note_percpu_populate` | transient per-CPU population metadata 조회 |
| `minimm_client_note_resize/flush/unlink` | page-aligned resize, file flush, capability registry 제거 |
| `minimm_capability_format/parse` | 16-byte token과 32자리 hex 변환 |

remote right는 core note right와 같은 `READ/WRITE/EDIT/SHARE/RESIZE`에
protocol 전용 `DELETE`를 더한다. CREATE 또는 COPY의 destination에 `SHARE`를
부여해야 128-bit capability가 registry에 연결된다. `SHARE`가 없으면 token은
모두 0이고 그 연결의 마지막 handle이 닫힐 때 note가 사라진다. `UNLINK` 뒤에도
이미 열린 handle은 닫을 때까지 유효하지만 새 OPEN은 `NOT_FOUND`다.

`minimm_client_note_copy(client, source_handle, rights, out_note)`는 source
handle에 `READ`를 요구하고, 지정한 최대 권한의 새 record, handle, capability를
반환한다. source의 같은 크기·내용을 한 시점에 snapshot하며 이후 두 record의
변경은 서로 독립적이다. COPY도 일반 note 하나로 계산되어 서버의 live-record
수와 전체 logical-byte quota를 모두 소비한다. 서버가 만든 note는 temporary
source이므로 COPY 자체는 page byte를 복사하지 않는 O(1) metadata lineage
연산이고, 이후 변경되는 page만 COW로 분리한다.

capability는 note record를 만들 때 지정한 최대 권한 전체를 나타내는 bearer
secret이다.
OPEN의 `requested_rights`는 새 handle 권한만 줄일 뿐 제한된 새 token을 만들지
않는다. 따라서 같은 token을 받은 상대는 생성 시 허용된 다른 권한으로 다시
OPEN할 수 있다. v1은 TLS, 계정, ACL, 영구 저장을 제공하지 않으며 서버 stop
또는 process 종료 시 registry가 사라진다. 자세한 수명·보안·CLI 규칙은
[서비스 안내](service.md), byte 형식은 [프로토콜 명세](protocol.md)를 본다.

client의 read/write/edit에서 `out_completed`는 생략할 수 있다. 길이 0도 한 번은
서버에 요청해 handle, 권한과 offset을 검증하며 성공 시 완료 길이는 0이다. 하나의
client 객체에 대한 호출은 내부 직렬화되지만 `minimm_client_disconnect()`를 같은
객체의 다른 호출과 race시키면 안 된다. connect timeout은 socket address 시도와
이후 I/O를 제한하지만 system DNS resolver 호출 시간까지 보장하지 않는다.
preview는 길이 1~4096이고 page를 넘지 않는 범위만 받는다. handle에는 `READ`가
필요하고 `WRITE`나 `EDIT`가 있으면 거부된다. 서버의
`enable_private_preview`가 false면
`MINIMM_ERROR_UNSUPPORTED`다. 성공한 preview는 private view만 변경하고 원본
note bytes는 보존해야 한다.
stack expansion도 같은 nonempty single-page 범위 제한을 사용하며 서버의
`enable_stack_expand`가 false면 지원되지 않는다. remap page는 page-aligned
note offset과 `READ|WRITE|SHARE` handle을 요구하고, 서버의 `enable_page_remap`이
false면 지원되지 않는다. 성공 시 `out_protection`은 host protection이 아닌
transient MiniMM VMA의 protection bit다.
`minimm_client_note_mseal_merge()`는 `READ|WRITE|SHARE` handle을 요구하며 서버의
`enable_mseal_merge`가 false면 지원되지 않는다. 성공 결과는 bounded transient
model의 total/sealed page 수, range validity와 page-aligned cursor 두 개를
제공한다. host 주소나 host `mseal(2)`을 사용하지 않으며 note byte도 보존한다.
`minimm_client_note_mglru_reparent()`는 `READ|WRITE|SHARE` handle을 요구하며 서버의
`enable_mglru_reparent`가 false면 지원되지 않는다. 성공 결과는 bounded transient
model의 total page 수, parent generation count, child의 대응하는 debt/credit과
clean/valid 상태를 제공한다. host memcg/LRU를 사용하지 않으며 note byte도
보존한다.
새 security model API 네 개도 정확히 4096-byte인 `READ|WRITE|SHARE` note를
요구하며 각 대응 server flag가 false면 `MINIMM_ERROR_UNSUPPORTED`를 반환한다.
각 API는 명시적인 bounded 정수 입력에 대한 transient model 결과를 반환한다.
모두 metadata-only 연산이고 host page table, swap, hugepage 또는 per-CPU allocator를
사용하지 않으며 note byte도 보존한다.
`minimm_client_note_resize()`는 실제 resize를 시도한 뒤 실패한 응답에 progress
payload가 있으면 변경되지 않은 크기를 `out_actual_size`로 반환한다. handle·권한
같은 사전 검증 오류가 빈 payload로 오면 출력은 0이다.

## Address space와 fork

| API | 동작 |
| --- | --- |
| `minimm_space_create/destroy` | 독립 VMA, page table, TLB를 가진 space 생성/해제 |
| `minimm_space_fork` | VMA/backing을 복제하고 향후 쓰기 가능한 private PTE를 COW로 전환 |
| `minimm_brk` | `requested_end == 0`이면 조회, 아니면 break 변경 |
| `minimm_sbrk` | signed 증감량을 적용하고 이전 break 반환 |

fork 후 shared mapping은 같은 backing/frame을 계속 사용한다. maximum
protection에 `WRITE`/`EDIT`가 있는 private mapping의 faulted page는 parent와
child가 frame을 공유하다가 첫 write/edit에서 4KiB copy를 만든다. maximum이
영구 read-only인 private mapping은 frame만 공유하고 불필요한 COW flag를 만들지
않는다. 아직 fault되지 않은 private anonymous page는 각 space에서 독립적으로
zero-fill된다. 부모의 memory lock은 child PTE에 상속되지 않으며 child TLB는
새로 비어 있는 상태다.

## Mapping API

`minimm_mmap_args_t`의 핵심 규칙은 다음과 같다.

- `length`는 0보다 커야 하며 내부에서 page 단위로 올림한다.
- `MINIMM_MAP_SHARED`와 `MINIMM_MAP_PRIVATE` 중 정확히 하나를 지정한다.
- anonymous mapping은 `MINIMM_MAP_ANONYMOUS`, `note == NULL`, offset 0을
  함께 사용한다.
- note mapping은 `MINIMM_MAP_ANONYMOUS`를 빼고 page-aligned note offset과
  note를 지정하며, 올림된 전체 길이가 note 안에 있어야 한다.
- non-fixed hint가 비어 있으면 그 주소를 먼저 사용하고, 겹치면 gap을 찾는다.
- `MINIMM_MAP_FIXED`는 겹친 범위를 교체하고,
  `MINIMM_MAP_FIXED_NOREPLACE`는 겹치면 `ADDRESS_IN_USE`를 반환한다.
- `MINIMM_MAP_POPULATE`는 protection이 있는 page를 clean하게 미리
  materialize하려는 best-effort hint다. fixed mapping과도 조합할 수 있다.
  prefault 중 자원 부족이나 I/O 오류가 나도 성공한 VMA를 rollback하거나 mmap
  자체를 실패시키지 않으며, 준비하지 못한 page는 이후 접근 때 fault한다.
  materialize된 page도 access/dirty flag, COW 분리, fault sequence를 만들지
  않는다.

| API | 동작 |
| --- | --- |
| `minimm_mmap` | anonymous/note VMA 생성, 선택적으로 populate |
| `minimm_munmap` | 범위를 제거하고 VMA split, PTE/TLB 제거 |
| `minimm_mprotect` | 완전히 매핑된 범위의 권한 변경과 VMA split |
| `minimm_page_protect` | 정확히 한 page에 대한 `mprotect` 편의 API |
| `minimm_mapping_query` | 주소를 포함하는 `[start,end)` VMA metadata copy-out |
| `minimm_mapping_copy` | 같은 space의 private VMA 전체를 새 주소에 frame 공유/COW 복제 |
| `minimm_mremap` | 정확히 하나의 전체 VMA를 shrink/grow, MAYMOVE 시 이동 |

`mapping_query`의 flags는 현재 shared/private 분류를 나타낸다. fixed,
anonymous, populate는 생성 방식이지 보존되는 VMA 속성이 아니다.

`minimm_mapping_copy(space, source_address, length, destination_hint,
out_address)`는 0보다 큰 `length`를 page 단위로 올린 source range가 정확히
하나의 private VMA 전체와 일치할 때만 같은 space 안에 별도의 VMA를 만든다.
shared VMA 또는 split된 일부 range의 복사는 `MINIMM_ERROR_UNSUPPORTED`다.
`destination_hint`가
`MINIMM_ADDRESS_AUTO`가 아니고 전체 range가 비어 있으면 그 주소를 우선한다.
유효한 hint가 기존 mapping과 겹치면 일반 자동 mapping과 같이 빈 gap을 찾으며,
기존 mapping을 교체하지 않는다. 정렬되지 않았거나 사용자 주소 범위를 벗어나는
hint는 `MINIMM_ERROR_INVALID_ARGUMENT`다.

destination은 source와 같은 backing, note offset, 현재/최대 protection 및
private 속성을 유지하지만 주소 범위와 `mapping_cookie`는 새로 받는다. 이미
설치된 source PTE는 resident 여부와 관계없이 destination과 같은 frame을
공유한다. maximum protection에 `WRITE`/`EDIT`가 있으면 양쪽 모두 effective
write-protected COW로 전환되어 어느 쪽이든 처음 write/edit할 때 4KiB private
copy로 분리된다. 영구 read-only이면 `fork`와 같이 불필요한 COW flag 없이
frame만 공유한다. 아직 PTE가 없는 page를 이 호출이 materialize하지는 않는다.
source의 PTE lock/accessed/dirty 상태는 유지된다. destination PTE는 clean, old,
unlocked private alias로 시작하므로 `LOCKED`, `ACCESSED`, `DIRTY`, `SHARED`를
상속하지 않으며, 향후 writable일 때만 `COW`가 설정된다.

snapshot, binding 또는 page-table 준비가 실패하면 source VMA/PTE와 frame
소유 관계는 유지되고 destination 일부도 남지 않는다. 실패 시 `out_address`는
`MINIMM_ADDRESS_AUTO`이며, 성공한 경우에만 선택한 destination 시작 주소를
받는다. 성공 시 source와 destination range의 software TLB entry를 명시적으로
무효화한다.

`mremap`의 old range는 정확히 VMA 전체와 일치해야 한다. 인접 공간이 있으면
제자리 확장하고, 없을 때 `MINIMM_MREMAP_MAYMOVE`가 있으면 새 gap으로 PTE를
옮긴다. non-fixed 호출의 `new_address_hint`는 반드시
`MINIMM_ADDRESS_AUTO`여야 한다. `MINIMM_MREMAP_FIXED`는 `MAYMOVE`와 함께
사용하며 지정한 destination의 기존 mapping을 교체하고 PTE 이동 중 실패하면
원래 source/destination을 복구한다.

`MINIMM_MREMAP_DONTUNMAP`은 `MAYMOVE`와 함께 같은 크기의 private anonymous
단일 VMA에만 지원한다. PTE와 lock 상태는 destination으로 이동하고 source VMA와
backing은 남는다. 이후 source 접근은 anonymous zero page로 다시 fault한다.
`FIXED|DONTUNMAP` 조합은 지정 destination을 교체한다. Linux의 더 넓은
`DONTUNMAP` 대상, `old_length == 0` shareable clone mode, 여러 VMA를 가로지르는
remap은 `MINIMM_ERROR_UNSUPPORTED`다.

## Fault, 접근, 변환

| API | 동작 |
| --- | --- |
| `minimm_handle_page_fault` | 한 주소와 정확히 한 access 종류를 명시적으로 처리 |
| `minimm_space_get_fault_trace` | 최근 handler 결과를 오래된 순서부터 고정 크기 snapshot으로 복사 |
| `minimm_space_clear_fault_trace` | 보관된 trace와 overwrite 누계만 초기화 |
| `minimm_read/write/edit` | page 경계를 나누고 fault를 자동 처리하며 byte 복사 |
| `minimm_query_page` | VMA/PTE/frame의 present, resident, PFN, dirty, COW, cold 등 조회 |
| `minimm_translate` | resident PTE의 synthetic PFN과 12-bit offset 조회 |

fault access는 `READ`, `WRITE`, `EDIT`, `EXECUTE` 중 정확히 한 bit여야 한다.
결과에는 sequence, 원주소, page 주소, 원인과 해결 방식이 기록된다. 대표적인
해결은 `ZERO_FILLED`, `PAGE_IN`, `COW_COPIED`, `DENIED`, `NO_ACTION`이다.
완료된 결과의 `origin`은 직접 호출한 handler면
`MINIMM_FAULT_ORIGIN_EXPLICIT`, `minimm_read/write/edit`가 resolution을
요청했으면 `MINIMM_FAULT_ORIGIN_ACCESS`다. `MINIMM_FAULT_ORIGIN_NONE`은 아직
결과가 없는 zero-initialized 구조체를 나타낸다. handler 입력 검증 자체가
실패하면 `out_fault`는 갱신되지 않으므로 그 내용을 결과로 해석하지 않는다.
private anonymous page와 shared-anonymous backing의 최초 생성만 `ZERO_FILLED`다.
PTE가 없는 alias가 이미 존재하는 shared backing에 연결되거나 page-out된 그
backing을 복원하면 byte를 새로 0으로 만들지 않으므로 `PAGE_IN`으로 기록한다.

space마다 마지막 `MINIMM_FAULT_TRACE_CAPACITY`개 handler 결과를 allocation 없는
ring에 보관한다. 자동 접근의 demand-zero/page-in/COW와 VMA permission/hole
denial, 명시적 handler의 성공·실패가 모두 포함되고 snapshot의 `events`는
oldest-to-newest 순서다. ring이 찬 뒤 밀려난 수는 `overwritten_count`에 마지막
clear 이후 누계로 남고 `UINT64_MAX`에서 포화한다. clear는 보관분만 지우므로
space lifetime의 `fault_sequence`는 되돌리지 않는다. fork child는 빈 trace와
sequence 0으로 시작하며 parent trace는 바뀌지 않는다.

입력 검증에서 거부된 잘못된 주소/access bit와 closing space 호출은 handler에
들어가지 않아 기록되지 않는다. resident TLB hit와 resident PTE refill,
`MAP_POPULATE`, `mlock`, `MINIMM_MADV_WILLNEED`의 clean materialization도
fault가 아니므로 trace에 없다. sequence는 극단적으로 wrap할 수 있으므로
overwrite 수를 sequence 차이로 추정하지 말고 `overwritten_count`를 사용한다.

`translate`와 `query_page`는 demand fault를 만들지 않는다. VMA만 있고 PTE가
없거나 PTE frame이 page-out된 경우 query는 성공하면서 `present == false`,
PFN은 `MINIMM_PFN_NONE`이고, translate는 `NOT_FOUND`다. nonresident PTE의
COW/dirty/accessed/locked 같은 software metadata는 query에 계속 보일 수 있다.
synthetic PFN은 resident frame 식별자일 뿐 host pointer가 아니다.
`cold`는 로컬 PTE bit가 아니라 underlying physical frame의 reclaim 우선순위다.
따라서 shared alias는 같은 cold 값을 관찰하지만 각 PTE의 `accessed` 값은 서로
다를 수 있다. PTE가 없거나 frame이 nonresident이면 `cold == false`다.
여러 VMA와 sparse PTE를 같은 관찰 묶음으로 보존해야 한다면 point query를
반복하는 대신 `minimm_space_snapshot_capture()`를 사용한다.

여러 page를 읽거나 쓰다가 뒤쪽 page에서 권한/매핑/I/O 오류가 나면 API는
오류를 반환하고 앞에서 완료한 byte 수를 `out_completed`에 기록한다.

## Linux 유사 memory API

`msync`, `mincore`, `madvise`, `mprotect`는 page-aligned 시작 주소를 요구하고,
양수 길이는 page 단위로 올림한다. `mlock`과 `munlock`은 Linux처럼 정렬되지
않은 시작 주소를 아래 page 경계로 내리고 끝을 위로 올린다. 모든 API에서
길이 0은 주소 범위만 검증하는 no-op이다. 양수 range는 각 API 계약에 따라
매핑 여부를 검사한다.

| API | 현재 의미 |
| --- | --- |
| `minimm_msync` | shared note range의 dirty frame을 FD로 write-back하고 로컬 dirty PTE flag를 clear |
| `minimm_mincore` | page별 Linux-style residency bit 0과 MiniMM 진단 bit 반환 |
| `minimm_mlock` | page를 clean page-in하고 pin. 부분 실패 시 새 PTE를 제거하고 기존 PTE frame/protection/flag를 복구 |
| `minimm_munlock` | range의 model pin 해제 |
| `MINIMM_MADV_WILLNEED` | 기존 PTE를 page-in하고 note/anonymous-shared backing cache를 미리 읽음. PTE 없는 anonymous-private page는 materialize하지 않음 |
| `MINIMM_MADV_DONTNEED` | 로컬 unlocked PTE 제거. 다른 alias가 같은 frame을 pin해도 그 로컬 PTE는 제거할 수 있으며 private anonymous/COW 수정은 버려질 수 있음 |
| `MINIMM_MADV_PAGEOUT` | 로컬 lock 또는 공유 frame pin이 없는 frame byte를 backing으로 내리고 PTE는 유지 |
| `MINIMM_MADV_COLD` | 설치된 로컬 PTE의 `ACCESSED`를 clear하고 resident physical frame을 cold 우선순위로 표시. page-in이나 즉시 page-out은 하지 않음 |
| `MINIMM_MADV_NORMAL/RANDOM/SEQUENTIAL` | 현재 validation 뒤 no-op |

`minimm_msync`는 Linux의 모든 flag/동기화 모드를 복제하지 않는다. shared note
외 mapping에는 write-back 동작이 없으며, storage-level `fsync`가 필요하면
`minimm_note_flush()`를 사용한다.

`minimm_mincore()` 결과의 bit 0인 `MINIMM_MINCORE_RESIDENT`만 Linux-compatible
residency 계약이다. VMA에 로컬 PTE가 없어도 같은 note/anonymous-shared backing
cache에 resident frame이 있으면 이 bit가 설정될 수 있다. bit 1 이상의
`PRESENT`, `DIRTY`, `LOCKED`, `SHARED`, `COW`, `ACCESSED`는 MiniMM 전용 진단값이다.
여기서 `PRESENT`는 resident인 로컬 PTE를 뜻하며 nonresident PTE에는 설정되지
않는다. 더 명확한 metadata가 필요하면 `minimm_query_page()`를 사용한다.

`MINIMM_MADV_COLD`는 untouched VMA에 PTE를 만들거나 frame을 page-in하지 않는다.
대상 PTE의 local `ACCESSED` bit와 TLB entry만 clear/invalidate하지만 cold 상태는
frame에 있으므로 같은 physical frame을 보는 모든 alias의
`minimm_page_info_t.cold`와
inspection snapshot에 보인다. 다른 alias의 PTE `accessed` bit는 지우지 않는다.
어느 alias든 frame을 실제로 다시 사용하면 frame은 warm으로 돌아가지만 그
접근이 다른 PTE의 local accessed bit까지 바꾸지는 않는다. locked frame에도
cold 표시는 할 수 있으나 explicit/automatic reclaim은 pin이 풀릴 때까지 그
frame을 victim으로 고르지 않는다. COLD 자체는 fault trace나 paging/reclaim
counter를 증가시키지 않는다.

실패한 `minimm_mlock()`의 PTE 상태 복구와 frame cache residency 복구는
구분한다. 새 transient frame/PTE와 pin은 제거되지만, 기존 nonresident frame을
page-in했거나 shared backing cache를 조회한 효과는 남을 수 있다. 즉 실패 후
byte, PFN, protection, COW/dirty/accessed/locked PTE 상태는 보존되지만 cache가
따뜻해질 수 있다.

## 짧은 예제

fault/COW/reclaim/immutable inspection/working-set 상태를 출력하고 자체
검증하는 전체 예제는
[`examples/mm_tour.c`](../examples/mm_tour.c)와 [MM tour](mm-tour.md)에 있다.

```c
#include <minimm/minimm.h>

int example(void)
{
    static const char text[] = "shared note";
    minimm_config_t config = minimm_config_default();
    minimm_t *mm = NULL;
    minimm_space_t *space = NULL;
    minimm_note_t *note = NULL;
    minimm_vaddr_t address = MINIMM_ADDRESS_AUTO;
    size_t completed = 0U;
    minimm_mmap_args_t args = {0};

    if (minimm_create(&config, &mm) != MINIMM_OK ||
        minimm_note_create(
            mm, MINIMM_PAGE_SIZE, MINIMM_NOTE_RIGHT_ALL, &note
        ) != MINIMM_OK ||
        minimm_space_create(mm, &space) != MINIMM_OK) {
        goto failure;
    }

    args.address_hint = MINIMM_ADDRESS_AUTO;
    args.length = MINIMM_PAGE_SIZE;
    args.protection = MINIMM_PROT_READ | MINIMM_PROT_WRITE |
        MINIMM_PROT_EDIT;
    args.maximum_protection = args.protection;
    args.flags = MINIMM_MAP_SHARED;
    args.note = note;

    if (minimm_mmap(space, &args, &address) != MINIMM_OK ||
        minimm_edit(space, address, text, sizeof(text), &completed) !=
            MINIMM_OK ||
        minimm_msync(space, address, MINIMM_PAGE_SIZE) != MINIMM_OK ||
        minimm_note_flush(note) != MINIMM_OK) {
        goto failure;
    }

    minimm_space_destroy(space);
    minimm_note_release(note);
    minimm_destroy(mm);
    return 0;

failure:
    minimm_space_destroy(space);
    minimm_note_release(note);
    minimm_destroy(mm);
    return 1;
}
```

## 제약과 호환성

- API 이름은 Linux 개념을 빌렸지만 syscall 번호, `errno`, signal, ABI 및
  세부 corner case 호환성을 제공하지 않는다.
- MiniMM virtual address를 host address에 직접 mapping하지 않으며 모든 모델
  접근은 `minimm_read/write/edit`를 거친다. 내부 resident byte arena의 host
  `mmap(2)`은 이 주소 변환·권한 모델과 별개다.
- `MINIMM_MREMAP_FIXED`는 지원하지만 source는 정확히 하나의 전체 VMA여야
  한다. `old_length == 0` clone과 여러 VMA remap은 지원하지 않는다.
- `MINIMM_MREMAP_DONTUNMAP`은 같은 크기의 private-anonymous VMA subset에만
  지원한다.
- `MINIMM_MAP_POPULATE`는 fixed 조합을 포함해 best-effort이며 prefault 실패를
  mmap 실패나 VMA rollback으로 보고하지 않는다.
- `minimm_mapping_copy()`는 같은 space에 있는 하나의 private VMA 전체만
  복제하며 shared VMA와 부분 VMA 복사는 지원하지 않는다.
- mapping 중인 note의 copy와 축소 resize는 `MINIMM_ERROR_BUSY`이고 확장은
  허용한다.
- 외부 FD writer 및 서로 다른 note object 사이의 cache coherence는 보장하지
  않는다.
- 임의 길이 file tail을 mapping해 Linux의 partial-EOF zero-fill 및 이후
  `SIGBUS`를 재현하지 않는다. note 크기와 offset은 4KiB 경계에 맞아야 한다.
- destroy/release와 같은 handle의 다른 API를 동시에 호출할 수 없다.
- 이 모델의 통계와 timing은 실제 Linux MM 성능 수치로 해석하면 안 된다.

더 넓은 subsystem 차이는 [Linux MM 대응표](linux-mm-parity.md)에 정리한다.
