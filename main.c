#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "dirent.h"


#define MAX_HISTORY 1000

static char *history[MAX_HISTORY];
static int history_count = 0;
static int history_saved = 0;

void history_add(const char *line);
int  history_read(const char *file);
int  history_write(const char *file, int from, const char *mode);
void builtin_history(char *args[], int arg_count);


#define MAX_VARS 256

static char *var_names[MAX_VARS];
static char *var_values[MAX_VARS];
static int var_count = 0;

int  is_valid_name(const char *name, size_t len);
int  var_set(const char *name, const char *value);
const char *var_get(const char *name);
void builtin_declare(char *args[], int arg_count);


#define MAX_COMPLETERS 64
#define MAX_CANDIDATES 256

static char *comp_cmds[MAX_COMPLETERS];
static char *comp_scripts[MAX_COMPLETERS];
static int comp_count = 0;
static char *comp_results[MAX_CANDIDATES];   // candidates from the last script run
static int comp_n = 0;

void builtin_complete(char *args[], int arg_count);
int  completer_index(const char *cmd);
int  run_completer(const char *script, const char *cmd,
                   const char *word, const char *prev);
char *script_generator(const char *text, int state);


#define MAX_JOBS 64

typedef struct {
  int num;        // job number shown as [N]
  pid_t pid;
  char *cmd;      // command text, without the trailing &
  int done;
} Job;

static Job jobs[MAX_JOBS];
static int job_count = 0;

void job_add(pid_t pid, const char *cmd);
void job_notify(void);
void builtin_jobs(void);
int run_command(char *args[], int arg_count, const char *job_text, int background);


#define MAX_STAGES 16

int is_builtin(const char *name);
int run_pipeline(char *args[], int arg_count, const char *job_text, int background);


void find_file(char *command);
int parse_command(const char *in, char *buf, size_t cap, char *args[]);

int extract_redirects(char *args[], int *argc,char **out_file, int *out_append,char **err_file, int *err_append);
int redirect_fd(int target_fd, const char *file, int append, int *saved);
void restore_fd(int target_fd, int saved);


static const char *builtins[] = {
  "exit", "echo", "type", "pwd", "cd", "history", "declare", "cls", "clear", "complete", "jobs", NULL
};
char **shell_completion(const char *text, int start, int end);
char  *command_generator(const char *text, int state);


int main(int argc, char *argv[]) {
  // Flush after every printf
  setbuf(stdout, NULL);

  char *histfile = getenv("HISTFILE");

  if (histfile != NULL) {
    history_read(histfile);
    history_saved = history_count;
  }

  rl_attempted_completion_function = shell_completion;

  while (1) {

    char command[1024];

    job_notify();
    char *line = readline("$ ");
    if (line == NULL) break;
    
    strncpy(command, line, sizeof(command) - 1);
    command[sizeof(command) - 1] = '\0';
    free(line);
    
    if (command[0] == '\0') continue;

    history_add(command);

    /*
    * Parse command.
    */
    char parsed[4096];
    char *args[100];

    int arg_count = parse_command(command, parsed, sizeof(parsed), args);
    if (arg_count == 0) continue;
  
    /* Trailing & means run in the background. */
    int background = 0;
    if (!strcmp(args[arg_count - 1], "&")) {
      background = 1;
      args[--arg_count] = NULL;
    }

    if (arg_count == 0) continue;
  
    /* Command text for the jobs table: the typed line without the trailing & */
    char job_text[1024];
    strcpy(job_text, command);
    size_t jl = strlen(job_text);
    
    while (jl > 0 && isspace((unsigned char)job_text[jl - 1])) jl--;

    if (background && jl > 0 && job_text[jl - 1] == '&') jl--;

    while (jl > 0 && isspace((unsigned char)job_text[jl - 1])) jl--;

    job_text[jl] = '\0';

    /* Pipeline? Run it and go back to the prompt. */
    int has_pipe = 0;
    for (int i = 0; i < arg_count; i++)
      if (strcmp(args[i], "|") == 0) has_pipe = 1;
    
    if (has_pipe) {
      run_pipeline(args, arg_count, job_text, background);
      continue;
    }

    /*
     * Pull out any redirects.
     */
    char *out_file = NULL, *err_file = NULL;
    int out_append = 0, err_append = 0;

    if (extract_redirects(args, &arg_count,
                          &out_file, &out_append,
                          &err_file, &err_append) < 0)
      continue;
    if (arg_count == 0) continue;

    /*
     * Apply redirects.
     */
    int saved_out = -1, saved_err = -1;

    if (out_file && redirect_fd(1, out_file, out_append, &saved_out) < 0)
      continue;

    if (err_file && redirect_fd(2, err_file, err_append, &saved_err) < 0) {
      restore_fd(1, saved_out);
      continue;
    }

    /*
     * Run, flush, then restore.
     */
    int should_exit = run_command(args, arg_count, job_text, background);

    fflush(stdout);
    restore_fd(1, saved_out);
    restore_fd(2, saved_err);

    if (should_exit) break;
  }

  if (histfile != NULL)
    history_write(histfile, 0, "w");

  return 0;
}

