#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/blk-mq.h>
#include <linux/hdreg.h>

#ifndef SUCCESS
#define SUCCESS 0
#endif
#ifndef SECTOR_SHIFT
#define SECTOR_SHIFT 9
#endif
#ifndef SECTOR_SIZE
#define SECTOR_SIZE 512
#endif
#define SIZE_DEVICE_MB 100

MODULE_LICENSE("GPL");

static int device_major = 0;
static  char *device_name = "myDev";
static int device_size_mb = (SIZE_DEVICE_MB*1024*1024)/SECTOR_SIZE;

static int block_device_create(void);
static int block_device_open(struct block_device *, fmode_t);
static void block_device_release(struct gendisk *, fmode_t);
int block_device_ioctl(struct block_device *, fmode_t, unsigned, unsigned long);
static int block_device_delete(void);
static blk_status_t queue_rq(struct blk_mq_hw_ctx *, const struct blk_mq_queue_data*);
static int do_request(struct request *, unsigned int *);

typedef struct
{
    sector_t                capacity;   //size device in bytes
    u8                      *data;      //data buffer
    struct blk_mq_tag_set   tag_set;    //all params device
    struct request_queue    *queue;     //for mutal exclusion
    struct gendisk          *gdisk;     //gendisk struct
}block_device_s;

static const struct block_device_operations block_device_opt = {
    .owner   = THIS_MODULE,
    .open    = block_device_open,
    .release = block_device_release,
    .ioctl   = block_device_ioctl
};

static struct blk_mq_ops mq_ops = {
    .queue_rq = queue_rq,
};

block_device_s *block_device = NULL;

static int __init block_device_init(void){
    block_device_create();
    return 0;
}
static void __exit block_device_exit(void){
    block_device_delete();

}

module_init(block_device_init);
module_exit(block_device_exit);


static int block_device_create(void){

        printk(KERN_INFO "Start create device\n");

        device_major = register_blkdev(device_major,device_name);

        if(device_major <= 0){
            printk(KERN_WARNING "%s: unable to get major number\n",device_name);
            return -EBUSY;
        }else{
            printk(KERN_INFO "%s: register and get major number %d\n",device_name,device_major);
        }

        block_device = kzalloc(sizeof(block_device_s),GFP_KERNEL);

        if(block_device == NULL){
            printk(KERN_WARNING "%s: Fail. No allocate to block_device struct\n",device_name);
            unregister_blkdev(device_major,device_name);
            return -ENOMEM;
        }else{
            printk(KERN_INFO "%s: Succsess. Memory allocate to block_device struct\n",device_name);
        }

        memset(block_device,0,sizeof(block_device_s));

        block_device->capacity = device_size_mb;
        block_device->data = vmalloc(block_device->capacity);

        if(block_device->data == NULL){
            printk(KERN_WARNING "%s: Fail. No allocate to data in block_device\n",device_name);

            unregister_blkdev(device_major,device_name);
            kfree(block_device);

            return -ENOMEM;
        }else{
            printk(KERN_INFO "%s: Succsess. Memory allocate to data in block_device\n", device_name);
        }
        printk(KERN_INFO "Initializing queue\n");

        block_device->queue = blk_mq_init_sq_queue(&block_device->tag_set,&mq_ops,128,BLK_MQ_F_SHOULD_MERGE);

        if(block_device->queue == NULL){
            printk(KERN_WARNING "%s: Fail. No allocate to queue in block_device",device_name);
            kfree(block_device->data);
            unregister_blkdev(device_major,device_name);
            kfree(block_device);
            return -ENOMEM;
        }else{
            printk(KERN_INFO "%s: Succsess. Memory allocate to queue in block_device\n",device_name);
        }

        block_device->queue->queuedata = block_device;
        block_device->gdisk = alloc_disk(1);
        block_device->gdisk->flags = GENHD_FL_NO_PART_SCAN;
        block_device->gdisk->major = device_major;
        block_device->gdisk->first_minor = 0;
        block_device->gdisk->fops = &block_device_opt;
        block_device->gdisk->queue = block_device->queue;
        block_device->gdisk->private_data = block_device;
        strncpy(block_device->gdisk->disk_name, device_name, 6);

        set_capacity(block_device->gdisk, block_device->capacity);
        add_disk(block_device->gdisk);
        printk(KERN_INFO "%s: Disk create\n", device_name);
    return 0;
}



