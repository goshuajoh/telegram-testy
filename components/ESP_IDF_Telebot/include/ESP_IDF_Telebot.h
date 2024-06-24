#define MAX_HTTP_RECV_BUFFER 2048
#define MAX_HTTP_OUTPUT_BUFFER 4096

static const char *TAG = "HTTP_CLIENT Handler";
static const char *TAG1 = "wifi station";

#define WIFI_SSID "ESP_1"
#define WIFI_PASS "Espressif!123"
#define MAXIMUM_RETRY 10

#define TELEGRAM_BOT_TOKEN "7397170163:AAEyJvVdmEfe1Jt2hKoWH-KZZWu-OOIr56s"
char url_string[512] = "https://api.telegram.org/bot";
#define TELEGRAM_HOST "api.telegram.org"
#define TELEGRAM_SSL_PORT 443
#define chat_ID2 "219745533"

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static int s_retry_num = 0;

extern const char telegram_certificate_pem_start[] asm("_binary_telegram_certificate_pem_start");

void esp_tele_init(const char *ssid, const char *password);
void add_command(const char *command, void (*handler)(void));
void start_get_updates_task(void);