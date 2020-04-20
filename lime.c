#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#include <pthread.h>

// sudo apt install liblimesuite-dev
#include <lime/LimeSuite.h>

#include "lime.h"
#include "timing.h"
#include "buffer/buffer_circular.h"

lime_fft_buffer_t lime_fft_buffer;

double frequency_tx =  1200000e3;
double frequency_rx = 10489750e3;
double frequency_downconversion = 9750e6;
double bandwidth = 512e3; // 512ks

//Device structure, should be initialize to NULL
static lms_device_t* device = NULL;

static bool limesdr_calibration_run(double bandwidth)
{
    /* Set max gain for calibration */
    if (LMS_SetNormalizedGain(device, true, 0, 31) < 0)
    {
        fprintf(stderr, "LMS_SetNormalizedGain() : %s\n", LMS_GetLastErrorMessage());
        return false;
    }
    
    if (LMS_Calibrate(device, true, 0, bandwidth, 0) < 0)
    {
        fprintf(stderr, "LMS_Calibrate() : %s\n", LMS_GetLastErrorMessage());
        /* Reset gain to zero */
        LMS_SetNormalizedGain(device, true, 0, 0);
        return false;
    }

    /* Reset gain to zero */
    LMS_SetNormalizedGain(device, true, 0, 0);
    return true;
}

static int16_t limesdr_calibration_ReadAnalogDC(const uint16_t addr)
{
    const uint16_t mask = addr < 0x05C7 ? 0x03FF : 0x003F;
    uint16_t value;
    int16_t result;
    LMS_WriteLMSReg(device, addr, 0);
    LMS_WriteLMSReg(device, addr, 0x4000);
    LMS_ReadLMSReg(device, addr, &value);
    LMS_WriteLMSReg(device, addr, value & ~0xC000);
    result = (value & mask);
    if (value & (mask + 1))
    {
        result *= -1;
    }
    return result;
}

static void limesdr_calibration_WriteAnalogDC(const uint16_t addr, const int16_t value)
{
    const uint16_t mask = addr < 0x05C7 ? 0x03FF : 0x003F;
    int16_t regValue = 0;
    if (value < 0)
    {
        regValue |= (mask + 1);
        regValue |= (abs(value + mask) & mask);
    }
    else
    {
        regValue |= (abs(value + mask + 1) & mask);
    }
    LMS_WriteLMSReg(device, addr, regValue);
    LMS_WriteLMSReg(device, addr, regValue | 0x8000);
}

static bool limesdr_calibration_loadFile(char *filename)
{
    //PLL tune same for Rx/Tx just switch channel A(Rx) / B(Tx)
    uint16_t reg011D; //FRAC_SDM[15:0]
    uint16_t reg011E; //INT_SDM & FRAC_SDM[19:16]
    uint16_t div_loch;
    uint16_t en_div2;
    uint16_t sel_vco;
    uint16_t csw_vco;
    
    // DC/IQ same for Rx/Tx just adjust the paramter names
    uint16_t gcorri;
    uint16_t gcorrq;
    uint16_t phaseOffset;
    int16_t dci;
    int16_t dcq;

    int r = 1;
    FILE *file;

    if (access(filename, F_OK) < 0)
    {
        fprintf(stderr,"Error, No calibration file found.\n");
        return false;
    }

    file = fopen(filename, "r");

    if(file == NULL)
    {
        fprintf(stderr, "Error, failed to open cal file to read.\n");
        return false;
    }

    r *= fscanf(file, "reg011D=%hu\n", &reg011D);
    r *= fscanf(file, "reg011E=%hu\n", &reg011E);
    r *= fscanf(file, "div_loch=%hu\n", &div_loch);
    r *= fscanf(file, "en_div2=%hu\n", &en_div2);
    r *= fscanf(file, "sel_vco=%hu\n", &sel_vco);
    r *= fscanf(file, "csw_vco=%hu\n", &csw_vco);

    r *= fscanf(file, "gcorri=%hu\n", &gcorri);
    r *= fscanf(file, "gcorrq=%hu\n", &gcorrq);
    r *= fscanf(file, "phaseOffset=%hu\n", &phaseOffset);
    r *= fscanf(file, "dci=%hd\n", &dci);
    r *= fscanf(file, "dcq=%hd\n", &dcq);

    fclose(file);

    if(r == 0)
    {
        fprintf(stderr, "Error, something failed to load from cal file.\n");
        return false;
    }

    //restore results
    
    LMS_WriteLMSReg(device, 0x011D, reg011D);
    LMS_WriteLMSReg(device, 0x011E, reg011E);
    LMS_WriteParam(device, LMS7_DIV_LOCH, div_loch);
    LMS_WriteParam(device, LMS7_EN_DIV2_DIVPROG, en_div2);
    LMS_WriteParam(device, LMS7_SEL_VCO, sel_vco);
    LMS_WriteParam(device, LMS7_CSW_VCO, csw_vco);

    LMS_WriteParam(device, LMS7_DCMODE,1);
    LMS_WriteParam(device, LMS7_PD_DCDAC_TXA,0);
    LMS_WriteParam(device, LMS7_PD_DCCMP_TXA,0);

    LMS_WriteParam(device, LMS7_GCORRI_TXTSP, gcorri);
    LMS_WriteParam(device, LMS7_GCORRQ_TXTSP, gcorrq);
    LMS_WriteParam(device, LMS7_IQCORR_TXTSP, phaseOffset);
    limesdr_calibration_WriteAnalogDC(LMS7_DC_TXAI.address, dci);
    limesdr_calibration_WriteAnalogDC(LMS7_DC_TXAQ.address, dcq);

    return true;
}

