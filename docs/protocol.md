# miniMM wire protocol v1

이 문서는 TCP note protocol 1.0의 실제 wire 형식을 정의한다. 모든 multibyte
integer는 unsigned network byte order(big-endian)이고 C struct padding을 wire에
복사하지 않는다. 아래의 `offset:type` offset은 payload 시작을 0으로 센다.

## Frame header

모든 frame은 고정 32-byte header 뒤에 `payload_length` bytes가 이어진다.

| offset | 크기 | 필드 | v1 규칙 |
| ---: | ---: | --- | --- |
| 0 | 4 | `magic` | `0x4d494d4d`, ASCII `MIMM` |
| 4 | 1 | `major` | `1` |
| 5 | 1 | `minor` | `0` |
| 6 | 2 | `header_size` | `32` |
| 8 | 2 | `opcode` | 아래 opcode 표의 값 |
| 10 | 2 | `flags` | request `0`, response `0x0001` |
| 12 | 4 | `wire_status` | request `0`, response는 status 값 |
| 16 | 8 | `request_id` | 0이 아니어야 하며 response가 그대로 반사 |
| 24 | 4 | `payload_length` | 뒤따르는 payload의 byte 수 |
| 28 | 4 | `reserved` | `0` |

정의된 header flag는 `RESPONSE(0x0001)`뿐이다. 알 수 없는 flag, 0인 request
ID, 잘못된 magic/header size/reserved, 또는 1,048,600 bytes보다 큰 payload
길이는 invalid header다.

request는 TCP 연결 하나에서 순서대로 처리되며 response의 opcode와 request ID는
request와 같다. HELLO가 광고하는 `max_inflight`는 1이고 공식 client도 client
mutex 아래 request/response exchange를 하나씩 수행한다.

## Handshake와 negotiated limit

연결의 첫 request는 반드시 `HELLO`여야 한다. frame header 자체의 version은
처음부터 1.0이어야 하며, HELLO payload가 표현하는 `[min, max]` 범위에도 1.0이
포함되어야 한다.

HELLO request, 16 bytes:

```text
0:u8  min_major
1:u8  min_minor
2:u8  max_major
3:u8  max_minor
4:u32 feature_bits          # v1 client는 0, 서버 응답도 0
8:u32 receive_max_payload   # 최소 40
12:u32 reserved             # 0
```

HELLO response, 32 bytes:

```text
0:u8  selected_major        # 1
1:u8  selected_minor        # 0
2:u16 reserved              # 0
4:u32 feature_bits          # 0
8:u32 negotiated_max_payload
12:u32 max_handles
16:u32 page_size             # 4096
20:u32 max_inflight          # 1
24:u64 max_note_size
```

`negotiated_max_payload`는 client의 `receive_max_payload`와 서버 설정값 중 작은
값이다. v1 hard maximum은 `1MiB + 24 = 1,048,600` bytes이고 최소값은 가장 큰
고정 응답인 CREATE/COPY response 크기 40이다. 이후 모든 request와 response
payload가 negotiated maximum 이하여야 한다. data field `N` 자체는 최대
1MiB다.
v1 feature bit는 정의되어 있지 않으므로 request의 nonzero `feature_bits`는
`INVALID_ARGUMENT`로 거부한다.

HELLO 전에 다른 opcode를 보내거나 HELLO를 두 번 보내면
`MALFORMED_MESSAGE`다. handshake 실패 응답 뒤에는 서버가 연결을 닫는다.

## Opcode

