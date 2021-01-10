////////////////////////////////////////////////////////////////////////////
//                       **** WAVPACK-STREAM ****                         //
//                      Streaming Audio Compressor                        //
//                Copyright (c) 1998 - 2020 David Bryant.                 //
//                          All Rights Reserved.                          //
//      Distributed under the BSD Software License (see license.txt)      //
////////////////////////////////////////////////////////////////////////////

// wvtest.c

// This is the main module for the WavPack command-line library tester.

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <pthread.h>

#include "wavpack-stream.h"
#include "utils.h"                  // for PACKAGE_VERSION, etc.
#include "md5.h"

#define CLEAR(destin) memset (&destin, 0, sizeof (destin));

#ifndef M_PI
#define M_PI 3.14159265358979323
#endif

static const char *sign_on = "\n"
" WVTEST-STREAM  Streaming Audio Compression Tester  %s Version %s\n"
" Copyright (c) 2019 David Bryant.  All Rights Reserved.\n\n";

static const char *version_warning = "\n"
" WARNING: WVTEST using libwavpack-stream version %s, expected %s (see README)\n\n";

static const char *usage =
" Usage:   WVTEST-STREAM --default|--exhaustive [-options]\n\n"
" Options: --default           = perform the default test suite\n"
"          --exhaustive        = perform the exhaustive test suite\n"
"          --short             = perform shorter runs of each test\n"
"          --long              = perform longer runs of each test\n"
"          --fuzz-period=n     = fuzz at specified average period in bytes\n"
"                                (decode errors reported and ignored)\n"
"          --no-decode         = skip the decoding process\n"
"          --no-extras         = skip the \"extra\" modes\n"
"          --no-hybrid         = skip the hybrid modes\n"
"          --no-floats         = skip the float modes\n"
"          --no-lossy          = skip the lossy modes\n"
"          --no-speeds         = skip the speed modes (fast, high, etc.)\n"
"          --help              = display this message\n"
"          --version           = write the version to stdout\n"
"          --write=n[-n][,...] = write specific test(s) (or range(s)) to disk\n\n"
" Web:     Visit www.wavpack.com for latest version and info\n";

#define TEST_FLAG_EXTRA_MODE(x) ((x) & TEST_FLAG_EXTRA_MASK)
#define TEST_FLAG_EXTRA_MASK            0x7
#define TEST_FLAG_FLOAT_DATA            0x8
#define TEST_FLAG_WRITE_FILE            0x10
#define TEST_FLAG_DEFAULT               0x20
#define TEST_FLAG_EXHAUSTIVE            0x40
#define TEST_FLAG_NO_FLOATS             0x80
#define TEST_FLAG_NO_HYBRID             0x100
#define TEST_FLAG_NO_EXTRAS             0x200
#define TEST_FLAG_NO_LOSSY              0x400
#define TEST_FLAG_NO_SPEEDS             0x800
#define TEST_FLAG_STORE_FLOAT_AS_INT32  0x1000
#define TEST_FLAG_STORE_INT32_AS_FLOAT  0x2000
#define TEST_FLAG_IGNORE_WVC            0x4000
#define TEST_FLAG_NO_DECODE             0x8000

static int run_test_size_modes (int wpconfig_flags, int test_flags, int base_minutes, int fuzz_period);
static int run_test_speed_modes (int wpconfig_flags, int test_flags, int bits, int num_chans, int num_seconds, int fuzz_period);
static int run_test_extra_modes (int wpconfig_flags, int test_flags, int bits, int num_chans, int num_seconds, int fuzz_period);
static int run_test (int wpconfig_flags, int test_flags, int bits, int num_chans, int num_seconds, int fuzz_period);

#define NUM_WRITE_RANGES 10
static struct { int start, stop; } write_ranges [NUM_WRITE_RANGES];
int number_of_ranges;

enum generator_type { noise, tone };

struct audio_generator {
    enum generator_type type;
    union {
        struct noise_generator {
            float sum1, sum2, sum2p;                // these are changing
            float factor, scalar;                   // these are constant
        } noise_cxt;

        struct tone_generator {
            int sample_rate, samples_per_update;    // these are constant
            int high_frequency, low_frequency;      // these are constant
            float angle, velocity, acceleration;    // these are changing
            int samples_left;
        } tone_cxt;    
    } u;
};

static void tone_generator_init (struct audio_generator *cxt, int sample_rate, int low_freq, int high_freq);
static void noise_generator_init (struct audio_generator *cxt, float factor);
static void audio_generator_run (struct audio_generator *cxt, float *samples, int num_samples);
static void mix_samples_with_gain (float *destin, float *source, int num_samples, int num_chans, float initial_gain, float final_gain);
static void truncate_float_samples (float *samples, int num_samples, int bits);
static void float_to_integer_samples (float *samples, int num_samples, int bits);
static void float_to_32bit_integer_samples (float *samples, int num_samples);
static void *store_samples (void *dst, int32_t *src, int qmode, int bps, int count);
static void frandom_set_seed (uint64_t seed);
static uint64_t frandom_get_seed (void);
static double frandom (void);

typedef struct {
    uint32_t buffer_size, bytes_written, bytes_read, first_block_size;
    volatile unsigned char *buffer_base, *buffer_head, *buffer_tail;
    int push_back, done, error, empty_waits, full_waits, fuzz_period;
    pthread_cond_t cond_read, cond_write;
    pthread_mutex_t mutex;
    FILE *file;
} StreamingFile;

typedef struct {
    StreamingFile *wv_stream, *wvc_stream;
    unsigned char md5_decoded [16];
    uint32_t sample_count;
    int num_errors;
} WavpackDecoder;

static void initialize_stream (StreamingFile *ws, int buffer_size, int fuzz_period);
static int write_block (void *id, void *data, int32_t length);
static void flush_stream (StreamingFile *ws);
static void free_stream (StreamingFile *ws);
static void *decode_thread (void *threadid);
static WavpackReader freader;

//////////////////////////////////////// main () function for CLI //////////////////////////////////////

