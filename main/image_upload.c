#include "image_upload.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "image_upload";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAXIMUM_RETRY 10

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num;
static bool s_netif_ready;
static bool s_event_loop_ready;
static bool s_handlers_registered;
static bool s_wifi_started;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static image_upload_handler_t s_handler;
static size_t s_expected_size;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnect reason: %u", event->reason);
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGW(TAG, "Failed to connect to the AP");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Open http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address");
    }
}

static bool build_sta_config(wifi_config_t *wifi_config)
{
    memset(wifi_config, 0, sizeof(*wifi_config));
    strlcpy((char *)wifi_config->sta.ssid, CONFIG_WIFI_SSID,
            sizeof(wifi_config->sta.ssid));
    strlcpy((char *)wifi_config->sta.password, CONFIG_WIFI_PASSWORD,
            sizeof(wifi_config->sta.password));
    wifi_config->sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config->sta.sae_pwe_h2e = WPA3_SAE_PWE_UNSPECIFIED;
    wifi_config->sta.pmf_cfg.capable = true;
    wifi_config->sta.pmf_cfg.required = false;

    if (strlen((const char *)wifi_config->sta.ssid) == 0) {
        return false;
    }

    if (strlen((const char *)wifi_config->sta.password) == 0) {
        wifi_config->sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    return true;
}

static void generate_ap_credentials(char *ssid, size_t ssid_len, char *pass,
                                    size_t pass_len)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, ssid_len, "epd-%02x%02x%02x", mac[3], mac[4], mac[5]);
    snprintf(pass, pass_len, "epd-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t wifi_init_sta(wifi_config_t *wifi_config)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    if (!s_netif_ready) {
        esp_err_t netif_ret = esp_netif_init();
        if (netif_ret != ESP_OK && netif_ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(netif_ret, TAG, "netif init failed");
        }
        s_netif_ready = true;
    }

    if (!s_event_loop_ready) {
        esp_err_t loop_ret = esp_event_loop_create_default();
        if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(loop_ret, TAG, "event loop init failed");
        }
        s_event_loop_ready = true;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

        if (!s_handlers_registered) {
            ESP_RETURN_ON_ERROR(
                esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                    &wifi_event_handler, NULL, NULL),
                TAG, "wifi handler register failed");
            ESP_RETURN_ON_ERROR(
                esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                    &wifi_event_handler, NULL, NULL),
                TAG, "ip handler register failed");
            s_handlers_registered = true;
        }
    }

    ESP_LOGI(TAG, "WiFi SSID: %s", (const char *)wifi_config->sta.ssid);
    ESP_LOGI(TAG, "WiFi password: %s", (const char *)wifi_config->sta.password);
    ESP_LOGI(TAG, "Connecting to %s", (const char *)wifi_config->sta.ssid);

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                            "set storage failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, wifi_config), TAG,
                            "set config failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "set power save failed");
        if (s_sta_netif) {
            esp_err_t dhcp_ret = esp_netif_dhcpc_start(s_sta_netif);
            if (dhcp_ret != ESP_OK && dhcp_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "DHCP client start failed: %s", esp_err_to_name(dhcp_ret));
            } else {
                ESP_LOGI(TAG, "DHCP client started");
            }
        }
        s_wifi_started = true;
    }

    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to AP");
    return ESP_FAIL;
}

