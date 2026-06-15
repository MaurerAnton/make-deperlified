/* GNU Make test driver - replaces test_driver.pl and run_make_tests.pl
   with a pure C89/C90-compatible implementation.

Copyright (C) 2025 Free Software Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation; either version 3 of the License, or (at your option) any later
version.

GNU Make is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
A PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <https://www.gnu.org/licenses/>.  */

/*
   Test driver for GNU Make test suite.

   Usage: run_make_tests [options] [testname...]

   Options:
     -make PATH       Path to make binary (default: "make")
     -debug           Enable debug output
     -verbose         Verbose test output
     -detail          Detailed test output
     -keep            Keep temporary files after test
     -help, -usage    Show usage
     -all-tests       Run all known tests regardless of platform

   Test files use a simple INI-like format (see test_template.test).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <regex.h>

#ifndef PATH_MAX
# define PATH_MAX 4096
#endif

/* ---- Platform detection ---- */

static const char *os_name = "";
static const char *port_type = "UNIX";
static const char *path_sep = "/";
static int short_filenames = 0;

static void
detect_os (void)
{
#if defined (_WIN32) || defined (__MSYS__) || defined (__MINGW32__)
  port_type = "W32";
  os_name = "Windows";
#elif defined (__MSDOS__) || defined (__DOS__)
  port_type = "DOS";
  os_name = "DOS";
  short_filenames = 1;
#elif defined (__OS2__)
  port_type = "OS/2";
  os_name = "OS/2";
#elif defined (__VMS)
  port_type = "VMS";
  os_name = "VMS";
#else
  port_type = "UNIX";
  {
    FILE *f = fopen ("/etc/os-release", "r");
    if (f)
      {
        char buf[256];
        while (fgets (buf, sizeof (buf), f))
          {
            if (strncmp (buf, "PRETTY_NAME=", 12) == 0)
              {
                char *p = buf + 12;
                char *q;
                /* Remove quotes and newline */
                if (*p == '"') p++;
                q = strchr (p, '"');
                if (q) *q = '\0';
                q = strchr (p, '\n');
                if (q) *q = '\0';
                os_name = strdup (p);
                break;
              }
          }
        fclose (f);
      }
    if (!*os_name)
      os_name = "POSIX";
  }
#endif

  /* Test for short filenames on non-DOS systems */
  if (!short_filenames)
    {
      FILE *f = fopen ("fancy.file.name", "w");
      if (!f)
        short_filenames = 1;
      else
        {
          fclose (f);
          unlink ("fancy.file.name");
        }
    }
}

/* ---- String utilities ---- */

static char *
xstrdup (const char *s)
{
  char *r;
  size_t len;
  if (!s) return NULL;
  len = strlen (s);
  r = malloc (len + 1);
  if (!r) { perror ("malloc"); exit (1); }
  memcpy (r, s, len + 1);
  return r;
}

static void *
xmalloc (size_t n)
{
  void *p = malloc (n);
  if (!p) { perror ("malloc"); exit (1); }
  return p;
}

static void *
xrealloc (void *p, size_t n)
{
  p = realloc (p, n);
  if (!p) { perror ("realloc"); exit (1); }
  return p;
}

static char *
strip_newline (char *s)
{
  size_t len = strlen (s);
  while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r'))
    s[--len] = '\0';
  return s;
}

static char *
trim (char *s)
{
  char *end;
  while (isspace ((unsigned char)*s)) s++;
  if (*s == '\0') return s;
  end = s + strlen (s) - 1;
  while (end > s && isspace ((unsigned char)*end)) end--;
  *(end + 1) = '\0';
  return s;
}

/* ---- Dynamic string ---- */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} dstr;

static void
dstr_init (dstr *ds, size_t initial)
{
  ds->data = xmalloc (initial);
  ds->data[0] = '\0';
  ds->len = 0;
  ds->cap = initial;
}

static void
dstr_free (dstr *ds)
{
  free (ds->data);
  ds->data = NULL;
  ds->len = ds->cap = 0;
}

static void
dstr_append (dstr *ds, const char *s, size_t n)
{
  if (ds->len + n + 1 > ds->cap)
    {
      ds->cap = (ds->len + n + 1) * 2;
      ds->data = xrealloc (ds->data, ds->cap);
    }
  memcpy (ds->data + ds->len, s, n);
  ds->len += n;
  ds->data[ds->len] = '\0';
}

static void
dstr_adds (dstr *ds, const char *s)
{
  dstr_append (ds, s, strlen (s));
}

static void
dstr_addc (dstr *ds, char c)
{
  dstr_append (ds, &c, 1);
}

/* ---- Path utilities ---- */

static char *
path_join (const char *dir, const char *file)
{
  dstr ds;
  size_t dlen;
  dstr_init (&ds, 256);
  dstr_adds (&ds, dir);
  dlen = strlen (dir);
  if (dlen > 0 && dir[dlen-1] != '/' && dir[dlen-1] != '\\')
    dstr_addc (&ds, '/');
  dstr_adds (&ds, file);
  return ds.data;
}

static char *
path_dirname (const char *path)
{
  const char *p = strrchr (path, '/');
  const char *q = strrchr (path, '\\');
  if (!p || (q && q > p)) p = q;
  if (!p) return xstrdup (".");
  {
    size_t len = (size_t)(p - path);
    char *r = xmalloc (len + 1);
    memcpy (r, path, len);
    r[len] = '\0';
    if (len == 0) { r[0] = '/'; r[1] = '\0'; }
    return r;
  }
}

static const char *
path_basename (const char *path)
{
  const char *p = strrchr (path, '/');
  const char *q = strrchr (path, '\\');
  if (!p || (q && q > p)) p = q;
  return p ? p + 1 : path;
}

/* ---- Find executable in PATH ---- */

