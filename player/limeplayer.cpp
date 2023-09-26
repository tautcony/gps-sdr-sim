#define _CRT_SECURE_NO_WARNINGS

#include <cstdio>
#include <cstring>
#include <ctime>
#include <csignal>
#include <string>
#include <algorithm>
#include <spdlog/spdlog.h>

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

#define FD_BUFFER_SIZE 8*1024

#define EXIT_CODE_CONTROL_C (SIGINT + 128)
#define EXIT_CODE_INVALID_ARGUMENTS (-3)
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
            "\t" "-a, --antenna <antenna> select antenna index in { 0, 1, 2, 3 } (default: " STRINGIFY(DEFAULT_ANTENNA) ")" "\n"
            "\t" "-b, --bits <bits>       configure IQ sample bit depth in { 1, 8, 12, 16 } (default: 16)" "\n"
            "\t" "-c, --channel <channel> select channel index in { 0, 1 } (default: 0)" "\n"
            "\t" "-d, --dynamic <dynamic> configure dynamic for the 1-bit mode (default: " STRINGIFY(MAX_DYNAMIC) ", max 12-bit signed value supported by LimeSDR)" "\n"
            "\t" "-f, --file <file>       read IQ samples from file instead of stdin" "\n"
            "\t" "-g, --gain <gain>       configure the so-called normalized RF gain in [0.0 .. 1.0] (default: 1.0 max RF power)" "\n"
            "\t" "-h, --help              print this help message" "\n"
            "\t" "-i, --index <index>     select specific LimeSDR device if multiple devices connected (default: 0)" "\n"
            "\t" "-l, --log-level <level> configure log level in { 0(trace), 1(debug), 2(info), 3(warn), 4(err), 5(critical), 6(off) } (default: 2)" "\n"
            "\t" "-s, --samplerate <samplerate>" "\n"
            "\t" "                        configure sampling rate for TX channels (default: " STRINGIFY(TX_SAMPLERATE) ")" "\n"
        "Example:" "\n"
        "\t" "./limeplayer -s 1000000 -b 1 -d 1023 -g 0.1 < ../circle.1b.1M.bin" "\n",
            program_name);
    exit(0);
}

lms_device_t *device = nullptr;
FILE* input_stream = nullptr;

int error(int exit_code) {
    if (device != nullptr) {
        LMS_Close(device);
    }
    if (input_stream != stdin) {
        fclose(input_stream);
    }
    exit(exit_code);
}

// File contains interleaved signed 16-bit IQ values, either with only 12-bit data, or with 16-bit data
typedef struct { int16_t i; int16_t q; } s16iq_sample_s;
// File contains interleaved signed  8-bit IQ values
typedef struct { int8_t i; int8_t q; } s8iq_sample_s;
// File contains interleaved 1-bit IQ values, Each byte is IQIQIQIQ
typedef int8_t iq_4_sample_s;

