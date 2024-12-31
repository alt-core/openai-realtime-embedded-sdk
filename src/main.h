#include <peer.h>

#define LOG_TAG "realtimeapi-sdk"
#define MAX_HTTP_OUTPUT_BUFFER 2048
#define MAX_CLIENT_SECRET 256
#define TICK_INTERVAL 15

void oai_wifi(void);
void oai_init_audio_capture(void);
void oai_init_audio_decoder(void);
void oai_init_audio_encoder();
void oai_send_audio(PeerConnection *peer_connection);
void oai_audio_decode(uint8_t *data, size_t size);
void oai_webrtc();
void oai_http_request(char *offer, char *answer, const char *secret);

void init_i2s(bool speaker, bool mic);

char* fetch_client_secret(char* out_buffer, char *out_secret, size_t out_size);
void send_event_message(const char* message);

void update_display();
void set_display_dirty();

extern bool is_microphone_active;
extern bool is_speaker_active;
extern bool g_wifi_connected;
extern bool is_peer_connected;