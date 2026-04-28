/*
 * ESP32-S3 I2S controller emulation
 *
 * Copyright (c) 2026 M5StickS3 Emulator contributors
 *
 * I2S peripheral for the ESP32-S3. Supports TX (playback) and RX (capture)
 * with DMA transfers via GDMA. Audio is routed to QEMU's audio subsystem,
 * which provides host audio I/O through SDL, CoreAudio, etc.
 *
 * The M5StickS3 uses I2S0 with an ES8311 codec (I2C-configured) and
 * AW8737 amplifier for audio output, plus a MEMS microphone for input.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "sysemu/sysemu.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "hw/registerfields.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/audio/esp32s3_i2s.h"
#include "qemu/error-report.h"
#include "audio/audio.h"

#define I2S_DEBUG   0
#define I2S_WARNING 0

/* Default audio settings when we can't determine from registers */
#define I2S_DEFAULT_SAMPLE_RATE    16000
#define I2S_DEFAULT_CHANNELS       2
#define I2S_DEFAULT_BITS           16


static void esp32s3_i2s_update_irq(ESP32S3I2sState *s)
{
    s->int_st = s->int_raw & s->int_ena;
    qemu_set_irq(s->irq, s->int_st ? 1 : 0);
}


/*
 * Compute sample rate from clock configuration registers.
 * The I2S clock is derived from: source_clk / (clkm_div_num + b/a)
 * Then BCLK = MCLK / bck_div_num
 * Sample rate = BCLK / (bits_per_channel * num_channels)
 *
 * For simplicity in emulation, we use common sample rates based on the
 * divider value, since ESP-IDF sets standard rates.
 */
static int esp32s3_i2s_compute_sample_rate(ESP32S3I2sState *s, bool is_tx)
{
    uint32_t clkm_conf = is_tx ? s->tx_clkm_conf : s->rx_clkm_conf;
    uint32_t conf1 = is_tx ? s->tx_conf1 : s->rx_conf1;

    uint32_t clkm_div = FIELD_EX32(clkm_conf, I2S_TX_CLKM_CONF, TX_CLKM_DIV_NUM);
    uint32_t bck_div = is_tx ?
        FIELD_EX32(conf1, I2S_TX_CONF1, TX_BCK_DIV_NUM) :
        FIELD_EX32(conf1, I2S_RX_CONF1, RX_BCK_DIV_NUM);
    uint32_t bits = is_tx ?
        FIELD_EX32(conf1, I2S_TX_CONF1, TX_BITS_MOD) :
        FIELD_EX32(conf1, I2S_RX_CONF1, RX_BITS_MOD);

    if (clkm_div == 0) clkm_div = 2;
    if (bck_div == 0) bck_div = 8;
    if (bits == 0) bits = 16;

    /*
     * ESP32-S3 I2S source clock is typically 160MHz (PLL_F160M) or XTAL (40MHz).
     * Standard configurations:
     *   16kHz: clkm_div=10, bck_div=8, bits=16, channels=2
     *   44.1kHz: clkm_div=4, bck_div=8, bits=16, channels=2 (approx)
     *   48kHz: clkm_div=3, bck_div=8, bits=16 (approx from 160MHz)
     *
     * Rather than computing the exact fractional divider, detect common
     * configurations and fall back to a calculation.
     */
    int source_clk_hz = 160000000; /* PLL_F160M default */
    int sample_rate = source_clk_hz / (clkm_div * bck_div * bits * 2);

    /* Clamp to reasonable range */
    if (sample_rate < 8000) sample_rate = 8000;
    if (sample_rate > 96000) sample_rate = 96000;

    return sample_rate;
}


static int esp32s3_i2s_get_bits_per_sample(ESP32S3I2sState *s, bool is_tx)
{
    uint32_t conf1 = is_tx ? s->tx_conf1 : s->rx_conf1;
    uint32_t bits = is_tx ?
        FIELD_EX32(conf1, I2S_TX_CONF1, TX_BITS_MOD) :
        FIELD_EX32(conf1, I2S_RX_CONF1, RX_BITS_MOD);

    if (bits == 0) return I2S_DEFAULT_BITS;
    /* bits_mod stores the actual bit width (8, 16, 24, 32) */
    return (int)bits;
}


