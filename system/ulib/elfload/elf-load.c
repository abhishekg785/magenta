// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <elfload/elfload.h>

#include <endian.h>
#include <limits.h>
#include <magenta/syscalls.h>
#include <magenta/syscalls/object.h>

#if BYTE_ORDER == LITTLE_ENDIAN
# define MY_ELFDATA ELFDATA2LSB
#elif BYTE_ORDER == BIG_ENDIAN
# define MY_ELFDATA ELFDATA2MSB
#else
# error what byte order?
#endif

#if defined(__arm__)
# define MY_MACHINE EM_ARM
#elif defined(__aarch64__)
# define MY_MACHINE EM_AARCH64
#elif defined(__x86_64__)
# define MY_MACHINE EM_X86_64
#elif defined(__i386__)
# define MY_MACHINE EM_386
#else
# error what machine?
#endif

// NOTE!  All code in this file must maintain the invariants that it's
// purely position-independent and uses no writable memory other than
// its own stack.

mx_status_t elf_load_prepare(mx_handle_t vmo, elf_load_header_t* header,
                             uintptr_t* phoff) {
    // Read the file header and validate basic format sanity.
    elf_ehdr_t ehdr;
    mx_size_t n;
    mx_status_t status = mx_vmo_read(vmo, &ehdr, 0, sizeof(ehdr), &n);
    if (status < 0)
        return status;
    if (n != sizeof(ehdr) ||
        ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
        ehdr.e_ident[EI_CLASS] != MY_ELFCLASS ||
        ehdr.e_ident[EI_DATA] != MY_ELFDATA ||
        ehdr.e_ident[EI_VERSION] != EV_CURRENT ||
        ehdr.e_phentsize != sizeof(elf_phdr_t) ||
        ehdr.e_phnum == PN_XNUM ||
        ehdr.e_machine != MY_MACHINE ||
        // This code could easily support loading fixed-address ELF files
        // (e_type == ET_EXEC).  But the system overall doesn't support
        // them.  It's Fuchsia policy that all executables must be PIEs.
        // So don't accept ET_EXEC files at all.
        ehdr.e_type != ET_DYN)
        return ERR_ELF_BAD_FORMAT;

    // Cache the few other bits we need from the header, and we're good to go.
    header->e_phnum = ehdr.e_phnum;
    header->e_entry = ehdr.e_entry;
    *phoff = ehdr.e_phoff;
    return NO_ERROR;
}

mx_status_t elf_load_read_phdrs(mx_handle_t vmo, elf_phdr_t phdrs[],
                                uintptr_t phoff, size_t phnum) {
    size_t phdrs_size = (size_t)phnum * sizeof(elf_phdr_t);
    mx_size_t n;
    mx_status_t status = mx_vmo_read(vmo, phdrs, phoff, phdrs_size, &n);
    if (status < 0)
        return status;
    if (n != phdrs_size)
        return ERR_ELF_BAD_FORMAT;
    return NO_ERROR;
}

// An ET_DYN file can be loaded anywhere, so choose where.  This computes
// handle->load_bias, which is the difference between p_vaddr values in
// this file and actual runtime addresses.  (Usually the lowest p_vaddr in
// an ET_DYN file will be 0 and so the load bias is also the load base
// address, but ELF does not require that the lowest p_vaddr be 0.)
static mx_status_t choose_load_bias(mx_handle_t vmar,
                                    const elf_load_header_t* header,
                                    const elf_phdr_t phdrs[],
                                    uintptr_t *bias) {
    // This file can be loaded anywhere, so the first thing is to
    // figure out the total span it will need and reserve a span
    // of address space that big.  The kernel decides where to put it.

    uintptr_t low = 0, high = 0;
    for (uint_fast16_t i = 0; i < header->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_LOAD) {
            uint_fast16_t j = header->e_phnum;
            do {
                --j;
            } while (j > i && phdrs[j].p_type != PT_LOAD);
            low = phdrs[i].p_vaddr & -PAGE_SIZE;
            high = ((phdrs[j].p_vaddr +
                     phdrs[j].p_memsz + PAGE_SIZE - 1) & -PAGE_SIZE);
            break;
        }
    }
    // Sanity check.  ELF requires that PT_LOAD phdrs be sorted in
    // ascending p_vaddr order.
    if (low > high)
        return ERR_ELF_BAD_FORMAT;

    const size_t span = high - low;
    if (span == 0)
        return NO_ERROR;

    // vm_map requires some vm_object handle, so create a dummy one.
    mx_handle_t vmo;
    if (mx_vmo_create(0, 0, &vmo) < 0)
        return ERR_NO_MEMORY;

    // Do a mapping to let the kernel choose an address range.
    // TODO(MG-161): This really ought to be a no-access mapping (PROT_NONE
    // in POSIX terms).  But the kernel currently doesn't allow that, so do
    // a read-only mapping.
    uintptr_t base = 0;
    mx_status_t status = mx_vmar_map(vmar, 0, vmo, 0,
                                     span, MX_VM_FLAG_PERM_READ,
                                     &base);
    mx_handle_close(vmo);
    if (status < 0)
        return ERR_NO_MEMORY;

    // TODO(MG-133): Really we should just leave the no-access mapping in
    // place and let each PT_LOAD mapping overwrite it.  But the kernel
    // currently doesn't allow splitting an existing mapping to overwrite
    // part of it.  So we remove the address-reserving mapping before
    // starting on the actual PT_LOAD mappings.  Since there is no chance
    // of racing with another thread doing mappings in this process,
    // there's no danger of "losing the reservation".
    status = mx_vmar_unmap(vmar, base, span);
    if (status < 0)
        return ERR_NO_MEMORY;

    *bias = base - low;
    return NO_ERROR;
}

