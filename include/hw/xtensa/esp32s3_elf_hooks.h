/*
 * ESP32-S3 ELF Symbol Patching Infrastructure
 *
 * Provides a mechanism to intercept firmware function calls at load time
 * by parsing ELF symbol tables and patching function entry points with
 * trampolines that redirect to emulator-provided implementations.
 *
 * The trampoline uses SIMCALL (Xtensa semihosting instruction) to transfer
 * control to the host-side handler while preserving all function arguments
 * in registers a2-a7.
 *
 * This infrastructure is used for BLE (NimBLE) API interception and can be
 * reused for WiFi API interception in the future.
 *
 * Copyright (c) 2024
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#pragma once

#include "qemu/osdep.h"
#include "hw/loader.h"

/* Maximum number of functions that can be hooked */
#define ESP32S3_ELF_HOOKS_MAX  64

/* Xtensa trampoline: ENTRY a1,32 + SIMCALL + RETW = 9 bytes */
#define ESP32S3_TRAMPOLINE_SIZE 9

/*
 * Register context passed to hook handlers.
 * Provides read/write access to the guest CPU registers without requiring
 * the CPUXtensaState type (avoiding target/ header dependency in hw/).
 *
 * regs[0] = a0, regs[1] = a1, regs[2] = a2, ..., regs[15] = a15
 * Function arguments are in regs[2]-regs[7] (windowed ABI, after ENTRY).
 * Set regs[2] to the return value before returning.
 *
 * cpu_state: opaque pointer to CPUState for memory access APIs
 */
typedef struct Esp32s3ElfHookRegs {
    uint32_t *regs;     /* Pointer to register array (a0-a15+) */
    void *cpu_state;    /* CPUState* for cpu_physical_memory_read/write */
    uint32_t pc;        /* Current program counter */
} Esp32s3ElfHookRegs;

/*
 * Hook handler callback.
 *
 * Called when a hooked function is invoked by the firmware.
 * The handler receives register context and can read/write registers.
 *
 * If the handler returns true, the SIMCALL handler will call cpu_loop_exit()
 * to force re-evaluation of CPU state (e.g., if PC was modified).
 * If false, execution continues normally to the next instruction (RETW).
 */
typedef bool (*esp32s3_elf_hook_fn)(Esp32s3ElfHookRegs *regs, void *opaque);

typedef struct Esp32s3ElfHookEntry {
    char name[128];          /* Symbol name */
    uint64_t addr;           /* Symbol address in firmware */
    esp32s3_elf_hook_fn handler;
    void *opaque;
    bool patched;            /* True if trampoline was written */
} Esp32s3ElfHookEntry;

typedef struct Esp32s3ElfHooks {
    Esp32s3ElfHookEntry entries[ESP32S3_ELF_HOOKS_MAX];
    int count;

    /* Lookup acceleration: sorted by address for binary search */
    int sorted_indices[ESP32S3_ELF_HOOKS_MAX];
    bool sorted;
} Esp32s3ElfHooks;

/* Global hook registry (one per machine) */
extern Esp32s3ElfHooks *esp32s3_elf_hooks_global;

/*
 * Register a function name to be intercepted.
 * Call this BEFORE loading the ELF.
 * The handler will be called when the firmware invokes this function.
 */
void esp32s3_elf_hooks_register(Esp32s3ElfHooks *hooks,
                                const char *func_name,
                                esp32s3_elf_hook_fn handler,
                                void *opaque);

/*
 * ELF symbol callback for use with load_elf_ram_sym().
 * Collects addresses of registered function names.
 */
void esp32s3_elf_hooks_symbol_cb(const char *st_name, int st_info,
                                 uint64_t st_value, uint64_t st_size);

/*
 * After ELF loading, patch all matched functions with trampolines.
 * Must be called after load_elf_ram_sym() and before CPU reset.
 *
 * cpu: the Xtensa CPU to use for address translation
 */
void esp32s3_elf_hooks_patch(Esp32s3ElfHooks *hooks, void *cpu);

/*
 * Look up a hook by the SIMCALL instruction address.
 * The SIMCALL is at offset 3 within the trampoline (after ENTRY).
 * So the function's patched entry address = simcall_pc - 3.
 *
 * Returns the hook entry, or NULL if not found.
 */
Esp32s3ElfHookEntry *esp32s3_elf_hooks_lookup(Esp32s3ElfHooks *hooks,
                                               uint64_t simcall_pc);

/*
 * Create and initialize a new hook registry.
 */
Esp32s3ElfHooks *esp32s3_elf_hooks_new(void);
