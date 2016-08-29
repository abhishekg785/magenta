/*
 * Copyright (c) 2016, Google, Inc. All rights reserved
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files
 * (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge,
 * publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <arch/arch_ops.h>
#include <dev/hw_rng.h>
#include <err.h>
#include <kernel/vm.h>
#include <lib/cbuf.h>
#include <trace.h>
#include <dev/virtio.h>

/*
 * Some notes about using the VirtIO RNG in QEMU on Linux.
 *
 * In Linux there are a few different choices of which device node to bind to in
 * order to generate random numbers depending on the behavior that you want to
 * see.
 *
 * /dev/random
 * This will give you random bytes from the kernel limited, high quality,
 * entropy pool.  Typically, HW RNGs are a bit slow, and /dev/random really
 * demonstrates this.  Once you are out of random bits, reads from /dev/random
 * will begin to block, which (in this driver) will manifest as long delays in
 * attempting to refill the entropy pool.  Because of this, there is probably no
 * need to throttle the VirtIO host side device in order to provide a good
 * simulation of a real HW RNG.  This said, the pool is small, and unless you
 * need the best of the best bits, you might want to consider using something
 * else.  Pulling too hard on /dev/random in your simulated machine may slow
 * down other operations in your host which need to use /dev/random for high
 * quality entropy.
 *
 * /dev/urandom
 * This will give you pretty good entropy.  It will seed from /dev/random and
 * churn out pseudo-random bits instead of blocking when the kernel entropy pool
 * runs out.  Because of this, there is virtual no limit to how fast you can
 * make random bits when you use /dev/urandom, and you should probably consider
 * configuring a throttle in order to more accurately simulate a real HW RNG.
 *
 * /dev/zero
 * If you want something deterministic, and you don't need to actually be random
 * at all, /dev/zero is a simple option.  Again, a throttle is probably
 * appropriate.
 *
 * Two sets of arguments need to be passed to QEMU in order to instantiate a
 * host side MMIO VirtIO RNG device.
 *
 * -object rng-random,filename=<file>,id=rng0
 *
 * This set declares the deivce in the guest.  Its name will be "rng0" and
 * needs to match the name in the next set of params which defines the host
 * side device.  <file> should be set to the name of the device node you want
 * to bind the virtual device to (ex: /dev/urandom)
 *
 * -device virtio-rng-device,rng=rng0
 *
 * This set selects the host side device driver and binds it to the name of the
 * device object instantiated in the first set of parameters.  In this case, we
 * are binding the MMIO VirtIO RNG driver to the object "rng0".
 *
 * If you need to throttle the virtual device, you can do so using the
 * "max-bytes" and "period" options which get passed to the driver.  The host
 * driver line would look something like...
 *
 * -device virtio-rng-device,rng=rng0,max-bytes=<N>,period=<T>"
 *
 *  N defines the number of bytes which get generated by the virtual device
 *  every T milliseconds.
 *
 *  Putting it all together, to get random bits from urandom and produce them at
 *  a rate of 1 Kbps with coarse (1/8 sec) timing granularity, one would add the
 *  following to your QEMU command line.
 *
 * -object rng-random,filename=/dev/urandom,id=rng0
 * -device virtio-rng-device,rng=rng0,max-bytes=16,period=125"
 *
 *  One final note:  You do not need to instantiate a device if you don't want
 *  to, but if you don't, and user code calls the hw_rng API in a blocking
 *  fashion (either via hw_rng_get_entropy or hw_rng_get_u32), it will hang.
 *  Non-blocking calls will always return 0 bytes.
 */

#define LOCAL_TRACE 0

#define VIRTIO_RNG_VIRTQUEUE_ID 0u
#define VIRTIO_RNG_VIRTQUEUE_DESC_COUNT 2u

#ifndef VIRTIO_RNG_ENTROPY_POOL_SIZE
#define VIRTIO_RNG_ENTROPY_POOL_SIZE 256
#endif

struct virtio_rng_device {
    struct virtio_device *vio_dev;
    cbuf_t  entropy_pool_cbuf;
    paddr_t entropy_pool_paddr;
    bool    fill_op_in_flight;
    uint8_t entropy_pool[VIRTIO_RNG_ENTROPY_POOL_SIZE];
};

static void virtio_rng_module_init(void);
static status_t virtio_rng_init(struct virtio_device *vio_dev);

VIRTIO_DEV_CLASS(rng, VIRTIO_DEV_ID_ENTROPY_SRC, virtio_rng_module_init, virtio_rng_init, NULL);

static struct virtio_rng_device g_device;

