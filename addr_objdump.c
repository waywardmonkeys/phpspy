#include "phpspy.h"

static int get_php_bin_path(pid_t pid, char *path_root, char *path);
static int get_php_base_addr(pid_t pid, char *path_root, char *path,
                             uint64_t *raddr);
static int get_symbol_offset(char *path_root, const char *symbol,
                             uint64_t *raddr);
static int popen_read_line(char *buf, size_t buf_size, char *cmd_fmt, ...);

int get_symbol_addr(addr_memo_t *memo, pid_t pid, const char *symbol,
                    uint64_t *raddr) {
  char *php_bin_path, *php_bin_path_root;
  uint64_t *php_base_addr;
  uint64_t addr_offset;
  php_bin_path = memo->php_bin_path;
  php_bin_path_root = memo->php_bin_path_root;
  php_base_addr = &memo->php_base_addr;
  if (*php_bin_path == '\0' &&
      get_php_bin_path(pid, php_bin_path_root, php_bin_path) != 0) {
    return 1;
  } else if (*php_base_addr == 0 &&
             get_php_base_addr(pid, php_bin_path_root, php_bin_path,
                               php_base_addr) != 0) {
    return 1;
  } else if (get_symbol_offset(php_bin_path_root, symbol, &addr_offset) != 0) {
    return 1;
  }
  *raddr = *php_base_addr + addr_offset;
  return 0;
}

static int get_php_bin_path(pid_t pid, char *path_root, char *path) {
  char buf[PHPSPY_STR_SIZE -
           16];  // TODO: This magic number compensates for format string size
  char *cmd_fmt =
      "awk '/libphp[78]/{print $NF; exit 0} END{exit 1}' /proc/%d/maps"
      " || readlink /proc/%d/exe";
  if (popen_read_line(buf, sizeof(buf), cmd_fmt, (int)pid, (int)pid) != 0) {
    log_error("get_php_bin_path: Failed\n");
    return 1;
  }
  snprintf(path_root, PHPSPY_STR_SIZE, "/proc/%d/root/%s", (int)pid, buf);
  if (access(path_root, F_OK) != 0) {
    snprintf(path_root, PHPSPY_STR_SIZE, "/proc/%d/exe", (int)pid);
  }
  strcpy(path, buf);
  return 0;
}

static int get_php_base_addr(pid_t pid, char *path_root, char *path,
                             uint64_t *raddr) {
  /**
   * This is very likely to be incorrect/incomplete. I thought the base
   * address from `/proc/<pid>/maps` + the symbol address from `readelf` would
   * lead to the actual memory address, but on at least one system I tested on
   * this is not the case. On that system, working backwards from the address
   * printed in `gdb`, it seems the missing piece was the 'virtual address' of
   * the LOAD section in ELF headers. I suspect this may have to do with
   * address relocation and/or a feature called 'prelinking', but not sure.
   */
  char buf[PHPSPY_STR_SIZE];
  uint64_t start_addr;
  uint64_t virt_addr;
  char *cmd_fmt = "grep -m1 ' %s$' /proc/%d/maps";
  if (popen_read_line(buf, sizeof(buf), cmd_fmt, path, (int)pid) != 0) {
    log_error("get_php_base_addr: Failed to get start_addr\n");
    return 1;
  }
  start_addr = strtoull(buf, NULL, 16);
  cmd_fmt = "objdump -p %s | awk '/LOAD/{print $5; exit}'";
  if (popen_read_line(buf, sizeof(buf), cmd_fmt, path_root) != 0) {
    log_error("get_php_base_addr: Failed to get virt_addr\n");
    return 1;
  }
  virt_addr = strtoull(buf, NULL, 16);
  *raddr = start_addr - virt_addr;
  return 0;
}

static int get_symbol_offset(char *path_root, const char *symbol,
                             uint64_t *raddr) {
  char buf[PHPSPY_STR_SIZE];
  char *cmd_fmt = "objdump -Tt %s | awk '/ %s$/{print $1; exit}'";
  if (popen_read_line(buf, sizeof(buf), cmd_fmt, path_root, symbol) != 0) {
    log_error("get_symbol_offset: Failed\n");
    return 1;
  }
  *raddr = strtoull(buf, NULL, 16);
  return 0;
}

static int popen_read_line(char *buf, size_t buf_size, char *cmd_fmt, ...) {
  FILE *fp;
  char cmd[PHPSPY_STR_SIZE];
  int buf_len;
  va_list cmd_args;
  va_start(cmd_args, cmd_fmt);
  vsprintf(cmd, cmd_fmt, cmd_args);
  va_end(cmd_args);
  if (!(fp = popen(cmd, "r"))) {
    perror("popen");
    return 1;
  }
  if (fgets(buf, buf_size - 1, fp) == NULL) {
    log_error("popen_read_line: No stdout; cmd=%s\n", cmd);
    pclose(fp);
    return 1;
  }
  pclose(fp);
  buf_len = strlen(buf);
  while (buf_len > 0 && buf[buf_len - 1] == '\n') {
    --buf_len;
  }
  if (buf_len < 1) {
    log_error("popen_read_line: Expected strlen(buf)>0; cmd=%s\n", cmd);
    return 1;
  }
  buf[buf_len] = '\0';
  return 0;
}
