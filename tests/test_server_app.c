#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
	MINIMM_TEST_PROCESS_TIMEOUT_MS = 5000,
	MINIMM_TEST_CLEANUP_TIMEOUT_MS = 2000,
	MINIMM_TEST_CAPTURE_SIZE = 512,
	MINIMM_TEST_TOKEN_LENGTH = 32
};

typedef struct minimm_test_process {
	pid_t pid;
	int output_fd;
} minimm_test_process_t;

static bool check(bool condition, const char *message)
{
	if (!condition) {
		(void)fprintf(stderr, "check failed: %s\n", message);
	}
	return condition;
}

static bool minimm_test_make_deadline(int timeout_ms, struct timespec *out_deadline)
{
	long nanoseconds = 0L;

	if (out_deadline == NULL || timeout_ms < 0 ||
	    clock_gettime(CLOCK_MONOTONIC, out_deadline) != 0) {
		return false;
	}
	out_deadline->tv_sec += (time_t)(timeout_ms / 1000);
	nanoseconds = out_deadline->tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
	out_deadline->tv_sec += (time_t)(nanoseconds / 1000000000L);
	out_deadline->tv_nsec = nanoseconds % 1000000000L;
	return true;
}

static int minimm_test_remaining_ms(const struct timespec *deadline)
{
	struct timespec now = { 0 };
	time_t seconds = 0;
	long nanoseconds = 0L;
	time_t milliseconds = 0;

	if (deadline == NULL || clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
		return -1;
	}
	if (now.tv_sec > deadline->tv_sec ||
	    (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)) {
		return 0;
	}
	seconds = deadline->tv_sec - now.tv_sec;
	nanoseconds = deadline->tv_nsec - now.tv_nsec;
	if (nanoseconds < 0L) {
		seconds -= (time_t)1;
		nanoseconds += 1000000000L;
	}
	if (seconds > (time_t)(INT_MAX / 1000)) {
		return INT_MAX;
	}
	milliseconds = seconds * (time_t)1000;
	milliseconds += (time_t)(nanoseconds / 1000000L);
	if (nanoseconds % 1000000L != 0L) {
		milliseconds += (time_t)1;
	}
	return milliseconds > (time_t)INT_MAX ? INT_MAX : (int)milliseconds;
}

