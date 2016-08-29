// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#pragma once

#include <threads.h>

// Copy of the kernel's autolock class to help virtio.
// TODO: move this into some shared space
class AutoLock {
public:
    AutoLock(mtx_t* mutex)
        : mutex_(mutex) {
        mtx_lock(mutex_);
    }

    AutoLock(mtx_t& mutex)
        : AutoLock(&mutex) {}

    ~AutoLock() {
        release();
    }

    // early release the mutex before the object goes out of scope
    void release() {
        if (mutex_) {
            mtx_unlock(mutex_);
            mutex_ = nullptr;
        }
    }

    // suppress default constructors
    AutoLock(const AutoLock& am) = delete;
    AutoLock& operator=(const AutoLock& am) = delete;
    AutoLock(AutoLock&& c) = delete;
    AutoLock& operator=(AutoLock&& c) = delete;

private:
    mtx_t* mutex_;
};