static esp_err_t wifi_init_ap(void)
{
    if (!s_wifi_event_group) {
        s_wifi_event_group = xEventGroupCreate();
    }

    if (!s_netif_ready) {
        esp_err_t netif_ret = esp_netif_init();
        if (netif_ret != ESP_OK && netif_ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(netif_ret, TAG, "netif init failed");
        }
        s_netif_ready = true;
    }

    if (!s_event_loop_ready) {
        esp_err_t loop_ret = esp_event_loop_create_default();
        if (loop_ret != ESP_OK && loop_ret != ESP_ERR_INVALID_STATE) {
            ESP_RETURN_ON_ERROR(loop_ret, TAG, "event loop init failed");
        }
        s_event_loop_ready = true;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    }

    char ssid[32];
    char pass[64];
    generate_ap_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ssid);
    strlcpy((char *)ap_config.ap.password, pass, sizeof(ap_config.ap.password));
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    if (strlen(pass) < 8) {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG,
                            "set storage failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG,
                            "set config failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_wifi_started = true;
    }

    ESP_LOGW(TAG, "No WiFi creds. SoftAP started");
    ESP_LOGI(TAG, "SoftAP SSID: %s", ssid);
    ESP_LOGI(TAG, "SoftAP password: %s", pass);

    if (s_ap_netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
            ESP_LOGI(TAG, "Open http://" IPSTR "/", IP2STR(&ip_info.ip));
        }
    }

    return ESP_OK;
}

static esp_err_t spiffs_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0;
    size_t used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS info failed: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "SPIFFS total=%u used=%u", (unsigned)total, (unsigned)used);
    }

    return ESP_OK;
}

static esp_err_t send_spiffs_file(httpd_req_t *req, const char *path, const char *type)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, type);
    char chunk[1024];
    size_t read_bytes = 0;
    while ((read_bytes = fread(chunk, 1, sizeof(chunk), file)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            fclose(file);
            httpd_resp_sendstr_chunk(req, NULL);
            return ESP_FAIL;
        }
    }

    fclose(file);
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}

static esp_err_t handle_root_get(httpd_req_t *req)
{
    return send_spiffs_file(req, "/spiffs/index.html", "text/html");
}

static esp_err_t handle_app_js(httpd_req_t *req)
{
    return send_spiffs_file(req, "/spiffs/app.js", "application/javascript");
}

static esp_err_t handle_styles_css(httpd_req_t *req)
{
    return send_spiffs_file(req, "/spiffs/styles.css", "text/css");
}

static esp_err_t handle_image_post(httpd_req_t *req)
{
    if (req->content_len != (int)s_expected_size) {
        ESP_LOGW(TAG, "Invalid size: %d (expected %u)", req->content_len,
                 (unsigned)s_expected_size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    FILE *file = fopen("/spiffs/image.sp6", "wb");
    if (!file) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open file");
        return ESP_FAIL;
    }

    uint8_t *buffer = malloc(s_expected_size);
    if (!buffer) {
        fclose(file);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < s_expected_size) {
        int chunk = httpd_req_recv(req, (char *)buffer + received,
                                   s_expected_size - received);
        if (chunk <= 0) {
            free(buffer);
            fclose(file);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        if (fwrite(buffer + received, 1, chunk, file) != (size_t)chunk) {
            free(buffer);
            fclose(file);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
            return ESP_FAIL;
        }
        received += (size_t)chunk;
    }

    fclose(file);
    ESP_LOGI(TAG, "Image received (%u bytes)", (unsigned)received);

    if (s_handler) {
        s_handler(buffer, received);
    }

    free(buffer);
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &root);

    httpd_uri_t app_js = {
        .uri = "/app.js",
        .method = HTTP_GET,
        .handler = handle_app_js,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &app_js);

    httpd_uri_t styles_css = {
        .uri = "/styles.css",
        .method = HTTP_GET,
        .handler = handle_styles_css,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &styles_css);

    httpd_uri_t image = {
        .uri = "/image",
        .method = HTTP_POST,
        .handler = handle_image_post,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &image);

    return server;
}

void image_upload_start(image_upload_handler_t handler, size_t expected_size)
{
    s_handler = handler;
    s_expected_size = expected_size;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(spiffs_init());

    wifi_config_t sta_config;
    bool has_sta = build_sta_config(&sta_config);

    esp_err_t wifi_ret = has_sta ? wifi_init_sta(&sta_config) : wifi_init_ap();
    if (wifi_ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed: %s", esp_err_to_name(wifi_ret));
        return;
    }

    start_webserver();
    ESP_LOGI(TAG, "HTTP server ready: POST /image");
}