static bool minimm_test_set_close_on_exec(int descriptor)
{
	const int flags = fcntl(descriptor, F_GETFD);

	return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static bool minimm_test_redirect_stdout(int descriptor)
{
	if (descriptor == STDOUT_FILENO) {
		return fcntl(descriptor, F_SETFD, 0) == 0;
	}
	if (dup2(descriptor, STDOUT_FILENO) < 0) {
		return false;
	}
	(void)close(descriptor);
	return true;
}

static bool minimm_test_process_spawn(char *const arguments[], minimm_test_process_t *out_process)
{
	int output_pipe[2] = { -1, -1 };
	pid_t child = (pid_t)-1;

	if (arguments == NULL || arguments[0] == NULL || out_process == NULL) {
		return false;
	}
	out_process->pid = (pid_t)-1;
	out_process->output_fd = -1;
	if (pipe(output_pipe) != 0 || !minimm_test_set_close_on_exec(output_pipe[0]) ||
	    !minimm_test_set_close_on_exec(output_pipe[1])) {
		if (output_pipe[0] >= 0) {
			(void)close(output_pipe[0]);
		}
		if (output_pipe[1] >= 0) {
			(void)close(output_pipe[1]);
		}
		return false;
	}

	child = fork();
	if (child < (pid_t)0) {
		(void)close(output_pipe[0]);
		(void)close(output_pipe[1]);
		return false;
	}
	if (child == (pid_t)0) {
		(void)close(output_pipe[0]);
		if (!minimm_test_redirect_stdout(output_pipe[1])) {
			_exit(127);
		}
		execv(arguments[0], arguments);
		_exit(127);
	}

	(void)close(output_pipe[1]);
	out_process->pid = child;
	out_process->output_fd = output_pipe[0];
	return true;
}

static bool minimm_test_read_output(minimm_test_process_t *process, char *output,
				    size_t output_capacity, size_t *in_out_length,
				    bool *out_end_of_file)
{
	ssize_t bytes_read = 0;

	if (*in_out_length + 1U >= output_capacity) {
		return false;
	}
	do {
		bytes_read = read(process->output_fd, output + *in_out_length,
				  output_capacity - *in_out_length - 1U);
	} while (bytes_read < (ssize_t)0 && errno == EINTR);
	if (bytes_read < (ssize_t)0) {
		return false;
	}
	if (bytes_read == (ssize_t)0) {
		*out_end_of_file = true;
		(void)close(process->output_fd);
		process->output_fd = -1;
		return true;
	}
	*in_out_length += (size_t)bytes_read;
	output[*in_out_length] = '\0';
	return true;
}

static bool minimm_test_collect_process(minimm_test_process_t *process, int timeout_ms,
					char *output, size_t output_capacity, int *out_wait_status)
{
	struct timespec deadline = { 0 };
	size_t output_length = 0U;
	bool end_of_file = false;

	if (process == NULL || process->pid <= (pid_t)0 || output == NULL ||
	    output_capacity == 0U || out_wait_status == NULL ||
	    !minimm_test_make_deadline(timeout_ms, &deadline)) {
		return false;
	}
	output[0] = '\0';
	for (;;) {
		pid_t waited = waitpid(process->pid, out_wait_status, WNOHANG);

		if (waited == process->pid) {
			process->pid = (pid_t)-1;
			while (!end_of_file &&
			       minimm_test_read_output(process, output, output_capacity,
						       &output_length, &end_of_file)) {
			}
			return end_of_file;
		}
		if (waited < (pid_t)0 && errno != EINTR) {
			return false;
		}

		const int remaining_ms = minimm_test_remaining_ms(&deadline);

		if (remaining_ms <= 0) {
			return false;
		}
		if (!end_of_file) {
			struct pollfd descriptor = {
				.fd = process->output_fd,
				.events = POLLIN,
			};
			int poll_status = 0;

			do {
				poll_status = poll(&descriptor, (nfds_t)1U, remaining_ms);
			} while (poll_status < 0 && errno == EINTR);
			if (poll_status <= 0 || (descriptor.revents & POLLNVAL) != 0) {
				return false;
			}
			if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0 &&
			    !minimm_test_read_output(process, output, output_capacity,
						     &output_length, &end_of_file)) {
				return false;
			}
		} else {
			const int poll_ms = remaining_ms < 10 ? remaining_ms : 10;

			if (poll(NULL, (nfds_t)0U, poll_ms) < 0 && errno != EINTR) {
				return false;
			}
		}
	}
}

static void minimm_test_force_cleanup(minimm_test_process_t *process)
{
	char output[MINIMM_TEST_CAPTURE_SIZE] = { 0 };
	int wait_status = 0;

	if (process == NULL) {
		return;
	}
	if (process->pid > (pid_t)0) {
		if (kill(process->pid, SIGKILL) != 0 && errno != ESRCH) {
			(void)fprintf(stderr, "cleanup kill failed: %s\n", strerror(errno));
		}
		if (!minimm_test_collect_process(process, MINIMM_TEST_CLEANUP_TIMEOUT_MS, output,
						 MINIMM_TEST_CAPTURE_SIZE, &wait_status)) {
			(void)fprintf(stderr, "cleanup wait timed out\n");
		}
	}
	if (process->output_fd >= 0) {
		(void)close(process->output_fd);
		process->output_fd = -1;
	}
}

