/*****************************************************************************
 *    Copyright (c) 2009-2011 by Hisilicon
 *    All rights reserved.
 *****************************************************************************/

#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mtd/mtd.h>
#include <linux/errno.h>
#include <linux/sched.h>
#include "../spi_ids.h"
#include "hisfc300new.h"
#define DEBUG_SPI 0
#define QE_CORRECT_VALUE  (0x02)

/*****************************************************************************/
static int spi_general_wait_ready(struct hisfc_spi *spi)
{
	unsigned long regval;
	unsigned long deadline = 0;
	struct hisfc300_host *host = (struct hisfc300_host *)spi->host;

	do {
		hisfc300_write(host, HISFC300_CMD_INS, SPI_CMD_RDSR);
		hisfc300_write(host, HISFC300_CMD_CONFIG,
			HISFC300_CMD_CONFIG_SEL_CS(spi->chipselect)
			| HISFC300_CMD_CONFIG_DATA_CNT(1)
			| HISFC300_CMD_CONFIG_DATA_EN
			| HISFC300_CMD_CONFIG_RW_READ
			| HISFC300_CMD_CONFIG_START);

		HISFC300_CMD_WAIT_CPU_FINISH(host);
		regval = hisfc300_read(host, HISFC300_CMD_DATABUF0);
		if (!(regval & SPI_CMD_SR_WIP))
			return 0;
		udelay(1);
	} while (deadline++ < (40 << 20));

	printk(KERN_ERR "Wait spi flash ready timeout.\n");

	return 1;
}
/*****************************************************************************/
static int spi_general_write_enable(struct hisfc_spi *spi)
{
	unsigned int regval = 0;
	struct hisfc300_host *host = (struct hisfc300_host *)spi->host;

	hisfc300_write(host, HISFC300_CMD_INS, SPI_CMD_WREN);

	regval = HISFC300_CMD_CONFIG_SEL_CS(spi->chipselect)
		| HISFC300_CMD_CONFIG_START;
	hisfc300_write(host, HISFC300_CMD_CONFIG, regval);

	HISFC300_CMD_WAIT_CPU_FINISH(host);

	return 0;
}
/*****************************************************************************/
/*
  enable 4byte adress for SPI which memory more than 16M
*/
static int spi_general_entry_4addr(struct hisfc_spi *spi, int enable)
{
	struct hisfc300_host *host = (struct hisfc300_host *)spi->host;

	if (spi->addrcycle != SPI_4BYTE_ADDR_LEN)
		return 0;

	if (enable) {
		hisfc300_write(host, HISFC300_CMD_INS, SPI_CMD_EN4B);
	} else {
		hisfc300_write(host, HISFC300_CMD_INS, SPI_CMD_EX4B);
	}

	hisfc300_write(host, HISFC300_CMD_CONFIG,
		HISFC300_CMD_CONFIG_SEL_CS(spi->chipselect)
		| HISFC300_CMD_CONFIG_START);

	HISFC300_CMD_WAIT_CPU_FINISH(host);

	host->set_host_addr_mode(host, enable);

	return 0;
}
/*****************************************************************************/
/*
  configure prepared for dma or bus read or write mode
*/
static int spi_general_bus_prepare(struct hisfc_spi *spi, int op)
{
	unsigned int regval = 0;
	struct hisfc300_host *host = (struct hisfc300_host *)spi->host;

	if (op == READ) {
		regval |= HISFC300_BUS_CONFIG1_READ_EN;
		regval |= HISFC300_BUS_CONFIG1_READ_PREF_CNT(0);
		regval |= HISFC300_BUS_CONFIG1_READ_INS(spi->read->cmd);
		regval |= HISFC300_BUS_CONFIG1_READ_DUMMY_CNT(spi->read->dummy);
		regval |= HISFC300_BUS_CONFIG1_READ_IF_TYPE(spi->read->iftype);
	}

	hisfc300_write(host, HISFC300_BUS_CONFIG1, regval);
	hisfc300_write(host, HISFC300_BUS_CONFIG2,
		HISFC300_BUS_CONFIG2_WIP_LOCATE(0));

	host->set_system_clock(host, spi->read, TRUE);

	return 0;
}
/*****************************************************************************/
/*
  judge whether SPI support QUAD read write or not
*/
static int hisfc300_is_quad(struct hisfc_spi *spi)
{
	if (spi->write->iftype == 5 || spi->write->iftype == 6
		|| spi->read->iftype == 5 || spi->read->iftype == 6) {
		if (DEBUG_SPI)
			printk(KERN_INFO "4r4w SPI...............\n");
		return 1;
	}
	if (DEBUG_SPI)
		printk(KERN_INFO "2r2w or 1r1w SPI...............\n");
	return 0;
}

/*
  enable QE bit if QUAD read write is supported by SPI
*/
static int spi_general_qe_enable(struct hisfc_spi *spi)
{
	struct hisfc300_host *host = (struct hisfc300_host *)spi->host;
	unsigned int regval = 0;
	unsigned int qe_op = 0;

	if (hisfc300_is_quad(spi))
		qe_op = SPI_CMD_SR_QE;
	else
		qe_op = SPI_CMD_SR_XQE;

	spi->driver->write_enable(spi);

	hisfc300_write(host, HISFC300_CMD_INS, SPI_CMD_WRSR);
	hisfc300_write(host, HISFC300_CMD_DATABUF0, qe_op);
	hisfc300_write(host, HISFC300_CMD_CONFIG,
			HISFC300_CMD_CONFIG_MEM_IF_TYPE(spi->
				write->iftype)
			| HISFC300_CMD_CONFIG_DATA_CNT(2)
			| HISFC300_CMD_CONFIG_DATA_EN
			| HISFC300_CMD_CONFIG_DUMMY_CNT(spi->
				write->dummy)
			| HISFC300_CMD_CONFIG_SEL_CS(spi->chipselect)
			| HISFC300_CMD_CONFIG_START);
	HISFC300_CMD_WAIT_CPU_FINISH(host);

	spi->driver->wait_ready(spi);

	if (DEBUG_SPI) {
		hisfc300_write(host, HISFC300_CMD_INS, SPI_CMD_RDSR2);
		hisfc300_write(host, HISFC300_CMD_CONFIG,
				HISFC300_CMD_CONFIG_SEL_CS(spi->chipselect)
				| HISFC300_CMD_CONFIG_DATA_CNT(2)
				| HISFC300_CMD_CONFIG_DATA_EN
				| HISFC300_CMD_CONFIG_RW_READ
				| HISFC300_CMD_CONFIG_START);
		HISFC300_CMD_WAIT_CPU_FINISH(host);
		regval = hisfc300_read(host, HISFC300_CMD_DATABUF0);
		printk(KERN_INFO "QEbit = 202? : 0x%x\n", regval);
		if ((regval & QE_CORRECT_VALUE))
			printk(KERN_INFO "QE bit enable success\n");
		else
			printk(KERN_INFO "QE bit enable failed\n");
	}
	return 0;
}