static int esp32s3_i2s_get_channels(ESP32S3I2sState *s, bool is_tx)
{
    uint32_t conf = is_tx ? s->tx_conf : s->rx_conf;
    bool mono = is_tx ?
        FIELD_EX32(conf, I2S_TX_CONF, TX_MONO) :
        FIELD_EX32(conf, I2S_RX_CONF, RX_MONO);
    return mono ? 1 : 2;
}


static AudioFormat esp32s3_i2s_get_audio_format(int bits_per_sample)
{
    switch (bits_per_sample) {
    case 8:  return AUDIO_FORMAT_S8;
    case 16: return AUDIO_FORMAT_S16;
    case 32: return AUDIO_FORMAT_S32;
    default: return AUDIO_FORMAT_S16;
    }
}


/*
 * Audio output callback -- called by QEMU audio subsystem when it needs
 * more data for playback. We read from the GDMA channel.
 */
static void esp32s3_i2s_out_cb(void *opaque, int free_bytes)
{
    ESP32S3I2sState *s = ESP32S3_I2S(opaque);

    if (!s->tx_running || s->gdma == NULL) {
        return;
    }

    /* Read audio data from GDMA TX channel and write to audio subsystem */
    uint32_t gdma_chan;
    if (!esp_gdma_get_channel_periph(s->gdma, s->gdma_periph_id,
                                     ESP_GDMA_OUT_IDX, &gdma_chan)) {
        return;
    }

    while (free_bytes > 0) {
        int chunk_size = MIN(free_bytes, (int)sizeof(s->out_buf));
        if (!esp_gdma_read_channel(s->gdma, gdma_chan, s->out_buf, chunk_size)) {
            /* No more DMA data available -- signal TX done */
            s->int_raw |= R_I2S_INT_RAW_TX_DONE_MASK;
            esp32s3_i2s_update_irq(s);
            break;
        }
        size_t written = AUD_write(s->out_voice, s->out_buf, chunk_size);
        free_bytes -= written;
        if ((int)written < chunk_size) {
            break;
        }
    }
}


/*
 * Audio input callback -- called by QEMU audio subsystem when capture
 * data is available. We write it into the GDMA RX channel.
 */
static void esp32s3_i2s_in_cb(void *opaque, int avail_bytes)
{
    ESP32S3I2sState *s = ESP32S3_I2S(opaque);

    if (!s->rx_running || s->gdma == NULL) {
        return;
    }

    uint32_t gdma_chan;
    if (!esp_gdma_get_channel_periph(s->gdma, s->gdma_periph_id,
                                     ESP_GDMA_IN_IDX, &gdma_chan)) {
        return;
    }

    while (avail_bytes > 0) {
        int chunk_size = MIN(avail_bytes, (int)sizeof(s->in_buf));
        size_t read_bytes = AUD_read(s->in_voice, s->in_buf, chunk_size);
        if (read_bytes == 0) {
            break;
        }
        if (!esp_gdma_write_channel(s->gdma, gdma_chan, s->in_buf, read_bytes)) {
            /* DMA buffer full -- signal RX done */
            s->int_raw |= R_I2S_INT_RAW_RX_DONE_MASK;
            esp32s3_i2s_update_irq(s);
            break;
        }
        avail_bytes -= read_bytes;
    }
}


/*
 * Open or reconfigure the audio output voice.
 */
static void esp32s3_i2s_open_out(ESP32S3I2sState *s)
{
    struct audsettings as;
    as.freq = esp32s3_i2s_compute_sample_rate(s, true);
    as.nchannels = esp32s3_i2s_get_channels(s, true);
    as.fmt = esp32s3_i2s_get_audio_format(esp32s3_i2s_get_bits_per_sample(s, true));
    as.endianness = 0; /* little-endian */

    s->sample_rate = as.freq;
    s->channels = as.nchannels;
    s->bits_per_sample = esp32s3_i2s_get_bits_per_sample(s, true);

#if I2S_DEBUG
    info_report("[I2S] Opening output: %d Hz, %d ch, %d bits",
                as.freq, as.nchannels, s->bits_per_sample);
#endif

    s->out_voice = AUD_open_out(&s->card, s->out_voice, "esp32s3-i2s-out",
                                s, esp32s3_i2s_out_cb, &as);
}


/*
 * Open or reconfigure the audio input voice.
 */
