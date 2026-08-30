# C 코딩 스타일

MiniMM의 C 소스와 헤더는 Linux kernel coding style을 따른다. 다만 MiniMM은
커널 내부 코드가 아니라 C11/POSIX 사용자 공간 라이브러리이므로 공개 API의
`stdint.h` 타입, `_t` 이름, 오류 반환 방식은 그대로 유지한다. 이 규칙은 코드
표현을 통일하며 API나 실행 의미를 바꾸기 위한 것이 아니다.

## 핵심 규칙

- 들여쓰기는 너비 8의 tab을 사용한다.
- 가능하면 80열 안에 두고 100열을 formatter 한도로 사용한다. 함수 signature를
  부자연스럽게 쪼개는 것보다 읽기 쉬운 한 줄이 나을 때는 80열을 넘길 수 있다.
- 함수 본문의 여는 brace는 다음 줄에 둔다. `if`, `for`, `while`, `switch`의
  여는 brace는 조건과 같은 줄에 둔다.
- pointer의 `*`는 변수나 함수 인자 이름에 붙인다.
- 함수 인자는 한 줄에 들어가면 같은 줄에 둔다. 길어서 나눌 때도 인자마다
  무조건 한 줄을 차지하게 하거나 닫는 괄호만 별도 줄에 두지 않는다.
- `switch`의 `case`는 `switch`와 같은 깊이에 둔다.

예를 들어 긴 이름의 단일 인자 함수는 다음처럼 작성한다.

```c
static bool minimm_page_table_node_is_empty(const minimm_page_table_node_t *node)
{
	size_t index = 0U;

	for (index = 0U; index < MINIMM_PAGE_TABLE_ENTRIES; ++index) {
		if (node->entries[index] != NULL) {
			return false;
		}
	}
	return true;
}
```

프로젝트의 `.clang-format`은 위 규칙을 자동 적용한다. 이 설정은 upstream
Linux kernel 설정의 사용자 공간 C에 해당하는 부분만 담고 있으며, 커널 전용
macro 목록은 복사하지 않는다. formatter가 brace를 자동으로 제거하거나 include
순서를 바꾸지 않도록 설정해 의미 변화 가능성을 줄였다.

## 자동 포맷과 검사

이 설정은 clang-format 21로 검증했다. clang-format을 설치한 뒤 CMake가 실행
파일을 찾게 한다. 여러 버전이 설치된 환경에서는 `MINIMM_CLANG_FORMAT`에
사용할 경로를 지정해 버전을 고정한다.

```sh
cmake -S . -B build/dev \
  -DMINIMM_CLANG_FORMAT=/usr/bin/clang-format-21
cmake --build build/dev --target format
cmake --build build/dev --target format-check
```

`format`은 `cmd/`, `include/`, `src/`, `tests/` 아래의 모든 C 소스와 헤더를
수정한다. `format-check`는 파일을 수정하지 않고 차이가 있으면 실패하므로 CI나
커밋 전 검사에 사용할 수 있다. 자동 포맷 뒤에는 항상 diff와 테스트를 함께
검토한다.

참고 기준은 upstream Linux 문서의 `process/coding-style.rst`, `.clang-format`,
`Documentation/dev-tools/clang-format.rst`이다.
