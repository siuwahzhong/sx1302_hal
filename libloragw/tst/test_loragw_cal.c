/*
 / _____)             _              | |
( (____  _____ ____ _| |_ _____  ____| |__
 \____ \| ___ |    (_   _) ___ |/ ___)  _ \
 _____) ) ____| | | || |_| ____( (___| | | |
(______/|_____)_|_|_| \__)_____)\____)_| |_|
  (C)2018 Semtech

Description:
    TBD

License: Revised BSD License, see LICENSE.TXT file include in the project
*/


/* -------------------------------------------------------------------------- */
/* --- DEPENDANCIES --------------------------------------------------------- */

/* fix an issue between POSIX and C99 */
#if __STDC_VERSION__ >= 199901L
    #define _XOPEN_SOURCE 600
#else
    #define _XOPEN_SOURCE 500
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <signal.h>     /* sigaction */
#include <getopt.h>     /* getopt_long */

#include "loragw_hal.h"
#include "loragw_reg.h"
#include "loragw_sx1302.h"
#include "loragw_sx125x.h"
#include "loragw_aux.h"
#include "loragw_cal.h"

/* -------------------------------------------------------------------------- */
/* --- PRIVATE MACROS ------------------------------------------------------- */

#define RAND_RANGE(min, max)        (rand() % (max + 1 - min) + min)

#define DEBUG_MSG(str)                fprintf(stderr, str)
#define DEBUG_PRINTF(fmt, args...)    fprintf(stderr,"%s:%d: "fmt, __FUNCTION__, __LINE__, args)

/* -------------------------------------------------------------------------- */
/* --- PRIVATE CONSTANTS ---------------------------------------------------- */

#define DEFAULT_CLK_SRC     0
#define DEFAULT_FREQ_HZ     868500000U

#define CAL_TX_TONE_FREQ_HZ     250000

/* -------------------------------------------------------------------------- */
/* --- PRIVATE TYPES -------------------------------------------------------- */
struct cal_tx_log {
    int32_t mean;
    int32_t i_offset;
    int32_t q_offset;
};

/* -------------------------------------------------------------------------- */
/* --- PRIVATE VARIABLES ---------------------------------------------------- */

FILE * fp;

static uint8_t nb_gains = 1;
static uint8_t dac_gain[TX_GAIN_LUT_SIZE_MAX] = { 2 };
static uint8_t mix_gain[TX_GAIN_LUT_SIZE_MAX] = { 14 };
static uint32_t rf_rx_freq[LGW_RF_CHAIN_NB] = {865500000, 865500000};
static enum lgw_radio_type_e rf_radio_type[LGW_RF_CHAIN_NB] = {LGW_RADIO_TYPE_SX1257, LGW_RADIO_TYPE_SX1257};

/* Signal handling variables */
static int exit_sig = 0; /* 1 -> application terminates cleanly (shut down hardware, close open files, etc) */
static int quit_sig = 0; /* 1 -> application terminates without shutting down the hardware */

#include "src/text_cal_sx1257_26_Oct_5.var"

/* -------------------------------------------------------------------------- */
/* --- PRIVATE FUNCTIONS ---------------------------------------------------- */

/* describe command line options */
void usage(void) {
    //printf("Library version information: %s\n", lgw_version_info());
    printf("Available options:\n");
    printf(" -h print this help\n");
    printf(" -k <uint> Concentrator clock source (Radio A or Radio B) [0..1]\n");
    printf(" -c <uint> RF chain to be used for TX (Radio A or Radio B) [0..1]\n");
    printf(" -r <uint> Radio type (1255, 1257, 1250)\n");
    printf(" -f <float> Radio TX frequency in MHz\n");
    printf( "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~\n" );
    printf(" --pa   <uint> PA gain [0..3]\n");
    printf(" --dig  <uint> sx1302 digital gain [0..3]\n");
    printf(" --dac  <uint> sx1257 DAC gain [0..3]\n");
    printf(" --mix  <uint> sx1257 MIX gain [0..15]\n");
}

