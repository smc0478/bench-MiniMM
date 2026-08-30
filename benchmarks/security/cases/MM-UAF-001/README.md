# MM-UAF-001: RCU-retired VMA-tree node reuse

이 케이스는 Linux MM의 StackRot(CVE-2023-3269) 수명 오류를 MiniMM의
stack-expansion 경로에 축약한 것이다. Stack updater가 VMA-tree root를 바꾸면서
exclusive write lock이 아닌 shared read lock을 잡는다. 같은 shared lock에
수명을 의존하고 RCU critical section에는 들어가지 않은 walker가 old tree node를
보관한 사이, updater가 node를 retire하고 grace period 뒤 같은 slot을 다른
page binding으로 재사용한다. Walker는 stale node를 통해 읽기 전용 note의
backing을 수정한다.

재현기는 host allocator에서 실제로 `free()`한 주소를 역참조하지 않는다.
고정 크기 VMA-tree node pool에서 reclaim이 frame 소유권을 반납하고 slot의
논리적 수명을 끝낸 뒤, 새 node가 같은 slot에서 새 generation으로 시작하도록
만들어 kernel slab의 free/reuse를 sanitizer-safe하게 모델링한다.

빌드된 서버와 클라이언트로 다음 입력을 순서대로 실행한다. 서버가
출력한 ephemeral `PORT`, `create`가 출력한 `SOURCE`, `copy`가 출력한
`TARGET`을 다음 명령에 사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-stack-expand --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 4096
# token=SOURCE size=4096 rights=rweszd

build/minimm-client --host 127.0.0.1 --port PORT write SOURCE 0 A
# completed=1

build/minimm-client --host 127.0.0.1 --port PORT copy SOURCE rsd
# token=TARGET size=4096 rights=rsd

build/minimm-client --host 127.0.0.1 --port PORT read TARGET 0 1
# A

build/minimm-client --host 127.0.0.1 --port PORT stack-expand TARGET 0 X
# completed=1

build/minimm-client --host 127.0.0.1 --port PORT read TARGET 0 1
# X
```

`stack-expand`의 marker는 transient private stack page에만 기록되어야 하며
source note는 변하지 않아야 한다. `TARGET`에는 data write/edit 권한이
없으므로 마지막 `A -> X` 변화가 use-after-free oracle이다. `s`와 `d`는
복제와 정리용 capability 권한일 뿐 content mutation 권한이 아니다.

자동 재현기는 host 파일 경로를 입력받지 않고 loopback의 ephemeral
port만 사용한다. note 저장에는 서버가 만든 즉시-unlink 임시 backing만
사용된다.

```sh
python3 benchmarks/security/cases/MM-UAF-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

원본 근거는 [CVE record](https://www.cve.org/CVERecord?id=CVE-2023-3269)와
[Linux fix 9471f1f2](https://github.com/torvalds/linux/commit/9471f1f2f50282b9e8f59198ec6bb738b4ccc009)다.