int run_command(char *args[], int arg_count, const char *job_text, int background) {

  /*
   * =========================
   * EXIT
   * =========================
   */
  if (strcmp(args[0], "exit") == 0) {
    return 1;
  }
  /*
   * =========================
   * ECHO
   * =========================
   */
  else if (strcmp(args[0], "echo") == 0) {
    for (int i = 1; i < arg_count; i++) {
      if (i > 1) {
        printf(" ");
      }
      printf("%s", args[i]);
    }
    printf("\n");
  }
  /*
   * =========================
   * CLEAR / CLS
   * =========================
   */
  else if (strcmp(args[0], "cls") == 0 || strcmp(args[0], "clear") == 0) {
    printf("\033[2J\033[H");
  }
  /*
   * =========================
   * PWD
   * =========================
   */
  else if (strcmp(args[0], "pwd") == 0) {
    char pwd[1024];
    if (getcwd(pwd, sizeof(pwd)) != NULL) {
      printf("%s\n", pwd);
    }
  }
  /*
   * =========================
   * CD
   * =========================
   */
  else if (strcmp(args[0], "cd") == 0) {
    
    char *path = args[1];
    if (path == NULL || !strcmp(path, "~"))
      path = getenv("HOME");
    if (path == NULL) {
      fprintf(stderr, "cd: HOME not set\n");
    } 
    else if (chdir(path) == -1) {
      fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
    }
  }
  /*
   * =========================
   * TYPE
   * =========================
   */
  else if (strcmp(args[0], "type") == 0){
    bool in_builtin = false;
    for (int i = 0; builtins[i] != NULL ; i++) {
      if (!strcmp(args[1], builtins[i])) {
        printf("%s is a shell builtin\n", args[1]);
        in_builtin = true;
        break;
      }
      
    }
    if (!in_builtin)
      find_file(args[1]);
  }
  /*
   * =========================
   * HISTORY
   * =========================
   */
  else if (strcmp(args[0], "history") == 0) {
    builtin_history(args, arg_count);
  }
  /*
   * =========================
   * DECLARE
   * =========================
   */
  else if (strcmp(args[0], "declare") == 0) {
    builtin_declare(args, arg_count);
  }
  /*
   * =========================
   * COMPLETE
   * =========================
   */
  else if (strcmp(args[0], "complete") == 0) {
  builtin_complete(args, arg_count);
  }
  /*
   * =========================
   * JOBS
   * =========================
   */
  else if (strcmp(args[0], "jobs") == 0) {
    builtin_jobs();
  }
  /*
   * =========================
   * EXTERNAL COMMAND
   * =========================
   */
  else {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
      perror("fork");
    }
    else if (pid == 0) {
      execvp(args[0], args);
      fprintf(stderr, "%s: command not found\n", args[0]);
      exit(127);
    }
    else if (background) {
      job_add(pid, job_text);          // don't wait
    }
    else {
      int status;
      waitpid(pid, &status, 0);
    }
  }

  return 0;
}