| 값 | 이름 | 기능 |
| ---: | --- | --- |
| `0x0001` | `HELLO` | version과 limit 협상 |
| `0x0002` | `PING` | nonce 왕복 |
| `0x0100` | `CREATE` | note 생성과 첫 handle 발급 |
| `0x0101` | `OPEN` | capability로 새 handle 발급 |
| `0x0102` | `CLOSE` | handle 닫기 |
| `0x0103` | `STAT` | handle의 크기·권한 조회 |
| `0x0104` | `READ` | byte 범위 읽기 |
| `0x0105` | `WRITE` | byte 범위 쓰기 |
| `0x0106` | `EDIT` | EDIT 권한을 추가로 검사해 byte 범위 쓰기 |
| `0x0107` | `RESIZE` | note 크기 변경 |
| `0x0108` | `FLUSH` | dirty note frame을 backing에 반영 |
| `0x0109` | `UNLINK` | capability registry entry 제거 |
| `0x010a` | `COPY` | 독립 note snapshot과 첫 handle 발급 |
| `0x010b` | `PREVIEW` | private view에 byte를 적용하고 폐기 |
| `0x010c` | `STACK_EXPAND` | transient private stack marker 적용과 폐기 |
| `0x010d` | `REMAP_PAGE` | transient shared view의 file page remap 결과 조회 |
| `0x010e` | `MSEAL_MERGE` | transient mseal merge의 범위 metadata 조회 |
| `0x010f` | `MGLRU_REPARENT` | transient MGLRU reparent accounting metadata 조회 |
| `0x0110` | `RMAP_UNMAP` | transient rmap unmap metadata 조회 |
| `0x0111` | `UFFD_MOVE` | transient userfaultfd move metadata 조회 |
| `0x0112` | `HUGETLB_RESERVE` | transient hugetlb reservation metadata 조회 |
| `0x0113` | `PERCPU_POPULATE` | transient per-CPU population metadata 조회 |

## 권한과 capability

| bit | 값 | 이름 |
| ---: | ---: | --- |
| 0 | `0x00000001` | `READ` |
| 1 | `0x00000002` | `WRITE` |
| 2 | `0x00000004` | `EDIT` |
| 3 | `0x00000008` | `SHARE` |
| 4 | `0x00000010` | `RESIZE` |
| 5 | `0x00000020` | `DELETE` |

그 밖의 bit는 invalid다. `EDIT`가 있으면 `WRITE`도 반드시 있어야 한다.
`DELETE`는 protocol에만 있는 권한이고 코어 note rights에는 없다.

CREATE의 `rights`와 COPY의 `destination_rights`는 새 record의 maximum
rights이자 최초 handle의 rights다. `SHARE`가 있으면 서버가 `/dev/urandom`
기반의 nonzero 16-byte capability를 발급하고 registry에 note를 넣는다.
`SHARE`가 없으면 CREATE/COPY response의 capability는 모두 0이고 다른 연결에서
열 수 없다.

capability는 maximum rights 전체에 대한 bearer token이다. OPEN의 requested
rights는 maximum rights의 부분집합이어야 하지만 새 connection-local handle만
축소한다. 같은 token으로 더 강한 handle을 다시 요청할 수 있으므로 OPEN은
token attenuation이 아니다. v1에는 attenuated token 파생 기능이 없다.
`UNLINK`는 capability가 가리키는 maximum rights에 `DELETE`가 있는지 검사한다.

## Operation payload

모든 `flags`와 `reserved` payload field는 v1에서 0이다. note size와 resize
크기는 4096의 배수여야 한다. handle은 발급한 연결에서만 유효하다.

### PING

request와 response가 모두 8 bytes이며 같은 값을 돌려준다.

```text
0:u64 nonce
```

### CREATE

Request, 16 bytes:

```text
0:u64 size
8:u32 rights
12:u32 flags              # 0
```

Response, 40 bytes:

```text
0:u64  handle
8:u8   capability[16]
24:u64 size
32:u32 rights
36:u32 reserved           # 0
```

size는 서버의 `max_note_size` 이하여야 하고 전체 logical-byte 및 live-record
quota 안에 들어야 한다.

### COPY

Request, 16 bytes:

```text
0:u64 source_handle
8:u32 destination_rights
12:u32 flags              # 0
```

