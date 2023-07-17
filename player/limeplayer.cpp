#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cstring>
#include <ctime>
#include <csignal>
#include <algorithm>

#ifdef _WIN32
#include "ya_getopt.h"
#else
#include <getopt.h>
#endif

#include <lime/LimeSuite.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#define SET_BINARY_MODE(handle) _setmode(handle, O_BINARY)
#else
#define SET_BINARY_MODE(handle) ((void)0)
#endif

#define STDIN  0
#define STDOUT 1
#define STDERR 2

#define EXIT_CODE_CONTROL_C (SIGINT + 128)
#define EXIT_CODE_NO_DEVICE (-2)
#define EXIT_CODE_LMS_INIT  (-1)

#define TX_FREQUENCY  1575420000.0
#define TX_SAMPLERATE 2500000.0
#define TX_BANDWIDTH  5000000.0
#define MAX_DYNAMIC   2047

#define ANTENNA_NONE  0
#define ANTENNA_BAND1 1
#define ANTENNA_BAND2 2
#define ANTENNA_AUTO  3
#define DEFAULT_ANTENNA ANTENNA_AUTO

#define STRINGIFY2(X) #X
#define STRINGIFY(X) STRINGIFY2(X)

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
int gettimeofday(struct timeval* tp, struct timezone* tzp) {
    // Note: some broken versions only have 8 trailing zero's, the correct epoch has 9 trailing zero's
    // This magic number is the number of 100 nanosecond intervals since January 1, 1601 (UTC)
    // until 00:00:00 January 1, 1970
    static const uint64_t EPOCH = ((uint64_t)116444736000000000ULL);

    SYSTEMTIME  system_time;
    FILETIME    file_time;
    uint64_t    time;

    GetSystemTime(&system_time);
    SystemTimeToFileTime(&system_time, &file_time);
    time = ((uint64_t)file_time.dwLowDateTime);
    time += ((uint64_t)file_time.dwHighDateTime) << 32;

    tp->tv_sec = (long)((time - EPOCH) / 10000000L);
    tp->tv_usec = (long)(system_time.wMilliseconds * 1000);
    return 0;
}
#else
#include <sys/time.h>
#endif

static int control_c_received = 0;
#ifdef _WIN32
BOOL WINAPI control_c_handler(DWORD fdwCtrlType)
{
    switch (fdwCtrlType)
    {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
        control_c_received = 1;
        return TRUE;
    default:
        return FALSE;
    }
}
#else
static void control_c_handler(int sig, siginfo_t *siginfo, void *context) {
    control_c_received = 1;
}
#endif

static void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s [option] < file" "\n"
            "\t" "-i, --index <index>     select specific LimeSDR device if multiple devices connected (default: 0)" "\n"
            "\t" "-a, --antenna <antenna> select antenna index in { 0, 1, 2, 3 } (default: " STRINGIFY(DEFAULT_ANTENNA) ")" "\n"
            "\t" "-c, --channel <channel> select channel index in { 0, 1 } (default: 0)" "\n"
            "\t" "-g, --gain <gain>       configure the so-called normalized RF gain in [0.0 .. 1.0] (default: 1.0 max RF power)" "\n"
            "\t" "-b, --bits <bits>       configure IQ sample bit depth in { 1, 8, 12, 16 } (default: 16)" "\n"
            "\t" "-s, --samplerate <samplerate>" "\n"
            "\t" "                        configure sampling rate for TX channels (default: " STRINGIFY(TX_SAMPLERATE) ")" "\n"
            "\t" "-d, --dynamic <dynamic> configure dynamic for the 1-bit mode (default: " STRINGIFY(MAX_DYNAMIC) ", max 12-bit signed value supported by LimeSDR)" "\n"
            "\t" "-h, --help              print this help message" "\n"
        "Example:" "\n"
        "\t" "./limeplayer -s 1000000 -b 1 -d 1023 -g 0.1 < ../circle.1b.1M.bin" "\n",
            program_name);
    exit(0);
}

//Device structure, should be initialize to nullptr
lms_device_t *device = nullptr;