int main (argc, argv) int argc; char **argv;
{
    int wpconfig_flags = CONFIG_MD5_CHECKSUM | CONFIG_OPTIMIZE_MONO, test_flags = 0, base_minutes = 2, res;
    int fuzz_period = 0;

    // loop through command-line arguments

    while (--argc) {
        if (**++argv == '-' && (*argv)[1] == '-' && (*argv)[2]) {
            char *long_option = *argv + 2, *long_param = long_option;

            while (*long_param)
                if (*long_param++ == '=')
                    break;

            if (!strcmp (long_option, "help")) {                        // --help
                printf ("%s", usage);
                return 0;
            }
            else if (!strcmp (long_option, "version")) {                // --version
                printf ("wvtest-stream %s\n", PACKAGE_VERSION);
                printf ("libwavpack-stream %s\n", WavpackStreamGetLibraryVersionString ());
                return 0;
            }
            else if (!strcmp (long_option, "short")) {                  // --short
                base_minutes = 1;
            }
            else if (!strcmp (long_option, "long")) {                   // --long
                base_minutes = 5;
            }
            else if (!strcmp (long_option, "default")) {                // --default
                test_flags |= TEST_FLAG_DEFAULT;
            }
            else if (!strcmp (long_option, "exhaustive")) {             // --exhaustive
                test_flags |= TEST_FLAG_EXHAUSTIVE;
            }
            else if (!strcmp (long_option, "no-extras")) {              // --no-extras
                test_flags |= TEST_FLAG_NO_EXTRAS;
            }
            else if (!strcmp (long_option, "no-hybrid")) {              // --no-hybrid
                test_flags |= TEST_FLAG_NO_HYBRID;
            }
            else if (!strcmp (long_option, "no-lossy")) {               // --no-lossy
                test_flags |= TEST_FLAG_NO_LOSSY;
            }
            else if (!strcmp (long_option, "no-speeds")) {              // --no-speeds
                test_flags |= TEST_FLAG_NO_SPEEDS;
            }
            else if (!strcmp (long_option, "no-floats")) {              // --no-floats
                test_flags |= TEST_FLAG_NO_FLOATS;
            }
            else if (!strcmp (long_option, "no-decode")) {              // --no-decode
                test_flags |= TEST_FLAG_NO_DECODE;
            }
            else if (!strncmp (long_option, "fuzz-period", 11)) {       // --fuzz-period
                fuzz_period = strtol (long_param, NULL, 10);

                if (fuzz_period < 10 || fuzz_period > 1000000) {
                    printf ("invalid fuzz period, must be 10 - 1000000 bytes!\n");
                    return 1;
                }
            }
            else if (!strncmp (long_option, "write", 5)) {              // --write
                for (number_of_ranges = 0; *long_param && isdigit (*long_param) && number_of_ranges < NUM_WRITE_RANGES;) {
                    write_ranges [number_of_ranges].start = strtol (long_param, &long_param, 10);

                    if (*long_param == '-') {
                        long_param++;
                        if (isdigit (*long_param))
                            write_ranges [number_of_ranges].stop = strtol (long_param, &long_param, 10);
                        else
                            break;
                    }
                    else
                        write_ranges [number_of_ranges].stop = write_ranges [number_of_ranges].start;

                    number_of_ranges++;

                    if (*long_param == ',')
                        long_param++;
                    else
                        break;
                }

                if (*long_param || !number_of_ranges) {
                    printf ("syntax error in write specification!\n");
                    return 1;
                }
                else
                    test_flags |= TEST_FLAG_WRITE_FILE;
            }
            else {
                printf ("unknown option: %s !\n", long_option);
                return 1;
            }
        }
        else {
            printf ("unknown option: %s !\n", *argv);
            return 1;
        }
    }

    if (strcmp (WavpackStreamGetLibraryVersionString (), PACKAGE_VERSION))
        printf (version_warning, WavpackStreamGetLibraryVersionString (), PACKAGE_VERSION);
    else
        printf (sign_on, VERSION_OS, WavpackStreamGetLibraryVersionString ());

    if (!(test_flags & (TEST_FLAG_DEFAULT | TEST_FLAG_EXHAUSTIVE))) {
        puts (usage);
        return 1;
    }

    printf ("\n\n                          ****** pure lossless ******\n");
    res = run_test_size_modes (wpconfig_flags, test_flags, base_minutes, fuzz_period);
    if (res) goto done;

    if (!(test_flags & TEST_FLAG_NO_HYBRID)) {
        if (!fuzz_period) {
            printf ("\n\n                         ****** hybrid lossless ******\n");
            res = run_test_size_modes (wpconfig_flags | CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC, test_flags, base_minutes, fuzz_period);
            if (res) goto done;
        }

        if (!(test_flags & TEST_FLAG_NO_LOSSY)) {
            printf ("\n\n                          ****** hybrid lossy ******\n");
            res = run_test_size_modes (wpconfig_flags | CONFIG_HYBRID_FLAG, test_flags, base_minutes, fuzz_period);
            if (res) goto done;

            printf ("\n\n            ****** hybrid lossless (but ignore wpsc on decode) ******\n");
            res = run_test_size_modes (wpconfig_flags | CONFIG_HYBRID_FLAG | CONFIG_CREATE_WVC,
                test_flags | TEST_FLAG_IGNORE_WVC, base_minutes, fuzz_period);
            if (res) goto done;
        }
    }

done:
    if (res)
        printf ("\ntest failed!\n\n");
    else
        printf ("\nall tests pass\n\n");

    return res;
}

// Given a WavPack configuration and test flags, run the various combinations of
// bit-depth and channel configurations. A return value of FALSE indicates an error.

