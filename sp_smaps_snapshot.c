/* This program dumps smaps data for all processes in the system.
 * This file is part of sp-smaps.
 *
 * Copyright (C) 2004-2007,2009,2011 Nokia Corporation.
 *
 * Contact: Eero Tamminen <eero.tamminen@nokia.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

/* ========================================================================= *
 *
 * Author: Simo Piiroinen
 *
 * -------------------------------------------------------------------------
 *
 * History:
 *
 * 25-Feb-2009 Simo Piiroinen
 * - possible NULL dereference fixed
 *
 * 07-Apr-2006 Simo Piiroinen
 * - interleaves data from /proc/pid/status to output
 * - application name taken from /proc/pid/cmdline
 *
 * 12-Sep-2005 Simo Piiroinen
 * - added "see also" section to usage info
 *
 * 09-Sep-2005 Simo Piiroinen
 * - replaces the old shell script
 * ========================================================================= */

/* ========================================================================= *
 * Include files
 * ========================================================================= */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sched.h>
#include <limits.h>

#define MSG_DISABLE_PROGRESS 0

#include <libsysperf/msg.h>
#include <libsysperf/argvec.h>

/* ========================================================================= *
 * Configuration
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * Tool Version
 * ------------------------------------------------------------------------- */

#define TOOL_NAME "sp_smaps_snapshot"
#include "release.h"

/* ------------------------------------------------------------------------- *
 * Runtime Manual
 * ------------------------------------------------------------------------- */

static const manual_t app_man[]=
{
  MAN_ADD("NAME",
          TOOL_NAME"  --  create snapshot of /proc/[1-9]*/smaps files\n"
          )
  MAN_ADD("SYNOPSIS",
          ""TOOL_NAME" [options] \n"
          )
  MAN_ADD("DESCRIPTION",
          "This tool generates snapshot of /proc/pid/smaps information\n"
          "for all processes in the system.\n"
          "\n"
          "The smaps data from all processes in concatenated into the\n"
          "output. Additionally, some data from /proc/pid/status is\n"
          "also included for each process to help postprocessing tools\n"
          "such as sp_smaps_analyze to perform additional tasks like\n"
          "recognizing multiple threads belonging to one application\n"
          "in order to provide more accurate memory usage statistics.\n"
          "\n"
          "When run as root, it's possible to request this tool to\n"
          "boost its priority to not-nice-at-all realtime scheduling which\n"
          "hopefully yields more stable results for threads of MT apps.\n"
          "However, this can be cause instability at least when the command\n"
          "is run from a serial console, so this behavior is optional\n"
          "rather than the default.\n"
          )
  MAN_ADD("OPTIONS", 0)

  MAN_ADD("EXAMPLES",
          "% "TOOL_NAME" > after_boot.cap\n"
          "\n"
          "  Collects /proc/*/smaps files from all running processes, and writes the\n"
          "  result to 'after_boot.cap'.\n"
          )
  MAN_ADD("COPYRIGHT",
          "Copyright (C) 2004-2007,2009,2011 Nokia Corporation.\n\n"
          "This is free software.  You may redistribute copies of it under the\n"
          "terms of the GNU General Public License v2 included with the software.\n"
          "There is NO WARRANTY, to the extent permitted by law.\n"
          )
  MAN_ADD("SEE ALSO",
          "sp_smaps_filter (1)\n"
          "\n"
          )
  MAN_END
};

/* ------------------------------------------------------------------------- *
 * Commandline Arguments
 * ------------------------------------------------------------------------- */

enum
{
  opt_noswitch = -1,
  opt_help,
  opt_vers,

  opt_verbose,
  opt_quiet,
  opt_silent,

  opt_output,
  opt_realtime,
};

