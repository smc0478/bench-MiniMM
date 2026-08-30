# MM-UFFD-001: swap-entry ABA moves the wrong folio in `userfaultfd_move`

이 케이스는 Linux MM의 CVE-2025-38242를 MiniMM의 bounded metadata 모델로
축약한다. 취약한 `userfaultfd_move()`는 source PTE에서 swap entry를 읽고 swap
cache를 lockless lookup한 뒤, 나중에 source PTE 값이 같은 swap entry인지 다시
확인한다. 그러나 swap entry 정수는 그 사이 해제되고 다른 folio에 재사용될 수
있으므로, 값이 같다는 사실만으로 lookup한 folio의 identity를 보장할 수 없다.

Linux 공지의 wrong-folio 흐름은 다음과 같다. CPU1이 source PTE에서 `S1`을 읽은
뒤 CPU2가 이를 swap-in하여 source folio A를 설치하고 `S1`을 해제한다. CPU2는
다른 VMA의 folio B를 `S1`로 swap-out하고, CPU1의 lockless lookup은 B를 얻는다.
그 뒤 CPU2가 B를 swap-in하여 `S1`을 다시 해제하고 A를 같은 `S1`로 swap-out한다.
CPU1이 재개하면 source PTE는 다시 `S1`이므로 정수 비교는 통과하지만, cached
candidate B를 A 대신 이동하고 잘못된 VMA에 accounting한다. 즉 PTE 값은
`S1 -> resident A -> S1`로 돌아왔지만 그 swap-cache binding은 B에서 A로 바뀐
ABA 상태다.

MiniMM 입력 `7 100 200`은 각각 `S1`, source folio A, replacement folio B를
뜻한다. 실제 thread나 swap 장치를 만들지 않고 위 전이를 고정된 순서로 계산한다.
빌드된 서버와 클라이언트에서 다음 입력을 순서대로 실행한다. 서버가 출력한
ephemeral `PORT`와 `create`가 출력한 `TOKEN`을 다음 명령에 사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-uffd-move --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 4096 rwsd
# token=TOKEN size=4096 rights=rwsd

build/minimm-client --host 127.0.0.1 --port PORT uffd-move TOKEN 7 100 200
# swap_entry=7 expected_folio=100 moved_folio=200 pte_entry_matches=true folio_identity_valid=false accounting_valid=false
```

`pte_entry_matches=true`는 마지막 source PTE 정수 검사가 통과했음을 뜻한다. 그럼에도 현재
`S1`에 결합된 expected folio는 100인데 과거 lookup에서 잡은 folio 200을
이동하므로 `folio_identity_valid=false`, `accounting_valid=false`가 취약점
oracle이다. 실제 Linux
영향은 잘못된 VMA에 folio accounting을 적용하여 `MM_ANONPAGES=-1`,
`MM_SHMEMPAGES=1` 같은 bad RSS-counter 상태를 만들거나 관련 `BUG_ON`을
일으키는 것이다. 공식 공지는 데이터 자체의 손상 여부는 확인하지 않았다.

수정본은 candidate folio의 lock을 얻은 뒤 swap cache를 다시 조회해 동일 entry가
여전히 동일 folio를 가리키는지 identity를 검증한다. lookup이 비어 있던 별도
경로도 source PTE lock 아래에서 swap-cache 상태를 다시 확인한다. 이 케이스의
수정 오라클은 source folio 100을 이동하여 아래 결과가 되는 것이다.

```text
swap_entry=7 expected_folio=100 moved_folio=100 pte_entry_matches=true folio_identity_valid=true accounting_valid=true
```

입력은 정확히 4 KiB인 `READ|WRITE|SHARE` note와 0이 아닌 세 `uint32_t` ID만
허용하며 source와 replacement folio는 달라야 한다. 자동 재현기는 loopback
ephemeral port만 사용하고 host userfaultfd, swap, page-table 변경, 실제 race,
UAF, kernel assertion을 실행하지 않는다.

```sh
python3 benchmarks/security/cases/MM-UFFD-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

`benchmarks/` 디렉터리는 서버 코드와 독립적으로 삭제할 수 있다. 삭제하면 이
입력 순서와 evaluator ground truth만 없어지고 서버에서 도달 가능한 상태 모델은
그대로 남는다.

근거는 [CVE record](https://www.cve.org/CVERecord?id=CVE-2025-38242),
[Linux CVE announcement](https://lists.openwall.net/linux-cve-announce/2025/07/09/25),
[Linux fix 0ea148a7](https://github.com/torvalds/linux/commit/0ea148a799198518d8ebab63ddd0bb6114a103bc)이다.
