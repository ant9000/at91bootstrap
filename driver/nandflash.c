/* ----------------------------------------------------------------------------
 *         ATMEL Microcontroller Software Support  -  ROUSSET  -
 * ----------------------------------------------------------------------------
 * Copyright (c) 2006, Atmel Corporation

 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the disclaimer below.
 *
 * Atmel's name may not be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * DISCLAIMER: THIS SOFTWARE IS PROVIDED BY ATMEL "AS IS" AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT ARE
 * DISCLAIMED. IN NO EVENT SHALL ATMEL BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * ----------------------------------------------------------------------------
 * File Name           : nandflash.c
 * Object              :
 * Creation            : 
 *-----------------------------------------------------------------------------
 */
#include "common.h"
#include "hardware.h"
#include "board.h"
#include "arch/at91_pio.h"
#include "arch/at91_nand_ecc.h"
#include "gpio.h"

#include "debug.h"

#include "nand.h"
#include "hamming.h"
#include "nand_ids.h"

/*
 * NAND Commands
 */
/* 8 bits devices */
#define WRITE_NAND_COMMAND(d) do { \
	*(volatile unsigned char *) \
	((unsigned long)AT91C_SMARTMEDIA_BASE | AT91_SMART_MEDIA_CLE) = \
	(unsigned char)(d); \
	} while(0)

#define WRITE_NAND_ADDRESS(d) do { \
	*(volatile unsigned char *) \
	((unsigned long)AT91C_SMARTMEDIA_BASE | AT91_SMART_MEDIA_ALE) = \
	(unsigned char)(d); \
	} while(0)

#define WRITE_NAND(d) do { \
	*(volatile unsigned char *) \
	((unsigned long)AT91C_SMARTMEDIA_BASE) = (unsigned char)d; \
	} while(0)

#define READ_NAND() ((unsigned char)(*(volatile unsigned char *) \
	(unsigned long)AT91C_SMARTMEDIA_BASE))

/* 16 bits devices */
#define WRITE_NAND_COMMAND16(d) do { \
	*(volatile unsigned short *) \
	((unsigned long)AT91C_SMARTMEDIA_BASE | AT91_SMART_MEDIA_CLE) = \
	(unsigned short)(d); \
	} while(0)

#define WRITE_NAND_ADDRESS16(d) do { \
	*(volatile unsigned short *) \
	((unsigned long)AT91C_SMARTMEDIA_BASE | AT91_SMART_MEDIA_ALE) = \
	(unsigned short)(d); \
	} while(0)

#define WRITE_NAND16(d) do { \
	*(volatile unsigned short *) \
	((unsigned long)AT91C_SMARTMEDIA_BASE) = (unsigned short)d; \
	} while(0)

#define READ_NAND16() ((unsigned short)(*(volatile unsigned short *) \
	(unsigned long)AT91C_SMARTMEDIA_BASE))

#undef CONFIG_USE_PMECC
#if defined(CPU_HAS_PMECC) && !defined(CONFIG_ENABLE_SW_ECC)
#define CONFIG_USE_PMECC
#endif

#ifdef CONFIG_USE_PMECC

#define TT_MAX			25
/* ECC offset in spare area */
#define ECC_START_ADDR		48
#define ECC_END_ADDR		63

#if defined(CONFIG_AT91SAM9X5EK) || defined(CONFIG_AT91SAM9N12EK)
#define PMECC_ALGO_FCT_ADDR		0x00100008
#define LOOKUP_TABLE_ALPHA_TO		0x10C000;
#define LOOKUP_TABLE_INDEX_OF		0x108000;
#endif

/* The PMECC descripter structure */
struct _PMECC_paramDesc_struct {
	unsigned int pageSize;
	unsigned int spareSize;
	unsigned int sectorSize;	// 0 for 512, 1 for 1024 bytes, like in PMECCFG register
	unsigned int errBitNbrCapability;
	unsigned int eccSizeByte;
	unsigned int eccStartAddress;
	unsigned int eccEndAddress;

	unsigned int nandWR;
	unsigned int spareEna;
	unsigned int modeAuto;
	unsigned int clkCtrl;
	unsigned int interrupt;

	int tt;
	int mm;
	int nn;

	short *alpha_to;
	short *index_of;

	short partialSyn[100];
	short si[100];

