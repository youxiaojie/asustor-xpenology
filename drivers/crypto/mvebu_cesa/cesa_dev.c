#ifdef CONFIG_ARCH_MVEBU
#include <generated/autoconf.h>
#else
#include <linux/autoconf.h>
#endif
#include <linux/types.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/unistd.h>
#include <linux/module.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/miscdevice.h>
#include <linux/version.h>
#include <asm/uaccess.h>

#include "cesa_dev.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,11)
#include <linux/syscalls.h>
#endif
#include "mvOs.h"
#include "mvCommon.h"
#include "mvCesa.h"

static int debug = 1;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug,
	   "Enable debug");

extern void cesaTest(int iter, int reqSize, int checkMode);
extern void combiTest(int iter, int reqSize, int checkMode);
extern void cesaOneTest(int testIdx, int caseIdx, int iter, int reqSize, int checkMode);
extern void multiSizeTest(int idx, int checkMode, int iter, char* inputData);
extern void aesTest(int iter, int reqSize, int checkMode);
extern void desTest(int iter, int reqSize, int checkMode);
extern void tripleDesTest(int iter, int reqSize, int checkMode);
extern void mdTest(int iter, int reqSize, int checkMode);
extern void sha1Test(int iter, int reqSize, int checkMode);
extern void sha2Test(int iter, int reqSize, int checkMode);

int run_cesa_test(CESA_TEST *cesa_test)
{
	switch(cesa_test->test){
		case(MULTI):
			combiTest(cesa_test->iter, cesa_test->req_size, cesa_test->checkmode);
			break;
		case(SIZE):
                        multiSizeTest(cesa_test->req_size, cesa_test->iter, cesa_test->checkmode, NULL);
			break;
		case(SINGLE):
			cesaOneTest(cesa_test->session_id, cesa_test->data_id, cesa_test->iter,
					cesa_test->req_size, cesa_test->checkmode);
			break;
		case(AES):
			aesTest(cesa_test->iter, cesa_test->req_size, cesa_test->checkmode);
			break;
		case(DES):
			desTest(cesa_test->iter, cesa_test->req_size, cesa_test->checkmode);
			break;
		case(TRI_DES):
			tripleDesTest(cesa_test->iter, cesa_test->req_size, cesa_test->checkmode);
			break;
		case(MD5):
                        mdTest(cesa_test->iter, cesa_test->req_size, cesa_test->checkmode);
                        break;
		case(SHA1):
                        sha1Test(cesa_test->iter, cesa_test->req_size, cesa_test->checkmode);
                        break;
		case(SHA2):
                        sha2Test(cesa_test->iter, cesa_test->req_size, cesa_test->checkmode);
                        break;

		default:
			dprintk("%s(unknown test 0x%x)\n", __FUNCTION__, cesa_test->test);
			return -EINVAL;
	}
	return 0;
}

extern void    		mvCesaDebugSAD(int mode);
extern void    		mvCesaDebugSA(short sid, int mode);
extern void    		mvCesaDebugQueue(int mode);
extern void    		mvCesaDebugStatus(void);
extern void    		mvCesaDebugSram(int mode);
extern void    		cesaTestPrintReq(int req, int offset, int size);
extern void	   	    cesaTestPrintSession(int idx);
extern void	   	    cesaTestPrintStatus(void);

int run_cesa_debug(CESA_DEBUG *cesa_debug)
{
	int error = 0;

	if (mv_cesa_mode == CESA_TEST_M) {
		dprintk("%s:cesa mode %d\n", __func__, mv_cesa_mode);

		switch (cesa_debug->debug) {
		case(STATUS):
			mvCesaDebugStatus();
			break;
		case(QUEUE):
			mvCesaDebugQueue(cesa_debug->mode);
			break;
		case(SA):
			mvCesaDebugSA(cesa_debug->index,
			    cesa_debug->mode);
			break;
		case(SRAM):
			mvCesaDebugSram(cesa_debug->mode);
			break;
		case(SAD):
			mvCesaDebugSAD(cesa_debug->mode);
			break;
		case(TST_REQ):
			cesaTestPrintReq(cesa_debug->index, 0,
			    cesa_debug->size);
			break;
		case(TST_SES):
			cesaTestPrintSession(cesa_debug->index);
			break;
		case(TST_STATS):
			cesaTestPrintStatus();
			break;
		default:
			dprintk("%s(unknown debug 0x%x)\n",
			    __func__, cesa_debug->debug);
			error = EINVAL;
			break;
		}
	}

	else if (mv_cesa_mode == CESA_OCF_M) {
		dprintk("%s:cesa mode %d\n", __func__, mv_cesa_mode);

		switch (cesa_debug->debug) {
		case(STATUS):
			mvCesaDebugStatus();
			break;
		case(QUEUE):
			mvCesaDebugQueue(cesa_debug->mode);
			break;
		case(SA):
			mvCesaDebugSA(cesa_debug->index,
			    cesa_debug->mode);
			break;
		case(SRAM):
			mvCesaDebugSram(cesa_debug->mode);
			break;
		case(SAD):
			mvCesaDebugSAD(cesa_debug->mode);
			break;
		default:
			dprintk("%s(unknown debug 0x%x)\n",
			    __func__, cesa_debug->debug);
			error = EINVAL;
			break;
		}
	}

	return(-error);
}