/* handle signals */
static void sig_handler(int sigio)
{
    if (sigio == SIGQUIT) {
        quit_sig = 1;
    }
    else if((sigio == SIGINT) || (sigio == SIGTERM)) {
        exit_sig = 1;
    }
}

int setup_tx_dc_offset(uint8_t rf_chain, uint32_t freq_hz, uint8_t dac_gain, uint8_t mix_gain, uint8_t radio_type) {
    uint32_t rx_freq_hz, tx_freq_hz;
    uint32_t rx_freq_int, rx_freq_frac;
    uint32_t tx_freq_int, tx_freq_frac;
    uint8_t rx_pll_locked, tx_pll_locked;

    /* Set PLL frequencies */
    rx_freq_hz = freq_hz - CAL_TX_TONE_FREQ_HZ;
    tx_freq_hz = freq_hz;
    switch (radio_type) {
        case LGW_RADIO_TYPE_SX1255:
            rx_freq_int = rx_freq_hz / (SX125x_32MHz_FRAC << 7); /* integer part, gives the MSB */
            rx_freq_frac = ((rx_freq_hz % (SX125x_32MHz_FRAC << 7)) << 9) / SX125x_32MHz_FRAC; /* fractional part, gives middle part and LSB */
            tx_freq_int = tx_freq_hz / (SX125x_32MHz_FRAC << 7); /* integer part, gives the MSB */
            tx_freq_frac = ((tx_freq_hz % (SX125x_32MHz_FRAC << 7)) << 9) / SX125x_32MHz_FRAC; /* fractional part, gives middle part and LSB */
            break;
        case LGW_RADIO_TYPE_SX1257:
            rx_freq_int = rx_freq_hz / (SX125x_32MHz_FRAC << 8); /* integer part, gives the MSB */
            rx_freq_frac = ((rx_freq_hz % (SX125x_32MHz_FRAC << 8)) << 8) / SX125x_32MHz_FRAC; /* fractional part, gives middle part and LSB */
            tx_freq_int = tx_freq_hz / (SX125x_32MHz_FRAC << 8); /* integer part, gives the MSB */
            tx_freq_frac = ((tx_freq_hz % (SX125x_32MHz_FRAC << 8)) << 8) / SX125x_32MHz_FRAC; /* fractional part, gives middle part and LSB */
            break;
        default:
            DEBUG_PRINTF("ERROR: UNEXPECTED VALUE %d FOR RADIO TYPE\n", radio_type);
            return LGW_HAL_ERROR;
    }
    lgw_sx125x_reg_w(SX125x_REG_FRF_RX_MSB, 0xFF & rx_freq_int, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_FRF_RX_MID, 0xFF & (rx_freq_frac >> 8), rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_FRF_RX_LSB, 0xFF & rx_freq_frac, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_FRF_TX_MSB, 0xFF & tx_freq_int, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_FRF_TX_MID, 0xFF & (tx_freq_frac >> 8), rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_FRF_TX_LSB, 0xFF & tx_freq_frac, rf_chain);

    /* Radio settings for calibration */
    //lgw_sx125x_reg_w(SX125x_RX_ANA_GAIN__LNA_ZIN, 1, rf_chain); /* Default: 1 */
    //lgw_sx125x_reg_w(SX125x_RX_ANA_GAIN__BB_GAIN, 15, rf_chain); /* Default: 15 */
    //lgw_sx125x_reg_w(SX125x_RX_ANA_GAIN__LNA_GAIN, 1, rf_chain); /* Default: 1 */
    lgw_sx125x_reg_w(SX125x_REG_RX_BW__BB_BW, 0, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_RX_BW__ADC_TRIM, 6, rf_chain);
    //lgw_sx125x_reg_w(SX125x_RX_BW__ADC_BW, 7, rf_chain);  /* Default: 7 */
    lgw_sx125x_reg_w(SX125x_REG_RX_PLL_BW__PLL_BW, 0, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_TX_BW__PLL_BW, 0, rf_chain);
    //lgw_sx125x_reg_w(SX125x_TX_BW__ANA_BW, 0, rf_chain); /* Default: 0 */
    lgw_sx125x_reg_w(SX125x_REG_TX_DAC_BW, 5, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_CLK_SELECT__DAC_CLK_SELECT, 1, rf_chain); /* Use external clock from SX1301 */
    lgw_sx125x_reg_w(SX125x_REG_TX_GAIN__DAC_GAIN, dac_gain, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_TX_GAIN__MIX_GAIN, mix_gain, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_CLK_SELECT__RF_LOOPBACK_EN, 1, rf_chain);
    lgw_sx125x_reg_w(SX125x_REG_MODE, 15, rf_chain);
    wait_ms(1);
    lgw_sx125x_reg_r(SX125x_REG_MODE_STATUS__RX_PLL_LOCKED, &rx_pll_locked, rf_chain);
    lgw_sx125x_reg_r(SX125x_REG_MODE_STATUS__TX_PLL_LOCKED, &tx_pll_locked, rf_chain);
    if ((rx_pll_locked == 0) || (tx_pll_locked == 0)) {
        DEBUG_MSG("ERROR: PLL failed to lock\n");
        return LGW_HAL_ERROR;
    }

    return 0;
}

