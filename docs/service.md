# TCP 노트 서비스

miniMM 서비스는 코어의 file-backed note를 TCP로 노출한다. 클라이언트는
versioned binary protocol로 노트를 만들거나 독립 snapshot으로 복사하고,
128-bit capability로 다른 연결에서 같은 노트를 열어 읽기·쓰기·편집·크기
변경·flush·unlink를 수행한다.

이 서비스의 노트 I/O는 주소 공간의 VMA를 통하지 않는다. 따라서 네트워크
`READ`/`WRITE`를 VMA/PTE COW나 page fault로 해석하면 안 된다. COPY lineage가
있을 때의 `WRITE`/`EDIT`는 note 계층에서 해당 page의 lazy COW overlay를 만들
수 있다. VMA, page fault, PTE COW와 software TLB 실험은 코어의 address-space
API가 담당하고, 서비스는 같은 4KiB frame/note 저장 계층을 공유한다.

## 구성과 수명

```text
minimm-client / client C API
            |
       framed TCP v1
            |
 listener -> connection worker -> connection-local handle
                              |
                     capability registry
                              |
                    shared note record lock
                              |
                    miniMM note / frame store
                              |
                 unlinked temporary files
```

- 서버에는 accept thread 하나가 있고, 수락한 연결마다 detached worker thread
  하나를 만든다.
- 각 worker는 먼저 `HELLO`를 처리한 뒤 한 번에 요청 하나만 순서대로 처리한다.
- handle은 해당 연결에서만 의미가 있는 0이 아닌 opaque `u64` 값이다. 다른
  연결에 전달하거나 연결 종료 후 재사용할 수 없다.
- 연결이 끝나면 그 연결의 handle을 모두 닫는다. 공유되지 않은 노트는 마지막
  handle이 닫힐 때 소멸한다.
- `SHARE` 권한으로 생성하거나 복사한 노트는 capability registry가 별도 참조를
  보유하므로 그 연결이 끝난 뒤에도 열 수 있다.
- `COPY`는 `READ` 가능한 source handle의 크기와 내용을 한 시점에 새 record로
  복사한다. source와 destination은 이후 쓰기·편집·크기 변경이 서로 영향을
  주지 않는 독립 note다. 서버 note에는 VMA mapping이 없고 모두 temporary
  note이므로 COPY는 page byte를 즉시 복사하지 않는다. sparse destination
  backing과 lazy per-page COW lineage를 연결하는 O(1) metadata snapshot이며,
  이후 source 또는 destination에서 처음 변경하는 page만 독립 overlay로
  보존·분리한다. VMA/PTE COW 복사는 이와 별도로 코어의
  `minimm_mapping_copy()`가 담당한다.
- `UNLINK`는 registry에서 capability를 제거한다. 이후 새 `OPEN`은 실패하지만
  이미 열린 handle은 닫힐 때까지 계속 유효하다.
- `minimm_server_stop()`은 listener와 client socket을 종료하고 모든 worker가
  빠져나올 때까지 기다린 다음 registry를 비운다. `SIGINT` 또는 `SIGTERM`을
  받은 `minimm-server`도 이 경로로 종료한다.

공유 note record마다 mutex가 하나 있어 서로 다른 연결의 `STAT`, `READ`,
`WRITE`, `EDIT`, `RESIZE`, `FLUSH`, source `COPY`, private preview, private stack
expansion, page remap, mseal merge와 opt-in security model을 직렬화한다. registry link/open/unlink와 서버 상태에는
각각 별도 mutex를 사용한다. 공개 client C API 역시 client별 mutex로 호출을
직렬화한다. 단, `minimm_client_disconnect()`를 다른 client 호출과 동시에 실행하는
것은 지원하지 않는다.

## 빌드와 실행

빌드 정의는 루트의 `CMakeLists.txt` 하나다. 도구는 기본으로 활성화되며
`MINIMM_BUILD_TOOLS=OFF`로 제외할 수 있다.

