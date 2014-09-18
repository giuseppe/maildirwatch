/* Maildirwatch -- watch a maildir for new messages
   Copyright (C) 2014 Giuseppe Scrivano <gscrivano@gnu.org>

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

Additional permission under GNU GPL version 3 section 7

If you modify this program, or any covered work, by linking or
combining it with the OpenSSL project's OpenSSL library (or a
modified version of that library), containing parts covered by the
terms of the OpenSSL or SSLeay licenses, the Free Software Foundation
grants you additional permission to convey the resulting work.
Corresponding Source for a non-source form of such a combination
shall include the source code for the parts of OpenSSL used as well
as that of the covered work.  */

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <string.h>

#include <dirent.h>
#include <sys/types.h>

static int inotify_fd;

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
message_is_read(const char *name)
{
  const char *flags;
  flags = strstr(name, ":2");
  if (flags == NULL)
    return -1;

  return strchr(flags + 2, 'S') == NULL ? 0 : 1;
}

static void
dump_stats(int details)
{
  struct Maildir *m;
  struct dirent *ent;
  size_t total_unread = 0;

  for (m = maildirs; m; m = m->next) {
    size_t unread = 0;
    size_t i;
    for (i = 0; i < 2; i++) {
      char *path;
      DIR *dir;

      asprintf(&path, "%s/%s", m->name, i == 0 ? "cur" : "new");
      if (path == NULL)
        continue;

      dir = opendir(path);
      free(path);
      if (dir == NULL)
        continue;

      for (;;) {
        ent = readdir(dir);
        if (ent == NULL)
          break;

        if (ent->d_type != DT_REG || ent->d_name[0] == '.')
          continue;

  
        if (message_is_read(ent->d_name) == 0)
          unread += 1;
      }
      closedir(dir);
    }

    if (details)
      printf("%s: unread messages %zu\n", m->pretty_name, unread);
    total_unread += unread;
  }

  printf("total unread: %zu\n", total_unread);
}

static int
add_dir(const char *pretty_name, const char *name)
{
  char *dir;
  struct Maildir *m = malloc(sizeof *m);
  if (m == NULL)
    return -1;

  memset(m, 0, sizeof (*m));

  m->name = strdup(name);
  if (m->name == NULL)
    goto fail;

  m->pretty_name = strdup(pretty_name);
  if (m->pretty_name == NULL)
    goto fail;

  asprintf(&dir, "%s/cur", name);
  if (dir == NULL)
    goto fail;
  m->fd_cur = inotify_add_watch(inotify_fd, dir,
                                IN_MOVED_TO);
  free(dir);
  if (m->fd_cur < 0)
    goto fail;

  asprintf(&dir, "%s/new", name);
  if (dir == NULL)
    goto fail;
  m->fd_new = inotify_add_watch(inotify_fd, dir,
                                IN_MOVED_TO);
  free(dir);
  if (m->fd_cur < 0)
    goto fail;

  m->next = maildirs;
  maildirs = m;
  return 0;

 fail:
  free(m->pretty_name);
  free(m->name);
  if (m->fd_cur)
    inotify_rm_watch(inotify_fd, m->fd_cur);
  if (m->fd_new)
    inotify_rm_watch(inotify_fd, m->fd_new);
  free(m);
  return -1;
}

static int
check_dir(const char *name)
{
  int ret = -1;
  struct dirent *ent;
  char *cur = NULL;
  DIR *dir = opendir(name);
  if (dir == NULL)
    return -1;

  for (;;) {
    ent = readdir(dir);
    if (ent == NULL)
      break;

    if (ent->d_type != DT_DIR || ent->d_name[0] == '.')
      continue;

    if (strcmp(ent->d_name, "cur") == 0 ||
        strcmp(ent->d_name, "new") == 0 ||
        strcmp(ent->d_name, "tmp") == 0)
      continue;

    asprintf(&cur, "%s/%s", name, ent->d_name);
    if (cur == NULL)
      error(EXIT_FAILURE, errno, "Could not allocate path for %s", ent->d_name);

    if (access(cur, F_OK) < 0)
      goto cleanup;

    printf("Adding: %s\n", ent->d_name);
    if (add_dir(ent->d_name, cur) < 0)
      error(EXIT_FAILURE, errno, "Could not add directory %s", ent->d_name);

    free(cur);
    cur = NULL;
  }

  ret = 0;

 cleanup:
  free(cur);
  closedir(dir);
  return ret;
}

