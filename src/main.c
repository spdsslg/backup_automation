#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include "config.h"
#include "filesystem_utils.h"
#include "monitor.h"
#include "restore.h"
#include "mirror.h"

#define MAX_ARGS 32

static volatile sig_atomic_t g_terminate = 0;
static volatile sig_atomic_t g_got_sigchld = 0;
static volatile sig_atomic_t g_child_exit = 0;

typedef struct {
  char *src;
  char *dst;
  pid_t pid;
  time_t created_at;
  int active;
} Backup;

typedef struct {
  Backup *backups;
  size_t backups_count;
  size_t backups_capacity;
} BackupList;


static BackupList g_list = {0};

static void on_parent_terminate(int sig) { g_terminate = 1; }

static void on_sigchld(int sig) { g_got_sigchld = 1; }

static void on_child_term(int sig) { g_child_exit = 1; }

static int sethandler(void (*f)(int), int sigNo) {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = f;
  sigemptyset(&act.sa_mask);

  if (sigaction(sigNo, &act, NULL) < 0) {
    perror("sigaction");
    return -1;
  }
  return 0;
}

static void install_parent_signals(void) {
  sethandler(on_parent_terminate, SIGINT);
  sethandler(on_parent_terminate, SIGTERM);
  sethandler(on_sigchld, SIGCHLD);
}

static void child_install_signals(void) {
  if (sethandler(on_child_term, SIGTERM) < 0)
    _exit(1);
}

// parsing
int parse_line(char *line, char *argv[MAX_ARGS],
               char argv_buf[MAX_ARGS][PATH_MAX], int *argc_out) {
  int argc = 0;
  char *p = line;
  while (*p) {
    while (isspace((unsigned char)*p)) {
      p++;
    }
    if (*p == '\0' || *p == '\n') {
      break;
    }

    if (argc >= MAX_ARGS) {
      printf("Too many arguments\n");
      return -1;
    }

    char *out = argv_buf[argc];
    int out_len = 0;
    out[0] = '\0';
    if (*p == '\'' || *p == '"') {
      char q = *p;
      p++;
      while (*p && q != *p) {
        if (q == '"' && *p == '\\') {
          p++;
          if (*p == '\0') // string ended after backslash
          {
            printf("Unexpected \\ or quote in the end of the argument\n");
            return -1;
          }
          char ch = *p;
          if (ch == '"' || ch == '\\') { // supports only \\ and \"
            if (out_len + 1 >= PATH_MAX) {
              printf("Path is too big\n");
              return -1;
            }
            out[out_len++] = ch;
            out[out_len] = '\0';
            p++;
          } else {
            printf("Unexpected escape sequence! Program supports only \\ and "
                   "\" inside arguments\n");
            return -1;
          }
        } else { // copy normal character
          if (out_len >= PATH_MAX) {
            printf("Path is too big\n");
            return -1;
          }
          out[out_len++] = *p;
          out[out_len] = '\0';
          p++;
        }
      }
      if (*p != q) {
        printf("No closing quote found\n");
        return -1;
      }
      p++;
    } else {
      while (*p && !isspace((unsigned char)*p)) {
        if (out_len >= PATH_MAX) {
          printf("Path is too big\n");
          return -1;
        }
        out[out_len++] = *p;
        out[out_len] = '\0';
        p++;
      }
    }
    argv[argc++] = out;
  }

  *argc_out = argc;
  return 0;
}

// restoring helpers
int check_src_against_backup(const char *src_path, const char *backup_path) {
  struct stat backup_st;
  if (lstat(backup_path, &backup_st) <
      0) { // if backup doesn't have smth, delete it from src
    if (errno == ENOENT) {
      return rm_tree(src_path); // rm_tree handles non-existent files
    }
    perror("lstat(check_src_against_backup)");
    return -1;
  }

  struct stat source_st;
  if (lstat(src_path, &source_st) < 0) {
    if (errno == ENOENT) {
      return 0;
    }
    perror("lstat(check_src_against_backup)");
    return -1;
  }

  int src_is_dir = S_ISDIR(source_st.st_mode) && !S_ISLNK(source_st.st_mode);
  int bck_is_dir = S_ISDIR(backup_st.st_mode) && !S_ISLNK(backup_st.st_mode);

  if (src_is_dir != bck_is_dir ||
      (S_ISREG(source_st.st_mode) != S_ISREG(backup_st.st_mode)) ||
      (S_ISLNK(source_st.st_mode) != S_ISLNK(backup_st.st_mode))) {
    return rm_tree(src_path);
  }

  if (!src_is_dir)
    return 0;

  DIR *dir;
  if ((dir = opendir(src_path)) == NULL) {
    perror("opendir(check_src_against_backup)");
    return -1;
  }
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;

    char src_child[PATH_MAX], bck_child[PATH_MAX];
    if (snprintf(src_child, PATH_MAX, "%s/%s", src_path, entry->d_name) >=
        PATH_MAX) {
      if (closedir(dir) < 0) {
        perror("closedir(check_src_against_backup)");
      }
      return -1;
    }
    if (snprintf(bck_child, PATH_MAX, "%s/%s", backup_path, entry->d_name) >=
        PATH_MAX) {
      if (closedir(dir) < 0) {
        perror("closedir(check_src_against_backup)");
      }
      return -1;
    }

    if (check_src_against_backup(src_child, bck_child) < 0) {
      if (closedir(dir) < 0) {
        perror("closedir(check_src_against_backup)");
      }
      return -1;
    }
  }

  if (closedir(dir) < 0) {
    perror("closedir(check_src_against_backup)");
    return -1;
  }
  return 0;
}

