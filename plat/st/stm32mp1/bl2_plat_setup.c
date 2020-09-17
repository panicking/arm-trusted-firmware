/*
 * Copyright (c) 2015-2021, ARM Limited and Contributors. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <assert.h>
#include <errno.h>
#include <string.h>

#include <arch_helpers.h>
#include <common/bl_common.h>
#include <common/debug.h>
#include <common/desc_image_load.h>
#include <drivers/generic_delay_timer.h>
#include <drivers/mmc.h>
#include <drivers/st/bsec.h>
#include <drivers/st/stm32_iwdg.h>
#include <drivers/st/stm32_uart.h>
#include <drivers/st/stm32mp1_clk.h>
#include <drivers/st/stm32mp1_pwr.h>
#include <drivers/st/stm32mp1_ram.h>
#include <drivers/st/stm32mp_pmic.h>
#include <lib/fconf/fconf.h>
#include <lib/fconf/fconf_dyn_cfg_getter.h>
#include <lib/mmio.h>
#include <lib/optee_utils.h>
#include <lib/xlat_tables/xlat_tables_v2.h>
#include <plat/common/platform.h>

#include <platform_def.h>
#include <stm32mp1_dbgmcu.h>

static struct stm32mp_auth_ops stm32mp1_auth_ops;

static void print_reset_reason(void)
{
	uint32_t rstsr = mmio_read_32(stm32mp_rcc_base() + RCC_MP_RSTSCLRR);

	if (rstsr == 0U) {
		WARN("Reset reason unknown\n");
		return;
	}

	INFO("Reset reason (0x%x):\n", rstsr);

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) == 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_STDBYRSTF) != 0U) {
			INFO("System exits from STANDBY\n");
			return;
		}

		if ((rstsr & RCC_MP_RSTSCLRR_CSTDBYRSTF) != 0U) {
			INFO("MPU exits from CSTANDBY\n");
			return;
		}
	}

	if ((rstsr & RCC_MP_RSTSCLRR_PORRSTF) != 0U) {
		INFO("  Power-on Reset (rst_por)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_BORRSTF) != 0U) {
		INFO("  Brownout Reset (rst_bor)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MCSYSRSTF) != 0U) {
		if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
			INFO("  System reset generated by MCU (MCSYSRST)\n");
		} else {
			INFO("  Local reset generated by MCU (MCSYSRST)\n");
		}
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPSYSRSTF) != 0U) {
		INFO("  System reset generated by MPU (MPSYSRST)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_HCSSRSTF) != 0U) {
		INFO("  Reset due to a clock failure on HSE\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG1RSTF) != 0U) {
		INFO("  IWDG1 Reset (rst_iwdg1)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_IWDG2RSTF) != 0U) {
		INFO("  IWDG2 Reset (rst_iwdg2)\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPUP0RSTF) != 0U) {
		INFO("  MPU Processor 0 Reset\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_MPUP1RSTF) != 0U) {
		INFO("  MPU Processor 1 Reset\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_PADRSTF) != 0U) {
		INFO("  Pad Reset from NRST\n");
		return;
	}

	if ((rstsr & RCC_MP_RSTSCLRR_VCORERSTF) != 0U) {
		INFO("  Reset due to a failure of VDD_CORE\n");
		return;
	}

	ERROR("  Unidentified reset reason\n");
}

void bl2_el3_early_platform_setup(u_register_t arg0,
				  u_register_t arg1 __unused,
				  u_register_t arg2 __unused,
				  u_register_t arg3 __unused)
{
	stm32mp_save_boot_ctx_address(arg0);
}

void bl2_platform_setup(void)
{
	int ret;

	ret = stm32mp1_ddr_probe();
	if (ret < 0) {
		ERROR("Invalid DDR init: error %d\n", ret);
		panic();
	}

	/* Map DDR for binary load, now with cacheable attribute */
	ret = mmap_add_dynamic_region(STM32MP_DDR_BASE, STM32MP_DDR_BASE,
				      STM32MP_DDR_MAX_SIZE, MT_MEMORY | MT_RW | MT_SECURE);
	if (ret < 0) {
		ERROR("DDR mapping: error %d\n", ret);
		panic();
	}

