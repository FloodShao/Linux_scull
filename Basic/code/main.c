/*
* @Author: FloodShao
* @Date:   2019-12-03 09:51:56
* @Last Modified by:   FloodShao
* @Last Modified time: 2019-12-03 16:19:12
*/

#include <linux/module.h> 
#include <linux/moduleparam.h>
#include <linux/init.h>

#include <linux/kernel.h> //printk()
#include <linux/slab.h> //kmalloc()
#include <linux/fs.h> //file-related
#include <linux/errno.h> //error codes
#include <linux/types.h> //size_t
#include <linux/proc_fs.h>
#include <linux/fcntl.h> //O_ACCMODE
#include <linux/seq_file.h>
#include <linux/cdev.h>


#include <linux/uaccess.h> //copt_*_user
#include "scull.h" //local defination

int scull_major 	= SCULL_MAJOR;
int scull_minor 	= 0;
int scull_nr_devs 	= SCULL_NR_DEVS;
int scull_quantum 	= SCULL_QUANTUM;
int scull_qset 		= SCULL_QSET;


struct scull_dev *scull_devices;

struct file_operations scull_fops = {
	.owner	= THIS_MODULE,
	.llseek	= scull_llseek,
	.read = scull_read,
	.write = scull_write,
	.open = scull_open,
	.release = scull_release,
};

/**
 * Find the qset, following the list
 * @param
 * @param
 * @return
 */
struct scull_qset *scull_follow(struct scull_dev *dev, int n){

	struct scull_qset *qs = dev->data; //the first qset pointer

	if(!qs){
		qs = dev->data = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
		if(qs == NULL){
			return NULL;
		}
		memset(qs, 0, sizeof(struct scull_qset));
	}

	while(n--){
		if(!qs->next){
			qs->next = kmalloc(sizeof(struct scull_qset), GFP_KERNEL);
			if(!qs->next) return NULL;
			memset(qs->next, 0, sizeof(struct scull_qset));
		}
		qs = qs->next;
		continue;
	}
	return qs;
}

/**
 * Open a char device
 * @param
 * @param
 * @return
 */
int scull_open(struct inode *inode, struct file *filp){
	struct scull_dev *dev;

	dev = container_of(inode->i_cdev, struct scull_dev, cdev);
	filp->private_data = dev;

	if((filp->f_flags & O_ACCMODE) == O_WRONLY){
		if(down_interruptible(&dev->sem)){
			return -ERESTARTSYS;
		}
		scull_trim(dev); //ignore erros
		up(&dev->sem);
	}

	return 0;
}



/**
 * Read within one quantum to the user space buf
 * @param 
 * @param
 * @param
 * @param
 * @return
 */
ssize_t scull_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos){

	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum;
	int qset = dev->qset;
	int itemsize = quantum * qset;
	int item, rest, s_pos, q_pos;
	ssize_t retval = 0;

	if(down_interruptible(&dev->sem)) //get the semaphore to proceed
		return -ERESTARTSYS; //upper kernel receive this return will restart the driver again
	if(*f_pos >= dev->size)
		goto out;
	if(*f_pos + count > dev->size)
		count = dev->size - *f_pos;
	
	/*find the listitem*/
	item = *f_pos / itemsize;
	rest = *f_pos % itemsize;
	s_pos = rest / quantum;
	q_pos = rest % quantum;

	dptr = scull_follow(dev, item); //find the current qset
	if(dptr == NULL || !dptr->data || !dptr->data[s_pos])
		goto out; //there is nothing to read from
	if(count > quantum - q_pos) //read within one quantum
		count = quantum - q_pos;
	if(copy_to_user(buf, dptr->data[s_pos]+q_pos, count)){
		retval = -EFAULT;
		goto out;
	}
	*f_pos += count;
	retval = count;

out:
	up(&dev->sem); //release the semaphore
	return retval;
}


/** Write from user space buf to kernel space file
 * @param
 * @param
 * @param
 * @param
 * @return
 */
