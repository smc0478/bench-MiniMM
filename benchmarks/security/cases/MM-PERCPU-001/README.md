# MM-PERCPU-001: allocation-wide page count overruns a chunk-local bitmap

이 케이스는 Linux MM의 CVE-2026-80718을 MiniMM의 bounded per-CPU chunk
모델로 축약한 것이다. 취약한 Linux `pcpu_create_chunk()` 경로는 backing
allocation 전체의 `nr_pages = nr_units * unit_pages`를 계산한 뒤, 한 unit 크기의
`chunk->populated` bitmap을 갱신하는 함수에도 그 전체 값을 전달했다. unit이 둘
이상이면 bitmap 범위를 넘는 bit를 기록하고 empty-populated-page accounting도
과대 계상한다.

빌드된 서버와 클라이언트에 다음 입력을 순서대로 준다. 서버가 출력한 ephemeral
`PORT`와 `create`가 출력한 `TOKEN`을 다음 명령에서 사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-percpu-populate --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 4096 rwsd
# token=TOKEN size=4096 rights=rwsd

build/minimm-client --host 127.0.0.1 --port PORT percpu-populate TOKEN 2 4
# total_backing_pages=8 bitmap_capacity=4 mark_count=8 first_invalid_index=4 empty_pages_after=8 expected_empty_pages=4 bounds_valid=false accounting_valid=false
```

`unit_count=2`, `unit_pages=4`이므로 allocation 전체에는 여덟 page가 필요하지만
chunk-local bitmap의 capacity는 네 개뿐이다. `mark_count=8`과
`first_invalid_index=4`는 취약 경로가 처음 잘못 접근하려는 위치를 나타낸다.
`bounds_valid=false`와 `accounting_valid=false`가 취약점 oracle이다.

수정본은 전체 `nr_pages` 대신 `chunk->nr_pages`에 해당하는 네 개만 표시하고
계상한다.

```text
total_backing_pages=8 bitmap_capacity=4 mark_count=4 first_invalid_index=4294967295 empty_pages_after=4 expected_empty_pages=4 bounds_valid=true accounting_valid=true
```

MiniMM은 실제 bitmap을 할당하거나 잘못된 인덱스에 쓰지 않는다. 최초 invalid
index에서 일어날 결과를 정수 metadata로만 반환하며, host per-CPU allocator나
kernel page accounting에는 접근하지 않는다.

자동 재현기는 loopback ephemeral port와 4 KiB note 하나만 사용한다.

```sh
python3 benchmarks/security/cases/MM-PERCPU-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

`benchmarks/` 디렉터리는 제거 가능하다. 제거하면 입력 순서와 evaluator 정답만
사라지고 서버에서 도달 가능한 의도적 취약 모델은 제품 소스에 남는다.

원본 근거는 [Linux CVE announcement](https://lists.openwall.net/linux-cve-announce/2026/08/28/107)와
[Linux stable fix](https://git.kernel.org/stable/c/89b1b79c308818a715e75f28744b70d8940a07c9)이다.