	/* sigma table */
	short smu[TT_MAX + 2][2 * TT_MAX + 1];
	/* polynom order */
	short lmu[TT_MAX + 1];

} PMECC_paramDesc_struct;

/* ECC detection/coreection */
typedef int (*PMECC_CorrectionAlgo_Rom_Func) (unsigned long pPMECC,
				unsigned long pPMERRLOC,
				struct _PMECC_paramDesc_struct *
				PMECC_desc,
				unsigned int PMECC_status,
				void *pageBuffer);

PMECC_CorrectionAlgo_Rom_Func pmecc_correction;


static int pmecc_readl(unsigned int reg)
{
	return(readl(AT91C_BASE_PMECC + reg));
}

static void pmecc_writel(unsigned int value, unsigned reg)
{
	writel(value, (AT91C_BASE_PMECC + reg));
}
#endif /* #ifdef CONFIG_USE_PMECC */

/*
* ooblayout 
*/
/* ooblayout for 256 byte pages. */
struct nand_ooblayout ooblayout_256 = {
	/* bad block marker is at position */
	5,
	/* 3 ecc bytes */
	3,
	/* ecc byte positions */
	{0, 1, 2},
	/* 4 extra bytes */
	4,
	/* extra byte positions */
	{3, 4, 6, 7}
};

/* ooblayout for 512 byte pages */
struct nand_ooblayout ooblayout_512 = {
	/* bad block marker is at position */
	5,
	/* 6 ecc bytes */
	6,
	/* ecc byte positions */
	{0, 1, 2, 3, 6, 7},
	/* 8 extra bytes */
	8,
	/* extra bytes positions */
	{8, 9, 10, 11, 12, 13, 14, 15}
};

/* ooblayout for 2048 byte pages */
struct nand_ooblayout ooblayout_2048 = {
	/* Bad block marker is at position */
	0,
	/* 24 ecc bytes */
	24,
	/* ecc byte positions */
	{40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57,
	 58, 59, 60, 61, 62, 63},
	/* 38 extra bytes */
	38,
	/* extra byte positions */
	{2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
	 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39}
};

static struct nand_chip nand_chip_default = {
	.chip_id	= 0x0,		/* Set ONFI parameter here */
	.numblocks	= 0x0,
	.blocksize	= 0x0,
	.pagesize	= 0x0,
	.oobsize	= 0x0,
	.buswidth	= 0x0,
	.ecclayout	= 0,
};

static struct nand_onfi_params onfi_params;

static void nand_wait_ready(void)
{
#ifdef CONFIG_SYS_NAND_READY_PIN
	while (pio_get_value(CONFIG_SYS_NAND_READY_PIN) != 1);
#endif
}

static void nand_cs_enable(void)
{
#ifdef CONFIG_SYS_NAND_ENABLE_PIN
	pio_set_value(CONFIG_SYS_NAND_ENABLE_PIN, 0);
#endif
}

static void nand_cs_disable(void)
{
#ifdef CONFIG_SYS_NAND_ENABLE_PIN
	pio_set_value(CONFIG_SYS_NAND_ENABLE_PIN, 1);
#endif
}

static unsigned short onfi_crc16(unsigned short crc, unsigned char const *p, unsigned int len)
{
	int i;

	while (len--) {
		crc ^= *p++ << 8;
		for (i = 0; i < 8; i++)
			crc = (crc << 1) ^ ((crc & 0x8000) ? 0x8005 : 0);
	}

	return crc;
}

