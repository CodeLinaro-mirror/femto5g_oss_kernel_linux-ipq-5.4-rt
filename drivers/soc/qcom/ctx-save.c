// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020, The Linux Foundation. All rights reserved.
*/
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <linux/utsname.h>
#include <linux/sizes.h>
#include <soc/qcom/ctx-save.h>
#include <linux/spinlock.h>
#include <linux/pfn.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mhi.h>
#include <linux/sysrq.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/init.h>
#include <uapi/linux/major.h>
#include <linux/highmem.h>
#include <linux/ioctl.h>

typedef struct ctx_save_tlv_msg {
	unsigned char *msg_buffer;
	unsigned char *cur_msg_buffer_pos;
	unsigned int len;
	spinlock_t spinlock;
	bool is_panic;
} ctx_save_tlv_msg_t;

struct ctx_save_props {
	unsigned int tlv_msg_offset;
	unsigned int crashdump_page_size;
};

ctx_save_tlv_msg_t tlv_msg;

/*
* Function: ctx_save_replace_tlv
* Description: Adds dump segment as a TLV into the global crashdump
* buffer at specified offset.
*
* @param:	[in] type - Type associated with Dump segment
*		[in] size - Size associted with Dump segment
*		[in] data - Physical address of the Dump segment
*		[in] offset - offset at which TLV entry is added to the crashdump
*		buffer
*
* Return: 0 on success, -ENOBUFS on failure
*/
int ctx_save_replace_tlv(unsigned char type, unsigned int size, const char *data, unsigned char *offset)
{
	unsigned char *x;
	unsigned char *y;
	unsigned long flags;

	if (!tlv_msg.msg_buffer) {
		return -ENOMEM;
	}

	spin_lock_irqsave(&tlv_msg.spinlock, flags);
	x = offset;
	y = tlv_msg.msg_buffer + tlv_msg.len;

	if ((x + CTX_SAVE_SCM_TLV_TYPE_LEN_SIZE + size) >= y) {
		spin_unlock_irqrestore(&tlv_msg.spinlock, flags);
		return -ENOBUFS;
	}

	x[0] = type;
	x[1] = size;
	x[2] = size >> 8;

	memcpy(x + 3, data, size);
	spin_unlock_irqrestore(&tlv_msg.spinlock, flags);

	return 0;
}
/*
* Function: ctx_save_add_tlv
* Description: Appends dump segment as a TLV entry to the end of the
* global crashdump buffer.
*
* @param: 	[in] type - Type associated with Dump segment
*		[in] size - Size associated with Dump segment
*		[in] data - Physical address of the Dump segment
*
* Return: 0 on success, -ENOBUFS on failure
*/
int ctx_save_add_tlv(unsigned char type, unsigned int size, const char *data)
{
	unsigned char *x;
	unsigned char *y;
	unsigned long flags;

	if (!tlv_msg.msg_buffer) {
		return -ENOMEM;
	}

	spin_lock_irqsave(&tlv_msg.spinlock, flags);
	x = tlv_msg.cur_msg_buffer_pos;
	y = tlv_msg.msg_buffer + tlv_msg.len;

	if ((x + CTX_SAVE_SCM_TLV_TYPE_LEN_SIZE + size) >= y) {
		spin_unlock_irqrestore(&tlv_msg.spinlock, flags);
		return -ENOBUFS;
	}

	x[0] = type;
	x[1] = size;
	x[2] = size >> 8;

	memcpy(x + 3, data, size);

	tlv_msg.cur_msg_buffer_pos +=
		(size + CTX_SAVE_SCM_TLV_TYPE_LEN_SIZE);

	spin_unlock_irqrestore(&tlv_msg.spinlock, flags);
	return 0;
}