static bool limesdr_calibration_saveFile(char *filename)
{
    //PLL tune same for Rx/Tx just switch channel A(Rx) / B(Tx)
    uint16_t reg011D; //FRAC_SDM[15:0]
    uint16_t reg011E; //INT_SDM & FRAC_SDM[19:16]
    uint16_t div_loch;
    uint16_t en_div2;
    uint16_t sel_vco;
    uint16_t csw_vco;

    // DC/IQ same for Rx/Tx just adjust the paramter names
    uint16_t gcorri;
    uint16_t gcorrq;
    uint16_t phaseOffset;
    int16_t dci;
    int16_t dcq;

    int r = 1;
    FILE *file;

    //readback results
    LMS_ReadLMSReg(device, 0x011D, &reg011D);
    LMS_ReadLMSReg(device, 0x011E, &reg011E);
    LMS_ReadParam(device, LMS7_DIV_LOCH, &div_loch);
    LMS_ReadParam(device, LMS7_EN_DIV2_DIVPROG, &en_div2);
    LMS_ReadParam(device, LMS7_SEL_VCO, &sel_vco);
    LMS_ReadParam(device, LMS7_CSW_VCO, &csw_vco);

    //readback results
    LMS_ReadParam(device, LMS7_GCORRI_TXTSP, &gcorri);
    LMS_ReadParam(device, LMS7_GCORRQ_TXTSP, &gcorrq);
    LMS_ReadParam(device, LMS7_IQCORR_TXTSP, &phaseOffset);
    dci = limesdr_calibration_ReadAnalogDC(LMS7_DC_TXAI.address);
    dcq = limesdr_calibration_ReadAnalogDC(LMS7_DC_TXAQ.address);

    file = fopen(filename, "w");

    if(file == NULL)
    {
        fprintf(stderr, "Error, failed to open cal file to write.\n");
        return false;
    }

    r *= fprintf(file, "reg011D=%hu\n", reg011D);
    r *= fprintf(file, "reg011E=%hu\n", reg011E);
    r *= fprintf(file, "div_loch=%hu\n", div_loch);
    r *= fprintf(file, "en_div2=%hu\n", en_div2);
    r *= fprintf(file, "sel_vco=%hu\n", sel_vco);
    r *= fprintf(file, "csw_vco=%hu\n", csw_vco);

    r *= fprintf(file, "gcorri=%hu\n", gcorri);
    r *= fprintf(file, "gcorrq=%hu\n", gcorrq);
    r *= fprintf(file, "phaseOffset=%hu\n", phaseOffset);
    r *= fprintf(file, "dci=%hd\n", dci);
    r *= fprintf(file, "dcq=%hd\n", dcq);

    fclose(file);

    if(r == 0)
    {
        fprintf(stderr, "Error, something failed to save to cal file.\n");
        return false;
    }

    return true;
}