/* Check if the NAND chip is ONFI compliant, returns 0 if it is, 1 otherwise */
static int nandflash_detect_onfi(struct nand_chip *chip)
{
	struct nand_onfi_params *p = &onfi_params;
	unsigned char onfi_ind[4];
	int i, j;
	unsigned int onfi_version;
	unsigned char *param;
	
	nand_cs_enable();

	WRITE_NAND_COMMAND(CMD_READID);
	WRITE_NAND_ADDRESS(0x20);

	onfi_ind[0] = READ_NAND();
	onfi_ind[1] = READ_NAND();
	onfi_ind[2] = READ_NAND();
	onfi_ind[3] = READ_NAND();

	nand_cs_disable();

	if ((onfi_ind[0] != 'O')
		|| (onfi_ind[1] != 'N')
		|| (onfi_ind[2] != 'F')
		|| (onfi_ind[3] != 'I')) 
		return 1;
	
	dbg_log(1, "ONFI flash detected\n\r");

	nand_cs_enable();

	/* read the nand ONFI parameter */
	WRITE_NAND_COMMAND(CMD_READ_ONFI);
	WRITE_NAND_ADDRESS(0x00);
	
	nand_wait_ready();
	
	for (i = 0; i < 3; i++) {
		param = (unsigned char *)p;
		/* Read the parameter table */
		for (j = 0; j < sizeof(onfi_params); j++)
			*param++ = READ_NAND();

		if (onfi_crc16(ONFI_CRC_BASE, (unsigned char *)p, 254) == p->crc) {
			dbg_log(1, "ONFI param page %d valid\n\r", i);
			break;
		}
	}

	nand_cs_disable();

	if (i == 3)
		return 1;

	/* check version */
	if (p->revision & (1 << 5))
		onfi_version = 23;
	else if (p->revision & (1 << 4))
		onfi_version = 22;
	else if (p->revision & (1 << 3))
		onfi_version = 21;
	else if (p->revision & (1 << 2))
		onfi_version = 20;
	else if (p->revision & (1 << 1))
		onfi_version = 10;
	else
		onfi_version = 0;

	if (!onfi_version) {
		dbg_log(1, "%s: unsupported ONFI version: %d\n\r", __func__, p->revision);
		return 1;
	}

	chip->numblocks = p->blocks_per_lun;
	chip->pagesize 	= p->byte_per_page;
	chip->blocksize = p->pages_per_block * chip->pagesize;
	chip->oobsize 	= p->spare_bytes_per_page;
	chip->buswidth	= p->features & 0x01;

	switch (chip->pagesize) {
	case 256: chip->ecclayout = &ooblayout_256; break;
	case 512: chip->ecclayout = &ooblayout_512; break;
	case 2048: chip->ecclayout = &ooblayout_2048; break;
	case 4096: break;
	default:
		dbg_log(1, "Not supported page size: %d\n\r", chip->pagesize);
		return 1;
	}
	return 0;
}

static int nandflash_detect_non_onfi(struct nand_chip *chip)
{
	int manf_id, dev_id, cellinfo, extid;
	struct nandflash_dev *type;

	nand_cs_enable();
	WRITE_NAND_COMMAND(CMD_READID);
	WRITE_NAND_ADDRESS(0x00);
	manf_id  = READ_NAND();
	dev_id   = READ_NAND();
	cellinfo = READ_NAND();
	extid    = READ_NAND();
	nand_cs_disable();

	type = (struct nandflash_dev *)&nandflash_ids[0];
	
	for (; type->name != NULL; type++)
		if (dev_id == type->id)
			break;
	
	if (type->name == NULL){
		if (manf_id != 0x00 && manf_id != 0xff 
			&& dev_id != 0x00 && dev_id != 0xff)
			dbg_log(1, "unknown NAND device: Manufacturer ID: %d", 
				"Chip ID: 0x%d\n\r", manf_id, dev_id);
		return 1;
	}
	
	dbg_log(1, "NAND device: %s, Manufacturer ID: %d Chip ID: %d\n\r",
			type->name, manf_id, dev_id);

	/* Newer devices have all the information in additional id bytes */
	if (type->pagesize == 0){
		/* Calc pagesize */
		chip->pagesize = 1024 << (extid & 0x3);
		extid >>= 2;
		/* Calc oobsize */
		chip->oobsize = (8 << (extid & 0x01)) * (chip->pagesize >> 9);
		extid >>= 2;
		/* Calc blocksize. Blocksize is multiples of 64KiB */
		chip->blocksize = (64 * 1024) << (extid & 0x03);
		extid >>= 2;
		/* Get buswidth information */
		chip->buswidth = (extid & 0x01) ? 1 : 0;
	} else {
		/* Old devices have chip data hardcoded in the device id table */
		chip->pagesize 	= type->pagesize;
		chip->blocksize = type->erasesize;
		chip->oobsize 	= chip->pagesize / 32;
		chip->buswidth 	= (((type->options & NAND_BUSWIDTH_16) 
						== NAND_BUSWIDTH_16) ? 1: 0); 
	}

	switch (chip->pagesize) {
	case 256: chip->ecclayout = &ooblayout_256; break;
	case 512: chip->ecclayout = &ooblayout_512; break;
	case 2048:chip->ecclayout = &ooblayout_2048; break;
	case 4096: break;
	default:
		dbg_log(1, "Not supported page size: %d\n\r", chip->pagesize);
		return 1;
	}

	return 0;

	
}