/*
* Function: ctx_save_fill_log_dump_tlv
* Description: Add 'static' dump segments - uname, demsg,
* page global directory, linux buffer and metadata text
* file to the global crashdump buffer
*
*
* Return: 0 on success, -ENOBUFS on failure
*/
static int ctx_save_fill_log_dump_tlv(void)
{
	struct new_utsname *uname;
	int ret_val;

	uname = utsname();

	ret_val = ctx_save_add_tlv(CTX_SAVE_LOG_DUMP_TYPE_UNAME,
			    sizeof(*uname),
			    (unsigned char *)uname);
	if (ret_val)
		return ret_val;

	if (tlv_msg.cur_msg_buffer_pos >=
		tlv_msg.msg_buffer + tlv_msg.len)
	return -ENOBUFS;

	return 0;
}

static int ctx_save_panic_handler(struct notifier_block *nb,
				  unsigned long event, void *ptr)
{
	tlv_msg.is_panic = true;
	return NOTIFY_DONE;
}

static struct notifier_block panic_nb = {
	.notifier_call = ctx_save_panic_handler,
};

static int ctx_save_probe(struct platform_device *pdev)
{
	void *scm_regsave;
	const struct ctx_save_props *prop = device_get_match_data(&pdev->dev);
	int ret;

	if (!prop)
		return -ENODEV;

	scm_regsave = (void *) __get_free_pages(GFP_KERNEL,
				get_order(prop->crashdump_page_size));

	if (!scm_regsave)
		return -ENOMEM;

	ret = qti_scm_regsave(SCM_SVC_UTIL, SCM_CMD_SET_REGSAVE,
			scm_regsave, prop->crashdump_page_size);

	if (ret) {
		pr_err("Setting register save address failed.\n"
			"Registers won't be dumped on a dog bite\n");
		return ret;
	}

	spin_lock_init(&tlv_msg.spinlock);
	tlv_msg.msg_buffer = scm_regsave + prop->tlv_msg_offset;
	tlv_msg.cur_msg_buffer_pos = tlv_msg.msg_buffer;
	tlv_msg.len = prop->crashdump_page_size -
				 prop->tlv_msg_offset;
	ret = ctx_save_fill_log_dump_tlv();

	/* if failed, we still return 0 because it should not
	 * affect the boot flow. The return value 0 does not
	 * necessarily indicate success in this function.
	 */
	if (ret) {
		pr_err("log dump initialization failed\n");
		return 0;
	}

	ret = atomic_notifier_chain_register(&panic_notifier_list, &panic_nb);

	if (ret)
		dev_err(&pdev->dev,
			"Failed to register panic notifier\n");

	return ret;
}

const struct ctx_save_props ctx_save_props_ipq5018 = {
	.tlv_msg_offset = (1012 * SZ_1K),
	/* As SBL overwrites the NSS IMEM, TZ has to copy it to some memory
	 * on crash before it restarts the system. Hence, reserving of 384K
	 * is required to copy the NSS IMEM before restart is done.
	 * So that TZ can dump NSS dump data after the first 8K.
	 *
	 * get_order function returns the next higher order as output,
	 * so when we pass 392K(8K for regsave + 384K for NSS IMEM) as argument
	 * 512K will be allocated.
	 *
	 * 3K is required for DCC regsave memory.
	 * 82K is unused currently and can be used based on future needs.
	 * 12K is used for crashdump TLV buffer for Minidump feature.
	 */

	/*
	 * The memory is allocated using alloc_pages, hence it will be in
	 * power of 2. The unused memory is the result of using alloc_pages.
	 * As we need contigous memory for > 256K we have to use alloc_pages.
	 *
	 *		 ---------------
	 *		|      8K	|
	 *		|    regsave	|
	 *		 ---------------
	 *		|		|
	 *		|     192K	|
	 *		|    NSS IMEM	|
	 *		|		|
	 *		|		|
	 *		 ---------------
	 *		|     352 K     |
	 *		|    BTSS RAM   |
	 *		 ---------------
	 *		|    3K - DCC	|
	 *		 ---------------
	 *		|		|
	 *		|     457K	|
	 *		|    Unused	|
	 *		|		|
	 *		 ---------------
	 *		|     12 K     |
	 *		|   TLV Buffer |
	 *		 ---------------
	 *
	 */
	.crashdump_page_size = (SZ_8K + (192 * SZ_1K) + (352 * SZ_1K) +
				(3 * SZ_1K) + (457 * SZ_1K) + (12 * SZ_1K)),
};

