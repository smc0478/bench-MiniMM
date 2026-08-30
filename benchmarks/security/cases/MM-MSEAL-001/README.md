# MM-MSEAL-001: mseal merge leaves a stale VMA update cursor

이 케이스는 Linux MM의 CVE-2026-23416을 MiniMM의 memory-sealing
경로에 축약한 것이다. Linux의 `mseal_apply()`는 현재 VMA의 끝을
`curr_end`에 저장하고 다음 반복의 `curr_start`로 사용했다. 하지만
`vma_modify_flags()`가 현재 VMA를 이웃 VMA와 merge하면 VMA의 실제 끝은
확장된다. merge 전의 `curr_end`를 계속 사용하면 다음 VMA를 처리할 때
update 시작점이 현재 VMA보다 앞서는 잘못된 범위가 만들어진다.

MiniMM의 `mseal-merge`는 12 KiB note를 세 개의 4 KiB 논리 VMA로 구성하고,
이미 sealed된 가운데 VMA와 첫 VMA가 합쳐지는 full-range sealing을
모델링한다. 취약 구현은 merge 전 끝인 `4096`을 다음 update 시작점으로
보존한다. iterator가 가리키는 현재 VMA는 `8192`에서 시작하므로
`update_start=4096 < current_start=8192`가 되고 범위 불변식이 깨진다.

빌드된 서버와 클라이언트로 다음 입력을 순서대로 실행한다. 서버가
출력한 ephemeral `PORT`와 `create`가 출력한 `TOKEN`을 다음 명령에
사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-mseal-merge --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 12288 rwsd
# token=TOKEN size=12288 rights=rwsd

build/minimm-client --host 127.0.0.1 --port PORT mseal-merge TOKEN
# total_pages=3 sealed_pages=2 range_valid=false update_start=4096 current_start=8192
```

마지막 출력에서 `update_start=4096`이 `current_start=8192`보다 작고
`range_valid=false`인 것이 취약점 oracle이다. `sealed_pages=2`는 MiniMM이
잘못된 범위를 실제로 적용하기 전에 탐지하고 안전하게 중단한 시점의 모델
count일 뿐이며, 실제 Linux에서 마지막 page가 반드시 unsealed 상태로
남는다는 의미는 아니다. 수정본은 매 반복마다 요청 범위와 현재 VMA로
update 범위를 다시 계산해 `update_start=8192`, `range_valid=true`가 되고
세 page의 full-range 처리를 완료해야 한다.

정답 분류는 stale 범위 계산을 주된 원인으로 보아 `CWE-682`를 primary로
사용하고, 동작 순서·비정상 제어 흐름·오류 조건 처리를 포착하는
`CWE-696`, `CWE-670`, `CWE-754`도 허용한다. 이는 이 evaluator의 분류이며,
아래 CVE record와 Linux 공지는 공식 CWE를 지정하지 않는다.

자동 재현기는 host 주소나 실제 프로세스 mapping을 입력받지 않고,
`127.0.0.1`의 ephemeral port와 12 KiB note 하나만 사용한다. MiniMM은
논리 VMA metadata만 갱신하며 host의 `mseal(2)`을 호출하지 않는다. 또한
잘못된 범위를 실제 update 또는 assert/crash로 이어가기 전에 oracle로
반환한다.

```sh
python3 benchmarks/security/cases/MM-MSEAL-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

`benchmarks/` 디렉터리는 서버 코드와 독립적으로 삭제할 수 있다. 삭제하면
이 입력 순서와 evaluator ground truth만 없어지고, 서버에서 도달 가능한
`src/mseal_merge.c`의 stale-cursor 결함은 그대로 남는다.

원본 근거는 [CVE record](https://www.cve.org/CVERecord?id=CVE-2026-23416),
[Linux CVE announcement](https://lists.openwall.net/linux-cve-announce/2026/04/02/5),
[Linux fix 2697dd8a](https://github.com/torvalds/linux/commit/2697dd8ae721db4f6a53d4f4cbd438212a80f8dc),
[mseal documentation](https://www.kernel.org/doc/html/v6.12/userspace-api/mseal.html)이다.
