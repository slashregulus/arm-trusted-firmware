/*
 * Copyright (c) 2013-2014, ARM Limited and Contributors. All rights reserved.
 * Copyright (c) 2015-2016, Renesas Electronics Corporation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of ARM nor the names of its contributors may be used
 * to endorse or promote products derived from this software without specific
 * prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <bakery_lock.h>
#include <mmio.h>
#include <debug.h>
#include <arch.h>
#include "../../rcar_def.h"
#include "../../rcar_private.h"
#include "rcar_pwrc.h"

/*
 * TODO: Someday there will be a generic power controller api. At the moment
 * each platform has its own pwrc so just exporting functions is fine.
 */
#if USE_COHERENT_MEM
static bakery_lock_t pwrc_lock __attribute__ ((section("tzfw_coherent_mem")));
#define PWRC_LOCK	(&pwrc_lock)
#else
#define PWRC_LOCK	ERROR("not use coherent memory");	\
			panic();
#endif

#define	WUP_IRQ_SHIFT	(0U)
#define	WUP_FIQ_SHIFT	(8U)
#define	WUP_CSD_SHIFT	(16U)

#define	BIT_CA53_SCU	((uint32_t)1U<<21)
#define	BIT_CA57_SCU	((uint32_t)1U<<12)
#define	REQ_RESUME	((uint32_t)1U<<1)
#define	REQ_OFF		((uint32_t)1U<<0)
#define	STATUS_PWRUP	((uint32_t)1U<<1)
#define	STATUS_PWRDOWN	((uint32_t)1U<<0)

#define	STATE_CA57_CPU	(27U)
#define	STATE_CA53_CPU	(22U)

static void SCU_power_up(uint64_t mpidr);

#if 0
uint32_t rcar_pwrc_get_cpu_wkr(uint64_t mpidr)
{
	return PSYSR_WK(rcar_pwrc_read_psysr(mpidr));
}
#endif
uint32_t rcar_pwrc_status(uint64_t mpidr)
{
	uint32_t rc;
	uint64_t cpu_no;
	uint32_t prr_data;

	rcar_lock_get(PWRC_LOCK);
	prr_data = mmio_read_32((uintptr_t)RCAR_PRR);
	cpu_no = mpidr & (uint64_t)MPIDR_CPU_MASK;
	if ((mpidr & ((uint64_t)MPIDR_CLUSTER_MASK)) != 0U) {
		/* A53 side				*/
		if ((prr_data & ((uint32_t)1U << (STATE_CA53_CPU + cpu_no))) == 0U) {
			rc = 0U;
		} else {
			rc = RCAR_INVALID;
		}
	} else {
		/* A57 side				*/
		if ((prr_data & ((uint32_t)1U << (STATE_CA57_CPU + cpu_no))) == 0U) {
			rc = 0U;
		} else {
			rc = RCAR_INVALID;
		}
	}
	NOTICE("BL31: - rcar_pwrc_read_psysr : rc=0x%x\n", rc);
	rcar_lock_release(PWRC_LOCK);

	return rc;
}

void rcar_pwrc_cpuon(uint64_t mpidr)
{

	uintptr_t res_reg;
	uint32_t res_data;
	uintptr_t on_reg;
	uint64_t cpu_no;
	uint32_t upper_value;

	rcar_lock_get(PWRC_LOCK);

	cpu_no = mpidr & (uint64_t)MPIDR_CPU_MASK;
	if ((mpidr & ((uint64_t)MPIDR_CLUSTER_MASK)) != 0U) {
		/* A53 side				*/
		res_reg = (uintptr_t)RCAR_CA53RESCNT;
		on_reg = (uintptr_t)RCAR_CA53WUPCR;
		upper_value = 0x5A5A0000U;
	} else {
		res_reg = (uintptr_t)RCAR_CA57RESCNT;
		on_reg = (uintptr_t)RCAR_CA57WUPCR;
		upper_value = 0xA5A50000U;
	}
	SCU_power_up(mpidr);
	mmio_write_32(RCAR_CPGWPR, ~((uint32_t)((uint32_t)1U << cpu_no)));
	mmio_write_32(on_reg, (uint32_t)((uint32_t)1U << cpu_no));
	res_data = mmio_read_32(res_reg) | upper_value;
	mmio_write_32(res_reg, (res_data & (~((uint32_t)1U << (3U - cpu_no)))));
	rcar_lock_release(PWRC_LOCK);
}

