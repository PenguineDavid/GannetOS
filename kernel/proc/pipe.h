/*
 * Copyright (C) 2026 David S
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>

#define PIPE_BUF_SIZE 4096

/* Activate pipe capture -- subsequent api->puts/putchar calls write
   into the pipe buffer instead of VGA.                              */
void pipe_begin(void);

/* End capture. Returns pointer to null-terminated captured string
   and resets the write position to 0.                              */
const char *pipe_end(void);

/* Returns 1 if capture is currently active */
int pipe_active(void);

/* Write one char into the pipe buffer (called by api shims) */
void pipe_putchar(char c);

/* ------------------------------------------------------------------ */
/* stdin side - the OTHER end of the same pipe. Once one stage's output */
/* has been captured via pipe_begin()/pipe_end(), the shell hands that  */
/* captured string to the NEXT stage's pipe_set_stdin() before running  */
/* it, so THAT stage's app can read it back via pipe_read_stdin() (or,  */
/* for a real app, api->read_stdin()/api->has_stdin() - see pexe.h)     */
/* instead of the shell having to know anything about how the app       */
/* wants its input delivered.                                           */
/* ------------------------------------------------------------------ */

/* Sets the string a subsequent pipe_read_stdin() call will read from,
   and resets the read position to the start. Pass NULL (or call this
   at all before a non-piped command) to mean "no piped-in stdin" - a
   subsequent pipe_has_stdin() then reports false and pipe_read_stdin()
   returns 0 immediately, same as if it had already been fully drained. */
void pipe_set_stdin(const char *data);

/* True if pipe_set_stdin() was last given a real (non-NULL) string -
   i.e. this command is running as a non-first pipeline stage. Doesn't
   say whether it's been fully read yet; use pipe_read_stdin's return
   value (0 = nothing left) for that. */
int pipe_has_stdin(void);

/* Reads up to maxlen-1 bytes from wherever pipe_set_stdin() pointed,
   continuing from the last read position so repeated calls drain it
   incrementally rather than re-reading from the start. Null-terminates
   whatever it writes into buf. Returns the number of bytes actually
   written - 0 once exhausted, or if pipe_set_stdin() was never called
   (or was last called with NULL). */
int pipe_read_stdin(char *buf, int maxlen);

#endif