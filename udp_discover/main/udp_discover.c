/* BSD Socket API Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>


/* The examples use simple WiFi configuration that you can set via
   'make menuconfig'.
   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_WIFI_SSID "esp-liyin"
#define EXAMPLE_WIFI_PASS "espressif"

#define NOTICE_UDP_PORT 7889
#define NOTICE_TCP_PORT 8899
#define NOTICE_UDP_BUF_SIZE (64)
#define NOTICE_UDP_RETRY_COUNT  (3)

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

const int IPV4_GOTIP_BIT = BIT0;
const int IPV6_GOTIP_BIT = BIT1;

static const char *TAG = "tcp discover";
static const char *payload = "Are You ESP32 Device?";

static ip4_addr_t subnet_addr;

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
    case SYSTEM_EVENT_STA_START:
        esp_wifi_connect();
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_START");
        break;
    case SYSTEM_EVENT_STA_CONNECTED:
        /* enable ipv6 */
        tcpip_adapter_create_ip6_linklocal(TCPIP_ADAPTER_IF_STA);
        break;
    case SYSTEM_EVENT_STA_GOT_IP:
        xEventGroupSetBits(wifi_event_group, IPV4_GOTIP_BIT);
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP");
        // caculater sub net addr
        subnet_addr.addr = event->event_info.got_ip.ip_info.ip.addr & event->event_info.got_ip.ip_info.netmask.addr;
        ESP_LOGI(TAG, "subnet_addr:"IPSTR, IP2STR(&subnet_addr));
        subnet_addr.addr += 2 << 3 * 8;
        break;
    case SYSTEM_EVENT_STA_DISCONNECTED:
        /* This is a workaround as ESP32 WiFi libs don't currently auto-reassociate. */
        esp_wifi_connect();
        xEventGroupClearBits(wifi_event_group, IPV4_GOTIP_BIT);
        xEventGroupClearBits(wifi_event_group, IPV6_GOTIP_BIT);
        break;
    case SYSTEM_EVENT_AP_STA_GOT_IP6:
        xEventGroupSetBits(wifi_event_group, IPV6_GOTIP_BIT);
        ESP_LOGI(TAG, "SYSTEM_EVENT_STA_GOT_IP6");

        char *ip6 = ip6addr_ntoa(&event->event_info.got_ip6.ip6_info.ip);
        ESP_LOGI(TAG, "IPv6: %s", ip6);
    default:
        break;
    }
    return ESP_OK;
}

static void initialise_wifi(void)
{
    tcpip_adapter_init();
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_WIFI_SSID,
            .password = EXAMPLE_WIFI_PASS,
        },
    };
    ESP_LOGI(TAG, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
    ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK( esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK( esp_wifi_start() );
}

static void wait_for_ip()
{
    uint32_t bits = IPV4_GOTIP_BIT | IPV6_GOTIP_BIT ;

    ESP_LOGI(TAG, "Waiting for AP connection...");
    xEventGroupWaitBits(wifi_event_group, bits, false, true, portMAX_DELAY);
    ESP_LOGI(TAG, "Connected to AP");
}

/**
 * @brief udp 发现 server 初始化
 */
static int notice_udp_server_create()
{
    esp_err_t ret = ESP_OK;
    int sockfd = 0;
    struct sockaddr_in server_addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(NOTICE_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };

retry:
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        goto ERR_EXIT;
    }

    ret = bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (sockfd < 0) {
        goto ERR_EXIT;
    }

    struct timeval socket_timeout = {0, 100 * 1000};
    ret = setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(struct timeval));
    if (sockfd < 0) {
        goto ERR_EXIT;
    }

    ESP_LOGD("udp notice", "create udp server, port: %d, sockfd: %d", NOTICE_UDP_PORT, sockfd);

    return sockfd;

ERR_EXIT:

    if (sockfd != -1) {
        ret = close(sockfd);

        if (ret != ESP_OK) {
            ESP_LOGD("udp notice", "close fail, ret: %d", ret);
        }
    }
    goto retry;

    return -1;
}

/**
 * @brief udp 发现初始化
 */
static void notice_udp_task(void *arg)
{
    uint8_t root_mac[6]          = {0};
    char *udp_server_buf         = malloc(NOTICE_UDP_BUF_SIZE);
    struct sockaddr_in from_addr = {0};
    socklen_t from_addr_len      = sizeof(struct sockaddr_in);

    int udp_server_sockfd        = notice_udp_server_create();

    if (udp_server_sockfd == -1) {
        ESP_LOGE("notice udp", "Failed to create UDP notification service");

        vTaskDelete(NULL);
        return ;
    }

    ESP_ERROR_CHECK(esp_wifi_get_mac(ESP_IF_WIFI_STA, root_mac));

    while(1){
        memset(udp_server_buf, 0, NOTICE_UDP_BUF_SIZE);
        if (recvfrom(udp_server_sockfd, udp_server_buf, NOTICE_UDP_BUF_SIZE,
                            0, (struct sockaddr *)&from_addr, (socklen_t *)&from_addr_len) > 0) {
            ESP_LOGD("udp notice task", "Mlink notice udp recvfrom, sockfd: %d, port: %d, ip: %s, udp_server_buf: %s",
                     udp_server_sockfd, ntohs(((struct sockaddr_in *)&from_addr)->sin_port),
                     inet_ntoa(((struct sockaddr_in *)&from_addr)->sin_addr), udp_server_buf);

            if (strcmp(udp_server_buf, "Are You ESP32 Device?")) {
                continue;
            }

            sprintf(udp_server_buf, "ESP32 MAC:%02x%02x%02x%02x%02x%02x TCP:%d",
                    MAC2STR(root_mac), NOTICE_TCP_PORT);

            ESP_LOGD("udp notice task", "Mlink notice udp sendto, sockfd: %d, data: %s", udp_server_sockfd, udp_server_buf);

            for (int i = 0, delay_time_ms = 0; i < NOTICE_UDP_RETRY_COUNT; ++i, delay_time_ms += delay_time_ms) {
                vTaskDelay(delay_time_ms);
                delay_time_ms = (i == 0) ? (10 / portTICK_RATE_MS) : delay_time_ms;

                if (sendto(udp_server_sockfd, udp_server_buf, strlen(udp_server_buf),
                           0, (struct sockaddr *)&from_addr, from_addr_len) <= 0) {
                    ESP_LOGW("udp notice task", "Mlink notice udp sendto, errno: %d, errno_str: %s", errno, strerror(errno));
                    break;
                }
            }
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

static u32_t discover_addr;

void app_main()
{
    ESP_ERROR_CHECK( nvs_flash_init() );

    // initialise wifi
    initialise_wifi();

    // wait network connected
    wait_for_ip();
    
    // initialise udp notice service
    xTaskCreatePinnedToCore(notice_udp_task, "notice_udp", 3 * 1024, NULL, 4, NULL, 1);
}