int cal_tx_dc_offset(uint8_t rf_chain, uint32_t freq_hz, uint8_t dac_gain, uint8_t mix_gain, uint8_t radio_type, int32_t f_offset, int32_t i_offset, int32_t q_offset, bool full_log, bool use_agc, uint8_t amp, uint8_t phi) {
    int i;
    uint16_t reg;
    int32_t val_min, val_max;
    int32_t acc;
    int32_t val_mean;
    float val_std;
    float acc2 = 0 ;
    int loop_len = 3;
    uint8_t dec_gain = 6;
    float res_sig[loop_len];

//    DEBUG_MSG("\n");
//    DEBUG_PRINTF("rf_chain:%u, freq_hz:%u, dac_gain:%u, mix_gain:%u, radio_type:%d\n", rf_chain, freq_hz, dac_gain, mix_gain, radio_type);

    if (setup_tx_dc_offset(rf_chain, freq_hz, dac_gain, mix_gain, radio_type) != LGW_HAL_SUCCESS) {
        return LGW_HAL_ERROR;
    }

    /* Trig calibration */

    /* Select radio to be connected to the Signal Analyzer (warning: RadioA:1, RadioB:0) */
    lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_RADIO_SEL, (rf_chain == 0) ? 1 : 0);

    reg = REG_SELECT(rf_chain,  SX1302_REG_TX_TOP_A_TX_RFFE_IF_CTRL_TX_MODE,
                                SX1302_REG_TX_TOP_B_TX_RFFE_IF_CTRL_TX_MODE);
    lgw_reg_w(reg, 0);

    reg = REG_SELECT(rf_chain,  SX1302_REG_TX_TOP_A_TX_TRIG_TX_TRIG_IMMEDIATE,
                                SX1302_REG_TX_TOP_B_TX_TRIG_TX_TRIG_IMMEDIATE);
    lgw_reg_w(reg, 1);
    lgw_reg_w(reg, 0);

    lgw_reg_w(SX1302_REG_RADIO_FE_CTRL0_RADIO_A_DC_NOTCH_EN, 1);

    /* Measuring */
    if (use_agc == true) {
        uint8_t val_sig, val_sig2;

        /* Set calibration parameters */
        sx1302_agc_mailbox_write(2, rf_chain + 4); /* Sig ana test radio A/B */
        sx1302_agc_mailbox_write(1, f_offset/*(CAL_TX_TONE_FREQ_HZ + f_offset) * 64e-6*/); /* Set frequency */
        sx1302_agc_mailbox_write(0, 0); /* correlation duration: 0:1, 1:2, 2:4, 3:8 ms) */

        /*  */
        sx1302_agc_mailbox_write(3, 0x00);
        sx1302_agc_mailbox_write(3, 0x01);
        sx1302_agc_wait_status(0x01);

        sx1302_agc_mailbox_write(2, amp); /* amp */
        sx1302_agc_mailbox_write(1, phi); /* phi */

        sx1302_agc_mailbox_write(3, 0x02);
        sx1302_agc_wait_status(0x02);

        sx1302_agc_mailbox_write(2, i_offset); /* i offset init */
        sx1302_agc_mailbox_write(1, q_offset); /* q offset init */

        sx1302_agc_mailbox_write(3, 0x03);
        sx1302_agc_wait_status(0x03);

        sx1302_agc_mailbox_write(2, dec_gain); /* dec_gain */

        sx1302_agc_mailbox_write(3, 0x04);

        reg = REG_SELECT(rf_chain,  SX1302_REG_TX_TOP_A_TX_TRIG_TX_TRIG_IMMEDIATE,
                                    SX1302_REG_TX_TOP_B_TX_TRIG_TX_TRIG_IMMEDIATE);
        lgw_reg_w(reg, 0);

        for (i = 0; i < loop_len; i++) {
            sx1302_agc_wait_status(0x06);
            sx1302_agc_mailbox_write(3, 0x06);

            sx1302_agc_wait_status(0x07);
            sx1302_agc_mailbox_read(0, &val_sig);
            sx1302_agc_mailbox_read(1, &val_sig2);
            res_sig[i] = val_sig2 * 256 + val_sig;

            if (i == (loop_len - 1)) {
                sx1302_agc_mailbox_write(3, 0x07); /* unlock */
            } else {
                sx1302_agc_mailbox_write(3, 0x00); /* unlock */
            }
        }
    } else {
        int32_t val;
        int32_t corr_i, corr_q;
        float abs_iq;

        lgw_reg_w(SX1302_REG_TX_TOP_A_TX_RFFE_IF_Q_OFFSET_Q_OFFSET, (int8_t)q_offset);
        lgw_reg_w(SX1302_REG_TX_TOP_A_TX_RFFE_IF_I_OFFSET_I_OFFSET, (int8_t)i_offset);

        lgw_reg_w(SX1302_REG_RADIO_FE_CTRL0_RADIO_A_DC_NOTCH_EN, 1);
        lgw_reg_w(SX1302_REG_RADIO_FE_CTRL0_RADIO_A_FORCE_HOST_FILTER_GAIN, 0x01);

        lgw_reg_w(SX1302_REG_RADIO_FE_CTRL0_RADIO_A_HOST_FILTER_GAIN, dec_gain);
        //lgw_reg_r(SX1302_REG_RADIO_FE_DEC_FILTER_RD_RADIO_A_DEC_FILTER_GAIN, &val);
        //dec_gain = (uint8_t)val;

        lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_FREQ_FREQ, f_offset);

