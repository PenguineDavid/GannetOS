# GannetOS TODO

## How to use this list

- Pick a task that matches the current priority before starting a new feature.
- Keep changes small enough to review and test independently.
- Update this file when a task is completed or its scope changes.
- Add a host test when a change affects logic that can run outside QEMU.

### Known bugs fixed this pass -- add a regression test so they can't come back silently

- [ ] Add a hosttest for `filesys/fs.c`'s `BITMAP_WORDS` sizing: assert it's large enough to cover `FS_TOTAL_SECTORS - FS_DATA_START` bits, so a future change to either constant can't silently reintroduce the out-of-bounds bitmap write.
- [ ] Add a hosttest for `apps/write.c`'s multi-word argument handling (`write file.txt hello world` must write "hello world", not just "hello").
- [ ] Add a hosttest for `tools/fmtcheck.c`'s `FS_SUPER_SECTOR` staying in sync with `filesys/fs.h` and `tools/fswrite.c` -- it silently drifted out of sync once already.

### Networking -- documented gaps, not yet fixed

- [ ] `kernel/drivers/rtl8139/rtl8139.c`: enable and handle the IMR error bits (RXOVW, TER, etc.) -- a card-side error currently goes completely unnoticed instead of logged.
- [ ] `kernel/drivers/rtl8139/rtl8139.c`: check the RX packet status word's ROK bit before trusting a received frame, instead of ignoring it entirely.
- [ ] `kernel/drivers/net/transport_protocols/udp.c`: send an ICMP port-unreachable reply when a datagram arrives for a port nothing is bound to (currently silently dropped).
- [ ] `kernel/drivers/net/internet_layer/icmp.c`: handle at least destination-unreachable and time-exceeded rather than silently dropping every ICMP type except echo request/reply.
- [ ] `kernel/drivers/net/internet_layer/ipv4.c`: no fragmentation or reassembly -- fine on a local QEMU link, worth flagging if this ever talks to a network with a smaller MTU somewhere in the path.
- [ ] `kernel/drivers/net/transport_protocols/tcp.c`: stop-and-wait only, no real sliding window, no TIME_WAIT, no out-of-order reassembly -- documented and intentional for now, but throughput-limiting if anything larger than small local transfers is ever.expected
- [ ] `kernel/drivers/net/application_layer/dns.c`: a single query with no retry -- one lost UDP packet means a full timeout with no second attempt.
- [ ] `kernel/drivers/net/application_layer/http.c`: no `Transfer-Encoding: chunked` support -- only works cleanly against servers that either send `Content-Length` or close the connection when done.
- [ ] Add NIC support for more device.
- [ ] Add TSS.
- [ ] Add SSH.

### Housekeeping

- [ ] `filesys/fs.c` does a linear `O(FS_MAX_INODES)` scan for most operations (`fs_find`, `fs_mkdir`'s free-slot search, `fs_rmdir`'s child check) -- fine at the current `FS_MAX_INODES`, worth revisiting if that number grows.
- [ ] No filesystem consistency checker -- an interrupted write mid-allocation currently has no recovery path beyond what the bitmap rebuild in `fs_init()` already does from inode `block_count`/`start_block`.
- [ ] `shell/shell.c`'s `wm stop` is informational only (real stop is the F12 hotkey) -- either wire up a real programmatic stop path or remove the subcommand so it doesn't imply one exists.

### Far Future Scope

- [ ] Add a user system, login system and rwx privilege system.
- [ ] Add x86_64 support this is a very big task.

### MISC

- [ ] Add audio drivers.