static const option_t app_opt[] =
{
  /* - - - - - - - - - - - - - - - - - - - *
   * usage, version & verbosity
   * - - - - - - - - - - - - - - - - - - - */

  OPT_ADD(opt_help,
          "h", "help", 0,
          "This help text\n"),

  OPT_ADD(opt_vers,
          "V", "version", 0,
          "Tool version\n"),

  OPT_ADD(opt_verbose,
          "v", "verbose", 0,
          "Enable diagnostic messages\n"),

  OPT_ADD(opt_quiet,
          "q", "quiet", 0,
          "Disable warning messages\n"),

  OPT_ADD(opt_silent,
          "s", "silent", 0,
          "Disable all messages\n"),

  /* - - - - - - - - - - - - - - - - - - - *
   * application options
   * - - - - - - - - - - - - - - - - - - - */

  OPT_ADD(opt_output,
          "o", "output", "<destination path>",
          "Output file to use instead of stdout.\n" ),

  OPT_ADD(opt_realtime,
          "r", "realtime", 0,
          "Use realtime priority (needs to be run as root for this)" ),

  OPT_END
};

/* ------------------------------------------------------------------------- *
 * IO buffer sizes
 * ------------------------------------------------------------------------- */

#define RXBUFF ( 8<<10) /* Should be large enough to allow reading typical
                         * /proc/pid/file in one go. Read buffers are taken
                         * from stack so avoid excessive sizes... */

#define TXBUFF (64<<10) /* All output - except for the final write - will be
                         * done in this sized blocks -> make it multiple of
                         * file system block size. */

static size_t page_size = 0;
static const char *outfile = 0;

/* ========================================================================= *
 * Utility functions
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * use_maximum_priority  --  to get stable results for MT apps
 * ------------------------------------------------------------------------- */

static void use_maximum_priority(void)
{
  /* - - - - - - - - - - - - - - - - - - - *
   * Purpose of this priority fiddling...
   *
   * By making it higly unlikely that other
   * processes get execution time while we
   * are generating the snapshot we make it
   * more likely that values recorded for
   * threads of MT apps contain the same
   * values -> allows us to recognize them
   * as threads in post processing.
   * - - - - - - - - - - - - - - - - - - - */

  /* - - - - - - - - - - - - - - - - - - - *
   * Nice -> as bad as possible
   * - - - - - - - - - - - - - - - - - - - */

  {
    int old = getpriority(PRIO_PROCESS, 0);
    if( setpriority(PRIO_PROCESS, 0, -20) == -1 )
    {
      msg_error("unable to setpriority\n");
    }
    int new = getpriority(PRIO_PROCESS, 0);
    msg_progress("nice  priority: %d -> %d\n", old, new);
  }

  /* - - - - - - - - - - - - - - - - - - - *
   * Scheduling -> RT FIFO policy
   * - - - - - - - - - - - - - - - - - - - */

  {
    struct sched_param old,new;

    if( sched_getparam(0, &old) == -1 )
    {
      msg_error("unable to get currect scheduling\n");
    }
    new = old;
    new.sched_priority = sched_get_priority_max(SCHED_FIFO);

    if( sched_setscheduler(0, SCHED_FIFO, &new) == -1 )
    {
      msg_error("unable to set scheduling\n");
    }
    else
    {
      msg_progress("sched policy  : SHED_FIFO\n");
    }
    if( sched_getparam(0, &new) == -1 )
    {
      msg_error("unable to get changed scheduling\n");
    }

    msg_progress("sched priority: %d -> %d\n",
                 old.sched_priority,
                 new.sched_priority);
  }
}

/* ------------------------------------------------------------------------- *
 * write_all_or_exit  --  write all or fail & exit
 * ------------------------------------------------------------------------- */

static void write_all_or_exit(int fd, const void *data, size_t size)
{
  const char *pos = data;
  const char *end = pos + size;

  while( pos < end )
  {
    int put = write(fd, pos, end-pos);

    if( put == -1 )
    {
      switch( errno )
      {
      case EAGAIN:
      case EINTR:
        continue;

      default:
        msg_fatal("write error: %s\n", strerror(errno));
      }
    }
    pos += put;
  }
}

/* ========================================================================= *
 * Buffered Output
 * ========================================================================= */

/* ------------------------------------------------------------------------- *
 * output_buff  --  writes to stdout done via this
 * ------------------------------------------------------------------------- */

static int    output_fd = -1;
static char   output_buff[TXBUFF];
static size_t output_offs = 0;