static char *
find_in_path (const char *cmd)
{
  const char *path_env;
  char *path_copy;
  char *dir;
  char *save;

  /* If cmd has a directory component, check directly */
  if (strchr (cmd, '/') || strchr (cmd, '\\'))
    {
      struct stat st;
      if (stat (cmd, &st) == 0 && (st.st_mode & S_IXUSR))
        return xstrdup (cmd);
      return NULL;
    }

  path_env = getenv ("PATH");
  if (!path_env) return NULL;

  path_copy = xstrdup (path_env);
  dir = strtok_r (path_copy, ":", &save);
  while (dir)
    {
      char *full = path_join (dir, cmd);
      struct stat st;
      if (stat (full, &st) == 0 && (st.st_mode & S_IXUSR))
        {
          free (path_copy);
          return full;
        }
      free (full);
      dir = strtok_r (NULL, ":", &save);
    }
  free (path_copy);
  return NULL;
}

/* ---- Global state ---- */

static int debug = 0;
static int verbose = 0;
static int detail = 0;
static int keep = 0;
static int all_tests = 0;

static const char *make_path = "make";
static const char *make_name = "make";
static char *mkpath = NULL;
static char *perl_name = NULL;  /* Now thelp path */
static char *helper_tool = NULL;
static char *diff_name = NULL;
static char *sh_name = "/bin/sh";
static int is_posix_sh = 1;

static const char *work_dir = "work";
static const char *temp_dir = "_tmp";
static const char *script_dir = "scripts";
static char *src_path = NULL;
static char *top_path = NULL;
static char *work_path = NULL;
static char *temp_path = NULL;
static char *cwd_path = NULL;
static char *testee_version = NULL;

/* Test statistics */
static int categories_run = 0;
static int categories_passed = 0;
static int total_tests_run = 0;
static int total_tests_passed = 0;
static int tests_run = 0;
static int tests_passed = 0;
static int some_test_failed = 0;

static int test_timeout = 60;

/* The current test name and path */
static char *current_test = NULL;
static char *current_testdir = NULL;
static int num_logfiles = 0;
static int num_tmpfiles = 0;
static char *last_makefile = NULL;
static int test_passed = 1;
static char *command_string = NULL;

/* Log file extensions */
static const char *log_ext = "log";
static const char *diff_ext = "diff";
static const char *base_ext = "base";
static const char *run_ext = "run";

/* ---- File operations ---- */

static void
touch_file (const char *filename)
{
  FILE *f = fopen (filename, "a");
  if (f)
    {
      fprintf (f, "\n");
      fclose (f);
    }
}

static void
touch_files (int n, char **files)
{
  int i;
  for (i = 0; i < n; i++)
    touch_file (files[i]);
}

static int
create_file (const char *filename, const char *content)
{
  FILE *f = fopen (filename, "w");
  if (!f) return 0;
  if (content)
    fputs (content, f);
  fclose (f);
  return 1;
}

static char *
read_file (const char *filename)
{
  FILE *f;
  long size;
  char *buf;

  f = fopen (filename, "r");
  if (!f) return NULL;

  fseek (f, 0, SEEK_END);
  size = ftell (f);
  fseek (f, 0, SEEK_SET);

  buf = xmalloc (size + 1);
  if (fread (buf, 1, size, f) != (size_t)size)
    {
      free (buf);
      fclose (f);
      return NULL;
    }
  buf[size] = '\0';
  fclose (f);
  return buf;
}

static int
remove_dir_recursive (const char *path)
{
  DIR *dir;
  struct dirent *ent;
  struct stat st;

  if (lstat (path, &st) != 0)
    return 1;  /* Doesn't exist, success */

  if (!S_ISDIR (st.st_mode))
    return unlink (path) == 0;

  dir = opendir (path);
  if (!dir) return 0;

  while ((ent = readdir (dir)) != NULL)
    {
      char *child;
      if (strcmp (ent->d_name, ".") == 0 || strcmp (ent->d_name, "..") == 0)
        continue;
      child = path_join (path, ent->d_name);
      remove_dir_recursive (child);
      free (child);
    }
  closedir (dir);
  return rmdir (path) == 0;
}

/* Create a directory and all parent directories as needed */
static int
mkdir_p (const char *path)
{
  char *tmp;
  char *p;
  struct stat st;

  if (stat (path, &st) == 0)
    return S_ISDIR (st.st_mode) ? 0 : -1;

  tmp = xstrdup (path);
  p = strrchr (tmp, '/');
  if (p && p != tmp)
    {
      *p = '\0';
      if (mkdir_p (tmp) != 0)
        {
          free (tmp);
          return -1;
        }
      *p = '/';
    }
  free (tmp);

#ifdef WIN32
  return mkdir (path);
#else
  return mkdir (path, 0777);
#endif
}

/* ---- Temp file naming ---- */

static char *
get_prefix (int num)
{
  char buf[32];
  snprintf (buf, sizeof (buf), "t%03d.", num);
  return xstrdup (buf);
}

static char *
get_logfile (int no_increment)
{
  char *pfx;
  char *result;
  if (!no_increment) num_logfiles++;
  pfx = get_prefix (num_logfiles);
  result = xmalloc (strlen (pfx) + strlen (log_ext) + 1);
  sprintf (result, "%s%s", pfx, log_ext);
  free (pfx);
  return result;
}

static char *
get_basefile (void)
{
  char *pfx = get_prefix (num_logfiles);
  char *result = xmalloc (strlen (pfx) + strlen (base_ext) + 1);
  sprintf (result, "%s%s", pfx, base_ext);
  free (pfx);
  return result;
}

static char *
get_difffile (void)
{
  char *pfx = get_prefix (num_logfiles);
  char *result = xmalloc (strlen (pfx) + strlen (diff_ext) + 1);
  sprintf (result, "%s%s", pfx, diff_ext);
  free (pfx);
  return result;
}

static char *
get_runfile (void)
{
  char *pfx = get_prefix (num_logfiles);
  char *result = xmalloc (strlen (pfx) + strlen (run_ext) + 1);
  sprintf (result, "%s%s", pfx, run_ext);
  free (pfx);
  return result;
}