```sh
cmake -S . -B build/dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIMM_WARNINGS_AS_ERRORS=ON
cmake --build build/dev --parallel
ctest --test-dir build/dev --output-on-failure
```

기본 서버는 loopback의 `127.0.0.1:7331`에서 대기한다.

```sh
./build/dev/minimm-server
./build/dev/minimm-client ping
```

### 앱 실행 모델

`minimm-server`는 daemonize하지 않고 foreground에서 실행되므로 systemd,
launchd, container runtime 같은 process supervisor가 직접 관리할 수 있다.
listener 준비가 끝나면 표준출력에
`listening address=ADDRESS port=PORT` 한 줄을 쓰고 즉시 flush한다. 운영 오류와
설정 오류는 표준오류에 쓴다. `minimm-client ping`이 성공해 `pong`을 출력하면
protocol handshake와 요청 처리가 모두 가능한 상태다.

`SIGINT`와 `SIGTERM`은 listener를 닫고 연결 worker와 registry를 정리하는
graceful shutdown을 시작한다. 정상 신호 종료와 `--help`, `--version`의 종료
코드는 0, 시작·실행·출력 실패는 1, 잘못된 명령행은 2다. 서버 상태와 capability는
메모리에만 있으므로 프로세스를 다시 시작하면 기존 note와 token은 모두
사라진다.

빌드한 두 앱은 원하는 prefix에도 설치할 수 있다.

```sh
cmake --install build/dev --prefix build/install
./build/install/bin/minimm-server --version
./build/install/bin/minimm-server
```

`--port 0`을 주면 운영체제가 빈 TCP port를 고르고, 서버가 실제 주소와 port를
표준출력에 `listening address=... port=...` 형식으로 표시한다.

```sh
./build/dev/minimm-server --bind 127.0.0.1 --port 0
```

서버 명령행 옵션은 다음과 같다.

| 옵션 | 의미 |
| --- | --- |
| `--bind ADDRESS` | bind할 주소 |
| `--port PORT` | TCP port, `0`은 ephemeral port |
| `--max-clients COUNT` | 동시에 처리할 client 수 |
| `--max-notes COUNT` | 동시에 살아 있을 수 있는 note record 수 |
| `--max-note-size BYTES` | note 하나의 최대 논리 크기 |
| `--max-total-note-size BYTES` | 모든 live note의 최대 논리 크기 합계 |
| `--memory-pages COUNT` | 코어 resident frame budget의 4KiB page 수 |
| `--timeout-ms MS` | client socket 송수신 timeout |
| `--enable-private-preview` | 실험용 private preview 활성화(loopback 전용) |
| `--enable-stack-expand` | 실험용 private stack expansion 활성화(loopback 전용) |
| `--enable-page-remap` | 실험용 file-page remap 활성화(loopback 전용) |
| `--enable-mseal-merge` | 실험용 mseal merge 활성화(loopback 전용) |
| `--enable-mglru-reparent` | 실험용 MGLRU reparent 활성화(loopback 전용) |
| `--enable-rmap-unmap` | 실험용 rmap PTE batch 활성화(loopback 전용) |
| `--enable-uffd-move` | 실험용 userfaultfd move model 활성화(loopback 전용) |
| `--enable-hugetlb-reserve` | 실험용 hugetlb reservation model 활성화(loopback 전용) |
| `--enable-percpu-populate` | 실험용 per-CPU population model 활성화(loopback 전용) |
| `--version` | 서버 앱과 MiniMM library version 출력 후 종료 |
| `--help` | 사용법 출력 후 종료 |

`--max-note-size`와 `--max-total-note-size`는 4096의 배수여야 하며, total은
single-note 한도 이상이어야 한다. `max-notes`는 registry entry만이 아니라
연결 handle이나 unlink 후 열린 handle 때문에 아직 살아 있는 record도 센다.
private preview, private stack expansion, page remap, mseal merge, MGLRU reparent와
네 security model 중 하나라도 활성화하면 bind 주소는 정확히 `127.0.0.1` 또는
`::1`이어야 한다. 이 기능들은 기본적으로 꺼져 있다.