static bool minimm_test_parse_ready_line(const char *line, uint16_t *out_port)
{
	static const char prefix[] = "listening address=127.0.0.1 port=";
	const char *port_text = NULL;
	char *end = NULL;
	unsigned long port = 0UL;

	if (line == NULL || out_port == NULL || strncmp(line, prefix, sizeof(prefix) - 1U) != 0) {
		return false;
	}
	port_text = line + sizeof(prefix) - 1U;
	errno = 0;
	port = strtoul(port_text, &end, 10);
	if (errno == ERANGE || end == port_text || *end != '\0' || port == 0UL ||
	    port > (unsigned long)UINT16_MAX) {
		return false;
	}
	*out_port = (uint16_t)port;
	return true;
}

static bool minimm_test_wait_until_ready(minimm_test_process_t *server, uint16_t *out_port)
{
	struct timespec deadline = { 0 };
	char line[MINIMM_TEST_CAPTURE_SIZE] = { 0 };
	size_t line_length = 0U;

	if (server == NULL || server->pid <= (pid_t)0 || server->output_fd < 0 ||
	    !minimm_test_make_deadline(MINIMM_TEST_PROCESS_TIMEOUT_MS, &deadline)) {
		return false;
	}
	for (;;) {
		struct pollfd descriptor = {
			.fd = server->output_fd,
			.events = POLLIN,
		};
		const int remaining_ms = minimm_test_remaining_ms(&deadline);
		int poll_status = 0;
		ssize_t bytes_read = 0;
		char *newline = NULL;

		if (remaining_ms <= 0 || line_length + 1U >= sizeof(line)) {
			return false;
		}
		do {
			poll_status = poll(&descriptor, (nfds_t)1U, remaining_ms);
		} while (poll_status < 0 && errno == EINTR);
		if (poll_status <= 0 || (descriptor.revents & POLLNVAL) != 0) {
			return false;
		}
		if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
			continue;
		}
		do {
			bytes_read = read(server->output_fd, line + line_length,
					  sizeof(line) - line_length - 1U);
		} while (bytes_read < (ssize_t)0 && errno == EINTR);
		if (bytes_read <= (ssize_t)0) {
			return false;
		}
		line_length += (size_t)bytes_read;
		line[line_length] = '\0';
		newline = memchr(line, '\n', line_length);
		if (newline != NULL) {
			if ((size_t)(newline - line) + 1U != line_length) {
				return false;
			}
			*newline = '\0';
			return minimm_test_parse_ready_line(line, out_port);
		}
	}
}

static bool minimm_test_run_program(char *const arguments[], int expected_exit_status, char *output,
				    size_t output_capacity)
{
	minimm_test_process_t process = { .pid = (pid_t)-1, .output_fd = -1 };
	int wait_status = 0;
	bool collected = false;

	if (!minimm_test_process_spawn(arguments, &process)) {
		return false;
	}
	collected = minimm_test_collect_process(&process, MINIMM_TEST_PROCESS_TIMEOUT_MS, output,
						output_capacity, &wait_status);
	if (!collected) {
		minimm_test_force_cleanup(&process);
		return false;
	}
	return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == expected_exit_status;
}

static bool minimm_test_parse_create_output(const char *output,
					    char token[MINIMM_TEST_TOKEN_LENGTH + 1U])
{
	static const char prefix[] = "token=";
	static const char suffix[] = " size=4096 rights=rwsd\n";
	size_t index = 0U;

	if (output == NULL || token == NULL || strncmp(output, prefix, sizeof(prefix) - 1U) != 0) {
		return false;
	}
	for (index = 0U; index < MINIMM_TEST_TOKEN_LENGTH; ++index) {
		const unsigned char byte = (unsigned char)output[sizeof(prefix) - 1U + index];

		if (byte == (unsigned char)'\0' || isxdigit((int)byte) == 0) {
			return false;
		}
		token[index] = (char)byte;
	}
	token[MINIMM_TEST_TOKEN_LENGTH] = '\0';
	return strcmp(output + sizeof(prefix) - 1U + MINIMM_TEST_TOKEN_LENGTH, suffix) == 0;
}