int error(int exit_code) {
    if (device != nullptr) {
        LMS_Close(device);
    }
    exit(exit_code);
}

int main(int argc, char *const argv[]) {
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(control_c_handler, TRUE)) {
        printf("Could not set control handler.\n");
    }
#else
    struct sigaction control_c{};

    memset(&control_c, 0, sizeof(control_c));
    control_c.sa_sigaction = &control_c_handler;
 
    /* The SA_SIGINFO flag tells sigaction() to use the sa_sigaction field, not sa_handler. */
    control_c.sa_flags = SA_SIGINFO;
 
    if (sigaction(SIGTERM, &control_c, nullptr) < 0) {
        perror("sigaction");
        return (EXIT_CODE_CONTROL_C);
    }
    if (sigaction(SIGQUIT, &control_c, nullptr) < 0) {
        perror("sigaction");
        return (EXIT_CODE_CONTROL_C);
    }
    if (sigaction(SIGINT, &control_c, nullptr) < 0) {
        perror("sigaction");
        return (EXIT_CODE_CONTROL_C);
    }
#endif

    SET_BINARY_MODE(STDIN);

    double gain = 1.0;
    int32_t antenna = DEFAULT_ANTENNA;
    int32_t channel = 0;
    int32_t index = 0;
    int32_t bits = 16;
    double sampleRate = TX_SAMPLERATE;
    int32_t dynamic = 2047;

    static struct option long_options[] = {
        {"gain",       required_argument, nullptr, 'g'},
        {"channel",    required_argument, nullptr, 'c'},
        {"antenna",    required_argument, nullptr, 'a'},
        {"index",      required_argument, nullptr, 'i'},
        {"bits",       required_argument, nullptr, 'b'},
        {"samplerate", required_argument, nullptr, 's'},
        {"dynamic",    required_argument, nullptr, 'd'},
        {"help",       no_argument,       nullptr, 'h'},
        {nullptr,      no_argument,       nullptr, '\0'}
    };

    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "g:c:a:i:s:b:d:h", long_options, &option_index);
        if (c == -1) break;

        switch (c) {
            case 0:
            #if 1
                fprintf(stderr, "option %s", long_options[option_index].name);
                if (optarg)
                    fprintf(stderr, " with arg %s", optarg);
                fprintf(stderr, "\n");
            #endif

            break;

            case 'a':
                antenna = (int32_t) strtol(optarg, nullptr, 10);
            break;
            case 'b':
                bits = (int32_t) strtol(optarg, nullptr, 10);
            break;
            case 'c':
                channel = (int32_t) strtol(optarg, nullptr, 10);
            break;
            case 'g':
                gain = strtod(optarg, nullptr);
            break;
            case 'i':
                index = (int32_t) strtol(optarg, nullptr, 10);
            break;
            case 's':
                sampleRate = strtod(optarg, nullptr);
            break;
            case 'd':
                dynamic = (int32_t) strtol(optarg, nullptr, 10);
            break;
            case 'h':
            default:
                print_usage(argv[0]);
            break;
        }
    }

    int device_count = LMS_GetDeviceList(nullptr);
    if (device_count < 1) {
        printf("no device connected, exiting...\n");
        error(EXIT_CODE_NO_DEVICE);
    }
    auto *device_list = (lms_info_str_t *) malloc(sizeof(lms_info_str_t) * device_count);
    device_count = LMS_GetDeviceList(device_list);

    for (int i = 0; i < device_count; ++i) {
        printf("device[%d/%d]=%s" "\n", i + 1, device_count, device_list[i]);
    }

    // Use correct values
    // Use existing device
    if (index < 0 || index >= device_count) {
        index = 0;
    }
    printf("Using device index %d [%s]" "\n", index, device_list[index]);

    // Normalized gain shall be in [0.0 .. 1.0]
    gain = std::min(std::max(0.0, gain), 1.0);
    printf("Using normalized gain: %lf" "\n", gain);

    dynamic = std::min(std::max(0, dynamic), MAX_DYNAMIC);
    printf("Using normalized dynamic: %d" "\n", dynamic);

    if (LMS_Open(&device, device_list[index], nullptr)) {
        error(EXIT_CODE_LMS_INIT);
    }
    free(device_list);

    int lmsReset = LMS_Reset(device);
    if (lmsReset) {
        printf("lmsReset %d(%s)" "\n", lmsReset, LMS_GetLastErrorMessage());
        error(EXIT_CODE_LMS_INIT);
    }
    int lmsInit = LMS_Init(device);
    if (lmsInit) {
        printf("lmsInit %d(%s)" "\n", lmsInit, LMS_GetLastErrorMessage());
        error(EXIT_CODE_LMS_INIT);
    }

    int channel_count = LMS_GetNumChannels(device, LMS_CH_TX);
    printf("Tx channel count: %d" "\n", channel_count);
    if (channel < 0 || channel >= channel_count) {
        channel = 0;
    }
    printf("Using channel: %d" "\n", channel);

    int antenna_count = LMS_GetAntennaList(device, LMS_CH_TX, channel, nullptr);
    printf("TX%d Channel has %d antenna(ae)" "\n", channel, antenna_count);
    if (antenna < 0 || antenna >= antenna_count) {
        antenna = DEFAULT_ANTENNA;
    }

    lms_name_t* antenna_name = (lms_name_t*)malloc(sizeof(lms_name_t)*antenna_count);
    lms_range_t antenna_bw{};
    if (antenna_count > 0) {
        LMS_GetAntennaList(device, LMS_CH_TX, channel, antenna_name);
        for(int i = 0 ; i < antenna_count ; i++) {
            LMS_GetAntennaBW(device, LMS_CH_TX, channel, i, &antenna_bw);
            printf("Channel %d, antenna [%s] has BW [%lf .. %lf] (step %lf)" "\n", channel, antenna_name[i], antenna_bw.min, antenna_bw.max, antenna_bw.step);
            if (ANTENNA_AUTO == antenna && antenna_bw.min < TX_FREQUENCY && TX_FREQUENCY < antenna_bw.max) {
                antenna = i;
            }
        }
    }
    printf("Using antenna %d: [%s]" "\n", antenna, antenna_name[antenna]);
    free(antenna_name);

    // LMS_SetAntenna(device, LMS_CH_TX, channel, antenna); // SetLOFrequency should take care of selecting the proper antenna
    
    LMS_SetNormalizedGain(device, LMS_CH_TX, channel, gain);
    // Disable all other channels
    for (int i = 0; i < channel_count; ++i) {
        if (i == channel) continue;
        LMS_EnableChannel(device, LMS_CH_RX, i, false);
        LMS_EnableChannel(device, LMS_CH_TX, i, false);
    }
    // Enable our channel
    LMS_EnableChannel(device, LMS_CH_RX, channel, true);
    LMS_EnableChannel(device, LMS_CH_TX, channel, true);

    int setLOFrequency = LMS_SetLOFrequency(device, LMS_CH_TX, channel, TX_FREQUENCY);
    if (setLOFrequency) {
        printf("setLOFrequency(%lf)=%d(%s)" "\n", TX_FREQUENCY, setLOFrequency, LMS_GetLastErrorMessage());
        error(EXIT_CODE_LMS_INIT);
    }