static void esp32s3_i2s_open_in(ESP32S3I2sState *s)
{
    struct audsettings as;
    as.freq = esp32s3_i2s_compute_sample_rate(s, false);
    as.nchannels = esp32s3_i2s_get_channels(s, false);
    as.fmt = esp32s3_i2s_get_audio_format(esp32s3_i2s_get_bits_per_sample(s, false));
    as.endianness = 0; /* little-endian */

#if I2S_DEBUG
    info_report("[I2S] Opening input: %d Hz, %d ch, %d bits",
                as.freq, as.nchannels,
                esp32s3_i2s_get_bits_per_sample(s, false));
#endif

    s->in_voice = AUD_open_in(&s->card, s->in_voice, "esp32s3-i2s-in",
                              s, esp32s3_i2s_in_cb, &as);
}


/*
 * Start or stop TX based on register state.
 */
static void esp32s3_i2s_update_tx(ESP32S3I2sState *s)
{
    bool tx_start = FIELD_EX32(s->tx_conf, I2S_TX_CONF, TX_START);

    if (tx_start && !s->tx_running) {
        /* Starting TX */
        esp32s3_i2s_open_out(s);
        if (s->out_voice) {
            AUD_set_active_out(s->out_voice, 1);
            s->tx_running = true;
#if I2S_DEBUG
            info_report("[I2S] TX started");
#endif
        }
    } else if (!tx_start && s->tx_running) {
        /* Stopping TX */
        if (s->out_voice) {
            AUD_set_active_out(s->out_voice, 0);
        }
        s->tx_running = false;
#if I2S_DEBUG
        info_report("[I2S] TX stopped");
#endif
    }
}


/*
 * Start or stop RX based on register state.
 */
static void esp32s3_i2s_update_rx(ESP32S3I2sState *s)
{
    bool rx_start = FIELD_EX32(s->rx_conf, I2S_RX_CONF, RX_START);

    if (rx_start && !s->rx_running) {
        /* Starting RX */
        esp32s3_i2s_open_in(s);
        if (s->in_voice) {
            AUD_set_active_in(s->in_voice, 1);
            s->rx_running = true;
#if I2S_DEBUG
            info_report("[I2S] RX started");
#endif
        }
    } else if (!rx_start && s->rx_running) {
        /* Stopping RX */
        if (s->in_voice) {
            AUD_set_active_in(s->in_voice, 0);
        }
        s->rx_running = false;
#if I2S_DEBUG
        info_report("[I2S] RX stopped");
#endif
    }
}


static uint64_t esp32s3_i2s_read(void *opaque, hwaddr addr, unsigned int size)
{
    ESP32S3I2sState *s = ESP32S3_I2S(opaque);
    uint64_t r = 0;

    switch (addr) {
    case A_I2S_INT_RAW:
        r = s->int_raw;
        break;
    case A_I2S_INT_ST:
        r = s->int_raw & s->int_ena;
        break;
    case A_I2S_INT_ENA:
        r = s->int_ena;
        break;
    case A_I2S_INT_CLR:
        r = 0; /* write-only */
        break;
    case A_I2S_RX_CONF:
        r = s->rx_conf;
        break;
    case A_I2S_TX_CONF:
        r = s->tx_conf;
        break;
    case A_I2S_RX_CONF1:
        r = s->rx_conf1;
        break;
    case A_I2S_TX_CONF1:
        r = s->tx_conf1;
        break;
    case A_I2S_RX_CLKM_CONF:
        r = s->rx_clkm_conf;
        break;
    case A_I2S_TX_CLKM_CONF:
        r = s->tx_clkm_conf;
        break;
    case A_I2S_RX_CLKM_DIV_CONF:
        r = s->rx_clkm_div_conf;
        break;
    case A_I2S_TX_CLKM_DIV_CONF:
        r = s->tx_clkm_div_conf;
        break;
    case A_I2S_TX_PCM2PDM_CONF:
        r = s->tx_pcm2pdm_conf;
        break;
    case A_I2S_TX_PCM2PDM_CONF1:
        r = s->tx_pcm2pdm_conf1;
        break;
    case A_I2S_RX_PDM2PCM_CONF:
        r = s->rx_pdm2pcm_conf;
        break;
    case A_I2S_TX_TDM_CTRL:
        r = s->tx_tdm_ctrl;
        break;
    case A_I2S_TX_TDM_ACTIVE_CHAN:
        r = s->tx_tdm_active_chan;
        break;
    case A_I2S_RX_TDM_CTRL:
        r = s->rx_tdm_ctrl;
        break;
    case A_I2S_RX_TDM_ACTIVE_CHAN:
        r = s->rx_tdm_active_chan;
        break;
    case A_I2S_RX_TIMING:
        r = s->rx_timing;
        break;
    case A_I2S_TX_TIMING:
        r = s->tx_timing;
        break;
    case A_I2S_LC_HUNG_CONF:
        r = s->lc_hung_conf;
        break;
    case A_I2S_RXEOF_NUM:
        r = s->rxeof_num;
        break;
    case A_I2S_CONF_SIGLE_DATA:
        r = s->conf_sigle_data;
        break;
    case A_I2S_STATE:
        /* TX_IDLE is set when TX is not running */
        r = s->tx_running ? 0 : R_I2S_STATE_TX_IDLE_MASK;
        break;
    case A_I2S_DATE:
        r = s->date;
        break;
    default:
#if I2S_WARNING
        warn_report("[I2S] Unsupported read from 0x%lx", (unsigned long)addr);
#endif
        break;
    }

#if I2S_DEBUG
    info_report("[I2S] Read 0x%02lx => 0x%08lx",
                (unsigned long)addr, (unsigned long)r);
#endif

    return r;
}