#if 1 /* FPGA 10.2 */
        lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_DURATION, 3);
        lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_EN, 1);

        for (i = 0; i < loop_len; i++) {
            lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_START, 0);
            lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_START, 1);

            do {
                lgw_reg_r(SX1302_REG_RADIO_FE_SIG_ANA_CFG_VALID, &val);
                wait_ms(1);
            } while (val == 0);

            lgw_reg_r(SX1302_REG_RADIO_FE_SIG_ANA_CORR_I_OUT_CORR_I_OUT, &corr_i);
            lgw_reg_r(SX1302_REG_RADIO_FE_SIG_ANA_CORR_Q_OUT_CORR_Q_OUT, &corr_q);
            abs_iq = (corr_q << 8) | corr_i;

            res_sig[i] = abs_iq;
        }
#else /* FPGA 10.1 */
        lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_EN, 1);

        for (i = 0; i < loop_len; i++) {
            lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_EN, 0);
            lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_EN, 1);
            lgw_reg_w(SX1302_REG_RADIO_FE_SIG_ANA_CFG_EN, 0);

            do {
                lgw_reg_r(SX1302_REG_RADIO_FE_SIG_ANA_CFG_VALID, &val);
                wait_ms(1);
            } while (val == 1); /* busy */

            lgw_reg_r(SX1302_REG_RADIO_FE_SIG_ANA_CORR_I_OUT_CORR_I_OUT, &corr_i);
            lgw_reg_r(SX1302_REG_RADIO_FE_SIG_ANA_CORR_Q_OUT_CORR_Q_OUT, &corr_q);
            abs_iq = (corr_q << 8) | corr_i;

            res_sig[i] = abs_iq;
        }