## 클라이언트 사용법

클라이언트의 공통 옵션은 `--host HOST`, `--port PORT`, `--timeout-ms MS`다.
host와 port 기본값은 `127.0.0.1`, `7331`이고 timeout `0`은 library 기본값인
30초를 사용한다.

```text
ping
create SIZE [RIGHTS]
copy TOKEN [RIGHTS]
stat TOKEN
read TOKEN OFFSET LENGTH
write TOKEN OFFSET TEXT
edit TOKEN OFFSET TEXT
preview TOKEN OFFSET TEXT
stack-expand TOKEN OFFSET TEXT
remap-page TOKEN OFFSET
mseal-merge TOKEN
mglru-reparent TOKEN
rmap-unmap TOKEN PTE_CAPACITY PTE_INDEX FOLIO_PAGES VMA_REMAINING
uffd-move TOKEN SWAP_ENTRY SOURCE_FOLIO REPLACEMENT_FOLIO
hugetlb-reserve TOKEN MAX MIN USED REQUEST GLOBAL_FREE
percpu-populate TOKEN UNIT_COUNT UNIT_PAGES
resize TOKEN SIZE
flush TOKEN
delete TOKEN
```

`SIZE`와 resize 크기는 4096-byte page 경계에 맞아야 한다. `create`와 `copy`의
기본 destination 권한은 전부(`rweszd`)이다. 이 one-shot CLI는 결과 handle을
닫은 뒤에도 새 note를 열 수 있어야 하고 stdout 출력 실패 시 생성물을 확실히
회수할 수 있어야 하므로 `create`와 `copy` destination 모두 `s`와 `d`를
요구한다. client C API와 wire protocol에는 이 CLI 제약이 없다. `copy`는 source
token을 `READ` 권한으로 열므로 source record에도 `READ`가 필요하다. 성공한
`create`와 `copy`는 모두
`token=... size=... rights=...` 형식의 한 줄을 출력한다. `read`는 raw byte를
표준출력에 쓰며 CLI 한 번의 최대 요청 길이는 16MiB다. client library는
negotiated payload 크기에 맞게 더 큰 data API 호출을 여러 protocol 요청으로
나눈다. `write`와 `edit`의 `TEXT`는 명령행 인자 그대로 전송한다.
`preview`는 `READ`만 가진 handle의 한 페이지 안에 있는 1~4096 bytes를 private
view에 적용한 뒤 view를 버린다. 성공해도 원본 note bytes는 바뀌지 않는 것이
이 명령의 계약이며, 서버에서 `--enable-private-preview`를 켜야 사용할 수 있다.
`stack-expand`도 `READ`만 가진 handle과 비어 있지 않은 단일-page 범위를
요구한다. 입력 byte를 transient private stack marker에 적용한 뒤 marker를
버리며, 성공해도 backing note bytes는 바뀌지 않는다. 서버에서
`--enable-stack-expand`를 켜야 사용할 수 있다.
`remap-page`는 `READ|WRITE|SHARE` handle과 page-aligned note offset을 받아
transient shared mapping의 첫 page를 다시 연결하고 최종 MiniMM VMA protection을
`protection=...` 형식으로 출력한다. metadata-only 경로이며 host memory
protection이나 실제 실행에는
영향을 주지 않는다. 서버에서 `--enable-page-remap`을 켜야 사용할 수 있다.
`mseal-merge`는 `READ|WRITE|SHARE` handle을 받아 transient mapping model을
처리하고 `total_pages=... sealed_pages=... range_valid=... update_start=...
current_start=...` 형식의 한 줄을 출력한다. note byte나 host mapping에는 영향을
주지 않는다. 서버에서 `--enable-mseal-merge`를 켜야 사용할 수 있다.
`mglru-reparent`는 `READ|WRITE|SHARE` handle을 받아 bounded transient generation
accounting model을 처리하고 `total_pages=... parent_old_pages=... parent_new_pages=...
child_old_debt_pages=... child_new_credit_pages=... exit_clean=... accounting_valid=...`
형식의 한 줄을 출력한다. host memcg/LRU와 note byte에는 영향을 주지 않는다.
서버에서 `--enable-mglru-reparent`를 켜야 사용할 수 있다.
`rmap-unmap`, `uffd-move`, `hugetlb-reserve`, `percpu-populate`는 모두
`READ|WRITE|SHARE` handle과 명령행에 표시된 bounded 정수 입력을 받아 각 transient
model의 metadata를 한 줄로 출력한다. 대응하는 `--enable-*` server option을 켜야
하며 실제 host page table, swap, hugepage 또는 per-CPU allocator를 조작하지 않고
note byte도 변경하지 않는다.

