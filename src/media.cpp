#include <driver/i2s.h>
#include <opus.h>

#include "main.h"

#include <esp_log.h>
#include <M5Unified.h>

#include <cstdint>
#include <vector>
#include <sys/socket.h>

#define OPUS_OUT_BUFFER_SIZE 1276  // 1276 bytes is recommended by opus_encode
#define SAMPLE_RATE 8000
#define BUFFER_SAMPLES 320

#define MCLK_PIN 0
#define BCLK_PIN 34
#define LRCLK_PIN 33
#define DATA_IN_PIN 14
#define DATA_OUT_PIN 13

#define OPUS_ENCODER_BITRATE 30000
#define OPUS_ENCODER_COMPLEXITY 0

constexpr const char *TAG = "media";

// UDP socket for audio data debugging
#ifdef CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT
static struct sockaddr_in s_debug_audio_in_dest_addr;
static struct sockaddr_in s_debug_audio_out_dest_addr;
static ssize_t s_debug_audio_sock;
#endif // CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT

// Initialization of AW88298 and ES7210 from M5Unified implementation.
constexpr std::uint8_t aw88298_i2c_addr = 0x36;
constexpr std::uint8_t es7210_i2c_addr = 0x40;
constexpr std::uint8_t aw9523_i2c_addr = 0x58;
static void aw88298_write_reg(std::uint8_t reg, std::uint16_t value)
{
  value = __builtin_bswap16(value);
  M5.In_I2C.writeRegister(aw88298_i2c_addr, reg, (const std::uint8_t*)&value, 2, 400000);
}

static void es7210_write_reg(std::uint8_t reg, std::uint8_t value)
{
  M5.In_I2C.writeRegister(es7210_i2c_addr, reg, &value, 1, 400000);
}

static void initialize_speaker_cores3()
{
  M5.In_I2C.bitOn(aw9523_i2c_addr, 0x02, 0b00000100, 400000);

  aw88298_write_reg( 0x61, 0x0673 );  // boost mode disabled 
  aw88298_write_reg( 0x04, 0x4040 );  // I2SEN=1 AMPPD=0 PWDN=0
  aw88298_write_reg( 0x05, 0x0008 );  // RMSE=0 HAGCE=0 HDCCE=0 HMUTE=0
  aw88298_write_reg( 0x06, 0x14C0 );  // INPLEV=0 (not attenuated), I2SRXEN=1 (enable), CHSEL=01 (left), I2SMD=00 (Philips Standard I2S), I2SFS=00 (16bit), I2SBCK=00 (32*fs), I2SSR=0000 (8kHz)
  aw88298_write_reg( 0x0C, 0x0064 );  // volume setting (full volume)
}

static void initialize_microphone_cores3()
{
  es7210_write_reg(0x00, 0xFF); // RESET_CTL
  struct __attribute__((packed)) reg_data_t
  {
    uint8_t reg;
    uint8_t value;
  };
  
  static constexpr reg_data_t data[] =
  {
    { 0x00, 0x41 }, // RESET_CTL
    { 0x01, 0x1f }, // CLK_ON_OFF
    { 0x06, 0x00 }, // DIGITAL_PDN
    { 0x07, 0x20 }, // ADC_OSR
    { 0x08, 0x10 }, // MODE_CFG
    { 0x09, 0x30 }, // TCT0_CHPINI
    { 0x0A, 0x30 }, // TCT1_CHPINI
    { 0x20, 0x0a }, // ADC34_HPF2
    { 0x21, 0x2a }, // ADC34_HPF1
    { 0x22, 0x0a }, // ADC12_HPF2
    { 0x23, 0x2a }, // ADC12_HPF1
    { 0x02, 0xC1 },
    { 0x04, 0x01 },
    { 0x05, 0x00 },
    { 0x11, 0x60 },
    { 0x40, 0x42 }, // ANALOG_SYS
    { 0x41, 0x70 }, // MICBIAS12
    { 0x42, 0x70 }, // MICBIAS34
    { 0x43, 0x1B }, // MIC1_GAIN
    { 0x44, 0x1B }, // MIC2_GAIN
    { 0x45, 0x00 }, // MIC3_GAIN
    { 0x46, 0x00 }, // MIC4_GAIN
    { 0x47, 0x00 }, // MIC1_LP
    { 0x48, 0x00 }, // MIC2_LP
    { 0x49, 0x00 }, // MIC3_LP
    { 0x4A, 0x00 }, // MIC4_LP
    { 0x4B, 0x00 }, // MIC12_PDN
    { 0x4C, 0xFF }, // MIC34_PDN
    { 0x01, 0x14 }, // CLK_ON_OFF
  };
  for (auto& d: data)
  {
    es7210_write_reg(d.reg, d.value);
  }
}

