# miniMM

miniMM은 Linux 메모리 관리(`mm`)의 핵심 개념을 사용자 공간에서 재현한
작고 결정적인 C 모델입니다. 4KiB 페이지, 다단계 페이지 테이블, VMA,
페이지 폴트, COW, 공유 노트, 파일 기반 page-out/page-in을 직접 관찰할 수
있도록 만들었습니다. 학습·실험과 메모리 취약점 탐지기 벤치마킹이
목적이며, Linux 커널이나 Linux syscall ABI를 대체하지 않습니다.

## 현재 구현

- 4KiB 고정 페이지와 48-bit/4단계 페이지 테이블
- 실제 물리 주소가 아닌 모델 내부 synthetic PFN
- 설정된 resident page 한도와 cold 우선 exact-LRU page-out/page-in, 명시적인
  bounded reclaim과 refault 통계
- host `mmap(2)` resident-byte arena와, unlink된 임시 파일에 대한
  `pread`/`pwrite` 기반 page-out backing
- Maple Tree에서 아이디어를 얻은 fanout 16 불변 VMA snapshot과 보수적인
  RCU-style 회수
- 주소 공간별 direct-mapped software TLB와 통계
- 익명/노트의 shared·private mapping, private write COW, 주소 공간 fork와
  같은 공간 private VMA 복사의 COW
- `READ`, `WRITE`, `EDIT`, `EXEC` 권한과 최대 권한 제한
- demand fault, zero-fill, backing page-in, COW fault, 접근·dirty 상태 추적
- 자동 접근과 명시적 handler를 구분하는 space별 bounded fault trace
- VMA와 sparse PTE/frame 관계를 값으로 보존하는 immutable space inspection
  snapshot
- temporary note와 서비스 note를 page 단위 lazy COW lineage로 복제하는
  독립 snapshot copy
- `mmap`, `munmap`, `mprotect`, `mremap`, `brk`, `sbrk`, `msync`, `mincore`,
  `mlock`, `munlock`, `madvise`에 대응하는 Linux 유사 API
- loopback TCP 서버, versioned binary protocol, 128-bit capability로 공유하는
  원격 노트의 생성·복사와 `minimm-client` 명령 도구

세부 동작은 [API 안내](docs/api.md), 내부 설계는
[아키텍처](docs/architecture.md), 평가 방법은
[벤치마킹 안내](docs/benchmarking.md)를 참고하세요. 서버 사용법과 wire 형식은
[서비스 안내](docs/service.md), [프로토콜 명세](docs/protocol.md)에 있습니다.
코어 MM을 바로 실행해 보는 과정은 [MM tour](docs/mm-tour.md)에 있습니다.
현재 Linux MM과 맞춘 관찰 계약 및 의도적으로 남긴 차이는
[Linux MM 대응표](docs/linux-mm-parity.md)에 정리했습니다.
C 코드 작성 규칙과 자동 검사 방법은 [코딩 스타일](docs/coding-style.md)에
정리했습니다.

## 빠른 시작

Linux/POSIX 환경, C11 컴파일러, CMake 3.20 이상이 필요합니다.

```sh
cmake -S . -B build/dev \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMINIMM_WARNINGS_AS_ERRORS=ON
cmake --build build/dev --parallel
ctest --test-dir build/dev --output-on-failure
```

먼저 syscall이나 TCP 서비스를 거치지 않는 코어 MM tour를 실행할 수 있습니다.

```sh
./build/dev/examples/minimm-mm-tour all
```

`fault`, `cow`, `reclaim`, `inspect`, `working-set`을 따로 지정하면
demand-zero/page-in, fork COW, lock pressure와 eviction, immutable inspection,
cold 우선 bounded reclaim/refault를 한 시나리오씩 실행합니다. 각 명령은 상태
전이를 출력하는 동시에 frame 관계, byte 격리와 resident 한도를 자체 검증하며
실패하면 0이 아닌 종료 코드를 반환합니다.
fault trace에는 자동 접근과 명시적 fault handler 결과가 순서대로 남습니다.
`inspect`는 VMA-only 상태에서 sparse PTE가 생기고, fork COW frame이 공유됐다가
child write로 분리되며, page-out 뒤에도 같은 frame identity가 남는 과정을
서로 다른 시점의 snapshot으로 보여 줍니다.
`working-set`은 resident page를 cold로 표시하고 명시적 reclaim 결과와 system
counter를 비교한 뒤, reclaim된 같은 frame의 첫 page-in만 refault로 집계되는
과정을 보여 줍니다.

원격 note 서비스를 시험하려면 한 terminal에서 서버를 실행합니다. 기본 bind는
`127.0.0.1:7331`입니다.

```sh
./build/dev/minimm-server
```

`minimm-server`는 daemon으로 분기하지 않는 foreground 앱이다. 준비가 끝나면
`listening address=... port=...`을 출력하며, `SIGINT`/`SIGTERM`을 받으면 열린
client를 정리하고 종료 코드 0으로 끝난다. `minimm-client ping`을 readiness와
health 확인에 사용할 수 있다.

다른 terminal에서 노트를 만들고 출력된 32자리 token을 사용합니다. 아래의
`TOKEN`에는 `token=...` 중 32자리 값만 넣습니다.