static bool minimm_test_run_client_flow(char *client_program, uint16_t port)
{
	char port_text[6] = { 0 };
	char output[MINIMM_TEST_CAPTURE_SIZE] = { 0 };
	char token[MINIMM_TEST_TOKEN_LENGTH + 1U] = { 0 };
	char *ping_arguments[] = { client_program, "--host", "127.0.0.1", "--port", port_text,
				   "--timeout-ms", "2000",   "ping",	  NULL };
	char *create_arguments[] = { client_program, "--host",	     "127.0.0.1", "--port",
				     port_text,	     "--timeout-ms", "2000",	  "create",
				     "4096",	     "rwsd",	     NULL };
	char *write_arguments[] = { client_program, "--host", "127.0.0.1", "--port", port_text,
				    "--timeout-ms", "2000",   "write",	   token,    "0",
				    "hello",	    NULL };
	char *read_arguments[] = {
		client_program, "--host", "127.0.0.1", "--port", port_text, "--timeout-ms",
		"2000",		"read",	  token,       "0",	 "5",	    NULL
	};
	char *delete_arguments[] = { client_program, "--host", "127.0.0.1", "--port", port_text,
				     "--timeout-ms", "2000",   "delete",    token,    NULL };

	if (snprintf(port_text, sizeof(port_text), "%u", (unsigned int)port) < 0) {
		return false;
	}
	if (!check(minimm_test_run_program(ping_arguments, 0, output, sizeof(output)),
		   "client ping succeeds") ||
	    !check(strcmp(output, "pong\n") == 0, "client ping prints pong") ||
	    !check(minimm_test_run_program(create_arguments, 0, output, sizeof(output)),
		   "client create succeeds") ||
	    !check(minimm_test_parse_create_output(output, token),
		   "client create prints a capability") ||
	    !check(minimm_test_run_program(write_arguments, 0, output, sizeof(output)),
		   "client write succeeds") ||
	    !check(strcmp(output, "completed=5\n") == 0, "client write reports progress") ||
	    !check(minimm_test_run_program(read_arguments, 0, output, sizeof(output)),
		   "client read succeeds") ||
	    !check(strcmp(output, "hello") == 0, "client read returns stored bytes") ||
	    !check(minimm_test_run_program(delete_arguments, 0, output, sizeof(output)),
		   "client delete succeeds") ||
	    !check(strcmp(output, "ok\n") == 0, "client delete reports success")) {
		return false;
	}
	return true;
}

static bool minimm_test_server_start(char *server_program, minimm_test_process_t *out_server,
				     uint16_t *out_port)
{
	char *server_arguments[] = { server_program, "--bind", "127.0.0.1", "--port", "0",
				     "--timeout-ms", "2000",   NULL };

	if (!minimm_test_process_spawn(server_arguments, out_server)) {
		return false;
	}
	if (!minimm_test_wait_until_ready(out_server, out_port)) {
		minimm_test_force_cleanup(out_server);
		return false;
	}
	return true;
}