void oai_init_audio_capture() {
  ESP_LOGI(TAG, "Initializing microphone");
  initialize_microphone_cores3();
  ESP_LOGI(TAG, "Initializing speaker");
  initialize_speaker_cores3();

#ifdef CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT
  // Initialize UDP socket for debug.
  s_debug_audio_sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (s_debug_audio_sock < 0) {
    ESP_LOGE(TAG, "Failed to create socket");
    return;
  }
  
  s_debug_audio_in_dest_addr.sin_addr.s_addr = inet_addr(CONFIG_MEDIA_DEBUG_AUDIO_HOST);
  s_debug_audio_in_dest_addr.sin_family = AF_INET;
  s_debug_audio_in_dest_addr.sin_port = htons(CONFIG_MEDIA_DEBUG_AUDIO_IN_PORT);
  s_debug_audio_out_dest_addr.sin_addr.s_addr = inet_addr(CONFIG_MEDIA_DEBUG_AUDIO_HOST);
  s_debug_audio_out_dest_addr.sin_family = AF_INET;
  s_debug_audio_out_dest_addr.sin_port = htons(CONFIG_MEDIA_DEBUG_AUDIO_OUT_PORT);
#endif // CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT

  ESP_LOGI(TAG, "Initializing I2S for audio input/output");
  i2s_config_t i2s_config = {
      .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
      .sample_rate = SAMPLE_RATE,
      .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
      .communication_format = I2S_COMM_FORMAT_I2S,
      .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
      .dma_buf_count = 8,
      .dma_buf_len = BUFFER_SAMPLES,
      .use_apll = 1,
      .tx_desc_auto_clear = true,
  };
  if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure I2S driver for audio input/output");
    return;
  }

  i2s_pin_config_t pin_config = {
      .mck_io_num = MCLK_PIN,
      .bck_io_num = BCLK_PIN,
      .ws_io_num = LRCLK_PIN,
      .data_out_num = DATA_OUT_PIN,
      .data_in_num = DATA_IN_PIN,
  };
  if (i2s_set_pin(I2S_NUM_0, &pin_config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set I2S pins for audio input/output");
    return;
  }
  i2s_zero_dma_buffer(I2S_NUM_0);
}

opus_int16 *output_buffer = NULL;
OpusDecoder *opus_decoder = NULL;

void oai_init_audio_decoder() {
  int decoder_error = 0;
  opus_decoder = opus_decoder_create(SAMPLE_RATE, 1, &decoder_error);
  if (decoder_error != OPUS_OK) {
    ESP_LOGE(TAG, "Failed to create OPUS decoder");
    return;
  }

  output_buffer = (opus_int16 *)malloc(BUFFER_SAMPLES * sizeof(opus_int16));
}

void oai_audio_decode(uint8_t *data, size_t size) {
  int decoded_size =
      opus_decode(opus_decoder, data, size, output_buffer, BUFFER_SAMPLES, 0);

  if (decoded_size > 0) {
    std::size_t bytes_written = 0;
    if( esp_err_t err = i2s_write(I2S_NUM_0, output_buffer, decoded_size * sizeof(opus_int16),
              &bytes_written, portMAX_DELAY); err != ESP_OK ) {
      ESP_LOGE(TAG, "Failed to write audio data to I2S: %s", esp_err_to_name(err));
    }
#ifdef CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT
    sendto(s_debug_audio_sock, output_buffer, decoded_size * sizeof(opus_int16), 0, (struct sockaddr *)&s_debug_audio_out_dest_addr, sizeof(s_debug_audio_out_dest_addr));
#endif // CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT
  }
}

OpusEncoder *opus_encoder = NULL;
opus_int16 *encoder_input_buffer = NULL;
uint8_t *encoder_output_buffer = NULL;

void oai_init_audio_encoder() {
  int encoder_error;
  opus_encoder = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP,
                                     &encoder_error);
  if (encoder_error != OPUS_OK) {
    ESP_LOGE(TAG, "Failed to create OPUS encoder");
    return;
  }

  if (opus_encoder_init(opus_encoder, SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP) !=
      OPUS_OK) {
    ESP_LOGE(TAG, "Failed to initialize OPUS encoder");
    return;
  }

  opus_encoder_ctl(opus_encoder, OPUS_SET_BITRATE(OPUS_ENCODER_BITRATE));
  opus_encoder_ctl(opus_encoder, OPUS_SET_COMPLEXITY(OPUS_ENCODER_COMPLEXITY));
  opus_encoder_ctl(opus_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  encoder_input_buffer = (opus_int16 *)malloc(BUFFER_SAMPLES*sizeof(opus_int16));
  encoder_output_buffer = (uint8_t *)malloc(OPUS_OUT_BUFFER_SIZE);
}

void oai_send_audio(PeerConnection *peer_connection) {
  static int silence_frame_count = 0;
  size_t bytes_read = 0;
  if( esp_err_t err = i2s_read(I2S_NUM_0, encoder_input_buffer, BUFFER_SAMPLES*sizeof(opus_int16), &bytes_read,
           portMAX_DELAY) ; err != ESP_OK ) {
    ESP_LOGE(TAG, "Failed to read audio data from I2S: %s", esp_err_to_name(err));
  }

  if (!is_microphone_active) {
    // マイクが無効な場合、音声をマスク
    if (silence_frame_count < 100) {
      memset(encoder_input_buffer, 0, BUFFER_SAMPLES*sizeof(opus_int16));
      silence_frame_count++;
    } else {
      // VADを認識するには十分な一定の無音を送信したので送信を止める
      return;
    }
  } else {
    // マイクが有効な場合、カウンタをリセット
    silence_frame_count = 0;
  }

#ifdef CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT
  sendto(s_debug_audio_sock, encoder_input_buffer, BUFFER_SAMPLES*sizeof(opus_int16), 0, (struct sockaddr *)&s_debug_audio_in_dest_addr, sizeof(s_debug_audio_in_dest_addr));
#endif // CONFIG_MEDIA_ENABLE_DEBUG_AUDIO_UDP_CLIENT

  auto encoded_size =
      opus_encode(opus_encoder, encoder_input_buffer, BUFFER_SAMPLES,
                  encoder_output_buffer, OPUS_OUT_BUFFER_SIZE);

  peer_connection_send_audio(peer_connection, encoder_output_buffer,
                             encoded_size);
}
