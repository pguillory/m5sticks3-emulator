/*
 * ESP32-S3 ELF Symbol Patching Infrastructure
 *
 * Intercepts firmware function calls by patching ELF function entry points
 * with SIMCALL trampolines at load time.
 *
 * Trampoline layout (9 bytes):
 *   Offset 0: ENTRY a1, 32   (0x36, 0x41, 0x00)  — standard windowed ABI entry
 *   Offset 3: SIMCALL         (0x00, 0x51, 0x00)  — transfers to host handler
 *   Offset 6: RETW            (0x90, 0x00, 0x00)  — return to caller
 *
 * When the firmware calls a hooked function:
 *   1. CALL8/CALL4 rotates the register window and jumps to the function
 *   2. ENTRY sets up the stack frame (a2-a7 hold the function arguments)
 *   3. SIMCALL triggers the semihosting handler in xtensa-semi.c
 *   4. The handler looks up the SIMCALL PC in the hook table
 *   5. If found, it calls the registered handler with the CPU state
 *   6. The handler reads args from a2-a7, writes return value to a2
 *   7. After SIMCALL returns, RETW returns to the caller
 *
 * Copyright (c) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "hw/xtensa/esp32s3_elf_hooks.h"
#include "exec/address-spaces.h"
#include "cpu.h"

/* Global hook registry */
Esp32s3ElfHooks *esp32s3_elf_hooks_global = NULL;

/*
 * Xtensa instruction bytes for the trampoline.
 *
 * Encoding reference (Xtensa little-endian, 3-byte instructions):
 *
 * ENTRY a1, 32:
 *   BRI12 format: op0=6, op1=3, s=1, imm12=32/8=4
 *   byte0 = (op1<<4)|op0 = 0x36
 *   byte1 = (imm12[3:0]<<4)|s = 0x41
 *   byte2 = imm12[11:4] = 0x00
 *
 * SIMCALL:
 *   RRR format: op0=0, r=5, s=1, t=0, op1=0, op2=0
 *   byte0 = (t<<4)|op0 = 0x00
 *   byte1 = (r<<4)|s = 0x51
 *   byte2 = (op2<<4)|op1 = 0x00
 *
 * RETW:
 *   RRR format: op0=0, r=0, s=0, t=9, op1=0, op2=0
 *   (Actually RETW is in the JR group: t=9, s=0)
 *   byte0 = (t<<4)|op0 = 0x90
 *   byte1 = (r<<4)|s = 0x00
 *   byte2 = 0x00
 */
static const uint8_t trampoline_bytes[ESP32S3_TRAMPOLINE_SIZE] = {
    0x36, 0x41, 0x00,  /* entry a1, 32 */
    0x00, 0x51, 0x00,  /* simcall */
    0x90, 0x00, 0x00,  /* retw */
};


Esp32s3ElfHooks *esp32s3_elf_hooks_new(void)
{
    Esp32s3ElfHooks *hooks = g_new0(Esp32s3ElfHooks, 1);
    return hooks;
}

