/* Maildirwatch -- watch a maildir for new messages
   Copyright (C) 2014, 2015 Giuseppe Scrivano <gscrivano@gnu.org>

Maildirwatch is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3 of the License, or
(at your option) any later version.

GNU Wget is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Wget.  If not, see <http://www.gnu.org/licenses/>.

*/
#define _GNU_SOURCE

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>
#include <error.h>

#include <dirent.h>
#include <sys/types.h>

static int inotify_fd;

static bool print_subject;
static bool opt_json;

struct Maildir
{
  struct Maildir *next;
  char *name;
  char *pretty_name;
  int fd_cur;
  int fd_new;
};

static struct Maildir *maildirs = NULL;

static int
message_is_read (const char *name)
{
  const char *flags;
  flags = strstr (name, ":2");
  if (flags == NULL)
    return -1;

  return strchr (flags + 2, 'S') == NULL ? 0 : 1;
}

static void
print_str_encoded (const char *str)
{
  for (; *str; str++)
    {
      switch (*str)
        {
        case '\'':
        case '"':
        case '\\':
        case '/':
        case '\b':
        case '\f':
        case '\t':
        case '\n':
        case '\r':
          putchar ('\\');
        default:
          putchar (*str);
        }
    }
}

static void
print_json (const char *folder, const char *path, const char *from, const char *subject)
{
  printf ("{\"folder\": \"");
  print_str_encoded (folder);
  printf ("\", \"path\": \"");
  print_str_encoded (path);
  printf ("\", \"from\": \"");
  print_str_encoded (from);
  printf ("\", \"subject\": \"");
  print_str_encoded (subject);
  printf ("\"}\n");
}

static void
dump_mdirs ()
{
  struct Maildir *m;

  for (m = maildirs; m; m = m->next)
    {
      printf ("%s : %s\n", m->pretty_name, m->name);
    }
}

static void
dump_stats (bool details)
{
  struct Maildir *m;
  struct dirent *ent;
  size_t total_unread = 0;

  for (m = maildirs; m; m = m->next)
    {
      size_t unread = 0;
      size_t i;
      for (i = 0; i < 2; i++)
	{
	  char *path;
	  DIR *dir;

	  asprintf (&path, "%s/%s", m->name, i == 0 ? "cur" : "new");
	  if (path == NULL)
	    continue;

	  dir = opendir (path);
	  free (path);
	  if (dir == NULL)
	    continue;

	  for (;;)
	    {
	      ent = readdir (dir);
	      if (ent == NULL)
		break;

	      if (ent->d_type != DT_REG
		  || strcmp (ent->d_name, ".") == 0
		  || strcmp (ent->d_name, "..") == 0)
		continue;


	      if (message_is_read (ent->d_name) == 0)
		unread += 1;
	    }
	  closedir (dir);
	}

      if (details)
	printf ("%s: unread messages %zu\n", m->pretty_name, unread);
      total_unread += unread;
    }

  printf ("total unread: %zu\n", total_unread);
}

static int
add_dir (const char *pretty_name, const char *name)
{
  char *dir;
  int ret;
  struct Maildir *m;

  m = malloc (sizeof *m);
  if (m == NULL)
    return -1;

  memset (m, 0, sizeof (*m));

  m->name = strdup (name);
  if (m->name == NULL)
    goto fail;

  m->pretty_name = strdup (pretty_name);
  if (m->pretty_name == NULL)
    goto fail;

  asprintf (&dir, "%s/cur", name);
  if (dir == NULL)
    goto fail;

  m->fd_cur = inotify_add_watch (inotify_fd, dir, IN_MOVED_TO);
  free (dir);
  if (m->fd_cur < 0)
    {
      if (access (dir, F_OK) < 0)
        goto ignore;
    goto fail;
    }

  asprintf (&dir, "%s/new", name);
  if (dir == NULL)
    goto fail;
  m->fd_new = inotify_add_watch (inotify_fd, dir, IN_MOVED_TO);
  free (dir);
  if (m->fd_cur < 0)
    goto fail;

  m->next = maildirs;
  maildirs = m;
  return 0;

ignore:
  ret = 0;
  goto cleanup;

fail:
  ret = -1;

cleanup:
  free (m->pretty_name);
  free (m->name);
  if (m->fd_cur)
    inotify_rm_watch (inotify_fd, m->fd_cur);
  if (m->fd_new)
    inotify_rm_watch (inotify_fd, m->fd_new);
  free (m);
  return ret;
}