void find_file(char *command) {
  char *path_env = getenv("PATH");

  if (path_env == NULL) {
    printf("%s: not found\n", command);
    return;
  }

  char *PATH = strdup(path_env);

  for (char *p = strtok(PATH, ":"); p != NULL; p = strtok(NULL, ":")) {

    char fp[1028];

    sprintf(fp, "%s/%s", p, command);

    if (access(fp, X_OK) == 0) {
      printf("%s is %s\n", command, fp);
      free(PATH);
      return;
    }
  }

  free(PATH);

  printf("%s: not found\n", command);
}

int parse_command(const char *in, char *buf, size_t cap, char *args[]) {

  int argc = 0;
  size_t out = 0;
  size_t start = 0;

  int arg_started = 0;
  int in_single_quotes = 0;
  int in_double_quotes = 0;

  for (size_t i = 0; in[i] != '\0'; i++) {

    char c = in[i];

    if (out + 2 >= cap) break;        // leave room for the terminator

    /* BACKSLASH */
    if (c == '\\' && !in_single_quotes) {
      char next = in[i + 1];

      if (!in_double_quotes && next != '\0') {
        if (!arg_started) { start = out; arg_started = 1; }
        buf[out++] = next;
        i++;
        continue;
      }
      if (in_double_quotes &&
          (next == '"' || next == '\\' || next == '$' || next == '`')) {
        buf[out++] = next;
        i++;
        continue;
      }
    }

    /* SINGLE QUOTE */
    if (c == '\'' && !in_double_quotes) {
      if (!arg_started) { start = out; arg_started = 1; }
      in_single_quotes = !in_single_quotes;
      continue;
    }

    /* DOUBLE QUOTE */
    if (c == '"' && !in_single_quotes) {
      if (!arg_started) { start = out; arg_started = 1; }
      in_double_quotes = !in_double_quotes;
      continue;
    }

    /* VARIABLE EXPANSION: $NAME or ${NAME} (not inside single quotes) */
    if (c == '$' && !in_single_quotes) {
      char name[256];
      size_t nlen = 0;
      size_t j = i + 1;
      int ok = 0;

      if (in[j] == '{') {
        const char *close = strchr(in + j, '}');
        if (close) {
          nlen = (size_t)(close - (in + j + 1));
          if (nlen < sizeof(name) && is_valid_name(in + j + 1, nlen)) {
            memcpy(name, in + j + 1, nlen);
            name[nlen] = '\0';
            j = (size_t)(close - in);          // index of the '}'
            ok = 1;
          }
        }
      } else if (isalpha((unsigned char)in[j]) || in[j] == '_') {
        while ((isalnum((unsigned char)in[j]) || in[j] == '_')
               && nlen < sizeof(name) - 1)
          name[nlen++] = in[j++];
        name[nlen] = '\0';
        j--;                                   // index of the last name char
        ok = 1;
      }

      if (ok) {
        const char *val = var_get(name);
        size_t vlen = val ? strlen(val) : 0;

        /* An unquoted empty expansion produces no argument at all. */
        if ((vlen > 0 || in_double_quotes) && !arg_started) {
          start = out;
          arg_started = 1;
        }
        if (out + vlen + 2 >= cap) vlen = cap - out - 2;
        if (vlen > 0) { memcpy(buf + out, val, vlen); out += vlen; }

        i = j;                                 // skip the name
        continue;
      }
      /* Not a valid name: fall through, `$` is a literal character. */
    }

    /* WHITESPACE OUTSIDE QUOTES */
    if (isspace((unsigned char)c) && !in_single_quotes && !in_double_quotes) {
      if (arg_started) {
        buf[out++] = '\0';
        args[argc++] = buf + start;
        arg_started = 0;
      }
      continue;
    }

    /* NORMAL CHARACTER */
    if (!arg_started) { start = out; arg_started = 1; }
    buf[out++] = c;
  }

  if (arg_started) {
    buf[out++] = '\0';
    args[argc++] = buf + start;
  }

  args[argc] = NULL;
  return argc;
}