int apply_backup(const char *backup_path, const char *src_path,
                 const char *backup_real, const char *src_real,
                 time_t created_at) {
  struct stat backup_st;
  if (lstat(backup_path, &backup_st) < 0)
    return -1;

  if (S_ISDIR(backup_st.st_mode) && !S_ISLNK(backup_st.st_mode)) {
    if (mkdir_p(src_path, backup_st.st_mode & 0777) < 0)
      return -1;

    DIR *dir;
    if ((dir = opendir(backup_path)) == NULL) {
      perror("opendir(apply_backup)");
      return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
      if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
        continue;

      char bck_child[PATH_MAX], src_child[PATH_MAX];
      if (snprintf(bck_child, PATH_MAX, "%s/%s", backup_path, entry->d_name) >=
          PATH_MAX) {
        if (closedir(dir) < 0)
          perror("closedir(apply_backup)");
        return -1;
      }
      if (snprintf(src_child, PATH_MAX, "%s/%s", src_path, entry->d_name) >=
          PATH_MAX) {
        if (closedir(dir) < 0)
          perror("closedir(apply_backup)");
        return -1;
      }

      if (apply_backup(bck_child, src_child, backup_real, src_real,
                       created_at) < 0) {
        closedir(dir);
        return -1;
      }
    }
    if (closedir(dir) < 0) {
      perror("closedir(apply_backup)");
      return -1;
    }
    return 0;
  }

  struct stat source_st;
  int src_exists = (lstat(src_path, &source_st) == 0);
  int to_write = 0;
  if (!src_exists) {
    to_write = 1;
  } else if (source_st.st_mtime > created_at) {
    to_write = 1;
  }

  if (!to_write)
    return 0;

  // types dont match
  if (src_exists &&
      ((S_ISREG(source_st.st_mode) != S_ISREG(backup_st.st_mode)) ||
       (S_ISLNK(source_st.st_mode) != S_ISLNK(backup_st.st_mode)) ||
       (S_ISDIR(source_st.st_mode) &&
        !S_ISLNK(source_st.st_mode) != S_ISDIR(backup_st.st_mode) &&
        !S_ISLNK(backup_st.st_mode)))) {
    if (rm_tree(src_path) < 0)
      return -1;
  }

  if (ensure_parent_dir(src_path) < 0)
    return -1;

  if (S_ISREG(backup_st.st_mode)) {
    return copy_file(backup_path, src_path, backup_st.st_mode, &g_child_exit);
  }

  if (S_ISLNK(backup_st.st_mode)) {
    return copy_symplink_rewrite(backup_path, src_path, backup_real, src_real);
  }
  return 0;
}

// dynamic registry for backups
int ensure_capacity(BackupList *lst, size_t need) {
  if (lst->backups_capacity >= need) {
    return 0;
  }
  size_t new_capacity =
      (lst->backups_capacity == 0) ? 8 : (lst->backups_capacity * 2);
  while (new_capacity < need) {
    new_capacity *= 2;
  }
  Backup *new_backup = realloc(lst->backups, sizeof(Backup) * new_capacity);
  if (!new_backup) {
    return -1;
  }
  lst->backups = new_backup;
  lst->backups_capacity = new_capacity;
  return 0;
}

void free_backup(Backup *backup) {
  if (!backup) {
    return;
  }
  free(backup->dst);
  free(backup->src);
  backup->dst = NULL;
  backup->src = NULL;
  backup->created_at = 0;
  backup->active = 0;
}