static int
check_dir (const char *name)
{
  int ret = -1;
  struct dirent *ent;
  char *cur = NULL;
  DIR *dir = opendir (name);
  if (dir == NULL)
    return -1;

  for (;;)
    {
      ent = readdir (dir);
      if (ent == NULL)
	break;

      if (ent->d_type != DT_DIR
	  || strcmp (ent->d_name, ".") == 0
	  || strcmp (ent->d_name, "..") == 0)
	continue;

      if (strcmp (ent->d_name, "cur") == 0 ||
	  strcmp (ent->d_name, "new") == 0 ||
	  strcmp (ent->d_name, "tmp") == 0)
	continue;

      asprintf (&cur, "%s/%s", name, ent->d_name);
      if (cur == NULL)
	error (EXIT_FAILURE, errno, "Could not allocate path for %s",
	       ent->d_name);

      if (access (cur, F_OK) < 0)
	goto cleanup;

      if (add_dir (ent->d_name, cur) < 0)
	error (EXIT_FAILURE, errno, "Could not add directory %s",
	       ent->d_name);

      free (cur);
      cur = NULL;
    }

  ret = 0;

cleanup:
  free (cur);
  closedir (dir);
  return ret;
}

enum
  {
    IN_HEADER,
    IN_SUBJECT,
    IN_FROM,
  };

static void
get_email_from_subject (const char *filename, char **from , char **subject)
{
  char *lineptr;
  size_t len;
  FILE *f;
  int state = IN_HEADER;
  int left = 0;

  if (from)
    {
      *from = NULL;
      left++;
    }
  if (subject)
    {
      *subject = NULL;
      left++;
    }

  f= fopen (filename, "r");
  if (f == NULL)
    return;

  while (left)
    {
      char *tmp;
      lineptr = NULL;
      len = 0;
      if (getline (&lineptr, &len, f) < 0)
	goto exit;

      switch (state)
        {
        case IN_SUBJECT:
          if (lineptr[0] != ' ')
            {
              state = IN_HEADER;
              left--;
              goto HANDLE_IN_HEADER;
            }
          *strchr (lineptr, '\n') = '\0';
          asprintf (&tmp, "%s%s", *subject, lineptr);
          free (*subject);
          *subject = tmp;
          break;

        case IN_FROM:
          if (lineptr[0] != ' ')
            {
              state = IN_HEADER;
              left--;
              goto HANDLE_IN_HEADER;
            }
          *strchr (lineptr, '\n') = '\0';
          asprintf (&tmp, "%s%s", *from, lineptr);
          free (*from);
          *from = tmp;
          break;

        case IN_HEADER:
HANDLE_IN_HEADER:
          if (subject && len > 10 && memcmp (lineptr, "Subject: ", 9) == 0)
            {
              tmp = strdup (lineptr + 9);
              *strchr (tmp, '\n') = '\0';
              *subject = tmp;
              state = IN_SUBJECT;
            }

          if (from && len > 7 && memcmp (lineptr, "From: ", 6) == 0)
            {
              tmp = strdup (lineptr + 6);
              *strchr (tmp, '\n') = '\0';
              *from = tmp;
              state = IN_FROM;
            }
          break;
        }
    }

exit:
  if (lineptr)
    free (lineptr);
  fclose (f);
}

