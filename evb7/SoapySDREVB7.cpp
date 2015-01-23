//
// SoapySDR wrapper for the LMS7002M driver.
//
// Copyright (c) 2015-2015 Fairwaves, Inc.
// Copyright (c) 2015-2015 Rice University
// SPDX-License-Identifier: Apache-2.0
// http://www.apache.org/licenses/LICENSE-2.0
//

#include <SoapySDR/Device.hpp>
#include <SoapySDR/Registry.hpp>
#include <pothos_zynq_dma_driver.h>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <iostream>

#include <LMS7002M/LMS7002M.h>
#include <LMS7002M/LMS7002M_impl.h>
#include "spidev_interface.h"
#include "sysfs_gpio_interface.h"
#include "xilinx_user_gpio.h"
#include "xilinx_user_mem.h"

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
#define FPGA_REGS_SIZE 1024

#define FPGA_REG_RD_SENTINEL 0 //readback a known value
#define FPGA_REG_RD_RX_CLKS 8 //sanity check clock counter
#define FPGA_REG_RD_TX_CLKS 12 //sanity check clock counter
#define FPGA_REG_RD_DATA_A 20 //RXA data for loopback test
#define FPGA_REG_RD_DATA_B 24 //RXB data for loopback test

#define FPGA_REG_WR_RX_STORE_OK 8 //can register RX samples (for test)
#define FPGA_REG_WR_EXT_RST 12 //active high external reset
#define FPGA_REG_WR_DATA_A 20 //TXA data for loopback test
#define FPGA_REG_WR_DATA_B 24 //TXB data for loopback test

#define RX_DMA_INDEX 0
#define RX_FRAME_SIZE 1000 //buff size - hdrs

#define DATA_NUM_BUFFS 16
#define DATA_BUFF_SIZE 4096

#define CTRL_NUM_BUFFS 16
#define CTRL_BUFF_SIZE 64

/***********************************************************************
 * Device interface
 **********************************************************************/
class EVB7 : public SoapySDR::Device
{
public:
    EVB7(void):
        _regs(NULL),
        _spiHandle(NULL),
        _lms(NULL),
        _rx_data_dma(NULL),
        _rx_ctrl_dma(NULL)
    {
        std::cout << "EVB7()\n";
        setvbuf(stdout, NULL, _IOLBF, 0);

        //map FPGA registers
        _regs = xumem_map_phys(FPGA_REGS, FPGA_REGS_SIZE);
        if (_regs == NULL)
        {
            throw std::runtime_error("EVB7 fail to map registers");
        }
        printf("Read sentinel 0x%x\n", xumem_read32(_regs, FPGA_REG_RD_SENTINEL));

        //perform reset
        SET_EMIO_OUT_LVL(RESET_EMIO, 0);
        SET_EMIO_OUT_LVL(RESET_EMIO, 1);

        //setup spi
        _spiHandle = spidev_interface_open("/dev/spidev32766.0");
        if (_spiHandle == NULL) std::runtime_error("EVB7 fail to spidev_interface_open()");

        //setup LMS7002M
        _lms = LMS7002M_create(spidev_interface_transact, _spiHandle);
        if (_lms == NULL) std::runtime_error("EVB7 fail to LMS7002M_create()");
        LMS7002M_set_spi_mode(_lms, 4); //set 4-wire spi mode first
        LMS7002M_reset(_lms);

        //read info register
        LMS7002M_regs_spi_read(_lms, 0x002f);
        printf("rev 0x%x\n", LMS7002M_regs(_lms)->reg_0x002f_rev);
        printf("ver 0x%x\n", LMS7002M_regs(_lms)->reg_0x002f_ver);

        //turn the clocks on
        int ret = LMS7002M_set_data_clock(_lms, 61.44e6/2, 61.44e6/2);
        if (ret != 0) throw std::runtime_error("EVB7 fail LMS7002M_set_data_clock()");

        //configure data port directions and data clock rates
        LMS7002M_configure_lml_port(_lms, LMS_PORT1, LMS_TX, 2);
        LMS7002M_configure_lml_port(_lms, LMS_PORT2, LMS_RX, 8);
        LMS7002M_invert_fclk(_lms, true); //makes it read in I, Q

        //external reset now that clocks are on
        xumem_write32(_regs, FPGA_REG_WR_EXT_RST, 1);
        xumem_write32(_regs, FPGA_REG_WR_EXT_RST, 0);

        printf("FPGA_REG_RD_RX_CLKS 0x%x\n", xumem_read32(_regs, FPGA_REG_RD_RX_CLKS));
        printf("FPGA_REG_RD_RX_CLKS 0x%x\n", xumem_read32(_regs, FPGA_REG_RD_RX_CLKS));

        printf("FPGA_REG_RD_TX_CLKS 0x%x\n", xumem_read32(_regs, FPGA_REG_RD_TX_CLKS));
        printf("FPGA_REG_RD_TX_CLKS 0x%x\n", xumem_read32(_regs, FPGA_REG_RD_TX_CLKS));

        //port output enables
        SET_EMIO_OUT_LVL(RXEN_EMIO, 1);
        SET_EMIO_OUT_LVL(TXEN_EMIO, 1);

        //setup dma buffs
        _rx_data_dma = pzdud_create(RX_DMA_INDEX, PZDUD_S2MM);
        if (_rx_data_dma == NULL) throw std::runtime_error("EVB7 fail create rx data DMA");
        pzdud_reset(_rx_data_dma);

        _rx_ctrl_dma = pzdud_create(RX_DMA_INDEX, PZDUD_MM2S);
        if (_rx_ctrl_dma == NULL) throw std::runtime_error("EVB7 fail create rx ctrl DMA");
        pzdud_reset(_rx_ctrl_dma);
        std::cout << "EVB7() setup OK\n";
    }