static char *
get_tmpfile (int no_increment)
{
  char *pfx;
  char *result;
  if (!no_increment) num_tmpfiles++;
  pfx = get_prefix (num_tmpfiles);
  result = xmalloc (strlen (pfx) + 3);
  sprintf (result, "%smk", pfx);
  free (pfx);
  return result;
}

/* ---- Environment management ---- */

static void
setup_clean_env (void)
{
  /* Set locale to C for consistent test results */
  setenv ("LC_ALL", "C", 1);
  setenv ("LANG", "C", 1);
  setenv ("LANGUAGE", "C", 1);
}

/* ---- Output comparison ---- */

static char *
normalize_output (const char *str)
{
  char *result;
  const char *s;
  char *d;

  result = xmalloc (strlen (str) + 1);
  d = result;

  for (s = str; *s; s++)
    {
      /* Skip "clock skew detected" messages */
      if (strncmp (s, "make: Warning: File", 19) == 0)
        {
          const char *rest = s + 19;
          while (*rest && *rest != '\n') rest++;
          s = (*rest == '\n') ? rest : rest - 1;
          continue;
        }
      /* Skip "modification time in the future" */
      if (strstr (s, "modification time") && strstr (s, "in the future"))
        {
          while (*s && *s != '\n') s++;
          if (*s == '\0') break;
          continue;
        }
      /* Normalize \r\n to \n */
      if (*s == '\r' && *(s+1) == '\n')
        continue;
      *d++ = *s;
    }
  *d = '\0';
  return result;
}

static int
compare_output (const char *expected, const char *logfile)
{
  char *actual;
  char *norm_actual;
  char *norm_expected;
  int result = 0;

  tests_run++;

  if (!expected)
    {
      tests_passed++;
      return 1;
    }

  actual = read_file (logfile);
  if (!actual)
    {
      fprintf (stderr, "Cannot read log file %s\n", logfile);
      return 0;
    }

  norm_actual = normalize_output (actual);
  norm_expected = normalize_output (expected);

  if (strcmp (norm_actual, norm_expected) == 0)
    result = 1;
  else
    {
      /* Check if expected is a regex pattern: /pattern/.
         Also check line-by-line for multiline answers. */
      {
        size_t elen = strlen (norm_expected);
        /* Single-line regex: entire answer is /pattern/ */
        if (elen >= 2 && norm_expected[0] == '/' && norm_expected[elen-1] == '/')
          {
            char *pat = xstrdup (norm_expected + 1);
            pat[elen - 2] = '\0';
            {
              regex_t re;
              if (regcomp (&re, pat, REG_EXTENDED | REG_NOSUB) == 0)
                {
                  if (regexec (&re, norm_actual, 0, NULL, 0) == 0)
                    result = 1;
                  regfree (&re);
                }
            }
            free (pat);
          }
        /* Multi-line: check each line for /pattern/ format */
        if (!result && strchr (norm_expected, '\n'))
          {
            char *ex_copy = xstrdup (norm_expected);
            char *line = strtok (ex_copy, "\n");
            char *act_copy = xstrdup (norm_actual);
            char *aline = strtok (act_copy, "\n");
            int all_match = 1;
            while (line && aline && all_match)
              {
                size_t llen = strlen (line);
                if (llen >= 2 && line[0] == '/' && line[llen-1] == '/')
                  {
                    char *pat = xstrdup (line + 1);
                    pat[llen - 2] = '\0';
                    regex_t re;
                    if (regcomp (&re, pat, REG_EXTENDED | REG_NOSUB) == 0)
                      {
                        if (regexec (&re, aline, 0, NULL, 0) != 0)
                          all_match = 0;
                        regfree (&re);
                      }
                    else
                      all_match = 0;
                    free (pat);
                  }
                else
                  {
                    if (strcmp (line, aline) != 0)
                      all_match = 0;
                  }
                line = strtok (NULL, "\n");
                aline = strtok (NULL, "\n");
              }
            if (all_match && !line && !aline)
              result = 1;
            free (ex_copy);
            free (act_copy);
          }
      }

      if (!result)
        {
          /* Also try with backslash normalization */
          char *a2 = xstrdup (norm_actual);
          char *e2 = xstrdup (norm_expected);
          char *p;
          for (p = a2; *p; p++) if (*p == '\\') *p = '/';
          for (p = e2; *p; p++) if (*p == '\\') *p = '/';
          if (strcmp (a2, e2) == 0)
            result = 1;
          free (a2);
          free (e2);
        }
    }

  if (result)
    tests_passed++;

  free (actual);
  free (norm_actual);
  free (norm_expected);
  return result;
}

/* ---- Running commands with timeout ---- */

static int
run_command_with_timeout (char *argv[], int timeout_secs)
{
  pid_t pid;
  int status;

  pid = fork ();
  if (pid == 0)
    {
      execvp (argv[0], argv);
      fprintf (stderr, "exec %s: %s\n", argv[0], strerror (errno));
      _exit (127);
    }
  else if (pid < 0)
    {
      perror ("fork");
      return -1;
    }

  if (timeout_secs > 0)
    {
      /* Simple timeout using alarm in parent via a signal */
      void (*old_alarm)(int) = signal (SIGALRM, SIG_DFL);
      alarm (timeout_secs);
      if (waitpid (pid, &status, 0) < 0)
        {
          if (errno == EINTR)
            {
              kill (pid, SIGKILL);
              waitpid (pid, &status, 0);
              alarm (0);
              signal (SIGALRM, old_alarm);
              return 14;  /* Timeout signal */
            }
        }
      alarm (0);
      signal (SIGALRM, old_alarm);
    }
  else
    {
      waitpid (pid, &status, 0);
    }

  if (WIFEXITED (status))
    return WEXITSTATUS (status) << 8;  /* Shift to match Perl's $? */
  if (WIFSIGNALED (status))
    return (WTERMSIG (status) << 8) | 0x7f;
  return -1;
}

