#include <esp_http_client.h>
#include <esp_log.h>
#include <string.h>
#include <cJSON.h>

#include "main.h"

#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

esp_err_t oai_http_event_handler(esp_http_client_event_t *evt) {
  static int output_len;
  switch (evt->event_id) {
    case HTTP_EVENT_REDIRECT:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_REDIRECT");
      esp_http_client_set_header(evt->client, "From", "user@example.com");
      esp_http_client_set_header(evt->client, "Accept", "text/html");
      esp_http_client_set_redirection(evt->client);
      break;
    case HTTP_EVENT_ERROR:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ERROR");
      break;
    case HTTP_EVENT_ON_CONNECTED:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_CONNECTED");
      break;
    case HTTP_EVENT_HEADER_SENT:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_HEADER_SENT");
      break;
    case HTTP_EVENT_ON_HEADER:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s",
               evt->header_key, evt->header_value);
      break;
    case HTTP_EVENT_ON_DATA: {
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
      if (esp_http_client_is_chunked_response(evt->client)) {
        ESP_LOGE(LOG_TAG, "Chunked HTTP response not supported");
#ifndef LINUX_BUILD
        esp_restart();
#endif
      }

      if (output_len == 0 && evt->user_data) {
        memset(evt->user_data, 0, MAX_HTTP_OUTPUT_BUFFER);
      }

      // If user_data buffer is configured, copy the response into the buffer
      int copy_len = 0;
      if (evt->user_data) {
        // The last byte in evt->user_data is kept for the NULL character in
        // case of out-of-bound access.
        copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
        if (copy_len) {
          memcpy(((char *)evt->user_data) + output_len, evt->data, copy_len);
        }
      }
      output_len += copy_len;

      break;
    }
    case HTTP_EVENT_ON_FINISH:
      ESP_LOGD(LOG_TAG, "HTTP_EVENT_ON_FINISH");
      output_len = 0;
      break;
    case HTTP_EVENT_DISCONNECTED:
      ESP_LOGI(LOG_TAG, "HTTP_EVENT_DISCONNECTED");
      output_len = 0;
      break;
  }
  return ESP_OK;
}

char* fetch_client_secret(char* out_buffer, char *out_secret, size_t out_size) {
  ESP_LOGI(LOG_TAG, "Starting to fetch client secret");
  esp_http_client_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.url = "https://api.openai.com/v1/realtime/sessions";
  cfg.event_handler = oai_http_event_handler; 
  cfg.user_data = out_buffer; // レスポンスを受け取るバッファ

  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Authorization", "Bearer " OPENAI_API_KEY);
  esp_http_client_set_header(client, "Content-Type", "application/json");

#if 0
  static const char *body = "{\"model\":\"gpt-4o-realtime-preview-2024-12-17\",\"voice\":\"sage\",\"turn_detection\":null,\"instructions\":\"When you hear Japanese, translate it into English. When you hear English, translate it into Japanese. You are a professional interpreter, so speak only for interpretation.\"}";
#else
  static const char *body = "{\"model\":\"gpt-4o-realtime-preview-2024-12-17\",\"voice\":\"sage\",\"instructions\":\"When you hear Japanese, translate it into English. When you hear English, translate it into Japanese. You are a professional interpreter, so speak only for interpretation.\"}";
#endif
  esp_http_client_set_post_field(client, body, strlen(body));

  esp_err_t err;
  while(true) {
    err = esp_http_client_perform(client);
    if(err != ESP_ERR_HTTP_EAGAIN) { break; }
  }

  if (err == ESP_OK) {
    int status_code = esp_http_client_get_status_code(client);
    if (status_code == 200) {
      ESP_LOGI(LOG_TAG, "HTTP request successful");
      ESP_LOGI(LOG_TAG, "Response: %s", out_buffer);
      cJSON *root = cJSON_Parse(out_buffer);
      if (root) {
        cJSON *data = cJSON_GetObjectItem(root, "client_secret");
        if (data) {
          cJSON *secret_val = cJSON_GetObjectItem(data, "value");
          if (secret_val && cJSON_IsString(secret_val)) {
            strncpy(out_secret, secret_val->valuestring, out_size - 1);
            out_secret[out_size - 1] = '\0';
            ESP_LOGI(LOG_TAG, "Client secret: %s", out_secret);
          } else {
            ESP_LOGE(LOG_TAG, "Failed to find client_secret value in JSON");
          }
        } else {
          ESP_LOGE(LOG_TAG, "Failed to find data in JSON");
        }
        cJSON_Delete(root);
      } else {
        ESP_LOGE(LOG_TAG, "Failed to parse JSON");
      }
    } else {
      ESP_LOGE(LOG_TAG, "HTTP request failed with status code: %d", status_code);
    }
  } else {
    ESP_LOGE(LOG_TAG, "HTTP request failed: %s", esp_err_to_name(err));
  }
  esp_http_client_cleanup(client);
  return out_secret;
}

void oai_http_request(char *offer, char *answer, const char *secret) {
  esp_http_client_config_t config;
  memset(&config, 0, sizeof(esp_http_client_config_t));

  config.url = OPENAI_REALTIMEAPI;
  config.event_handler = oai_http_event_handler;
  config.user_data = answer;

  // 取得した client_secret を Bearer トークンに
  snprintf(answer, MAX_HTTP_OUTPUT_BUFFER, "Bearer %s", secret);

  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/sdp");
  esp_http_client_set_header(client, "Authorization", answer);
  esp_http_client_set_post_field(client, offer, strlen(offer));

  esp_err_t err = esp_http_client_perform(client);
  if (err != ESP_OK || esp_http_client_get_status_code(client) != 201) {
    ESP_LOGE(LOG_TAG, "Error perform http request %s", esp_err_to_name(err));
#ifndef LINUX_BUILD
    esp_restart();
#endif
  }

  esp_http_client_cleanup(client);
}
