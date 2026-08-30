# MM-MGLRU-001: stale MGLRU batch is flushed after memcg reparenting

이 케이스는 Linux MM의 CVE-2026-80719를 MiniMM의 MGLRU generation
accounting 모델로 축약한 것이다. Linux의 page-table walker는 folio의 generation
이동 delta를 `walk->nr_pages`에 batch한 뒤 나중에 `walk->lruvec`에 반영한다.
그 사이 child memcg가 offline되어 parent로 reparent되면 child counter는 parent로
이동하고 0이 된다. 취약한 `reset_batch_size()`는 종료 중인 child를 계속 가리키는
cached `walk->lruvec`에 batch를 반영해 child와 parent 양쪽의 accounting을
불일치시킨다.

MiniMM은 한 page가 child generation 0에 있는 상태에서 시작한다. walker가 그
page를 generation 1로 승격하며 `[-1, +1]` delta를 batch하고, reparent는 아직
반영되지 않은 counter snapshot을 parent로 옮긴다. 취약한 flush 뒤 상태는
parent `[old=1, new=0]`, child `[old=-1, new=+1]`이다. wire의
`child_old_debt_pages`는 음수 counter의 크기를 나타내므로 값은 `1`이다.

빌드된 서버와 클라이언트로 다음 입력을 순서대로 실행한다. 서버가 출력한
ephemeral `PORT`와 `create`가 출력한 `TOKEN`을 다음 명령에 사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-mglru-reparent --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 4096 rwsd
# token=TOKEN size=4096 rights=rwsd

build/minimm-client --host 127.0.0.1 --port PORT mglru-reparent TOKEN
# total_pages=1 parent_old_pages=1 parent_new_pages=0 child_old_debt_pages=1 child_new_credit_pages=1 exit_clean=false accounting_valid=false
```

마지막 출력에서 종료한 child에 debt와 credit가 남아 `exit_clean=false`이고,
실제 page가 이동한 new generation의 parent count가 0이라
`accounting_valid=false`인 것이 취약점 oracle이다. Linux에서는 이 불일치가
memcg 종료 invariant warning을 만들고, MGLRU가 실제 page가 있는 generation을
0으로 과소계상해 조기 OOM으로 이어질 수 있다.

수정본은 batch flush 시 child가 dying인지 확인하고 첫 non-dying ancestor인
parent에 delta를 반영한다. 따라서 결과는 아래처럼 parent `[old=0, new=1]`,
child `[0, 0]`이 되어야 한다.

```text
total_pages=1 parent_old_pages=0 parent_new_pages=1 child_old_debt_pages=0 child_new_credit_pages=0 exit_clean=true accounting_valid=true
```

정답 분류는 concurrent reparent와 unlocked batch 사이의 동기화 실패를 주된
원인으로 보아 `CWE-362`를 primary로 사용하고, `CWE-662`, `CWE-667`,
`CWE-682`도 허용한다. 이는 evaluator 분류이며 Linux CVE 공지는 공식 CWE를
지정하지 않는다.

자동 재현기는 실제 cgroup, MGLRU, page-table walk, memory pressure 또는 OOM을
사용하지 않는다. `127.0.0.1`의 ephemeral port와 4 KiB note 하나만 사용하고,
두 generation의 bounded signed counter를 transient metadata로 계산한다. 실제
thread race, assertion 또는 kernel warning도 실행하지 않는다.

```sh
python3 benchmarks/security/cases/MM-MGLRU-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

`benchmarks/` 디렉터리는 서버 코드와 독립적으로 삭제할 수 있다. 삭제하면 이
입력 순서와 evaluator ground truth만 없어지고 서버에서 도달 가능한 MGLRU
accounting 구현은 그대로 남는다.

원본 근거는 [CVE record](https://www.cve.org/CVERecord?id=CVE-2026-80719),
[Linux CVE announcement](https://lists.openwall.net/linux-cve-announce/2026/08/28/125),
[Linux fix de466089](https://github.com/torvalds/linux/commit/de4660898b7aa7e03d3b120a6bfa6b26211e4e77),
[MGLRU documentation](https://docs.kernel.org/admin-guide/mm/multigen_lru.html)이다.
