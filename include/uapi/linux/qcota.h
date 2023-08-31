/*
 * Copyright (c) 2023 Qualcomm Innovation Center, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#ifndef _UAPI_QCOTA_H
#define _UAPI_QCOTA_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define QCE_OTA_MAX_BEARER   31
#define OTA_KEY_SIZE 16   /* 128 bits of keys. */
#define OTA_MAC_SIZE 4

enum qce_ota_dir_enum {
	QCE_OTA_DIR_UPLINK   = 0,
	QCE_OTA_DIR_DOWNLINK = 1,
	QCE_OTA_DIR_LAST
};

enum qce_ota_algo_enum {
	QCE_OTA_ALGO_SNOW3G = 0,
	QCE_OTA_ALGO_ZUC = 1,
	QCE_OTA_ALGO_LAST
};

/**
 * struct qce_f8_req - qce f8 request
 * @data_in:	packets input data stream to be ciphered.
 * @data_out:	ciphered packets output data.
 * @data_len:	length of data_in and data_out in bytes.
 * @last_bits:	number of partial bits of  last byte of data_in. Range 0-7.
 *		0 if last byte is full byte.
 * @count_c:	count-C, ciphering sequence number, 32 bit
 * @bearer:	5 bit of radio bearer identifier.
 * @ckey:	128 bits of confidentiality key,
 *		ckey[0] bit 127-120, ckey[1] bit 119-112,.., ckey[15] bit 7-0.
 * @direction:	uplink or donwlink.
 * @algorithm:	Zuc, or Snow3G.
 *
 */
struct qce_f8_req {
	__u8  *data_in;
	__u8  *data_out;
	__u16  data_len;
	__u8   last_bits;
	__u32  count_c;
	__u8   bearer;
	__u8   ckey[OTA_KEY_SIZE];
	enum qce_ota_dir_enum  direction;
	enum qce_ota_algo_enum algorithm;
};

/**
 * struct qce_f9_req - qce f9 request
 * @message:	message
 * @msize:	message size in bytes (include the last partial byte).
 * @last_bits:	number of partial bits of the last byte of message. Range 0-7.
 *		0 if last byte is full byte.
 * @mac_i:	4 byte message authentication code, to be returned.
 * @count_i:	32 bit count-I integrity sequence number.
 * @fresh_bearer: random 32 bit number, one per user.
 * @ikey:	128 bits of integrity key,
 *		ikey[0] bit 127-120, ikey[1] bit 119-112,.., ikey[15] bit 7-0.
 * @direction:	uplink or donwlink.
 * @algorithm:	Zuc, or Snow3G.
 */
struct qce_f9_req {
	__u8   *message;
	__u16   msize;
	__u8    last_bits;
	__u8    mac_i[OTA_MAC_SIZE];
	__u32   count_i;
	__u32   fresh_bearer;
	__u8    ikey[OTA_KEY_SIZE];
	enum qce_ota_dir_enum direction;
	enum qce_ota_algo_enum algorithm;
};

#define QCOTA_IOC_MAGIC     'G'

#define QCOTA_F8_REQ _IOWR(QCOTA_IOC_MAGIC, 1, struct qce_f8_req)

#define QCOTA_F9_REQ _IOWR(QCOTA_IOC_MAGIC, 2, struct qce_f9_req)

#define QCOTA_OPEN_EEA _IOWR(QCOTA_IOC_MAGIC, 3, enum qce_ota_algo_enum)

#define QCOTA_OPEN_EIA _IOWR(QCOTA_IOC_MAGIC, 4, enum qce_ota_algo_enum)

#define QCOTA_CLOSE_EEA _IOWR(QCOTA_IOC_MAGIC, 5, enum qce_ota_algo_enum)

#define QCOTA_CLOSE_EIA _IOWR(QCOTA_IOC_MAGIC, 6, enum qce_ota_algo_enum)

#endif /* _UAPI_QCOTA_H */
