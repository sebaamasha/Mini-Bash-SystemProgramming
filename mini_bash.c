#include <unistd.h>    
#include <stdlib.h>     
#include <string.h>     
#include <sys/wait.h>   
#include <errno.h>      
#include <stdio.h>      // perror 


#define PROMPT "mini-bash $ "
#define PROMPT_LEN 11
#define INITIAL_BUF 256
#define MAX_ARGS 128
#define PATH_BUF 4096

/*Write a full C-string to a file descriptor*/
static void write_str(int fd, const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    (void)write(fd, s, n); // ignore partial writes for simplicity (OK for this exercise)
}

/*Write a single character*/
static void write_chr(int fd, char c) {
    (void)write(fd, &c, 1);
}

/*Convert an integer to decimal text and print it*/
static void write_int(int fd, int x) {
    char buf[32];
    int i = 0;

    if (x == 0) {
        write_chr(fd, '0');
        return;
    }

    if (x < 0) {
        write_chr(fd, '-');
        if (x == (-2147483647 - 1)) { // INT_MIN for 32-bit
            write_str(fd, "2147483648");
            return;
        }
        x = -x;
    }

    // Build digits in reverse order
    while (x > 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (x % 10));
        x /= 10;
    }

    // Print in correct order
    while (i > 0) {
        write_chr(fd, buf[--i]);
    }
}

/*Return 1 if the input line contains only whitespace characters, otherwise 0*/
static int starts_empty_or_ws(const char *s) {
    for (size_t i = 0; s[i] != '\0'; i++) {
        if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n')
            return 0;
    }
    return 1;
}

/*Read one full line from stdin*/
static ssize_t read_line(char **buf, size_t *cap) {
    if (*buf == NULL || *cap == 0) {
        *cap = INITIAL_BUF;
        *buf = (char *)malloc(*cap);
        if (!*buf) return -1;
    }

    size_t len = 0;
    while (1) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1); 

        if (r == 0) { // EOF
            if (len == 0) return -1; // no input at all
            break;                   // return partial line
        }
        if (r < 0) { // read error
            return -1;
        }

        // Ensure space for new char + '\0'
        if (len + 2 > *cap) {
            size_t new_cap = (*cap) * 2;
            char *nb = (char *)realloc(*buf, new_cap);
            if (!nb) return -1;
            *buf = nb;
            *cap = new_cap;
        }

        (*buf)[len++] = c;
        if (c == '\n') break;
    }

    (*buf)[len] = '\0'; // null-terminate for string operations
    return (ssize_t)len;
}

/*Parse the line into tokens separated by spaces/tabs/newlines*/
static int parse_line(char *line, char *argv[], int max_args) {
    int argc = 0;

    // Skip leading whitespace
    while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
        line++;

    while (*line != '\0' && argc < max_args - 1) {
        // token begins here
        argv[argc++] = line;

        // move until separator/end
        while (*line != '\0' && *line != ' ' && *line != '\t' &&
               *line != '\r' && *line != '\n') {
            line++;
        }

        if (*line == '\0') break;

        // terminate token
        *line = '\0';
        line++;

        // skip following whitespace
        while (*line == ' ' || *line == '\t' || *line == '\r' || *line == '\n')
            line++;
    }

    argv[argc] = NULL;
    return argc;
}


static size_t str_len(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') n++;
    return n;
}

/* build_path: prefix + "/" + cmd into out buffer */
static int build_path(char *out, size_t out_sz, const char *prefix, const char *cmd) {
    size_t p = str_len(prefix);
    size_t c = str_len(cmd);

    if (p + 1 + c + 1 > out_sz) return 0;

    for (size_t i = 0; i < p; i++) out[i] = prefix[i];
    out[p] = '/';
    for (size_t i = 0; i < c; i++) out[p + 1 + i] = cmd[i];
    out[p + 1 + c] = '\0';
    return 1;
}