#endif
    }

    if (full_log == true) {
        printf("i_offset:%d q_offset:%d f_offset:%d dac_gain:%d mix_gain:%d dec_gain:%d amp:%u phi:%u => ", i_offset, q_offset, f_offset, dac_gain, mix_gain, dec_gain, amp, phi);
    } else {
        fprintf(fp, "%d %d ", i_offset, q_offset);
        //printf("%d %d ", i_offset, q_offset);
        //printf("%d %d ", amp, phi);
    }

    /* Analyze result */
    val_min = res_sig[0];
    val_max = res_sig[0];
    acc = 0;
    for (i = 0; i < loop_len; i++) {
        if (res_sig[i] > val_max) {
            val_max = res_sig[i];
        }
        if (res_sig[i] < val_min) {
            val_min = res_sig[i];
        }
        acc += res_sig[i];
    }
    val_mean = acc / loop_len;

    for (i = 0; i < loop_len; i++) {
        acc2 += pow((res_sig[i]-val_mean),2);
    }
    val_std = sqrt(acc2/loop_len);

    if (full_log == true) {
        printf(" min:%u max:%u mean:%u std:%f\n", val_min, val_max, val_mean, val_std);
    } else {
        fprintf(fp, "%u %u %u %f\n", val_min, val_max, val_mean, val_std);
    }

    return LGW_HAL_SUCCESS;
}

/* -------------------------------------------------------------------------- */
/* --- MAIN FUNCTION -------------------------------------------------------- */

int test_freq_scan(uint8_t rf_chain, bool full_log) {
    int f;

    printf("-------------------------------------\n");
    for (f = 0; f < 256; f++)
    {
        cal_tx_dc_offset(rf_chain, rf_rx_freq[rf_chain], dac_gain[0], mix_gain[0], rf_radio_type[rf_chain], f, 0, 0, full_log, true, 0, 0);

        if ((quit_sig == 1) || (exit_sig == 1)) {
            break;
        }
    }

    return 0;
}

int test_iq_offset(uint8_t rf_chain, uint8_t f_offset, bool full_log, bool use_agc) {
    int l, m, j;

    printf("-------------------------------------\n");
    for (j = 0; j < nb_gains; j++) {
        for (l = 0; l < 40; l+=1)
        {
            for (m = 0; m < 30; m+=1)
            {
                cal_tx_dc_offset(rf_chain, rf_rx_freq[rf_chain], dac_gain[j], mix_gain[j], rf_radio_type[rf_chain], f_offset, l, m, full_log, use_agc, 0, 0);
                if ((quit_sig == 1) || (exit_sig == 1)) {
                    return 0;
                }
            }
        }
    }

    return 0;
}

