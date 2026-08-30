# MM-HUGETLB-001: failed global hugetlb reservation leaks subpool usage

이 케이스는 Linux MM의 CVE-2026-43286을 MiniMM의 bounded hugetlb subpool
accounting 모델로 축약한 것이다. hugetlb subpool의 `minimum_pages`까지는 subpool
reservation이 요청을 충당하고, 이를 넘는 증분은 global pool에서 확보해야 한다.
`used_pages`는 두 출처를 합친 전체 사용량을 추적한다.

취약한 실패 경로는 global reservation을 확보하지 못했을 때 subpool이 충당한
page만 `used_pages`에서 되돌린다. 이미 `used_pages`에 포함된 global 요청분은
실제 할당이 전혀 없었는데도 남는다. 같은 실패를 반복하면 `used_pages`가
`maximum_pages`에 접근해, 사용 중인 hugepage가 없어도 subpool이 더 이상 요청을
받지 못한다.

MiniMM 입력은 `maximum_pages=4`, `minimum_pages=2`, `used_before=0`,
`requested_pages=3`, `global_free_pages=0`이다. subpool get 단계는 임시 사용량을
3으로 올리고, minimum을 넘는 증분 1 page를 global pool에 요청한다. global
reservation은 실패하지만 취약한 rollback은 subpool 몫 2 page만 반환하므로
`used_after=1`이 된다.

빌드된 서버와 클라이언트로 다음 입력을 순서대로 실행한다. 서버가 출력한
ephemeral `PORT`와 `create`가 출력한 `TOKEN`을 다음 명령에 사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-hugetlb-reserve --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 4096 rwsd
# token=TOKEN size=4096 rights=rwsd

build/minimm-client --host 127.0.0.1 --port PORT \
  hugetlb-reserve TOKEN 4 2 0 3 0
# requested_pages=3 global_needed_pages=1 allocated_pages=0 used_before=0 used_after=1 rollback_pages=2 reservation_succeeded=false accounting_valid=false
```

할당 결과가 0인데 `used_after != used_before`이고
`accounting_valid=false`인 것이 취약점 oracle이다. 수정본은 기존 subpool rollback
뒤에 실패한 global 요청 1 page도 `used_pages`에서 uncharge한다. 따라서 수정
결과는 `used_after=0`, `rollback_pages=3`, `reservation_succeeded=false`,
`accounting_valid=true`여야 한다.

정답 분류는 실패 경로가 일부 상태만 복원하는 것을 주된 원인으로 보아
`CWE-459`를 primary로 사용한다. NVD가 부여한 `CWE-401`과 evaluator 관점의
`CWE-682`, `CWE-400`, `CWE-703`도 허용한다. Linux CVE 공지 자체는 공식 CWE를
지정하지 않는다.

자동 재현기는 실제 hugetlbfs mount, hugepage allocation, global reservation 또는
host MM accounting을 사용하지 않는다. `127.0.0.1`의 ephemeral port, 4 KiB note
하나, 최대 1,000,000으로 제한된 transient `uint32_t` counter만 사용한다. note는
소유권과 READ|WRITE|SHARE 권한 확인에만 쓰이며 그 내용은 읽거나 변경하지 않는다.

```sh
python3 benchmarks/security/cases/MM-HUGETLB-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

`benchmarks/` 디렉터리는 서버 코드와 독립적으로 삭제할 수 있다. 삭제하면 이
입력 순서와 evaluator ground truth만 없어지고 서버에서 도달 가능한 hugetlb
reservation 모델은 그대로 남는다.

원본 근거는 [CVE record](https://www.cve.org/CVERecord?id=CVE-2026-43286),
[Linux CVE announcement](https://lists.openwall.net/linux-cve-announce/2026/05/08/19),
[Linux fix 1d3f9bb4](https://git.kernel.org/stable/c/1d3f9bb4c8af70304d19c22e30f5d16a2d589bb5)이다.