int find_backup(char *src, char *dst) {
  for (int i = 0; i < (int)g_list.backups_count; i++) {
    if (strcmp(g_list.backups[i].src, src) == 0 &&
        strcmp(g_list.backups[i].dst, dst) == 0) {
      return i;
    }
  }
  return -1;
}

void reap_children() {
  while (1) {
    pid_t pid = waitpid(-1, NULL, WNOHANG);
    if (pid <= 0)
      break;

    for (size_t i = 0; i < g_list.backups_count; i++) {
      if (g_list.backups[i].active && g_list.backups[i].pid == pid) {
        g_list.backups[i].active = 0;
        g_list.backups[i].pid = 0;
        break;
      }
    }
  }
}

void child_loop(char *src, char *dst) {
  g_child_exit = 0; 
  child_install_signals();

  char src_real[PATH_MAX];

  if (norm_existing_dir(src, src_real) < 0) {
    _exit(0);
  }

  if (create_empty_dir(dst)) {
    _exit(0);
  }

  char dst_real[PATH_MAX];
  if (!realpath(dst, dst_real)) {
    _exit(0);
  }

  copy_tree(src_real, dst_real, src_real, dst_real, &g_child_exit);

  if (monitor_and_mirror(src_real, dst_real, &g_child_exit) < 0)
    _exit(1);
  _exit(0);
}

// spawning
static int spawn_backup(char *src, char *dst) {
  pid_t pid = fork();
  if (pid < 0) {
    perror("fork");
    return -1;
  }

  if (pid == 0) {
    child_loop(src, dst);
    _exit(EXIT_SUCCESS);
  }

  if (ensure_capacity(&g_list, g_list.backups_count + 1) < 0) {
    if (kill(pid, SIGTERM) < 0) {
      perror("kill");
    }
    return -1;
  }

  Backup new_backup;
  new_backup.src = strdup(src);
  new_backup.dst = strdup(dst);
  new_backup.pid = pid;
  new_backup.created_at = time(NULL);
  new_backup.active = 1;

  g_list.backups[g_list.backups_count++] = new_backup;
  return 0;
}

// commands
void cmd_help(void) {
  printf("Commands:\n");
  printf("  add <source> <target1> [target2 ...]\n");
  printf("  end <source> <target1> [target2 ...]\n");
  printf("  list\n");
  printf("  restore <source> <target>\n");
  printf("  exit\n");
}

void cmd_list() {
  reap_children();
  if (g_list.backups_count == 0) {
    printf("(no active backups)\n");
    return;
  }

  for (size_t i = 0; i < g_list.backups_count; i++) {
    if (g_list.backups[i].active) {
      printf("[ACTIVE] pid=%d src=\"%s\" dst=\"%s\"\n",
             (int)g_list.backups[i].pid, g_list.backups[i].src,
             g_list.backups[i].dst);
    } else {
      printf("[ENDED] src=\"%s\" dst=\"%s\"\n", g_list.backups[i].src,
             g_list.backups[i].dst);
    }
  }
}

void cmd_add(char *argv[], int argc) {
  if (argc < 3) {
    printf("usage: add <source> <target1> [target2 ...]\n");
    return;
  }

  char src_norm[PATH_MAX];
  if (norm_existing_dir(argv[1], src_norm) < 0) {
    printf("add: invalid source\n");
    return;
  }

  for (int i = 2; i < argc; i++) {
    char dst_norm[PATH_MAX];
    if (norm_target_path(argv[i], dst_norm) < 0) {
      printf("add: invalid target \"%s\"\n", argv[i]);
      continue;
    }

    if (has_prefix_path(dst_norm, src_norm)) {
      fprintf(stderr,
              "add: target is inside source (or same): src=\"%s\" dst=\"%s\"\n",
              src_norm, dst_norm);
      continue;
    }
    if (find_backup(src_norm, dst_norm) >= 0) {
      printf("add: already active src=\"%s\" dst=\"%s\"\n", src_norm, dst_norm);
      continue;
    }
    if (ensure_empty_dir(dst_norm) < 0) {
      perror("add: target invalid");
      continue;
    }
    if (spawn_backup(src_norm, dst_norm) >= 0) {
      printf("added src=\"%s\" -> dst=\"%s\"\n", src_norm, dst_norm);
    } else {
      printf("add failed for dst=\"%s\"\n", dst_norm);
    }
  }
}

