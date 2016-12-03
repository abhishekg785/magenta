// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <mx/handle.h>
#include <mx/vmo.h>

namespace mx {

class vmar : public handle<vmar> {
public:
    vmar() = default;

    explicit vmar(mx_handle_t value) : handle(value) {}

    explicit vmar(handle<void>&& h) : handle(h.release()) {}

    vmar(vmar&& other) : vmar(other.release()) {}

    vmar& operator=(vmar&& other) {
        reset(other.release());
        return *this;
    }

    static inline const vmar& root_self() {
        return *reinterpret_cast<vmar*>(&__magenta_vmar_root_self);
    }

    mx_status_t map(mx_size_t vmar_offset, const vmo& vmo_handle, uint64_t vmo_offset,
                    mx_size_t len, uint32_t flags, uintptr_t* ptr) const {
        return mx_vmar_map(get(), vmar_offset, vmo_handle.get(), vmo_offset, len, flags, ptr);
    }

    mx_status_t unmap(uintptr_t address, mx_size_t len) const {
        return mx_vmar_unmap(get(), address, len);
    }

    mx_status_t protect(uintptr_t address, mx_size_t len, uint32_t prot) const {
        return mx_vmar_protect(get(), address, len, prot);
    }

    mx_status_t destroy() const {
        return mx_vmar_destroy(get());
    }

    mx_status_t allocate(mx_size_t offset, mx_size_t size, uint32_t flags,
                         vmar* child, uintptr_t* child_addr) const;
};

} // namespace mx
