/*
** Nofrendo (c) 1998-2000 Matthew Conte (matt@conte.com)
**
** map162.c
**
** Mapper 162 and 163 interface (Nanjing games), adapted from retro-go.
*/

#include "../noftypes.h"
#include "../nes/nes.h"
#include "../nes/nes_mmc.h"

static uint8 reg5000;
static uint8 reg5100;
static uint8 reg5101;
static uint8 reg5200;
static uint8 reg5300;
static uint8 trigger;

static void map162_update(void)
{
   uint8 bank = (uint8)(((reg5200 & 0x03) << 4) | (reg5000 & 0x0F));
   mmc_bankrom(32, 0x8000, bank);
}

static uint8 map162_read(uint32 address)
{
   switch (address & 0x7300)
   {
   case 0x5100:
      return reg5300;

   case 0x5500:
      return trigger ? reg5300 : 0;

   default:
      UNUSED(address);
      return 0x04;
   }
}

static void map162_write(uint32 address, uint8 value)
{
   switch (address & 0x7300)
   {
   case 0x5000:
      reg5000 = value;
      if (!(reg5000 & 0x80) && nes_getcontextptr()->scanline < 128)
      {
         mmc_bankvrom(8, 0x0000, 0);
      }
      map162_update();
      break;

   case 0x5100:
      if (address == 0x5101)
      {
         if (reg5101 && !value)
            trigger = !trigger;
         reg5101 = value;
      }
      else if (value == 0x06)
      {
         mmc_bankrom(32, 0x8000, 3);
      }
      reg5100 = value;
      break;

   case 0x5200:
      reg5200 = value;
      map162_update();
      break;

   case 0x5300:
      reg5300 = value;
      break;

   default:
      break;
   }
}

static void map162_hblank(int vblank)
{
   nes_t *nes;

   if (vblank || !(reg5000 & 0x80))
      return;

   nes = nes_getcontextptr();
   if (nes->scanline == 127)
   {
      mmc_bankvrom(4, 0x0000, 1);
      mmc_bankvrom(4, 0x1000, 1);
   }
   else if (nes->scanline == 239)
   {
      mmc_bankvrom(4, 0x0000, 0);
      mmc_bankvrom(4, 0x1000, 0);
   }
}

static void map162_init(void)
{
   reg5000 = 0;
   reg5100 = 1;
   reg5101 = 1;
   reg5200 = 0;
   reg5300 = 0;
   trigger = 0;
   mmc_bankvrom(4, 0x0000, 0);
   mmc_bankvrom(4, 0x1000, 0);
   map162_update();
}

static map_memread map162_memread[] =
    {
        {0x5000, 0x5FFF, map162_read},
        {-1, -1, NULL}};

static map_memwrite map162_memwrite[] =
    {
        {0x5000, 0x5FFF, map162_write},
        {-1, -1, NULL}};

mapintf_t map162_intf =
    {
        162,             /* mapper number */
        "Nanjing 162",   /* mapper name */
        map162_init,     /* init routine */
        NULL,            /* vblank callback */
        map162_hblank,   /* hblank callback */
        NULL,            /* get state (snss) */
        NULL,            /* set state (snss) */
        map162_memread,  /* memory read structure */
        map162_memwrite, /* memory write structure */
        NULL             /* external sound device */
};

mapintf_t map163_intf =
    {
        163,             /* mapper number */
        "Nanjing 163",   /* mapper name */
        map162_init,     /* init routine */
        NULL,            /* vblank callback */
        map162_hblank,   /* hblank callback */
        NULL,            /* get state (snss) */
        NULL,            /* set state (snss) */
        map162_memread,  /* memory read structure */
        map162_memwrite, /* memory write structure */
        NULL             /* external sound device */
};
