#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "aesdchar.h"
#include "aesd_ioctl.h"

int aesd_major = 0;
int aesd_minor = 0;
MODULE_AUTHOR("ash-134");
MODULE_LICENSE("Dual BSD/GPL");
struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp) {
    filp->private_data = container_of(inode->i_cdev, struct aesd_dev, cdev);
    return 0;
}
int aesd_release(struct inode *inode, struct file *filp) { return 0; }

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    ssize_t retval = 0;
    struct aesd_dev *dev = filp->private_data;
    struct aesd_buffer_entry *entry;
    size_t entry_offset = 0, bytes_to_read;
    if (mutex_lock_interruptible(&dev->lock)) return -ERESTARTSYS;
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->buffer, *f_pos, &entry_offset);
    if (entry != NULL) {
        bytes_to_read = entry->size - entry_offset;
        if (bytes_to_read > count) bytes_to_read = count;
        if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_read)) retval = -EFAULT;
        else { *f_pos += bytes_to_read; retval = bytes_to_read; }
    }
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos) {
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;
    char *new_buf;
    const char *old_buf;
    if (mutex_lock_interruptible(&dev->lock)) return -ERESTARTSYS;
    new_buf = krealloc(dev->working_entry.buffptr, dev->working_entry.size + count, GFP_KERNEL);
    if (!new_buf) { mutex_unlock(&dev->lock); return -ENOMEM; }
    dev->working_entry.buffptr = new_buf;
    if (copy_from_user((void *)(dev->working_entry.buffptr + dev->working_entry.size), buf, count)) {
        mutex_unlock(&dev->lock); return -EFAULT;
    }
    dev->working_entry.size += count;
    retval = count;
    if (memchr(dev->working_entry.buffptr, '\n', dev->working_entry.size) != NULL) {
        old_buf = aesd_circular_buffer_add_entry(&dev->buffer, &dev->working_entry);
        if (old_buf) kfree(old_buf);
        dev->working_entry.buffptr = NULL;
        dev->working_entry.size = 0;
    }
    mutex_unlock(&dev->lock);
    return retval;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence) {
    struct aesd_dev *dev = filp->private_data;
    loff_t total_size = 0, retval;
    uint8_t index;
    struct aesd_buffer_entry *entry;
    if (mutex_lock_interruptible(&dev->lock)) return -ERESTARTSYS;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &dev->buffer, index) {
        if (entry->buffptr) total_size += entry->size;
    }
    retval = fixed_size_llseek(filp, off, whence, total_size);
    mutex_unlock(&dev->lock);
    return retval;
}

static long aesd_adjust_file_offset(struct file *filp, unsigned int write_cmd, unsigned int write_cmd_offset) {
    struct aesd_dev *dev = filp->private_data;
    uint8_t num_entries = 0, i, actual_idx;
    loff_t new_fpos = 0;
    if (mutex_lock_interruptible(&dev->lock)) return -ERESTARTSYS;
    if (dev->buffer.full) num_entries = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    else num_entries = (dev->buffer.in_offs - dev->buffer.out_offs + AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    if (write_cmd >= num_entries) { mutex_unlock(&dev->lock); return -EINVAL; }
    for (i = 0; i < write_cmd; i++) {
        actual_idx = (dev->buffer.out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
        new_fpos += dev->buffer.entry[actual_idx].size;
    }
    actual_idx = (dev->buffer.out_offs + write_cmd) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    if (write_cmd_offset >= dev->buffer.entry[actual_idx].size) { mutex_unlock(&dev->lock); return -EINVAL; }
    filp->f_pos = new_fpos + write_cmd_offset;
    mutex_unlock(&dev->lock);
    return 0;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
    struct aesd_seekto seekto;
    if (_IOC_TYPE(cmd) != AESD_IOC_MAGIC || _IOC_NR(cmd) > AESDCHAR_IOC_MAXNR) return -ENOTTY;
    if (cmd == AESDCHAR_IOCSEEKTO) {
        if (copy_from_user(&seekto, (const void __user *)arg, sizeof(seekto)) != 0) return -EFAULT;
        return aesd_adjust_file_offset(filp, seekto.write_cmd, seekto.write_cmd_offset);
    }
    return -ENOTTY;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE, .read = aesd_read, .write = aesd_write,
    .open = aesd_open, .release = aesd_release, .llseek = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl, .compat_ioctl = compat_ptr_ioctl,
};

int aesd_init_module(void) {
    dev_t dev = 0;
    int result = alloc_chrdev_region(&dev, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) return result;
    memset(&aesd_device, 0, sizeof(struct aesd_dev));
    mutex_init(&aesd_device.lock);
    aesd_circular_buffer_init(&aesd_device.buffer);
    cdev_init(&aesd_device.cdev, &aesd_fops);
    aesd_device.cdev.owner = THIS_MODULE;
    aesd_device.cdev.ops = &aesd_fops;
    result = cdev_add(&aesd_device.cdev, MKDEV(aesd_major, aesd_minor), 1);
    if (result) unregister_chrdev_region(dev, 1);
    return result;
}

void aesd_cleanup_module(void) {
    uint8_t index;
    struct aesd_buffer_entry *entry;
    cdev_del(&aesd_device.cdev);
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.buffer, index) {
        if (entry->buffptr) kfree(entry->buffptr);
    }
    if (aesd_device.working_entry.buffptr) kfree(aesd_device.working_entry.buffptr);
    mutex_destroy(&aesd_device.lock);
    unregister_chrdev_region(MKDEV(aesd_major, aesd_minor), 1);
}
module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