void esp32s3_elf_hooks_register(Esp32s3ElfHooks *hooks,
                                const char *func_name,
                                esp32s3_elf_hook_fn handler,
                                void *opaque)
{
    if (hooks->count >= ESP32S3_ELF_HOOKS_MAX) {
        qemu_log("[ELF-Hooks] Maximum number of hooks reached (%d)\n",
                 ESP32S3_ELF_HOOKS_MAX);
        return;
    }

    Esp32s3ElfHookEntry *entry = &hooks->entries[hooks->count];
    strncpy(entry->name, func_name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->handler = handler;
    entry->opaque = opaque;
    entry->addr = 0;
    entry->patched = false;
    hooks->count++;
    hooks->sorted = false;

    qemu_log("[ELF-Hooks] Registered hook for '%s'\n", func_name);
}

void esp32s3_elf_hooks_symbol_cb(const char *st_name, int st_info,
                                 uint64_t st_value, uint64_t st_size)
{
    Esp32s3ElfHooks *hooks = esp32s3_elf_hooks_global;
    if (!hooks || !st_name) {
        return;
    }

    /* Only look at function symbols (STT_FUNC = 2) */
    int st_type = st_info & 0xf;
    if (st_type != 2) {  /* STT_FUNC */
        return;
    }

    for (int i = 0; i < hooks->count; i++) {
        if (strcmp(hooks->entries[i].name, st_name) == 0) {
            hooks->entries[i].addr = st_value;
            qemu_log("[ELF-Hooks] Found '%s' at 0x%08" PRIx64
                     " (size=%" PRIu64 ")\n",
                     st_name, st_value, st_size);
            return;
        }
    }
}

/* Comparison function for sorting hooks by address */
static int compare_hook_addrs(const void *a, const void *b)
{
    const Esp32s3ElfHooks *hooks = esp32s3_elf_hooks_global;
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    uint64_t addr_a = hooks->entries[ia].addr;
    uint64_t addr_b = hooks->entries[ib].addr;
    if (addr_a < addr_b) return -1;
    if (addr_a > addr_b) return 1;
    return 0;
}

void esp32s3_elf_hooks_patch(Esp32s3ElfHooks *hooks, void *cpu_opaque)
{
    XtensaCPU *cpu = (XtensaCPU *)cpu_opaque;
    CPUState *cs = CPU(cpu);
    int patched = 0;

    for (int i = 0; i < hooks->count; i++) {
        Esp32s3ElfHookEntry *entry = &hooks->entries[i];
        if (entry->addr == 0) {
            qemu_log("[ELF-Hooks] WARNING: '%s' not found in ELF symbols\n",
                     entry->name);
            continue;
        }

        /* Write the trampoline at the function's address */
        hwaddr phys_addr = cpu_get_phys_page_debug(cs, entry->addr);
        if (phys_addr == (hwaddr)-1) {
            qemu_log("[ELF-Hooks] WARNING: cannot translate address 0x%08"
                     PRIx64 " for '%s'\n", entry->addr, entry->name);
            continue;
        }

        /* Account for page offset */
        hwaddr offset = entry->addr & (TARGET_PAGE_SIZE - 1);
        phys_addr += offset;

        /* Write trampoline bytes to physical memory */
        cpu_physical_memory_write(phys_addr, trampoline_bytes,
                                  ESP32S3_TRAMPOLINE_SIZE);

        entry->patched = true;
        patched++;

        qemu_log("[ELF-Hooks] Patched '%s' at 0x%08" PRIx64
                 " (phys 0x%08" HWADDR_PRIx ")\n",
                 entry->name, entry->addr, phys_addr);
    }

    /* Build sorted index for fast lookup */
    for (int i = 0; i < hooks->count; i++) {
        hooks->sorted_indices[i] = i;
    }
    qsort(hooks->sorted_indices, hooks->count, sizeof(int),
          compare_hook_addrs);
    hooks->sorted = true;

    qemu_log("[ELF-Hooks] Patched %d/%d functions\n", patched, hooks->count);
}

Esp32s3ElfHookEntry *esp32s3_elf_hooks_lookup(Esp32s3ElfHooks *hooks,
                                               uint64_t simcall_pc)
{
    if (!hooks || hooks->count == 0) {
        return NULL;
    }

    /*
     * The SIMCALL instruction is at offset 3 in the trampoline
     * (after the 3-byte ENTRY instruction).
     * The function's entry address = simcall_pc - 3.
     */
    uint64_t func_addr = simcall_pc - 3;

    /* Linear scan (fast enough for < 64 entries) */
    for (int i = 0; i < hooks->count; i++) {
        if (hooks->entries[i].patched && hooks->entries[i].addr == func_addr) {
            return &hooks->entries[i];
        }
    }
    return NULL;
}