#if STM32MP_USE_STM32IMAGE
#ifdef AARCH32_SP_OPTEE
	INFO("BL2 runs OP-TEE setup\n");
#else
	INFO("BL2 runs SP_MIN setup\n");
#endif
#endif /* STM32MP_USE_STM32IMAGE */
}

void bl2_el3_plat_arch_setup(void)
{
	int32_t result;
	const char *board_model;
	boot_api_context_t *boot_context =
		(boot_api_context_t *)stm32mp_get_boot_ctx_address();
	uintptr_t pwr_base;
	uintptr_t rcc_base;

	mmap_add_region(BL_CODE_BASE, BL_CODE_BASE,
			BL_CODE_END - BL_CODE_BASE,
			MT_CODE | MT_SECURE);

#if STM32MP_USE_STM32IMAGE
#ifdef AARCH32_SP_OPTEE
	mmap_add_region(STM32MP_OPTEE_BASE, STM32MP_OPTEE_BASE,
			STM32MP_OPTEE_SIZE,
			MT_MEMORY | MT_RW | MT_SECURE);
#else
	/* Prevent corruption of preloaded BL32 */
	mmap_add_region(BL32_BASE, BL32_BASE,
			BL32_LIMIT - BL32_BASE,
			MT_RO_DATA | MT_SECURE);
#endif
#endif /* STM32MP_USE_STM32IMAGE */

	/* Prevent corruption of preloaded Device Tree */
	mmap_add_region(DTB_BASE, DTB_BASE,
			DTB_LIMIT - DTB_BASE,
			MT_RO_DATA | MT_SECURE);

	configure_mmu();

	if (dt_open_and_check(STM32MP_DTB_BASE) < 0) {
		panic();
	}

	pwr_base = stm32mp_pwr_base();
	rcc_base = stm32mp_rcc_base();

	/*
	 * Disable the backup domain write protection.
	 * The protection is enable at each reset by hardware
	 * and must be disabled by software.
	 */
	mmio_setbits_32(pwr_base + PWR_CR1, PWR_CR1_DBP);

	while ((mmio_read_32(pwr_base + PWR_CR1) & PWR_CR1_DBP) == 0U) {
		;
	}

	if (bsec_probe() != 0) {
		panic();
	}

	/* Reset backup domain on cold boot cases */
	if ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_RTCSRC_MASK) == 0U) {
		mmio_setbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);

		while ((mmio_read_32(rcc_base + RCC_BDCR) & RCC_BDCR_VSWRST) ==
		       0U) {
			;
		}

		mmio_clrbits_32(rcc_base + RCC_BDCR, RCC_BDCR_VSWRST);
	}

	/* Disable MCKPROT */
	mmio_clrbits_32(rcc_base + RCC_TZCR, RCC_TZCR_MCKPROT);

	generic_delay_timer_init();

#if STM32MP_UART_PROGRAMMER
	/* Disable programmer UART before changing clock tree */
	if (boot_context->boot_interface_selected ==
	    BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_UART) {
		uintptr_t uart_prog_addr =
			get_uart_address(boot_context->boot_interface_instance);

		stm32_uart_stop(uart_prog_addr);
	}
#endif
	if (stm32mp1_clk_probe() < 0) {
		panic();
	}

	if (stm32mp1_clk_init() < 0) {
		panic();
	}

	stm32_save_boot_interface(boot_context->boot_interface_selected,
				  boot_context->boot_interface_instance);

#if STM32MP_USB_PROGRAMMER
	/* Deconfigure all UART RX pins configured by ROM code */
	stm32mp1_deconfigure_uart_pins();
#endif

	if (stm32mp_uart_console_setup() != 0) {
		goto skip_console_init;
	}

	stm32mp_print_cpuinfo();

	board_model = dt_get_board_model();
	if (board_model != NULL) {
		NOTICE("Model: %s\n", board_model);
	}

	stm32mp_print_boardinfo();

	if (boot_context->auth_status != BOOT_API_CTX_AUTH_NO) {
		NOTICE("Bootrom authentication %s\n",
		       (boot_context->auth_status == BOOT_API_CTX_AUTH_FAILED) ?
		       "failed" : "succeeded");
	}