int test_amp_phi(uint8_t rf_chain, uint8_t f_offset, bool full_log, bool use_agc) {
    int amp, phi;

    printf("-------------------------------------\n");
    for (amp = 0; amp < 64; amp++)
    {
        for (phi = 0; phi < 64; phi++)
        {
            cal_tx_dc_offset(rf_chain, rf_rx_freq[rf_chain], dac_gain[0], mix_gain[0], rf_radio_type[rf_chain], f_offset, 0, 0, full_log, use_agc, amp, phi);
            if ((quit_sig == 1) || (exit_sig == 1)) {
                return 0;
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    int i, x;
    uint32_t ft = DEFAULT_FREQ_HZ;
    double arg_d = 0.0;
    unsigned int arg_u;
    uint8_t clocksource = 0;
    uint8_t rf_chain = 0;
    enum lgw_radio_type_e radio_type = LGW_RADIO_TYPE_NONE;

    struct lgw_conf_board_s boardconf;
    struct lgw_conf_rxrf_s rfconf;
    struct lgw_tx_gain_lut_s txlut; /* TX gain table */

    static struct sigaction sigact; /* SIGQUIT&SIGINT&SIGTERM signal handling */

    /* Initialize TX gain LUT */
    txlut.size = 0;
    memset(txlut.lut, 0, sizeof txlut.lut);

    /* Parameter parsing */
    int option_index = 0;
    static struct option long_options[] = {
        {"pa", 1, 0, 0},
        {"dac", 1, 0, 0},
        {"dig", 1, 0, 0},
        {"mix", 1, 0, 0},
        {0, 0, 0, 0}
    };

    /* parse command line options */
    while ((i = getopt_long (argc, argv, "hf:k:r:c:", long_options, &option_index)) != -1) {
        switch (i) {
            case 'h':
                usage();
                return -1;
                break;
            case 'r': /* <uint> Radio type */
                i = sscanf(optarg, "%u", &arg_u);
                if ((i != 1) || ((arg_u != 1255) && (arg_u != 1257) && (arg_u != 1250))) {
                    printf("ERROR: argument parsing of -r argument. Use -h to print help\n");
                    return EXIT_FAILURE;
                } else {
                    switch (arg_u) {
                        case 1255:
                            radio_type = LGW_RADIO_TYPE_SX1255;
                            break;
                        case 1257:
                            radio_type = LGW_RADIO_TYPE_SX1257;
                            break;
                        default: /* 1250 */
                            radio_type = LGW_RADIO_TYPE_SX1250;
                            break;
                    }
                }
                break;
            case 'k': /* <uint> Clock Source */
                i = sscanf(optarg, "%u", &arg_u);
                if ((i != 1) || (arg_u > 1)) {
                    printf("ERROR: argument parsing of -k argument. Use -h to print help\n");
                    return EXIT_FAILURE;
                } else {
                    clocksource = (uint8_t)arg_u;
                }
                break;
            case 'c': /* <uint> RF chain */
                i = sscanf(optarg, "%u", &arg_u);
                if ((i != 1) || (arg_u > 1)) {
                    printf("ERROR: argument parsing of -c argument. Use -h to print help\n");
                    return EXIT_FAILURE;
                } else {
                    rf_chain = (uint8_t)arg_u;
                }
                break;
            case 'f': /* <float> Radio TX frequency in MHz */
                i = sscanf(optarg, "%lf", &arg_d);
                if (i != 1) {
                    printf("ERROR: argument parsing of -f argument. Use -h to print help\n");
                    return EXIT_FAILURE;
                } else {
                    ft = (uint32_t)((arg_d*1e6) + 0.5); /* .5 Hz offset to get rounding instead of truncating */
                }
                break;
            case 0:
                if (strcmp(long_options[option_index].name, "pa") == 0) {
                    i = sscanf(optarg, "%u", &arg_u);
                    if ((i != 1) || (arg_u > 3)) {
                        printf("ERROR: argument parsing of --pa argument. Use -h to print help\n");
                        return EXIT_FAILURE;
                    } else {
                        txlut.size = 1;
                        txlut.lut[0].pa_gain = (uint8_t)arg_u;
                    }
                } else if (strcmp(long_options[option_index].name, "dac") == 0) {
                    i = sscanf(optarg, "%u", &arg_u);
                    if ((i != 1) || (arg_u > 3)) {
                        printf("ERROR: argument parsing of --dac argument. Use -h to print help\n");
                        return EXIT_FAILURE;
                    } else {
                        txlut.size = 1;
                        txlut.lut[0].dac_gain = (uint8_t)arg_u;
                    }
                } else if (strcmp(long_options[option_index].name, "mix") == 0) {
                    i = sscanf(optarg, "%u", &arg_u);
                    if ((i != 1) || (arg_u > 15)) {
                        printf("ERROR: argument parsing of --mix argument. Use -h to print help\n");
                        return EXIT_FAILURE;
                    } else {
                        txlut.size = 1;
                        txlut.lut[0].mix_gain = (uint8_t)arg_u;
                    }
                } else if (strcmp(long_options[option_index].name, "dig") == 0) {
                    i = sscanf(optarg, "%u", &arg_u);
                    if ((i != 1) || (arg_u > 3)) {
                        printf("ERROR: argument parsing of --dig argument. Use -h to print help\n");
                        return EXIT_FAILURE;
                    } else {
                        txlut.size = 1;
                        txlut.lut[0].dig_gain = (uint8_t)arg_u;
                    }
                } else {
                    printf("ERROR: argument parsing options. Use -h to print help\n");
                    return EXIT_FAILURE;
                }
                break;
            default:
                printf("ERROR: argument parsing\n");
                usage();
                return -1;
        }
    }

    /* Configure signal handling */
    sigemptyset( &sigact.sa_mask );
    sigact.sa_flags = 0;
    sigact.sa_handler = sig_handler;
    sigaction( SIGQUIT, &sigact, NULL );
    sigaction( SIGINT, &sigact, NULL );
    sigaction( SIGTERM, &sigact, NULL );

    /* Board reset */
    system("./reset_lgw.sh start");

    /* Configure the gateway */
    memset( &boardconf, 0, sizeof boardconf);
    boardconf.lorawan_public = true;
    boardconf.clksrc = clocksource;
    if (lgw_board_setconf(boardconf) != LGW_HAL_SUCCESS) {
        printf("ERROR: failed to configure board\n");
        return EXIT_FAILURE;
    }

    memset( &rfconf, 0, sizeof rfconf);
    rfconf.enable = ((rf_chain == 0) ? true : false);
    rfconf.freq_hz = ft;
    rfconf.type = radio_type;
    rfconf.tx_enable = true;
    if (lgw_rxrf_setconf(0, rfconf) != LGW_HAL_SUCCESS) {
        printf("ERROR: failed to configure rxrf 0\n");
        return EXIT_FAILURE;
    }

    memset( &rfconf, 0, sizeof rfconf);
    rfconf.enable = ((rf_chain == 1) ? true : false);
    rfconf.freq_hz = ft;
    rfconf.type = radio_type;
    rfconf.tx_enable = true;
    if (lgw_rxrf_setconf(1, rfconf) != LGW_HAL_SUCCESS) {
        printf("ERROR: failed to configure rxrf 1\n");
        return EXIT_FAILURE;
    }

    if (txlut.size > 0) {
        if (lgw_txgain_setconf(&txlut) != LGW_HAL_SUCCESS) {
            printf("ERROR: failed to configure txgain lut\n");
            return EXIT_FAILURE;
        }
    }

    /* open log file for writing */
    fp = fopen ("log.txt", "w+");

    /* connect the gateway */
    x = lgw_connect();
    if (x != 0) {
        printf("ERROR: failed to connect the gateway\n");
        return EXIT_FAILURE;
    }

    sx1302_radio_reset(rf_chain, SX1302_RADIO_TYPE_SX125X);
    sx1302_radio_clock_select(clocksource, true);

    printf("Loading CAL fw for sx125x\n");
    if (sx1302_agc_load_firmware(cal_firmware_sx125x) != LGW_HAL_SUCCESS) {
        return LGW_HAL_ERROR;
    }

    printf("waiting for capture ram\n");
    wait_ms(1000);

    /* testing */
    test_freq_scan(rf_chain, true);
    //test_iq_offset(rf_chain, 16, true, true);
    //test_amp_phi(rf_chain, 240, true, true);

    /* disconnect the gateway */
    x = lgw_disconnect();
    if (x != 0) {
        printf("ERROR: failed to disconnect the gateway\n");
        return EXIT_FAILURE;
    }

    /* Close log file */
    fclose(fp);

    printf("=========== Test End ===========\n");

    return 0;
}

/* --- EOF ------------------------------------------------------------------ */