static void
handle_events (int argc, char *argv[])
{
#define BUFFER_SIZE 4096
  char *buf;
  const struct inotify_event *event;
  int i;
  ssize_t len;
  char *ptr;
  const char *subdir = NULL;

  buf = malloc (BUFFER_SIZE);
  if (buf == NULL)
    return;

  for (;;)
    {
      len = read (inotify_fd, buf, BUFFER_SIZE);
      if (len == -1 && errno != EAGAIN)
	{
	  exit (EXIT_FAILURE);
	}

      if (len <= 0)
	break;

      for (ptr = buf; ptr < buf + len;
	   ptr += sizeof (struct inotify_event) + event->len)
	{
          struct Maildir *maildir = NULL;

	  event = (const struct inotify_event *) ptr;

	  if (event->mask & IN_ISDIR)
	    {
	      continue;
	    }

	  for (maildir = maildirs; maildir; maildir = maildir->next)
	    {
	      if (event->wd == maildir->fd_cur)
		{
		  subdir = "cur";
		  break;
		}
	      if (event->wd == maildir->fd_new)
		{
		  subdir = "new";
		  break;
		}
	    }

	  if (subdir == NULL)
	    error (EXIT_FAILURE, errno, "Could not find descriptor for %s",
		   subdir);

	  if (event->mask & IN_MOVED_TO)
	    {
	      char *path;

	      asprintf (&path, "%s/%s/%s", maildir->name, subdir, event->name);
	      if (path == NULL)
		error (EXIT_FAILURE, errno, "Could not allocate path for %s",
		       subdir);

	      if (message_is_read (event->name) == 0)
		{
                  if (opt_json)
                    {
                      char *from = NULL, *subject = NULL;
                      get_email_from_subject (path, &from, &subject);
                      print_json (maildir->pretty_name, path, from, subject);
                    }
		  else if (print_subject)
		    {
                      char *subject = NULL;
                      get_email_from_subject (path, NULL, &subject);
		      printf ("New message: %s : %s\n", path, subject);
		      free (subject);
		    }
		  else
		    {
		      printf ("%s\n", path);
		    }
		}
	      free (path);
	    }
	}
    }

  free (buf);
}

int
main (int argc, char *argv[])
{
  char buf;
  int i, poll_num;
  int *wd;
  nfds_t nfds;
  struct pollfd fds[2];

  if (argc < 2)
    {
      printf ("Usage: %s PATH [PATH ...]\n", argv[0]);
      exit (EXIT_FAILURE);
    }

  inotify_fd = inotify_init1 (IN_NONBLOCK);
  if (inotify_fd == -1)
    {
      error (EXIT_FAILURE, errno, "Could not init inotify");
    }

  for (i = 1; i < argc && argv[i][0] == '-'; i++)
    {
      if (strcmp (argv[i], "--json") == 0)
        opt_json = 1;
    }

  for (; i < argc; i++)
    {
      if (check_dir (argv[i]))
	error (EXIT_FAILURE, errno, "Could not check directory %s", argv[i]);
    }

  nfds = 2;

  fds[0].fd = inotify_fd;
  fds[0].events = POLLIN;

  fds[1].fd = 0;
  fds[1].events = POLLIN;

  while (1)
    {
      poll_num = poll (fds, nfds, -1);
      if (poll_num == -1)
	{
	  if (errno == EINTR)
	    continue;
	  error (EXIT_FAILURE, errno, "Could not poll");
	}

      if (poll_num > 0)
	{
	  if (fds[0].revents & POLLIN)
	    {
	      handle_events (argc, argv);
	    }

	  if (fds[1].revents & POLLIN)
	    {
	      char buf[64];
	      read (0, buf, sizeof (buf));

	      switch (buf[0])
		{
		case 'l':
		  dump_stats (true);
		  break;

		case 'i':
		  dump_stats (false);
		  break;

		case 's':
		  print_subject = !print_subject;
		  break;

		case 'd':
		  dump_mdirs ();
		}
	    }
	}
    }
  return 0;
}
