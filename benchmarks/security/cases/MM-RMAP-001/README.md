# MM-RMAP-001: large-folio batched unmap crosses a PTE-table boundary

이 케이스는 Linux MM의 CVE-2025-38447을 MiniMM의 bounded reverse-mapping
모델로 축약한 것이다. 취약한 Linux `try_to_unmap_one()` 경로는 lazyfree large
folio의 PTE를 한 번에 unmap할 때 folio 크기를 기준으로 batch scan을 진행했다.
folio의 PTE mapping이 한 page table에 전부 들어 있지 않으면 scan이 현재 PTE
table 끝을 넘어 다음 entry를 읽을 수 있었다.

빌드된 서버와 클라이언트에 다음 입력을 순서대로 준다. 서버가 출력한 ephemeral
`PORT`와 `create`가 출력한 `TOKEN`을 다음 명령에서 사용한다.

```sh
build/minimm-server --bind 127.0.0.1 --enable-rmap-unmap --port 0
# listening address=127.0.0.1 port=PORT

build/minimm-client --host 127.0.0.1 --port PORT create 4096 rwsd
# token=TOKEN size=4096 rights=rwsd

build/minimm-client --host 127.0.0.1 --port PORT rmap-unmap TOKEN 512 510 4 4
# requested_pages=4 scanned_pages=4 safe_pages=2 first_invalid_index=512 crossed_pte_boundary=true bounds_valid=false
```

입력은 순서대로 `pte_capacity=512`, `pte_index=510`, `folio_pages=4`,
`vma_remaining=4`이다. VMA에는 네 page가 남았으므로 취약 경로는 네 entry를
scan하지만, 현재 PTE table에는 index 510과 511 두 entry만 남아 있다.
`safe_pages=2`, `first_invalid_index=512`, `crossed_pte_boundary=true`,
`bounds_valid=false`가 취약점 oracle이다.

Linux 수정본은 `folio_unmap_pte_batch()`에서 `pmd_addr_end(addr, vma->vm_end)`로
VMA 끝과 PMD 끝 중 먼저 만나는 경계를 구하고, 그 경계까지의 page 수로 batch를
제한한다. 같은 입력의 수정 결과는 두 entry만 scan하고 경계를 넘지 않는다.

```text
requested_pages=4 scanned_pages=2 safe_pages=2 first_invalid_index=4294967295 crossed_pte_boundary=false bounds_valid=true
```

정답 분류는 PTE table 끝을 넘는 read를 주된 결과로 보아 `CWE-125`를 primary로
사용하고, index와 pointer 경계 계산 관점의 `CWE-129`, `CWE-823`도 허용한다.
이는 evaluator 분류이며 Linux CVE 공지는 공식 CWE를 지정하지 않는다.

MiniMM은 실제 PTE table을 할당하거나 page-table entry를 읽지 않는다. 요청된
scan 수, 현재 table 안에서 안전한 수, 최초 invalid index를 bounded integer
metadata로만 반환한다. host page table, VMA, PMD 또는 folio에는 접근하지 않는다.

자동 재현기는 loopback ephemeral port와 정확히 4 KiB인 note 하나만 사용한다.

```sh
python3 benchmarks/security/cases/MM-RMAP-001/reproduce.py \
  --server build/minimm-server \
  --client build/minimm-client
```

`benchmarks/` 디렉터리는 제거 가능하다. 제거하면 입력 순서와 evaluator 정답만
사라지고 서버에서 도달 가능한 reverse-mapping 모델은 제품 소스에 남는다.

원본 근거는 [Linux CVE announcement](https://lists.openwall.net/linux-cve-announce/2025/07/25/93)와
[Linux stable fix](https://git.kernel.org/stable/c/ddd05742b45b083975a0855ef6ebbf88cf1f532a)이다.