int main(int argc, char *const argv[]) {
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(control_c_handler, TRUE)) {
        spdlog::error("Could not set control handler.");
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

    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S %z] [%^%l%$] %v");
    LMS_RegisterLogHandler([](int lvl, const char* msg) {
        switch (lvl)
        {
            case LMS_LOG_CRITICAL: spdlog::critical(msg); break;
            case LMS_LOG_ERROR: spdlog::error(msg); break;
            case LMS_LOG_WARNING: spdlog::warn(msg); break;
            case LMS_LOG_INFO: spdlog::info(msg); break;
            case LMS_LOG_DEBUG: spdlog::debug(msg); break;
            default: break;
        }
    });

    double gain = 1.0;
    int32_t antenna = DEFAULT_ANTENNA;
    int32_t channel = 0;
    int32_t index = 0;
    int32_t bits = 16;
    double sampleRate = TX_SAMPLERATE;
    int32_t dynamic = 2047;
    std::string path;

    static struct option long_options[] = {
        {"antenna",    required_argument, nullptr, 'a'},
        {"bits",       required_argument, nullptr, 'b'},
        {"channel",    required_argument, nullptr, 'c'},
        {"dynamic",    required_argument, nullptr, 'd'},
        {"file",       required_argument, nullptr, 'f'},
        {"gain",       required_argument, nullptr, 'g'},
        {"help",       no_argument,       nullptr, 'h'},
        {"index",      required_argument, nullptr, 'i'},
        {"log-level",  optional_argument, nullptr, 'l'},
        {"samplerate", required_argument, nullptr, 's'},
        {nullptr,      no_argument,       nullptr, '\0'}
    };

    int rawLevel;
    while (true) {
        int option_index = 0;
        int c = getopt_long(argc, argv, "a:b:c:d:f:g:hl:i:s:", long_options, &option_index);
        if (c == -1) break;

        char *endptr = nullptr;
        switch (c) {
            case 'a': antenna    = (int32_t) strtol(optarg, &endptr, 10); break;
            case 'b': bits       = (int32_t) strtol(optarg, &endptr, 10); break;
            case 'c': channel    = (int32_t) strtol(optarg, &endptr, 10); break;
            case 'd': dynamic    = (int32_t) strtol(optarg, &endptr, 10); break;
            case 'f': path       = std::string(optarg);                   break;
            case 'g': gain       = strtod(optarg, &endptr);               break;
            case 'h':              print_usage(argv[0]);                  break;
            case 'i': index      = (int32_t) strtol(optarg, &endptr, 10); break;
            case 'l': rawLevel   = (int32_t) strtol(optarg, &endptr, 10); break;
            case 's': sampleRate =           strtod(optarg, &endptr);     break;
        }
        if (endptr != nullptr && *endptr != '\0') {
            spdlog::critical("Failed to parse argument for option -{} => {}", c, optarg);
            exit(EXIT_CODE_INVALID_ARGUMENTS);
        }
    }

    spdlog::level::level_enum logLevel = (spdlog::level::level_enum) std::min(std::max(0, rawLevel), (int)spdlog::level::off);
    spdlog::set_level(logLevel);

    if (path.empty()) {
        input_stream = stdin;
        SET_BINARY_MODE(STDIN);
    } else {
        input_stream = fopen(path.c_str(), "rb");
        if (input_stream == nullptr) {
            spdlog::critical("Failed to open signal file: {}", path);
            error(EXIT_CODE_INVALID_ARGUMENTS);
        }
    }

    int device_count = LMS_GetDeviceList(nullptr);
    if (device_count < 1) {
        spdlog::critical("no device connected");
        error(EXIT_CODE_NO_DEVICE);
    }
    auto *device_list = (lms_info_str_t *) malloc(sizeof(lms_info_str_t) * device_count);
    device_count = LMS_GetDeviceList(device_list);
    if (device_list != nullptr) {
        for (int i = 0; i < device_count; ++i) {
            spdlog::info("device[{}/{}]={}", i + 1, device_count, device_list[i]);
        }
    }

    // Use correct values
    // Use existing device
    if (index < 0 || index >= device_count) {
        index = 0;
    }
    spdlog::info("Using device index {} [{}]", index, device_list[index]);

    // Normalized gain shall be in [0.0 .. 1.0]
    gain = std::min(std::max(0.0, gain), 1.0);
    spdlog::info("Using normalized gain: {}", gain);

    dynamic = std::min(std::max(0, dynamic), MAX_DYNAMIC);
    spdlog::info("Using normalized dynamic: {}", dynamic);

    if (LMS_Open(&device, device_list[index], nullptr)) {
        error(EXIT_CODE_LMS_INIT);
    }
    free(device_list);

    if (LMS_Reset(device)) {
        spdlog::critical("LMS_Reset failed");
        error(EXIT_CODE_LMS_INIT);
    }
    if (LMS_Init(device)) {
        spdlog::critical("LMS_Init failed");
        error(EXIT_CODE_LMS_INIT);
    }

    int channel_count = LMS_GetNumChannels(device, LMS_CH_TX);
    if (channel_count < 0) {
        spdlog::critical("LMS_GetNumChannels failed");
        error(EXIT_CODE_LMS_INIT);
    }
    spdlog::info("Tx channel count: {}", channel_count);
    if (channel < 0 || channel >= channel_count) {
        channel = 0;
    }
    spdlog::info("Using channel: {}", channel);

    int antenna_count = LMS_GetAntennaList(device, LMS_CH_TX, channel, nullptr);
    if (antenna_count < 0) {
        spdlog::critical("LMS_GetAntennaList failed");
        error(EXIT_CODE_LMS_INIT);
    }
    spdlog::info("TX{} Channel has {} antenna(ae)", channel, antenna_count);
    if (antenna < 0 || antenna >= antenna_count) {
        antenna = DEFAULT_ANTENNA;
    }

    lms_name_t* antenna_name = (lms_name_t*)malloc(sizeof(lms_name_t) * antenna_count);
    lms_range_t antenna_bw{};
    if (antenna_count > 0) {
        LMS_GetAntennaList(device, LMS_CH_TX, channel, antenna_name);
        for(int i = 0 ; i < antenna_count ; i++) {
            LMS_GetAntennaBW(device, LMS_CH_TX, channel, i, &antenna_bw);
            spdlog::info("Channel {}, antenna [{}] has BW [{} .. {}] (step {})", channel, antenna_name[i], antenna_bw.min, antenna_bw.max, antenna_bw.step);
            if (ANTENNA_AUTO >= antenna_count &&
                ANTENNA_AUTO == antenna &&
                antenna_bw.min < TX_FREQUENCY && TX_FREQUENCY < antenna_bw.max) {
                antenna = i;
            }
        }
    }
    spdlog::info("Using antenna {}: [{}]", antenna, antenna_name[antenna]);
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

    if (LMS_SetLOFrequency(device, LMS_CH_TX, channel, TX_FREQUENCY)) {
        spdlog::critical("setLOFrequency({})=({})", TX_FREQUENCY, LMS_GetLastErrorMessage());
        error(EXIT_CODE_LMS_INIT);
    }