/*
 * Removes redirect operators (and their filenames) from args.
 * Returns -1 on a syntax error, 0 otherwise.
 */
int extract_redirects(char *args[], int *argc,
                      char **out_file, int *out_append,
                      char **err_file, int *err_append) {
  int w = 0;

  for (int i = 0; i < *argc; i++) {
    char *a = args[i];

    int is_out     = !strcmp(a, ">")   || !strcmp(a, "1>");
    int is_out_app = !strcmp(a, ">>")  || !strcmp(a, "1>>");
    int is_err     = !strcmp(a, "2>");
    int is_err_app = !strcmp(a, "2>>");

    if (is_out || is_out_app || is_err || is_err_app) {
      if (i + 1 >= *argc) {
        fprintf(stderr, "syntax error: expected a filename after '%s'\n", a);
        return -1;
      }
      if (is_out || is_out_app) {
        *out_file = args[i + 1];
        *out_append = is_out_app;
      } else {
        *err_file = args[i + 1];
        *err_append = is_err_app;
      }
      i++;               // skip the filename too
      continue;
    }

    args[w++] = a;       // keep normal arguments
  }

  args[w] = NULL;
  *argc = w;
  return 0;
}

/*
 * Points target_fd (1 or 2) at `file`. Saves the old descriptor
 * in *saved so restore_fd can undo it.
 */
int redirect_fd(int target_fd, const char *file, int append, int *saved) {
  int flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  int fd = open(file, flags, 0644);

  if (fd < 0) {
    fprintf(stderr, "%s: %s\n", file, strerror(errno));
    return -1;
  }

  *saved = dup(target_fd);   // remember where it used to point
  dup2(fd, target_fd);       // target_fd now writes to the file
  close(fd);                 // the extra descriptor is no longer needed
  return 0;
}

void restore_fd(int target_fd, int saved) {
  if (saved >= 0) {
    dup2(saved, target_fd);
    close(saved);
  }
}

void history_add(const char *line) {
  if (history_count == MAX_HISTORY) {          // full: drop the oldest
    free(history[0]);
    memmove(history, history + 1, (MAX_HISTORY - 1) * sizeof(char *));
    history_count--;
    if (history_saved > 0) history_saved--;
  }
  history[history_count++] = strdup(line);
  add_history(line);
}

/* Appends every non-empty line of `file` to the in-memory history. */
int history_read(const char *file) {
  FILE *fp = fopen(file, "r");
  if (fp == NULL) return -1;

  char line[1024];
  while (fgets(line, sizeof(line), fp)) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0] != '\0') history_add(line);
  }
  fclose(fp);
  return 0;
}

/* Writes entries [from, history_count) using mode "w" or "a". */
int history_write(const char *file, int from, const char *mode) {
  FILE *fp = fopen(file, mode);
  if (fp == NULL) return -1;

  for (int i = from; i < history_count; i++)
    fprintf(fp, "%s\n", history[i]);

  fclose(fp);
  history_saved = history_count;
  return 0;
}