#ifdef __USE_LPF__
    lms_range_t LPFBWRange;
    LMS_GetLPFBWRange(device, LMS_CH_TX, &LPFBWRange);
    printf("TX%d LPFBW [%lf .. %lf] (step %lf)" "\n", channel, LPFBWRange.min, LPFBWRange.max, LPFBWRange.step);
    double LPFBW = TX_BANDWIDTH;
    if (LPFBW < LPFBWRange.min) {
        LPFBW = LPFBWRange.min;
    }
    if (LPFBW > LPFBWRange.max) {
        LPFBW = LPFBWRange.min;
    }
    int setLPFBW = LMS_SetLPFBW(device, LMS_CH_TX, channel, LPFBW);
    if (setLPFBW) {
        printf("setLPFBW(%lf)=%d(%s)" "\n", LPFBW, setLPFBW, LMS_GetLastErrorMessage());
    }
    int enableLPF = LMS_SetLPF(device, LMS_CH_TX, channel, true);
    if (enableLPF) {
        printf("enableLPF=%d(%s)" "\n", enableLPF, LMS_GetLastErrorMessage());
    }
#endif

    lms_range_t sampleRateRange;
    int getSampleRateRange = LMS_GetSampleRateRange(device, LMS_CH_TX, &sampleRateRange);
    if (getSampleRateRange) {
        printf("getSampleRateRange=%d(%s)" "\n", getSampleRateRange, LMS_GetLastErrorMessage());
    } else {
        printf("sampleRateRange [%lf MHz.. %lf MHz] (step=%lf Hz)" "\n", sampleRateRange.min / 1e6, sampleRateRange.max / 1e6, sampleRateRange.step);
    }

    printf("Set sample rate to %lf ..." "\n", sampleRate);
    int setSampleRate = LMS_SetSampleRate(device, sampleRate, 0);
    if (setSampleRate) {
        printf("setSampleRate=%d(%s)" "\n", setSampleRate, LMS_GetLastErrorMessage());
        error(EXIT_CODE_LMS_INIT);
    }
    double actualHostSampleRate = 0.0;
    double actualRFSampleRate = 0.0;
    int getSampleRate = LMS_GetSampleRate(device, LMS_CH_TX, channel, &actualHostSampleRate, &actualRFSampleRate);
    if (getSampleRate) {
        printf("getSampleRate=%d(%s)" "\n", getSampleRate, LMS_GetLastErrorMessage());
    } else {
        printf("actualRate %lf (Host) / %lf (RF)" "\n", actualHostSampleRate, actualRFSampleRate);
    }

    printf("Calibrating ..." "\n");
    int calibrate = LMS_Calibrate(device, LMS_CH_TX, channel, TX_BANDWIDTH, 0);
    if (calibrate) {
        printf("calibrate=%d(%s)" "\n", calibrate, LMS_GetLastErrorMessage());
        error(EXIT_CODE_LMS_INIT);
    }

    printf("Setup TX stream ..." "\n");
    lms_stream_t tx_stream = {};
    tx_stream.isTx = true;                         // TX channel
    tx_stream.channel = (uint32_t)channel;         // channel number
    tx_stream.fifoSize = 1024 * 1024;              // fifo size in samples
    tx_stream.throughputVsLatency = 0.5f;          // 0 min latency, 1 max throughput
    tx_stream.dataFmt = lms_stream_t::LMS_FMT_I12; // 12-bit signed integer samples

    int setupStream = LMS_SetupStream(device, &tx_stream);
    if (setupStream) {
        printf("setupStream=%d(%s)" "\n", setupStream, LMS_GetLastErrorMessage());
        error(EXIT_CODE_LMS_INIT);
    }

    // File contains interleaved signed 16-bit IQ values, either with only 12-bit data, or with 16-bit data
    struct s16iq_sample_s {
        int16_t i;
        int16_t q;
    };
    // File contains interleaved signed  8-bit IQ values
    struct s8iq_sample_s {
        int8_t i;
        int8_t q;
    };

    int nSamples = (int)(sampleRate / 100);
    auto sampleBuffer = (struct s16iq_sample_s*)malloc(sizeof(struct s16iq_sample_s) * nSamples);

    LMS_StartStream(&tx_stream);

    int64_t loop_step = 0;
    auto print_progress = [&]() {
        if (0 != (loop_step++ % 100)) return;
        struct timeval tv{};
        char tm_buf[64];

        gettimeofday(&tv, nullptr);
        time_t current_time = tv.tv_sec;
        auto current_tm = localtime(&current_time);
        strftime(tm_buf, sizeof tm_buf, "%Y-%m-%d %H:%M:%S", current_tm);

#ifdef __APPLE__
        printf("gettimeofday() => %s.%06d ; ", tm_buf, tv.tv_usec);
#else
        printf("gettimeofday() => %s.%06ld ; ", tm_buf, tv.tv_usec);
#endif
        lms_stream_status_t status;
        LMS_GetStreamStatus(&tx_stream, &status); //Obtain TX stream stats
        printf("TX rate: %lf MiB/s" "\n", status.linkRate / (1<<20));
    };

    if ((12 == bits) || (16 == bits)) {
        while ((0 == control_c_received) && fread(sampleBuffer, sizeof(struct s16iq_sample_s), nSamples, stdin)) {
            print_progress();
            if (16 == bits) {
                // Scale down to 12-bit
                // Quick and dirty, so -1 (0xFFFF) to -15 (0xFFF1) scale down to -1 instead of 0
                for (int i = 0; i < nSamples; ++i) {
                    sampleBuffer[i].i >>= 4;
                    sampleBuffer[i].q >>= 4;
                }
            }
            int sendStream = LMS_SendStream(&tx_stream, sampleBuffer, nSamples, nullptr, 1000);
            if (sendStream < 0) {
                printf("sendStream %d(%s)" "\n", sendStream, LMS_GetLastErrorMessage());
            }
        }
    } else if (8 == bits) {
        auto fileSamples = (struct s8iq_sample_s*)malloc(sizeof(struct s8iq_sample_s) * nSamples);
        while ((0 == control_c_received) && fread(fileSamples, sizeof(struct s8iq_sample_s), nSamples, stdin)) {
            print_progress();
            // Up-Scale to 12-bit
            for (int i = 0; i < nSamples; ++i) {
                sampleBuffer[i].i = (int16_t) (fileSamples[i].i << 4);
                sampleBuffer[i].q = (int16_t) (fileSamples[i].q << 4);
            }
            int sendStream = LMS_SendStream(&tx_stream, sampleBuffer, nSamples, nullptr, 1000);
            if (sendStream < 0) {
                printf("sendStream %d(%s)" "\n", sendStream, LMS_GetLastErrorMessage());
            }
        }
        free(fileSamples);
    } else if (1 == bits) {
        // File contains interleaved signed 1-bit IQ values
        // Each byte is IQIQIQIQ
        int16_t expand_lut[256][8];
        for (int i = 0; i < 256; i++) {
            for (int j = 0; j < 8; j++) {
                expand_lut[i][j] = (int16_t)(((i >> (7 - j)) & 0x1) ? dynamic : -dynamic);
            }
        }
        printf("1-bit mode: using dynamic=%d" "\n", dynamic);
        // printf("sizeof(expand_lut[][])=%lu, sizeof(expand_lut[0])=%lu" "\n", sizeof(expand_lut), sizeof(expand_lut[0]));
        auto *fileBuffer = (int8_t*)malloc(sizeof(int8_t) * nSamples);
        while ((0 == control_c_received) && fread(fileBuffer, sizeof(int8_t), nSamples / 4, stdin)) {
            print_progress();
            // Expand
            int src = 0;
            int dst = 0;
            while (src < (nSamples / 4)) {
                memcpy(sampleBuffer + dst, expand_lut + fileBuffer[src], sizeof(expand_lut[0]));
                dst += 4;
                src++;
            }
            int sendStream = LMS_SendStream(&tx_stream, sampleBuffer, nSamples, nullptr, 1000);
            if (sendStream < 0) {
                printf("sendStream %d(%s)" "\n", sendStream, LMS_GetLastErrorMessage());
            }
        }
        free(fileBuffer);
    }

    printf("Releasing resources...\n");
    LMS_StopStream(&tx_stream);
    LMS_DestroyStream(device, &tx_stream);
    LMS_EnableChannel(device, LMS_CH_TX, channel, false);
    LMS_EnableChannel(device, LMS_CH_RX, channel, false);
    LMS_Close(device);
    device = nullptr;

    free(sampleBuffer);
    printf("Done.\n");

    if (control_c_received) {
        return (EXIT_CODE_CONTROL_C);
    }
    return 0;
}
