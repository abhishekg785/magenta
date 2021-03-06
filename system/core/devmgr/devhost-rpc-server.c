// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "devhost.h"
#include "device-internal.h"

#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <ddk/completion.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/ioctl.h>
#include <ddk/iotxn.h>
#include <ddk/protocol/device.h>

#include <magenta/processargs.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>

#include <mxio/debug.h>
#include <mxio/vfs.h>

#define MXDEBUG 0

#define CAN_WRITE(ios) (((03 & ios->flags) == O_RDWR) || ((03 & ios->flags) == O_WRONLY))
#define CAN_READ(ios) (((03 & ios->flags) == O_RDWR) || ((03 & ios->flags) == O_RDONLY))

mxio_dispatcher_t* devhost_rio_dispatcher;

devhost_iostate_t* create_devhost_iostate(mx_device_t* dev) {
    devhost_iostate_t* ios;
    if ((ios = calloc(1, sizeof(devhost_iostate_t))) == NULL) {
        return NULL;
    }
    ios->dev = dev;
    mtx_init(&ios->lock, mtx_plain);
    return ios;
}

mx_status_t __mxrio_clone(mx_handle_t h, mx_handle_t* handles, uint32_t* types);

static mx_status_t devhost_get_handles(mx_device_t* dev, const char* path,
                                       uint32_t flags, mx_handle_t* handles,
                                       uint32_t* ids) {
    mx_status_t r;
    devhost_iostate_t* newios;

    if ((newios = create_devhost_iostate(dev)) == NULL) {
        return ERR_NO_MEMORY;
    }
    newios->flags = flags;

    mx_handle_t h0, h1;
    if ((r = mx_channel_create(0, &h0, &h1)) < 0) {
        free(newios);
        return r;
    }
    handles[0] = h0;
    ids[0] = MX_HND_TYPE_MXIO_REMOTE;

    if ((r = device_openat(dev, &dev, path, flags)) < 0) {
        printf("devhost_get_handles(%p:%s) open path='%s', r=%d\n",
               dev, dev->name, path ? path : "", r);
        goto fail1;
    }
    newios->dev = dev;

    if (dev->event > 0) {
        //TODO: read only?
        if ((r = mx_handle_duplicate(dev->event, MX_RIGHT_SAME_RIGHTS, &handles[1])) < 0) {
            goto fail2;
        }
        ids[1] = MX_HND_TYPE_MXIO_REMOTE;
        r = 2;
    } else {
        r = 1;
    }

    mxio_dispatcher_add(devhost_rio_dispatcher, h1, devhost_rio_handler, newios);
    return r;

fail2:
    device_close(dev, flags);
fail1:
    mx_handle_close(h0);
    mx_handle_close(h1);
    free(newios);
    return r;
}

mx_status_t txn_handoff_clone(mx_handle_t srv, mx_handle_t rh) {
    mxrio_msg_t msg;
    memset(&msg, 0, MXRIO_HDR_SZ);
    msg.op = MXRIO_CLONE;
    return mxrio_txn_handoff(srv, rh, &msg);
}

static void sync_io_complete(iotxn_t* txn, void* cookie) {
    completion_signal((completion_t*)cookie);
}

static ssize_t do_sync_io(mx_device_t* dev, uint32_t opcode, void* buf, size_t count, mx_off_t off) {
    iotxn_t* txn;
    mx_status_t status = iotxn_alloc(&txn, 0, MXIO_CHUNK_SIZE, 0);
    if (status != NO_ERROR) {
        return status;
    }

    assert(count <= MXIO_CHUNK_SIZE);

    completion_t completion = COMPLETION_INIT;

    txn->opcode = opcode;
    txn->offset = off;
    txn->length = count;
    txn->complete_cb = sync_io_complete;
    txn->cookie = &completion;

    // if write, write the data to the iotxn
    if (opcode == IOTXN_OP_WRITE) {
        txn->ops->copyto(txn, buf, txn->length, 0);
    }

    dev->ops->iotxn_queue(dev, txn);
    completion_wait(&completion, MX_TIME_INFINITE);

    if (txn->status != NO_ERROR) {
        size_t txn_status = txn->status;
        txn->ops->release(txn);
        return txn_status;
    }

    // if read, get the data
    if (opcode == IOTXN_OP_READ) {
        txn->ops->copyfrom(txn, buf, txn->actual, 0);
    }

    ssize_t actual = txn->actual;
    txn->ops->release(txn);
    return actual;
}

static ssize_t do_ioctl(mx_device_t* dev, uint32_t op, const void* in_buf, size_t in_len, void* out_buf, size_t out_len) {
    mx_status_t r;
    switch (op) {
    case IOCTL_DEVICE_BIND: {
        const char* drv = in_len > 0 ? (const char*)in_buf : NULL;
        r = device_bind(dev, drv);
        break;
    }
    case IOCTL_DEVICE_GET_EVENT_HANDLE: {
        if (out_len < sizeof(mx_handle_t)) {
            r = ERR_BUFFER_TOO_SMALL;
        } else {
            mx_handle_t* event = out_buf;
            r = mx_handle_duplicate(dev->event, MX_RIGHT_DUPLICATE | MX_RIGHT_TRANSFER | MX_RIGHT_READ, event);
            if (r == NO_ERROR) {
                r = sizeof(mx_handle_t);
            }
        }
        break;
    }
    case IOCTL_DEVICE_GET_DRIVER_NAME: {
        if (!dev->driver) {
            r = ERR_NOT_SUPPORTED;
        } else if (!out_buf) {
            r = ERR_INVALID_ARGS;
        } else {
            r = strlen(dev->driver->name);
            if (out_len < (size_t)r) {
                r = ERR_BUFFER_TOO_SMALL;
            } else {
                strncpy(out_buf, dev->driver->name, r);
            }
        }
        break;
    }
    case IOCTL_DEVICE_GET_DEVICE_NAME: {
        if (!out_buf) {
            r = ERR_INVALID_ARGS;
        } else {
            r = strlen(dev->name);
            if (out_len < (size_t)r) {
                r = ERR_BUFFER_TOO_SMALL;
            } else {
                strncpy(out_buf, dev->name, r);
            }
        }
        break;
    }
    case IOCTL_DEVICE_DEBUG_SUSPEND: {
        r = dev->ops->suspend(dev);
        break;
    }
    case IOCTL_DEVICE_DEBUG_RESUME: {
        r = dev->ops->resume(dev);
        break;
    }
    default:
        r = dev->ops->ioctl(dev, op, in_buf, in_len, out_buf, out_len);
    }
    return r;
}