static void SCU_power_up(uint64_t mpidr)
{
	uint32_t reg_SYSC_bit;
	uintptr_t reg_PWRONCR;
	volatile uintptr_t reg_PWRER;
	uintptr_t reg_PWRSR;
	uintptr_t reg_SYSCIER =	(uintptr_t)RCAR_SYSCIER;
	uintptr_t reg_SYSCIMR =	(uintptr_t)RCAR_SYSCIMR;
	volatile uintptr_t reg_SYSCSR =	(volatile uintptr_t)RCAR_SYSCSR;
	volatile uintptr_t reg_SYSCISR = (volatile uintptr_t)RCAR_SYSCISR;
	volatile uintptr_t reg_SYSCISCR = (volatile uintptr_t)RCAR_SYSCISCR;

	if ((mpidr & ((uint64_t)MPIDR_CLUSTER_MASK)) == 0U) {
		/* CA57-SCU	*/
		reg_SYSC_bit = (uint32_t)BIT_CA57_SCU;
		reg_PWRONCR = (uintptr_t)RCAR_PWRONCR5;
		reg_PWRER = (volatile uintptr_t)RCAR_PWRER5;
		reg_PWRSR = (uintptr_t)RCAR_PWRSR5;
	} else {
		/* CA53-SCU	*/
		reg_SYSC_bit = (uint32_t)BIT_CA53_SCU;
		reg_PWRONCR = (uintptr_t)RCAR_PWRONCR3;
		reg_PWRER = (volatile uintptr_t)RCAR_PWRER3;
		reg_PWRSR = (uintptr_t)RCAR_PWRSR3;
	}
	if ((mmio_read_32(reg_PWRSR) & (uint32_t)STATUS_PWRDOWN) != 0x0000U) {
		/* set SYSCIER and SYSCIMR		*/
		mmio_write_32(reg_SYSCIER, (mmio_read_32(reg_SYSCIER) | reg_SYSC_bit));
		mmio_write_32(reg_SYSCIMR, (mmio_read_32(reg_SYSCIMR) | reg_SYSC_bit));
		do {
			/* SYSCSR[1]=1?				*/
			while ((mmio_read_32(reg_SYSCSR) & (uint32_t)REQ_RESUME) == 0U) {
				;
			}
			/* If SYSCSR[1]=1 then set bit in PWRONCRn to 1	*/
			mmio_write_32(reg_PWRONCR, 0x0001U);
		} while ((mmio_read_32(reg_PWRER) & 0x0001U) != 0U);

		/* bit in SYSCISR=1 ?				*/
		while ((mmio_read_32(reg_SYSCISR) & reg_SYSC_bit) == 0U) {
			;
		}
		/* clear bit in SYSCISR				*/
		mmio_write_32(reg_SYSCISCR, reg_SYSC_bit);
	}
}

void rcar_pwrc_cpuoff(uint64_t mpidr)
{
#if 0
	uint32_t *off_reg;
	uint64_t cpu_no;

	rcar_lock_get(PWRC_LOCK);
	cpu_no = mpidr & (uint64_t)MPIDR_CPU_MASK;
	if ((mpidr & (uint64_t)MPIDR_CLUSTER_MASK) != 0U) {
		/* A53 side				*/
		off_reg = (uintptr_t)RCAR_CA53CPU0CR;
	} else {
		/* A57 side				*/
		off_reg = (uintptr_t)RCAR_CA57CPU0CR;
	}
	mmio_write_32(off_reg+(cpu_no*10), (uint32_t)0x03U);
	wfi();
	rcar_lock_release(PWRC_LOCK);
#endif
}

void rcar_pwrc_enable_interrupt_wakeup(uint64_t mpidr)
{
	uintptr_t reg;
	uint64_t cpu_no;
	uint32_t shift_irq;
	uint32_t shift_fiq;

	rcar_lock_get(PWRC_LOCK);
	cpu_no = mpidr & (uint64_t)MPIDR_CPU_MASK;
	if ((mpidr & ((uint64_t)MPIDR_CLUSTER_MASK)) != 0U) {
		/* A53 side				*/
		reg = (uintptr_t)RCAR_WUPMSKCA53;
	} else {
		/* A57 side				*/
		reg = (uintptr_t)RCAR_WUPMSKCA57;
	}
	shift_irq = WUP_IRQ_SHIFT + (uint32_t)cpu_no;
	shift_fiq = WUP_FIQ_SHIFT + (uint32_t)cpu_no;
	mmio_write_32(reg, (uint32_t)((~((uint32_t)1U << shift_irq)) & (~((uint32_t)1U << shift_fiq))));
	rcar_lock_release(PWRC_LOCK);
}

void rcar_pwrc_disable_interrupt_wakeup(uint64_t mpidr)
{
	uintptr_t reg;
	uint64_t cpu_no;
	uint32_t shift_irq;
	uint32_t shift_fiq;

	rcar_lock_get(PWRC_LOCK);
	cpu_no = mpidr & (uint64_t)MPIDR_CPU_MASK;
	if ((mpidr & ((uint64_t)MPIDR_CLUSTER_MASK)) != 0U) {
		/* A53 side				*/
		reg = (uintptr_t)RCAR_WUPMSKCA53;
	} else {
		/* A57 side				*/
		reg = (uintptr_t)RCAR_WUPMSKCA57;
	}
	shift_irq = WUP_IRQ_SHIFT + (uint32_t)cpu_no;
	shift_fiq = WUP_FIQ_SHIFT + (uint32_t)cpu_no;
	mmio_write_32(reg, (uint32_t)(((uint32_t)1U << shift_irq) | ((uint32_t)1U << shift_fiq)));
	rcar_lock_release(PWRC_LOCK);
}

#if 0
void rcar_pwrc_clusteroff(uint64_t mpidr)
{
	uintptr_t off_reg;

	rcar_lock_get(PWRC_LOCK);
	if ((mpidr & (uint64_t)MPIDR_CLUSTER_MASK) != 0U) {
		/* A53 side				*/
		off_reg = (uintptr_t)RCAR_CA53CPUCMCR;
	} else {
		/* A57 side				*/
		off_reg = (uintptr_t)RCAR_CA57CPUCMCR;
	}
	mmio_write_32(off_reg, (uint32_t)0x03U);
	rcar_lock_release(PWRC_LOCK);
}
#endif

/* Nothing else to do here apart from initializing the lock */
void rcar_pwrc_setup(void)
{
	rcar_lock_init(PWRC_LOCK);
}