void builtin_history(char *args[], int arg_count) {

  /* history -r|-w|-a <file> */
  if (arg_count >= 3 &&
      (!strcmp(args[1], "-r") || !strcmp(args[1], "-w") || !strcmp(args[1], "-a"))) {

    int rc;
    if (!strcmp(args[1], "-r")) {
      rc = history_read(args[2]);
      history_saved = history_count;      // loaded entries are already on disk
    } else if (!strcmp(args[1], "-w")) {
      rc = history_write(args[2], 0, "w");
    } else {
      rc = history_write(args[2], history_saved, "a");
    }

    if (rc < 0)
      fprintf(stderr, "history: %s: %s\n", args[2], strerror(errno));
    return;
  }

  /* history [N] */
  int start = 0;
  if (arg_count >= 2) {
    char *end;
    long n = strtol(args[1], &end, 10);
    if (*end != '\0' || n < 0) {
      fprintf(stderr, "history: %s: numeric argument required\n", args[1]);
      return;
    }
    if (n < history_count) start = history_count - (int)n;
  }

  for (int i = start; i < history_count; i++)
    printf("%5d  %s\n", i + 1, history[i]);
}

int is_valid_name(const char *name, size_t len) {
  if (len == 0 || !(isalpha((unsigned char)name[0]) || name[0] == '_'))
    return 0;
  for (size_t i = 1; i < len; i++)
    if (!(isalnum((unsigned char)name[i]) || name[i] == '_'))
      return 0;
  return 1;
}

int var_set(const char *name, const char *value) {
  for (int i = 0; i < var_count; i++) {
    if (!strcmp(var_names[i], name)) {
      free(var_values[i]);
      var_values[i] = strdup(value);
      return 0;
    }
  }
  if (var_count == MAX_VARS) return -1;
  var_names[var_count]  = strdup(name);
  var_values[var_count] = strdup(value);
  var_count++;
  return 0;
}

/* Shell variables first, then the environment ($HOME, $PATH...). */
const char *var_get(const char *name) {
  for (int i = 0; i < var_count; i++)
    if (!strcmp(var_names[i], name)) return var_values[i];
  return getenv(name);   // NULL if unset
}

void builtin_declare(char *args[], int arg_count) {
  int i = 1;
  int print = 0;

  if (i < arg_count && strcmp(args[i], "-p") == 0) {
    print = 1;
    i++;
  }

  /* declare -p  (no names): list every shell variable */
  if (print && i >= arg_count) {
    for (int v = 0; v < var_count; v++)
      printf("declare -- %s=\"%s\"\n", var_names[v], var_values[v]);
    return;
  }

  for (; i < arg_count; i++) {
    if (print) {
      /* declare -p NAME */
      const char *val = NULL;
      for (int v = 0; v < var_count; v++)
        if (!strcmp(var_names[v], args[i])) val = var_values[v];

      if (val) printf("declare -- %s=\"%s\"\n", args[i], val);
      else     fprintf(stderr, "declare: %s: not found\n", args[i]);
    } else {
      /* declare NAME=value  or  declare NAME */
      char *eq = strchr(args[i], '=');
      size_t nlen = eq ? (size_t)(eq - args[i]) : strlen(args[i]);

      if (!is_valid_name(args[i], nlen)) {
        fprintf(stderr, "declare: `%s': not a valid identifier\n", args[i]);
        continue;
      }

      char name[256];
      if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
      memcpy(name, args[i], nlen);
      name[nlen] = '\0';

      if (eq) var_set(name, eq + 1);
      else if (!var_get(name)) var_set(name, "");
    }
  }
}