static void nand_info_init(struct nand_info *nand, struct nand_chip *chip)
{
	unsigned int pagesize, i = 0;

	/* number of blocks in device */
	nand->numblocks = chip->numblocks;
	/* number of data bytes in a block */
	nand->blocksize = chip->blocksize;
	/* number of bytes in page area */
	nand->pagesize = chip->pagesize;
	/* number of bytes in oob area */
	nand->oobsize = chip->oobsize;
	/* Total number of bytes in a sector */
	nand->sectorsize = nand->pagesize + nand->oobsize;
	nand->ecclayout = chip->ecclayout;
	nand->buswidth = chip->buswidth;	/* Data Bus Width (8/16 bits) */

	pagesize = nand->pagesize - 1;
	nand->page_shift = 0;
	while (pagesize >> i) {
		nand->page_shift++;
		i++;
	}

	if (nand->buswidth)
		nand->badblockpos = 2 * nand->ecclayout->badblockpos;
	else
		nand->badblockpos = nand->ecclayout->badblockpos;
}

static void nandflash_reset(void)
{
	nand_cs_enable();
	WRITE_NAND_COMMAND(0xFF);
	nand_wait_ready();
	nand_wait_ready();
	nand_cs_disable();
}

static int nandflash_get_type(struct nand_info *nand)
{
	struct nand_chip *chip = &nand_chip_default;

	nandflash_reset();

	/* Check if the Nandflash is ONFI compliant */
	if (nandflash_detect_onfi(chip)) {
		if (nandflash_detect_non_onfi(chip)) {
			dbg_log(1, "Not Find Support NAND Device!\n\r");
			return 1;
		}
	}

	nand_info_init(nand, chip);
	
	if (nand->buswidth == 0)
		nandflash_config_buswidth(0);
	else 
		nandflash_config_buswidth(1);
	
	return 0;
}

static void send_large_block_address(unsigned int addr)
{
	WRITE_NAND_ADDRESS((addr >> 0) & 0xFF);
	WRITE_NAND_ADDRESS((addr >> 8) & 0xFF);
}

static void send_sector_address(unsigned int addr)
{
	send_large_block_address(addr);
	WRITE_NAND_ADDRESS((addr >> 16) & 0xFF);
}

int nand_erase_block_0(void)
{
	unsigned int block = 0;

	nand_cs_enable();

	WRITE_NAND_COMMAND(CMD_ERASE_1);

	send_sector_address(block);

	WRITE_NAND_COMMAND(CMD_ERASE_2);

	/* Wait for nand to be ready */
	nand_wait_ready();
	nand_wait_ready();

	/* Check status bit for error notification */
	WRITE_NAND_COMMAND(CMD_STATUS);
	nand_wait_ready();
	if (READ_NAND() & STATUS_ERROR)
		return 1;

	nand_cs_disable();

	return 0;
}

#ifdef CONFIG_USE_PMECC
static int init_pmecc_descripter(struct _PMECC_paramDesc_struct *pmecc_params, unsigned int pagesize)
{
	switch (pagesize) {
	case 2048:
		pmecc_params->pageSize = AT91C_PMECC_PAGESIZE_4SEC;
		pmecc_params->sectorSize = AT91C_PMECC_SECTORSZ_512;
		pmecc_params->spareSize = 64;
		pmecc_params->errBitNbrCapability = 0;	/* 2bits correction */
		pmecc_params->eccSizeByte = 16;
		pmecc_params->eccStartAddress = ECC_START_ADDR;
		pmecc_params->eccEndAddress = ECC_END_ADDR;
		pmecc_params->spareEna = 0;
		pmecc_params->clkCtrl = 2;	/* stated in datasheet */
		pmecc_params->interrupt = 0;
		pmecc_params->tt = 2;
		pmecc_params->mm = 13;
		pmecc_params->nn = (1 << pmecc_params->mm) - 1;
		pmecc_params->alpha_to = (short *)LOOKUP_TABLE_ALPHA_TO;
		pmecc_params->index_of = (short *)LOOKUP_TABLE_INDEX_OF;
		break;

	case 512:
	case 1024:
	case 4096:
		/* TODO */
	default:
		dbg_log(1, "Not supported page size: %d\n\r",
			pagesize);
		return 1;
	}
	return 0;
} 

