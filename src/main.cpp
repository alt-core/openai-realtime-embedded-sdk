#include "main.h"

#include <esp_event.h>
#include <esp_log.h>
#include <peer.h>

#ifndef LINUX_BUILD
#include "nvs_flash.h"
#include <esp_timer.h>
#include <esp_heap_caps.h>

#include <M5Unified.h>

constexpr const char* TAG = "main";

#ifdef CONFIG_ENABLE_HEAP_MONITOR
static esp_timer_handle_t s_monitor_timer;
#endif // CONFIG_ENABLE_HEAP_MONITOR

bool is_microphone_active = false;
bool display_dirty = false;
extern PeerConnection *peer_connection;

static int debounce_counter = 0;
constexpr int debounce_threshold = 6; // Debounce threshold (15*6=90ms)

void set_display_dirty() {
  display_dirty = true;
}

extern "C" void app_main(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

#ifdef CONFIG_ENABLE_HEAP_MONITOR
  esp_timer_create_args_t timer_args = {
      .callback = [](void* arg) {
    ESP_LOGW(TAG, "current heap %7d | minimum ever %7d | largest free %7d ",
             xPortGetFreeHeapSize(),
             xPortGetMinimumEverFreeHeapSize(),
             heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
      },
      .arg = nullptr,
      .dispatch_method = ESP_TIMER_TASK,
      .name = "monitor_timer"
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_monitor_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_monitor_timer, CONFIG_HEAP_MONITOR_INTERVAL_MS * 1000ULL));
#endif // CONFIG_ENABLE_HEAP_MONITOR

  auto cfg = M5.config();
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  M5.begin(cfg);

  M5.Lcd.clear();
  update_display(); // Display initial state

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  oai_wifi();
  update_display(); // WiFi connected

  oai_init_audio_capture();
  oai_init_audio_decoder();
  
  oai_webrtc();
  set_display_dirty();

  while (true) {
    M5.update();

    if (debounce_counter > 0) {
      debounce_counter--;
    } else if (M5.Touch.getCount()) { // Check for screen touch
      is_microphone_active = true;
      set_display_dirty();
      debounce_counter = debounce_threshold; // Reset debounce counter
    } else if (is_microphone_active) {
      is_microphone_active = false;
      set_display_dirty();
    }

    if (display_dirty) {
      update_display();
      display_dirty = false;
    }

    if (is_peer_connected) {
      peer_connection_loop(peer_connection);
    }

    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#else
int main(void) {
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  peer_init();
  oai_webrtc();
  while (true) {
    if (is_peer_connected) {
      peer_connection_loop(peer_connection);
    }
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif

void update_display() {
  int color = TFT_DARKGRAY;
  if (g_wifi_connected) {
    if (!is_peer_connected) {
      color = TFT_WHITE;
    } else if (is_microphone_active) {
      color = TFT_RED;
    } else {
      color = TFT_GREEN;
    }
  }
  M5.Lcd.clear(color); // Change background color
}