ssize_t scull_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos){
	struct scull_dev *dev = filp->private_data;
	struct scull_qset *dptr;
	int quantum = dev->quantum, qset = dev->qset;
	int itemsize = quantum * qset;
	int item, s_pos, q_pos, rest;
	ssize_t retval = -ENOMEM;

	if(down_interruptible(&dev->sem)){ //didn't get the sem
		return -ERESTARTSYS;
	}


	// find the listitem
	item = *f_pos / itemsize;
	rest = *f_pos % itemsize;
	s_pos = rest / quantum; q_pos = rest % quantum;

	dptr = scull_follow(dev, item);
	if(!dptr){//no mem to write
		goto out;
	}
	if(!dptr->data){
		dptr->data = kmalloc(qset * sizeof(char *), GFP_KERNEL); //alloc a qset
		if(!dptr->data){
			goto out; //no mem for one qset
		}
		memset(dptr->data, 0, qset * sizeof(char *));
	}
	if(!dptr->data[s_pos]){
		dptr->data[s_pos] = kmalloc(quantum, GFP_KERNEL);
		if(!dptr->data[s_pos]){
			goto out;
		}
	}

	if(count > quantum - q_pos){
		count = quantum - q_pos;
	}

	if(copy_from_user(dptr->data[s_pos] + q_pos, buf, count)){
		retval = -EFAULT;
		goto out;
	}

	*f_pos += count;
	retval = count;

	if(dev->size < *f_pos)
		dev->size = *f_pos;

out:
	up(&dev->sem);
	return retval;

}

/** Free the kernel space of the device
 * @param
 * @return
 */
int scull_trim(struct scull_dev *dev){
	struct scull_qset *next, *dptr;
	int qset = dev->qset; //the current qset size
	int i;
	for(dptr = dev->data; dptr; dptr = next){
		if(dptr->data){
			for(i = 0; i<qset; i++){ //free the quantum first
				kfree(dptr->data[i]);
			}
			kfree(dptr->data); //free the quantum pointer
			dptr->data = NULL; //free the qset pointer
		}
		next = dptr->next;
		kfree(dptr); 
	}

	//reconstruct the dev with 0 content
	dev->size = 0;
	dev->quantum = scull_quantum;
	dev->qset = scull_qset;
	dev->data = NULL;
	return 0; 
} 


static void scull_setup_cdev(struct scull_dev *dev, int index){
	int err, devno = MKDEV(scull_major, scull_minor+index);
	cdev_init(&dev->cdev, &scull_fops); //static init the cdev
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &scull_fops;
	err = cdev_add(&dev->cdev, devno, 1);

	if(err){
		printk("error %d adding scull %d", err, index);
	}
}


void scull_cleanup_module(void)
{
	int i;
	dev_t devno = MKDEV(scull_major, scull_minor);

	/* Get rid of our char dev entries */
	if (scull_devices) {
		for (i = 0; i < scull_nr_devs; i++) {
			scull_trim(scull_devices + i);
			cdev_del(&scull_devices[i].cdev);
		}
		kfree(scull_devices);
	}

	/* cleanup_module is never called if registering failed */
	unregister_chrdev_region(devno, scull_nr_devs);

	/* and call the cleanup functions for friend devices */
	//scull_p_cleanup();
	//scull_access_cleanup();
}

/*
Several related functions
 */
loff_t scull_llseek(struct file *filp, loff_t off, int whence){
	return 0;
}

int scull_ioctl(struct inode *inode, struct file *filp, unsigned int cmd, unsigned long arg){
	return 0;
}

int scull_release(struct inode* inode, struct file *filp){
	return 0;
}

int scull_p_init(dev_t dev){
	return 0;
}
void scull_p_cleanup(void){
	//return 0;
}
int scull_access_init(dev_t dev){
	return 0;
}
void scull_acess_cleanup(void){

}



/*
The init module function
 */

int scull_init_module(void){

	int result, i;
	dev_t dev = 0;

	if(scull_major){
		dev = MKDEV(scull_major, scull_minor);
		result = register_chrdev_region(dev, scull_nr_devs, "scull");		
	} else{
		result = alloc_chrdev_region(&dev, scull_minor, scull_nr_devs, "scull");
		scull_major = MAJOR(dev);
	}

	if(result < 0){
		printk("scull_sgl: can't get major %d. \n", scull_major);
		return result;
	}else{
		printk("make a dev %d %d \n", scull_major, scull_minor);
	}

	//alloc the data structure for dev
	scull_devices = kmalloc(scull_nr_devs * sizeof(struct scull_dev), GFP_KERNEL);
	if(!scull_devices){
		result = 1;
		goto fail;
	}
	memset(scull_devices, 0, scull_nr_devs * sizeof(struct scull_dev));

	for(i = 0; i < scull_nr_devs; i++){
		scull_devices[i].quantum = scull_quantum;
		scull_devices[i].qset = scull_qset;
		sema_init(&scull_devices[i].sem, 1);
		//check the defination
		scull_setup_cdev(&scull_devices[i], i);
		//defined in where?

	}
	dev = MKDEV(scull_major, scull_minor + scull_nr_devs);

	return 0;

fail:
	scull_cleanup_module();
	return result;
}




module_init(scull_init_module);
module_exit(scull_cleanup_module);