static int init_pmecc_core(struct _PMECC_paramDesc_struct *pmecc_params)
{
	pmecc_params->modeAuto = AT91C_PMECC_SPAREENA_ENA;
	pmecc_params->nandWR = 0;

	pmecc_writel(AT91C_PMECC_RST, PMECC_CTRL);
	pmecc_writel(AT91C_PMECC_DISABLE, PMECC_CTRL);
/*	writel(pmecc_params->errBitNbrCapability |
	       pmecc_params->sectorSize |
	       pmecc_params->pageSize |
	       pmecc_params->nandWR |
	       pmecc_params->spareEna |
	       pmecc_params->modeAuto, AT91C_BCH_PMECCFG0);
*/
	pmecc_writel(pmecc_params->errBitNbrCapability |
		pmecc_params->sectorSize |
		pmecc_params->pageSize |
		pmecc_params->nandWR |
		pmecc_params->spareEna |
		pmecc_params->modeAuto, PMECC_CFG);
		
	pmecc_writel((pmecc_params->spareSize - 1), PMECC_SAREA);
	
	pmecc_writel(pmecc_params->eccStartAddress, PMECC_SADDR);
	pmecc_writel(pmecc_params->eccEndAddress, PMECC_EADDR);
	pmecc_writel(pmecc_params->clkCtrl, PMECC_CLK);
	pmecc_writel(0xFF, PMECC_IDR);
	pmecc_writel(AT91C_PMECC_ENABLE, PMECC_CTRL);
	pmecc_writel(AT91C_PMECC_DATA, PMECC_CTRL);

	return 0;

}

static int init_pmecc(unsigned int pagesize)
{
	pmecc_correction = (PMECC_CorrectionAlgo_Rom_Func)
			(*(unsigned int *)PMECC_ALGO_FCT_ADDR);

	if (init_pmecc_descripter(&PMECC_paramDesc_struct, pagesize) != 0)
		return 1;
	
	init_pmecc_core(&PMECC_paramDesc_struct);

	return 0;
}
#endif /* #ifdef CONFIG_USE_PMECC */

#ifdef NANDFLASH_SMALL_BLOCKS
static int nand_read_sector(struct nand_info *nand, 
				unsigned int sectoraddr,
				unsigned char *buffer,
				unsigned int zone_flag)
{
	unsigned int readbytes, i;
	unsigned char command;

	/*
	 * WARNING : During a read procedure you can't call the ReadStatus flash cmd
	 * * The ReadStatus fill the read register with 0xC0 and then corrupt the read
	 */
	switch (zone_flag) {
	case ZONE_DATA:
		readbytes = nand->pagesize;
		command = CMD_READ_A0;
		break;
	case ZONE_INFO:
		readbytes = nand->oobsize;
		buffer += nand->pagesize;
		command = CMD_READ_C;
		break;
	case ZONE_DATA | ZONE_INFO:
		readbytes = nand->sectorsize;
		command = CMD_READ_A0;
		break;
	default:
		return 1;
	}

	nand_cs_enable();

	/* Write specific command, Read from start */
	if (nand->buswidth) { /* 16 bits */
		WRITE_NAND_COMMAND16(command);
	} else {
		WRITE_NAND_COMMAND(command);
	}

	sectoraddr >>= nand->page_shift;

	if (nand->buswidth) { /* 16 bits */
		WRITE_NAND_ADDRESS16(0x00);
		WRITE_NAND_ADDRESS16((sectoraddr >> 0) & 0xFF);
		WRITE_NAND_ADDRESS16((sectoraddr >> 8) & 0xFF);
		WRITE_NAND_ADDRESS16((sectoraddr >> 16) & 0xFF);
	} else {
		WRITE_NAND_ADDRESS(0x00);
		WRITE_NAND_ADDRESS((sectoraddr >> 0) & 0xFF);
		WRITE_NAND_ADDRESS((sectoraddr >> 8) & 0xFF);
		WRITE_NAND_ADDRESS((sectoraddr >> 16) & 0xFF);
	}

	/* Wait for flash to be ready (can't pool on status, read upper WARNING) */
	nand_wait_ready();
	nand_wait_ready();

	/* Read loop */
	if (nand->buswidth) { /* 16bits */
		for (i = 0; i < readbytes / 2; i++) {	// Div2 because of 16bits
			*((short *)buffer) = READ_NAND16();
			buffer += 2;
		}
	} else { /* 8 bits */
		if (command == CMD_READ_C) {
			for (i = 0; i < readbytes; i++) {
				*buffer = READ_NAND();
				buffer++;
			}
		} else {
			for (i = 0; i < readbytes / 2; i++) {
				*buffer = READ_NAND();
				buffer++;
			}

			command = CMD_READ_A1;
			WRITE_NAND_COMMAND(command);
			WRITE_NAND_ADDRESS(0x00);
			WRITE_NAND_ADDRESS((sectoraddr >> 0) & 0xFF);
			WRITE_NAND_ADDRESS((sectoraddr >> 8) & 0xFF);
			WRITE_NAND_ADDRESS((sectoraddr >> 16) & 0xFF);

			/* Need to be done twice, READY detected too early the first time? */
			nand_wait_ready();
			nand_wait_ready();

			for (i = 0; i < (readbytes / 2); i++) {
				*buffer = READ_NAND();
				buffer++;
			}
		}
	}

	nand_cs_disable();

	return 0;
}