// TODO(mcgrathr): Temporary hack to avoid modifying the file VMO.
// This will go away when we have copy-on-write.
static mx_handle_t get_writable_vmo(mx_handle_t vmar_self,
                                    mx_handle_t vmo, size_t data_size,
                                    uintptr_t* file_start,
                                    uintptr_t* file_end) {
    mx_handle_t copy_vmo;
    mx_status_t status = mx_vmo_create(data_size, 0, &copy_vmo);
    if (status < 0)
        return status;
    uintptr_t window = 0;
    status = mx_vmar_map(vmar_self, 0, vmo,
                         *file_start, data_size, MX_VM_FLAG_PERM_READ,
                         &window);
    if (status < 0) {
        mx_handle_close(copy_vmo);
        return status;
    }
    mx_size_t n;
    status = mx_vmo_write(copy_vmo, (void*)window, 0, data_size, &n);
    mx_vmar_unmap(vmar_self, window, data_size);
    if (status < 0) {
        mx_handle_close(copy_vmo);
        return status;
    }
    if (n != data_size) {
        mx_handle_close(copy_vmo);
        return ERR_IO;
    }
    *file_end -= *file_start;
    *file_start = 0;
    return copy_vmo;
}

static mx_status_t finish_load_segment(
    mx_handle_t vmar, mx_handle_t vmo, const elf_phdr_t* ph,
    uintptr_t start, size_t size,
    uintptr_t file_start, uintptr_t file_end, size_t partial_page) {
    const uint32_t flags = MX_VM_FLAG_SPECIFIC |
        ((ph->p_flags & PF_R) ? MX_VM_FLAG_PERM_READ : 0) |
        ((ph->p_flags & PF_W) ? MX_VM_FLAG_PERM_WRITE : 0) |
        ((ph->p_flags & PF_X) ? MX_VM_FLAG_PERM_EXECUTE : 0);

    // Compute the VMAR base so we can calculate offsets to give to map.
    // TODO(teisenbe): This will become unnecessary once this code switches
    // to using sub-regions.
    mx_info_vmar_t vmar_info;
    mx_status_t status = mx_object_get_info(vmar, MX_INFO_VMAR,
                                            &vmar_info, sizeof(vmar_info),
                                            NULL, NULL);
    if (status != NO_ERROR)
        return status;

    if (start < vmar_info.base)
        return ERR_ELF_BAD_FORMAT;

    size_t start_offset = start - vmar_info.base;

    if (ph->p_filesz == ph->p_memsz)
        // Straightforward segment, map all the whole pages from the file.
        return mx_vmar_map(vmar, start_offset, vmo, file_start, size, flags,
                           &start);

    const size_t file_size = file_end - file_start;

    // This segment has some bss, so things are more complicated.
    // Only the leading portion is directly mapped in from the file.
    if (file_size > 0) {
        mx_status_t status = mx_vmar_map(vmar, start_offset, vmo, file_start,
                                         file_size, flags, &start);
        if (status != NO_ERROR)
            return status;

        start_offset = start - vmar_info.base;
        start_offset += file_size;
        size -= file_size;
    }

    // The rest of the segment will be backed by anonymous memory.
    mx_handle_t bss_vmo;
    status = mx_vmo_create(size, 0, &bss_vmo);
    if (status < 0)
        return status;

    // The final partial page of initialized data falls into the
    // region backed by bss_vmo rather than (the file) vmo.  We need
    // to read that data out of the file and copy it into bss_vmo.
    if (partial_page > 0) {
        char buffer[PAGE_SIZE];
        mx_size_t n;
        status = mx_vmo_read(vmo, buffer, file_end, partial_page, &n);
        if (status < 0) {
            mx_handle_close(bss_vmo);
            return status;
        }
        if (n != partial_page) {
            mx_handle_close(bss_vmo);
            return ERR_ELF_BAD_FORMAT;
        }
        status = mx_vmo_write(bss_vmo, buffer, 0, n, &n);
        if (status < 0) {
            mx_handle_close(bss_vmo);
            return status;
        }
        if (n != partial_page) {
            mx_handle_close(bss_vmo);
            return ERR_IO;
        }
    }

    status = mx_vmar_map(vmar, start_offset, bss_vmo, 0, size, flags, &start);
    mx_handle_close(bss_vmo);

    return status;
}