static int block_device_delete(void){

     if (block_device->gdisk) {
        del_gendisk(block_device->gdisk);
        put_disk(block_device->gdisk);
    }

    if (block_device->queue) {
        blk_cleanup_queue(block_device->queue);
        printk(KERN_INFO "%s: Memory queue device free\n",device_name);
    }
            vfree(block_device->data);
            block_device->data = NULL;
            block_device->capacity = 0;
            printk(KERN_INFO "%s: Memory data device free\n",device_name);


    if(device_major > 0){
        unregister_blkdev(device_major,device_name);
        printk(KERN_INFO "%s: Unregister\n",device_name);
    }
    kfree(block_device);
    printk(KERN_INFO "%s: Memory device free\n",device_name);



    return 0;
}

static int block_device_open(struct block_device *device, fmode_t mode){
    printk(KERN_INFO "%s: Disk open\n",device_name);
    return 0;
}
static void block_device_release(struct gendisk *gdisk, fmode_t mode){
    printk(KERN_INFO "%s: Disk close\n",device_name);
}
int block_device_ioctl(struct block_device *device, fmode_t mode, unsigned cmd, unsigned long arg){

    printk(KERN_INFO "ioctl cmd 0x%08x\n", cmd);

    return -ENOTTY;
}
static int do_request(struct request *rq, unsigned int *nr_bytes){
    int ret = SUCCESS;
    struct bio_vec bvec;
    struct req_iterator iter;
    block_device_s *dev = rq->q->queuedata;
    loff_t pos = blk_rq_pos(rq) << SECTOR_SHIFT;
    loff_t dev_size = (loff_t)(dev->capacity << SECTOR_SHIFT);

    printk(KERN_WARNING "%s: request start from sector %lld  pos = %lld  dev_size = %lld\n", device_name,blk_rq_pos(rq), pos, dev_size);

    /* Iterate over all requests segments */
    rq_for_each_segment(bvec, rq, iter)
    {
        unsigned long b_len = bvec.bv_len;

        /* Get pointer to the data */
        void* b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

        /* Simple check that we are not out of the memory bounds */
        if ((pos + b_len) > dev_size) {
            b_len = (unsigned long)(dev_size - pos);
        }

        if (rq_data_dir(rq) == WRITE) {
            /* Copy data to the buffer in to required position */
            printk(KERN_INFO "%s: start write\n",device_name);
            memcpy(dev->data + pos, b_buf, b_len);

        } else {
            /* Read data from the buffer's position */
            printk(KERN_INFO "%s: start read\n",device_name);
            memcpy(b_buf, dev->data + pos, b_len);
        }

        /* Increment counters */
        pos += b_len;
        *nr_bytes += b_len;
    }

    return ret;
}

/* queue callback function */
static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx, const struct blk_mq_queue_data* bd){
    unsigned int nr_bytes = 0;
    blk_status_t status = BLK_STS_OK;
    struct request *rq = bd->rq;

    /* Start request serving procedure */
    blk_mq_start_request(rq);

    if (do_request(rq, &nr_bytes) != 0) {
        status = BLK_STS_IOERR;
    }

    /* Notify kernel about processed nr_bytes */
    if (blk_update_request(rq, status, nr_bytes)) {
        /* Shouldn't fail */
        BUG();
    }

    /* Stop request serving procedure */
    __blk_mq_end_request(rq, status);

    return status;
}