#else /* For large blocks */
static int nand_read_sector(struct nand_info *nand,
				unsigned int sectoraddr,
				unsigned char *buffer, 
				unsigned int zone_flag)
{
	unsigned int readbytes, i;
	unsigned int address;

#ifdef CONFIG_USE_PMECC
	int ret = 0; 
	unsigned int status;
	unsigned char *pbuf = buffer;

	PMECC_paramDesc_struct.modeAuto = AT91C_PMECC_SPAREENA_ENA;
	PMECC_paramDesc_struct.nandWR = 0;

	pmecc_writel(AT91C_PMECC_RST, PMECC_CTRL);
	pmecc_writel(AT91C_PMECC_DISABLE, PMECC_CTRL);
	pmecc_writel(PMECC_paramDesc_struct.errBitNbrCapability |
	       PMECC_paramDesc_struct.sectorSize |
	       PMECC_paramDesc_struct.pageSize |
	       PMECC_paramDesc_struct.nandWR |
	       PMECC_paramDesc_struct.spareEna |
	       PMECC_paramDesc_struct.modeAuto, PMECC_CFG);
//	writel(PMECC_paramDesc_struct.spareSize - 1, AT91C_BCH_PMECCFG1);
//	writel(PMECC_paramDesc_struct.eccStartAddress, AT91C_BCH_PMECCFG2);
//	writel(PMECC_paramDesc_struct.eccEndAddress, AT91C_BCH_PMECCFG3);
//	writel(PMECC_paramDesc_struct.clkCtrl, AT91C_BCH_PMECCFG4);
//	writel(0xFF, AT91C_BCH_PMECCIDR);
	pmecc_writel(AT91C_PMECC_ENABLE, PMECC_CTRL);
	pmecc_writel(AT91C_PMECC_DATA, PMECC_CTRL);

	zone_flag = ZONE_DATA | ZONE_INFO;
#endif
	/*
	 * WARNING : During a read procedure you can't call the ReadStatus flash cmd
	 * * The ReadStatus fill the read register with 0xC0 and then corrupt the read
	 */
	nand_cs_enable();

	WRITE_NAND_COMMAND(CMD_READ_1);

	address = 0x00;
	switch (zone_flag) {
	case ZONE_DATA:
		readbytes = nand->pagesize;
		break;

	case ZONE_INFO:
		readbytes = nand->oobsize;
		buffer += nand->pagesize;
		address = nand->pagesize;
		if (nand->buswidth) {	/* 16 bits */
			address = address / 2; /* Div 2 is because we address in word and not in byte */
		}
		break;

	case ZONE_DATA | ZONE_INFO:
		readbytes = nand->sectorsize;
		break;

	default:
		return 1;
	}

	send_large_block_address(address);

	sectoraddr >>= nand->page_shift;
	send_sector_address(sectoraddr);

	WRITE_NAND_COMMAND(CMD_READ_2);

	/* Wait for flash to be ready (can't pool on status, read upper WARNING) */
	nand_wait_ready();
	nand_wait_ready();

	/* Read loop */
	if (nand->buswidth) {
		for (i = 0; i < readbytes / 2; i++) { /* Div2 because of 16bits  */
			*((short *)buffer) = READ_NAND16();
			buffer += 2;
		}
	} else {
		for (i = 0; i < readbytes; i++)
			*buffer++ = READ_NAND();
	}

#ifdef CONFIG_USE_PMECC
	while (pmecc_readl(PMECC_SR) & AT91C_PMECC_BUSY) ;

	status = pmecc_readl(PMECC_ISR);
	if (status)
		ret = pmecc_correction((AT91C_BASE_PMECC + PMECC_CFG),
					(AT91C_BASE_PMERRLOC + PMERRLOC_ELCFG),
					&PMECC_paramDesc_struct,
					status,
					pbuf);
	if (ret != 0) return 1;
#endif

	nand_cs_disable();

	return 0;
}
#endif /* #ifdef NANDFLASH_SMALL_BLOCKS */

