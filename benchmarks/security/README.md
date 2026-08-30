# MiniMM security evaluator

이 디렉터리는 취약점 탐지 평가용 PoC와 정답만 담는 제거 가능한 레이어다.
의도적 취약 구현은 제품 소스에 있으며 여기의 파일은 기본 빌드나 설치에
포함되지 않는다.

opt-in 재현 테스트는 빌드된 loopback 전용 서버와 CLI만 사용한다.

```sh
cmake -S . -B build/security \
  -DMINIMM_BUILD_SECURITY_BENCHMARKS=ON
cmake --build build/security \
  --target minimm-security-benchmark-race-001 \
           minimm-security-benchmark-uaf-001 \
           minimm-security-benchmark-logic-001 \
           minimm-security-benchmark-mseal-001 \
           minimm-security-benchmark-mglru-001 \
           minimm-security-benchmark-rmap-001 \
           minimm-security-benchmark-uffd-001 \
           minimm-security-benchmark-hugetlb-001 \
           minimm-security-benchmark-percpu-001
ctest --test-dir build/security -L security --output-on-failure
```

현재 케이스는 다음과 같다.

- [`MM-RACE-001`](cases/MM-RACE-001/README.md): private COW race condition
- [`MM-UAF-001`](cases/MM-UAF-001/README.md): RCU-retired VMA-tree node use-after-free
- [`MM-LOGIC-001`](cases/MM-LOGIC-001/README.md): file-page remap W^X policy bypass
- [`MM-MSEAL-001`](cases/MM-MSEAL-001/README.md): stale VMA cursor after an mseal merge
- [`MM-MGLRU-001`](cases/MM-MGLRU-001/README.md): stale generation batch after memcg reparenting
- [`MM-RMAP-001`](cases/MM-RMAP-001/README.md): PTE batch crosses an rmap table boundary
- [`MM-UFFD-001`](cases/MM-UFFD-001/README.md): swap-cache ABA during userfaultfd move
- [`MM-HUGETLB-001`](cases/MM-HUGETLB-001/README.md): incomplete hugetlb reservation rollback
- [`MM-PERCPU-001`](cases/MM-PERCPU-001/README.md): allocation-wide count overruns a unit-local bitmap

재현기는 외부 호스트나 파일 경로를 받지 않으며 `127.0.0.1`의
ephemeral port만 사용한다.
