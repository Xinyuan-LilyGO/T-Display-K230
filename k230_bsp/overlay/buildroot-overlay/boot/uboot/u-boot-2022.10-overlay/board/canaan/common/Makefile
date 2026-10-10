ifdef CONFIG_SPL_BUILD
obj-y += k230_spl.o
endif

obj-y += k230_board_common.o
obj-y += k230_img.o
obj-y += k230_perf.o

obj-$(CONFIG_SIPLP4_3200_WODT) += gen_siplp4_3200_hwodt.o
obj-$(CONFIG_SIPLP4_3200_WALLODT) += gen_siplp4_3200_allodt.o
obj-$(CONFIG_SIPLP4_1600) += gen_siplp4_1600.o
obj-$(CONFIG_SIPLP4_2667) += gen_siplp4_2667.o

$(obj)/gen_siplp4_3200_hwodt.c: $(src)/sip_lpddr4_init_32_swap_3200_with_wodt.c
	($(srctree)/arch/riscv/cpu/k230/ddr_2d.sh $< $@ 0x50000 0x53fff 0x54000 0x5433d 0x542bd || exit 1)

$(obj)/gen_siplp4_3200_allodt.c: $(src)/sip_lpddr4_init_32_swap_3200_with_allodt.c
	($(srctree)/arch/riscv/cpu/k230/ddr_2d.sh $< $@ 0x50000 0x53fff 0x54000 0x5433d 0x542bd || exit 1)

$(obj)/gen_siplp4_1600.c: $(src)/sip_lpddr4_init_32_swap_1600.c
	($(srctree)/arch/riscv/cpu/k230/ddr.sh $< $@ 0x50000 0x53fff 0x54000 0x5433d || exit 1)

$(obj)/gen_siplp4_2667.c: $(src)/sip_lpddr4_init_32_swap_2667.c
	($(srctree)/arch/riscv/cpu/k230/ddr.sh $< $@ 0x50000 0x53fff 0x54000 0x5433d || exit 1)

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

ifndef CONFIG_SPL_BUILD
obj-$(CONFIG_K230_BARE_DISP_LOGO) += logo/
endif
