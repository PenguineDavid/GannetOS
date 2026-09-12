/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef LOADER_H
#define LOADER_H

/* Fixed virtual address where apps are loaded and linked against.
   Must match APP_LOAD_ADDR in the Makefile.                       */
#define APP_LOAD_ADDR 0x200000

/*
 * loader_exec: search for and execute a named command.
 *
 * name      -- bare command name (e.g. "echo") or explicit path ("/bin/echo")
 * argc/argv -- argument vector, argv[0] is the command name
 * path_env  -- colon-separated search path string, e.g. "/bin:/usr/bin"
 *              The caller (shell) owns this string; loader does not modify it.
 *              Ignored when name contains a '/'.
 * cwd       -- current working directory, searched BEFORE path_env entries
 *              for bare names.  Pass NULL to skip cwd search.
 *
 * Returns the app's return value, or -1 if not found / failed to load.
 */
int loader_exec(const char *name, int argc, char **argv,
                const char *path_env, const char *cwd);

/* Loads and runs a window manager .pexe at an explicit absolute path (no
   PATH search - WMs always live in /wm/ and are found there, see the `wm`
   shell command). Blocking: the caller doesn't get control back until the
   WM's own entry point returns, same as a normal app - a WM is expected
   to keep running (drawing, polling should_quit()) until it decides to
   stop, at which point returning from entry() hands control back here.
   Returns the WM's own return value, or -1 if it couldn't be loaded. */
int wm_try_exec(const char *path, int argc, char **argv);

void loader_init(void);

#endif