Response, 40 bytes로 CREATE response와 같은 형식이다.

```text
0:u64  handle
8:u8   capability[16]
24:u64 size
32:u32 rights             # destination_rights
36:u32 reserved           # 0
```

source handle에는 `READ`가 필요하다. 서버는 source의 크기와 내용을 한 시점에
snapshot하여 같은 크기의 독립 record를 만들고, 이후 source와 destination의
변경은 서로 반영되지 않는다. destination은 `destination_rights`를 최대 권한과
최초 handle 권한으로 사용하며, `SHARE`가 있으면 source와 다른 새 capability를
발급한다. COPY도 일반 note 생성과 같이 live-record 수 하나와 source 크기만큼의
전체 logical-byte quota를 소비한다.

### OPEN

Request, 24 bytes:

```text
0:u8   capability[16]
16:u32 requested_rights
20:u32 flags              # 0
```

Response, 24 bytes:

```text
0:u64 handle
8:u64 size
16:u32 rights             # requested_rights
20:u32 reserved           # 0
```

record maximum rights에 `SHARE`가 있어야 하며 requested rights 전체가 maximum
rights에 포함되어야 한다. 요청 rights가 0인 handle도 허용되며 `STAT`에 쓸 수
있다.

### CLOSE

Request, 8 bytes:

```text
0:u64 handle
```

성공 response payload는 없다.

### STAT

Request, 8 bytes:

```text
0:u64 handle
```

Response, 16 bytes:

```text
0:u64 size
8:u32 rights
12:u32 flags              # 0
```

`rights`는 record의 maximum이 아니라 이 연결 handle에 요청해 부여된 권한이다.

### READ

Request, 24 bytes:

```text
0:u64 handle
8:u64 offset
16:u32 length
20:u32 flags              # 0
```

Response, `8 + N` bytes:

```text
0:u32 completed
4:u32 reserved            # 0
8:u8  data[N]             # N == completed
```

handle에는 `READ`가 필요하다. `length`는 1MiB 이하이고 `8 + length`가 negotiated
payload 이하여야 한다. offset과 length의 합은 `u64`를 overflow할 수 없으며
note 범위 안이어야 한다.

### WRITE와 EDIT

Request, `24 + N` bytes:

```text
0:u64 handle
8:u64 offset
16:u32 data_length        # N
20:u32 flags              # 0
24:u8  data[N]
```

Response, 8 bytes:

```text
0:u32 completed
4:u32 reserved            # 0
```

`N`은 1MiB 이하이고 전체 request가 negotiated payload 이하여야 한다. WRITE에는
`WRITE`, EDIT에는 `WRITE|EDIT`가 필요하다. 범위는 note 밖으로 자동 확장되지
않으며 offset과 N의 합이 현재 size를 넘으면 실패한다.

### PREVIEW

Request와 response layout은 WRITE와 같다. `N`은 1~4096이고 요청 범위 전체가
하나의 4096-byte page 안에 있어야 한다. handle에는 `READ`가 있어야 하며
`WRITE`나 `EDIT`가 있는 handle은 거부한다. 서버 설정에서 private preview를
활성화하지 않았으면 `UNSUPPORTED_OPCODE`를 반환한다.

서버는 note의 해당 범위를 private mapping으로 읽고 입력 byte를 그 view에
적용한 뒤 mapping을 폐기한다. 성공 response의 `completed`는 view에 적용한 byte
수이며, 원본 note에 대한 이후 READ 결과는 요청 전과 같아야 한다.

### STACK_EXPAND

Request는 `24 + N` bytes, response는 8 bytes이며 PREVIEW와 같은 layout을
사용한다. `N`은 1~4096이고 요청 범위 전체가 하나의 4096-byte page 안에 있어야
한다. handle에는 `READ`가 있어야 하며 `WRITE`나 `EDIT`가 있는 handle은 거부한다.
서버 설정에서 stack expansion을 활성화하지 않았으면 `UNSUPPORTED_OPCODE`를
반환한다.

서버는 입력 byte를 transient private stack marker에 적용한 뒤 marker를 폐기한다.
성공 response의 `completed`는 marker에 적용한 byte 수이며, backing note에 대한
이후 READ 결과는 요청 전과 같아야 한다.

### REMAP_PAGE

Request, 16 bytes:

```text
0:u64 handle
8:u64 note_offset
```

Response, 8 bytes:

```text
0:u32 protection
4:u32 reserved            # 0
```

handle에는 `READ|WRITE|SHARE`가 필요하며 `note_offset`은 4096-byte page 경계에
맞고 note 안의 완전한 한 page를 가리켜야 한다. 서버 설정에서 page remap을
활성화하지 않았으면 `UNSUPPORTED_OPCODE`를 반환한다.

서버는 transient shared mapping의 첫 page를 지정한 note page로 다시 연결하고
최종 MiniMM VMA protection bit를 반환한다. 이 경로는 MiniMM metadata만 다루며
host page protection을 바꾸거나 코드를 실행하지 않는다. core remap 처리 중
실패하면 response는 8-byte layout과 `protection=0`을 유지하고, payload 검증·
handle·권한 같은 사전 오류는 payload 없이 반환될 수 있다.

### MSEAL_MERGE

Request, 8 bytes:

```text
0:u64 handle
```

Response, 32 bytes:

```text
0:u32  total_pages
4:u32  sealed_pages
8:u32  range_valid        # 0 또는 1
12:u32 reserved           # 0
16:u64 update_start
24:u64 current_start
```

handle에는 `READ|WRITE|SHARE`가 필요하다. 서버 설정에서 mseal merge를
활성화하지 않았으면 `UNSUPPORTED_OPCODE`를 반환한다. 서버는 bounded transient
mapping model을 처리한 뒤 전체 page 수, 처리된 page 수와 두 range cursor의
metadata를 반환한다. `update_start`와 `current_start`는 4096-byte page 경계에
맞는다. 이 연산은 MiniMM 내부 metadata만 사용하며 host 주소나 host `mseal(2)`을
받거나 호출하지 않고 note byte도 변경하지 않는다.

core 처리 중 실패하면 response는 32-byte zero-valued layout을 유지할 수 있고,
payload 검증·handle·권한 같은 사전 오류는 payload 없이 반환될 수 있다.

### MGLRU_REPARENT

Request, 8 bytes:

```text
0:u64 handle
```

Response, 32 bytes:

```text
0:u32  total_pages
4:u32  parent_old_pages
8:u32  parent_new_pages
12:u32 child_old_debt_pages
16:u32 child_new_credit_pages
20:u32 exit_clean          # 0 또는 1
24:u32 accounting_valid    # 0 또는 1
28:u32 reserved            # 0
```

handle에는 `READ|WRITE|SHARE`가 필요하다. 서버 설정에서 MGLRU reparent를
활성화하지 않았으면 `UNSUPPORTED_OPCODE`를 반환한다. 서버는 bounded transient
generation accounting model을 처리하고 parent의 old/new page 수, child에 남은
old debt/new credit과 두 상태 flag를 반환한다. page count는 `total_pages` 범위
안이며 parent count의 합은 total과 같고 child debt와 credit은 서로 대응한다.
이 연산은 MiniMM 내부 metadata만 사용하며 host memcg나 LRU를 조작하지 않고
note byte도 변경하지 않는다.

core 처리 중 실패하면 response는 32-byte zero-valued layout을 유지할 수 있고,
payload 검증·handle·권한 같은 사전 오류는 payload 없이 반환될 수 있다.

### RMAP_UNMAP

Request, 24 bytes:

```text
0:u64  handle
8:u32  pte_capacity
12:u32 pte_index
16:u32 folio_pages
20:u32 vma_remaining
```