/* ------------------------------------------------------------------------- *
 * output_space  --  return space available in output buffer
 * ------------------------------------------------------------------------- */

static size_t output_space(int force_flush)
{
  if( output_offs != 0 )
  {
    if( output_offs == sizeof output_buff || force_flush )
    {
      if( output_fd == -1 )
      {
        output_fd = STDOUT_FILENO;

        if( outfile != 0 )
        {
          int fd = open(outfile, O_WRONLY|O_CREAT|O_TRUNC, 0666);
          if( fd == -1 )
          {
            msg_error("%s: %s\n(using stdout)", outfile, strerror(errno));
          }
          else
          {
            output_fd = fd;
          }
        }
      }

      write_all_or_exit(output_fd, output_buff, output_offs);
      output_offs = 0;
    }
  }
  return sizeof output_buff - output_offs;
}

/* ------------------------------------------------------------------------- *
 * output_raw  --  queue output
 * ------------------------------------------------------------------------- */

static void output_raw(const void *data, size_t size)
{
  const char *pos = data;
  const char *end = pos + size;

  while( pos < end )
  {
    size_t count = end - pos;
    size_t space = output_space(0);

    if( count > space ) count = space;

    memcpy(output_buff + output_offs, pos, count);

    output_offs += count;
    pos += count;
  }
}

/* ------------------------------------------------------------------------- *
 * output_fmt  --  queue formatted output
 * ------------------------------------------------------------------------- */

static void output_fmt(const char *fmt, ...)
{
  char temp[1<<10];
  char *work = temp;

  va_list va;
  size_t  n;

  va_start(va, fmt);
  n = vsnprintf(work, sizeof temp, fmt, va);
  va_end(va);

  if( n > sizeof temp )
  {
    work = alloca(n);
    va_start(va, fmt);
    vsnprintf(work, n, fmt, va);
    va_end(va);
  }

  output_raw(work, n);
}

/* ------------------------------------------------------------------------- *
 * output_file  --  queue file contents to output
 * ------------------------------------------------------------------------- */

static ssize_t output_file(const char *path)
{
  ssize_t cnt = 0;
  char temp[RXBUFF];
  int file = open(path,O_RDONLY);

  if( file == -1 )
  {
    msg_error("%s: %s\n", path, strerror(errno));
    cnt = -1;
    goto cleanup;
  }

  for( ;; )
  {
    int rc = read(file, temp, sizeof temp);

    if( rc == 0 )
    {
      break;
    }

    if( rc == -1 )
    {
      switch( errno )
      {
      case EAGAIN:
      case EINTR:
        continue;

      default:
        perror(path);
        cnt = -1;
        goto cleanup;
      }
    }

    output_raw(temp, rc);
    cnt += rc;
  }

  cleanup:

  if( file != -1 ) close(file);
  return cnt;
}

/* ------------------------------------------------------------------------- *
 * input_file  --  read file contents, terminate with '\0'
 * ------------------------------------------------------------------------- */

static size_t input_file(const char *path, void *pdata, size_t *psize)
{

  size_t  done = 0;
  char   *data = *(char **)pdata;
  size_t  size = *psize;
  int     file = -1;

  if( (file = open(path,O_RDONLY)) == -1 )
  {
    msg_error("%s: %s\n", path, strerror(errno));
    goto cleanup;
  }

  for( ;; )
  {
    if( size - done < page_size )
    {
      if( (data = realloc(data, (size += page_size))) == 0 )
      {
        msg_fatal("%s: %s\n", path, strerror(errno));
      }
    }

    ssize_t rc = read(file, data + done, size - done);

    if( rc == -1 )
    {
      switch( errno )
      {
      case EAGAIN:
      case EINTR:
        continue;

      default:
        msg_error("%s: %s\n", path, strerror(errno));
        goto cleanup;
      }
    }

    if( rc == 0 )
    {
      break;
    }

    done += (size_t)rc;
  }

  cleanup:

  if( file != -1 )
  {
    close(file);
  }

  if( done == size )
  {
    if( (data = realloc(data, (size += 1))) == 0 )
    {
      msg_fatal("%s: %s\n", path, strerror(errno));
    }
  }
  data[done] = 0;

  *(char **)pdata = data;
  *psize = size;

  return done;
}