static long
cesadev_ioctl(
	struct file *filp,
	unsigned int cmd,
	unsigned long arg)
{
	CESA_DEBUG cesa_debug;
	u32 error = 0;

	dprintk("%s: cmd=0x%x, CIOCDEBUG=0x%x, CIOCTEST=0x%x\n",
                __FUNCTION__, cmd, CIOCDEBUG, CIOCTEST);

	if (mv_cesa_mode == CESA_TEST_M) {
		dprintk("%s:cesa mode %d\n", __func__, mv_cesa_mode);

		switch (cmd) {
		case CIOCDEBUG:
			if (copy_from_user(&cesa_debug, (void *)arg,
							   sizeof(CESA_DEBUG)))
				error = -EFAULT;

			dprintk("%s(CIOCDBG): dbg %d idx %d mode %d size %d\n",
				__func__, cesa_debug.debug,
				cesa_debug.index, cesa_debug.mode,
				cesa_debug.size);
			error = run_cesa_debug(&cesa_debug);
			break;
		case CIOCTEST:
			{
			CESA_TEST cesa_test;

			if (copy_from_user(&cesa_test, (void *)arg,
							    sizeof(CESA_TEST)))
				error = -EFAULT;

			dprintk("%s(CIOCTST): test %d iter %d req_size %d",
			    __func__, cesa_test.test, cesa_test.iter,
			    cesa_test.req_size);
			dprintk(" checkmode %d sess_id %d data_id %d\n",
				cesa_test.checkmode, cesa_test.session_id,
				cesa_test.data_id);
			error = run_cesa_test(&cesa_test);
			}
			break;
		default:
			dprintk("%s(unknown ioctl 0x%x)\n", __func__, cmd);
			error = EINVAL;
			break;
		}
	}

	else if (mv_cesa_mode == CESA_OCF_M) {
		dprintk("%s:cesa mode %d\n", __func__, mv_cesa_mode);

		switch (cmd) {
		case CIOCDEBUG:
			if (copy_from_user(&cesa_debug, (void *)arg,
							   sizeof(CESA_DEBUG)))
				error = -EFAULT;

			dprintk("%s(CIOCDBG): dbg %d idx %d mode %d size %d\n",
				__func__, cesa_debug.debug,
				cesa_debug.index, cesa_debug.mode,
				cesa_debug.size);
			error = run_cesa_debug(&cesa_debug);
			break;
		default:
			dprintk("%s(unknown ioctl 0x%x)\n", __func__, cmd);
			error = EINVAL;
			break;
		}
	}

	return(-error);
}

static int
cesadev_open(struct inode *inode, struct file *filp)
{
	dprintk("%s()\n", __FUNCTION__);
	return(0);
}

static int
cesadev_release(struct inode *inode, struct file *filp)
{
	dprintk("%s()\n", __FUNCTION__);
	return(0);
}

static struct file_operations cesadev_fops = {
	.owner = THIS_MODULE,
	.open = cesadev_open,
	.release = cesadev_release,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,36)
	.ioctl = cesadev_ioctl,
#endif
#ifdef HAVE_UNLOCKED_IOCTL
	.unlocked_ioctl = cesadev_ioctl,
#endif
};

static struct miscdevice cesadev = {
	.minor = CESADEV_MINOR,
	.name = "cesa",
	.fops = &cesadev_fops,
};

static int __init
cesadev_init(void)
{
	int rc;

#if defined(CONFIG_MV78200) || defined(CONFIG_MV632X)
	if (MV_FALSE == mvSocUnitIsMappedToThisCpu(CESA))
	{
		dprintk("CESA is not mapped to this CPU\n");
		return -ENODEV;
	}
#endif

	dprintk("%s(%p)\n", __FUNCTION__, cesadev_init);
	rc = misc_register(&cesadev);
	if (rc) {
		printk(KERN_ERR "cesadev: registration of /dev/cesadev failed\n");
		return(rc);
	}
	return(0);
}

static void __exit
cesadev_exit(void)
{
	dprintk("%s()\n", __FUNCTION__);
	misc_deregister(&cesadev);
}

module_init(cesadev_init);
module_exit(cesadev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ronen Shitrit");
MODULE_DESCRIPTION("Cesadev (user interface to CESA)");
