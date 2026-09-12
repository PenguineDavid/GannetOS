# Kernel API Documentation

This is an index into `kernel/`'s headers, organized by subsystem. It exists
so you can find the right file quickly - it is **not** a substitute for
reading that file. Every header in this tree carries its own detailed
doc comments (what a function does, its failure modes, and often *why*
it's built the way it is); this page just tells you which header to open
and gives you the one-line version of its public API. When the two ever
disagree, the header is right and this page is stale - please send a PR.

Struct/typedef definitions are only listed by name here; see the header
for their fields.

## Layout

```
kernel/
├── kernel.c, bsp.h        top-level: boot entry, WM layout tree
├── arch/x86/              CPU-level setup: GDT, IDT, paging
├── mem/                   physical memory allocation
├── proc/                  tasks, syscalls, the app loader, pipes
├── drivers/               PCI, PIC, PIT, RTC, keyboard, mouse, RTL8139, networking
└── ui/                    framebuffer, terminal, window manager
```

## Boot & CPU setup - `kernel/arch/x86/`

**`gdt.h`** - Global Descriptor Table and the TSS. Defines the kernel/user
code/data selectors and installs a real GDT (replacing boot.asm's minimal
one) with the ring0/ring3 split and a TSS for privilege-level transitions.
- `void gdt_init(void);`
- `void tss_set_kernel_stack(uint32_t esp0);` - sets `TSS.esp0`, i.e. which
  kernel stack a ring3→ring0 transition (interrupt, syscall) lands on for
  whichever task is about to run.
- `void df_handler_init(uint32_t kernel_cr3);`

**`idt.h`** - Interrupt Descriptor Table. `struct idt_entry` / `struct
idt_ptr` are the raw x86 gate/LIDT layouts.
- `void idt_set_gate(uint8_t num, uint32_t base, uint16_t selector, uint8_t flags);`
- `void idt_init();`

**`paging.h`** - Virtual memory, built up in three phases documented at the
top of the file: (1) identity-mapped paging with no isolation, (2) one page
directory per task but shared page tables, (3) genuinely private physical
frames per task. This is also where the per-task private-memory machinery
used by `try_exec`/`task_create_ring3_child` (see `proc/task.h`) lives.
- `void paging_init(void);`
- `int paging_enabled(void);`
- `uint32_t paging_kernel_directory_phys(void);`
- `uint32_t paging_clone_kernel_directory(void);` / `void paging_free_directory(uint32_t dir_phys);`
- `void paging_switch_directory(uint32_t phys_addr);`
- `uint32_t paging_map_private_page(uint32_t dir_phys, uint32_t vaddr);` - maps one private, per-directory 4KB page; reuses an already-owned table for a second page in the same 4MB slot rather than clobbering it.
- `void paging_free_private_pages(uint32_t dir_phys);` - frees everything a directory privately owns (not what it merely shares in - see `paging_share_pde`).
- `void paging_share_pde(uint32_t dst_dir_phys, uint32_t src_dir_phys, uint32_t vaddr);` - lets a spawned child task share its parent's code/data mapping.
- `void paging_unmap_page(uint32_t vaddr);`
- `void paging_mark_kernel_region_user(uint32_t vaddr, uint32_t length);` - marks an already-mapped kernel region ring3-accessible (used once at boot for the app exec buffer/stack/syscall trampolines).

**`include/asm/io.h`** - `inb`/`outb`/`inw`/`outw`/`inl`/`outl` port I/O
primitives, `static inline` wrappers around the `in`/`out` instructions.
Included as `<asm/io.h>` (see the Makefile's `-I kernel/arch/x86/include`) -
this is deliberately the one Linux-style include path in the codebase,
mirroring `arch/<arch>/include/asm/` in a real kernel tree.

## Memory - `kernel/mem/`

**`pmm.h`** - Physical memory manager: a bitmap allocator over a fixed
pool of RAM (see the header for the known limitation that the pool range
is hardcoded, not derived from a real memory map). This is what makes
`paging.h`'s per-task pages point at genuinely different physical RAM
instead of all the same page tables.
- `void pmm_init(void);`
- `uint32_t pmm_alloc_frame(void);` - returns a free, zeroed 4KB frame's physical address, or 0.
- `void pmm_free_frame(uint32_t phys_addr);`

## Process & task management - `kernel/proc/`

**`task.h`** - The task table and every way a task gets created. Read this
one before touching scheduling or process lifetime.
- `void task_init(void);`
- `int task_create(task_entry_t entry, void *arg, const char *name);` - plain ring0 task.
- `int task_create_ring3(uint32_t entry, uint32_t user_stack_top, const char *name, uint32_t *out_private_frame);` - raw ring3 task, no argument-passing convention.
- `int task_create_app_ring3(uint32_t dir_phys, uint32_t app_main_addr, int argc, char **argv, void *api, uint32_t user_stack_top, const char *name);` - runs a real app (`app_main(argc, argv, api)`), used by `loader.c`'s `try_exec`.
- `int task_create_ring3_child(uint32_t entry, void *arg, const char *name);` - what `SYS_SPAWN_TASK` is built on: a real ring3 sibling task sharing its parent's code/data.
- `void task_yield(void);` / `void task_exit(void);` / `void task_tick(void);`
- `int task_current_id(void);` / `uint32_t task_current_cr3(void);`
- `int task_is_zombie(int id);` / `void task_reap(int id);` - reclaims a zombie's slot; also cascades into and reaps any children it spawned first (see the header's own comment on why that ordering matters).

**`loader.h`** - Finds and runs a named command (or, for the window
manager, the one WM binary). `APP_LOAD_ADDR` here must match the
Makefile's own copy.
- `int loader_exec(const char *name, int argc, char **argv, const char *path_env);`
- `int wm_try_exec(const char *path, int argc, char **argv);`
- `void loader_init(void);`

**`pexe.h`** - The `.pexe` executable format (`PEXE_MAGIC`, the on-disk
header) and `app_api_t`, the syscall-trampoline table every app receives
as its third argument. If you're adding a new syscall an app should be
able to call, this struct gets a new function-pointer field, matching
`kernel/proc/syscall.h`'s enum and `user_trampolines.c`'s trampoline for
it. Also defines `APP_PRIVATE_VADDR`, the one page every task gets
privately for free use.

**`pipe.h`** - Shell pipeline (`cmd1 | cmd2`) output capture: redirects
what would otherwise go to the screen into an in-memory buffer.
- `void pipe_begin(void);` / `const char *pipe_end(void);` / `int pipe_active(void);` / `void pipe_putchar(char c);`
- `void pipe_set_stdin(const char *data);` / `int pipe_has_stdin(void);` / `int pipe_read_stdin(char *buf, int maxlen);`

**`syscall.h`** - The `int 0x80` gate: the ABI (`eax` = syscall number,
args in `ebx`/`ecx`/`edx`/`esi`/`edi`) and the `enum` of syscall numbers
every app's `app_api_t` trampoline maps onto. Pointer arguments coming
through this gate are validated against the calling task's own memory
before use - see `syscall.c`'s `user_range_ok`/`user_str_ok`.
- `void syscall_init(void);`
- `uint32_t syscall_dispatch(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);`

## Drivers - `kernel/drivers/`

**`pci/pci.h`** - Legacy two-port PCI configuration space access
(`0xCF8`/`0xCFC`), used to find and configure the RTL8139 NIC.
- `uint32_t pci_config_read32(...)` / `void pci_config_write32(...)` (and `16`-bit variants)
- `int pci_find_device(uint16_t vendor_id, uint16_t device_id, pci_device_t *out);`
- `void pci_enable_bus_mastering(const pci_device_t *dev);`

**`pic/pic.h`** - 8259 PIC remapping and end-of-interrupt signalling.
- `void pic_remap(uint8_t offset1, uint8_t offset2);` / `void pic_send_eoi(uint8_t irq);`

**`pit/pit.h`** - Programmable Interval Timer; also the scheduler's tick
source once initialized.
- `void pit_init(uint32_t frequency_hz);` / `uint32_t pit_ticks(void);`

**`rtc/rtc.h`** - Reads the CMOS real-time clock directly (no interrupt
involved - the chip keeps wall-clock time on its own).
- `void rtc_format_time(char *buf);` - writes `"HH:MM:SS\0"` (9 bytes) into `buf`.

**`keyboard/keyboard.h`** - PS/2 keyboard: scancode translation, the
shell's normal buffered input, and a raw unbuffered mode for apps that
need every keystroke live (see `SHELL_KEY_*` defines for the special-key
codes it hands the shell).
- `void keyboard_init(void);` / `void keyboard_handler(void);`
- `void kb_set_raw_mode(int on);` / `int kb_raw_getchar(void);` / `int kb_raw_getchar_nonblocking(void);`

**`mouse/mouse.h`** - PS/2 auxiliary device (mouse), Intellimouse wheel
reporting.
- `void mouse_init(void);` / `void mouse_handler(void);`

**`rtl8139/rtl8139.h`** - The RTL8139 NIC driver: DMA ring buffer setup,
frame TX/RX. See the header's own comment on why this card was chosen
over anything fancier as the first network driver.
- `int rtl8139_init(void);` / `void rtl8139_get_mac(uint8_t mac_out[6]);`
- `int rtl8139_send(const void *data, uint16_t len);` / `int rtl8139_receive(void *buf);`

### Networking - `kernel/drivers/net/`

**`net.h`** - Brings up the whole stack in order (Ethernet → ARP → IPv4 →
UDP → TCP → DNS) with QEMU usermode-networking defaults.
- `void net_init(uint32_t local_ip);` / `void net_poll(void);`

**`include/net_byteorder.h`** - `htons`/`ntohs`/`htonl`/`ntohl` and the
underlying `net_swap16`/`net_swap32` (a swap is its own inverse, so both
directions use the same swap - the separate names exist purely so call
sites read as "to/from wire order").

**`link_layer/ethernet.h`** - `struct eth_header` (wire format) and the
driver-agnostic Ethernet layer.
- `void ethernet_init(void);` / `void ethernet_get_mac(uint8_t mac_out[ETH_ADDR_LEN]);` / `void ethernet_poll(void);`

**`link_layer/arp.h`** - RFC 826 ARP: cache + resolution.
- `void arp_init(void);` / `int arp_resolve(uint32_t ip, uint8_t mac_out[ETH_ADDR_LEN]);`
- `void arp_handle_frame(const uint8_t *payload, uint16_t len, const uint8_t src_mac[ETH_ADDR_LEN]);`

**`internet_layer/ipv4.h`** - Fixed 20-byte IPv4 header (no options),
`IPV4_ADDR(a,b,c,d)` for building an address literal.
- `void ipv4_init(uint32_t local_ip);` / `uint32_t ipv4_get_address(void);`
- `void ipv4_set_gateway(uint32_t gateway_ip, uint32_t subnet_mask);`
- `uint16_t ipv4_checksum(const void *data, uint16_t len);`
- `int ipv4_send(uint32_t dest_ip, uint8_t protocol, const void *payload, uint16_t payload_len);`
- `void ipv4_handle_frame(const uint8_t *frame_payload, uint16_t len, const uint8_t src_mac[6]);`

**`internet_layer/icmp.h`** - RFC 792 echo request/reply; what `ping`
(the app) is built on.
- `void icmp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip);`
- `int icmp_ping(uint32_t dest_ip, uint16_t seq, uint32_t *rtt_ticks_out);`

**`transport_protocols/udp.h`** - RFC 768 UDP.
- `void udp_init(void);` / `int udp_socket_open(uint16_t local_port);` / `void udp_socket_close(int handle);`
- `void udp_handle_packet(const uint8_t *packet, uint16_t len, uint32_t src_ip);`

**`transport_protocols/tcp.h`** - RFC 793 TCP, no options
(`data_offset` always 20 bytes on send).
- `void tcp_init(void);`
- `int tcp_listen(uint16_t local_port);` / `int tcp_accept(int listen_handle);` / `int tcp_connect(uint32_t remote_ip, uint16_t remote_port);`
- `int tcp_send(int handle, const void *data, uint16_t len);` / `uint16_t tcp_recv(int handle, void *buf, uint16_t buf_len);`
- `void tcp_close(int handle);` / `void tcp_poll(void);` / `void tcp_handle_packet(...)`

**`application_layer/dns.h`** - Resolver, defaults to QEMU SLIRP's built-in
DNS proxy.
- `void dns_init(uint32_t resolver_ip);` / `void dns_set_resolver(uint32_t resolver_ip);`
- `int dns_resolve(const char *hostname, uint32_t *ip_out);`

**`application_layer/http.h`** - One blocking HTTP/1.1 GET (what `fetch`
is built on). Always sends `Connection: close` and reads to peer-close
rather than trusting `Content-Length`.
- `int http_get(const char *host, uint16_t port, const char *path, char *body_buf, uint32_t body_buf_len);` - returns the HTTP status code, or a negative value for a failure before any status line arrived (see the header for exactly which negative value means what).

## UI - `kernel/ui/`

**`fb.h`** - Linear framebuffer: `fb_info_t` describes the mode (from the
bootloader), everything else draws into it.
- `void fb_init(void);` / `void fb_putpixel(...)` / `void fb_fillrect(...)` / `void fb_clear(uint32_t color);` / `void fb_swap(void);`
- `void fb_draw_char(...)` / `void fb_draw_string(...)`

**`terminal.h`** - The text-mode terminal drawn on top of the
framebuffer: 80x40 cell grid, `TERMINAL_*` colour constants.
- `void terminal_init(void);` / `void terminal_putchar(char c, uint32_t attr);` / `void terminal_puts(const char *s, uint32_t attr);`
- `void terminal_clear(void);` / `void terminal_set_cursor(...)` / `void terminal_get_cursor(...)` / `void terminal_write_at(...)`
- `void terminal_render(void);` (full redraw) / `void terminal_update(void);` (only when dirty)

**`window.h`** - The window manager's kernel-side half: window pool,
tiling layout (via `kernel/bsp.h`), and the `win_*`/`win_*_id` drawing
primitives a window's own draw callback uses.
- `void wm_init(void);` / `int wm_open_window(wm_entry_t entry, const char *name);` / `int wm_close_window(int window_id);`
- `void wm_relayout(void);` / `void wm_present(void);`
- `void win_fillrect(window_t *win, ...)` / `win_putpixel` / `win_clear` / `win_draw_string` and their `_id`-suffixed equivalents (address a window by its plain integer id instead of a pointer - what `wm_api.h`'s userland syscalls actually use).

**`wm_api.h`** - The window manager's OWN syscall-trampoline table
(deliberately separate from `pexe.h`'s `app_api_t` - a WM needs a
graphics/windowing-shaped surface, not a filesystem/network one). A
window is always addressed by integer `window_id` here, never a kernel
pointer. Current limitation, by design: opening a window from userland
does not give it its own private draw buffer yet - see the header's own
comment.

## Top-level `kernel/`

**`bsp.h`** - Binary space partitioning tree for the bspwm-style tiling
layout. Deliberately hardware/OS-agnostic (no `fb.h`, no `terminal.h`, no
GannetOS-specific headers at all) so it stays testable on the host with
plain `gcc` - see `hosttest/` for exactly that.
- `void bsp_tree_init(bsp_tree_t *t);` / `int bsp_remove(...)` / `void bsp_flip(...)` / `void bsp_rotate(...)` / `void bsp_resize(...)`
- `void bsp_layout(bsp_tree_t *t, const bsp_rect_t *root_rect, bsp_visit_fn visit, void *user_data);` / `int bsp_find(bsp_tree_t *t, int window_id);`

`kernel.c` itself has no header - it's the boot entry point (`kmain`),
wiring every subsystem above together in order. Read it top to bottom for
the actual boot sequence.

---

`filesys/` (`fs.h`, `ata.h`) isn't part of this index yet - see those
headers directly for now.