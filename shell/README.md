# Shell API Documentation

Index into `shell/`'s two files. As with `kernel/README.md`, this points
at the real documentation rather than replacing it - every function below
has a fuller comment at its actual declaration/definition; open the file
for the details.

## `shell.h` / `shell.c`

The command-line shell: input handling, built-in commands (`cd`, `ls`,
`setenv`, `for`/pipes, etc.), and environment variable expansion.

```c
void shell_init(void);
void shell_handle_char(char c);
void shell_handle_key(int key);       // special keys - see keyboard.h's SHELL_KEY_* defines
int  shell_has_pending_command(void);
void shell_run_pending_command(void);
```

`shell_handle_char`/`shell_handle_key` are called directly from the
keyboard ISR (see `keyboard.c`), but a completed command line is only
*flagged* there, not run - `shell_has_pending_command`/
`shell_run_pending_command` split reading a line from executing it, so a
command actually dispatches from the kernel's main loop instead of from
inside an interrupt handler. That split matters: interrupt gates clear
`IF` on entry and don't restore it until `iret`, so running a command
synchronously from ISR context would mean any app that itself needs to
block waiting for further keyboard input (see `kernel/proc/loader.c`'s
raw input mode) would deadlock waiting for an interrupt that can't fire.

Variable expansion (`$VAR`, and the `$((...))` arithmetic form `echo`
evaluates) lives in `shell.c`'s own `expand_vars()` - not declared in the
header since nothing outside `shell.c` calls it directly. Notably, a bare
identifier inside `$((...))` is **not** expanded (you need the `$`, e.g.
`$(($i + 5))`, not `$((i + 5))`) - this is deliberate, not a missing
feature.

## `isr_wrappers.s`

Every interrupt/exception entry point, and every place execution crosses
a CPL boundary (ring0↔ring3). No header - these are `extern`-declared
individually wherever they're used (`idt.c`'s gate installs, `task.c`,
`user_trampolines.c`). Grouped here by what they're for:

**Interrupt/exception entry points** (installed into the IDT by `idt.c`):
- `isr_default_common` / `isr_default` - shared tail (EOI + register
  restore + `iret`) and the default handler for vectors with no CPU-pushed
  error code (most exceptions, all hardware IRQs).
- `isr_err_stub` - for vectors that DO push an error code; pops it off
  before `iret` runs, or `iret` pops the error code as the return `EIP`
  and cascades into a double, then triple, fault.
- `page_fault_isr` - page faults specifically, so `page_fault_handler`'s
  C-side diagnostics can report the exact faulting instruction.
- `keyboard_isr` / `mouse_isr` / `rtl8139_isr` / `pit_isr` - the four
  hardware IRQ handlers. `rtl8139_isr` does NOT send its own EOI (the C
  side already knows the master-vs-slave rule); `pit_isr` sends EOI
  *before* calling `task_tick()`, since that call can switch away from
  this task entirely and a late EOI would stall the whole scheduler.

**Privilege/segment loads:**
- `gdt_flush` - reloads `CS` via a far jump (the only way to reload `CS`
  outside of an interrupt/call/ret).
- `tss_flush` - loads the task register, so the CPU knows where to find
  `esp0`/`ss0` on a ring3→ring0 transition.

**Syscall gate:**
- `syscall_isr` - the `int 0x80` entry point every `app_api_t`/`wm_api_t`
  trampoline (`user_trampolines.c`) traps through; see `syscall.h` for the
  register ABI.

**Task switching** (used by `task.c`):
- `task_switch_asm` - the actual context switch: save the outgoing task's
  registers, load the incoming task's.
- `task_trampoline` - where a brand-new plain task (`task_create`) lands
  on its very first switch-in, instead of the normal "resume mid-function"
  path `task_switch_asm` otherwise takes.

**Ring3 entry points** - each is a small "read arguments off the
fabricated stack into registers, `iret` into a `.usertramp`-resident
stub that does the real call" landing pad. A plain `iret` can't push a
call argument the way `call` would - it just starts fetching at `EIP`
with the CPU already at ring3 - so each of these hands off to a stub
that does the actual cdecl push-and-call once privilege has actually
dropped:
- `ring3_entry_trampoline` → (no stub) - a bare no-argument entry point,
  used by `task_create_ring3`'s raw smoke-test task.
- `ring3_entry_arg_trampoline` → **`user_task_arg_crt0`** - delivers a
  single `void *arg`, matching `task_entry_t`'s signature. Used by
  `task_create_ring3_child`, i.e. `SYS_SPAWN_TASK`.
- `app_entry_trampoline` → **`user_app_crt0`** - delivers `(argc, argv,
  api)`, matching `app_entry_t`. Used by `task_create_app_ring3` to start
  a real app's `app_main`.

Both `.usertramp`-resident stubs call `SYS_EXIT_TASK` after their entry
point returns, as a safety net in case it returns without calling
`exit_task()`/`api->exit_task()` itself.