```sh
./build/dev/minimm-client create 8192 rweszd
# 출력 예: token=0123...cdef size=8192 rights=rweszd

./build/dev/minimm-client write TOKEN 0 hello
./build/dev/minimm-client read TOKEN 0 5
./build/dev/minimm-client copy TOKEN rwsd
# 출력 예: token=fedc...3210 size=8192 rights=rwsd
./build/dev/minimm-client resize TOKEN 12288
./build/dev/minimm-client flush TOKEN
./build/dev/minimm-client delete TOKEN
```

`TOKEN` 위치에 `-`를 쓰면 표준입력 첫 줄에서 정확히 32개의 16진수 문자를
읽는다. process argument와 shell history에 token을 남기지 않을 때 유용하다.

```sh
printf '%s\n' "$TOKEN" | ./build/dev/minimm-client read - 0 5
```

권한 문자는 다음과 같다. 중복 문자는 허용하지 않으며 `e`는 `w`와 함께
지정해야 한다.

| 문자 | 권한 | 필요한 연산 |
| --- | --- | --- |
| `r` | `READ` | `READ`, `COPY` source |
| `w` | `WRITE` | `WRITE`, `FLUSH` |
| `e` | `EDIT` | `EDIT`에는 `WRITE|EDIT` 둘 다 필요 |
| `s` | `SHARE` | capability registry 등록과 이후 `OPEN` |
| `z` | `RESIZE` | `RESIZE` |
| `d` | `DELETE` | capability `UNLINK` |

`STAT`은 유효한 handle만 있으면 별도 권한 없이 가능하다.

## Capability 보안 모델

공유 노트를 만들거나 복사하면 서버는 `/dev/urandom`에서 16-byte nonzero
capability를 만들고, CLI는 이를 32자리 16진수로 표시한다. 이 값은 bearer
token이다. 토큰을 가진 주체는 해당 record를 만들 때 정한 **최대 권한 전체**를
잠재적으로 가진다.

`OPEN`의 requested rights는 해당 record 최대 권한의 부분집합이어야 하고, 그
요청으로 생긴 connection-local handle만 축소한다. 토큰 자체는 축소되지
않는다. 예를 들어 같은 토큰으로 read-only handle을 열어도 토큰 소유자는
나중에 최대 권한 범위 안에서 write handle을 다시 열 수 있다. v1에는 축소된
권한의 새 token을 파생하는 기능이 없다. `UNLINK`도 handle 권한이 아니라
토큰이 가리키는 record의 최대 `DELETE` 권한으로 승인된다.

`COPY`가 반환하는 token은 source token의 권한 축소본이 아니라, 복사된 독립
record를 가리키는 새 capability다. destination 권한은 COPY 요청에서 별도로
지정한다. logical-byte quota는 snapshot 전체 크기로 계산하지만 COPY 시점의
resident frame·page-in 수는 note 크기에 비례해 증가하지 않는다. 이후 실제로
읽거나 변경한 page만 frame cache와 COW overlay를 사용한다.