Response, 24 bytes:

```text
0:u32  requested_pages
4:u32  scanned_pages
8:u32  safe_pages
12:u32 first_invalid_index
16:u32 crossed_pte_boundary  # 0 또는 1
20:u32 bounds_valid          # 0 또는 1
```

handle에는 `READ|WRITE|SHARE`가 필요하고 note 크기는 정확히 4096 bytes여야 한다.
모든 page count는 1~4096이며 `pte_index < pte_capacity`여야 한다. 서버는 입력에
대한 deterministic batch metadata를 반환하며 실제 page table을 읽지 않는다.
`enable_rmap_unmap`이 false면 `UNSUPPORTED_OPCODE`다.

### UFFD_MOVE

Request, 24 bytes:

```text
0:u64  handle
8:u32  swap_entry
12:u32 source_folio
16:u32 replacement_folio
20:u32 reserved              # 0
```

Response, 24 bytes:

```text
0:u32  swap_entry
4:u32  expected_folio
8:u32  moved_folio
12:u32 pte_entry_matches     # 0 또는 1
16:u32 folio_identity_valid  # 0 또는 1
20:u32 accounting_valid      # 0 또는 1
```

세 identifier는 0이 아니며 두 folio identifier는 달라야 한다. 서버는 입력에 대한
deterministic move metadata를 반환한다. host swap, userfaultfd, thread 또는 page
table은 사용하지 않는다. handle에는 `READ|WRITE|SHARE`가 필요하고
`enable_uffd_move`가 false면 지원되지 않는다.

### HUGETLB_RESERVE

Request, 32 bytes:

```text
0:u64  handle
8:u32  maximum_pages
12:u32 minimum_pages
16:u32 used_before
20:u32 requested_pages
24:u32 global_free_pages
28:u32 reserved              # 0
```

Response, 32 bytes:

```text
0:u32  requested_pages
4:u32  global_needed_pages
8:u32  allocated_pages
12:u32 used_before
16:u32 used_after
20:u32 rollback_pages
24:u32 reservation_succeeded # 0 또는 1
28:u32 accounting_valid      # 0 또는 1
```

page count는 각각 최대 1,000,000이고 request는 model의 남은 maximum 안이어야
한다. 서버는 입력에 대한 deterministic reservation metadata를 반환하며 host
hugepage를 예약하지 않는다. handle에는 `READ|WRITE|SHARE`가 필요하고
`enable_hugetlb_reserve`가 false면 지원되지 않는다.

### PERCPU_POPULATE

Request, 16 bytes:

```text
0:u64  handle
8:u32  unit_count
12:u32 unit_pages
```

Response, 32 bytes:

```text
0:u32  total_backing_pages
4:u32  bitmap_capacity
8:u32  mark_count
12:u32 first_invalid_index
16:u32 empty_pages_after
20:u32 expected_empty_pages
24:u32 bounds_valid          # 0 또는 1
28:u32 accounting_valid      # 0 또는 1
```

unit 값은 각각 1~4096이고 total은 최대 1,048,576이다. 서버는 입력에 대한
deterministic population metadata를 반환하며 실제 bitmap을 할당하거나 수정하지
않는다. handle에는 `READ|WRITE|SHARE`가 필요하고 `enable_percpu_populate`가 false면
지원되지 않는다.

### RESIZE

Request, 16 bytes:

```text
0:u64 handle
8:u64 new_size
```

Response, 8 bytes:

```text
0:u64 actual_size
```

handle에는 `RESIZE`가 필요하다. `new_size`는 4096의 배수이고 per-note 및 전체
logical-byte quota 안이어야 한다. 실제 resize 단계에서 quota/backing 오류가 나면
response는 변경되지 않은 크기를 담을 수 있다. 잘못된 크기, 없는 handle, 권한
부족처럼 resize를 시작하기 전의 오류 response는 payload가 없을 수 있다.

