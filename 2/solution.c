#include "parser.h"

#include <sys/wait.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

struct background_jobs {
    pid_t *pids;
    int count;
    int capacity;
};

bool
is_valid_pid_idx(struct background_jobs *bg_jobs, int idx) {
    return idx >= 0 && idx < bg_jobs->count;
}

void
init_bg_jobs(struct background_jobs *bg_jobs) {
    bg_jobs->pids = calloc(10, sizeof(pid_t));
    bg_jobs->count = 0;
    bg_jobs->capacity = 10;
}

void
add_bg_job(struct background_jobs *bg_jobs, pid_t pid) {
    if (bg_jobs->count == bg_jobs->capacity) {
        bg_jobs->capacity = bg_jobs->capacity * 2;
        bg_jobs->pids = realloc(bg_jobs->pids, bg_jobs->capacity * sizeof(pid_t));
    }

    bg_jobs->pids[bg_jobs->count] = pid;
    bg_jobs->count++;
}

void
delete_bg_job(struct background_jobs *bg_jobs, int idx) {
    if (!is_valid_pid_idx(bg_jobs, idx)) {
        return;
    }

    for (int i = idx; i < bg_jobs->count - 1; i++) {
        bg_jobs->pids[i] = bg_jobs->pids[i + 1];
    }

    bg_jobs->count--;
}

void
free_bg_jobs(struct background_jobs *bg_jobs) {
    free(bg_jobs->pids);
}

void
check_bg_jobs(struct background_jobs *bg_jobs) {
    int i = 0;
    while (i < bg_jobs->count) {
        int status;
        pid_t wpid = waitpid(bg_jobs->pids[i], &status, WNOHANG);

        if (wpid == -1) {
            delete_bg_job(bg_jobs, i);
        } else if (wpid == bg_jobs->pids[i]) {
            if (WIFEXITED(status) || WIFSIGNALED(status)) {
                delete_bg_job(bg_jobs, i);
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
}

static void
execute_command_line(const struct command_line *line, int *exit_code, struct background_jobs *bg_jobs)
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

                e = e->next;
                continue;
            }

            if (strcmp(cmd->exe, "exit") == 0 && !e->next && fd_in == STDIN_FILENO) {
                if (cmd->args) {
                    *exit_code = atoi(cmd->args[0]);
                } else {
                    *exit_code = 0;
                }
                break;
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
                return;
            }

            if (pid == 0) {
                if (fd_in != STDIN_FILENO) {
                    dup2(fd_in, STDIN_FILENO);
                    close(fd_in);
                }

                if (is_use_pipe) {
                    close(fds[0]);
                    dup2(fds[1], STDOUT_FILENO);
                    close(fds[1]);
                }

				if (is_use_file) {
				    int flags = O_WRONLY | O_CREAT;
				    if (out_type == OUTPUT_TYPE_FILE_NEW) {
				        flags |= O_TRUNC;
				    } else {
				        flags |= O_APPEND;
				    }

				    int fd = open(out_file, flags, 0644);
					dup2(fd, STDOUT_FILENO);
					close(fd);
				}

                if (strcmp(cmd->exe, "exit") == 0) {
                    if (cmd->args) {
                        *exit_code = atoi(cmd->args[0]) ;
                    } else {
	                    *exit_code = EXIT_SUCCESS;
                    }
                    _exit(*exit_code);
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

            if (line->is_background) {
                add_bg_job(bg_jobs, pid);
            } else {
                pids[pid_cnt++] = pid;
            }

			if (fd_in != STDIN_FILENO) {
				close(fd_in);
			}

			if (is_use_pipe) {
			    close(fds[1]);
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
    int exit_code = 0;
    struct parser *p = parser_new();
    struct background_jobs bg_jobs;
    init_bg_jobs(&bg_jobs);

    while ((rc = read(STDIN_FILENO, buf, buf_size)) > 0) {
        parser_feed(p, buf, rc);
        struct command_line *line = NULL;

        while (true) {
            enum parser_error err = parser_pop_next(p, &line);

            if (err == PARSER_ERR_NONE && line == NULL) {
                break;
            }

            if (err != PARSER_ERR_NONE) {
                printf("Error: %d\n", (int)err);
                continue;
            }

        	if (line->head && line->head->type == EXPR_TYPE_COMMAND) {
        		struct command *cmd = &line->head->cmd;
        		struct expr *next = line->head->next;

        		if (strcmp(cmd->exe, "exit") == 0 && (!next || next->type != EXPR_TYPE_PIPE)) {
        			exit_code = cmd->args ? atoi(cmd->args[0]) : 0;

        			command_line_delete(line);
        			parser_delete(p);
        			free_bg_jobs(&bg_jobs);

        			return exit_code;
				}
        	}

            execute_command_line(line, &exit_code, &bg_jobs);
            command_line_delete(line);
            check_bg_jobs(&bg_jobs);
        }
    }

    free_bg_jobs(&bg_jobs);
    parser_delete(p);
    return exit_code;
}