#define uc(c) ((unsigned char)(c))
#define wc(c) ((c)>0 && (c)<33)
#define bc(c) (uc(c)>32)

static char *strip(char *str)
{
  char *src = str;
  char *dst = str;

  while( wc(*src) ) ++src;

  for( ;; )
  {
    if( bc(*src) )
    {
      *dst++ = *src++;
      continue;
    }

    while( wc(*src) ) ++src;

    if( *src == 0 ) break;

    *dst++ = ' ';
  }
  *dst = 0;
  return str;
}

static char *token(char **ppos, int sep)
{
  char *beg = *ppos;

  if( sep < 0 )
  {
    while( wc(*beg) ) ++beg;
  }

  char *end = beg;

  for( ; *end; ++end )
  {
    //printf("??? '%c' vs '%c'\n", (sep<0) ? '?' : sep, *end);
    if( ((sep<0) && wc(*end)) || (uc(*end) == uc(sep)) )
    {
      *end++ = 0; break;
    }
  }
  *ppos = end;
  return strip(beg);
}

typedef struct proc_pid_status_t {
  char *Name;
  char *Pid;
  char *PPid;
  char *Threads;
  char *FDSize;
  char *VmPeak;
  char *VmSize;
  char *VmLck;
  char *VmHWM;
  char *VmRSS;
  char *VmData;
  char *VmStk;
  char *VmExe;
  char *VmLib;
  char *VmPTE;

} proc_pid_status_t;

static void
proc_pid_status_parse(proc_pid_status_t *self, char *data)
{
  static char empty[1] = "";

  memset(self, 0, sizeof *self);

  self->Name    = empty;
  self->Pid     = "0";
  self->PPid    = "0";
  self->Threads = "0";

  while( *data )
  {
    char *row = token(&data, '\n');
    char *key = token(&row, ':');

    if( !strcmp(key, "Name") )
    {
      self->Name = strip(row);
    }
#define X(v) else if( !strcmp(key, #v) ) { self->v = token(&row, -1); }
    X(Pid)
    X(PPid)
    X(Threads)
    X(FDSize)
    X(VmPeak)
    X(VmSize)
    X(VmLck)
    X(VmHWM)
    X(VmRSS)
    X(VmData)
    X(VmStk)
    X(VmExe)
    X(VmLib)
    X(VmPTE)
#undef X
  }
}

/* ========================================================================= *
 * Snapshot from /proc/pid/smaps information
 * ========================================================================= */

static char *kthreadd_pid;

static int is_kthreadd(const proc_pid_status_t *status)
{
  if (status->Name == NULL)
    return 0;
  if (status->PPid == NULL)
    return 0;
  return strcmp(status->Name, "kthreadd") == 0
         && strcmp(status->PPid, "0") == 0;
}

static int is_kernel_thread(const proc_pid_status_t *status)
{
  if (kthreadd_pid == NULL)
    return 0;
  if (status->PPid == NULL)
    return 0;
  return strcmp(status->PPid, kthreadd_pid) == 0;
}

static void check_kthreadd(const proc_pid_status_t *status)
{
  if (kthreadd_pid)
    return;
  if (status->Pid == NULL)
    return;
  if (is_kthreadd(status))
    kthreadd_pid = strdup(status->Pid);
}

/* ------------------------------------------------------------------------- *
 * snapshot_all  -- retrieve snapshot of information for one process
 * ------------------------------------------------------------------------- */

