// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <err.h>
#include <platform.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <trace.h>

#include <dev/interrupt.h>
#include <dev/udisplay.h>
#include <kernel/vm.h>
#include <kernel/vm/vm_object.h>
#include <lib/user_copy.h>
#include <lib/user_copy/user_ptr.h>

#include <magenta/interrupt_dispatcher.h>
#include <magenta/interrupt_event_dispatcher.h>
#include <magenta/io_mapping_dispatcher.h>
#include <magenta/magenta.h>
#include <magenta/process_dispatcher.h>
#include <magenta/syscalls/pci.h>
#include <magenta/user_copy.h>
#include <magenta/vm_object_dispatcher.h>

#include "syscalls_priv.h"

#define LOCAL_TRACE 0

static_assert(MX_CACHE_POLICY_CACHED == ARCH_MMU_FLAG_CACHED,
              "Cache policy constant mismatch - CACHED");
static_assert(MX_CACHE_POLICY_UNCACHED == ARCH_MMU_FLAG_UNCACHED,
              "Cache policy constant mismatch - UNCACHED");
static_assert(MX_CACHE_POLICY_UNCACHED_DEVICE == ARCH_MMU_FLAG_UNCACHED_DEVICE,
              "Cache policy constant mismatch - UNCACHED_DEVICE");
static_assert(MX_CACHE_POLICY_WRITE_COMBINING == ARCH_MMU_FLAG_WRITE_COMBINING,
              "Cache policy constant mismatch - WRITE_COMBINING");

// HACK: move the mmio mappings to a high address to get out of the way of DSOs and other user data.
// Will go away once these mappings move into a generic VMO map call.
static const vaddr_t mmio_map_base_address =
#if _LP64
    0x7ff0'0000'0000ULL;
#else
    0x20000000UL;
#endif

mx_handle_t sys_interrupt_create(mx_handle_t hrsrc, uint32_t vector, uint32_t flags) {
    LTRACEF("vector %u flags 0x%x\n", vector, flags);

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    status_t result = InterruptEventDispatcher::Create(vector, flags, &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));

    auto up = ProcessDispatcher::GetCurrent();
    mx_handle_t hv = up->MapHandleToValue(handle.get());
    up->AddHandle(mxtl::move(handle));
    return hv;
}

mx_status_t sys_interrupt_complete(mx_handle_t handle_value) {
    LTRACEF("handle %d\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<InterruptDispatcher> interrupt;
    mx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != NO_ERROR)
        return status;

    return interrupt->InterruptComplete();
}

mx_status_t sys_interrupt_wait(mx_handle_t handle_value) {
    LTRACEF("handle %d\n", handle_value);

    auto up = ProcessDispatcher::GetCurrent();
    mxtl::RefPtr<InterruptDispatcher> interrupt;
    mx_status_t status = up->GetDispatcher(handle_value, &interrupt);
    if (status != NO_ERROR)
        return status;

    return interrupt->WaitForInterrupt();
}


mx_status_t sys_mmap_device_memory(mx_handle_t hrsrc, uintptr_t paddr, uint32_t len,
                                   mx_cache_policy_t cache_policy,
                                   user_ptr<void*> out_vaddr) {

    LTRACEF("addr %#" PRIxPTR " len %#x\n", paddr, len);

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    if (!out_vaddr)
        return ERR_INVALID_ARGS;

    void* vaddr = (void *)mmio_map_base_address;
    uint arch_mmu_flags =
        ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE |
        ARCH_MMU_FLAG_PERM_USER;

    switch (cache_policy) {
        case MX_CACHE_POLICY_CACHED:
            arch_mmu_flags |= ARCH_MMU_FLAG_CACHED;
            break;
        case MX_CACHE_POLICY_UNCACHED:
            arch_mmu_flags |= ARCH_MMU_FLAG_UNCACHED;
            break;
        case MX_CACHE_POLICY_UNCACHED_DEVICE:
            arch_mmu_flags |= ARCH_MMU_FLAG_UNCACHED_DEVICE;
            break;
        case MX_CACHE_POLICY_WRITE_COMBINING:
            arch_mmu_flags |= ARCH_MMU_FLAG_WRITE_COMBINING;
            break;
        default: return ERR_INVALID_ARGS;
    }

    auto aspace = ProcessDispatcher::GetCurrent()->aspace();
    status_t res = aspace->AllocPhysical("user_mmio", len, &vaddr,
                                         PAGE_SIZE_SHIFT, 0, (paddr_t)paddr,
                                         VMM_FLAG_VALLOC_BASE,  // vmm flags
                                         arch_mmu_flags);

    if (res != NO_ERROR)
        return res;

    if (out_vaddr.copy_to_user(vaddr) != NO_ERROR) {
        aspace->FreeRegion(reinterpret_cast<vaddr_t>(vaddr));
        return ERR_INVALID_ARGS;
    }

    return NO_ERROR;
}