void cmd_end(char *argv[], int argc) {
  if (argc < 3) {
    printf("usage: end <source> <target1> <target2> ...\n");
    return;
  }

  char src_norm[PATH_MAX];
  if (norm_existing_dir(argv[1], src_norm) < 0) {
    printf("add: invalid source\n");
    return;
  }

  for (int i = 2; i < argc; i++) {
    char dst_norm[PATH_MAX];
    if (norm_target_path(argv[i], dst_norm) < 0) {
      printf("add: invalid target \"%s\"\n", argv[i]);
      continue;
    }
    int index = find_backup(src_norm, dst_norm);
    if (index < 0) {
      printf("end: not found src=\"%s\" dst=\"%s\"\n", src_norm, dst_norm);
      continue;
    }

    if (!g_list.backups[index].active) {
      printf("end: already ended src=\"%s\" dst=\"%s\"\n",
             g_list.backups[index].src, g_list.backups[index].dst);
      continue;
    }

    if (kill(g_list.backups[index].pid, SIGTERM) < 0) {
      perror("kill");
    }
    if (waitpid(g_list.backups[index].pid, NULL, 0) < 0) {
      perror("waitpid");
    }
    g_list.backups[index].active = 0;
    g_list.backups[index].pid = 0;

    printf("ended src=\"%s\" dst=\"%s\" (backup kept for restore)\n",
           g_list.backups[index].src, g_list.backups[index].dst);
  }
}

void cmd_restore(char *argv[], int argc) {
  if (argc != 3) {
    printf("usage: end <source> <target1> <target2> ...\n");
    return;
  }

  char src_norm[PATH_MAX];
  if (norm_existing_dir(argv[1], src_norm) < 0) {
    printf("restore: invalid source\n");
    return;
  }

  char dst_norm[PATH_MAX];
  if (norm_target_path(argv[2], dst_norm) < 0) {
    printf("restore: invalid target \"%s\"\n", argv[2]);
    return;
  }

  int index = find_backup(src_norm, dst_norm);
  if (index < 0) {
    printf("restore: backup not found for this pair\n");
    return;
  }

  time_t created_at = g_list.backups[index].created_at;
  if (g_list.backups[index].active) {
    pid_t pid = g_list.backups[index].pid;
    if (kill(pid, SIGTERM) < 0) {
      perror("kill");
    }
    if (waitpid(pid, NULL, 0) < 0) {
      perror("waitpid");
    }
    g_list.backups[index].active = 0;
    g_list.backups[index].pid = 0;
  }

  if (check_src_against_backup(src_norm, dst_norm) < 0) {
    return;
  }
  if (apply_backup(dst_norm, src_norm, dst_norm, src_norm, created_at) < 0) {
    perror("apply backup");
    return;
  }

  printf("restored src=\"%s\" from backup=\"%s\"\n", src_norm, dst_norm);
}

int main() {
  install_parent_signals();
  cmd_help();

  char line[4096];
  while (!g_terminate) {
    reap_children();

    printf("> ");
    fflush(stdout);

    errno = 0;
    if (!fgets(line, sizeof(line), stdin)) {
      if (errno == EINTR) {
        printf("\n");
        continue;
      }
      break;
    }

    char *argv[MAX_ARGS];
    char args_buf[MAX_ARGS][PATH_MAX];
    int argc = 0;

    if (parse_line(line, argv, args_buf, &argc) < 0) {
      printf("parse error\n");
      continue;
    }
    if (argc == 0) {
      continue;
    }

    if (strcmp(argv[0], "help") == 0)
      cmd_help();
    else if (strcmp(argv[0], "list") == 0)
      cmd_list();
    else if (strcmp(argv[0], "add") == 0)
      cmd_add(argv, argc);
    else if (strcmp(argv[0], "end") == 0)
      cmd_end(argv, argc);
    else if (strcmp(argv[0], "restore") == 0)
      cmd_restore(argv, argc);
    else if (strcmp(argv[0], "exit") == 0)
      break;
    else
      printf("unknown command: %s\n", argv[0]);
  }

  for (size_t i = 0; i < g_list.backups_count; i++) {
    if (g_list.backups[i].active) {
      kill(g_list.backups[i].pid, SIGTERM);
    }
  }

  for (size_t i = 0; i < g_list.backups_count; i++) {
    if (g_list.backups[i].active) {
      if (waitpid(g_list.backups[i].pid, NULL, 0) < 0) {
        perror("waitpid");
      }
      g_list.backups[i].active = 0;
      g_list.backups[i].pid = 0;
    }
  }

  for (size_t i = 0; i < g_list.backups_count; i++)
    free_backup(&g_list.backups[i]);
  free(g_list.backups);

  return 0;
}