/* Waveshare ESP32-S3-RLCD-4.2 audio.
 * Pin map and codec setup from waveshareteam/ESP32-S3-RLCD-4.2,
 * 02_Example/ESP-IDF/07_Audio_Test. Shared clocks stay at 16 kHz.
 */
#include "voice_board.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include <stdlib.h>
static esp_err_t speaker_write_all(const int16_t *samples, size_t count);
static const char *TAG = "link.audio";
static esp_codec_dev_handle_t s_spk, s_mic;
static bool s_initialized;
static i2s_chan_handle_t s_tx;
static unsigned s_down_phase;
esp_err_t voice_board_init(void) {
    // Board note (NEXT_STEPS.md): amp enable GPIO46 is active high and
    // must be raised BEFORE codec init, or the amp never comes up and
    // playback is silent even though I2S writes succeed.
    gpio_config_t pa = {.pin_bit_mask=1ULL<<46, .mode=GPIO_MODE_OUTPUT};
    gpio_config(&pa);
    gpio_set_level(46, 1);
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {.i2c_port=I2C_NUM_0, .sda_io_num=13, .scl_io_num=14,
        .clk_source=I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt=7, .flags.enable_internal_pullup=true};
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bc, &bus), TAG, "I2C");
    /* Probe before allocating I2S DMA; addresses are 7-bit here. */
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, 0x18, 100), TAG, "ES8311 absent");
    ESP_RETURN_ON_ERROR(i2c_master_probe(bus, 0x40, 100), TAG, "ES7210 absent");
    i2s_chan_handle_t tx, rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num=8; cc.dma_frame_num=160; cc.auto_clear=true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&cc, &tx, &rx), TAG, "I2S channels");
    s_tx = tx;
    i2s_std_config_t sc = {
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=16,.bclk=9,.ws=45,.dout=8,.din=10},
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx,&sc), TAG,"I2S TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx,&sc), TAG,"I2S RX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), TAG,"TX enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), TAG,"RX enable");
    audio_codec_i2s_cfg_t dc = {.port=I2S_NUM_0, .tx_handle=tx, .rx_handle=rx};
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&dc);
    const audio_codec_gpio_if_t *gpio = audio_codec_new_gpio();
    audio_codec_i2c_cfg_t ic = {.port=I2C_NUM_0,.addr=ES8311_CODEC_DEFAULT_ADDR,.bus_handle=bus};
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ic);
    es8311_codec_cfg_t spkc = {.codec_mode=ESP_CODEC_DEV_WORK_MODE_DAC,.ctrl_if=ctrl,
        .gpio_if=gpio,.pa_pin=46,.use_mclk=true,.hw_gain.pa_gain=6};
    const audio_codec_if_t *spk = es8311_codec_new(&spkc);
    ic.addr=ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *micctrl = audio_codec_new_i2c_ctrl(&ic);
    es7210_codec_cfg_t micc = {.ctrl_if=micctrl,.mic_selected=ES7210_SEL_MIC1 | ES7210_SEL_MIC3};
    const audio_codec_if_t *mic = es7210_codec_new(&micc);
    if (!data || !gpio || !ctrl || !spk || !micctrl || !mic) return ESP_ERR_NO_MEM;
    esp_codec_dev_cfg_t dev = {.codec_if=spk,.data_if=data,.dev_type=ESP_CODEC_DEV_TYPE_OUT};
    s_spk=esp_codec_dev_new(&dev);
    dev.codec_if=mic; dev.dev_type=ESP_CODEC_DEV_TYPE_IN;
    s_mic=esp_codec_dev_new(&dev);
    if (!s_spk || !s_mic) return ESP_ERR_NO_MEM;
    esp_codec_dev_sample_info_t fmt = {.sample_rate=16000,.channel=2,.bits_per_sample=16};
    if (esp_codec_dev_open(s_spk,&fmt) || esp_codec_dev_open(s_mic,&fmt)) return ESP_FAIL;
    // ES7210 PGA at its 37.5 dB maximum (was 25 dB): Scott's captures
    // were consistently quiet, and he asked for the gain cranked hard.
    // Watch for clipping on close/loud speech; 30-33 dB is the fallback.
    esp_err_t gerr = esp_codec_dev_set_in_gain(s_mic, 37.5f);
    ESP_LOGI(TAG, "ES7210 mic input gain 37.5 dB: %s", esp_err_to_name(gerr));
    voice_board_set_volume(100); voice_board_amp(true);
    s_initialized=true;
    ESP_LOGI(TAG,"ES8311 speaker and ES7210 mic ready: 16 kHz stereo");
    return ESP_OK;
}
esp_err_t voice_board_mic_start(void) { return s_initialized ? ESP_OK : ESP_ERR_INVALID_STATE; }
void voice_board_mic_stop(void) {}
// Digital gain on top of the ES7210's 37.5 dB analog maximum: measured
// captures at full analog gain still peaked ~-21 dBFS (RMS ~-39 dBFS),
// well under healthy speech levels, so multiply by 4 (+12 dB) with
// saturation. Raises the noise floor by the same 12 dB; drop to 2 if
// close/loud speech clips.
#define MIC_DIGITAL_GAIN 4
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    int16_t stereo[640];
    if (frames > 320) frames=320;
    if (!s_initialized || esp_codec_dev_read(s_mic, stereo, frames*4)) return 0;
    *peak=0;
    for (size_t i=0;i<frames;i++) {
        int v=stereo[2*i]*MIC_DIGITAL_GAIN;
        if (v>32767) v=32767; else if (v<-32768) v=-32768;
        pcm[i]=(int16_t)v;
        int a=abs(v); if(a>*peak) *peak=a;
    }
    return frames;
}
// Preserve progress on timeout: never discard or replay a partially queued block.
static esp_err_t speaker_write_all(const int16_t *samples, size_t count) {
    const uint8_t *p = (const uint8_t *)samples;
    size_t left = count * sizeof(*samples);
    unsigned stalled = 0;
    while (left) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx, p, left, &written, 1000);
        p += written; left -= written;
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return err;
        if (written) stalled = 0;
        else if (++stalled >= 3) return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    /* Existing player emits 48 kHz stereo/32-bit. Decimate to shared 16 kHz. */
    int16_t out[640]; size_t n=0;
    for(size_t i=0;i<count;i++) {
        if (++s_down_phase != 3) continue;
        s_down_phase=0;
        out[n++]=frames[2*i]/65536; out[n++]=frames[2*i+1]/65536;
        if(n==640) { esp_err_t err=speaker_write_all(out,n); if(err != ESP_OK) return err; n=0; }
    }
    return n ? speaker_write_all(out,n) : ESP_OK;
}
void voice_board_amp(bool on) { gpio_set_level(46,on); }
void voice_board_set_volume(int percent) { if(s_spk) esp_codec_dev_set_out_vol(s_spk,percent); }
bool voice_board_muted(void) { return false; }
int voice_board_dial_steps(void) { return 0; }
// No acoustic echo cancellation on this board (same as the reSpeaker Lite
// backend); upstream added this query after the original port.
bool voice_board_echo_cancelled(void) { return false; }
