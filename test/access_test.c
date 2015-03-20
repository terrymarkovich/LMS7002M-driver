//
// Test instantiating the LMS7002M through spidev on linux
//
// Copyright (c) 2014-2015 Fairwaves, Inc.
// Copyright (c) 2014-2015 Rice University
// SPDX-License-Identifier: Apache-2.0
// http://www.apache.org/licenses/LICENSE-2.0
//

#include <LMS7002M/LMS7002M.h>

#include <stdio.h>
#include <stdlib.h>

#include "spidev_interface.h"
#include "sysfs_gpio_interface.h"
#include "xilinx_user_gpio.h"
#include "xilinx_user_mem.h"

/*
 * for testing with gpio...
#define MOSI_MIO 10
#define MISO_MIO 11
#define SCLK_MIO 12
#define SEN_MIO 13
*/

#define EMIO_OFFSET 54
#define RESET_EMIO    (EMIO_OFFSET+0)
#define DIG_RST_EMIO  (EMIO_OFFSET+1)
#define RXEN_EMIO     (EMIO_OFFSET+2)
#define TXEN_EMIO     (EMIO_OFFSET+3)
#define DIO_DIR_CTRL1_EMIO   (EMIO_OFFSET+4)
#define DIO_DIR_CTRL2_EMIO   (EMIO_OFFSET+5)
#define IQSEL1_DIR_EMIO      (EMIO_OFFSET+6)
#define IQSEL2_DIR_EMIO      (EMIO_OFFSET+7)

#define SET_EMIO_OUT_LVL(emio, lvl) \
    gpio_export(emio); \
    gpio_set_dir(emio, 1); \
    gpio_set_value(emio, lvl)

#define CLEANUP_EMIO(emio) \
    gpio_set_dir(emio, 0); \
    gpio_unexport(emio)

#define FPGA_REGS 0x43C00000

#define FPGA_REG_RD_SENTINEL 0 //readback a known value
#define FPGA_REG_RD_RX_CLKS 8 //sanity check clock counter
#define FPGA_REG_RD_TX_CLKS 12 //sanity check clock counter
#define FPGA_REG_RD_DATA_A 28 //RXA data for loopback test
#define FPGA_REG_RD_DATA_B 32 //RXB data for loopback test

#define FPGA_REG_WR_EXT_RST 12 //active high external reset
//#define FPGA_REG_WR_RX_STORE_OK 8 //can register RX samples (for test)
#define FPGA_REG_WR_DATA_A 28 //TXA data for loopback test
#define FPGA_REG_WR_DATA_B 32 //TXB data for loopback test
#define FPGA_REG_WR_TX_TEST 36

static inline double estimate_clock_rate(void *regs, int offset)
{
    uint32_t t0 = xumem_read32(regs, offset);
    sleep(1);
    uint32_t t1 = xumem_read32(regs, offset);
    return ((double)(t1 - t0));
}

int main(int argc, char **argv)
{
    printf("=========================================================\n");
    printf("== Test LMS7002M access                                  \n");
    printf("=========================================================\n");
    if (argc < 2)
    {
        printf("Usage %s /dev/spidevXXXXXX\n", argv[0]);
        return EXIT_FAILURE;
    }

    //map FPGA registers
    void *regs = xumem_map_phys(FPGA_REGS, 1024);
    if (regs == NULL)
    {
        printf("failed to map registers\n");
        return EXIT_FAILURE;
    }
    printf("Read sentinel 0x%x\n", xumem_read32(regs, FPGA_REG_RD_SENTINEL));

    //perform reset
    SET_EMIO_OUT_LVL(RESET_EMIO, 0);
    SET_EMIO_OUT_LVL(RESET_EMIO, 1);

    void *handle = spidev_interface_open(argv[1]);
    if (handle == NULL) return EXIT_FAILURE;
    int ret = 0;

    //create and test lms....
    printf("Create LMS7002M instance\n");
    LMS7002M_t *lms = LMS7002M_create(spidev_interface_transact, handle);
    if (lms == NULL) return EXIT_FAILURE;
    LMS7002M_set_spi_mode(lms, 4); //set 4-wire spi mode first
    LMS7002M_reset(lms);

    //read info register
    LMS7002M_regs_spi_read(lms, 0x002f);
    printf("rev 0x%x\n", LMS7002M_regs(lms)->reg_0x002f_rev);
    printf("ver 0x%x\n", LMS7002M_regs(lms)->reg_0x002f_ver);

    //turn the clocks on
    double actualRate = 0.0;
    ret = LMS7002M_set_data_clock(lms, 61.44e6/2, 61.44e6/2, &actualRate);
    if (ret != 0)
    {
        printf("clock tune failure %d\n", ret);
        return EXIT_FAILURE;
    }

    //configure data port directions and data clock rates
    LMS7002M_configure_lml_port(lms, LMS_PORT1, LMS_TX, 2);
    LMS7002M_configure_lml_port(lms, LMS_PORT2, LMS_RX, 1);
    LMS7002M_invert_fclk(lms, true); //makes it read in I, Q
    LMS7002M_setup_digital_loopback(lms);

    //readback clock counters, are they alive?
    printf("RX CLK RATE %f MHz\n", estimate_clock_rate(regs, FPGA_REG_RD_RX_CLKS)/1e6);
    printf("TX CLK RATE %f MHz\n", estimate_clock_rate(regs, FPGA_REG_RD_TX_CLKS)/1e6);

    //port output enables
    SET_EMIO_OUT_LVL(RXEN_EMIO, 1);
    SET_EMIO_OUT_LVL(TXEN_EMIO, 1);

    xumem_write32(regs, FPGA_REG_WR_TX_TEST, 1); //enable fpga registers to TX
    xumem_write32(regs, FPGA_REG_WR_DATA_A, 0xAAAABBBB);
    xumem_write32(regs, FPGA_REG_WR_DATA_B, 0xCCCCDDDD);
    //xumem_write32(regs, FPGA_REG_WR_RX_STORE_OK, 1);
    //sleep(1);
    //xumem_write32(regs, FPGA_REG_WR_RX_STORE_OK, 0);
    sleep(1);
    printf("FPGA_REG_RD_DATA_A = 0x%x\n", xumem_read32(regs, FPGA_REG_RD_DATA_A));
    printf("FPGA_REG_RD_DATA_B = 0x%x\n", xumem_read32(regs, FPGA_REG_RD_DATA_B));

    //power down and clean up
    LMS7002M_power_down(lms);
    LMS7002M_destroy(lms);

    //back to inputs
    CLEANUP_EMIO(RESET_EMIO);
    CLEANUP_EMIO(RXEN_EMIO);
    CLEANUP_EMIO(TXEN_EMIO);

    spidev_interface_close(handle);

    printf("Done!\n");
    return EXIT_SUCCESS;
}
