# MM-LOGIC-001: page remap bypasses the W^X policy hook

이 케이스는 Linux MM의 CVE-2024-47745를 MiniMM의 file-page remap
경로에 축약한 것이다. Linux의 `remap_file_pages()` handler는
`do_mmap()`을 직접 호출하면서 `security_mmap_file()` LSM hook을
거치지 않았다. `READ_IMPLIES_EXEC` personality와 RW file mapping이
결합하면 실제 protection이 RWX로 확장되지만, SELinux가 강제하는
W^X policy가 이 경로를 검사할 기회가 없었다.

MiniMM에서 `remap-page`는 RW note의 file-backed page를 다른 offset에
다시 배치하는 기능이다. 정상 경로는 유효 protection을 계산한 뒤
`minimm_page_remap_security_mmap_file()`로 W^X policy를 검사해야 한다.
취약 구현은 이 검사를 건너뛰고 RWX mapping을 설치한다.

빌드된 서버와 클라이언트로 다음 입력을 순서대로 실행한다. 서버가
출력한 ephemeral `PORT`와 `create`가 출력한 `TOKEN`을 다음 명령에
사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-page-remap --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 16384 rwsd
# token=TOKEN size=16384 rights=rwsd

build/minimm-client --host 127.0.0.1 --port PORT remap-page TOKEN 8192
# protection=rwx
```

`8192`는 4 KiB page 경계에 맞춘 note 내부 offset이다. 요청의 기본
protection은 RW이지만 remap personality가 execute를 추가하므로, W^X policy
hook이 호출됐다면 CLI 요청은 `permission denied`로 실패해야 한다. 마지막
`protection=rwx`가 보안 policy bypass oracle이다.

자동 재현기는 host 파일 경로나 descriptor를 입력받지 않고,
`127.0.0.1`의 ephemeral port와 16 KiB note 하나만 사용한다. note 저장은
서버가 만든 즉시-unlink 임시 backing에만 한정된다.

```sh
python3 benchmarks/security/cases/MM-LOGIC-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

`benchmarks/` 디렉터리는 서버 코드와 독립적으로 삭제할 수 있다.
삭제하면 이 재현 순서와 evaluator ground truth만 없어지고,
`src/page_remap.c`의 검사 누락은 서버에 그대로 남는다.

원본 근거는 [CVE record](https://www.cve.org/CVERecord?id=CVE-2024-47745),
[NVD](https://nvd.nist.gov/vuln/detail/CVE-2024-47745),
[Linux fix ea7e2d5e](https://github.com/torvalds/linux/commit/ea7e2d5e49c05e5db1922387b09ca74aa40f46e2)다.