char *command_generator(const char *text, int state) {
  static int idx;
  static size_t len;
  static char *path_dup = NULL;
  static char *cursor = NULL;
  static char *saveptr = NULL;
  static DIR *dir = NULL;
  static char dirpath[1024];

  if (state == 0) {
    idx = 0;
    len = strlen(text);
    if (dir) { closedir(dir); dir = NULL; }
    free(path_dup);
    const char *p = getenv("PATH");
    path_dup = p ? strdup(p) : NULL;
    cursor = path_dup;
  }

  /* 1. builtins */
  while (builtins[idx] != NULL) {
    const char *name = builtins[idx++];
    if (strncmp(name, text, len) == 0)
      return strdup(name);
  }

  /* 2. executables in PATH */
  if (path_dup == NULL) return NULL;

  while (1) {
    if (dir == NULL) {
      char *d = strtok_r(cursor, ":", &saveptr);
      cursor = NULL;                       // only pass the string on the first call
      if (d == NULL) {
        free(path_dup);
        path_dup = NULL;
        return NULL;                       // no more directories
      }
      strncpy(dirpath, d, sizeof(dirpath) - 1);
      dirpath[sizeof(dirpath) - 1] = '\0';
      dir = opendir(dirpath);
      if (dir == NULL) continue;           // skip directories that don't exist
    }

    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
      if (strncmp(e->d_name, text, len) != 0) continue;

      char full[2048];
      snprintf(full, sizeof(full), "%s/%s", dirpath, e->d_name);

      struct stat st;
      if (stat(full, &st) == 0 && S_ISREG(st.st_mode) && access(full, X_OK) == 0)
        return strdup(e->d_name);
    }

    closedir(dir);
    dir = NULL;
  }
}

char **shell_completion(const char *text, int start, int end) {
  (void)end;

  /* First word: builtins and PATH executables */
  if (start == 0) {
    rl_attempted_completion_over = 1;
    return rl_completion_matches(text, command_generator);
  }

  /* Command name = first word of the line */
  char cmd[256];
  int n = 0;
  while (rl_line_buffer[n] && rl_line_buffer[n] != ' ' && n < (int)sizeof(cmd) - 1) {
    cmd[n] = rl_line_buffer[n];
    n++;
  }
  cmd[n] = '\0';

  int idx = completer_index(cmd);
  if (idx < 0) return NULL;             // no completer: normal filename completion

  /* Word before the one being completed */
  char prev[256] = "";
  int e = start;
  while (e > 0 && rl_line_buffer[e - 1] == ' ') e--;
  int s = e;
  while (s > 0 && rl_line_buffer[s - 1] != ' ') s--;
  if (e - s > 0 && e - s < (int)sizeof(prev)) {
    memcpy(prev, rl_line_buffer + s, e - s);
    prev[e - s] = '\0';
  }

  rl_attempted_completion_over = 1;     // never mix in filenames
  if (run_completer(comp_scripts[idx], cmd, text, prev) <= 0)
    return NULL;                        // script failed or gave nothing: bell

  return rl_completion_matches(text, script_generator);
}

int completer_index(const char *cmd) {
  for (int i = 0; i < comp_count; i++)
    if (!strcmp(comp_cmds[i], cmd)) return i;
  return -1;
}

void builtin_complete(char *args[], int arg_count) {

  /* complete -C <script> <command> */
  if (arg_count >= 4 && !strcmp(args[1], "-C")) {
    int idx = completer_index(args[3]);
    if (idx >= 0) {
      free(comp_scripts[idx]);
      comp_scripts[idx] = strdup(args[2]);
    } else if (comp_count < MAX_COMPLETERS) {
      comp_cmds[comp_count]    = strdup(args[3]);
      comp_scripts[comp_count] = strdup(args[2]);
      comp_count++;
    }
    return;
  }

  /* complete -p [<command>] */
  if (arg_count >= 2 && !strcmp(args[1], "-p")) {
    if (arg_count == 2) {                         // list everything
      for (int i = 0; i < comp_count; i++)
        printf("complete -C '%s' %s\n", comp_scripts[i], comp_cmds[i]);
      return;
    }
    int idx = completer_index(args[2]);
    if (idx >= 0)
      printf("complete -C '%s' %s\n", comp_scripts[idx], comp_cmds[idx]);
    else
      fprintf(stderr, "complete: %s: no completion specification\n", args[2]);
    return;
  }

  /* complete -r <command> */
  if (arg_count >= 3 && !strcmp(args[1], "-r")) {
    int idx = completer_index(args[2]);
    if (idx >= 0) {
      free(comp_cmds[idx]);
      free(comp_scripts[idx]);
      comp_count--;
      comp_cmds[idx]    = comp_cmds[comp_count];      // move the last entry into the gap
      comp_scripts[idx] = comp_scripts[comp_count];
    } else {
      fprintf(stderr, "complete: %s: no completion specification\n", args[2]);
    }
    return;
  }

  fprintf(stderr, "complete: usage: complete [-C script command] [-p [command]] [-r command]\n");
}