static void esp32s3_i2s_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned int size)
{
    ESP32S3I2sState *s = ESP32S3_I2S(opaque);
    uint32_t wval = (uint32_t)value;

#if I2S_DEBUG
    info_report("[I2S] Write 0x%02lx <= 0x%08lx",
                (unsigned long)addr, (unsigned long)value);
#endif

    switch (addr) {
    case A_I2S_INT_RAW:
        s->int_raw = wval;
        esp32s3_i2s_update_irq(s);
        break;
    case A_I2S_INT_ENA:
        s->int_ena = wval;
        esp32s3_i2s_update_irq(s);
        break;
    case A_I2S_INT_CLR:
        s->int_raw &= ~wval;
        esp32s3_i2s_update_irq(s);
        break;
    case A_I2S_RX_CONF:
        /* Handle reset bits (self-clearing) */
        if (wval & R_I2S_RX_CONF_RX_RESET_MASK) {
            wval &= ~R_I2S_RX_CONF_RX_RESET_MASK;
        }
        if (wval & R_I2S_RX_CONF_RX_FIFO_RESET_MASK) {
            wval &= ~R_I2S_RX_CONF_RX_FIFO_RESET_MASK;
        }
        s->rx_conf = wval;
        esp32s3_i2s_update_rx(s);
        break;
    case A_I2S_TX_CONF:
        /* Handle reset bits (self-clearing) */
        if (wval & R_I2S_TX_CONF_TX_RESET_MASK) {
            wval &= ~R_I2S_TX_CONF_TX_RESET_MASK;
        }
        if (wval & R_I2S_TX_CONF_TX_FIFO_RESET_MASK) {
            wval &= ~R_I2S_TX_CONF_TX_FIFO_RESET_MASK;
        }
        s->tx_conf = wval;
        esp32s3_i2s_update_tx(s);
        break;
    case A_I2S_RX_CONF1:
        s->rx_conf1 = wval;
        break;
    case A_I2S_TX_CONF1:
        s->tx_conf1 = wval;
        break;
    case A_I2S_RX_CLKM_CONF:
        s->rx_clkm_conf = wval;
        break;
    case A_I2S_TX_CLKM_CONF:
        s->tx_clkm_conf = wval;
        break;
    case A_I2S_RX_CLKM_DIV_CONF:
        s->rx_clkm_div_conf = wval;
        break;
    case A_I2S_TX_CLKM_DIV_CONF:
        s->tx_clkm_div_conf = wval;
        break;
    case A_I2S_TX_PCM2PDM_CONF:
        s->tx_pcm2pdm_conf = wval;
        break;
    case A_I2S_TX_PCM2PDM_CONF1:
        s->tx_pcm2pdm_conf1 = wval;
        break;
    case A_I2S_RX_PDM2PCM_CONF:
        s->rx_pdm2pcm_conf = wval;
        break;
    case A_I2S_TX_TDM_CTRL:
        s->tx_tdm_ctrl = wval;
        break;
    case A_I2S_TX_TDM_ACTIVE_CHAN:
        s->tx_tdm_active_chan = wval;
        break;
    case A_I2S_RX_TDM_CTRL:
        s->rx_tdm_ctrl = wval;
        break;
    case A_I2S_RX_TDM_ACTIVE_CHAN:
        s->rx_tdm_active_chan = wval;
        break;
    case A_I2S_RX_TIMING:
        s->rx_timing = wval;
        break;
    case A_I2S_TX_TIMING:
        s->tx_timing = wval;
        break;
    case A_I2S_LC_HUNG_CONF:
        s->lc_hung_conf = wval;
        break;
    case A_I2S_RXEOF_NUM:
        s->rxeof_num = wval;
        break;
    case A_I2S_CONF_SIGLE_DATA:
        s->conf_sigle_data = wval;
        break;
    case A_I2S_DATE:
        s->date = wval;
        break;
    default:
#if I2S_WARNING
        warn_report("[I2S] Unsupported write to 0x%lx (0x%08lx)",
                    (unsigned long)addr, (unsigned long)value);
#endif
        break;
    }
}