### FLUSH

Request, 8 bytes:

```text
0:u64 handle
```

handle에는 `WRITE`가 필요하며 성공 response payload는 없다. backing은 unlink된
임시 file이므로 FLUSH는 서버 재시작 이후 영속성을 의미하지 않는다.

### UNLINK

Request, 16 bytes:

```text
0:u8 capability[16]
```

성공 response payload는 없다. registry entry가 사라져 새 OPEN은
`NOT_FOUND`가 되지만 기존 handle은 유지된다.

## Wire status

wire status 값은 `minimm_status_t`의 enum 번호가 아니라 독립적으로 고정된
값이다.

| 값 | 이름 | 의미 |
| ---: | --- | --- |
| 0 | `OK` | 성공 |
| 1 | `MALFORMED_MESSAGE` | frame 순서 또는 payload 형태 오류 |
| 2 | `UNSUPPORTED_VERSION` | 지원하지 않는 protocol version |
| 3 | `UNSUPPORTED_OPCODE` | 알 수 없는 opcode |
| 4 | `INVALID_ARGUMENT` | field, flag, 정렬 또는 범위 오류 |
| 5 | `NOT_FOUND` | capability 또는 handle 없음 |
| 6 | `PERMISSION_DENIED` | handle/record 권한 부족 또는 operation policy 거부 |
| 7 | `OUT_OF_MEMORY` | process memory allocation 실패 |
| 8 | `NO_SPACE` | backing 또는 ID 공간 부족 |
| 9 | `IO_ERROR` | socket 이외 note/backing I/O 실패 |
| 10 | `BUSY` | 대상 상태 때문에 작업 불가 |
| 11 | `ADDRESS_IN_USE` | bind 주소 사용 중 |
| 12 | `LIMIT_EXCEEDED` | service quota 또는 negotiated limit 초과 |
| 13 | `INTERNAL_ERROR` | 다른 내부 실패 |

공식 client는 `MALFORMED_MESSAGE`와 `INVALID_ARGUMENT`를
`MINIMM_ERROR_INVALID_ARGUMENT`, version/opcode 오류를
`MINIMM_ERROR_UNSUPPORTED`, `LIMIT_EXCEEDED`를 `MINIMM_ERROR_NO_SPACE`로
변환한다. `INTERNAL_ERROR`는 `MINIMM_ERROR_IO`로 변환한다.

## 오류와 연결 종료

- 수신자는 `wire_status`와 관계없이 header의 `payload_length`만큼 payload를
  소비해야 한다. 성공 payload 크기는 위 형식과 정확히 같아야 한다.
- READ, WRITE, EDIT, RESIZE는 작업 중 오류에도 `completed` 또는 기존 크기를
  담은 고정 progress payload를 보낼 수 있다. 다른 오류 response는 보통 빈
  payload다. 따라서 오류 response가 항상 비어 있다고 가정하면 안 된다.
- invalid header, request에 설정된 RESPONSE flag, socket EOF/timeout/I/O 오류는
  response 없이 연결을 끝낸다.
- header version이 1.0이 아니면 `UNSUPPORTED_VERSION`, payload가 negotiated
  maximum을 넘으면 `LIMIT_EXCEEDED`를 보낸 뒤 연결을 닫는다. hard maximum보다
  큰 길이는 header 자체가 invalid이므로 response가 없다.
- 최초 HELLO의 오류와 중복 HELLO는 오류 response 뒤 연결을 닫는다. handshake
  이후 알 수 없는 opcode나 개별 operation의 field/권한 오류는 오류 response를
  보내고 연결은 계속 처리할 수 있다.
- response의 opcode, request ID, version, RESPONSE flag, status 범위나 negotiated
  payload 상한이 맞지 않으면 공식 client는 연결을 broken 상태로 만들고
  닫는다. 성공 response의 operation별 고정 크기와 reserved 값도 검증한다.