mx_status_t sys_vmo_create_contiguous(mx_handle_t hrsrc, size_t size,
                                      user_ptr<mx_handle_t> out) {
    LTRACEF("size 0x%zu\n", size);

    if (size == 0) return ERR_INVALID_ARGS;

    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    size = ROUNDUP_PAGE_SIZE(size);
    // create a vm object
    mxtl::RefPtr<VmObject> vmo = VmObjectPaged::Create(PMM_ALLOC_FLAG_ANY, size);
    if (!vmo)
        return ERR_NO_MEMORY;

    // always immediately commit memory to the object
    uint64_t committed;
    status = vmo->CommitRangeContiguous(0, size, &committed, PAGE_SIZE_SHIFT);
    if (status < 0 || (size_t)committed < size) {
        LTRACEF("failed to allocate enough pages (asked for %zu, got %zu)\n", size / PAGE_SIZE,
                (size_t)committed / PAGE_SIZE);
        return ERR_NO_MEMORY;
    }

    // create a Vm Object dispatcher
    mxtl::RefPtr<Dispatcher> dispatcher;
    mx_rights_t rights;
    mx_status_t result = VmObjectDispatcher::Create(mxtl::move(vmo), &dispatcher, &rights);
    if (result != NO_ERROR)
        return result;

    // create a handle and attach the dispatcher to it
    HandleUniquePtr handle(MakeHandle(mxtl::move(dispatcher), rights));
    if (!handle)
        return ERR_NO_MEMORY;

    auto up = ProcessDispatcher::GetCurrent();

    if (out.copy_to_user(up->MapHandleToValue(handle.get())) != NO_ERROR)
        return ERR_INVALID_ARGS;

    up->AddHandle(mxtl::move(handle));
    return NO_ERROR;
}

#if ARCH_X86
extern uint32_t bootloader_fb_base;
extern uint32_t bootloader_fb_width;
extern uint32_t bootloader_fb_height;
extern uint32_t bootloader_fb_stride;
extern uint32_t bootloader_fb_format;
#endif

mx_status_t sys_bootloader_fb_get_info(uint32_t* format, uint32_t* width, uint32_t* height, uint32_t* stride) {
#if ARCH_X86
    if (!bootloader_fb_base || copy_to_user_u32_unsafe(format, bootloader_fb_format) ||
            copy_to_user_u32_unsafe(width, bootloader_fb_width) ||
            copy_to_user_u32_unsafe(height, bootloader_fb_height) ||
            copy_to_user_u32_unsafe(stride, bootloader_fb_stride)) {
        return ERR_INVALID_ARGS;
    } else {
        return NO_ERROR;
    }
#else
    return ERR_NOT_SUPPORTED;
#endif
}

mx_status_t sys_set_framebuffer(mx_handle_t hrsrc, void* vaddr, uint32_t len, uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    intptr_t paddr = vaddr_to_paddr(vaddr);
    udisplay_set_framebuffer(paddr, vaddr, len);

    struct display_info di;
    memset(&di, 0, sizeof(struct display_info));
    di.format = format;
    di.width = width;
    di.height = height;
    di.stride = stride;
    di.flags = DISPLAY_FLAG_HW_FRAMEBUFFER;
    udisplay_set_display_info(&di);

    return NO_ERROR;
}

/**
 * Gets info about an I/O mapping object.
 * @param handle Handle associated with an I/O mapping object.
 * @param out_vaddr Mapped virtual address for the I/O range.
 * @param out_len Mapped size of the I/O range.
 */
mx_status_t sys_io_mapping_get_info(mx_handle_t handle,
                                    user_ptr<uintptr_t> out_vaddr,
                                    user_ptr<uint64_t> out_size) {
    LTRACEF("handle %d\n", handle);

    if (!out_vaddr || !out_size)
        return ERR_INVALID_ARGS;

    auto up = ProcessDispatcher::GetCurrent();

    mxtl::RefPtr<IoMappingDispatcher> io_mapping;
    mx_status_t status = up->GetDispatcher(handle, &io_mapping, MX_RIGHT_READ);
    if (status != NO_ERROR)
        return status;

    // If we do not have read rights, or we are calling from a different address
    // space than the one that this mapping exists in, refuse to tell the user
    // the vaddr/len of the mapping.
    if (ProcessDispatcher::GetCurrent()->aspace() != io_mapping->aspace())
        return ERR_ACCESS_DENIED;

    uintptr_t vaddr = reinterpret_cast<uintptr_t>(io_mapping->vaddr());
    uint64_t  size  = io_mapping->size();

    status = out_vaddr.copy_to_user(vaddr);
    if (status != NO_ERROR)
        return status;

    return out_size.copy_to_user(size);
}

#if ARCH_X86
#include <arch/x86/descriptor.h>
#include <arch/x86/ioport.h>

mx_status_t sys_mmap_device_io(mx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }

    LTRACEF("addr 0x%x len 0x%x\n", io_addr, len);

    return x86_set_io_bitmap(io_addr, len, 1);
}
#else
mx_status_t sys_mmap_device_io(mx_handle_t hrsrc, uint32_t io_addr, uint32_t len) {
    // doesn't make sense on non-x86
    return ERR_NOT_SUPPORTED;
}
#endif

uint32_t sys_acpi_uefi_rsdp(mx_handle_t hrsrc) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }
#if ARCH_X86
    extern uint32_t bootloader_acpi_rsdp;
    return bootloader_acpi_rsdp;
#endif
    return 0;
}

mx_status_t sys_acpi_cache_flush(mx_handle_t hrsrc) {
    // TODO: finer grained validation
    mx_status_t status;
    if ((status = validate_resource_handle(hrsrc)) < 0) {
        return status;
    }
    // TODO(teisenbe): This should be restricted to when interrupts are
    // disabled, but we haven't added support for letting the ACPI process
    // disable interrupts yet.  It only uses this for S-state transitions
    // like poweroff and (more importantly) sleep.
#if ARCH_X86
    __asm__ volatile ("wbinvd");
    return NO_ERROR;
#else
    return ERR_NOT_SUPPORTED;
#endif
}