static bool minimm_test_server_stop(minimm_test_process_t *server, int termination_signal)
{
	char output[MINIMM_TEST_CAPTURE_SIZE] = { 0 };
	int wait_status = 0;

	if (server == NULL || server->pid <= (pid_t)0 ||
	    (kill(server->pid, termination_signal) != 0 && errno != ESRCH)) {
		return false;
	}
	if (!minimm_test_collect_process(server, MINIMM_TEST_PROCESS_TIMEOUT_MS, output,
					 MINIMM_TEST_CAPTURE_SIZE, &wait_status)) {
		minimm_test_force_cleanup(server);
		return false;
	}
	return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

static bool minimm_test_wait_until_stopped(pid_t child)
{
	struct timespec deadline = { 0 };

	if (child <= (pid_t)0 ||
	    !minimm_test_make_deadline(MINIMM_TEST_PROCESS_TIMEOUT_MS, &deadline)) {
		return false;
	}
	for (;;) {
		int wait_status = 0;
		const pid_t waited = waitpid(child, &wait_status, WNOHANG | WUNTRACED);
		const int remaining_ms = minimm_test_remaining_ms(&deadline);

		if (waited == child) {
			return WIFSTOPPED(wait_status);
		}
		if ((waited < (pid_t)0 && errno != EINTR) || remaining_ms <= 0) {
			return false;
		}
		const int poll_ms = remaining_ms < 10 ? remaining_ms : 10;

		if (poll(NULL, (nfds_t)0U, poll_ms) < 0 && errno != EINTR) {
			return false;
		}
	}
}

static bool minimm_test_server_stop_with_pending_signals(minimm_test_process_t *server)
{
	char output[MINIMM_TEST_CAPTURE_SIZE] = { 0 };
	int wait_status = 0;

	if (server == NULL || server->pid <= (pid_t)0 || kill(server->pid, SIGSTOP) != 0 ||
	    !minimm_test_wait_until_stopped(server->pid) || kill(server->pid, SIGINT) != 0 ||
	    kill(server->pid, SIGTERM) != 0 || kill(server->pid, SIGCONT) != 0) {
		minimm_test_force_cleanup(server);
		return false;
	}
	if (!minimm_test_collect_process(server, MINIMM_TEST_PROCESS_TIMEOUT_MS, output,
					 MINIMM_TEST_CAPTURE_SIZE, &wait_status)) {
		minimm_test_force_cleanup(server);
		return false;
	}
	return WIFEXITED(wait_status) && WEXITSTATUS(wait_status) == 0;
}

int main(int argument_count, char **arguments)
{
	minimm_test_process_t server = { .pid = (pid_t)-1, .output_fd = -1 };
	char output[MINIMM_TEST_CAPTURE_SIZE] = { 0 };
	char *oversized_note_arguments[] = { NULL,
					     "--max-note-size",
					     "9223372036854779904",
					     "--max-total-note-size",
					     "9223372036854779904",
					     NULL };
	uint16_t port = 0U;

	if (!check(argument_count == 3, "receive client and server executable paths")) {
		return EXIT_FAILURE;
	}
	oversized_note_arguments[0] = arguments[2];
	if (!check(minimm_test_run_program(oversized_note_arguments, 2, output, sizeof(output)),
		   "server rejects a page-aligned note size above its backing-file limit")) {
		return EXIT_FAILURE;
	}
	if (!check(minimm_test_server_start(arguments[2], &server, &port),
		   "server publishes an ephemeral loopback port")) {
		return EXIT_FAILURE;
	}
	if (!minimm_test_run_client_flow(arguments[1], port)) {
		minimm_test_force_cleanup(&server);
		return EXIT_FAILURE;
	}
	if (!check(minimm_test_server_stop(&server, SIGTERM),
		   "SIGTERM stops the server with exit status zero")) {
		minimm_test_force_cleanup(&server);
		return EXIT_FAILURE;
	}

	if (!check(minimm_test_server_start(arguments[2], &server, &port),
		   "server restarts on another ephemeral port")) {
		return EXIT_FAILURE;
	}
	if (!check(minimm_test_server_stop(&server, SIGINT),
		   "SIGINT stops the server with exit status zero")) {
		minimm_test_force_cleanup(&server);
		return EXIT_FAILURE;
	}

	if (!check(minimm_test_server_start(arguments[2], &server, &port),
		   "server starts for simultaneous termination signals")) {
		return EXIT_FAILURE;
	}
	if (!check(minimm_test_server_stop_with_pending_signals(&server),
		   "pending SIGINT and SIGTERM still produce exit status zero")) {
		minimm_test_force_cleanup(&server);
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