static void virtio_rng_fill_entropy_pool(struct virtio_rng_device *dev) {
    // If we were never successfully bound to an underlying VirtIO device, then
    // this is a no-op.
    if (!dev->vio_dev)
        return;

    // If there is space in the buffer, and there is not already a read
    // operation in flight, start a new read op.
    spin_lock_saved_state_t state;
    spin_lock_irqsave(&dev->entropy_pool_cbuf.lock, state);

    if ((dev->fill_op_in_flight) || !cbuf_space_avail(&dev->entropy_pool_cbuf)) {
        spin_unlock_irqrestore(&dev->entropy_pool_cbuf.lock, state);
        return;
    }

    dev->fill_op_in_flight = true;
    spin_unlock_irqrestore(&dev->entropy_pool_cbuf.lock, state);

    // How much space do we need to fill up?
    __UNUSED size_t to_fill;
    iovec_t fill_regions[2];
    to_fill = cbuf_peek_write(&dev->entropy_pool_cbuf, fill_regions);
    DEBUG_ASSERT(to_fill);

    // Grab a descriptor chain of appropriate length.
    struct vring_desc* chain;
    uint16_t chain_head;
    size_t chain_len = fill_regions[1].iov_len ? 2 : 1;

    chain = virtio_alloc_desc_chain(dev->vio_dev,
                                    VIRTIO_RNG_VIRTQUEUE_ID,
                                    chain_len,
                                    &chain_head);
    DEBUG_ASSERT(chain);

    // Point the chain at the regions of the cbuf which need to be filled.
    for (size_t i = 0; i < chain_len; ++i) {
        if (i) {
            DEBUG_ASSERT(chain->flags == (VRING_DESC_F_NEXT | VRING_DESC_F_WRITE));
            chain = virtio_desc_index_to_desc(dev->vio_dev,
                                              VIRTIO_RNG_VIRTQUEUE_ID,
                                              chain->next);
            DEBUG_ASSERT(chain);
        }

        size_t offset = (size_t)((intptr_t)fill_regions[i].iov_base -
                                 (intptr_t)dev->entropy_pool);

        DEBUG_ASSERT(fill_regions[i].iov_base && fill_regions[i].iov_len);
        DEBUG_ASSERT((offset < sizeof(dev->entropy_pool)) &&
                     (fill_regions[i].iov_len <=(sizeof(dev->entropy_pool) - offset)));

        chain->addr   = dev->entropy_pool_paddr + offset;
        chain->len    = fill_regions[i].iov_len;
        chain->flags |= VRING_DESC_F_WRITE;
    }

    DEBUG_ASSERT(chain->flags == VRING_DESC_F_WRITE);

    // Submit it and start the transfer
    virtio_submit_chain(dev->vio_dev, VIRTIO_RNG_VIRTQUEUE_ID, chain_head);
    virtio_kick(dev->vio_dev, VIRTIO_RNG_VIRTQUEUE_ID);
}

static enum handler_return virtio_rng_irq(struct virtio_device *vio_dev,
                                          uint virtqueue_id,
                                          const struct vring_used_elem *used) {
    struct virtio_rng_device* dev = (struct virtio_rng_device*)(vio_dev->priv);

    DEBUG_ASSERT(virtqueue_id == VIRTIO_RNG_VIRTQUEUE_ID);
    DEBUG_ASSERT(used->len <= cbuf_space_avail(&dev->entropy_pool_cbuf));
    DEBUG_ASSERT(dev->fill_op_in_flight);

    // Give the chain back.
    virtio_free_desc_chain(dev->vio_dev, VIRTIO_RNG_VIRTQUEUE_ID, used->id);

    // Advance the cbuf write pointer.
    // TODO(johngro): invalidate the dcache for the region of the cbuf we just DMAed to.
    cbuf_advance_write(&dev->entropy_pool_cbuf, used->len, false);

    // Flag the fact that this fill operation is no longer in flight.
    dev->fill_op_in_flight = false;
    smp_wmb();

    // Schedule the next read, if needed.
    virtio_rng_fill_entropy_pool(dev);

    return INT_RESCHEDULE;
}

// VirtIO MMIO Driver API implementation
static void virtio_rng_module_init(void)
{
    struct virtio_rng_device* dev = &g_device;

    /* Set up our cbuf to use our entropy pool buffer */
    cbuf_initialize_etc(&dev->entropy_pool_cbuf,
                        sizeof(dev->entropy_pool),
                        dev->entropy_pool);
}

static status_t virtio_rng_init(struct virtio_device *vio_dev)
{
    struct virtio_rng_device* dev = &g_device;

    /* Already initialized? */
    if (dev->vio_dev)
        return ERR_ALREADY_STARTED;

    /* Grab the physical address of the entropy pool buffer. */
#if WITH_KERNEL_VM
    dev->entropy_pool_paddr = vaddr_to_paddr(dev->entropy_pool);
#else
    dev->entropy_pool_paddr = (paddr_t)dev->entropy_pool;
#endif

    /* Place the device in reset */
    virtio_reset_device(vio_dev);

    /* Let the device know that we see it, and know how to talk to it */
    virtio_status_acknowledge_driver(vio_dev);

    /* TODO(johngro): negotiate features */

    /* Create out virtqueue */
    status_t res = virtio_alloc_ring(vio_dev,
                                     VIRTIO_RNG_VIRTQUEUE_ID,
                                     VIRTIO_RNG_VIRTQUEUE_DESC_COUNT);
    if (res != NO_ERROR) {
        LTRACEF("Failed to allocate virtqueue for VirtIO HW RNG (queue #%u, desc_count %u)\n",
                VIRTIO_RNG_VIRTQUEUE_ID, VIRTIO_RNG_VIRTQUEUE_DESC_COUNT);
        goto finished;
    }

    /* Setup our callbacks */
    vio_dev->priv = dev;
    vio_dev->irq_driver_callback = &virtio_rng_irq;
    vio_dev->config_change_callback = NULL;
    dev->vio_dev = vio_dev;

    /* Inform the device that we are ready to go. */
    virtio_status_driver_ok(vio_dev);

    /* Send out a transfer to fill up the entropy pool with tasty random bits */
    virtio_rng_fill_entropy_pool(dev);

finished:
    return res;
}

// HW RNG API implementation
size_t hw_rng_get_entropy(void* buf, size_t len, bool block) {
    if (!len)
        return 0;

    DEBUG_ASSERT(buf);

    struct virtio_rng_device* dev = &g_device;
    size_t done = 0;

    done = cbuf_read(&dev->entropy_pool_cbuf, buf, len, block);
    virtio_rng_fill_entropy_pool(dev);  // Make sure we are keeping the pool full

    if (block) {
        while (done < len) {
            done += cbuf_read(&dev->entropy_pool_cbuf, buf + done, len - done, true);
            virtio_rng_fill_entropy_pool(dev);
        }
    }

    return done;
}