static int
run_command_with_output (const char *logfile, char *argv[])
{
  pid_t pid;
  int status;
  int fd;

  fd = open (logfile, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    {
      perror (logfile);
      return -1;
    }

  pid = fork ();
  if (pid == 0)
    {
      dup2 (fd, STDOUT_FILENO);
      dup2 (fd, STDERR_FILENO);
      close (fd);
      execvp (argv[0], argv);
      fprintf (stderr, "exec %s: %s\n", argv[0], strerror (errno));
      _exit (127);
    }
  else if (pid < 0)
    {
      perror ("fork");
      close (fd);
      return -1;
    }

  close (fd);

  if (test_timeout > 0)
    {
      void (*old_alarm)(int) = signal (SIGALRM, SIG_DFL);
      alarm (test_timeout);
      if (waitpid (pid, &status, 0) < 0)
        {
          if (errno == EINTR)
            {
              kill (pid, SIGKILL);
              waitpid (pid, &status, 0);
              alarm (0);
              signal (SIGALRM, old_alarm);
              return 14;
            }
        }
      alarm (0);
      signal (SIGALRM, old_alarm);
    }
  else
    {
      waitpid (pid, &status, 0);
    }

  if (WIFEXITED (status))
    return WEXITSTATUS (status) << 8;
  if (WIFSIGNALED (status))
    return 128 + WTERMSIG (status);
  return -1;
}

/* ---- Token substitution ---- */

static char *
subst_tokens (const char *str)
{
  dstr ds;
  const char *p = str;
  dstr_init (&ds, strlen (str) + 256);

  while (*p)
    {
      if (*p == '\\' && *(p+1))
        {
          switch (*(p+1))
            {
            case 't': dstr_addc (&ds, '\t'); p += 2; continue;
            case 'n': dstr_addc (&ds, '\n'); p += 2; continue;
            case 'r': dstr_addc (&ds, '\r'); p += 2; continue;
            case '\\': dstr_addc (&ds, '\\'); p += 2; continue;
            case '#': dstr_addc (&ds, '#'); p += 2; continue;
            default: break;
            }
        }
      if (*p == '#')
        {
          if (strncmp (p, "#MAKEFILE#", 10) == 0)
            { dstr_adds (&ds, last_makefile ? last_makefile : ""); p += 10; continue; }
          if (strncmp (p, "#MAKEPATH#", 10) == 0)
            { dstr_adds (&ds, mkpath ? mkpath : ""); p += 10; continue; }
          if (strncmp (p, "#MAKE#", 6) == 0)
            { dstr_adds (&ds, make_name); p += 6; continue; }
          if (strncmp (p, "#PWD#", 5) == 0)
            {
              char cwd[PATH_MAX];
              if (getcwd (cwd, sizeof (cwd)))
                dstr_adds (&ds, cwd);
              else
                dstr_adds (&ds, ".");
              p += 5; continue;
            }
          if (strncmp (p, "#PERL#", 6) == 0)
            { dstr_adds (&ds, helper_tool ? helper_tool : "thelp"); p += 6; continue; }
          if (strncmp (p, "#HELPER#", 8) == 0)
            { dstr_adds (&ds, helper_tool ? helper_tool : "thelp"); p += 8; continue; }
          if (strncmp (p, "#TAB#", 5) == 0)
            { dstr_addc (&ds, '\t'); p += 5; continue; }
          if (strncmp (p, "#SPACE#", 7) == 0)
            { dstr_addc (&ds, ' '); p += 7; continue; }
          if (strncmp (p, "#PATHSEP#", 9) == 0)
            { dstr_adds (&ds, path_sep); p += 9; continue; }
        }
      dstr_addc (&ds, *p);
      p++;
    }
  return ds.data;
}

/* ---- Test case runner ---- */

/* Simple INI-like parser for .test files */

#define MAX_LINE 8192
#define MAX_SECTION_NAME 256
#define MAX_KEY_LEN 256

typedef struct test_case {
  char *makefile;
  char *options;
  char *answer;
  int exit_code;
  char *clean;         /* Files to delete before this case */
  struct test_case *next;
} test_case_t;

typedef struct test_script {
  char *name;
  char *description;
  char *details;
  char *setup_files;   /* Space-separated list of files to touch */
  char *setup_dirs;    /* Space-separated list of dirs to create */
  char *cleanup_files; /* Files to delete between cases */
  test_case_t *cases;
  struct test_script *next;
} test_script_t;

static char *
clean_value (const char *s)
{
  char *r;
  size_t len;

  r = xstrdup (s);
  len = strlen (r);
  /* Strip trailing newlines and spaces */
  while (len > 0 && (r[len-1] == '\n' || r[len-1] == '\r' || r[len-1] == ' '))
    r[--len] = '\0';
  /* Strip surrounding double quotes */
  if (len >= 2 && r[0] == '"' && r[len-1] == '"')
    {
      memmove (r, r+1, len-1);
      r[len-2] = '\0';
    }
  return r;
}

static test_script_t *
parse_test_file (const char *filename)
{
  FILE *f;
  char line[MAX_LINE];
  test_script_t *ts = NULL;
  test_case_t *last_case = NULL;
  dstr current_value;
  char current_key[MAX_KEY_LEN];
  int in_multiline = 0;
  int in_section = 0;
  int lineno = 0;

  current_key[0] = '\0';
  dstr_init (&current_value, 4096);

  f = fopen (filename, "r");
  if (!f) return NULL;

  ts = xmalloc (sizeof (test_script_t));
  memset (ts, 0, sizeof (test_script_t));
  ts->name = xstrdup (path_basename (filename));

  while (fgets (line, sizeof (line), f))
    {
      char *p;
      lineno++;

      /* Strip comments: only if # is first non-whitespace char on line */
      {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#')
          {
            if (!in_multiline)
              continue;  /* Full-line comment, skip */
            /* In multiline, keep the line as-is */
          }
      }

      /* Keep original line for multiline (tabs are significant).
         orig points to the start of the line before trimming. */
      {
        char *orig = xstrdup (line);

        p = trim (line);
        if (*p == '\0')
          {
            if (in_multiline)
              dstr_addc (&current_value, '\n');
            free (orig);
            continue;
          }

        /* Section header: [case] or [setup] or [case name] */
        if (*p == '[')
          {
            char *end = strchr (p, ']');
            if (!end) { free (orig); continue; }
            *end = '\0';

            if (strcmp (p + 1, "setup") == 0)
              {
                /* Setup section: key-value pairs for test setup */
                if (in_multiline)
                  {
                    char *val = clean_value (current_value.data);
                    if (strcmp (current_key, "makefile") == 0 && last_case)
                      last_case->makefile = val;
                    else
                      free (val);
                    current_value.len = 0;
                    current_value.data[0] = '\0';
                    in_multiline = 0;
                  }
                in_section = 1;  /* We're in a setup section now */
                free (orig);
                continue;
              }

            if (in_multiline)
              {
                /* Finish previous key */
                char *val = clean_value (current_value.data);
                if (strcmp (current_key, "description") == 0)
                  ts->description = val;
                else if (strcmp (current_key, "details") == 0)
                  ts->details = val;
                else if (strcmp (current_key, "makefile") == 0 && last_case)
                  last_case->makefile = val;
                else if (strcmp (current_key, "options") == 0 && last_case)
                  last_case->options = val;
                else if (strcmp (current_key, "answer") == 0 && last_case)
                  last_case->answer = val;
                else
                  free (val);
                current_value.len = 0;
                current_value.data[0] = '\0';
                in_multiline = 0;
              }

            {
              test_case_t *tc;
              tc = xmalloc (sizeof (test_case_t));
              memset (tc, 0, sizeof (test_case_t));
              tc->exit_code = 0;

              if (!ts->cases)
                ts->cases = tc;
              else
                last_case->next = tc;
              last_case = tc;
            }

            in_section = 0;
            free (orig);
            continue;
          }

        /* Key-value in setup section */
        if (in_section)
          {
            char *colon = strchr (p, ':');
            if (colon)
              {
                *colon = '\0';
                {
                  const char *kp = trim (p);
                  char *vp = trim (colon + 1);
                  if (strcmp (kp, "touch") == 0)
                    ts->setup_files = clean_value (vp);
                  else if (strcmp (kp, "mkdir") == 0)
                    ts->setup_dirs = clean_value (vp);
                  else if (strcmp (kp, "cleanup") == 0)
                    ts->cleanup_files = clean_value (vp);
                }
                free (orig);
                continue;
              }
          }

        /* Key-value: key: value */
        if (!in_section && !in_multiline)
          {
            char *colon = strchr (p, ':');
            if (colon)
              {
                *colon = '\0';
                strncpy (current_key, trim (p), MAX_KEY_LEN - 1);
                current_key[MAX_KEY_LEN - 1] = '\0';

                p = trim (colon + 1);
                if (*p == '\0')
                  {
                    in_multiline = 1;
                    current_value.len = 0;
                    current_value.data[0] = '\0';
                  }
                else
                  {
                    char *val = clean_value (p);
                    if (strcmp (current_key, "description") == 0)
                      ts->description = val;
                    else if (strcmp (current_key, "details") == 0)
                      ts->details = val;
                    else if (strcmp (current_key, "makefile") == 0 && last_case)
                      last_case->makefile = val;
                    else if (strcmp (current_key, "options") == 0 && last_case)
                      last_case->options = val;
                    else if (strcmp (current_key, "answer") == 0 && last_case)
                      last_case->answer = val;
                    else if (strcmp (current_key, "exit_code") == 0 && last_case)
                      last_case->exit_code = atoi (val);
                    else if (strcmp (current_key, "clean") == 0 && last_case)
                      last_case->clean = val;
                    else
                      free (val);
                  }
                free (orig);
                continue;
              }
          }

        if (in_multiline)
          {
            /* Check if this line starts a new key */
            {
              char *colon = strchr (p, ':');
              if (colon)
                {
                  char save = *colon;
                  *colon = '\0';
                  {
                    const char *kp = trim (p);
                    if (strcmp (kp, "description") == 0
                        || strcmp (kp, "details") == 0
                        || strcmp (kp, "makefile") == 0
                        || strcmp (kp, "options") == 0
                        || strcmp (kp, "answer") == 0
                        || strcmp (kp, "exit_code") == 0)
                      {
                        char *val;

                        /* Finish current multiline */
                        val = clean_value (current_value.data);
                        if (strcmp (current_key, "description") == 0)
                          ts->description = val;
                        else if (strcmp (current_key, "details") == 0)
                          ts->details = val;
                        else if (strcmp (current_key, "makefile") == 0 && last_case)
                          last_case->makefile = val;
                        else if (strcmp (current_key, "options") == 0 && last_case)
                          last_case->options = val;
                        else if (strcmp (current_key, "answer") == 0 && last_case)
                          last_case->answer = val;
                        else
                          free (val);
                        current_value.len = 0;
                        current_value.data[0] = '\0';
                        in_multiline = 0;

                        /* Process this line as a new key.
                           kp points into line buffer — save its length
                           BEFORE restoring the colon. */
                        {
                          size_t kplen = strlen (kp);
                          *colon = save;  /* Restore colon for value parsing */
                          if (kplen >= MAX_KEY_LEN) kplen = MAX_KEY_LEN - 1;
                          memcpy (current_key, kp, kplen);
                          current_key[kplen] = '\0';
                        }
                        {
                          char *vp = trim (colon + 1);
                          if (*vp == '\0')
                            {
                              in_multiline = 1;
                              current_value.len = 0;
                              current_value.data[0] = '\0';
                            }
                          else
                            {
                              char *val2 = clean_value (vp);
                              if (strcmp (current_key, "description") == 0)
                                ts->description = val2;
                              else if (strcmp (current_key, "details") == 0)
                                ts->details = val2;
                              else if (strcmp (current_key, "makefile") == 0 && last_case)
                                last_case->makefile = val2;
                              else if (strcmp (current_key, "options") == 0 && last_case) {
                                last_case->options = val2;
                              }
                              else if (strcmp (current_key, "answer") == 0 && last_case)
                                last_case->answer = val2;
                              else if (strcmp (current_key, "exit_code") == 0 && last_case)
                                last_case->exit_code = atoi (val2);
                              else if (strcmp (current_key, "clean") == 0 && last_case)
                                last_case->clean = val2;
                              else
                                free (val2);
                            }
                        }
                        free (orig);
                        continue;
                      }
                  }
                  *colon = save;
                }
            }
            /* Not a key — append original line (with tabs) to multiline */
            {
              /* Trim trailing newline from orig but keep leading whitespace */
              size_t olen = strlen (orig);
              while (olen > 0 && (orig[olen-1] == '\n' || orig[olen-1] == '\r'))
                olen--;
              dstr_append (&current_value, orig, olen);
              dstr_addc (&current_value, '\n');
            }
          }

        free (orig);
      }
    }

  /* Finish last key if in multiline */
  if (in_multiline && current_key[0])
    {
      char *val = clean_value (current_value.data);
      if (strcmp (current_key, "description") == 0)
        ts->description = val;
      else if (strcmp (current_key, "details") == 0)
        ts->details = val;
      else if (strcmp (current_key, "makefile") == 0 && last_case)
        last_case->makefile = val;
      else if (strcmp (current_key, "options") == 0 && last_case)
        last_case->options = val;
      else if (strcmp (current_key, "answer") == 0 && last_case)
        last_case->answer = val;
      else
        free (val);
    }

  dstr_free (&current_value);
  fclose (f);
  return ts;
}

static void
free_test_script (test_script_t *ts)
{
  test_case_t *tc, *next;
  free (ts->name);
  free (ts->description);
  free (ts->details);
  free (ts->setup_files);
  free (ts->setup_dirs);
  free (ts->cleanup_files);
  for (tc = ts->cases; tc; tc = next)
    {
      next = tc->next;
      free (tc->makefile);
      free (tc->options);
      free (tc->answer);
      free (tc->clean);
      free (tc);
    }
  free (ts);
}

/* ---- Tokenize options string into argv ---- */

static char **
tokenize_options (const char *opts, int *nargs)
{
  char **argv;
  int n, cap;
  const char *p;
  dstr cur;

  argv = xmalloc (32 * sizeof (char *));
  n = 0;
  cap = 32;
  dstr_init (&cur, 256);

  p = opts;
  while (*p)
    {
      /* Skip whitespace */
      while (*p == ' ' || *p == '\t') p++;
      if (!*p) break;

      cur.len = 0;

      if (*p == '"' || *p == '\'')
        {
          char quote = *p++;
          while (*p && *p != quote)
            dstr_addc (&cur, *p++);
          if (*p == quote) p++;
        }
      else
        {
          while (*p && *p != ' ' && *p != '\t')
            dstr_addc (&cur, *p++);
        }

      if (n >= cap)
        {
          cap *= 2;
          argv = xrealloc (argv, cap * sizeof (char *));
        }
      argv[n++] = xstrdup (cur.data);
    }

  argv[n] = NULL;
  *nargs = n;
  dstr_free (&cur);
  return argv;
}

/* ---- Run a single test case ---- */

static int
run_one_test_case (const char *makefile, const char *options_str,
                   const char *answer, int expected_code)
{
  char **make_argv;
  int nargs;
  char *logfile;
  char *subst_makefile = NULL;
  char *subst_answer = NULL;
  int code;

  /* Create the makefile if provided */
  if (makefile && *makefile)
    {
      if (!last_makefile)
        {
          free (last_makefile);
          last_makefile = get_tmpfile (0);
        }
      subst_makefile = subst_tokens (makefile);
      create_file (last_makefile, subst_makefile);
      free (subst_makefile);
    }

  /* Substitute tokens in answer */
  if (answer)
    subst_answer = subst_tokens (answer);
  else
    subst_answer = NULL;

  /* Ensure expected answer ends with newline (match Perl driver behavior) */
  if (subst_answer)
    {
      size_t alen = strlen (subst_answer);
      if (alen == 0 || subst_answer[alen-1] != '\n')
        {
          subst_answer = xrealloc (subst_answer, alen + 2);
          subst_answer[alen] = '\n';
          subst_answer[alen+1] = '\0';
        }
    }

  /* Build make command */
  make_argv = xmalloc (16 * sizeof (char *));
  nargs = 0;
  make_argv[nargs++] = xstrdup (make_path);

  if (last_makefile)
    {
      make_argv[nargs++] = xstrdup ("-f");
      make_argv[nargs++] = xstrdup (last_makefile);
    }

  if (options_str && *options_str)
    {
      char **more;
      int i, nm;
      more = tokenize_options (options_str, &nm);
      make_argv = xrealloc (make_argv, (nargs + nm + 1) * sizeof (char *));
      for (i = 0; i < nm; i++)
        make_argv[nargs++] = more[i];
      free (more);
    }
  make_argv[nargs] = NULL;

  /* Record the command */
  {
    dstr cmd;
    int i;
    dstr_init (&cmd, 256);
    for (i = 0; i < nargs; i++)
      {
        if (i > 0) dstr_addc (&cmd, ' ');
        dstr_adds (&cmd, make_argv[i]);
      }
    free (command_string);
    command_string = cmd.data;
  }

  /* Run make and capture output */
  logfile = get_logfile (0);
  test_passed = 1;

  code = run_command_with_output (logfile, make_argv);

  /* Free argv */
  {
    int i;
    for (i = 0; i < nargs; i++)
      free (make_argv[i]);
    free (make_argv);
  }

  if (code != expected_code)
    {
      fprintf (stderr, "Error running make (expected %d; got %d)\n",
               expected_code, code);
      fprintf (stderr, "Command: %s\n", command_string);
      test_passed = 0;
      {
        char *runfile = get_runfile ();
        create_file (runfile, command_string);
        free (runfile);
      }
      free (logfile);
      free (subst_answer);
      return 0;
    }

  if (!compare_output (subst_answer, logfile))
    {
      char *basefile = get_basefile ();
      char *runfile = get_runfile ();
      char *difffile = get_difffile ();

      create_file (basefile, subst_answer ? subst_answer : "");
      create_file (runfile, command_string);

      if (diff_name)
        {
          char *diff_argv[5];
          diff_argv[0] = xstrdup (diff_name);
          diff_argv[1] = xstrdup ("-c");
          diff_argv[2] = basefile;
          diff_argv[3] = logfile;
          diff_argv[4] = NULL;
          run_command_with_output (difffile, diff_argv);
          free (diff_argv[0]);
          free (diff_argv[1]);
        }
      else
        {
          char msg[1024];
          snprintf (msg, sizeof (msg),
                    "Log file %s differs from base file %s\n",
                    logfile, basefile);
          create_file (difffile, msg);
        }

      test_passed = 0;

      free (basefile);
      free (runfile);
      free (difffile);
    }

  free (logfile);
  free (subst_answer);
  return test_passed;
}

/* ---- Run all tests ---- */

static void
collect_test_files (const char *dir, char ***files, int *n)
{
  DIR *d;
  struct dirent *ent;
  d = opendir (dir);
  if (!d) return;

  while ((ent = readdir (d)) != NULL)
    {
      char *full;
      if (strcmp (ent->d_name, ".") == 0 || strcmp (ent->d_name, "..") == 0)
        continue;

      full = path_join (dir, ent->d_name);

      {
        struct stat st;
        if (stat (full, &st) != 0) { free (full); continue; }

        if (S_ISDIR (st.st_mode))
          {
            collect_test_files (full, files, n);
            free (full);
          }
        else if (strstr (ent->d_name, ".test"))
          {
            *files = xrealloc (*files, (*n + 2) * sizeof (char *));
            (*files)[*n] = full;
            (*files)[*n + 1] = NULL;
            (*n)++;
          }
        else
          free (full);
      }
    }
  closedir (d);
}

static int
cmpstringp (const void *a, const void *b)
{
  return strcmp (*(const char **)a, *(const char **)b);
}

static void
print_banner (void)
{
  printf ("Running tests for GNU Make on %s\n", os_name);
  if (testee_version)
    printf ("%s\n", testee_version);
  printf ("----------------------------------------\n\n");
}

static void
print_usage (const char *prog)
{
  printf ("Usage: %s [options] [testname...]\n", prog);
  printf ("Options:\n");
  printf ("  -make PATH       Path to make binary (default: make)\n");
  printf ("  -debug           Enable debug output\n");
  printf ("  -verbose         Verbose test output\n");
  printf ("  -detail          Detailed test output\n");
  printf ("  -keep            Keep temporary files\n");
  printf ("  -help            Show this help\n");
}

int
main (int argc, char *argv[])
{
  int i;
  int tests_selected = 0;
  char **test_files = NULL;
  int n_test_files = 0;
  char **selected_tests = NULL;
  int n_selected = 0;

  /* Parse arguments */
  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "-make") == 0 && i + 1 < argc)
        {
          make_path = argv[++i];
        }
      else if (strcmp (argv[i], "-debug") == 0)
        {
          debug = 1;
          verbose = 1;
        }
      else if (strcmp (argv[i], "-verbose") == 0)
        verbose = 1;
      else if (strcmp (argv[i], "-detail") == 0)
        { verbose = 1; detail = 1; }
      else if (strcmp (argv[i], "-keep") == 0)
        keep = 1;
      else if (strcmp (argv[i], "-help") == 0 || strcmp (argv[i], "-usage") == 0)
        {
          print_usage (argv[0]);
          return 0;
        }
      else if (strcmp (argv[i], "-all-tests") == 0)
        all_tests = 1;
      else if (argv[i][0] == '-')
        {
          fprintf (stderr, "Unknown option: %s\n", argv[i]);
          print_usage (argv[0]);
          return 1;
        }
      else
        {
          selected_tests = xrealloc (selected_tests,
                                      (n_selected + 2) * sizeof (char *));
          selected_tests[n_selected++] = xstrdup (argv[i]);
          selected_tests[n_selected] = NULL;
          tests_selected = 1;
        }
    }

  /* Initialize */
  detect_os ();
  setup_clean_env ();

  /* Get current directory */
  {
    char buf[PATH_MAX];
    if (getcwd (buf, sizeof (buf)))
      cwd_path = xstrdup (buf);
    else
      cwd_path = xstrdup (".");
  }

  /* Set up paths */
  src_path = xstrdup (cwd_path);
  work_path = path_join (cwd_path, work_dir);
  temp_path = path_join (cwd_path, temp_dir);

  /* Find programs */
  {
    char *diff = find_in_path ("diff");
    if (diff)
      diff_name = diff;
  }

  /* Find thelp helper */
  {
    char *thelp = find_in_path ("thelp");
    if (thelp)
      helper_tool = thelp;
    else
      {
        /* Look in current directory and src path */
        char *candidate = path_join (cwd_path, "thelp");
        struct stat st;
        if (stat (candidate, &st) == 0 && (st.st_mode & S_IXUSR))
          helper_tool = candidate;
        else
          {
            free (candidate);
            candidate = path_join (src_path, "thelp");
            if (stat (candidate, &st) == 0 && (st.st_mode & S_IXUSR))
              helper_tool = candidate;
            else
              {
                free (candidate);
                helper_tool = xstrdup ("thelp");
              }
          }
      }
  }

  /* Determine make version and path */
  {
    char *make_argv[4];
    char *out;
    make_argv[0] = xstrdup (make_path);
    make_argv[1] = xstrdup ("-v");
    make_argv[2] = NULL;
    out = NULL;

    {
      pid_t pid;
      int pipefd[2];
      if (pipe (pipefd) == 0)
        {
          pid = fork ();
          if (pid == 0)
            {
              close (pipefd[0]);
              dup2 (pipefd[1], STDOUT_FILENO);
              close (pipefd[1]);
              execvp (make_argv[0], make_argv);
              _exit (1);
            }
          else if (pid > 0)
            {
              char buf[4096];
              ssize_t n;
              dstr ds;
              close (pipefd[1]);
              dstr_init (&ds, 1024);
              while ((n = read (pipefd[0], buf, sizeof (buf) - 1)) > 0)
                {
                  buf[n] = '\0';
                  dstr_adds (&ds, buf);
                }
              close (pipefd[0]);
              waitpid (pid, NULL, 0);
              out = ds.data;
            }
        }
    }

    if (out && *out)
      {
        /* Extract "GNU Make X.Y" */
        char *p = strstr (out, "GNU Make ");
        if (p)
          {
            char *eol = strchr (p, '\n');
            if (eol) *eol = '\0';
            testee_version = xstrdup (p);
          }
      }
    free (out);
    free (make_argv[0]);
    free (make_argv[1]);
  }

  make_name = path_basename (make_path);
  /* Resolve to absolute path so it works from test subdirectories */
  {
    char resolved[PATH_MAX];
    if (realpath (make_path, resolved))
      make_path = xstrdup (resolved);
  }
  mkpath = xstrdup (make_path);

  /* Print banner */
  print_banner ();

  /* Prepare temp and work directories */
  remove_dir_recursive (temp_path);
  mkdir_p (temp_path);

  remove_dir_recursive (work_path);
  mkdir_p (work_path);

  /* Collect test files */
  {
    char *script_path = path_join (src_path, script_dir);
    collect_test_files (script_path, &test_files, &n_test_files);
    free (script_path);
  }

  if (n_test_files == 0)
    {
      fprintf (stderr, "No .test files found in %s/%s\n", src_path, script_dir);
      return 1;
    }

  /* Sort test files */
  qsort (test_files, n_test_files, sizeof (char *), cmpstringp);

  /* Run tests */
  for (i = 0; i < n_test_files; i++)
    {
      test_script_t *ts;
      test_case_t *tc;
      int cases_run = 0;
      int cases_passed = 0;
      const char *test_name;
      char *rel_name;

      /* Compute relative name */
      {
        size_t prefix_len = strlen (src_path) + strlen (script_dir) + 2;
        const char *fname = test_files[i];
        if (strncmp (fname, src_path, strlen (src_path)) == 0)
          rel_name = xstrdup (fname + strlen (src_path) + 1 + strlen (script_dir) + 1);
        else
          rel_name = xstrdup (fname);

        /* Strip .test extension */
        {
          char *dot = strstr (rel_name, ".test");
          if (dot) *dot = '\0';
        }
      }

      /* Check if this test was selected */
      if (tests_selected)
        {
          int found = 0;
          int j;
          for (j = 0; j < n_selected; j++)
            {
              if (strstr (rel_name, selected_tests[j]))
                { found = 1; break; }
            }
          if (!found) { free (rel_name); continue; }
        }

      printf ("%-50s", rel_name);
      fflush (stdout);

      ts = parse_test_file (test_files[i]);
      if (!ts)
        {
          printf ("FAILED (parse error)\n");
          some_test_failed = 1;
          free (rel_name);
          continue;
        }

      /* Create test working directory */
      {
        char *testdir = path_join (work_path, rel_name);
        remove_dir_recursive (testdir);
        mkdir_p (testdir);
        chdir (testdir);
        free (current_testdir);
        current_testdir = testdir;
      }

      /* Reset per-test state */
      num_logfiles = 0;
      num_tmpfiles = 0;
      free (last_makefile);
      last_makefile = NULL;
      tests_run = 0;
      tests_passed = 0;
      free (command_string);
      command_string = NULL;

      /* Run setup: create files and directories */
      if (ts->setup_files)
        {
          char *s = xstrdup (ts->setup_files);
          char *tok = strtok (s, " ");
          while (tok)
            {
              touch_file (tok);
              tok = strtok (NULL, " ");
            }
          free (s);
        }
      if (ts->setup_dirs)
        {
          char *s = xstrdup (ts->setup_dirs);
          char *tok = strtok (s, " ");
          while (tok)
            {
              mkdir_p (tok);
              tok = strtok (NULL, " ");
            }
          free (s);
        }

      /* Run each test case */
      for (tc = ts->cases; tc; tc = tc->next)
        {
          /* Cleanup files from previous case if requested */
          if (tc != ts->cases && ts->cleanup_files)
            {
              char *s = xstrdup (ts->cleanup_files);
              char *tok = strtok (s, " ");
              while (tok)
                {
                  unlink (tok);
                  tok = strtok (NULL, " ");
                }
              free (s);
            }

          /* Per-case cleanup */
          if (tc->clean)
            {
              char *s = xstrdup (tc->clean);
              char *tok = strtok (s, " ");
              while (tok)
                {
                  unlink (tok);
                  tok = strtok (NULL, " ");
                }
              free (s);
            }

          if (run_one_test_case (tc->makefile, tc->options,
                                  tc->answer, tc->exit_code))
            cases_passed++;
          cases_run++;
        }

      total_tests_run += tests_run;
      total_tests_passed += tests_passed;
      categories_run++;

      if (cases_run == 0)
        {
          printf ("FAILED (no test cases)\n");
          some_test_failed = 1;
        }
      else if (cases_passed == cases_run)
        {
          categories_passed++;
          printf ("ok     (%d passed)\n", cases_passed);

          /* Clean up */
          if (!keep)
            remove_dir_recursive (current_testdir);
        }
      else
        {
          printf ("FAILED (%d/%d passed)\n", cases_passed, cases_run);
          some_test_failed = 1;
        }

      /* Return to base dir */
      chdir (cwd_path);

      free_test_script (ts);
      free (rel_name);
    }

  /* Cleanup */
  remove_dir_recursive (temp_path);
  if (!keep)
    remove_dir_recursive (work_path);

  /* Report */
  {
    int failed = categories_run - categories_passed;
    int tfailed = total_tests_run - total_tests_passed;
    printf ("\n");
    if (some_test_failed || tfailed > 0)
      {
        printf ("%d Tests in %d Categories Failed :-(\n", tfailed, failed);
        return 1;
      }
    else
      {
        printf ("%d Tests in %d Categories Complete ... No Failures :-)\n",
                total_tests_passed, categories_passed);
        return 0;
      }
  }
}