const struct ctx_save_props ctx_save_props_ipq5332 = {
	.tlv_msg_offset = (500 * SZ_1K),

	/* 300K for TME-L Crashdump
	 * 8K for regsave
	 * 192K is unused currently and can be used based on future needs.
	 * 12K is used for crashdump TLV buffer for Minidump feature.
	 *
	 * get_order function returns the next higher order as output,
	 * so when we pass 320K as argument 512K will be allocated.
	 *
	 * The memory is allocated using alloc_pages, hence it will be in
	 * power of 2. The unused memory is the result of using alloc_pages.
	 * As we need contigous memory for > 256K we have to use alloc_pages.
	 *
	 *              -----------------
	 *              |           	|
	 *              |      300K	|
	 *              |    TMEL ctxt  |
	 *              |               |
	 *              -----------------
	 *              |     8K        |
	 *              |    regsave    |
	 *              |               |
	 *              -----------------
	 *              |               |
	 *              |     192K      |
	 *              |    Unused     |
	 *              |               |
	 *              -----------------
	 *              |     12 K      |
	 *              |   TLV Buffer  |
	 *		 ---------------
	 *
	 */
	.crashdump_page_size = ((300 * SZ_1K) + (8 * SZ_1K) + (192 * SZ_1K) +
				(12 * SZ_1K)),
};

const struct ctx_save_props ctx_save_props_ipq6018 = {
	.tlv_msg_offset = (244 * SZ_1K),
	/* As XBL overwrites the NSS UTCM, TZ has to copy it to some memory
	 * on crash before it restarts the system. Hence, reserving of 192K
	 * is required to copy the NSS UTCM before restart is done.
	 * So that TZ can dump NSS dump data after the first 8K.
	 *
	 * 3K for DCC Memory
	 *
	 * get_order function returns the next higher order as output,
	 * so when we pass 203K as argument 256K will be allocated.
	 * 41K is unused currently and can be used based on future needs.
	 *
	 * 12K is used for crashdump TLV buffer for Minidump feature.
	 * For minidump feature, last 16K of crashdump page size is used for
	 * TLV buffer in the case of ipq807x. Same offset (last 16 K from end
	 * of crashdump page) is used for ipq60xx as well, to keep design
	 * consistent.
	 *
	 *
	 * The memory is allocated using alloc_pages, hence it will be in
	 * power of 2. The unused memory is the result of using alloc_pages.
	 * As we need contigous memory for > 256K we have to use alloc_pages.
	 *
	 *		 ---------------
	 *		|      8K	|
	 *		|    regsave	|
	 *		 ---------------
	 *		|		|
	 *		|     192K	|
	 *		|    NSS UTCM	|
	 *		|		|
	 *		|		|
	 *		 ---------------
	 *		|    3K - DCC	|
	 *		 ---------------
	 *		|		|
	 *		|     41K	|
	 *		|    Unused	|
	 *		|		|
	 *		 ---------------
	 *		|     12 K     |
	 *		|   TLV Buffer |
	 *		---------------
	 *
	 */
	.crashdump_page_size = (SZ_8K + (192 * SZ_1K) + (3 * SZ_1K) +
				(41 * SZ_1K) + (12 * SZ_1K)),
};

