# MM-RACE-001: read-only private preview race

이 케이스는 Dirty COW(CVE-2016-5195)의 COW page identity 오류를 MiniMM의
private-preview 경로에 축약한 것이다. `preview`는 읽기 전용 복제 note를 임시로
미리 보는 기능이어야 하지만, 취약 구현은 강제 write retry 중 backing page를
선택해 note 내용을 바꾼다.

빌드된 서버와 클라이언트로 다음 입력을 순서대로 실행한다. 서버가 출력한
ephemeral `PORT`, `create`가 출력한 `SOURCE`, `copy`가 출력한 `TARGET`을 다음
명령에 사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-private-preview --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 4096
# token=SOURCE size=4096 rights=rweszd

build/minimm-client --host 127.0.0.1 --port PORT write SOURCE 0 A
# completed=1

build/minimm-client --host 127.0.0.1 --port PORT copy SOURCE rsd
# token=TARGET size=4096 rights=rsd

build/minimm-client --host 127.0.0.1 --port PORT read TARGET 0 1
# A

build/minimm-client --host 127.0.0.1 --port PORT preview TARGET 0 X
# completed=1

build/minimm-client --host 127.0.0.1 --port PORT read TARGET 0 1
# X
```

`TARGET`에는 data write/edit 권한이 없다. `s`와 `d`는 복제와 정리용 capability
권한일 뿐 content mutation 권한이 아니므로, 마지막 `A -> X` 변화가 취약점
oracle이다. 종료 전에 두 token을 `delete`하고 서버에 `SIGTERM`을 보낸다.

자동 재현기는 host 파일 경로를 입력받지 않고 loopback만 사용한다. note 저장에는
서버가 만든 즉시-unlink 임시 backing만 쓰인다.

```sh
python3 benchmarks/security/cases/MM-RACE-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

원본 근거는 [CVE record](https://www.cve.org/CVERecord?id=CVE-2016-5195)와
[Linux fix 19be0eaf](https://github.com/torvalds/linux/commit/19be0eaffa3ac7d8eb6784ad9bdbc7d67ed8e619)다.
