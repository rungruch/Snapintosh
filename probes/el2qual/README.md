# el2qual

el2qual is the Phase 0A CPU qualification, built as a UEFI application. The
header comment of `el2qual.c` says what it measures and what it never
touches. It is built with `python3 probes/el2qual/build.py`, and assembled
onto a stick with `python3 tools/el2qual-usb.py`
([docs/GAOKUN3-QUALIFICATION.md](../../docs/GAOKUN3-QUALIFICATION.md)).

## Load options

Space separated, in any order, from the boot entry's `options` line
(`parse_options` in `el2qual.c`). Every measurement beyond the CPU's own is
off by default. Options marked "test only" exist for QEMU runs, and the
report says so when one is in effect.

| Option | Effect |
| --- | --- |
| `serial` | Also write the report to the SPCR UART (PL011/SBSA only). |
| `poweroff` | PSCI `SYSTEM_OFF` after reporting (QEMU runs). |
| `nopac` | Skip every PAuth key access and PAC instruction. |
| `nosmp` | Measure the boot CPU only. |
| `emulate-vhe` | Recreate slbounce's E2H\|TGE handoff first. Test only, and refused unless the machine started at EL2 without E2H. |
| `exit-vhe` | Leave slbounce's E2H\|TGE state for the non-VHE EL2 that q1n1 and the monitor assume (`vhe.c` plans it, `el2q_exit_vhe` switches), then measure there. Refused, with the reason reported, if the entry state is not one the plan recognises. |
| `emulate-rotated` | Treat the firmware's GOP mode as having no framebuffer address, as the gaokun3's does. Test only. |
| `emulate-no-fb` | Treat every GOP mode that way. Test only. |
| `emulate-dt-fb` | Describe the firmware's real GOP framebuffer as if it were the device tree's `/chosen` simple-framebuffer. Test only. |
| `thermal` | After the CPU measurements, run the SC8280XP tsens guard (`thermal.c`) on the boot CPU for 12 s. Read only, and only the guard's 58 words, each checked against the firmware's page tables before the first read. |
| `usb` | Read whether the gaokun3's USB0 controller is powered, from the clock controller's status words, before and after ExitBootServices. After it, and only if powered and clocked, also read the controller's own state (`usbprobe.c`). Read only. |
| `usb-smmu` | `usb`, then also apps_smmu's state for USB0's stream. When it translates, also read the context bank it names, and the stage-2 bank that one nests, if any. |
| `usb-smmu-map` | `usb-smmu`, but first add the SMMU's 1 MiB register window to el2qual's own EL2 tables as Device, read-only identity pages (`pagemap.c`), so the nested stage-2 bank the UEFI tables leave unmapped can be read too. Writes RAM (one table page of el2qual's own and one descriptor in UEFI's leftover tables), never hardware. |
| `emulate-map-hole` | With `usb-smmu-map`, map two test windows QEMU's EDK2 leaves unmapped instead of the SMMU's, so the map is written and checked there. Test only. |
| `usb-s2-walk` | `usb-smmu`, then walk the stage-2 bank's tables for a 1 MiB DMA arena allocated below 4 GiB as q1n1 allocates its own (`s2walk.c`). Descriptor reads only, through mappings UEFI's tables already have, stopping at the first fault or SError. |
| `usb-s2-survey` | `usb-s2-walk`, then survey that stage 2's top two levels: every level-1 entry, and the level-2 table under the arena (`s2walk.c`), with the walk's guards. |
| `usb-s2-fixed` | `usb-s2-walk`, but walk the fixed range `0xC8000000`–`0xC80FFFFF` instead of an allocated arena, and report UEFI's memory map (as accepted by ExitBootServices) over `0xC6200000`–`0xC85FFFFF`, the identity region run 23 found. Nothing at the range is allocated or touched. |
| `usb-s2-bypass-plan` | `usb-smmu`, then work out the one-field change to `S2CR(6)` that would give USB0's stream untranslated DMA, and report it (`s2bypass.c`). Reads the SMMU's global registers to confirm the machine is still in the state the plan assumes, including that no second group matches the stream. **Writes nothing**: the module has no write callback, so no path through it reaches the SMMU. |
| `emulate-s2-tables` | With `usb-s2-walk`, walk a stage-2 table set el2qual builds for its arena instead of the SMMU's, every page identity read-write. With `usb-s2-fixed`, the memory map is reported over 36 MiB from QEMU virt's RAM base, `0x40000000`, instead. Test only. |
| `emulate-s2-odd` | The same, with arena pages 1 to 3 moved, invalid and read-only. Test only. |
| `emulate-s2-device` | The same tables, but with the root pointed at QEMU's PL011, a Device page the walk must refuse unread. Test only. |
| `usb-halt` | `usb`, then read USB0's xHCI `USBCMD` and `USBSTS` from the operational base the controller's own `CAPLENGTH` gives, and report whether it is halted (`usbprobe.c`). Read only. |
| `emulate-usb-halt` | The halt read against a page el2qual owns, standing in for the controller (`CAPLENGTH` `0x30`, halted), through the real mapping check and guarded read with only the Device-attribute requirement relaxed; the line is marked `(TEST)`. Test only. |
| `emulate-usb-halt-running` | The same, with the stand-in running. Test only. |
| `rng` | Before ExitBootServices, look for UEFI's RNG protocol, list its algorithms, and read 256 bytes twice from the default algorithm, and from `RAW` when it is listed; report whether they are non-zero and differ (`rng.c`; Phase 3 step P7). Read only. |

What each USB option is for, and what it found on the gaokun3, is in
[docs/GAOKUN3-USB-DEVICE-MODE.md](../../docs/GAOKUN3-USB-DEVICE-MODE.md).