static int run_test_size_modes (int wpconfig_flags, int test_flags, int base_minutes, int fuzz_period)
{
    int res;

    printf ("\n   *** 8-bit, mono ***\n");
    res = run_test_speed_modes (wpconfig_flags, test_flags, 8, 1, base_minutes*5*60, fuzz_period);
    if (res) return res;

    if (test_flags & TEST_FLAG_EXHAUSTIVE) {
        printf ("\n   *** 16-bit, mono ***\n");
        res = run_test_speed_modes (wpconfig_flags, test_flags, 16, 1, base_minutes*5*60, fuzz_period);
        if (res) return res;
    }

    printf ("\n   *** 16-bit, stereo ***\n");
    res = run_test_speed_modes (wpconfig_flags, test_flags, 16, 2, base_minutes*3*60, fuzz_period);
    if (res) return res;

    if ((test_flags & TEST_FLAG_EXHAUSTIVE) && !(test_flags & TEST_FLAG_NO_FLOATS)) {
        printf ("\n   *** 16-bit (converted to float), stereo ***\n");
        res = run_test_speed_modes (wpconfig_flags, test_flags | TEST_FLAG_FLOAT_DATA, 16, 2, base_minutes*3*60, fuzz_period);
        if (res) return res;
    }

    printf ("\n   *** 24-bit, 5.1 channels ***\n");
    res = run_test_speed_modes (wpconfig_flags, test_flags, 24, 6, base_minutes*60, fuzz_period);
    if (res) return res;

    if (test_flags & TEST_FLAG_EXHAUSTIVE) {
        if (!(test_flags & TEST_FLAG_NO_FLOATS)) {
            printf ("\n   *** 24-bit (converted to float), 5.1 channels ***\n");
            res = run_test_speed_modes (wpconfig_flags, test_flags | TEST_FLAG_FLOAT_DATA, 24, 6, base_minutes*60, fuzz_period);
            if (res) return res;
        }
 
        printf ("\n   *** 32-bit integer, 5.1 channels ***\n");
        res = run_test_speed_modes (wpconfig_flags, test_flags, 32, 6, base_minutes*60, fuzz_period);
        if (res) return res;

        if (!(test_flags & TEST_FLAG_NO_FLOATS)) {
            printf ("\n   *** 32-bit float stored as integer (pathological), 5.1 channels ***\n");
            res = run_test_speed_modes (wpconfig_flags, test_flags | TEST_FLAG_STORE_FLOAT_AS_INT32, 32, 6, base_minutes*60, fuzz_period);
            if (res) return res;
        
            if (!(wpconfig_flags & CONFIG_HYBRID_FLAG)) {
                printf ("\n   *** 32-bit integer stored as float (pathological), 5.1 channels ***\n");
                res = run_test_speed_modes (wpconfig_flags, test_flags | TEST_FLAG_STORE_INT32_AS_FLOAT, 32, 6, base_minutes*60, fuzz_period);
                if (res) return res;
            }
        }
    }

    if (!(test_flags & TEST_FLAG_NO_FLOATS)) {
        printf ("\n   *** 32-bit float, 5.1 channels ***\n");
        res = run_test_speed_modes (wpconfig_flags, test_flags | TEST_FLAG_FLOAT_DATA, 32, 6, base_minutes*60, fuzz_period);
        if (res) return res;
    }

    return 0;
}

// Given a WavPack configuration and test flags, run the various combinations of
// speed modes (i.e, fast, high, etc). A return value of FALSE indicates an error.

