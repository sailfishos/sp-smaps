/* This program dumps smaps data for all processes in the system.
 * This file is part of sp-smaps.
 *
 * Copyright (C) 2004-2007,2009,2011 Nokia Corporation.
 * Copyright (C) 2026 Jolla Mobile Ltd
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
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sched.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/param.h>

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

#define COMPACT_FORMAT_VERSION "1"

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
          "Copyright (C) 2004-2007,2009,2011 Nokia Corporation.\n"
          "Copyright (C) 2026 Jolla Mobile Ltd\n\n"
          "This is free software.  You may redistribute copies of it under the\n"
          "terms of the GNU General Public License v2 included with the software.\n"
          "There is NO WARRANTY, to the extent permitted by law.\n"
          )
  MAN_ADD("SEE ALSO",
          "sp_smaps_filter (1), sp_smaps_expand (1)\n"
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
  opt_compact,
  opt_realtime,
  opt_root,
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

  OPT_ADD(opt_compact,
          "c", "compact", 0,
          "Produce output in the compact format. Use the `sp_smaps_expand'\n"
          "tool to convert the output from the compact format prior to\n"
          "processing with other tools.\n" ),

  OPT_ADD(opt_realtime,
          "r", "realtime", 0,
          "Use realtime priority (needs to be run as root for this)" ),

  OPT_ADD(opt_root,
          0, "root", "<procfs path>",
          "Use nonstandard procfs location. For testing.\n" ),

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
static uintptr_t page_size_mask = 0;
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
 * output_line  --  queue one line for output
 * ------------------------------------------------------------------------- */

