#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([maildirwatch], [0.1], [gscrivano@gnu.org])
AC_CONFIG_SRCDIR([maildirwatch.c])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([1.9])

AC_PROG_CC

AC_CHECK_HEADERS([stdlib.h string.h unistd.h])

AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

AC_FUNC_ERROR_AT_LINE
AC_FUNC_MALLOC
AC_CHECK_FUNCS([memset strchr strdup strstr])
AC_CHECK_FUNCS([inotify_add_watch], [], [AC_MSG_ERROR([inotify needed])])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT
