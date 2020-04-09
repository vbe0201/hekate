/*
 * Copyright (c) 2018 naehrwert
 * Copyright (c) 2018-2019 CTCaer
 * Copyright (c) 2018 balika011
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

#include <string.h>

#include "../sec/tsec.h"
#include "../sec/tsec_t210.h"
#include "../sec/se_t210.h"
#include "../soc/bpmp.h"
#include "../soc/clock.h"
#include "../soc/kfuse.h"
#include "../soc/smmu.h"
#include "../soc/t210.h"
#include "../mem/heap.h"
#include "../mem/mc.h"
#include "../utils/util.h"
#include "../hos/hos.h"

#include "../gfx/gfx.h"

static int _tsec_dma_wait_idle()
{
	u32 timeout = get_tmr_ms() + 10000;

	while (!(TSEC(TSEC_DMATRFCMD) & TSEC_DMATRFCMD_IDLE))
		if (get_tmr_ms() > timeout)
			return 0;

	return 1;
}

static int _tsec_dma_pa_to_internal_100(int not_imem, int i_offset, int pa_offset)
{
	u32 cmd;

	if (not_imem)
		cmd = TSEC_DMATRFCMD_SIZE_256B; // DMA 256 bytes
	else
		cmd = TSEC_DMATRFCMD_IMEM;      // DMA IMEM (Instruction memmory)

	TSEC(TSEC_DMATRFMOFFS) = i_offset;
	TSEC(TSEC_DMATRFFBOFFS) = pa_offset;
	TSEC(TSEC_DMATRFCMD) = cmd;

	return _tsec_dma_wait_idle();
}

int tsec_load_firmware(const void *firmware, size_t firmware_size)
{
	int res = 0;
	u8 *fwbuf = (u8 *)malloc(0x4000);

	// Enable clocks.
	clock_enable_host1x();
	usleep(2);
	clock_enable_tsec();
	clock_enable_sor_safe();
	clock_enable_sor0();
	clock_enable_sor1();
	clock_enable_kfuse();

	// Wait for KFUSE to initialize.
	kfuse_wait_ready();

	// Configure Falcon.
	TSEC(TSEC_DMACTL) = 0;
	TSEC(TSEC_IRQMSET) =
		TSEC_IRQMSET_EXT(0xFF) |
		TSEC_IRQMSET_WDTMR |
		TSEC_IRQMSET_HALT |
		TSEC_IRQMSET_EXTERR |
		TSEC_IRQMSET_SWGEN0 |
		TSEC_IRQMSET_SWGEN1;
	TSEC(TSEC_IRQDEST) =
		TSEC_IRQDEST_EXT(0xFF) |
		TSEC_IRQDEST_HALT |
		TSEC_IRQDEST_EXTERR |
		TSEC_IRQDEST_SWGEN0 |
		TSEC_IRQDEST_SWGEN1;
	TSEC(TSEC_ITFEN) = TSEC_ITFEN_CTXEN | TSEC_ITFEN_MTHDEN;

	if (!_tsec_dma_wait_idle())
	{
		res = -1;
		goto err;
	}

	// Align and load in the firmware.
	u8 *fwbuf_aligned = (u8 *)ALIGN((u32)fwbuf, 0x100);
	memcpy(fwbuf_aligned, firmware, firmware_size);
	TSEC(TSEC_DMATRFBASE) = (u32)fwbuf_aligned >> 8;

	// Configure DMA to transfer the physical buffer to Falcon.
	for (u32 addr = 0; addr < firmware_size; addr += 0x100)
	{
		if (!_tsec_dma_pa_to_internal_100(false, addr, addr))
		{
			res = -2;
			goto err;
		}
	}

	goto out;

err:;

	// Disable clocks.
	clock_disable_kfuse();
	clock_disable_sor1();
	clock_disable_sor0();
	clock_disable_sor_safe();
	clock_disable_tsec();

out:;

	free(fwbuf);
	return res;
}

int tsec_boot_firmware(u32 bootvector, u32 *mailbox0, u32 *mailbox1)
{
	int res = 0;

	// Configure Falcon and start the CPU.
	TSEC(TSEC_MAILBOX0) = (u32)mailbox0;
	TSEC(TSEC_MAILBOX1) = (u32)mailbox1;
	TSEC(TSEC_BOOTVEC) = bootvector;
	TSEC(TSEC_CPUCTL) = TSEC_CPUCTL_STARTCPU;

	if (!_tsec_dma_wait_idle())
	{
		res = -1;
		goto out;
	}

	// Wait for the CPU to be halted.
	while (TSEC(TSEC_CPUCTL) != TSEC_IRQMSET_HALT)
		;

	// Check if the CPU has crashed.
	u32 exception_info = TSEC(TSEC_EXCI);
	if (exception_info)
	{
		res = -3;

		// Print exception details.
		gfx_printf("Error during TSEC execution:\n");
		gfx_printf("  Program Counter:  %02X\n", exception_info & 0x80000);
		gfx_printf("  Exception Caluse: ");
		switch((exception_info >> 20) & 0xF)
		{
			case 0: gfx_printf("Trap0 \n");
			case 1: gfx_printf("Trap 1\n");
			case 2: gfx_printf("Trap 2\n");
			case 3: gfx_printf("Trap 3\n");
			case 4: gfx_printf("Invalid Opcode\n");
			case 5: gfx_printf("Authentication Entry\n");
			case 6: gfx_printf("Page Miss\n");
			case 7: gfx_printf("Multiple Page Miss\n");
			case 8: gfx_printf("Breakpoint Hit\n");
			default: gfx_printf("Unknown\n");
		}
	}

out:;

	// Disable clocks (which should be up because of tsec_load_firmware).
	clock_disable_kfuse();
	clock_disable_sor1();
	clock_disable_sor0();
	clock_disable_sor_safe();
	clock_disable_tsec();

	return res;
}

int tsec_query(u8 *tsec_keys, u8 kb, tsec_ctxt_t *tsec_ctxt)
{
	int res = 0;
	u8 *fwbuf = NULL;

	bpmp_mmu_disable();
	bpmp_clk_rate_set(BPMP_CLK_NORMAL);

	// Enable clocks.
	clock_enable_host1x();
	usleep(2);
	clock_enable_tsec();
	clock_enable_sor_safe();
	clock_enable_sor0();
	clock_enable_sor1();
	clock_enable_kfuse();

	kfuse_wait_ready();

	// Configure Falcon.
	TSEC(TSEC_DMACTL) = 0;
	TSEC(TSEC_IRQMSET) =
		TSEC_IRQMSET_EXT(0xFF) |
		TSEC_IRQMSET_WDTMR |
		TSEC_IRQMSET_HALT |
		TSEC_IRQMSET_EXTERR |
		TSEC_IRQMSET_SWGEN0 |
		TSEC_IRQMSET_SWGEN1;
	TSEC(TSEC_IRQDEST) =
		TSEC_IRQDEST_EXT(0xFF) |
		TSEC_IRQDEST_HALT |
		TSEC_IRQDEST_EXTERR |
		TSEC_IRQDEST_SWGEN0 |
		TSEC_IRQDEST_SWGEN1;
	TSEC(TSEC_ITFEN) = TSEC_ITFEN_CTXEN | TSEC_ITFEN_MTHDEN;
	if (!_tsec_dma_wait_idle())
	{
		res = -1;
		goto out;
	}

	// Load firmware.
	fwbuf = (u8 *)malloc(0x4000);
	u8 *fwbuf_aligned = (u8 *)ALIGN((u32)fwbuf, 0x100);
	memcpy(fwbuf_aligned, tsec_ctxt->fw, tsec_ctxt->size);
	TSEC(TSEC_DMATRFBASE) = (u32)fwbuf_aligned >> 8;

	for (u32 addr = 0; addr < tsec_ctxt->size; addr += 0x100)
	{
		if (!_tsec_dma_pa_to_internal_100(false, addr, addr))
		{
			res = -2;
			goto out_free;
		}
	}

	// Execute firmware.
	HOST1X(HOST1X_CH0_SYNC_SYNCPT_160) = 0x34C2E1DA;
	TSEC(TSEC_STATUS) = 0;
	TSEC(TSEC_BOOTKEYVER) = 1; // HOS uses key version 1.
	TSEC(TSEC_BOOTVEC) = 0;
	TSEC(TSEC_CPUCTL) = TSEC_CPUCTL_STARTCPU;

	if (!_tsec_dma_wait_idle())
	{
		res = -3;
		goto out_free;
	}

	u32 timeout = get_tmr_ms() + 2000;
	while (!TSEC(TSEC_STATUS))
		if (get_tmr_ms() > timeout)
		{
			res = -4;
			goto out_free;
		}

	if (TSEC(TSEC_STATUS) != 0xB0B0B0B0)
	{
		res = -5;
		goto out_free;
	}

	// Fetch result.
	HOST1X(HOST1X_CH0_SYNC_SYNCPT_160) = 0;

	u32 buf[4];
	buf[0] = SOR1(SOR_NV_PDISP_SOR_DP_HDCP_BKSV_LSB);
	buf[1] = SOR1(SOR_NV_PDISP_SOR_TMDS_HDCP_BKSV_LSB);
	buf[2] = SOR1(SOR_NV_PDISP_SOR_TMDS_HDCP_CN_MSB);
	buf[3] = SOR1(SOR_NV_PDISP_SOR_TMDS_HDCP_CN_LSB);

	SOR1(SOR_NV_PDISP_SOR_DP_HDCP_BKSV_LSB) = 0;
	SOR1(SOR_NV_PDISP_SOR_TMDS_HDCP_BKSV_LSB) = 0;
	SOR1(SOR_NV_PDISP_SOR_TMDS_HDCP_CN_MSB) = 0;
	SOR1(SOR_NV_PDISP_SOR_TMDS_HDCP_CN_LSB) = 0;

	memcpy(tsec_keys, &buf, 0x10);

out_free:;

	// Extract important result codes.
	tsec_ctxt->status = TSEC(TSEC_STATUS);
	tsec_ctxt->cmd_err = TSEC(0x1498);
	tsec_ctxt->exception_info = TSEC(TSEC_EXCI);

	free(fwbuf);

out:;

	// Disable clocks.
	clock_disable_kfuse();
	clock_disable_sor1();
	clock_disable_sor0();
	clock_disable_sor_safe();
	clock_disable_tsec();
	bpmp_mmu_enable();
	bpmp_clk_rate_set(BPMP_CLK_DEFAULT_BOOST);

	return res;
}