int run_completer(const char *script, const char *cmd,
                  const char *word, const char *prev) {
  for (int i = 0; i < comp_n; i++) free(comp_results[i]);
  comp_n = 0;

  int fd[2];
  if (pipe(fd) < 0) return -1;

  fflush(stdout);
  pid_t pid = fork();
  if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }

  if (pid == 0) {                       // child: run the script
    close(fd[0]);
    dup2(fd[1], STDOUT_FILENO);
    close(fd[1]);

    char point[32];
    snprintf(point, sizeof(point), "%d", rl_point);
    setenv("COMP_LINE", rl_line_buffer, 1);
    setenv("COMP_POINT", point, 1);

    execl(script, script, cmd, word, prev, (char *)NULL);
    _exit(127);                         // exec failed
  }

  close(fd[1]);                         // parent: read its output
  FILE *fp = fdopen(fd[0], "r");
  char *line = NULL;
  size_t cap = 0;

  while (getline(&line, &cap, fp) != -1 && comp_n < MAX_CANDIDATES) {
    line[strcspn(line, "\n")] = '\0';
    if (line[0] != '\0')
      comp_results[comp_n++] = strdup(line);
  }

  free(line);
  fclose(fp);
  waitpid(pid, NULL, 0);
  return comp_n;
}

/* Hands the stored candidates to readline one at a time. */
char *script_generator(const char *text, int state) {
  static int i;
  (void)text;
  if (state == 0) i = 0;
  return (i < comp_n) ? strdup(comp_results[i++]) : NULL;
}

/* Smallest positive number not used by a current job. */
static int next_job_num(void) {
  for (int n = 1; ; n++) {
    int used = 0;
    for (int i = 0; i < job_count; i++)
      if (jobs[i].num == n) used = 1;
    if (!used) return n;
  }
}

void job_add(pid_t pid, const char *cmd) {
  if (job_count == MAX_JOBS) return;
  jobs[job_count].num  = next_job_num();
  jobs[job_count].pid  = pid;
  jobs[job_count].cmd  = strdup(cmd);
  jobs[job_count].done = 0;
  fprintf(stderr, "[%d] %d\n", jobs[job_count].num, (int)pid);
  job_count++;
}

/* Non-blocking check: mark every job that has finished. */
static void job_update(void) {
  for (int i = 0; i < job_count; i++) {
    if (jobs[i].done) continue;
    int status;
    if (waitpid(jobs[i].pid, &status, WNOHANG) == jobs[i].pid)
      jobs[i].done = 1;
  }
}

static void job_print(int i) {
  char mark = ' ';
  if (i == job_count - 1)      mark = '+';   // most recent job
  else if (i == job_count - 2) mark = '-';   // the one before it

  printf("[%d]%c  %-24s%s%s\n",
         jobs[i].num, mark,
         jobs[i].done ? "Done" : "Running",
         jobs[i].cmd,
         jobs[i].done ? "" : " &");
}

/* Delete finished jobs (backwards so indexes stay valid). */
static void job_remove_done(void) {
  for (int i = job_count - 1; i >= 0; i--) {
    if (!jobs[i].done) continue;
    free(jobs[i].cmd);
    memmove(&jobs[i], &jobs[i + 1], (job_count - i - 1) * sizeof(Job));
    job_count--;
  }
}

/* Called before each prompt: report jobs that finished meanwhile. */
void job_notify(void) {
  job_update();
  for (int i = 0; i < job_count; i++)
    if (jobs[i].done) job_print(i);
  job_remove_done();
}

