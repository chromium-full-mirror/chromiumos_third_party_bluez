/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2022 Google LLC
 *
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 */

#include "metrics_allowlist.h"

static const int device_info_allow_list_sig[][2] = {
	/* Ericsson Technology Licensing */
	{ 0, 52 },
	/* Motorola */
	{ 8, 11905 },
	/* Qualcomm Technologies International, Ltd. (QTIL) */
	{ 10, 65535 },
	/* Texas Instruments Inc. */
	{ 13, 0 },
	/* Broadcom Corporation. */
	{ 15, 4608 },
	/* Qualcomm */
	{ 29, 4608 },
	/* Integrated System Solution Corp. */
	{ 57, 5028 },
	{ 57, 5506 },
	/* MediaTek, Inc. */
	{ 70, 4608 },
	/* Apple, Inc. */
	{ 76, 8194 },
	{ 76, 8198 },
	{ 76, 8201 },
	{ 76, 8203 },
	{ 76, 8204 },
	{ 76, 8206 },
	{ 76, 8207 },
	{ 76, 8208 },
	{ 76, 8211 },
	/* Harman International Industries, Inc. */
	{ 87, 35 },
	{ 87, 7977 },
	/* Realtek Semiconductor Corporation */
	{ 93, 8763 },
	/* Samsung Electronics Co. Ltd. */
	{ 117, 256 },
	{ 117, 40977 },
	{ 117, 40978 },
	{ 117, 40979 },
	/* Airoha Technology Corp. */
	{ 148, 4 },
	{ 148, 291 },
	/* LG Electronics​ */
	{ 196, 5025 },
	/* Google */
	{ 224, 0 },
	{ 224, 4608 },
	{ 224, 12288 },
	{ 224, 12289 },
	{ 224, 12290 },
	{ 224, 12291 },
	{ 224, 12292 },
	{ 224, 12544 },
	{ 224, 50181 },
	/* Amazon.com Services, LLC */
	{ 369, 384 },
	/* Bestechnic(Shanghai),Ltd */
	{ 688, 0 },
	/* LEGO System A/S */
	{ 919, 1 },
	/* Actions (Zhuhai) Technology Co., Limited */
	{ 992, 12298 },
	/* STABILO International */
	{ 1256, 32896 },
	/* GoerTek Dynaudio Co., Ltd. */
	{ 1452, 544 },
	/* Zhuhai Jieli technology Co.,Ltd */
	{ 1494, 10 }
};

static const int device_info_allow_list_usb[][2] = {
	/* Unknown */
	{ 14, 13330 },
	/* Unknown */
	{ 97, 1 },
	/* Unknown  */
	{ 125, 628 },
	/* HP, Inc */
	{ 1008, 2124 },
	{ 1008, 588 },
	/* Unknown  */
	{ 1014, 40961 },
	/* Microsoft Corp. */
	{ 1118, 736 },
	{ 1118, 765 },
	{ 1118, 1954 },
	{ 1118, 2053 },
	{ 1118, 2054 },
	{ 1118, 2087 },
	{ 1118, 2095 },
	{ 1118, 2326 },
	{ 1118, 2354 },
	{ 1118, 2397 },
	{ 1118, 2835 },
	{ 1118, 2848 },
	/* Primax Electronics, Ltd */
	{ 1121, 20206 },
	{ 1121, 20207 },
	/* Logitech, Inc. */
	{ 1133, 45072 },
	{ 1133, 45076 },
	{ 1133, 45077 },
	{ 1133, 45078 },
	{ 1133, 45081 },
	{ 1133, 45082 },
	{ 1133, 45083 },
	{ 1133, 45089 },
	{ 1133, 45091 },
	{ 1133, 45093 },
	{ 1133, 45094 },
	{ 1133, 45095 },
	{ 1133, 45883 },
	{ 1133, 45885 },
	{ 1133, 45890 },
	{ 1133, 45901 },
	{ 1133, 45915 },
	{ 1133, 45917 },
	{ 1133, 45890 },
	/* Samsung Electronics Co., Ltd */
	{ 1256, 28705 },
	/* Sony Corp. */
	{ 1356, 1476 },
	{ 1356, 2508 },
	{ 1356, 3302 },
	/* Wacom Co., Ltd */
	{ 1386, 887 },
	/* Nintendo Co., Ltd */
	{ 1406, 8198 },
	{ 1406, 8199 },
	{ 1406, 8201 },
	/* Apple, Inc. */
	{ 1452, 544 },
	{ 1452, 556 },
	{ 1452, 569 },
	{ 1452, 591 },
	{ 1452, 597 },
	{ 1452, 781 },
	{ 1452, 12850 },
	/* Zippy Technology Corp. */
	{ 2458, 1280 },
	/* Broadcom Corp. */
	{ 2652, 1 },
	{ 2652, 17667 },
	{ 2652, 63369 },
	/* Microdia */
	{ 3141, 32270 },
	/* Focusrite-Novation */
	{ 4661, 43554 },
	/* Razer USA, Ltd */
	{ 5426, 130 },
	/* Nordic Semiconductor ASA */
	{ 6421, 64 },
	/* Lab126, Inc. */
	{ 6473, 1026 },
	/* Anker Innovations Limited */
	{ 10522, 34050 },
	/* Unknown */
	{ 12994, 1 },
	/* Fuji Yusoki Kogyo Co., Ltd. */
	{ 44580, 34328 },

};

bool is_device_info_in_allowlist(int vendor_id_source, int vendor_id,
				 int product_id)
{
	int i;

	if (vendor_id_source == 1) {
		for (i = 0; i < sizeof(device_info_allow_list_sig) /
					sizeof(device_info_allow_list_sig[0]);
		     i++) {
			if (vendor_id == device_info_allow_list_sig[i][0] &&
			    product_id == device_info_allow_list_sig[i][1])
				return true;
		}
	} else if (vendor_id_source == 2) {
		for (i = 0; i < sizeof(device_info_allow_list_usb) /
					sizeof(device_info_allow_list_usb[0]);
		     i++) {
			if (vendor_id == device_info_allow_list_usb[i][0] &&
			    product_id == device_info_allow_list_usb[i][1])
				return true;
		}
	}

	return false;
}
