#include "parser.h"

#include <sys/wait.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

static void
execute_command_line(const struct command_line *line, int *exit_code)
{
	struct expr *e = line->head;
	enum output_type out_type = line->out_type;
	char *out_file = line->out_file;

	if (!e) {
		return;
	}

	int fds[2];

	pid_t pid;
	int pid_cnt = 0;
	pid_t pids[1024];

	int fd_in = STDIN_FILENO;

	while (e) {
		if (e->type == EXPR_TYPE_COMMAND) {
			struct command *cmd = &e->cmd;

			if (strcmp(cmd->exe, "cd") == 0) {
				if (chdir(cmd->args[0]) == -1) {
					perror("cd failed");
				}

				break;
			}

			if (strcmp(cmd->exe, "exit") == 0 && !e->next && fd_in == STDIN_FILENO) {
				if (cmd->args) {
					exit(atoi(cmd->args[0]));
				}

				exit(0);
			}

			bool is_use_pipe = e->next && e->next->type == EXPR_TYPE_PIPE;
			bool is_use_file = (out_type == OUTPUT_TYPE_FILE_NEW || out_type == OUTPUT_TYPE_FILE_APPEND)\
			&& !e->next && out_file;

			if (is_use_pipe) {
				pipe(fds);
			}

			pid = fork();
			if (pid == -1) {
				perror("fork failed");
				exit(EXIT_FAILURE);
			}

			if (pid == 0) {
				if (strcmp(cmd->exe, "exit") == 0) {
					if (cmd->args) {
						exit(atoi(cmd->args[0]));
					}

					exit(EXIT_SUCCESS);
				}

				if (fd_in != STDIN_FILENO) {
					dup2(fd_in, STDIN_FILENO);
					close(fd_in);
				}

				if (is_use_pipe) {
					dup2(fds[1], STDOUT_FILENO);
					close(fds[1]);

					close(fds[0]);
				}

				if (is_use_file) {
					int flags = out_type == OUTPUT_TYPE_FILE_NEW ? O_RDWR | O_TRUNC | O_CREAT : O_RDWR | O_APPEND;
					int fd = open(out_file, flags, 0644);
					dup2(fd, STDOUT_FILENO);
					close(fd);
				}

				size_t arg_count = cmd->arg_count + 2;
				char **argv = calloc(arg_count, sizeof(char *));

				argv[0] = cmd->exe;
				memcpy(argv + 1, cmd->args, cmd->arg_count * sizeof(char *));
				argv[arg_count - 1] = NULL;

				execvp(argv[0], argv);
				perror("execvp failed");
				free(argv);
				exit(EXIT_FAILURE);
			}

			pids[pid_cnt++] = pid;

			if (is_use_pipe) {
				close(fds[1]);
			}

			if (fd_in != STDIN_FILENO) {
				close(fd_in);
			}

			if (is_use_pipe) {
				fd_in = fds[0];
			} else {
				fd_in = STDIN_FILENO;
			}
		}

		e = e->next;
	}

	for (int i = 0; i < pid_cnt; i++) {
		int status;
		waitpid(pids[i], &status, 0);

		if (WIFEXITED(status)) {
			*exit_code = WEXITSTATUS(status);
		}
	}
}

int
main(void)
{
	const size_t buf_size = 1024;
	char buf[buf_size];
	int rc;
	int exit_code;
	struct parser *p = parser_new();
	while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
		parser_feed(p, buf, rc);
		struct command_line *line = NULL;
		while (true) {
			enum parser_error err = parser_pop_next(p, &line);
			if (err == PARSER_ERR_NONE && line == NULL)
				break;
			if (err != PARSER_ERR_NONE) {
				printf("Error: %d\n", (int)err);
				continue;
			}
			execute_command_line(line, &exit_code);
			command_line_delete(line);
		}
	}
	parser_delete(p);
	return exit_code;
}
