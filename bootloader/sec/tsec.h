/*
* Copyright (c) 2018 naehrwert
* Copyright (c) 2018 CTCaer
*
* This program is free software; you can redistribute it and/or modify it
* under the terms and conditions of the GNU General Public License,
* version 2, as published by the Free Software Foundation.
*
* This program is distributed in the hope it will be useful, but WITHOUT
* ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
* FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
* more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _TSEC_H_
#define _TSEC_H_

#include "../utils/types.h"

typedef struct _tsec_ctxt_t
{
	void *fw;
	u32 size;
	void *pkg1;
	u32 pkg11_off;
	u32 secmon_base;
	u32 status;
	u32 cmd_err;
	u32 exception_info;
} tsec_ctxt_t;

int tsec_load_firmware(void *firmware, u32 size);
int tsec_boot_firmware(u32 bootvector);
int tsec_query(u8 *tsec_keys, u8 kb, tsec_ctxt_t *tsec_ctxt);

#endif