static char *
get_email_subject(const char *filename)
{
  char *lineptr;
  size_t len;
  char *ret = NULL;
  FILE *f = fopen(filename, "r");
  if (f == NULL)
    return NULL;

  for (;;) {
    lineptr = NULL;
    len = 0;

    if (getline(&lineptr, &len, f) < 0)
      goto exit;

    if (len > 10 && memcmp(lineptr, "Subject: ", 9) == 0) {
      ret = strdup(lineptr + 9);
      *strchr(ret, '\n') = '\0';
      goto exit;
    }

    free(lineptr);
  }

 exit:
  free(lineptr);
  fclose (f);
  return ret;
}

static void
handle_events(int argc, char* argv[])
{
#define BUFFER_SIZE 4096
  char *buf;
  const struct inotify_event *event;
  int i;
  ssize_t len;
  char *ptr;
  struct Maildir *it;
  const char *subdir = NULL;

  buf = malloc(BUFFER_SIZE);
  if (buf == NULL)
    return;

  for (;;) {

    len = read(inotify_fd, buf, BUFFER_SIZE);
    if (len == -1 && errno != EAGAIN) {
      exit(EXIT_FAILURE);
    }

    if (len <= 0)
      break;

    for (ptr = buf; ptr < buf + len;
         ptr += sizeof(struct inotify_event) + event->len) {

      event = (const struct inotify_event *) ptr;

      if (event->mask & IN_ISDIR) {
        continue;
      }

      for (it = maildirs; it; it = it->next) {
        if (event->wd == it->fd_cur) {
          subdir = "cur";
          break;
        }
        if (event->wd == it->fd_new) {
          subdir = "new";
          break;
        }
      }

      if (subdir == NULL)
          error(EXIT_FAILURE, errno, "Could not find descriptor for %s", subdir);

      if (event->mask & IN_MOVED_TO) {
        char *subject, *path;

        asprintf(&path, "%s/%s/%s", it->name, subdir, event->name);
        if (path == NULL)
          error(EXIT_FAILURE, errno, "Could not allocate path for %s", subdir);

        subject = get_email_subject(path);
        if (message_is_read(event->name) == 0)
          printf("New message: %s : %s\n", path, subject);

        free(path);
        free(subject);
      }
    }
  }

  free(buf);
}

int
main(int argc, char* argv[])
{
  char buf;
  int i, poll_num;
  int *wd;
  nfds_t nfds;
  struct pollfd fds[2];

  if (argc < 2) {
    printf("Usage: %s PATH [PATH ...]\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  inotify_fd = inotify_init1(IN_NONBLOCK);
  if (inotify_fd == -1) {
    error(EXIT_FAILURE, errno, "Could not init inotify");
  }

  for (i = 1; i < argc; i++) {
    if (check_dir(argv[i]))
      error(EXIT_FAILURE, errno, "Could not check directory %s", argv[i]);
  }

  printf("READY\n");

  nfds = 2;

  fds[0].fd = inotify_fd;
  fds[0].events = POLLIN;

  fds[1].fd = 0;
  fds[1].events = POLLIN;

  while (1) {
    poll_num = poll(fds, nfds, -1);
    if (poll_num == -1) {
      if (errno == EINTR)
        continue;
      error(EXIT_FAILURE, errno, "Could not poll");
    }

    if (poll_num > 0) {
      if (fds[0].revents & POLLIN) {
        handle_events(argc, argv);
      }

      if (fds[1].revents & POLLIN) {
        char buf[64];
        read(0, buf, sizeof(buf));
        dump_stats(buf[0] == 'l');
      }
    }
  }

  return 0;
}
