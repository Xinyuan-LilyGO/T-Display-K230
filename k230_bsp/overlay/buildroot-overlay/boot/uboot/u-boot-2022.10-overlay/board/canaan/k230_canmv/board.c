#include <asm/asm.h>
#include <asm/io.h>
#include <asm/types.h>
#include <lmb.h>
#include <cpu_func.h>
#include <stdio.h>
#include <common.h>
#include <command.h>
#include <image.h>
#include <gzip.h>
#include <asm/spl.h>
#include "sysctl.h"

#include <pufs_hmac.h>
#include <pufs_ecp.h>
#include <pufs_rt.h>
#include "pufs_sm2.h"
#include <pufs_sp38a.h>
#include <pufs_sp38d.h>
#include <linux/kernel.h>
// #include "sdk_autoconf.h"
#include "../common/k230_board_common.h"
#include <env_internal.h>
#include <linux/delay.h>
#include <dm.h>

#ifdef CONFIG_K230_BARE_DISP_LOGO_RM69A10
#define RM69A10_KEYBOARD_BACKLIGHT_GPIO 52U
#define RM69A10_KEYBOARD_BACKLIGHT_GPIO_MASK \
    (1U << (RM69A10_KEYBOARD_BACKLIGHT_GPIO - 32U))
#define RM69A10_KEYBOARD_BACKLIGHT_IOMUX_IO52 (52U * 4U)
#define RM69A10_KEYBOARD_BACKLIGHT_IOMUX_GPIO 0x0000018fU

static void rm69a10_keyboard_backlight_early_off(void)
{
    u32 data;
    u32 dir;

    data = readl((void *)(GPIO_BASE_ADDR1 + 0x0));
    data &= ~RM69A10_KEYBOARD_BACKLIGHT_GPIO_MASK;
    writel(data, (void *)(GPIO_BASE_ADDR1 + 0x0));

    writel(RM69A10_KEYBOARD_BACKLIGHT_IOMUX_GPIO,
           (void *)(IOMUX_BASE_ADDR + RM69A10_KEYBOARD_BACKLIGHT_IOMUX_IO52));

    dir = readl((void *)(GPIO_BASE_ADDR1 + 0x4));
    dir |= RM69A10_KEYBOARD_BACKLIGHT_GPIO_MASK;
    writel(dir, (void *)(GPIO_BASE_ADDR1 + 0x4));

    data = readl((void *)(GPIO_BASE_ADDR1 + 0x0));
    data &= ~RM69A10_KEYBOARD_BACKLIGHT_GPIO_MASK;
    writel(data, (void *)(GPIO_BASE_ADDR1 + 0x0));
}

int board_init(void)
{
    rm69a10_keyboard_backlight_early_off();
    return 0;
}

void quick_boot_board_init(void)
{
    rm69a10_keyboard_backlight_early_off();
}
#endif

sysctl_boot_mode_e sysctl_boot_get_boot_mode(void)
{
	return SYSCTL_BOOT_SDIO1;
}

#ifdef CONFIG_BOARD_LATE_INIT
int if_sdio_wifi_reset(void)
{
    ofnode node;

    node = ofnode_by_compatible(ofnode_null(), "kendryte,k230_canmv_v2");
    if (ofnode_valid(node)) {
        return 1;
    }

    node = ofnode_by_compatible(ofnode_null(), "kendryte,k230_canmv_v3p0"); //v3.0 // lckfb;
    if (ofnode_valid(node)) {
        return 1;
    }

    return 0;
}
int board_late_init(void)
{
    ofnode node;

    if(if_sdio_wifi_reset())
        wifi_sdio0_rst();

    node = ofnode_by_compatible(ofnode_null(), "kendryte,k230_canmv");
	if (ofnode_valid(node)) {
        wifi_gpio_rst(1);
    }

    return 0;
}
#endif