static mx_status_t load_segment(mx_handle_t vmar_self,
                                mx_handle_t vmar, mx_handle_t vmo,
                                uintptr_t bias, const elf_phdr_t* ph) {
    // The p_vaddr can start in the middle of a page, but the
    // semantics are that all the whole pages containing the
    // p_vaddr+p_filesz range are mapped in.
    uintptr_t start = (uintptr_t)ph->p_vaddr + bias;
    uintptr_t end = start + ph->p_memsz;
    start &= -PAGE_SIZE;
    end = (end + PAGE_SIZE - 1) & -PAGE_SIZE;
    size_t size = end - start;

    // Nothing to do for an empty segment (degenerate case).
    if (size == 0)
        return NO_ERROR;

    uintptr_t file_start = (uintptr_t)ph->p_offset;
    uintptr_t file_end = file_start + ph->p_filesz;
    const size_t partial_page = file_end & (PAGE_SIZE - 1);
    file_start &= -PAGE_SIZE;
    file_end &= -PAGE_SIZE;

    uintptr_t data_end =
        (ph->p_offset + ph->p_filesz + PAGE_SIZE - 1) & -PAGE_SIZE;
    const size_t data_size = data_end - file_start;

    // With no writable data, it's the simple case.
    if (!(ph->p_flags & PF_W) || data_size == 0)
        return finish_load_segment(vmar, vmo, ph, start, size,
                                   file_start, file_end, partial_page);

    // For a writable segment, we need a writable VMO.
    mx_handle_t writable_vmo = get_writable_vmo(vmar_self, vmo, data_size,
                                                &file_start, &file_end);
    if (writable_vmo < 0)
        return writable_vmo;
    mx_status_t status = finish_load_segment(vmar, writable_vmo, ph,
                                             start, size, file_start,
                                             file_end, partial_page);
    mx_handle_close(writable_vmo);
    return status;
}

mx_status_t elf_load_map_segments(mx_handle_t vmar_self, mx_handle_t vmar,
                                  const elf_load_header_t* header,
                                  const elf_phdr_t phdrs[],
                                  mx_handle_t vmo,
                                  mx_vaddr_t* base, mx_vaddr_t* entry) {
    uintptr_t bias = 0;
    mx_status_t status = choose_load_bias(vmar, header, phdrs, &bias);

    for (uint_fast16_t i = 0; status == NO_ERROR && i < header->e_phnum; ++i) {
        if (phdrs[i].p_type == PT_LOAD)
            status = load_segment(vmar_self, vmar, vmo, bias, &phdrs[i]);
    }

    if (status == NO_ERROR) {
        if (base != NULL)
            *base = bias;
        if (entry != NULL)
            *entry = header->e_entry != 0 ? header->e_entry + bias : 0;
    }
    return status;
}

bool elf_load_find_interp(const elf_phdr_t phdrs[], size_t phnum,
                          uintptr_t* interp_off, size_t* interp_len) {
    for (size_t i = 0; i < phnum; ++i) {
        if (phdrs[i].p_type == PT_INTERP) {
            *interp_off = phdrs[i].p_offset;
            *interp_len = phdrs[i].p_filesz;
            return true;
        }
    }
    return false;
}