static const MemoryRegionOps esp32s3_i2s_ops = {
    .read  = esp32s3_i2s_read,
    .write = esp32s3_i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};


static void esp32s3_i2s_reset_hold(Object *obj, ResetType type)
{
    ESP32S3I2sState *s = ESP32S3_I2S(obj);

    /* Stop any active audio */
    if (s->tx_running && s->out_voice) {
        AUD_set_active_out(s->out_voice, 0);
    }
    if (s->rx_running && s->in_voice) {
        AUD_set_active_in(s->in_voice, 0);
    }
    s->tx_running = false;
    s->rx_running = false;

    s->int_raw = 0;
    s->int_st = 0;
    s->int_ena = 0;
    s->int_clr = 0;

    s->rx_conf = 0;
    s->tx_conf = 0;
    s->rx_conf1 = 0;
    s->tx_conf1 = 0;

    s->rx_clkm_conf = 0;
    s->tx_clkm_conf = 0;
    s->rx_clkm_div_conf = 0;
    s->tx_clkm_div_conf = 0;

    s->tx_pcm2pdm_conf = 0;
    s->tx_pcm2pdm_conf1 = 0;
    s->rx_pdm2pcm_conf = 0;

    s->tx_tdm_ctrl = 0;
    s->tx_tdm_active_chan = 0xFFFF; /* all channels active by default */
    s->rx_tdm_ctrl = 0;
    s->rx_tdm_active_chan = 0xFFFF;

    s->rx_timing = 0;
    s->tx_timing = 0;

    s->lc_hung_conf = 0;
    s->rxeof_num = 0x40; /* default EOF threshold */

    s->conf_sigle_data = 0;
    s->state = R_I2S_STATE_TX_IDLE_MASK; /* TX idle after reset */

    s->date = 0x02101030; /* I2S version date */

    s->out_buf_pos = 0;
    s->in_buf_pos = 0;
    s->in_buf_len = 0;

    s->sample_rate = I2S_DEFAULT_SAMPLE_RATE;
    s->channels = I2S_DEFAULT_CHANNELS;
    s->bits_per_sample = I2S_DEFAULT_BITS;
}


static void esp32s3_i2s_realize(DeviceState *dev, Error **errp)
{
    ESP32S3I2sState *s = ESP32S3_I2S(dev);

    /* Register with QEMU audio subsystem */
    if (!AUD_register_card("esp32s3-i2s", &s->card, errp)) {
        return;
    }
}


static void esp32s3_i2s_init(Object *obj)
{
    ESP32S3I2sState *s = ESP32S3_I2S(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &esp32s3_i2s_ops, s,
                          TYPE_ESP32S3_I2S, ESP32S3_I2S_IO_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->gdma = NULL;
    s->gdma_periph_id = GDMA_I2S0;
    s->out_voice = NULL;
    s->in_voice = NULL;
    s->tx_running = false;
    s->rx_running = false;

    esp32s3_i2s_reset_hold(obj, RESET_TYPE_COLD);
}


static Property esp32s3_i2s_properties[] = {
    DEFINE_PROP_END_OF_LIST(),
};


static void esp32s3_i2s_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = esp32s3_i2s_reset_hold;
    dc->realize = esp32s3_i2s_realize;
    device_class_set_props(dc, esp32s3_i2s_properties);
}


static const TypeInfo esp32s3_i2s_info = {
    .name          = TYPE_ESP32S3_I2S,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ESP32S3I2sState),
    .instance_init = esp32s3_i2s_init,
    .class_init    = esp32s3_i2s_class_init,
};


static void esp32s3_i2s_register_types(void)
{
    type_register_static(&esp32s3_i2s_info);
}

type_init(esp32s3_i2s_register_types)