static int nand_check_badblock(struct nand_info *nand,
				unsigned int block,
				unsigned char *buffer)
{
	unsigned int i = 0;
	unsigned int sectoraddr = block * nand->blocksize;

	/* Read the first page and second page oob zone to detect if block is bad */
	for (i = 0; i < 2; i++) {
		nand_read_sector(nand, sectoraddr + i * nand->pagesize, buffer, ZONE_INFO);
		
		if (*(buffer + nand->pagesize + nand->badblockpos) != 0xFF)
			return 1;
	}

	return 0;
}

#ifdef CONFIG_ENABLE_SW_ECC
static void nand_read_ecc(struct nand_ooblayout *ooblayout,
				unsigned char *buffer,
				unsigned char *ecc)
{
	unsigned int i;

	for (i = 0; i < ooblayout->eccbytes; i++)
		ecc[i] = buffer[ooblayout->eccpos[i]];
}
#endif

static int nand_read_page(struct nand_info *nand,
				unsigned int block,
				unsigned int page,
				unsigned int zone_flag,
				unsigned char *buffer)
{
	unsigned int sectoraddr = block * nand->blocksize + page * nand->pagesize;

	if (nand_check_badblock(nand, block, buffer) == 1) {
		dbg_log(1, "Bad block: #%d\n\r", block);
		return 1;
	}

#ifndef CONFIG_ENABLE_SW_ECC
	return nand_read_sector(nand, sectoraddr, buffer, ZONE_DATA);
#else

	int retval;
	unsigned char hamming[48], error;

	retval = nand_read_sector(nand, sectoraddr, buffer,ZONE_DATA | ZONE_INFO);

	if (retval)
		return 1;

	nand_read_ecc(nand->ecclayout, buffer + nand->pagesize, hamming);

	error = Hamming_Verify256x(buffer, nand->pagesize, hamming);
	if (error && (error != Hamming_ERROR_SINGLEBIT)) {
		dbg_log(1, "Hamming ECC error!\n\r");
		return 1;
	}

	return 0;
#endif /* #ifndef CONFIG_ENABLE_SW_ECC */
}

int load_nandflash(unsigned long offset, unsigned int size, unsigned char *dest)
{
	struct nand_info nand;
	unsigned char *buffer = dest;
	unsigned int block, length, readsize, numpage, page;

	nandflash_hw_init();
	
	if (nandflash_get_type(&nand)) 
		return 1;

#ifdef CONFIG_USE_PMECC
	if (init_pmecc(nand.pagesize))
		return 1;
#endif

	dbg_log(1, "Nand: Copy %d bytes from %d to %d\r\n", size, offset, dest);

	block = offset / nand.blocksize;
	length = size;
	while (length > 0) {
		/* read a buffer corresponding to a block in the origin file */
		if (length < nand.blocksize) 
			readsize = length;
		else
			readsize = nand.blocksize;

		/* Adjust the number of sectors to read */
		numpage = readsize / nand.pagesize;
		if (readsize % nand.pagesize)
			numpage++;

		/* Loop until a valid block has been read */
		while (1) {
			for (page = 0; page < numpage; page++) {
				if (nand_read_page(&nand, block, page, ZONE_DATA, buffer) == 1) /* skip this block */
					break;
				else
					buffer += nand.pagesize;
			}
			block++;

			if (page >= numpage)
				break;
		}
		length -= readsize;
	}
	return 0;
}

