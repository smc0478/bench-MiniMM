#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool help_fails_when_stdout_fails(const char *program)
{
	pid_t child = fork();
	int wait_status = 0;

	if (child < (pid_t)0) {
		return false;
	}
	if (child == (pid_t)0) {
		int broken_pipe[2] = { -1, -1 };
		int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);

		if (pipe(broken_pipe) < 0 || null_fd < 0) {
			_exit(127);
		}
		(void)close(broken_pipe[0]);
		if (dup2(broken_pipe[1], STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0) {
			_exit(127);
		}
		(void)close(broken_pipe[1]);
		(void)close(null_fd);
		execl(program, program, "--help", (char *)NULL);
		_exit(127);
	}

	while (waitpid(child, &wait_status, 0) < (pid_t)0) {
		if (errno != EINTR) {
			return false;
		}
	}
	return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 1;
}

static bool create_without_delete_is_usage_error(const char *program)
{
	pid_t child = fork();
	int wait_status = 0;

	if (child < (pid_t)0) {
		return false;
	}
	if (child == (pid_t)0) {
		int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);

		if (null_fd < 0 || dup2(null_fd, STDOUT_FILENO) < 0 ||
		    dup2(null_fd, STDERR_FILENO) < 0) {
			_exit(127);
		}
		(void)close(null_fd);
		execl(program, program, "create", "4096", "s", (char *)NULL);
		_exit(127);
	}

	while (waitpid(child, &wait_status, 0) < (pid_t)0) {
		if (errno != EINTR) {
			return false;
		}
	}
	return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 2;
}

int main(int argument_count, char **arguments)
{
	if (!check(argument_count == 3, "receive both CLI executable paths") ||
	    !check(help_fails_when_stdout_fails(arguments[1]),
		   "client reports a buffered stdout failure") ||
	    !check(create_without_delete_is_usage_error(arguments[1]),
		   "one-shot create requires delete rights for output rollback") ||
	    !check(help_fails_when_stdout_fails(arguments[2]),
		   "server reports a buffered stdout failure")) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