#ifdef __USE_LPF__
    lms_range_t LPFBWRange{};
    LMS_GetLPFBWRange(device, LMS_CH_TX, &LPFBWRange);
    spdlog::info("TX{} LPFBW [{} .. {}] (step {})", channel, LPFBWRange.min, LPFBWRange.max, LPFBWRange.step);
    double LPFBW = TX_BANDWIDTH;
    if (LPFBW < LPFBWRange.min || LPFBW > LPFBWRange.max) {
        LPFBW = LPFBWRange.min;
    }
    if (LMS_SetLPFBW(device, LMS_CH_TX, channel, LPFBW)) {
        spdlog::error("LMS_SetLPFBW({}) failed", LPFBW);
    }
    if (LMS_SetLPF(device, LMS_CH_TX, channel, true)) {
        spdlog::error("LMS_SetLPF(true) failed");
    }
#endif

    lms_range_t sampleRateRange{};
    if (LMS_GetSampleRateRange(device, LMS_CH_TX, &sampleRateRange)) {
        spdlog::error("LMS_GetSampleRateRange failed");
    } else {
        spdlog::info("SampleRateRange: [{} MHz .. {} MHz] (step={} Hz)", sampleRateRange.min / 1e6, sampleRateRange.max / 1e6, sampleRateRange.step);
    }

    spdlog::info("Set sample rate to {} Hz ...", sampleRate);
    if (LMS_SetSampleRate(device, sampleRate, 0)) {
        spdlog::critical("LMS_SetSampleRate({}) failed", sampleRate);
        error(EXIT_CODE_LMS_INIT);
    }
    double actualHostSampleRate = 0.0;
    double actualRFSampleRate = 0.0;
    if (LMS_GetSampleRate(device, LMS_CH_TX, channel, &actualHostSampleRate, &actualRFSampleRate)) {
        spdlog::error("LMS_GetSampleRate failed");
    } else {
        spdlog::info("actualRate {} Hz (Host) / {} Hz (RF)", actualHostSampleRate, actualRFSampleRate);
    }

    spdlog::info("Calibrating ...");
    if (LMS_Calibrate(device, LMS_CH_TX, channel, TX_BANDWIDTH, 0)) {
        spdlog::critical("LMS_Calibrate failed");
        error(EXIT_CODE_LMS_INIT);
    }

    spdlog::info("Setup TX stream ...");
    lms_stream_t tx_stream{};
    tx_stream.isTx = true;                         // TX channel
    tx_stream.channel = (uint32_t)channel;         // channel number
    tx_stream.fifoSize = 1024 * 1024;              // fifo size in samples
    tx_stream.throughputVsLatency = 0.5f;          // 0 min latency, 1 max throughput
    tx_stream.dataFmt = 16 == bits ?
                        lms_stream_t::LMS_FMT_I16 :
                        lms_stream_t::LMS_FMT_I12; // 12-bit/16-bit data format

    lms_stream_meta_t tx_meta{};
    tx_meta.waitForTimestamp = true;               // wait for HW timestamp to send samples
    tx_meta.flushPartialPacket = false;            // send samples to HW after packet is completely filled

    if (LMS_SetupStream(device, &tx_stream)) {
        spdlog::critical("LMS_SetupStream failed");
        error(EXIT_CODE_LMS_INIT);
    }
    if (LMS_StartStream(&tx_stream)) {
        spdlog::critical("LMS_StartStream failed");
        error(EXIT_CODE_LMS_INIT);
    }

    int nSamples = (int)(sampleRate / 100);
    if (1 == bits) {
        // trim extra samples in 1-bit mode
        nSamples -= nSamples % 4;
    }
    auto sampleBuffer   = (s16iq_sample_s*)malloc(sizeof(s16iq_sample_s) * nSamples);
    auto fileBuffer8bit = (s8iq_sample_s*) malloc(sizeof(s8iq_sample_s) * nSamples);
    auto fileBuffer1bit = (iq_4_sample_s*)malloc(sizeof(iq_4_sample_s) * nSamples / 4);

    int64_t loop_step = 0;
    auto print_progress = [&]() {
        if (0 != (loop_step++ % 100)) return;
        lms_stream_status_t status{};
        if (LMS_GetStreamStatus(&tx_stream, &status)) {
            spdlog::error("LMS_GetStreamStatus failed");
        }
        spdlog::info("TX rate: {:06f} MiB/s", status.linkRate / (1LL << 20));
    };

    int16_t expand_lut[1 << 8][8] = {};
    for (int i = 0; i < 256; i++) {
        for (int j = 0; j < 8; j++) {
            expand_lut[i][j] = (int16_t)(((i >> (7 - j)) & 0x1) ? dynamic : -dynamic);
        }
    }

    double transmitBandwidth = sampleRate * (bits == 16 ? 16 : 12) * 2 / 8 / (1LL << 20);
    spdlog::info("transmit bit mode: {}-bit, sample rate: {} Hz, expected bandwidth: {} MiB/s", bits, sampleRate, transmitBandwidth);

    int sampleCount = 0;
    while (0 == control_c_received) {
        if (12 == bits || 16 == bits) {
            sampleCount = (int) fread(sampleBuffer, sizeof(s16iq_sample_s), nSamples, input_stream);
            if (0 == sampleCount) {
                break;
            }
        } else if (8 == bits) {
            sampleCount = (int) fread(fileBuffer8bit, sizeof(s8iq_sample_s), nSamples, input_stream);
            if (0 == sampleCount) {
                break;
            }
            // Up-Scale to 12-bit
            for (int i = 0; i < sampleCount; ++i) {
                sampleBuffer[i].i = fileBuffer8bit[i].i << 4;
                sampleBuffer[i].q = fileBuffer8bit[i].q << 4;
            }
        } else if (1 == bits) {
            sampleCount = (int) fread(fileBuffer1bit, sizeof(iq_4_sample_s), nSamples / 4, input_stream);
            if (0 == sampleCount) {
                break;
            }
            for (int i = 0, offset = 0; i < sampleCount; ++i, offset += 4) {
                memcpy(sampleBuffer + offset, expand_lut + fileBuffer1bit[i], sizeof(expand_lut[0]));
            }
            sampleCount *= 4;
        }
        print_progress();
        int sentSampleCount = LMS_SendStream(&tx_stream, sampleBuffer, sampleCount, nullptr, 1000);
        if (sentSampleCount < 0) {
            spdlog::error("LMS_SendStream failed");
        }
        tx_meta.timestamp += sentSampleCount;
    }

    spdlog::info("Total transmit duration: {}s", tx_meta.timestamp / sampleRate);

    spdlog::info("Releasing resources...");
    if (input_stream != stdin) {
        fclose(input_stream);
        input_stream = nullptr;
    }
    free(sampleBuffer);
    free(fileBuffer8bit);
    free(fileBuffer1bit);
    LMS_StopStream(&tx_stream);
    LMS_DestroyStream(device, &tx_stream);
    LMS_EnableChannel(device, LMS_CH_TX, channel, false);
    LMS_EnableChannel(device, LMS_CH_RX, channel, false);
    LMS_Close(device);
    device = nullptr;
    spdlog::info("Done");

    if (control_c_received) {
        return (EXIT_CODE_CONTROL_C);
    }
    return 0;
}