    ~EVB7(void)
    {
        //power down and clean up
        LMS7002M_power_down(_lms);
        LMS7002M_destroy(_lms);

        //back to inputs
        CLEANUP_EMIO(RESET_EMIO);
        CLEANUP_EMIO(RXEN_EMIO);
        CLEANUP_EMIO(TXEN_EMIO);

        pzdud_destroy(_rx_data_dma);
        pzdud_destroy(_rx_ctrl_dma);
        spidev_interface_close(_spiHandle);
        xumem_unmap_phys(_regs, FPGA_REGS_SIZE);
    }

/*******************************************************************
* Identification API
******************************************************************/
    std::string getDriverKey(void) const
    {
        return "EVB7";
    }

    std::string getHardwareKey(void) const
    {
        return "EVB7";
    }

/*******************************************************************
* Channels API
******************************************************************/
    size_t getNumChannels(const int direction) const
    {
        return (direction == SOAPY_SDR_TX)?0 : 1;
    }

    bool getFullDuplex(const int, const size_t) const
    {
        return true;
    }

/*******************************************************************
* Stream API
******************************************************************/

    SoapySDR::Stream *setupStream(
        const int direction,
        const std::string &format,
        const std::vector<size_t> &channels,
        const SoapySDR::Kwargs &)
    {
        std::cout << "EVB7::setupStream() " << size_t(_rx_data_dma) << "\n";
        if (direction != SOAPY_SDR_RX) throw std::runtime_error("EVB7::setupStream: no TX support yet");
        if (format != "CS16") throw std::runtime_error("EVB7::setupStream: "+format);
        if (channels.size() != 1) throw std::runtime_error("EVB7::setupStream: only one channel for now");
        if (channels[0] != 0) throw std::runtime_error("EVB7::setupStream: only ch0 for now");

        //allocate dma memory
        int ret = 0;
        ret = pzdud_alloc(_rx_data_dma, DATA_NUM_BUFFS, DATA_BUFF_SIZE);
        if (ret != PZDUD_OK) throw std::runtime_error("EVB7::setupStream: fail alloc rx data DMA");
        ret = pzdud_alloc(_rx_ctrl_dma, CTRL_NUM_BUFFS, CTRL_BUFF_SIZE);
        if (ret != PZDUD_OK) throw std::runtime_error("EVB7::setupStream: fail alloc rx ctrl DMA");

        //start the channels
        ret = pzdud_init(_rx_data_dma, true);
        if (ret != PZDUD_OK) throw std::runtime_error("EVB7::setupStream: fail init rx data DMA");
        ret = pzdud_init(_rx_ctrl_dma, true);
        if (ret != PZDUD_OK) throw std::runtime_error("EVB7::setupStream: fail init rx ctrl DMA");

        return reinterpret_cast<SoapySDR::Stream *>(_rx_data_dma);
    }

    void closeStream(SoapySDR::Stream *stream)
    {
        std::cout << "EVB7::closeStream() " << size_t(stream) << "\n";
        if (stream == reinterpret_cast<SoapySDR::Stream *>(_rx_data_dma))
        {
            //halt the channels
            pzdud_halt(_rx_data_dma);
            pzdud_halt(_rx_ctrl_dma);

            //free dma memory
            pzdud_free(_rx_data_dma);
            pzdud_free(_rx_ctrl_dma);
        }
    }

    int sendControlMessage(const bool timeFlag, const bool burstFlag, const bool contFlag, const int frameSize, const int burstSize, const long long time)
    {
        size_t len = 0;
        int handle = pzdud_acquire(_rx_ctrl_dma, &len);
        if (handle < 0) return SOAPY_SDR_STREAM_ERROR;

        uint32_t *ctrl_msg = (uint32_t *)pzdud_addr(_rx_ctrl_dma, handle);
        ctrl_msg[0] = frameSize-1;
        if (timeFlag) ctrl_msg[0] |= (1 << 31);
        if (burstFlag) ctrl_msg[0] |= (1 << 28);
        if (contFlag) ctrl_msg[0] |= (1 << 27);
        ctrl_msg[1] = burstSize - 1;
        ctrl_msg[2] = time >> 32;
        ctrl_msg[3] = time & 0xffffffff;
        for (size_t i = 0; i < 4; i++) printf("ctrl_msg[%d] = 0x%x\n", int(i), ctrl_msg[i]);

        pzdud_release(_rx_ctrl_dma, handle, sizeof(uint32_t)*4);
        return 0;
    }