skip_console_init:
	if (dt_pmic_status() > 0) {
		initialize_pmic();
	}

	stm32mp1_syscfg_init();

	if (stm32_iwdg_init() < 0) {
		panic();
	}

	stm32_iwdg_refresh();

	result = stm32mp1_dbgmcu_freeze_iwdg2();
	if (result != 0) {
		INFO("IWDG2 freeze error : %i\n", result);
	}

	stm32mp1_auth_ops.check_key = boot_context->bootrom_ecdsa_check_key;
	stm32mp1_auth_ops.verify_signature =
		boot_context->bootrom_ecdsa_verify_signature;

	stm32mp_init_auth(&stm32mp1_auth_ops);

	stm32mp1_arch_security_setup();

	print_reset_reason();

#if !STM32MP_USE_STM32IMAGE
	fconf_populate("TB_FW", STM32MP_DTB_BASE);
#endif /* !STM32MP_USE_STM32IMAGE */

	stm32mp_io_setup();
}

/*******************************************************************************
 * This function can be used by the platforms to update/use image
 * information for given `image_id`.
 ******************************************************************************/
int bl2_plat_handle_post_image_load(unsigned int image_id)
{
	int err = 0;
	bl_mem_params_node_t *bl_mem_params = get_bl_mem_params_node(image_id);
	bl_mem_params_node_t *bl32_mem_params;
	bl_mem_params_node_t *pager_mem_params __unused;
	bl_mem_params_node_t *paged_mem_params __unused;
#if !STM32MP_USE_STM32IMAGE
	const struct dyn_cfg_dtb_info_t *config_info;
	bl_mem_params_node_t *tos_fw_mem_params;
	unsigned int i;
	unsigned int idx;
	unsigned long long ddr_top __unused;
	const unsigned int image_ids[] = {
		BL32_IMAGE_ID,
		BL33_IMAGE_ID,
		HW_CONFIG_ID,
		TOS_FW_CONFIG_ID,
	};
#endif /* !STM32MP_USE_STM32IMAGE */

	assert(bl_mem_params != NULL);

	switch (image_id) {
#if !STM32MP_USE_STM32IMAGE
	case FW_CONFIG_ID:
		/* Set global DTB info for fixed fw_config information */
		set_config_info(STM32MP_FW_CONFIG_BASE, STM32MP_FW_CONFIG_MAX_SIZE, FW_CONFIG_ID);
		fconf_populate("FW_CONFIG", STM32MP_FW_CONFIG_BASE);

		idx = dyn_cfg_dtb_info_get_index(TOS_FW_CONFIG_ID);

		/* Iterate through all the fw config IDs */
		for (i = 0U; i < ARRAY_SIZE(image_ids); i++) {
			if ((image_ids[i] == TOS_FW_CONFIG_ID) && (idx == FCONF_INVALID_IDX)) {
				continue;
			}

			bl_mem_params = get_bl_mem_params_node(image_ids[i]);
			assert(bl_mem_params != NULL);

			config_info = FCONF_GET_PROPERTY(dyn_cfg, dtb, image_ids[i]);
			if (config_info == NULL) {
				continue;
			}

			bl_mem_params->image_info.image_base = config_info->config_addr;
			bl_mem_params->image_info.image_max_size = config_info->config_max_size;

			bl_mem_params->image_info.h.attr &= ~IMAGE_ATTRIB_SKIP_LOADING;

			switch (image_ids[i]) {
			case BL32_IMAGE_ID:
				bl_mem_params->ep_info.pc = config_info->config_addr;

				/* In case of OPTEE, initialize address space with tos_fw addr */
				pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
				pager_mem_params->image_info.image_base = config_info->config_addr;
				pager_mem_params->image_info.image_max_size =
					config_info->config_max_size;

				/* Init base and size for pager if exist */
				paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
				paged_mem_params->image_info.image_base = STM32MP_DDR_BASE +
					(dt_get_ddr_size() - STM32MP_DDR_S_SIZE -
					 STM32MP_DDR_SHMEM_SIZE);
				paged_mem_params->image_info.image_max_size = STM32MP_DDR_S_SIZE;
				break;

			case BL33_IMAGE_ID:
				bl_mem_params->ep_info.pc = config_info->config_addr;
				break;

			case HW_CONFIG_ID:
			case TOS_FW_CONFIG_ID:
				break;

			default:
				return -EINVAL;
			}
		}
		break;
#endif /* !STM32MP_USE_STM32IMAGE */

	case BL32_IMAGE_ID:
		if (optee_header_is_valid(bl_mem_params->image_info.image_base)) {
			/* BL32 is OP-TEE header */
			bl_mem_params->ep_info.pc = bl_mem_params->image_info.image_base;
			pager_mem_params = get_bl_mem_params_node(BL32_EXTRA1_IMAGE_ID);
			paged_mem_params = get_bl_mem_params_node(BL32_EXTRA2_IMAGE_ID);
			assert((pager_mem_params != NULL) && (paged_mem_params != NULL));

#if STM32MP_USE_STM32IMAGE && defined(AARCH32_SP_OPTEE)
			/* Set OP-TEE extra image load areas at run-time */
			pager_mem_params->image_info.image_base = STM32MP_OPTEE_BASE;
			pager_mem_params->image_info.image_max_size = STM32MP_OPTEE_SIZE;

			paged_mem_params->image_info.image_base = STM32MP_DDR_BASE +
								  dt_get_ddr_size() -
								  STM32MP_DDR_S_SIZE -
								  STM32MP_DDR_SHMEM_SIZE;
			paged_mem_params->image_info.image_max_size = STM32MP_DDR_S_SIZE;
#endif /* STM32MP_USE_STM32IMAGE && defined(AARCH32_SP_OPTEE) */

			err = parse_optee_header(&bl_mem_params->ep_info,
						 &pager_mem_params->image_info,
						 &paged_mem_params->image_info);
			if (err) {
				ERROR("OPTEE header parse error.\n");
				panic();
			}

			/* Set optee boot info from parsed header data */
			bl_mem_params->ep_info.args.arg0 = paged_mem_params->image_info.image_base;
			bl_mem_params->ep_info.args.arg1 = 0; /* Unused */
			bl_mem_params->ep_info.args.arg2 = 0; /* No DT supported */
		} else {
#if !STM32MP_USE_STM32IMAGE
			bl_mem_params->ep_info.pc = bl_mem_params->image_info.image_base;
			tos_fw_mem_params = get_bl_mem_params_node(TOS_FW_CONFIG_ID);
			bl_mem_params->image_info.image_max_size +=
				tos_fw_mem_params->image_info.image_max_size;
#endif /* !STM32MP_USE_STM32IMAGE */
			bl_mem_params->ep_info.args.arg0 = 0;
		}
		break;

	case BL33_IMAGE_ID:
		bl32_mem_params = get_bl_mem_params_node(BL32_IMAGE_ID);
		assert(bl32_mem_params != NULL);
		bl32_mem_params->ep_info.lr_svc = bl_mem_params->ep_info.pc;
		break;

	default:
		/* Do nothing in default case */
		break;
	}

#if STM32MP_SDMMC || STM32MP_EMMC
	/*
	 * Invalidate remaining data read from MMC but not flushed by load_image_flush().
	 * We take the worst case which is 2 MMC blocks.
	 */
	if ((image_id != FW_CONFIG_ID) &&
	    ((bl_mem_params->image_info.h.attr & IMAGE_ATTRIB_SKIP_LOADING) == 0U)) {
		inv_dcache_range(bl_mem_params->image_info.image_base +
				 bl_mem_params->image_info.image_size,
				 2U * MMC_BLOCK_SIZE);
	}
#endif /* STM32MP_SDMMC || STM32MP_EMMC */

	return err;
}

void bl2_el3_plat_prepare_exit(void)
{
	uint16_t boot_itf = stm32mp_get_boot_itf_selected();

	switch (boot_itf) {
#if STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER
	case BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_UART:
	case BOOT_API_CTX_BOOT_INTERFACE_SEL_SERIAL_USB:
		/* Invalidate the downloaded buffer used with io_memmap */
		inv_dcache_range(DWL_BUFFER_BASE, DWL_BUFFER_SIZE);
		break;
#endif /* STM32MP_UART_PROGRAMMER || STM32MP_USB_PROGRAMMER */
	default:
		/* Do nothing in default case */
		break;
	}

	stm32mp1_security_setup();
}