void *lime_thread(void *arg)
{
    bool *exit_requested = (bool *)arg;

    //Find devices
    int n;
    lms_info_str_t list[8]; //should be large enough to hold all detected devices
    if ((n = LMS_GetDeviceList(list)) < 0) //NULL can be passed to only get number of devices
    {
        return NULL;
    }

    if (n < 1)
    {
        fprintf(stderr, "Error: No LimeSDR device found, aborting.\n");
        return NULL;
    }

    if(n > 1)
    {
        printf("Warning: Multiple LimeSDR devices found (%d devices)\n", n);
    }

    //open the first device
    if (LMS_Open(&device, list[0], NULL))
    {
        return NULL;
    }

    const lms_dev_info_t *device_info;
    double Temperature;

    device_info = LMS_GetDeviceInfo(device);

    printf("Lime Device: %s, Serial: 0x%016" PRIx64 "\n",
        device_info->deviceName,
        device_info->boardSerialNumber
    );
    printf(" - Hardware: v%s, Firmware: v%s, Gateware: v%s\n",
        device_info->hardwareVersion,
        device_info->firmwareVersion,
        device_info->gatewareVersion
    );
    //printf("protocol version: %s\n", device_info->protocolVersion);
    //printf("gateware target: %s\n", device_info->gatewareTargetBoard);

    LMS_GetChipTemperature(device, 0, &Temperature);
    printf(" - Temperature: %.0f°C\n", Temperature);

    char *device_cal_filename;

    if(asprintf(&device_cal_filename, "%016" PRIx64 ".cal", device_info->boardSerialNumber) < 0)
    {
        return NULL;
    }

    //Initialize device with default configuration
    //Do not use if you want to keep existing configuration
    //Use LMS_LoadConfig(device, "/path/to/file.ini") to load config from INI
    if (LMS_Init(device) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    //Enable RX channel
    if (LMS_EnableChannel(device, LMS_CH_RX, 0, true) != 0)
    {
        LMS_Close(device);
        return NULL;
    }
    //Enable TX channels
    if (LMS_EnableChannel(device, LMS_CH_TX, 0, true) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    printf("Lime Frequency Plan:\n"
            " - TX Center: %.0fHz\n"
            " - RX Center: %.0fHz\n"
            "   - Downconversion LO: %.0fHz\n"
            "   - IF Center: %.0fHz\n",
        frequency_tx,
        frequency_rx,
            frequency_downconversion, 
            (frequency_rx - frequency_downconversion)        
    );

    //Set RX center frequency
    if (LMS_SetLOFrequency(device, LMS_CH_RX, 0, (frequency_rx - frequency_downconversion)) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    //Set TX center frequency
    //Automatically selects antenna port
    if (LMS_SetLOFrequency(device, LMS_CH_TX, 0, frequency_tx) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    //Set sample rate to 10 MHz, preferred oversampling in RF 4x
    //This set sampling rate for all channels
    if (LMS_SetSampleRate(device, bandwidth, 4) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    double sr_host, sr_rf;
    if (LMS_GetSampleRate(device, LMS_CH_TX, 0, &sr_host, &sr_rf) < 0)
    {
        fprintf(stderr, "Warning : LMS_GetSampleRate() : %s\n", LMS_GetLastErrorMessage());
        return NULL;
    }
    printf("Lime TX Samplerate: Host: %.1fKs, RF: %.1fKs\n", (sr_host/1000.0), (sr_rf/1000.0));

    if (LMS_GetSampleRate(device, LMS_CH_RX, 0, &sr_host, &sr_rf) < 0)
    {
        fprintf(stderr, "Warning : LMS_GetSampleRate() : %s\n", LMS_GetLastErrorMessage());
        return NULL;
    }
    printf("Lime RX Samplerate: Host: %.1fKs, RF: %.1fKs\n", (sr_host/1000.0), (sr_rf/1000.0));


    if(!limesdr_calibration_loadFile(device_cal_filename))
    {
        printf("Lime Calibration: No file found, running calibration!\n");

        if(limesdr_calibration_run(bandwidth))
        {
            limesdr_calibration_saveFile(device_cal_filename);
            printf("Lime Calibration: Saved to file (%s)\n", device_cal_filename);
        }
    }
    else
    {
        printf("Lime Calibration: Loaded from file (%s)\n", device_cal_filename);
    }

    //Set RX gain
    if (LMS_SetNormalizedGain(device, LMS_CH_RX, 0, 0.9) != 0)
    {
        LMS_Close(device);
        return NULL;
    }
    //Set TX gain
    if (LMS_SetNormalizedGain(device, LMS_CH_TX, 0, 0.5) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    //Disable test signals generation in RX channel
    if (LMS_SetTestSignal(device, LMS_CH_RX, 0, LMS_TESTSIG_NONE, 0, 0) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    //Streaming Setup
    lms_stream_t rx_stream;
    lms_stream_t tx_stream;

    //Initialize streams
    rx_stream.channel = 0; //channel number
    rx_stream.fifoSize = 1024 * 1024; //fifo size in samples
    rx_stream.throughputVsLatency = 0.5;
    rx_stream.isTx = false; //RX channel
    rx_stream.dataFmt = LMS_FMT_F32;
    if (LMS_SetupStream(device, &rx_stream) != 0)
    {
        LMS_Close(device);
        return NULL;
    }
    tx_stream.channel = 0; //channel number
    tx_stream.fifoSize = 1024 * 1024; //fifo size in samples
    tx_stream.throughputVsLatency = 0.5;
    tx_stream.isTx = true; //TX channel
    tx_stream.dataFmt = LMS_FMT_F32;
    if (LMS_SetupStream(device, &tx_stream) != 0)
    {
        LMS_Close(device);
        return NULL;
    }

    //Initialize data buffers
    const int buffersize = 8 * 1024; // 8192 I+Q samples
    float *buffer;
    buffer = malloc(buffersize * 2 * sizeof(float)); //buffer to hold complex values (2*samples))

    //Start streaming
    LMS_StartStream(&rx_stream);
    //LMS_StartStream(&tx_stream);

    //Streaming

    lms_stream_meta_t rx_metadata; //Use metadata for additional control over sample receive function behavior
    rx_metadata.flushPartialPacket = false; //currently has no effect in RX
    rx_metadata.waitForTimestamp = false; //currently has no effect in RX

    //lms_stream_meta_t tx_metadata; //Use metadata for additional control over sample send function behavior
    //tx_metadata.flushPartialPacket = false; //do not force sending of incomplete packet
    //tx_metadata.waitForTimestamp = true; //Enable synchronization to HW timestamp

    //uint64_t last_stats = 0;

    int samplesRead = 0;
    //lms_stream_status_t status;

#if 0
    bool monotonic_started = false;
    uint64_t samples_total = 0;
    uint64_t start_monotonic = 0;
#endif

    uint32_t samples_rx_transferred = samplesRead / 2.0;
    while(false == *exit_requested)
    {
        //Receive samples
        samplesRead = LMS_RecvStream(&rx_stream, buffer, buffersize, &rx_metadata, 1000);
        if(*exit_requested)
        {
            break;
        }

#if 0
        if(!monotonic_started)
        {
            start_monotonic = monotonic_ms();
            monotonic_started = true;
        }
#endif

        /* Copy out for Band FFT */
        pthread_mutex_lock(&lime_fft_buffer.mutex);
        /* Reset index so FFT knows it's new data */
        lime_fft_buffer.index = 0;
        memcpy(
            lime_fft_buffer.data,
            buffer,
            (samplesRead * 2 * sizeof(float))
        );
        lime_fft_buffer.size = (samplesRead * sizeof(float));
        pthread_cond_signal(&lime_fft_buffer.signal);
        pthread_mutex_unlock(&lime_fft_buffer.mutex);

        if(*exit_requested)
        {
            break;
        }

        /* Copy out for demod */
        samples_rx_transferred = samplesRead;
        buffer_circular_push(&buffer_circular_iq_main, (buffer_iqsample_t *)buffer, &samples_rx_transferred);
        if(samples_rx_transferred > 0)
        {
            fprintf(stderr, "Lime: WARNING push to IF subsample buffer was lossy (%d / %d returned)\n",
                samples_rx_transferred, samplesRead);
        }

#if 0
        uint32_t head, tail, capacity, occupied;
        buffer_circular_stats(&buffer_circular_iq_main, &head, &tail, &capacity, &occupied);
        printf(" - Head: %d, Tail: %d, Capacity: %d, Occupied: %d\n",
            head, tail, capacity, occupied);
#endif

#if 0
        samples_total += samplesRead;
        printf("Lime samplerate: %.3f (total: %lld\n", (float)(samples_total * 1000) / (monotonic_ms() - start_monotonic), samples_total);
#endif
        //printf("read %d samples\n", buffersize);

        //Send samples with 1024*256 sample delay from RX (waitForTimestamp is enabled)
        //tx_metadata.timestamp = rx_metadata.timestamp + 1024 * 256;
        //LMS_SendStream(&tx_stream, buffer, samplesRead, &tx_metadata, 1000);

        //Print stats every 1s
        //if (monotonic_ms() > last_stats + 1000)
        //{
            //last_stats = monotonic_ms();

            //Print stats
            //LMS_GetStreamStatus(&rx_stream, &status); //Obtain RX stream stats
            //printf("RX Rate: %.3f MB/s\n", (status.linkRate / 1e6)); //link data rate (both channels))
            //printf("RX 0 FIFO: %.2f%%\n", ((double)status.fifoFilledCount / status.fifoSize)); //percentage of RX 0 fifo filled

            //LMS_GetStreamStatus(&tx_stream, &status); //Obtain TX stream stats
            //printf("TX Rate: %.3f MB/s\n", (status.linkRate / 1e6)); //link data rate (both channels))
            //printf("TX 0 FIFO: %.2f%%\n", ((double)status.fifoFilledCount / status.fifoSize)); //percentage of TX 0 fifo filled
        //}
    }

    //Stop streaming
    LMS_StopStream(&rx_stream); //stream is stopped but can be started again with LMS_StartStream()
    //LMS_StopStream(&tx_stream);

    LMS_DestroyStream(device, &rx_stream); //stream is deallocated and can no longer be used
    //LMS_DestroyStream(device, &tx_stream);
    
    free(buffer);

    //Close device
    LMS_Close(device);

    return NULL;
}
