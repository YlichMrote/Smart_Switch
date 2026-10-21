#include "Audio.h"
#include "driver/i2s.h"


void playAudio(const uint8_t* audioData, size_t dataSize) {
  // 计算实际需要写入的字节数 (16位音频，每个采样点占2字节)
  size_t bytesToWrite = dataSize ; 
  size_t bytes_written = 0;
  
  // 将数据写入 I2S 缓冲区
  i2s_write(I2S_NUM_1, audioData, bytesToWrite, &bytes_written, portMAX_DELAY);
  
  // 【关键】播放完毕后，向 I2S 写入一段静音数据（全0）
  // 这能确保 DMA 缓冲区里残留的音频数据被冲刷干净，防止爆音
  int16_t silence[64] = {0}; 
  i2s_write(I2S_NUM_1, silence, sizeof(silence), &bytes_written, portMAX_DELAY);
}

void Audio_PlayHaode() {
  playAudio(audio_haode, AUDIO_HAODE_SIZE);   // “好的”
}

void Audio_PlayWozai() {
  playAudio(audio_wozai, AUDIO_WOZAI_SIZE);   // “我在”
}

void Audio_Init() {

  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = true  // 自动清除发送描述符，防止底噪
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK_PIN,
    .ws_io_num = I2S_LRC_PIN,
    .data_out_num = I2S_DOUT_PIN,
    .data_in_num = I2S_PIN_NO_CHANGE
  };

  i2s_driver_install(I2S_NUM_1, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &pin_config);
  
//   // 播放第一段音频：“好的”
//   playAudio(audio_haode, AUDIO_HAODE_SIZE);
  
//   // 播放第二段音频：“我在”
//   playAudio(audio_wozai, AUDIO_WOZAI_SIZE);
}