void builtin_jobs(void) {
  job_update();
  for (int i = 0; i < job_count; i++)
    job_print(i);
  job_remove_done();
}

int is_builtin(const char *name) {
  for (int i = 0; builtins[i] != NULL; i++)
    if (strcmp(name, builtins[i]) == 0) return 1;
  return 0;
}

/*
 * args has already been parsed. Splits it at each "|" and runs the
 * stages connected by pipes. Returns -1 on a syntax or setup error.
 */
int run_pipeline(char *args[], int arg_count, const char *job_text, int background) {

  char **stage[MAX_STAGES];
  int stage_argc[MAX_STAGES];
  int n = 0;

  /* 1. Split args into stages; each "|" becomes a NULL terminator. */
  int start = 0;
  for (int i = 0; i <= arg_count; i++) {
    if (i == arg_count || strcmp(args[i], "|") == 0) {
      if (i == start) {
        fprintf(stderr, "syntax error near unexpected token `|'\n");
        return -1;
      }
      if (n == MAX_STAGES) {
        fprintf(stderr, "too many commands in pipeline\n");
        return -1;
      }
      stage[n] = &args[start];
      stage_argc[n] = i - start;
      n++;
      if (i < arg_count) args[i] = NULL;
      start = i + 1;
    }
  }

  /* 2. One pipe between each pair of neighbouring stages. */
  int fds[MAX_STAGES][2];
  for (int k = 0; k < n - 1; k++) {
    if (pipe(fds[k]) < 0) {
      perror("pipe");
      for (int j = 0; j < k; j++) { close(fds[j][0]); close(fds[j][1]); }
      return -1;
    }
  }

  /* 3. Fork one child per stage. */
  pid_t pids[MAX_STAGES];
  int started = 0;
  fflush(stdout);

  for (int k = 0; k < n; k++) {
    pid_t pid = fork();

    if (pid < 0) {
      perror("fork");
      break;
    }

    if (pid == 0) {
      /* Wire stdin/stdout to the neighbouring pipes. */
      if (k > 0)     dup2(fds[k - 1][0], STDIN_FILENO);
      if (k < n - 1) dup2(fds[k][1], STDOUT_FILENO);

      /* Close every pipe fd; the dup2 copies are all we need. */
      for (int j = 0; j < n - 1; j++) {
        close(fds[j][0]);
        close(fds[j][1]);
      }

      /* Redirects inside a stage (cmd > file | ...) win over the pipe. */
      char **sa = stage[k];
      int sc = stage_argc[k];
      char *out_file = NULL, *err_file = NULL;
      int out_append = 0, err_append = 0;

      if (extract_redirects(sa, &sc, &out_file, &out_append,
                            &err_file, &err_append) < 0 || sc == 0)
        _exit(1);

      int saved_out = -1, saved_err = -1;
      if (out_file && redirect_fd(1, out_file, out_append, &saved_out) < 0) _exit(1);
      if (err_file && redirect_fd(2, err_file, err_append, &saved_err) < 0) _exit(1);
      if (saved_out >= 0) close(saved_out);
      if (saved_err >= 0) close(saved_err);

      if (is_builtin(sa[0])) {
        run_command(sa, sc, "", 0);
        fflush(stdout);
        _exit(0);
      }

      execvp(sa[0], sa);
      fprintf(stderr, "%s: command not found\n", sa[0]);
      _exit(127);
    }

    pids[started++] = pid;
  }

  /* 4. Parent: close all pipe ends, otherwise readers never see EOF. */
  for (int j = 0; j < n - 1; j++) {
    close(fds[j][0]);
    close(fds[j][1]);
  }

  /* 5. Wait for every stage, or register the pipeline as a job. */
  if (background && started > 0) {
    job_add(pids[started - 1], job_text);
  } else {
    for (int k = 0; k < started; k++)
      waitpid(pids[k], NULL, 0);
  }

  return 0;
}