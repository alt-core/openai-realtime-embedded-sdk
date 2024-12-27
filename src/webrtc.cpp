#ifndef LINUX_BUILD
#include <driver/i2s.h>
#include <opus.h>
#endif

#include <esp_event.h>
#include <esp_log.h>
#include <string.h>

#include "main.h"

PeerConnection *peer_connection = NULL;
bool is_peer_connected = false;
bool is_datachannel_opened = false;

#ifndef LINUX_BUILD
StaticTask_t task_buffer;
void oai_send_audio_task(void *user_data) {
  oai_init_audio_encoder();

  while (1) {
    oai_send_audio(peer_connection);
    vTaskDelay(pdMS_TO_TICKS(TICK_INTERVAL));
  }
}
#endif

static void oai_onconnectionstatechange_task(PeerConnectionState state,
                                             void *user_data) {
  ESP_LOGI(LOG_TAG, "PeerConnectionState: %s",
           peer_connection_state_to_string(state));

  if (state == PEER_CONNECTION_DISCONNECTED ||
      state == PEER_CONNECTION_CLOSED) {
#ifndef LINUX_BUILD
    esp_restart();
#endif
  } else if (state == PEER_CONNECTION_CONNECTED) {
#ifndef LINUX_BUILD
#if CONFIG_SPIRAM
    constexpr size_t stack_size = 20000;
    StackType_t *stack_memory = (StackType_t *)heap_caps_malloc(
        stack_size * sizeof(StackType_t), MALLOC_CAP_SPIRAM);
#else // CONFIG_SPIRAM
    constexpr size_t stack_size = 20000;
    StackType_t *stack_memory = (StackType_t *)malloc(stack_size * sizeof(StackType_t));
#endif // CONFIG_SPIRAM
    if (stack_memory == nullptr) {
      ESP_LOGE(LOG_TAG, "Failed to allocate stack memory for audio publisher.");
      esp_restart();
    }
    xTaskCreateStaticPinnedToCore(oai_send_audio_task, "audio_publisher", stack_size,
                                  NULL, 7, stack_memory, &task_buffer, 0);
#endif
  }
}

static void oai_on_icecandidate_task(char *description, void *user_data) {
  char local_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};
  char *client_secret = (char *)user_data;
  oai_http_request(description, local_buffer, client_secret);
  peer_connection_set_remote_description(peer_connection, local_buffer);
}

void send_webrtc_message(const char* message) {
  if (is_peer_connected && is_datachannel_opened) {
    peer_connection_datachannel_send(peer_connection, (char*)message, strlen(message));
  }
}

static void on_datachannel_open(void* user_data) {
  is_datachannel_opened = true;
}

static void on_datachannel_close(void* user_data) {
}

static void on_datachannel_message(char* msg, size_t size, void* user_data, uint16_t sid) {
  ESP_LOGI(LOG_TAG, "Received WebRTC message: %.*s", size, msg);
}

void oai_webrtc() {
  ESP_LOGI(LOG_TAG, "Initializing WebRTC connection");
  // Buffer to store client_secret (256 bytes)
  static char s_client_secret[MAX_CLIENT_SECRET];
  memset(s_client_secret, 0, sizeof(s_client_secret));

  // Obtain client_secret
  {
    char local_buffer[MAX_HTTP_OUTPUT_BUFFER + 1] = {0};

    fetch_client_secret(local_buffer, s_client_secret, sizeof(s_client_secret));
    ESP_LOGI(LOG_TAG, "client_secret: %s", s_client_secret);
    if (s_client_secret[0] == 0) {
      ESP_LOGE(LOG_TAG, "Failed to obtain client_secret");
#ifndef LINUX_BUILD
      esp_restart();
#endif
    }
  }

  PeerConfiguration peer_connection_config = {
      .ice_servers = {},
      .audio_codec = CODEC_OPUS,
      .video_codec = CODEC_NONE,
      .datachannel = DATA_CHANNEL_STRING,
      .onaudiotrack = [](uint8_t *data, size_t size, void *userdata) -> void {
#ifndef LINUX_BUILD
        oai_audio_decode(data, size);
#endif
      },
      .onvideotrack = NULL,
      .on_request_keyframe = NULL,
      .user_data = s_client_secret,
  };

  peer_connection = peer_connection_create(&peer_connection_config);
  if (peer_connection == NULL) {
    ESP_LOGE(LOG_TAG, "Failed to create peer connection");
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }

  peer_connection_oniceconnectionstatechange(peer_connection,
                                             oai_onconnectionstatechange_task);
  peer_connection_onicecandidate(peer_connection, oai_on_icecandidate_task);
  peer_connection_ondatachannel(peer_connection, on_datachannel_message, on_datachannel_open, on_datachannel_close);
  peer_connection_create_offer(peer_connection);

  is_peer_connected = (peer_connection != NULL);
}