const struct ctx_save_props ctx_save_props_ipq807x = {
	.tlv_msg_offset = (500 * SZ_1K),
	/* As SBL overwrites the NSS IMEM, TZ has to copy it to some memory
	 * on crash before it restarts the system. Hence, reserving of 384K
	 * is required to copy the NSS IMEM before restart is done.
	 * So that TZ can dump NSS dump data after the first 8K.
	 * Additionally 8K memory is allocated which can be used by TZ
	 * to dump PMIC memory.
	 * get_order function returns the next higher order as output,
	 * so when we pass 400K as argument 512K will be allocated.
	 * 3K is required for DCC regsave memory.
	 * 15K is required for CPR.
	 * 82K is unused currently and can be used based on future needs.
	 * 12K is used for crashdump TLV buffer for Minidump feature.
	 *
	 * The memory is allocated using alloc_pages, hence it will be in
	 * power of 2. The unused memory is the result of using alloc_pages.
	 * As we need contigous memory for > 256K we have to use alloc_pages.
	 *
	 *		*---------------*
	 *		|      8K	|
	 *		|    regsave	|
	 *		*---------------*
	 *		|		|
	 *		|     384K	|
	 *		|    NSS IMEM	|
	 *		|		|
	 *		|		|
	 *		*---------------*
	 *		|      8K	|
	 *		|    PMIC mem	|
	 *		*---------------*
	 *		|    3K - DCC	|
	 *		|		|
	 *		*---------------*
	 *		|      15K      |
	 *		|    CPR Reg    |
	 *		* --------------*
	 *		|		|
	 *		|     82K	|
	 *		|    Unused	|
	 *		|		|
	 *		* --------------*
	 *		|     12 K      |
	 *		|   TLV Buffer  |
	 *		*---------------*
	 *
	 */
	.crashdump_page_size = (SZ_8K + (384 * SZ_1K) + (SZ_8K) + (3 * SZ_1K) +
				(15 * SZ_1K) + (82 * SZ_1K) + (12 * SZ_1K)),
};

const struct ctx_save_props ctx_save_props_ipq9574 = {
	.tlv_msg_offset = (500 * SZ_1K),

	/* Allocating 300K for TME-L Crashdump
	 * 80K for regsave
	 * 3K for DCC Memory
	 * 117K is unused currently and can be used based on future needs.
	 * 12K is used for crashdump TLV buffer for Minidump feature.
	 *
	 * get_order function returns the next higher order as output,
	 * so when we pass 395K as argument 512K will be allocated.
	 *
	 * The memory is allocated using alloc_pages, hence it will be in
	 * power of 2. The unused memory is the result of using alloc_pages.
	 * As we need contigous memory for > 256K we have to use alloc_pages.
	 *
	 *              -----------------
	 *              |           	|
	 *              |      300K	|
	 *              |    TMEL ctxt  |
	 *              |               |
	 *              |               |
	 *              -----------------
	 *              |     80K       |
	 *              |    regsave    |
	 *              |               |
	 *              -----------------
	 *              |    3K - DCC   |
	 *              -----------------
	 *              |               |
	 *              |     117K      |
	 *              |    Unused     |
	 *              |               |
	 *              -----------------
	 *              |     12 K      |
	 *              |   TLV Buffer  |
	 *              -----------------
	 *
	 */
	.crashdump_page_size = ((300 * SZ_1K) + (80 * SZ_1K) + (3 * SZ_1K) +
				(117 * SZ_1K) + (12 * SZ_1K)),
};

static const struct of_device_id ctx_save_of_table[] = {
	{
		.compatible = "qti,ctxt-save-ipq5018",
		.data = (void *)&ctx_save_props_ipq5018,
	},
	{
		.compatible = "qti,ctxt-save-ipq5332",
		.data = (void *)&ctx_save_props_ipq5332,
	},
	{
		.compatible = "qti,ctxt-save-ipq6018",
		.data = (void *)&ctx_save_props_ipq6018,
	},
	{
		.compatible = "qti,ctxt-save-ipq8074",
		.data = (void *)&ctx_save_props_ipq807x,
	},
	{
		.compatible = "qti,ctxt-save-ipq9574",
		.data = (void *)&ctx_save_props_ipq9574,
	},
	{}
};

static struct platform_driver ctx_save_driver = {
	.probe = ctx_save_probe,
	.driver = {
		.name = "qti_ctx_save_driver",
		.of_match_table = ctx_save_of_table,
	},
};
module_platform_driver(ctx_save_driver);

MODULE_DESCRIPTION("QTI context save driver for storing cpu regs, etc");
MODULE_LICENSE("GPL v2");