```sh
./build/dev/minimm-client create 8192 rweszd
TOKEN=0123456789abcdef0123456789abcdef
printf '%s\n' "$TOKEN" | ./build/dev/minimm-client write - 0 hello
printf '%s\n' "$TOKEN" | ./build/dev/minimm-client read - 0 5
printf '%s\n' "$TOKEN" | ./build/dev/minimm-client copy -
printf '%s\n' "$TOKEN" | ./build/dev/minimm-client delete -
```

실행 파일을 별도 prefix에 설치하려면 다음처럼 실행한다.

```sh
cmake --install build/dev --prefix build/install
./build/install/bin/minimm-server --version
./build/install/bin/minimm-server
```

설치는 `MINIMM_BUILD_TOOLS=ON`일 때 `minimm-server`와 `minimm-client`,
`MINIMM_BUILD_EXAMPLES=ON`일 때 `minimm-mm-tour` 실행 파일을 `bin` 아래에
배치한다.

`-`는 token을 표준입력에서 읽어 process argument와 shell history 노출을
줄입니다. `copy`는 기본적으로 모든 권한을 가진 같은 크기의 독립 snapshot을
만들고 새 token을 출력합니다. 서비스의 temporary note COPY는 호출 시 page를
복사하지 않고 lazy per-page COW lineage를 연결합니다. 이 capability가 유일한
원격 권한 증명이고 v1에는 TLS나 사용자 인증이 없으므로 기본 loopback 밖에
그대로 공개하면 안 됩니다.

ASan과 UBSan을 함께 사용하는 검증 빌드는 다음과 같습니다.

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

위 `detect_leaks=0`은 LeakSanitizer를 실행할 수 없는 ptrace 기반 sandbox용이다.
일반 Linux 환경에서는 이를 생략해 leak 검사까지 활성화하는 것을 권장한다.

## 저장소 구조

```text
.
├── CMakeLists.txt             # 유일한 빌드 정의
├── include/minimm/            # MM, protocol, server/client 공개 C API
├── src/                       # MM 모델과 protocol/service 구현
├── cmd/                       # minimm-server, minimm-client 진입점
├── examples/                  # 실행 가능한 코어 MM 학습 시나리오
├── tests/                     # CTest 기반 단위·통합 테스트
└── docs/                      # 설계, API, 벤치마킹 문서
```

## CMake 옵션

| 옵션 | 기본값 | 용도 |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | 테스트 target 구성 |
| `MINIMM_ENABLE_ASAN` | `OFF` | AddressSanitizer 활성화 |
| `MINIMM_ENABLE_UBSAN` | `OFF` | UndefinedBehaviorSanitizer 활성화 |
| `MINIMM_WARNINGS_AS_ERRORS` | `OFF` | 컴파일 경고를 오류로 처리 |
| `MINIMM_BUILD_TOOLS` | `ON` | TCP 서버·클라이언트 실행 파일 구성 |
| `MINIMM_BUILD_EXAMPLES` | `ON` | 코어 MM tour 실행 파일 구성 |
| `MINIMM_BUILD_SECURITY_BENCHMARKS` | `OFF` | 선택적 evaluator 재현 test 활성화 |

## 중요한 범위 제한

miniMM의 API 이름과 개념은 Linux와 닮았지만 동작과 오류 코드는 독자적인
사용자 공간 모델입니다. `MINIMM_MREMAP_FIXED`는 지원하고,
`MINIMM_MAP_POPULATE`도 fixed mapping과 조합할 수 있으며 best-effort hint로
처리합니다. `MINIMM_MREMAP_DONTUNMAP`은 같은 크기의 private-anonymous VMA
subset만 지원합니다. mapping 중인 note는 snapshot COPY와 축소 resize를
`MINIMM_ERROR_BUSY`로 거부하지만 확장 resize는 허용합니다. 전체 제약과
수명·동시성 계약은 [API 안내](docs/api.md#제약과-호환성)와
[Linux MM 대응표](docs/linux-mm-parity.md)를 확인하세요.

`minimm_mmap()`은 miniMM 가상 주소 공간을 조작하는 모델 API 이름일 뿐 host
`mmap(2)` syscall wrapper가 아닙니다. host `mmap(2)`은 내부 resident frame의
byte arena 할당에만 사용합니다. 이 arena는 항상 read/write 가능하며 MiniMM
주소, PTE 권한, page fault, COW 또는 TLB를 구현하지 않습니다. 그 의미론은
MiniMM 코드가 직접 검사하고 상태를 전이합니다. note와 page-out file I/O는
계속 `pread`/`pwrite`로 수행합니다.

space inspection snapshot도 Linux `/proc/*/maps`, `/proc/*/pagemap` ABI가
아닙니다. 한 MiniMM space의 VMA와 설치된 sparse PTE를 capture 시점의 값으로
복사하는 진단 API이며, system 전체나 실제 kernel의 모든 MM 상태를 한 시점에
고정하는 global atomic snapshot으로 해석하면 안 됩니다.

`MINIMM_MADV_COLD`와 `minimm_system_reclaim()`도 Linux reclaim 구현의 예측기가
아닙니다. MiniMM physical frame에 cold 우선순위를 주고 나머지는 exact-LRU로
고르는 결정적 실험 API입니다. Linux active/inactive list, refault distance와
workingset 판정, kswapd, MGLRU 또는 full rmap을 구현하지 않습니다.