static const char *output_line(const char *data, const char *end)
{
  const char *line_end = memchr(data, '\n', end - data);

  if( !line_end )
  {
    line_end = end - 1;
  }

  output_raw(data, line_end - data + 1);

  return line_end + 1;
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
 * input_file_head  --  read portion of file contents, terminate with '\0'
 * ------------------------------------------------------------------------- */

static size_t input_file_head(const char *path, void *pdata, size_t *psize, size_t limit)
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
    if( size == 0 )
    {
      if( (data = malloc((size = MIN(page_size, limit)))) == 0 )
      {
        msg_fatal("%s: %s\n", path, strerror(errno));
      }
    }
    else if( (size < limit) && (size - done < page_size) )
    {
      if( (data = realloc(data, (size = MIN(size * 2, limit)))) == 0 )
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

/* ------------------------------------------------------------------------- *
 * input_file  --  read file contents, terminate with '\0'
 * ------------------------------------------------------------------------- */

static size_t input_file(const char *path, void *pdata, size_t *psize)
{
  return input_file_head(path, pdata, psize, SIZE_MAX);
}

/* ========================================================================= *
 * Sequential Input
 * ========================================================================= */

typedef struct scan_file_t
{
  const char *path;     /* File path */
  const char *data;     /* The data read from the file, available in a sliding
                           window. As the scan_file_advance() function is
                           called, more data become available and previously
                           consumed data get invalidated. */
  const char *end;      /* Points one element after the data read so far, moving
                           with scan_file_advance(). */

  /* private */
  int _fd;              /* File descriptor */
  size_t _max_size;     /* Nuber of bytes reserved for the data */
  int _eof;             /* EOF reached, 'end' is at its final position. */
  uintptr_t _window;    /* Start of the window. */
} scan_file_t;

/* ------------------------------------------------------------------------- *
 * scan_file_create
 * ------------------------------------------------------------------------- */

static scan_file_t *scan_file_create(const char *path, size_t max_size)
{
  scan_file_t *self = calloc(sizeof(scan_file_t), 1);
  self->path = path;
  self->_fd = -1;

  void *addr = NULL;

  if( (self->_fd = open(path, O_RDONLY)) == -1 )
  {
    msg_error("%s: %s\n", path, strerror(errno));
    goto fail;
  }

  addr = mmap(NULL, max_size, PROT_NONE,
              MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
              -1, 0);
  if( !addr )
  {
    msg_error("%s: %s\n", path, strerror(errno));
    goto fail;
  }

  self->data = addr;
  self->end = addr;
  self->_window = (uintptr_t)addr;
  self->_max_size = max_size;

  return self;

  fail:

  if( self->_fd != -1 ) close(self->_fd);
  free(self);
  return NULL;
}

/* ------------------------------------------------------------------------- *
 * scan_file_advance -- fetch more data from the file on demand
 *
 * Ensure that at least window_size bytes are available for processing unless
 * EOF is reached. Reading happens in blocks of window_size.
 *
 * Indicate the progress of processing the data with 'pos'. Data before this
 * point may become unavailable after this call. Do not try to read them again.
 *
 * On success, the number of bytes available for reading after this call is
 * returned, computed as (self->end - pos). On error, -1 is returned.
 * ------------------------------------------------------------------------- */

static ssize_t scan_file_advance(scan_file_t *self,
                                 const char *pos,
                                 size_t window_size)
{
  if( !self->_eof && ((self->end - pos) < window_size) )
  {
    const uintptr_t end_aligned = (page_size_mask & (uintptr_t)self->end);
    const size_t to_unlock = window_size + (size_t)(self->end - end_aligned);

    if( mprotect((char *)end_aligned, to_unlock, PROT_READ | PROT_WRITE) != 0 )
    {
      msg_error("%s: mprotect: %s\n", self->path, strerror(errno));
      return -1;
    }

    size_t to_read = window_size;
    while( to_read > 0 )
    {
      ssize_t rc = read(self->_fd, (char *)self->end, to_read);

      if( rc == 0 )
      {
        self->_eof = 1;
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
          perror("read");
          return -1;
        }
      }

      to_read -= (size_t)rc;
      self->end += (size_t)rc;
    }
  }

  const uintptr_t pos_aligned = (page_size_mask & (uintptr_t)pos);

  if( self->_window < pos_aligned )
  {
    int rc = madvise((void*)self->_window,
                     pos_aligned - self->_window,
                     MADV_DONTNEED);

    if( rc != 0 )
    {
      msg_warning("%s: madvise(MADV_DONTNEED): %s\n",
                  self->path, strerror(errno));
    }

    self->_window = pos_aligned;
  }

  return self->end - pos;
}

/* ------------------------------------------------------------------------- *
 * scan_file_delete
 * ------------------------------------------------------------------------- */

static void scan_file_delete(scan_file_t *self)
{
  if( !self )
  {
    return;
  }

  if( munmap((char *)self->data, self->_max_size) != 0 )
  {
    msg_warning("%s: munmap: %s\n", self->path, strerror(errno));
  }

  self->data = NULL;

  if( close(self->_fd) != 0 )
  {
    msg_warning("%s: close: %s\n", self->path, strerror(errno));
  }

  self->_fd = -1;

  free(self);
}

/* ========================================================================= *
 * text parsing utilities
 * ========================================================================= */

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
 * smaps fields
 * ------------------------------------------------------------------------- */

typedef struct field_t {
    char *name;
    int name_length;
    char *unit;
    int unit_length;
} field_t;

typedef struct fields_t
{
    field_t *fields;
    int count;
} fields_t;

const char compact_process_fields_leader[] = "@";
const char compact_segment_fields_leader[] = "!";

const char compact_dir_name_key[] = "~DirName";
const char compact_cmd_name_key[] = "~CmdName";

const char compact_field_sep = '\x1F'; /* ASCII unit separator */
const int compact_fields_max_size = 1 << 10;

/*
 * No smaps file can be greater than 100 MB...?
 */
static const size_t smaps_max_size = 100 << 20;

/*
 * Reading more than 3 kB from a smaps file (procfs in general) usually leads to
 * partial reads. So an attempt to use e.g. window_size equal to page_size,
 * assuming page_size of 4 kB, would lead to 2 reads in each scan_file_advance
 * call.
 */
static const size_t smaps_window_size = 3 << 10;

/* ------------------------------------------------------------------------- *
 * fields_delete
 * ------------------------------------------------------------------------- */

void fields_delete(fields_t *fields)
{
  if( !fields )
  {
    return;
  }

  for( int i = 0; i < fields->count; ++i )
  {
    free(fields->fields[i].name);
    free(fields->fields[i].unit);
  }

  free(fields->fields);
  free(fields);
}

/* ------------------------------------------------------------------------- *
 * parse_smaps_segment_fields - determine the fields from a sample file
 * ------------------------------------------------------------------------- */

fields_t *parse_smaps_segment_fields(const char *path)
{
  fields_t *self = calloc(sizeof(fields_t), 1);
  char     *sample_text = 0;
  size_t    sample_size = 0;
  size_t    sample_limit = 4 << 10;

  input_file_head(path, &sample_text, &sample_size, sample_limit);

  char *sample = sample_text;

  /* Skip first range header */
  token(&sample, '\n');

  while( *sample )
  {
    char *row = token(&sample, '\n');

    /* Stop on next range header */
    if( (*row >= '0' && *row <= '9') || (*row >= 'a' && *row <= 'f') )
    {
      break;
    }

    self->count++;
    self->fields = reallocarray(self->fields, self->count, sizeof(field_t));

    field_t *field = self->fields + self->count - 1;

    char *key = token(&row, ':');
    char *value = strip(row);
    size_t value_length = strlen(value);

    field->name = strdup(key);
    field->name_length = strlen(key);

    /* Assume no other than kB unit is possible */
    if( value_length >= 4
        && *value >= '0' && *value <= '9'
        && *(value + value_length - 3) == ' '
        && *(value + value_length - 2) == 'k'
        && *(value + value_length - 1) == 'B' )
    {
      field->unit = strdup("kB");
      field->unit_length = 2;
    }
    else
    {
      field->unit = strdup("");
      field->unit_length = 0;
    }
  }

  free(sample_text);

  return self;
}

/* ------------------------------------------------------------------------- *
 * output_compact_header -- output file header
 * ------------------------------------------------------------------------- */

static void output_compact_header(const fields_t *segment_fields)
{
  output_fmt("#smaps.ccap:%s\n", COMPACT_FORMAT_VERSION);

  output_fmt("#%s", compact_process_fields_leader);
  output_fmt("%c%s", compact_field_sep, compact_dir_name_key);
  output_fmt("%c%s", compact_field_sep, compact_cmd_name_key);

#define X(v) output_fmt("%c%s", compact_field_sep, #v);
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

  output_fmt("\n");

  output_fmt("#%s", compact_segment_fields_leader);

  for( int i = 0; i < segment_fields->count; ++i )
  {
    const field_t *field = &segment_fields->fields[i];
    output_fmt("%c%s:%s", compact_field_sep, field->name, field->unit);
  }

  output_fmt("\n");
}

/* ------------------------------------------------------------------------- *
 * output_compact_fields -- output one block of fields in the compact format
 *
 * Returns position to continue from.
 * ------------------------------------------------------------------------- */

static const char *output_compact_fields(scan_file_t *file,
                                         const char *start_pos,
                                         const char *leader,
                                         const fields_t *fields)
{
    const char *pos = start_pos;
    const char *last_line_start = pos;

    char buf[compact_fields_max_size];
    char *buf_pos = buf;
    char *const buf_end = buf + compact_fields_max_size;

    buf_pos = stpncpy(buf_pos, leader, buf_end - buf_pos);

    for( int i = 0; i < fields->count; ++i )
    {
      const field_t *const field = &fields->fields[i];

      /*
       * If we fail, we will dump the data as-is up to this point, return,
       * and processing will continue from this point.
       */
      last_line_start = pos;

      /*
       * Consume the expected field name
       */

      if( strncmp(pos, field->name, MIN(field->name_length, file->end - pos)) != 0 )
      {
        char snip[64];
        snprintf(snip, MIN(sizeof snip, file->end - pos), "%s", pos);
        msg_warning("%s: Expected field '%s', got '%s...'\n",
                    file->path, field->name, snip);
        goto fail;
      }

      pos += field->name_length;

      /*
       * Consume the expected colon character
       */

      if( pos >= file->end )
      {
        msg_warning("%s: Expected ':', encountered EOF\n", file->path);
        goto fail;
      }

      if( *pos != ':' )
      {
        char snip[64];
        snprintf(snip, MIN(sizeof snip, file->end - pos), "%s", pos);
        msg_warning("%s: Expected ':', got '%s...'\n", file->path, snip);
        goto fail;
      }

      pos += 1;

      /*
       * Consume all spaces - seek to the start of the value string
       */

      while( pos < file->end && *pos == ' ' )
        ++pos;

      const char *value_start = pos;
      const char *value_end = pos;

      /*
       * Seek to the end of line, check that the expected unit is there and
       * reverse-find the end of the value string.
       */

      while( pos < file->end && *pos != '\n' )
        ++pos;

      if( field->unit_length != 0 )
      {
        if( strncmp(pos - field->unit_length, field->unit, field->unit_length) != 0 )
        {
          msg_warning("%s: Expected unit string '%s' for field '%s'\n",
                      file->path, field->unit, field->name);
          goto fail;
        }
        value_end = pos - field->unit_length - 1;
      }
      else
      {
        value_end = pos - 1;
      }

      while( *value_end == ' ' )
        --value_end;
      ++value_end; /* point one behind */

      /*
       * Append the field.
       */

      if( buf_pos + (value_end - value_start) + 1 >= buf_end )
      {
        msg_warning("%s: Buffer too small\n", file->path);
        goto fail;
      }

      *buf_pos++ = compact_field_sep;
      buf_pos = stpncpy(buf_pos, value_start, value_end - value_start);

      /*
       * Consume the line break
       */

      if( pos < file->end && *pos == '\n' )
        ++pos;
    }

    if( buf_pos >= buf_end )
    {
      msg_warning("%s: No space for newline in buffer\n", file->path);
      goto fail;
    }

    *buf_pos++ = '\n';

    output_raw(buf, buf_pos - buf);
    return pos;

fail:
    output_raw(start_pos, last_line_start - start_pos);
    return last_line_start;
}

/* ------------------------------------------------------------------------- *
 * output_compact_smaps -- output smaps file content in the compact format
 *
 * On success, returns the number of bytes read. On I/O error, returns -1. Does
 * not indicate parsing errors. Content is passed through as is in case of
 * parsing errors.
 * ------------------------------------------------------------------------- */

static ssize_t output_compact_smaps(scan_file_t *file,
                                    const fields_t *segment_fields)
{
  const char *pos = file->data;
  const char *last_line_start = pos;

  while( pos < file->end )
  {
    if( scan_file_advance(file, pos, smaps_window_size) < 0 )
    {
      return -1;
    };

    last_line_start = pos;

    if( (*pos >= 'a' && *pos <= 'f') || (*pos >= '0' && *pos <= '9') )
    {
      const char *line_end = memchr(pos, '\n', file->end - pos);

      if( !line_end )
      {
        msg_warning("%s: unterminated line\n", file->path);
        output_raw(pos, file->end - pos);
        pos = file->end;
        break;
      }

      output_raw(pos, line_end - pos + 1);

      pos = line_end + 1;
      pos = output_compact_fields(file, pos, compact_segment_fields_leader,
                                  segment_fields);
    }
    else
    {
      pos = output_line(last_line_start, file->end);
    }
  }

  return pos - file->data;
}

/* ------------------------------------------------------------------------- *
 * output_compact_smaps_file -- output one smaps file in the compact format
 *
 * On success, returns the number of bytes read. On I/O error, returns -1. Does
 * not indicate parsing errors. Content is passed through as is in case of
 * parsing errors.
 * ------------------------------------------------------------------------- */

static ssize_t output_compact_smaps_file(const char *path,
                                         const fields_t *segment_fields)
{
  scan_file_t *file = NULL;
  ssize_t rc = -1;

  file = scan_file_create(path, smaps_max_size);
  if( !file )
    goto cleanup;

  rc = scan_file_advance(file, file->data, smaps_window_size);
  if( rc == -1 )
    goto cleanup;

  rc = output_compact_smaps(file, segment_fields);

  cleanup:

  scan_file_delete(file);
  return rc;
}

/* ------------------------------------------------------------------------- *
 * snapshot_all  -- retrieve snapshot of information for one process
 * ------------------------------------------------------------------------- */

static int snapshot_all(const char *root, int compact)
{
  char   *status_text = 0;
  size_t  status_size = 0;
  char   *cmdline_text = 0;
  size_t  cmdline_size = 0;

  fields_t *segment_fields = 0;

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

      ++cnt;

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

      if( cnt != 1 )
      {
        output_raw("\n",1);
      }

      snprintf(path, sizeof path, "%s/%s/smaps", root, de->d_name);

      if( compact )
      {
        if( cnt == 1 )
        {
          if( strcmp(de->d_name, "1") != 0 )
          {
            msg_fatal("PID 1 expected first\n");
          }

          segment_fields = parse_smaps_segment_fields(path);
          if( !segment_fields )
          {
            msg_fatal("Could not load fields\n");
          }

          output_compact_header(segment_fields);
        }

        output_fmt("%s%c%s", compact_process_fields_leader, compact_field_sep,
                   de->d_name);
      }
      else
      {
        output_fmt("==> /proc/%s/smaps <==\n", de->d_name);
      }

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

      if( compact )
      {
        output_fmt("%c%s", compact_field_sep, name);
      }
      else
      {
        output_fmt("#Name: %s\n", name);
      }

#define X(v)                                                  \
      if( compact )                                           \
      {                                                       \
        output_fmt("%c%s", compact_field_sep,                 \
                   status.v ? status.v : "");                 \
      }                                                       \
      else                                                    \
      {                                                       \
        if( status.v ) output_fmt("#%s: %s\n", #v, status.v); \
      }

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

      if( compact )
      {
        output_fmt("\n");
      }
#undef X

      smaps_bytes = compact
        ? output_compact_smaps_file(path, segment_fields)
        : output_file(path);

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
  int compact = 0;
  const char *root = "/proc";

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
    case opt_compact:
      compact = 1;
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
    case opt_root:
      root = par;
      break;
    }
  }

  argvec_delete(args);

  page_size = sysconf(_SC_PAGESIZE);
  if( page_size < 0 )
  {
    msg_fatal("sysconf: %s\n", strerror(errno));
  }

  page_size_mask = ~((uintptr_t)page_size - 1);

  return snapshot_all(root, compact) ? EXIT_FAILURE : EXIT_SUCCESS;
}