static mx_status_t _devhost_rio_handler(mxrio_msg_t* msg, mx_handle_t rh,
                                        devhost_iostate_t* ios, bool* should_free_ios) {
    mx_device_t* dev = ios->dev;
    uint32_t len = msg->datalen;
    int32_t arg = msg->arg;
    msg->datalen = 0;

    *should_free_ios = false;

    // only IOCTL txns are prepared to deal with inbound handles
    if (msg->hcount && (MXRIO_OP(msg->op) != MXRIO_IOCTL)) {
        for (unsigned i = 0; i < msg->hcount; i++) {
            mx_handle_close(msg->handle[i]);
        }
        msg->hcount = 0;
    }

    switch (MXRIO_OP(msg->op)) {
    case MXRIO_CLOSE:
        device_close(dev, ios->flags);
        *should_free_ios = true;
        return NO_ERROR;
    case MXRIO_OPEN:
        if ((len < 1) || (len > 1024)) {
            return ERR_INVALID_ARGS;
        }
        msg->data[len] = 0;
        // fallthrough
    case MXRIO_CLONE: {
        uint32_t ids[VFS_MAX_HANDLES];
        mx_status_t r;
        char* path = NULL;
        uint32_t flags = arg;
        if (MXRIO_OP(msg->op) == MXRIO_OPEN) {
            xprintf("devhost_rio_handler() open dev %p name '%s' at '%s'\n",
                    dev, dev->name, (char*) msg->data);
            if (strcmp((char*)msg->data, ".")) {
                path = (char*) msg->data;
            }
        } else {
            xprintf("devhost_rio_handler() clone dev %p name '%s'\n", dev, dev->name);
            flags = ios->flags;
        }
        r = devhost_get_handles(dev, path, flags, msg->handle, ids);
        if (r < 0) {
            return r;
        }
        msg->arg2.protocol = MXIO_PROTOCOL_REMOTE;
        msg->hcount = r;
        return NO_ERROR;
    }
    case MXRIO_READ: {
        if (!CAN_READ(ios)) {
            return ERR_ACCESS_DENIED;
        }
        mx_status_t r = do_sync_io(dev, IOTXN_OP_READ, msg->data, arg, ios->io_off);
        if (r >= 0) {
            ios->io_off += r;
            msg->arg2.off = ios->io_off;
            msg->datalen = r;
        }
        return r;
    }
    case MXRIO_READ_AT: {
        if (!CAN_READ(ios)) {
            return ERR_ACCESS_DENIED;
        }
        mx_status_t r = do_sync_io(dev, IOTXN_OP_READ, msg->data, arg, msg->arg2.off);
        if (r >= 0) {
            msg->datalen = r;
        }
        return r;
    }
    case MXRIO_WRITE: {
        if (!CAN_WRITE(ios)) {
            return ERR_ACCESS_DENIED;
        }
        mx_status_t r = do_sync_io(dev, IOTXN_OP_WRITE, msg->data, len, ios->io_off);
        if (r >= 0) {
            ios->io_off += r;
            msg->arg2.off = ios->io_off;
        }
        return r;
    }
    case MXRIO_WRITE_AT: {
        if (!CAN_WRITE(ios)) {
            return ERR_ACCESS_DENIED;
        }
        mx_status_t r = do_sync_io(dev, IOTXN_OP_WRITE, msg->data, len, msg->arg2.off);
        return r;
    }
    case MXRIO_SEEK: {
        size_t end, n;
        end = dev->ops->get_size(dev);
        switch (arg) {
        case SEEK_SET:
            if ((msg->arg2.off < 0) || ((size_t)msg->arg2.off > end)) {
                return ERR_INVALID_ARGS;
            }
            n = msg->arg2.off;
            break;
        case SEEK_CUR:
            // TODO: track seekability with flag, don't update off
            // at all on read/write if not seekable
            n = ios->io_off + msg->arg2.off;
            if (msg->arg2.off < 0) {
                // if negative seek
                if (n > ios->io_off) {
                    // wrapped around
                    return ERR_INVALID_ARGS;
                }
            } else {
                // positive seek
                if (n < ios->io_off) {
                    // wrapped around
                    return ERR_INVALID_ARGS;
                }
            }
            break;
        case SEEK_END:
            n = end + msg->arg2.off;
            if (msg->arg2.off <= 0) {
                // if negative or exact-end seek
                if (n > end) {
                    // wrapped around
                    return ERR_INVALID_ARGS;
                }
            } else {
                if (n < end) {
                    // wrapped around
                    return ERR_INVALID_ARGS;
                }
            }
            break;
        default:
            return ERR_INVALID_ARGS;
        }
        if (n > end) {
            // devices may not seek past the end
            return ERR_INVALID_ARGS;
        }
        ios->io_off = n;
        msg->arg2.off = ios->io_off;
        return NO_ERROR;
    }
    case MXRIO_STAT: {
        msg->datalen = sizeof(vnattr_t);
        vnattr_t* attr = (void*)msg->data;
        memset(attr, 0, sizeof(vnattr_t));
        attr->mode = V_TYPE_CDEV | V_IRUSR | V_IWUSR;
        attr->size = dev->ops->get_size(dev);
        return msg->datalen;
    }
    case MXRIO_SYNC: {
        return do_ioctl(dev, IOCTL_DEVICE_SYNC, NULL, 0, NULL, 0);
    }
    case MXRIO_IOCTL: {
        if (len > MXIO_IOCTL_MAX_INPUT || arg > (ssize_t)sizeof(msg->data)) {
            for (unsigned i = 0; i < msg->hcount; i++) {
                mx_handle_close(msg->handle[i]);
            }
            return ERR_INVALID_ARGS;
        }

        char in_buf[MXIO_IOCTL_MAX_INPUT];
        memcpy(in_buf, msg->data, len);

        uint32_t kind = IOCTL_KIND(msg->arg2.op);

        if (kind == IOCTL_KIND_SET_HANDLE) {
            if (len < sizeof(mx_handle_t)) {
                len = sizeof(mx_handle_t);
            }
            // The sending side copied the handle into msg->handle[0]
            // so that it would be sent via channel_write().  Here we
            // copy the local version back into the space in the buffer
            // that the original occupied.
            memcpy(in_buf, msg->handle, sizeof(mx_handle_t));

            // close any extraneous handles
            for (unsigned i = 1; i < msg->hcount; i++) {
                mx_handle_close(msg->handle[i]);
            }
            msg->hcount = 0;
        }

        mx_status_t r = do_ioctl(dev, msg->arg2.op, in_buf, len, msg->data, arg);
        if (r >= 0) {
            switch (kind) {
                case IOCTL_KIND_DEFAULT:
                    break;
                case IOCTL_KIND_GET_HANDLE:
                    msg->hcount = 1;
                    memcpy(msg->handle, msg->data, sizeof(mx_handle_t));
                    break;
                case IOCTL_KIND_GET_TWO_HANDLES:
                    msg->hcount = 2;
                    memcpy(msg->handle, msg->data, 2 * sizeof(mx_handle_t));
                    break;
            }
            msg->datalen = r;
            msg->arg2.off = ios->io_off;
        } else if ((r == ERR_NOT_SUPPORTED) && (kind == IOCTL_KIND_SET_HANDLE)) {
            mx_handle_close(msg->handle[0]);
        }
        return r;
    }
    default:
        return ERR_NOT_SUPPORTED;
    }
}

mx_status_t devhost_rio_handler(mxrio_msg_t* msg, mx_handle_t rh, void* cookie) {
    devhost_iostate_t* ios = cookie;
    mx_status_t status;
    bool should_free_ios = false;
    mtx_lock(&ios->lock);
    if (ios->dev != NULL) {
        status = _devhost_rio_handler(msg, rh, ios, &should_free_ios);
    } else {
        printf("rpc-device: stale ios %p\n", ios);
        status = NO_ERROR;
    }
    mtx_unlock(&ios->lock);
    if (should_free_ios) {
        free(ios);
    }
    return status;
}