static int snapshot_all(void)
{
  char   *status_text = 0;
  size_t  status_size = 0;
  char   *cmdline_text = 0;
  size_t  cmdline_size = 0;

  static const char root[] = "/proc";

  int  err = -1;
  DIR *dir = 0;
  int  cnt = 0;

  struct dirent *de;

  if( (dir = opendir(root)) == 0 )
  {
    perror(root);
    goto cleanup;
  }

  while( (de = readdir(dir)) != 0 )
  {
    if( '1' <= de->d_name[0] && de->d_name[0] <= '9' )
    {
      char exe[256];
      char path[PATH_MAX];
      proc_pid_status_t status;
      size_t smaps_bytes;
      char *name = NULL;

      /* - - - - - - - - - - - - - - - - - - - *
       * /proc/pid/exe -> link to executable
       * - - - - - - - - - - - - - - - - - - - */

      snprintf(path, sizeof path, "%s/%s/%s", root, de->d_name,"exe");
      int n = readlink(path, exe, sizeof exe - 1);
      exe[n>0?n:0] = 0;

      /* - - - - - - - - - - - - - - - - - - - *
       * /proc/pid/cmdline -> argv[] data
       * - - - - - - - - - - - - - - - - - - - */

      snprintf(path, sizeof path, "%s/%s/%s", root, de->d_name,"cmdline");
      input_file(path, &cmdline_text, &cmdline_size);

      /* - - - - - - - - - - - - - - - - - - - *
       * /proc/pid/status -> name, pid, ...
       * - - - - - - - - - - - - - - - - - - - */

      snprintf(path, sizeof path, "%s/%s/%s", root, de->d_name,"status");
      input_file(path, &status_text, &status_size);
      proc_pid_status_parse(&status, status_text);

      check_kthreadd(&status);

      if( cnt++ != 0 )
      {
        output_raw("\n",1);
      }

      snprintf(path, sizeof path, "%s/%s/smaps", root, de->d_name);
      output_fmt("==> %s <==\n", path);

      name = strip(cmdline_text);

      if( name == NULL || *name == 0 )
      {
        name = strip(exe);
      }
      if( name == NULL || *name == 0 )
      {
        name = strip(status.Name);
      }
      if( name == NULL || *name == 0 )
      {
        name = "unknown";
      }

      output_fmt("#Name: %s\n", name);

#define X(v) if( status.v ) output_fmt("#%s: %s\n",#v,status.v);
      X(Pid)
      X(PPid)
      X(Threads)
      X(FDSize)
      X(VmPeak)
      X(VmSize)
      X(VmLck)
      X(VmHWM)
      X(VmRSS)
      X(VmData)
      X(VmStk)
      X(VmExe)
      X(VmLib)
      X(VmPTE)
#undef X

      smaps_bytes = output_file(path);
      if( smaps_bytes < 0 )
      {
        msg_warning("%s: read/write error\n", path);
        continue;
      }

      if (smaps_bytes == 0
          && !is_kthreadd(&status)
          && !is_kernel_thread(&status))
      {
        msg_warning("`%s' is empty for process named '%s'!\n", path, name);
      }
    }
  }

  err = 0;

  cleanup:

  if( dir != 0 ) closedir(dir);

  output_space(1);

  free(cmdline_text);
  free(status_text);

  return err;
}

/* ========================================================================= *
 * Main Entry Point
 * ========================================================================= */

int main(int ac, char **av)
{
  argvec_t *args = argvec_create(ac, av, app_opt, app_man);

  while( !argvec_done(args) )
  {
    int       tag  = 0;
    char     *par  = 0;

    if( !argvec_next(args, &tag, &par) )
    {
      msg_error("(use --help for usage)\n");
      exit(1);
    }

    switch( tag )
    {
    case opt_help:
      argvec_usage(args);
      exit(EXIT_SUCCESS);

    case opt_vers:
      printf("%s\n", TOOL_VERS);
      exit(EXIT_SUCCESS);

    case opt_verbose:
      msg_incverbosity();
      break;
    case opt_quiet:
      msg_decverbosity();
      break;
    case opt_silent:
      msg_setsilent();
      break;

    case opt_output:
      outfile = par;
      break;
    case opt_realtime:
      if( geteuid() == 0 )
      {
        msg_progress("Attempting to adjust priority/scheduling.\n");
        use_maximum_priority();
      }
      else
      {
        msg_warning("Realtime mode is only available when run as root.\n");
        exit(1);
      }
      break;
    }
  }

  argvec_delete(args);

  page_size = sysconf(_SC_PAGESIZE);
  if( page_size < 0 )
  {
    msg_fatal("sysconf: %s\n", strerror(errno));
  }

  return snapshot_all() ? EXIT_FAILURE : EXIT_SUCCESS;
}
