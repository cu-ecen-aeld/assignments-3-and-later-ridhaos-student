/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Ridha Noomane"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");

    struct aesd_dev *dev;

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);

    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    // declare all variables at top:
    struct aesd_dev *dev;
    struct aesd_buffer_entry *entry;
    size_t entry_offset;
    size_t bytes_to_copy;

    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    // Step 1: get dev from filp
    dev = filp->private_data;
    
    // Step 2: lock mutex
    if(mutex_lock_interruptible(&dev->lock)){
        return -ERESTARTSYS;
    }

    // Step 3: call find_entry_offset_for_fpos
    //         pass *f_pos as char_offset
    //         pass &entry_offset for the last argument
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->aesd_buffer, (size_t)*f_pos, &entry_offset);

    // Step 4: if entry is NULL → goto out (retval=0 = EOF)
    if (!entry)
    {
        retval = 0;
        goto out;
    }
    
    // Step 5: calculate bytes_to_copy using min()
    bytes_to_copy = min(count, entry->size - entry_offset);

    // Step 6: copy_to_user from correct position
    if(copy_to_user(buf, entry->buffptr+entry_offset, bytes_to_copy)){
        retval = -EFAULT;
        goto out;
    }

    // Step 7: update *f_pos
    *f_pos += bytes_to_copy;

    // Step 8: set retval = bytes_to_copy
    retval = bytes_to_copy;
out:
    // Step 9: unlock mutex
    mutex_unlock(&dev->lock);
    // Step 10: return retval
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    
    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev;
    size_t new_size;
    char *new_buf;
    struct aesd_buffer_entry entry;

    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);


    // #1 Get device struct
    dev = filp->private_data;

    // #2 Lock Mutex
    if(mutex_lock_interruptible(&dev->lock)){
        return -ERESTARTSYS;
    }
    
    // #3 Prepare buffer for new message or add new message
    new_size = dev->temp_buff_size + count;


    new_buf = krealloc(dev->temp_buff, new_size, GFP_KERNEL);
    if(!new_buf)
        goto out;
    
    dev->temp_buff = new_buf;

    // Step 4: copy_from_user into NEW space at end
    if(copy_from_user(dev->temp_buff+dev->temp_buff_size, buf, count)){
        retval = -EFAULT;
        goto out;
    }

    // Step 5: update temp_buff_size
    dev->temp_buff_size += count;

    // Step 6: check for newline with memchr
    // Step 7: if newline found
    //         → free old circular buffer entry if full
    //         → fill aesd_buffer_entry
    //         → add_entry
    //         → reset temp_buff and temp_buff_size

    if(memchr(dev->temp_buff, '\n', dev->temp_buff_size)){
        if (dev->aesd_buffer.full) {
            kfree(dev->aesd_buffer.entry[dev->aesd_buffer.in_offs].buffptr);
        }

        entry.buffptr =  dev->temp_buff;
        entry.size    =  dev->temp_buff_size;

        aesd_circular_buffer_add_entry(&dev->aesd_buffer, &entry);
        dev->temp_buff = NULL;
        dev->temp_buff_size = 0;
    }


    // Step 9: return count
    retval = count;

out:
    mutex_unlock(&dev->lock);
    return retval;
}


struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.aesd_buffer);
    mutex_init(&aesd_device.lock);
    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);
    uint8_t index;
    struct aesd_buffer_entry *entry;    
    cdev_del(&aesd_device.cdev);


    AESD_CIRCULAR_BUFFER_FOREACH(entry,&aesd_device.aesd_buffer,index) {
       kfree(entry->buffptr);
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