    int activateStream(
        SoapySDR::Stream *stream,
        const int flags,
        const long long timeNs,
        const size_t numElems)
    {
        std::cout << "EVB7::activateStream() " << size_t(stream) << "\n";
        if (stream == reinterpret_cast<SoapySDR::Stream *>(_rx_data_dma))
        {
            return sendControlMessage(
                (flags & SOAPY_SDR_HAS_TIME) != 0, //timeFlag
                (numElems) != 0, //burstFlag
                (flags & SOAPY_SDR_END_BURST) == 0, //contFlag
        //TODO time ticks != time ns
                RX_FRAME_SIZE, numElems, timeNs);
        }
        return SOAPY_SDR_STREAM_ERROR;
    }

    int deactivateStream(
        SoapySDR::Stream *stream,
        const int flags,
        const long long timeNs)
    {
        std::cout << "EVB7::deactivateStream() " << size_t(stream) << "\n";
        if (stream == reinterpret_cast<SoapySDR::Stream *>(_rx_data_dma))
        {
            return sendControlMessage(
                (flags & SOAPY_SDR_HAS_TIME) != 0, //timeFlag
                false, //burstFlag
                false, //contFlag
        //TODO time ticks != time ns
                0, 0, timeNs);
        }
        return SOAPY_SDR_STREAM_ERROR;
    }

    int readStream(
        SoapySDR::Stream *,
        void * const *buffs,
        const size_t numElems,
        int &flags,
        long long &timeNs,
        const long timeoutUs)
    {
        //std::cout << "EVB7::readStream() " << numElems << "\n";
        size_t len = 0;
        int ret = 0;

        //try to acquire asap, then wait
        int handle = pzdud_acquire(_rx_data_dma, &len);
        if (handle == PZDUD_ERROR_CLAIMED) throw std::runtime_error("EVB7::readStream() all claimed");
        if (handle < 0)
        {
            pzdud_wait(_rx_data_dma, timeoutUs);

            //try to acquire again or timeout
            handle = pzdud_acquire(_rx_data_dma, &len);
            if (handle < 0) return SOAPY_SDR_TIMEOUT;
        }

        uint32_t *hdr = (uint32_t *)pzdud_addr(_rx_data_dma, handle);
        static int cnt = 0;
        //std::cout << cnt++ << " -- read " << len << std::endl;
        //for (size_t i = 0; i < 4; i++) printf("hdr[%d] = 0x%x\n", int(i), hdr[i]);

        const size_t burstCount = hdr[1] + 1;
        const size_t frameSize = (hdr[0] & 0xffff) + 1;
        const size_t numSamples = (len - (sizeof(uint32_t)*4))/sizeof(uint32_t);

        flags = 0;
        if (((hdr[0] >> 31) & 0x1) != 0) flags |= SOAPY_SDR_HAS_TIME;

        if (((hdr[0] >> 28) & 0x1) != 0) //in burst
        {
            if (burstCount == numSamples) flags |= SOAPY_SDR_END_BURST;
            else if (frameSize != numSamples) ret = SOAPY_SDR_OVERFLOW;
        }
        else
        {
            //std::cout << "frameSize = " << frameSize << " numSamples = " << numSamples << std::endl;
            if (frameSize != numSamples) ret = SOAPY_SDR_OVERFLOW;
            if (frameSize != numSamples) std::cout << "O" << std::endl;
        }

        //gather time even if its not valid
        //TODO time ticks != time ns
        timeNs = (((uint64_t)hdr[2]) << 32) | hdr[3];

        pzdud_release(_rx_data_dma, handle, 0);

        if (ret == 0) //no errors yet, copy buffer -- sorry for the copy, zero copy in the future...
        {
            //TODO if numElems < numSamples, keep remainder...
            ret = std::min(numSamples, numElems);
            std::memcpy(buffs[0], hdr+4, ret*sizeof(uint32_t));
        }

        //std::cout << "ret = " << ret << std::endl;
        return ret;
    }

private:
    void *_regs;
    void *_spiHandle;
    LMS7002M_t *_lms;
    pzdud_t *_rx_data_dma;
    pzdud_t *_rx_ctrl_dma;
};

/***********************************************************************
 * Find available devices
 **********************************************************************/
std::vector<SoapySDR::Kwargs> findEVB7(const SoapySDR::Kwargs &args)
{
    //always discovery "args" -- the sdr is the board itself
    std::vector<SoapySDR::Kwargs> discovered;
    discovered.push_back(args);
    return discovered;
}

/***********************************************************************
 * Make device instance
 **********************************************************************/
SoapySDR::Device *makeEVB7(const SoapySDR::Kwargs &)
{
    //ignore the args because again, the sdr is the board
    return new EVB7();
}

/***********************************************************************
 * Registration
 **********************************************************************/
static SoapySDR::Registry registerEVB7("evb7", &findEVB7, &makeEVB7, SOAPY_SDR_ABI_VERSION);