static int run_test_speed_modes (int wpconfig_flags, int test_flags, int bits, int num_chans, int num_seconds, int fuzz_period)
{
    int res;

    if (!(test_flags & TEST_FLAG_NO_SPEEDS)) {
        res = run_test_extra_modes (wpconfig_flags | CONFIG_FAST_FLAG, test_flags, bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    res = run_test_extra_modes (wpconfig_flags, test_flags, bits, num_chans, num_seconds, fuzz_period);
    if (res) return res;

    if (!(test_flags & TEST_FLAG_NO_SPEEDS)) {
        res = run_test_extra_modes (wpconfig_flags | CONFIG_HIGH_FLAG, test_flags, bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    if (!(test_flags & TEST_FLAG_NO_SPEEDS)) {
        res = run_test_extra_modes (wpconfig_flags | CONFIG_VERY_HIGH_FLAG, test_flags, bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    return 0;
}

// Given a WavPack configuration and test flags, run the various combinations of "extra" modes (0-6).
// Note that except for the base mode (no extra), the "default" and "exhaustive" configurations do
// different extra modes. Combining the "default" and "exhaustive" configurations does all the extra
// modes. A return value of FALSE indicates an error.

static int run_test_extra_modes (int wpconfig_flags, int test_flags, int bits, int num_chans, int num_seconds, int fuzz_period)
{
    int res;

    res = run_test (wpconfig_flags, test_flags, bits, num_chans, num_seconds, fuzz_period);
    if (res) return res;

    if (test_flags & TEST_FLAG_NO_EXTRAS)
        return 0;

    if (test_flags & TEST_FLAG_EXHAUSTIVE) {
        res = run_test (wpconfig_flags, test_flags | TEST_FLAG_EXTRA_MODE (1), bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    if (test_flags & TEST_FLAG_DEFAULT) {
        res = run_test (wpconfig_flags, test_flags | TEST_FLAG_EXTRA_MODE (2), bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    if (test_flags & TEST_FLAG_EXHAUSTIVE) {
        res = run_test (wpconfig_flags, test_flags | TEST_FLAG_EXTRA_MODE (3), bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    if (test_flags & TEST_FLAG_EXHAUSTIVE) {
        res = run_test (wpconfig_flags, test_flags | TEST_FLAG_EXTRA_MODE (4), bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    if (test_flags & TEST_FLAG_DEFAULT) {
        res = run_test (wpconfig_flags, test_flags | TEST_FLAG_EXTRA_MODE (5), bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    if (test_flags & TEST_FLAG_EXHAUSTIVE) {
        res = run_test (wpconfig_flags, test_flags | TEST_FLAG_EXTRA_MODE (6), bits, num_chans, num_seconds, fuzz_period);
        if (res) return res;
    }

    return 0;
}

// Given a WavPack configuration and test flags, actually run the specified test. This entails
// generating the actual audio test data, creating the "virtual" WavPack file and writing to it,
// and spawning the thread that will read the "virtual" file and do the decoding (which is obviously
// required to verify the entire encode/decode chain). For lossless modes, and MD5 hash is used to
// verify the result, otherwise the decoder is trusted to detect and report errors (although the
// total number of samples is verified).

#define BUFFER_SIZE 1000000
#define NUM_GENERATORS 6

struct audio_channel {
    float audio_gain_hist [NUM_GENERATORS], audio_gain [NUM_GENERATORS], angle_offset;
    int lfe_flag;
};

#define SAMPLE_RATE 44100
#define ENCODE_SAMPLES 128
#define NOISE_GAIN 0.6667
#define TONE_GAIN 0.3333

static int run_test (int wpconfig_flags, int test_flags, int bits, int num_chans, int num_seconds, int fuzz_period)
{
    static int test_number;

    float sequencing_angle = 0.0, speed = 60.0, width = 200.0, *source, *destin, ratio, bps;
    int lossless = !(wpconfig_flags & CONFIG_HYBRID_FLAG) || ((wpconfig_flags & CONFIG_CREATE_WVC) && !(test_flags & TEST_FLAG_IGNORE_WVC));
    char md5_string1 [] = "????????????????????????????????";
    char md5_string2 [] = "????????????????????????????????";
    uint32_t total_encoded_bytes, total_encoded_samples;
    struct audio_generator generators [NUM_GENERATORS];
    int seconds = 0, samples = 0, wc = 0, chan_mask;
    char *filename = NULL, mode_string [32] = "-";
    struct audio_channel *channels;
    pthread_t pthread;
    WavpackContext *out_wpc;
    WavpackStreamConfig wpconfig;
    StreamingFile wv_stream, wvc_stream;
    WavpackDecoder wv_decoder;
    unsigned char md5_encoded [16];
    MD5_CTX md5_context;
    void *term_value;
    int i, j, k;

    if (wpconfig_flags & CONFIG_FAST_FLAG)
        strcat (mode_string, "f");
    else if (wpconfig_flags & CONFIG_HIGH_FLAG)
        strcat (mode_string, "h");
    else if (wpconfig_flags & CONFIG_VERY_HIGH_FLAG)
        strcat (mode_string, "hh");

    printf ("test %04d...", ++test_number); fflush (stdout);
    MD5_Init (&md5_context);

    noise_generator_init (&generators [0], 128.0);
    tone_generator_init (&generators [1], SAMPLE_RATE, 20, 200);
    noise_generator_init (&generators [2], 12.0);
    tone_generator_init (&generators [3], SAMPLE_RATE, 200, 2000);
    noise_generator_init (&generators [4], 1.75);
    tone_generator_init (&generators [5], SAMPLE_RATE, 2000, 20000);

    CLEAR (wpconfig);
    CLEAR (wv_decoder);
    CLEAR (wv_stream);
    CLEAR (wvc_stream);

    channels = malloc (num_chans * sizeof (*channels));
    source = malloc (ENCODE_SAMPLES * sizeof (*source));
    destin = malloc (ENCODE_SAMPLES * num_chans * sizeof (*destin));

    if (!channels || !source || !destin) {
        printf ("run_test(): can't allocate memory!\n");
        exit (-1);
    }

    memset (channels, 0, num_chans * sizeof (*channels));

    switch (num_chans) {
        case 1:
            channels [0].angle_offset = 0.0;
            chan_mask = 0x4;
            break;

        case 2:
            channels [0].angle_offset -= M_PI / 24.0;
            channels [1].angle_offset += M_PI / 24.0;
            chan_mask = 0x3;
            break;

        case 4:
            channels [0].angle_offset -= M_PI / 24.0;
            channels [1].angle_offset += M_PI / 24.0;
            channels [2].angle_offset -= 23.0 * M_PI / 24.0;
            channels [3].angle_offset += 23.0 * M_PI / 24.0;
            chan_mask = 0x33;
            break;

        case 6:
            channels [0].angle_offset -= M_PI / 24.0;
            channels [1].angle_offset += M_PI / 24.0;
            channels [3].lfe_flag = 1;
            channels [4].angle_offset -= 23.0 * M_PI / 24.0;
            channels [5].angle_offset += 23.0 * M_PI / 24.0;
            chan_mask = 0x3F;
            break;

        default:
            printf ("invalid channel count = %d\n", num_chans);
            exit (-1);
    }

    if (!(test_flags & TEST_FLAG_NO_DECODE)) {
        initialize_stream (&wv_stream, BUFFER_SIZE, fuzz_period);
        wv_decoder.wv_stream = &wv_stream;
    }
    else
        initialize_stream (&wv_stream, 0, 0);

    if (test_flags & TEST_FLAG_WRITE_FILE) {
        int i;

        for (i = 0; i < number_of_ranges; ++i)
            if (test_number >= write_ranges [i].start && test_number <= write_ranges [i].stop) {
                filename = malloc (32);

                if (!filename) {
                    printf ("run_test(): can't allocate memory!\n");
                    exit (-1);
                }

                sprintf (filename, "testfile-%04d.wps", test_number);

                if (((wv_stream.file = fopen (filename, "w+b")) == NULL)) {
                    printf ("can't create file %s!\n", filename);
                    free_stream (&wv_stream);
                    return 1;
                }

                break;
            }
    }

    if (wpconfig_flags & CONFIG_CREATE_WVC) {
        if (!(test_flags & (TEST_FLAG_IGNORE_WVC | TEST_FLAG_NO_DECODE))) {
            initialize_stream (&wvc_stream, BUFFER_SIZE, fuzz_period);
            wv_decoder.wvc_stream = &wvc_stream;
        }
        else
            initialize_stream (&wvc_stream, 0, 0);

        if (filename) {
            char *filename_c = malloc (strlen (filename) + 10);

            strcpy (filename_c, filename);
            strcat (filename_c, "c");

            if ((wvc_stream.file = fopen (filename_c, "w+b")) == NULL) {
                printf ("can't create file %s!\n", filename_c);
                free_stream (&wv_stream);
                free_stream (&wvc_stream);
                return 1;
            }

            free (filename_c);
        }
    }

    out_wpc = WavpackStreamOpenFileOutput (write_block, &wv_stream, (wpconfig_flags & CONFIG_CREATE_WVC) ? &wvc_stream : NULL);

    if (!(test_flags & TEST_FLAG_NO_DECODE))
        pthread_create (&pthread, NULL, decode_thread, (void *) &wv_decoder);

    if (test_flags & (TEST_FLAG_FLOAT_DATA | TEST_FLAG_STORE_INT32_AS_FLOAT)) {
        wpconfig.float_norm_exp = 127;
        wpconfig.bytes_per_sample = 4;
        wpconfig.bits_per_sample = 32;
    }
    else {
        wpconfig.bytes_per_sample = (bits + 7) >> 3;
        wpconfig.bits_per_sample = bits;
    }

    if (test_flags & TEST_FLAG_EXTRA_MASK) {
        sprintf (mode_string + strlen (mode_string), "x%c", '0' + (test_flags & TEST_FLAG_EXTRA_MASK));
        wpconfig.xmode = test_flags & TEST_FLAG_EXTRA_MASK;
        wpconfig_flags |= CONFIG_EXTRA_MODE;
    }

    wpconfig.sample_rate = SAMPLE_RATE;
    wpconfig.num_channels = num_chans;
    wpconfig.channel_mask = chan_mask;
    wpconfig.flags = wpconfig_flags;

    if (wpconfig_flags & CONFIG_HYBRID_FLAG) {
        if (wpconfig_flags & CONFIG_CREATE_WVC) {
            if (test_flags & TEST_FLAG_IGNORE_WVC) {
                strcat (mode_string, "b4c");
                wpconfig.bitrate = 4.0;
            }
            else {
                strcat (mode_string, "b3c");
                wpconfig.bitrate = 3.0;
            }
        }
        else {
            strcat (mode_string, "b5");
            wpconfig.bitrate = 5.0;
        }
    }

    WavpackStreamSetConfiguration64 (out_wpc, &wpconfig, -1, NULL);
    WavpackStreamPackInit (out_wpc);

    while (seconds < num_seconds) {

        double translated_angle = cos (sequencing_angle) * 100.0;
        double width_scalar = pow (2.0, -width);

        for (k = 0; k < num_chans; ++k) {
            channels [k].audio_gain [0] = pow (sin (translated_angle + channels [k].angle_offset - M_PI * 1.6667) + 1.0, width) * width_scalar * NOISE_GAIN;
            channels [k].audio_gain [1] = pow (sin (translated_angle + channels [k].angle_offset - M_PI * 0.6667) + 1.0, width) * width_scalar * TONE_GAIN;
            channels [k].audio_gain [2] = pow (sin (translated_angle + channels [k].angle_offset - M_PI * 0.3333) + 1.0, width) * width_scalar * NOISE_GAIN;
            channels [k].audio_gain [3] = pow (sin (translated_angle + channels [k].angle_offset - M_PI * 1.3333) + 1.0, width) * width_scalar * TONE_GAIN;
            channels [k].audio_gain [4] = pow (sin (translated_angle + channels [k].angle_offset - M_PI) + 1.0, width) * width_scalar * NOISE_GAIN;
            channels [k].audio_gain [5] = pow (sin (translated_angle + channels [k].angle_offset) + 1.0, width) * width_scalar * TONE_GAIN;
        }

        memset (destin, 0, ENCODE_SAMPLES * num_chans * sizeof (*destin));

        for (j = 0; j < NUM_GENERATORS; ++j) {
            audio_generator_run (&generators [j], source, ENCODE_SAMPLES);

            for (k = 0; k < num_chans; ++k) {
                if (!channels [k].lfe_flag || j < 2)
                    mix_samples_with_gain (destin + k, source, ENCODE_SAMPLES, num_chans, channels [k].audio_gain_hist [j], channels [k].audio_gain [j]);

                channels [k].audio_gain_hist [j] = channels [k].audio_gain [j];
            }
        }

        if (test_flags & TEST_FLAG_FLOAT_DATA) {
            if (bits <= 25)
                truncate_float_samples (destin, ENCODE_SAMPLES * num_chans, bits);
            else if (bits != 32) {
                printf ("invalid bits configuration\n");
                exit (-1);
            }
        }
        else if (!(test_flags & TEST_FLAG_STORE_FLOAT_AS_INT32)) {
            if (bits < 32)
                float_to_integer_samples (destin, ENCODE_SAMPLES * num_chans, bits);
            else if (bits == 32)
                float_to_32bit_integer_samples (destin, ENCODE_SAMPLES * num_chans);
            else {
                printf ("invalid bits configuration\n");
                exit (-1);
            }
        }

        if (!WavpackStreamPackSamples (out_wpc, (int32_t *) destin, ENCODE_SAMPLES))
            printf ("...PackSamples() returned FALSE\n");

        store_samples (destin, (int32_t *) destin, 0, wpconfig.bytes_per_sample, ENCODE_SAMPLES * num_chans);
        MD5_Update (&md5_context, (unsigned char *) destin, wpconfig.bytes_per_sample * ENCODE_SAMPLES * num_chans);

        sequencing_angle += 2.0 * M_PI / SAMPLE_RATE / speed * ENCODE_SAMPLES;
        if (sequencing_angle > M_PI) sequencing_angle -= M_PI * 2.0;

        if ((samples += ENCODE_SAMPLES) >= SAMPLE_RATE) {
            samples -= SAMPLE_RATE;
            ++seconds;

            if (!(wc & 1)) {
                if (width > 1.0) width *= 0.875;
                else if (width > 0.125) width -= 0.125;
                else {
                    width = 0.0;
                    wc++;
                }
            }
            else {
                if (width < 1.0) width += 0.125;
                else if (width < 200.0) width *= 1.125;
                else wc++;
            }
        }
    }

    WavpackStreamFlushSamples (out_wpc);
    MD5_Final (md5_encoded, &md5_context);

    if (wpconfig.flags & CONFIG_MD5_CHECKSUM) {
        WavpackStreamStoreMD5Sum (out_wpc, md5_encoded);
        WavpackStreamFlushSamples (out_wpc);
    }

    WavpackStreamCloseFile (out_wpc);

    free (channels);
    free (source);
    free (destin);

    if ((wpconfig_flags & CONFIG_CREATE_WVC) && !(test_flags & TEST_FLAG_IGNORE_WVC))
        total_encoded_bytes = wv_stream.bytes_written + wvc_stream.bytes_written;
    else
        total_encoded_bytes = wv_stream.bytes_written;

    total_encoded_samples = seconds * SAMPLE_RATE + samples;
    ratio = total_encoded_bytes / ((float) total_encoded_samples * wpconfig.bytes_per_sample * num_chans);
    bps = total_encoded_bytes * 8 / ((float) total_encoded_samples * num_chans);

    flush_stream (&wv_stream);
    flush_stream (&wvc_stream);

    if (!(test_flags & TEST_FLAG_NO_DECODE)) {
        pthread_join (pthread, &term_value);

        if (term_value) {
            printf ("decode_thread() returned error %d\n", (int) (long) term_value);
            return 1;
        }
    }

    free_stream (&wv_stream);
    free_stream (&wvc_stream);

    if (!(test_flags & TEST_FLAG_NO_DECODE)) {
        for (i = 0; i < 16; ++i) {
            sprintf (md5_string1 + (i * 2), "%02x", md5_encoded [i]);
            sprintf (md5_string2 + (i * 2), "%02x", wv_decoder.md5_decoded [i]);
        }

        if (wv_decoder.num_errors || wv_decoder.sample_count != total_encoded_samples ||
            (lossless && memcmp (md5_encoded, wv_decoder.md5_decoded, sizeof (md5_encoded)))) {
                printf ("\n---------------------------------------------\n");
                printf ("enc/dec sample count: %u / %u\n", total_encoded_samples, wv_decoder.sample_count);
                printf ("encoded md5: %s\n", md5_string1);
                printf ("decoded md5: %s\n", md5_string2);
                printf ("reported decode errors: %d\n", wv_decoder.num_errors);
                printf ("---------------------------------------------\n");
                return fuzz_period ? 0 : wv_decoder.num_errors + 1;
        }
    }

    printf ("pass (%8s, %.2f%%, %.2f bps, %s)\n", mode_string, 100.0 - ratio * 100.0, bps, md5_string2);

    return 0;
}

// Thread / function that opens a virtual WavPack file, decodes it and calculates the MD5 hash of the
// decoded audio data.

#define DECODE_SAMPLES 1000

static void *decode_thread (void *threadid)
{
    WavpackDecoder *wd = (WavpackDecoder *) threadid;
    char error [80];
    WavpackContext *wpc;
    int32_t *decoded_samples, num_chans, bps;
    MD5_CTX md5_context;

    while (1) {
        wpc = WavpackStreamOpenFileInputEx (&freader, wd->wv_stream, wd->wvc_stream, error, 0, 0);

        if (wpc)
            break;

        wd->num_errors++;

        if (wd->wv_stream->done || (wd->wvc_stream && wd->wvc_stream->done))
            break;
    }

    if (!wpc)
        pthread_exit (NULL);

    MD5_Init (&md5_context);
    num_chans = WavpackStreamGetNumChannels (wpc);
    bps = WavpackStreamGetBytesPerSample (wpc);

    decoded_samples = malloc (sizeof (int32_t) * DECODE_SAMPLES * num_chans);

    if (!decoded_samples) {
        printf ("decode_thread(): can't allocate memory!\n");
        exit (-1);
    }

    while (1) {
        int samples = WavpackStreamUnpackSamples (wpc, decoded_samples, DECODE_SAMPLES);

        if (samples) {
            store_samples (decoded_samples, decoded_samples, 0, bps, samples * num_chans);
            MD5_Update (&md5_context, (unsigned char *) decoded_samples, bps * samples * num_chans);
            wd->sample_count += samples;
        }
        else if (wd->wv_stream->done || (wd->wvc_stream && wd->wvc_stream->done))
            break;
        else
            wd->num_errors++;
    }

    MD5_Final (wd->md5_decoded, &md5_context);
    wd->num_errors = WavpackStreamGetNumErrors (wpc);
    free (decoded_samples);
    WavpackStreamCloseFile (wpc);
    pthread_exit (NULL);
    return NULL;
}

// Given a desired average period of corruptions and the length of the input data,
// calculate the probability that the specified number of hits will occur.

static double hit_probability (int period, int length, int num_hits)
{
    double probability = pow ((period - 1.0) / period, length - num_hits);
    int hits;

    for (hits = 0; hits < num_hits; ++hits)
        probability *= (double)(length - hits) / (period * (hits + 1));

    return probability;
}

// Fuzzing code. The fuzz "period" is the average distance in bytes between corrupted samples
// (but of course they are randomly distributed). For each byte selected for corruption, we
// randomly choose between 1 and 8 bits to toggle. We don't check for duplicate bits or bytes.
// So that the underlying file is identical regardless of fuzzing, we load and restore the
// random number generator seed value.

static void fuzz_buffer (void *data, int32_t length, int fuzz_period)
{
    uint64_t saved_seed = frandom_get_seed();
    double fuzz_factor = frandom (), probability_accum = 0.0;
    unsigned char *data_ptr = data;
    int num_hits = 0;

    while (1) {
        if ((probability_accum += hit_probability (fuzz_period, length, num_hits)) < fuzz_factor)
            num_hits++;
        else
            break;

        if (num_hits == (length + 1) / 2)   // should not get here, but let's not hang...
            break;
    }

    while (num_hits--) {
        int index = floor (frandom () * length);
        int delta_bits = ceil (frandom () * 8);
        unsigned char initial_value;

        if (index == length)
            index--;

        initial_value = data_ptr [index];

        while (delta_bits-- > 0 || data_ptr [index] == initial_value)
            data_ptr [index] ^= 1 << (int)(floor (frandom () * 8));
    }

    frandom_set_seed (saved_seed);
}

// This code implements a simple virtual "file" so that we can have a WavPack encoding process and
// a WavPack decoding process running at the same time (using Pthreads).

static int write_block (void *id, void *data, int32_t length)
{
    StreamingFile *ws = (StreamingFile *) id;
    unsigned char *data_ptr = data;

    if (!ws || !data || !length)
        return 0;

    if (ws->fuzz_period)
        fuzz_buffer (data, length, ws->fuzz_period);

    if (!ws->first_block_size)
        ws->first_block_size = length;

    ws->bytes_written += length;

    if (ws->file && !ws->error) {
        if (!fwrite (data, 1, length, ws->file)) {
            ws->error = 1;
            fclose (ws->file);
            ws->file = NULL;
        }
    }

    if (!ws->buffer_size)       // if no buffer, just swallow data silently
        return 1;

    pthread_mutex_lock (&ws->mutex);

    while (length) {
        int32_t bytes_available = ws->buffer_tail - ws->buffer_head - 1;
        int32_t bytes_to_copy = length;

        if (bytes_available < 0)
            bytes_available += ws->buffer_size;

        if (bytes_available < bytes_to_copy)
            bytes_to_copy = bytes_available;

        if (ws->buffer_head + bytes_to_copy > ws->buffer_base + ws->buffer_size)
            bytes_to_copy = ws->buffer_base + ws->buffer_size - ws->buffer_head;

        if (!bytes_to_copy) {
            ws->full_waits++;
            pthread_cond_wait (&ws->cond_read, &ws->mutex);
            continue;
        }

        memcpy ((void *) ws->buffer_head, data_ptr, bytes_to_copy);

        if ((ws->buffer_head += bytes_to_copy) == ws->buffer_base + ws->buffer_size)
            ws->buffer_head = ws->buffer_base;

        data_ptr += bytes_to_copy;
        length -= bytes_to_copy;
    }

    pthread_cond_signal (&ws->cond_write);
    pthread_mutex_unlock (&ws->mutex);

    return 1;
}

static int32_t read_bytes (void *id, void *data, int32_t bcount)
{
    StreamingFile *ws = (StreamingFile *) id;
    unsigned char *data_ptr = data;

    pthread_mutex_lock (&ws->mutex);

    while (bcount) {
        if (ws->push_back) {
            *data_ptr++ = ws->push_back;
            ws->push_back = 0;
            bcount--;
        }
        else if (ws->buffer_head != ws->buffer_tail) {
            int bytes_available = ws->buffer_head - ws->buffer_tail;
            int32_t bytes_to_copy = bcount;

            if (bytes_available < 0)
                bytes_available += ws->buffer_size;

            if (bytes_available < bytes_to_copy)
                bytes_to_copy = bytes_available;

            if (ws->buffer_tail + bytes_to_copy > ws->buffer_base + ws->buffer_size)
                bytes_to_copy = ws->buffer_base + ws->buffer_size - ws->buffer_tail;

            memcpy (data_ptr, (void *) ws->buffer_tail, bytes_to_copy);

            if ((ws->buffer_tail += bytes_to_copy) == ws->buffer_base + ws->buffer_size)
                ws->buffer_tail = ws->buffer_base;

            ws->bytes_read += bytes_to_copy;
            data_ptr += bytes_to_copy;
            bcount -= bytes_to_copy;
        }
        else if (ws->done)
            break;
        else {
            ws->empty_waits++;
            pthread_cond_wait (&ws->cond_write, &ws->mutex);
        }
    }

    pthread_cond_signal (&ws->cond_read);
    pthread_mutex_unlock (&ws->mutex);

    return data_ptr - (unsigned char *) data;
}

static uint32_t get_pos (void *id)
{
    return -1;
}

static int set_pos_abs (void *id, uint32_t pos)
{
    return 0;
}

static int set_pos_rel (void *id, int32_t delta, int mode)
{
    return -1;
}

static int push_back_byte (void *id, int c)
{
    StreamingFile *ws = (StreamingFile *) id;

    if (!ws->push_back)
        return ws->push_back = c;
    else
        return EOF;
}

static uint32_t get_length (void *id)
{
    return 0;
}

static int can_seek (void *id)
{
    return 0;
}

static WavpackReader freader = {
    read_bytes, get_pos, set_pos_abs, set_pos_rel, push_back_byte, get_length, can_seek,
};

static void initialize_stream (StreamingFile *ws, int buffer_size, int fuzz_period)
{
    if (buffer_size) {
        ws->buffer_base = malloc (ws->buffer_size = buffer_size);
        ws->buffer_head = ws->buffer_tail = ws->buffer_base;
        ws->fuzz_period = fuzz_period;
        pthread_cond_init (&ws->cond_write, NULL);
        pthread_cond_init (&ws->cond_read, NULL);
        pthread_mutex_init (&ws->mutex, NULL);
    }
}

static void flush_stream (StreamingFile *ws)
{
    if (ws->buffer_base) {
        pthread_mutex_lock (&ws->mutex);
        ws->done = 1;
        pthread_cond_signal (&ws->cond_write);
        pthread_mutex_unlock (&ws->mutex);
    }
}

static void free_stream (StreamingFile *ws)
{
    if (ws->file) {
        fclose (ws->file);
        ws->file = NULL;
    }

    if (ws->buffer_base) {
        free ((void *) ws->buffer_base);
        ws->buffer_base = NULL;
    }
}

// Helper utilities for generating the audio used for testing.

// Return a random value in the range: 0.0 <= n < 1.0

static uint64_t random_seed = 0x3141592653589793;

static double frandom (void)
{
    random_seed = ((random_seed << 4) - random_seed) ^ 1;
    random_seed = ((random_seed << 4) - random_seed) ^ 1;
    random_seed = ((random_seed << 4) - random_seed) ^ 1;
    return (random_seed >> 32) / 4294967296.0;
}

static uint64_t frandom_get_seed (void)
{
    return random_seed;
}

static void frandom_set_seed (uint64_t seed)
{
    random_seed = seed;
}

static void tone_generator_init (struct audio_generator *cxt, int sample_rate, int low_freq, int high_freq)
{
    struct tone_generator *tone_cxt = &cxt->u.tone_cxt;

    memset (cxt, 0, sizeof (*cxt));
    cxt->type = tone;

    tone_cxt->sample_rate = sample_rate;
    tone_cxt->high_frequency = high_freq;
    tone_cxt->low_frequency = low_freq;
    tone_cxt->samples_per_update = sample_rate / low_freq * 4;
}

static void tone_generator_run (struct tone_generator *cxt, float *samples, int num_samples)
{
    float target_frequency, target_velocity;

    while (num_samples--) {
        if (!cxt->samples_left) {
            cxt->samples_left = cxt->samples_per_update;

            target_frequency = cxt->low_frequency * pow (cxt->high_frequency / cxt->low_frequency, frandom ());
            target_velocity = (M_PI * 2.0) / ((float) cxt->sample_rate / target_frequency);
            cxt->acceleration = (target_velocity - cxt->velocity) / cxt->samples_left;
        }

        *samples++ = sin (cxt->angle += cxt->velocity += cxt->acceleration);
        if (cxt->angle > M_PI) cxt->angle -= M_PI * 2.0;
        cxt->samples_left--;
    }
}

static void noise_generator_init (struct audio_generator *cxt, float factor)
{
    struct noise_generator *noise_cxt = &cxt->u.noise_cxt;

    memset (cxt, 0, sizeof (*cxt));
    cxt->type = noise;

    noise_cxt->scalar = factor * factor * factor * sqrt (factor) / (2.0 + factor * factor);
    noise_cxt->factor = factor;
}

static void noise_generator_run (struct noise_generator *cxt, float *samples, int num_samples)
{
    while (num_samples--) {
        float source = (frandom () - 0.5) * cxt->scalar;
        cxt->sum1 += (source - cxt->sum1) / cxt->factor;
        cxt->sum2 += (cxt->sum1 - cxt->sum2) / cxt->factor;
        *samples++ = cxt->sum2 - cxt->sum2p;
        cxt->sum2p = cxt->sum2;
    }
}

static void audio_generator_run (struct audio_generator *cxt, float *samples, int num_samples)
{
    switch (cxt->type) {
        case noise:
            noise_generator_run (&cxt->u.noise_cxt, samples, num_samples);
            break;

        case tone:
            tone_generator_run (&cxt->u.tone_cxt, samples, num_samples);
            break;

        default:
            printf ("bad audio generator type!\n");
            exit (-1);
    }
}

static void mix_samples_with_gain (float *destin, float *source, int num_samples, int num_chans, float initial_gain, float final_gain)
{
    float delta_gain = (final_gain - initial_gain) / num_samples;
    float gain = initial_gain - delta_gain;

    while (num_samples--) {
        *destin += *source++ * (gain += delta_gain);
        destin += num_chans;
    }
}

static void truncate_float_samples (float *samples, int num_samples, int bits)
{
    int isample, imin = -(1 << (bits - 1)), imax = (1 << (bits - 1)) - 1;
    float scalar = (float) (1 << (bits - 1));

    while (num_samples--) {
        if (*samples >= 1.0)
            isample = imax;
        else if (*samples <= -1.0)
            isample = imin;
        else
            isample = floor (*samples * scalar);

        *samples++ = isample / scalar;
    } 
}

static void float_to_integer_samples (float *samples, int num_samples, int bits)
{
    int isample, imin = -(1 << (bits - 1)), imax = (1 << (bits - 1)) - 1;
    float scalar = (float) (1 << (bits - 1));
    int ishift = (8 - (bits & 0x7)) & 0x7;

    while (num_samples--) {
        if (*samples >= 1.0)
            isample = imax;
        else if (*samples <= -1.0)
            isample = imin;
        else
            isample = floor (*samples * scalar);

        *(int32_t *)samples = (uint32_t) isample << ishift;
        samples++;
    } 
}

static void float_to_32bit_integer_samples (float *samples, int num_samples)
{
    int isample, imin = 0x8000000, imax = 0x7fffffff;
    float scalar = 2147483648.0;

    while (num_samples--) {
        if (*samples >= 1.0)
            isample = imax;
        else if (*samples <= -1.0)
            isample = imin;
        else
            isample = floor (*samples * scalar);

        // if there are trailing zeros, fill them in with random data

        if (isample && !(isample & 1)) {
            int tzeros = 1;

            while (!((isample >>= 1) & 1))
                tzeros++;

            while (tzeros--)
                isample = ((unsigned int) isample << 1) + ((frandom() > 0.5) ? 1 : 0);
        }

        *(int32_t *)samples = isample;
        samples++;
    } 
}

// Code to store samples. Source is an array of int32_t data (which is what WavPack uses
// internally), but the destination can have from 1 to 4 bytes per sample. Also, the destination
// data is assumed to be little-endian and signed, except for byte data which is unsigned (these
// are WAV file defaults). The endian and signedness can be overridden with the qmode flags
// to support other formats.

static void *store_little_endian_unsigned_samples (void *dst, int32_t *src, int bps, int count);
static void *store_little_endian_signed_samples (void *dst, int32_t *src, int bps, int count);
static void *store_big_endian_unsigned_samples (void *dst, int32_t *src, int bps, int count);
static void *store_big_endian_signed_samples (void *dst, int32_t *src, int bps, int count);

static void *store_samples (void *dst, int32_t *src, int qmode, int bps, int count)
{
    if (qmode & QMODE_BIG_ENDIAN) {
        if ((qmode & QMODE_UNSIGNED_WORDS) || (bps == 1 && !(qmode & QMODE_SIGNED_BYTES)))
            return store_big_endian_unsigned_samples (dst, src, bps, count);
        else
            return store_big_endian_signed_samples (dst, src, bps, count);
    }
    else if ((qmode & QMODE_UNSIGNED_WORDS) || (bps == 1 && !(qmode & (QMODE_SIGNED_BYTES | QMODE_DSD_AUDIO))))
        return store_little_endian_unsigned_samples (dst, src, bps, count);
    else
        return store_little_endian_signed_samples (dst, src, bps, count);
}

static void *store_little_endian_unsigned_samples (void *dst, int32_t *src, int bps, int count)
{
    unsigned char *dptr = dst;
    int32_t temp;

    switch (bps) {

        case 1:
            while (count--)
                *dptr++ = *src++ + 0x80;

            break;

        case 2:
            while (count--) {
                *dptr++ = (unsigned char) (temp = *src++ + 0x8000);
                *dptr++ = (unsigned char) (temp >> 8);
            }

            break;

        case 3:
            while (count--) {
                *dptr++ = (unsigned char) (temp = *src++ + 0x800000);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) (temp >> 16);
            }

            break;

        case 4:
            while (count--) {
                *dptr++ = (unsigned char) (temp = *src++ + 0x80000000);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) (temp >> 16);
                *dptr++ = (unsigned char) (temp >> 24);
            }

            break;
    }

    return dptr;
}

static void *store_little_endian_signed_samples (void *dst, int32_t *src, int bps, int count)
{
    unsigned char *dptr = dst;
    int32_t temp;

    switch (bps) {

        case 1:
            while (count--)
                *dptr++ = *src++;

            break;

        case 2:
            while (count--) {
                *dptr++ = (unsigned char) (temp = *src++);
                *dptr++ = (unsigned char) (temp >> 8);
            }

            break;

        case 3:
            while (count--) {
                *dptr++ = (unsigned char) (temp = *src++);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) (temp >> 16);
            }

            break;

        case 4:
            while (count--) {
                *dptr++ = (unsigned char) (temp = *src++);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) (temp >> 16);
                *dptr++ = (unsigned char) (temp >> 24);
            }

            break;
    }

    return dptr;
}

static void *store_big_endian_unsigned_samples (void *dst, int32_t *src, int bps, int count)
{
    unsigned char *dptr = dst;
    int32_t temp;

    switch (bps) {

        case 1:
            while (count--)
                *dptr++ = *src++ + 0x80;

            break;

        case 2:
            while (count--) {
                *dptr++ = (unsigned char) ((temp = *src++ + 0x8000) >> 8);
                *dptr++ = (unsigned char) temp;
            }

            break;

        case 3:
            while (count--) {
                *dptr++ = (unsigned char) ((temp = *src++ + 0x800000) >> 16);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) temp;
            }

            break;

        case 4:
            while (count--) {
                *dptr++ = (unsigned char) ((temp = *src++ + 0x80000000) >> 24);
                *dptr++ = (unsigned char) (temp >> 16);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) temp;
            }

            break;
    }

    return dptr;
}

static void *store_big_endian_signed_samples (void *dst, int32_t *src, int bps, int count)
{
    unsigned char *dptr = dst;
    int32_t temp;

    switch (bps) {

        case 1:
            while (count--)
                *dptr++ = *src++;

            break;

        case 2:
            while (count--) {
                *dptr++ = (unsigned char) ((temp = *src++) >> 8);
                *dptr++ = (unsigned char) temp;
            }

            break;

        case 3:
            while (count--) {
                *dptr++ = (unsigned char) ((temp = *src++) >> 16);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) temp;
            }

            break;

        case 4:
            while (count--) {
                *dptr++ = (unsigned char) ((temp = *src++) >> 24);
                *dptr++ = (unsigned char) (temp >> 16);
                *dptr++ = (unsigned char) (temp >> 8);
                *dptr++ = (unsigned char) temp;
            }

            break;
    }

    return dptr;
}