/*
 Search command executable in required order:
  1) $HOME/<cmd>
  2) /bin/<cmd>
*/
static int find_executable(const char *cmd, char *out, size_t out_sz) {
    const char *home = getenv("HOME"); // reads environment variable

    if (home && home[0] != '\0') {
        if (build_path(out, out_sz, home, cmd)) {
            if (access(out, X_OK) == 0) return 1; // has execute permission
        }
    }

    if (build_path(out, out_sz, "/bin", cmd)) {
        if (access(out, X_OK) == 0) return 1;
    }

    return 0;
}

/*
 Print the required message format:
 [command_name]: Unknown Command
 */
static void print_unknown(const char *cmd) {
    write_chr(STDERR_FILENO, '[');
    write_str(STDERR_FILENO, cmd);
    write_str(STDERR_FILENO, "]: Unknown Command\n");
}

/* 
  After waitpid(), report:
  - success + return code if exited normally
  - signal number if terminated by signal
 */
static void report_status(int status) {
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        write_str(STDOUT_FILENO, "Command executed successfully. Return code: ");
        write_int(STDOUT_FILENO, code);
        write_chr(STDOUT_FILENO, '\n');
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        write_str(STDOUT_FILENO, "Command terminated by signal: ");
        write_int(STDOUT_FILENO, sig);
        write_chr(STDOUT_FILENO, '\n');
    } else {
        write_str(STDOUT_FILENO, "Command finished. (unknown status)\n");
    }
}

/*
  Run a non-builtin command using:
  1) find_executable() according to required order
  2) fork() to create child process
  3) child: execv(path, argv) to replace process image
  4) parent: waitpid() to wait for child termination
 */
static void execute_external(char *argv[]) {
    char path[PATH_BUF];

    if (!find_executable(argv[0], path, sizeof(path))) {
        print_unknown(argv[0]);
        return;
    }

    pid_t pid = fork(); 
    if (pid < 0) {
        perror("fork");
        return;
    }

    if (pid == 0) {
        // Child process: load and run the executable
        execv(path, argv); 

        // If execv returns, it failed:
        perror("execv");   // required in assignment
        _exit(1);          // exit child immediately without flushing stdio buffers
    }

    // Parent process: wait for child
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) { 
        perror("waitpid");
        return;
    }

    report_status(status);
}

/*
  Internal command cd:
  Must run in the shell process itself, so it affects future commands.
  Implemented using chdir() system call.
 
  Behavior:
  - "cd <dir>" -> go to <dir>
  - "cd"       -> go to $HOME
 */
static void builtin_cd(char *argv[], int argc) {
    const char *target = NULL;

    if (argc >= 2) target = argv[1];
    else target = getenv("HOME");

    if (!target) {
        write_str(STDERR_FILENO, "cd: HOME not set\n");
        return;
    }

    if (chdir(target) != 0) {
        perror("chdir");
    }
}

int main(void) {
    char *line = NULL;
    size_t cap = 0;

    while (1) {
        // 1) Print prompt 
        if (write(STDOUT_FILENO, PROMPT, PROMPT_LEN) < 0) {
            perror("write");
            break;
        }

        // 2) Read command line
        ssize_t nread = read_line(&line, &cap);
        if (nread < 0) {
            // EOF
            write_chr(STDOUT_FILENO, '\n');
            break;
        }

        // Ignore empty/whitespace
        if (starts_empty_or_ws(line)) continue;

        // 3) Parse line into argv[]
        char *argv[MAX_ARGS];
        int argc = parse_line(line, argv, MAX_ARGS);
        if (argc == 0) continue;

        // 4) Execute: builtins or external
        if (strcmp(argv[0], "exit") == 0) {
            // exit: terminate shell
            break;
        } else if (strcmp(argv[0], "cd") == 0) {
            // cd: internal command
            builtin_cd(argv, argc);
        } else {
            // external command
            execute_external(argv);
        }
    }

    // Free dynamic input buffer
    free(line);
    return 0;
}
