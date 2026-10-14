# /* Copyright (c) 2023, Canaan Bright Sight Co., Ltd
#  *
#  * SPDX-License-Identifier: GPL-2.0+
#  */

obj-y += board.o

obj-$(CONFIG_CANMV_LPDDR3_2133) += gen_pilpddr3_2133.o

$(obj)/gen_pilpddr3_2133.c: $(src)/k230pi_ddr_init_2133.c $(srctree)/arch/riscv/cpu/k230/ddr.sh
	($(srctree)/arch/riscv/cpu/k230/ddr.sh $< $@ 0x50000 0x53fff 0x54000 0x54359 || exit 1)

obj-$(CONFIG_CANMV_V3_LPDDR4_2667) += gen_canmvv3_lpddr4_2667.o
obj-$(CONFIG_CANMV_V3_LPDDR4_3200) += gen_canmvv3_lpddr4_3200.o

$(obj)/gen_canmvv3_lpddr4_2667.c: $(src)/../k230_canmv_01studio/lpddr4_init_32_swap_2667.c $(srctree)/arch/riscv/cpu/k230/ddr.sh
	($(srctree)/arch/riscv/cpu/k230/ddr.sh $< $@ 0x50000 0x53fff 0x54000 0x5433d || exit 1)

$(obj)/gen_canmvv3_lpddr4_3200.c: $(src)/../k230_canmv_01studio/lpddr4_init_32_swap_3200.c $(srctree)/arch/riscv/cpu/k230/ddr_2d.sh
	($(srctree)/arch/riscv/cpu/k230/ddr_2d.sh $< $@ 0x50000 0x53fff 0x54000 0x5433d 0x542bd || exit 1)

EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_common
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_crypto
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_dma
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_ka
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_rt
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_ecc
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_hmac
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_ecp
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_sm2
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_sp38a
EXTRA_CFLAGS += -I$(srctree)/arch/$(ARCH)/cpu/k230/puf_fw/pufs_sp38d