다음 제한을 전제로 사용해야 한다.

- protocol v1에는 TLS, 사용자 인증, transport 암호화가 없다.
- capability는 요청에 그대로 실리며 유출되면 회수 전까지 복제해 사용할 수
  있다. 기본 loopback 밖에 직접 공개하지 않는다.
- 동시 client 한도를 넘긴 새 연결은 protocol 응답 없이 닫힐 수 있다. CREATE와
  COPY는 각각 live note record 수를 하나 소비하고 note 크기만큼 전체 논리 크기
  quota를 소비한다. quota 초과는 `LIMIT_EXCEEDED` 응답으로 보고한다. 이 서버는
  신뢰 경계용 reverse proxy나 인터넷 공개 서비스가 아니다.
- 한 연결이 장시간 I/O를 멈추면 configured socket timeout 뒤 종료된다.

## 기본 제한

| 항목 | 기본값 |
| --- | ---: |
| bind | `127.0.0.1` |
| TCP port | `7331` |
| 동시 client | 32 |
| live note record | 1024 |
| 연결당 handle | `min(max_notes, 256)`, 기본 256 |
| note 하나의 논리 크기 | 64MiB |
| 전체 note 논리 크기 | 256MiB |
| negotiated payload 상한 | 1MiB + 24 bytes (1,048,600 bytes) |
| 한 data field | 1MiB |
| socket I/O timeout | 30초 |
| 코어 resident memory budget | 64MiB |
| page size | 4096 bytes |
| protocol inflight | 1 |

논리 note 크기 제한과 resident frame budget은 다른 값이다. resident budget을
넘는 frame은 코어의 file-backed page-out 대상으로 남을 수 있다.

## 임시 파일과 host `mmap` 사용 경계

서버가 만든 note는 `/tmp/minimm-note-XXXXXX`, frame store는
`/tmp/minimm-pages-XXXXXX` 형태로 `mkstemp()`한 뒤 즉시 unlink한 file
descriptor를 사용한다. note와 page-out byte 이동은 `pread()`/`pwrite()`,
note 크기 변경은 `ftruncate()`로 수행한다.

별도로 frame store는 설정된 resident memory budget 크기의 anonymous host
`mmap(2)` arena를 만들고 4KiB slot으로 나눠 resident byte를 저장한다. 이
mapping은 내부 저장소일 뿐 MiniMM 주소와 대응하지 않는다. 모든 slot의 host
protection은 동일하며, note capability와 `READ/WRITE/EDIT`, VMA/PTE protection,
page fault, COW와 software TLB는 MiniMM이 별도로 검사하고 갱신한다.

외부 note FD는 다른 descriptor에서 truncate될 수 있어 직접 file-map하면
접근 중 `SIGBUS`가 날 수 있으므로 mmap 대상에서 제외한다. `minimm_mmap()`이라는
이름도 별도의 MiniMM 가상 주소 공간 모델 API이며 OS syscall wrapper가 아니다.
서비스 자체는 외부 FD를 note로 여는 operation을 노출하지 않으므로 서비스의
COPY는 항상 temporary-note lazy COW 경로를 사용한다. 코어에서
`minimm_note_open_fd()` source를 직접 COPY할 때만 외부 writer와의 snapshot
격리를 위해 eager page-copy fallback을 사용한다. 이 fallback은 COPY 완료 뒤
변경은 격리하지만 COPY와 동시에 실행되는 외부 writer에 대해 atomic whole-file
snapshot을 보장하지 않는다. 서비스 note에는 외부 FD writer가 없으므로 이
제약은 service COPY에는 적용되지 않는다.

임시 파일은 이름이 없고 열린 descriptor가 닫히면 사라진다. registry와
capability도 memory에만 있으므로 `FLUSH`는 현재 서버의 backing file에 dirty
page를 반영할 뿐, 서버 종료나 재시작 뒤의 영속성을 제공하지 않는다.
