/* Portable test helper for GNU Make test suite.
   Replaces thelp.pl with a pure C implementation.

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
   This helper supports the following operators:
     out <word>     : echo <word> to stdout with a trailing newline
     raw <word>     : echo <word> to stdout without adding anything
     env <word>     : echo the value of the env.var <word>, or "<unset>"
     file <word>    : echo <word> to stdout AND create the file <word>
     dir <word>     : echo <word> to stdout AND create the directory <word>
     rm <word>      : echo <word> to stdout AND delete the file/directory <word>
     wait <word>    : wait for a file named <word> to exist (timeout from tmout)
     exist <word>   : echo <word> AND fail if a file named <word> doesn't exist
     noexist <word> : echo <word> AND fail if a file named <word> exists
     tmout <secs>   : Change the timeout for waiting.  Default is 10 seconds.
     sleep <secs>   : Sleep for <secs> seconds then echo <secs>
     term <pid>     : send SIGTERM to PID <pid>
     fail <err>     : echo <err> to stdout then exit with error code err

   If given -q as the first argument, only "out", "raw", and "env" generate
   output; all other commands are silent.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>

#ifndef WIN32
# include <sys/wait.h>
#endif

static int quiet = 0;
static int timeout = 10;

static void
die (const char *msg)
{
  fprintf (stderr, "%s\n", msg);
  exit (1);
}

static void
do_out (const char *word)
{
  printf ("%s\n", word);
}

static void
do_raw (const char *word)
{
  printf ("%s", word);
}

static void
do_env (const char *word)
{
  const char *val = getenv (word);
  if (!quiet)
    printf ("%s=", word);
  if (val)
    printf ("%s\n", val);
  else
    printf ("<unset>\n");
}

static void
do_file (const char *word)
{
  FILE *f;
  if (!quiet)
    printf ("file %s\n", word);
  f = fopen (word, "w");
  if (!f)
    {
      fprintf (stderr, "%s: open: %s\n", word, strerror (errno));
      exit (1);
    }
  fclose (f);
}

static void
do_exist (const char *word)
{
  struct stat st;
  if (stat (word, &st) != 0 || !S_ISREG (st.st_mode))
    {
      fprintf (stderr, "%s: file should exist: %s\n", word, strerror (errno));
      exit (1);
    }
  if (!quiet)
    printf ("exist %s\n", word);
}

static void
do_noexist (const char *word)
{
  struct stat st;
  if (stat (word, &st) == 0)
    {
      fprintf (stderr, "%s: file exists\n", word);
      exit (1);
    }
  if (!quiet)
    printf ("noexist %s\n", word);
}

static void
do_dir (const char *word)
{
  if (!quiet)
    printf ("dir %s\n", word);
#ifdef WIN32
  if (mkdir (word) != 0)
#else
  if (mkdir (word, 0777) != 0)
#endif
    {
      fprintf (stderr, "%s: mkdir: %s\n", word, strerror (errno));
      exit (1);
    }
}

static void
do_rm (const char *word)
{
  struct stat st;
  if (stat (word, &st) == 0)
    {
      if (S_ISDIR (st.st_mode))
        {
          if (rmdir (word) != 0)
            {
              fprintf (stderr, "%s: rmdir: %s\n", word, strerror (errno));
              exit (1);
            }
        }
      else
        {
          if (unlink (word) != 0)
            {
              fprintf (stderr, "%s: unlink: %s\n", word, strerror (errno));
              exit (1);
            }
        }
    }
  else
    {
      fprintf (stderr, "%s: not file or directory: %s\n",
               word, strerror (errno));
      exit (1);
    }
  if (!quiet)
    printf ("rm %s\n", word);
}

static void
do_wait (const char *word)
{
  time_t start = time (0);
  time_t end = start + timeout;
  struct stat st;

  while (time (0) <= end)
    {
      if (stat (word, &st) == 0 && S_ISREG (st.st_mode))
        {
          if (!quiet)
            printf ("wait %s\n", word);
          return;
        }
#ifdef WIN32
      Sleep (100);
#else
      usleep (100000);  /* 0.1 seconds */
#endif
    }
  fprintf (stderr, "wait %s: timeout after %ld seconds\n",
           word, (long)(time (0) - start - 1));
  exit (1);
}

static void
do_sleep (const char *word)
{
  int secs = atoi (word);
#ifdef WIN32
  Sleep (secs * 1000);
#else
  sleep (secs);
#endif
  if (!quiet)
    printf ("sleep %s\n", word);
}

static void
do_term (const char *word)
{
  pid_t pid = (pid_t) atoi (word);
  if (!quiet)
    printf ("term %s\n", word);
#ifdef WIN32
  /* Windows doesn't have SIGTERM in the same way */
  fprintf (stderr, "term not supported on Windows\n");
  exit (1);
#else
  if (kill (pid, SIGTERM) != 0)
    {
      fprintf (stderr, "term %s: kill: %s\n", word, strerror (errno));
      exit (1);
    }
#endif
}

static void
do_fail (const char *word)
{
  int code = atoi (word);
  if (!quiet)
    printf ("fail %s\n", word);
  exit (code);
}

static void
do_tmout (const char *word)
{
  timeout = atoi (word);
  if (!quiet)
    printf ("tmout %s\n", word);
}

struct command {
  const char *name;
  void (*func)(const char *word);
};

static struct command commands[] = {
  { "out",     do_out },
  { "raw",     do_raw },
  { "env",     do_env },
  { "file",    do_file },
  { "exist",   do_exist },
  { "noexist", do_noexist },
  { "dir",     do_dir },
  { "rm",      do_rm },
  { "wait",    do_wait },
  { "sleep",   do_sleep },
  { "term",    do_term },
  { "fail",    do_fail },
  { "tmout",   do_tmout },
  { NULL, NULL }
};

int
main (int argc, char *argv[])
{
  int i;

  /* Force unbuffered output */
  setvbuf (stdout, NULL, _IONBF, 0);

  i = 1;
  if (argc > 1 && strcmp (argv[1], "-q") == 0)
    {
      quiet = 1;
      i = 2;
    }

  while (i + 1 < argc)
    {
      const char *op = argv[i];
      const char *arg = argv[i + 1];
      struct command *cmd;
      int found = 0;

      for (cmd = commands; cmd->name; cmd++)
        {
          if (strcmp (cmd->name, op) == 0)
            {
              cmd->func (arg);
              found = 1;
              break;
            }
        }

      if (!found)
        {
          fprintf (stderr, "Invalid command: %s %s\n", op, arg);
          exit (1);
        }

      i += 2;
    }

  return 0;
}
