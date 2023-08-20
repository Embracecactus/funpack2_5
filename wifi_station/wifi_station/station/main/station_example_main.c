
/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#include "esp_http_client.h"
#include "lv_port.h"

// 移植wifi
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/sys.h"
//

// http
#include "esp_http_client.h"
#include "cJSON.h"

// 声音
#include "esp_tts.h"
#include "esp_tts_voice_xiaole.h"
#include "esp_tts_voice_template.h"
#include "esp_tts_player.h"
#include "ringbuf.h"

#include "esp_partition.h"
#include "esp_idf_version.h"


#include "bsp_board.h"
#include "driver/i2s.h"

#include "bsp_btn.h"


#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 2048
char output_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0}; // 用于接收通过http协议返回的数据

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
//

// wifi



/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1


// cjson数据解析
/**
 * @brief 心知天气（seniverse） 天气各项数据结构体
 */
typedef struct
{
    char *date;
    char *text_day;
    char *code_day;
    char *text_night;
    char *code_night;
    char *high;
    char *low;
    char *rainfall;
    char *precip;
    char *wind_direction;
    char *wind_direction_degree;
    char *wind_speed;
    char *wind_scale;
    char *humidity;

} user_seniverse_day_config_t;

/**
 * @brief 心知天气（seniverse） 数据结构体
 */
typedef struct
{
    char *id;
    char *name;
    char *country;
    char *path;
    char *timezone;
    char *timezone_offset;
    user_seniverse_day_config_t day_config;
    char *last_update;

} user_seniverse_config_t;

// 解析cjson的全局变量
cJSON *root = NULL;
cJSON *cjson_item = NULL;
cJSON *cjson_results = NULL;
cJSON *cjson_now = NULL;
cJSON *cjson_temperature = NULL;
cJSON *cjson_weather = NULL;

#include "bsp_wifi.h"

// //声音播放
// ///
// //移植
static int s_play_sample_rate = 16000;
static int s_play_channel_format = 1;
static int s_bits_per_chan = 16;

// //移植于bsp_board.c
// /**
//  * @brief Codec device handle
//  */
// typedef void *esp_codec_dev_handle_t;
// //static esp_codec_dev_handle_t play_dev = NULL;

// i2s play
int iot_dac_audio_play(const uint8_t *data, int length, TickType_t ticks_to_wait)
{
    size_t bytes_write = 0;
    i2s_write(I2S_NUM_0, (const char *)data, length, &bytes_write, ticks_to_wait);
    return ESP_OK;
}

esp_err_t bsp_audio_play(const int16_t *data, int length, TickType_t ticks_to_wait)
{
    size_t bytes_write = 0;
    esp_err_t ret = ESP_OK;

    int out_length = length;
    int audio_time = 1;
    audio_time *= (16000 / s_play_sample_rate);
    audio_time *= (2 / s_play_channel_format);

    int *data_out = NULL;
    if (s_bits_per_chan != 32)
    {
        out_length = length * 2;
        data_out = malloc(out_length);
        for (int i = 0; i < length / sizeof(int16_t); i++)
        {
            int ret = data[i];
            data_out[i] = ret << 16;
        }
    }

    int *data_out_1 = NULL;
    if (s_play_channel_format != 2 || s_play_sample_rate != 16000)
    {
        out_length *= audio_time;
        data_out_1 = malloc(out_length);
        int *tmp_data = NULL;
        if (data_out != NULL)
        {
            tmp_data = data_out;
        }
        else
        {
            tmp_data = data;
        }

        for (int i = 0; i < out_length / (audio_time * sizeof(int)); i++)
        {
            for (int j = 0; j < audio_time; j++)
            {
                data_out_1[audio_time * i + j] = tmp_data[i];
            }
        }
        if (data_out != NULL)
        {
            free(data_out);
            data_out = NULL;
        }
    }

    if (data_out != NULL)
    {
        ret = iot_dac_audio_play((void *)data_out, out_length, portMAX_DELAY);
        free(data_out);
    }
    else if (data_out_1 != NULL)
    {
        ret = iot_dac_audio_play((void *)data_out_1, out_length, portMAX_DELAY);
        free(data_out_1);
    }
    else
    {
        ret = iot_dac_audio_play((void *)data, length, portMAX_DELAY);
    }

    return ret;
}


static void btn_test_task(void *pvParameters)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "voice_data");
    if (part == NULL)
    {
        printf("Couldn't find voice data partition!\n");
        return 0;
    }
    else
    {
        printf("voice_data paration size:%d\n", part->size);
    }
    void *voicedata;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_partition_mmap_handle_t mmap;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &voicedata, &mmap);
#else
    spi_flash_mmap_handle_t mmap;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, SPI_FLASH_MMAP_DATA, &voicedata, &mmap);
#endif
    if (err != ESP_OK)
    {
        printf("Couldn't map voice data partition!\n");
        return 0;
    }
    esp_tts_voice_t *voice = esp_tts_voice_set_init(&esp_tts_voice_template, (int16_t *)voicedata);

    esp_tts_handle_t *tts_handle = esp_tts_create(voice);

    while (1)
    {

        cJSON *root = NULL;
        root = cJSON_Parse(output_buffer);

        cJSON *cjson_item = cJSON_GetObjectItem(root, "results");
        cJSON *cjson_results = cJSON_GetArrayItem(cjson_item, 0);
        cJSON *cjson_now = cJSON_GetObjectItem(cjson_results, "now");
        cJSON *cjson_temperature = cJSON_GetObjectItem(cjson_now, "temperature");
        cJSON *cjson_weather = cJSON_GetObjectItem(cjson_now, "text");
        if (bsp_btn_get_state(BOARD_BTN_ID_PREV))
        {
            /*** 2. play prompt text ***/
            char *prompt1 = "今天杭州的气温是";
            char *prompt2 = cjson_temperature->valuestring;
            char *prompt3 = "度";
            printf("%s\n", prompt1);
            if (esp_tts_parse_chinese(tts_handle, prompt1))
            {
                int len[1] = {0};
                do
                {
                    // 速度设置为0
                    short *pcm_data = esp_tts_stream_play(tts_handle, len, 0);
#ifdef SDCARD_OUTPUT_ENABLE
                    wav_encoder_run(wav_encoder, pcm_data, len[0] * 2);
#else
                    iot_dac_audio_play(pcm_data, len[0] * 2, portMAX_DELAY);
#endif
                } while (len[0] > 0);
                i2s_zero_dma_buffer(0);
            }
            esp_tts_stream_reset(tts_handle);
#ifdef SDCARD_OUTPUT_ENABLE
            wav_encoder_close(wav_encoder);
#endif
            if (esp_tts_parse_chinese(tts_handle, prompt2))
            {
                int len[1] = {0};
                do
                {
                    // 速度设置为0
                    short *pcm_data = esp_tts_stream_play(tts_handle, len, 0);
#ifdef SDCARD_OUTPUT_ENABLE
                    wav_encoder_run(wav_encoder, pcm_data, len[0] * 2);
#else
                    iot_dac_audio_play(pcm_data, len[0] * 2, portMAX_DELAY);
#endif
                    // printf("data:%d \n", len[0]);
                } while (len[0] > 0);
                i2s_zero_dma_buffer(0);
            }
            esp_tts_stream_reset(tts_handle);
#ifdef SDCARD_OUTPUT_ENABLE
            wav_encoder_close(wav_encoder);
#endif
            if (esp_tts_parse_chinese(tts_handle, prompt3))
            {
                int len[1] = {0};
                do
                {
                    // 速度设置为0
                    short *pcm_data = esp_tts_stream_play(tts_handle, len, 0);
#ifdef SDCARD_OUTPUT_ENABLE
                    wav_encoder_run(wav_encoder, pcm_data, len[0] * 2);
#else
                    iot_dac_audio_play(pcm_data, len[0] * 2, portMAX_DELAY);
#endif
                    // printf("data:%d \n", len[0]);
                } while (len[0] > 0);
                i2s_zero_dma_buffer(0);
            }
            esp_tts_stream_reset(tts_handle);
#ifdef SDCARD_OUTPUT_ENABLE
            wav_encoder_close(wav_encoder);
#endif
        }
        else if (bsp_btn_get_state(BOARD_BTN_ID_ENTER))
        {
            /*** 2. play prompt text ***/
            char *prompt1 = "杭州今天的天气";
            char *prompt2 = cjson_weather->valuestring;
            printf("%s\n", prompt1);
            printf("%s\n", prompt2);
            if (esp_tts_parse_chinese(tts_handle, prompt1))
            {
                int len[1] = {0};
                do
                {
                    // 速度设置为0
                    short *pcm_data = esp_tts_stream_play(tts_handle, len, 0);
#ifdef SDCARD_OUTPUT_ENABLE
                    wav_encoder_run(wav_encoder, pcm_data, len[0] * 2);
#else
                    iot_dac_audio_play(pcm_data, len[0] * 2, portMAX_DELAY);
#endif
                } while (len[0] > 0);
                i2s_zero_dma_buffer(0);
            }
            esp_tts_stream_reset(tts_handle);
#ifdef SDCARD_OUTPUT_ENABLE
            wav_encoder_close(wav_encoder);
#endif
            /*** 2. play prompt text ***/
          
            // strcat(prompt1,cjson_weather->valuestring);
            printf("%s\n", prompt2);
            if (esp_tts_parse_chinese(tts_handle, prompt2))
            {
                int len[1] = {0};
                do
                {
                    // 速度设置为0
                    short *pcm_data = esp_tts_stream_play(tts_handle, len, 0);
#ifdef SDCARD_OUTPUT_ENABLE
                    wav_encoder_run(wav_encoder, pcm_data, len[0] * 2);
#else

                    iot_dac_audio_play(pcm_data, len[0] * 2, portMAX_DELAY);
#endif

                } while (len[0] > 0);
                i2s_zero_dma_buffer(0);
            }
            esp_tts_stream_reset(tts_handle);
#ifdef SDCARD_OUTPUT_ENABLE
            wav_encoder_close(wav_encoder);
#endif
        }
        else if (bsp_btn_get_state(BOARD_BTN_ID_NEXT))
        {
            /*** 2. play prompt text ***/
            char *prompt1 = "杭州今天天气";
            printf("%s\n", prompt1);
            if (esp_tts_parse_chinese(tts_handle, prompt1))
            {
                int len[1] = {0};
                do
                {
                    // 速度设置为0
                    short *pcm_data = esp_tts_stream_play(tts_handle, len, 0);
#ifdef SDCARD_OUTPUT_ENABLE
                    wav_encoder_run(wav_encoder, pcm_data, len[0] * 2);
#else

                    iot_dac_audio_play(pcm_data, len[0] * 2, portMAX_DELAY);
#endif
                    // printf("data:%d \n", len[0]);
                } while (len[0] > 0);
                i2s_zero_dma_buffer(0);
            }
            esp_tts_stream_reset(tts_handle);
#ifdef SDCARD_OUTPUT_ENABLE
            wav_encoder_close(wav_encoder);
#endif
        }
        vTaskDelay(1);
    }
}


// /**
//  *  @brief 该代码要实现连接wifi，并且从心知天气获取天气和温度的代码
//  *
//  **/

static void http_test_task(void *pvParameters)
{

    // 02-1 定义需要的变量

    int content_length = 0; // http协议头的长度

    // 02-2 配置http结构体

    // 定义http配置结构体，并且进行清零
    esp_http_client_config_t config;
    memset(&config, 0, sizeof(config));

    // 向配置结构体内部写入url
    static const char *URL = "http://api.seniverse.com/v3/weather/now.json?key=Sd1BdVJsfUOpncfuh&location=hangzhou&language=zh-Hans&unit=c";
    // static const char *URL = " http://api.seniverse.com/v3/weather/daily.json?key=Sd1BdVJsfUOpncfuh&location=hangzhou&language=zh-Hans&unit=c&start=0&days=5";
    config.url = URL;

    // 初始化结构体
    esp_http_client_handle_t client = esp_http_client_init(&config); // 初始化http连接

    // 设置发送请求
    esp_http_client_set_method(client, HTTP_METHOD_GET);

    // 02-3 循环通讯
    while (1)
    {
        // 与目标主机创建连接，并且声明写入内容长度为0
        esp_err_t err = esp_http_client_open(client, 0);

        // 如果连接失败
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
            // return false;
        }
        // 如果连接成功
        else
        {

            // 读取目标主机的返回内容的协议头
            content_length = esp_http_client_fetch_headers(client);

            // 如果协议头长度小于0，说明没有成功读取到
            if (content_length < 0)
            {
                ESP_LOGE(TAG, "HTTP client fetch headers failed");
                // return false;
            }

            // 如果成功读取到了协议头
            else
            {

                // 读取目标主机通过http的响应内容
                int data_read = esp_http_client_read_response(client, output_buffer, MAX_HTTP_OUTPUT_BUFFER);
                if (data_read >= 0)
                {

                    // 打印响应内容，包括响应状态，响应体长度及其内容
                    ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                             esp_http_client_get_status_code(client),     // 获取响应状态信息
                             esp_http_client_get_content_length(client)); // 获取响应信息长度
                    printf("data:%s\n", output_buffer);
                }
                // 如果不成功
                else
                {
                    ESP_LOGE(TAG, "Failed to read response");
                    // return false;
                }
            }
        }

        // 关闭连接
        esp_http_client_close(client);
        // return true;
        //;

        // 延时，因为心知天气免费版本每分钟只能获取20次数据
        vTaskDelay(3000 / portTICK_PERIOD_MS);
    }
}

/* Root cert for howsmyssl.com, taken from howsmyssl_com_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const char howsmyssl_com_root_cert_pem_start[] asm("_binary_howsmyssl_com_root_cert_pem_start");
extern const char howsmyssl_com_root_cert_pem_end[] asm("_binary_howsmyssl_com_root_cert_pem_end");

extern const char postman_root_cert_pem_start[] asm("_binary_postman_root_cert_pem_start");
extern const char postman_root_cert_pem_end[] asm("_binary_postman_root_cert_pem_end");

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer; // Buffer to store response of http request from event handler
    static int output_len;      // Stores number of bytes read
    switch (evt->event_id)
    {
    case HTTP_EVENT_ERROR:
        ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        /*
         *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
         *  However, event handler can also be used in case chunked encoding is used.
         */
        if (!esp_http_client_is_chunked_response(evt->client))
        {
            // If user_data buffer is configured, copy the response into the buffer
            if (evt->user_data)
            {
                memcpy(evt->user_data + output_len, evt->data, evt->data_len);
            }
            else
            {
                if (output_buffer == NULL)
                {
                    output_buffer = (char *)malloc(esp_http_client_get_content_length(evt->client));
                    output_len = 0;
                    if (output_buffer == NULL)
                    {
                        ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                        return ESP_FAIL;
                    }
                }
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
            }
            output_len += evt->data_len;
        }

        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
        if (output_buffer != NULL)
        {
            // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
            // ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
            free(output_buffer);
            output_buffer = NULL;
        }
        output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
        if (err != 0)
        {
            if (output_buffer != NULL)
            {
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
            ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
        }
        break;
    }
    return ESP_OK;
}

static void http_rest_with_url(void)
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    /**
     * NOTE: All the configuration parameters for http_client must be spefied either in URL or as host and path parameters.
     * If host and path parameters are not set, query parameter will be ignored. In such cases,
     * query parameter should be specified in URL.
     *
     * If URL as well as host and path parameters are specified, values of host and path will be considered.
     */
    esp_http_client_config_t config = {
        .host = "httpbin.org",
        .path = "/get",
        .query = "esp",
        .event_handler = _http_event_handler,
        .user_data = local_response_buffer, // Pass address of local buffer to get response
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // GET
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    ESP_LOG_BUFFER_HEX(TAG, local_response_buffer, strlen(local_response_buffer));

    // POST
    const char *post_data = "{\"field1\":\"value1\"}";
    esp_http_client_set_url(client, "http://httpbin.org/post");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    // PUT
    esp_http_client_set_url(client, "http://httpbin.org/put");
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP PUT Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP PUT request failed: %s", esp_err_to_name(err));
    }

    // PATCH
    esp_http_client_set_url(client, "http://httpbin.org/patch");
    esp_http_client_set_method(client, HTTP_METHOD_PATCH);
    esp_http_client_set_post_field(client, NULL, 0);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP PATCH Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP PATCH request failed: %s", esp_err_to_name(err));
    }

    // DELETE
    esp_http_client_set_url(client, "http://httpbin.org/delete");
    esp_http_client_set_method(client, HTTP_METHOD_DELETE);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP DELETE Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP DELETE request failed: %s", esp_err_to_name(err));
    }

    // HEAD
    esp_http_client_set_url(client, "http://httpbin.org/get");
    esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP HEAD Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP HEAD request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

static void http_rest_with_hostname_path(void)
{
    esp_http_client_config_t config = {
        .host = "httpbin.org",
        .path = "/get",
        .transport_type = HTTP_TRANSPORT_OVER_TCP,
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // GET
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    // POST
    const char *post_data = "field1=value1&field2=value2";
    esp_http_client_set_url(client, "/post");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
    }

    // PUT
    esp_http_client_set_url(client, "/put");
    esp_http_client_set_method(client, HTTP_METHOD_PUT);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP PUT Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP PUT request failed: %s", esp_err_to_name(err));
    }

    // PATCH
    esp_http_client_set_url(client, "/patch");
    esp_http_client_set_method(client, HTTP_METHOD_PATCH);
    esp_http_client_set_post_field(client, NULL, 0);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP PATCH Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP PATCH request failed: %s", esp_err_to_name(err));
    }

    // DELETE
    esp_http_client_set_url(client, "/delete");
    esp_http_client_set_method(client, HTTP_METHOD_DELETE);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP DELETE Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP DELETE request failed: %s", esp_err_to_name(err));
    }

    // HEAD
    esp_http_client_set_url(client, "/get");
    esp_http_client_set_method(client, HTTP_METHOD_HEAD);
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP HEAD Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP HEAD request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}

#if CONFIG_ESP_HTTP_CLIENT_ENABLE_BASIC_AUTH
static void http_auth_basic(void)
{
    /**
     * Note: `max_authorization_retries` in esp_http_client_config_t
     * can be used to configure number of retry attempts to be performed
     * in case unauthorized status code is received.
     *
     * To disable authorization retries, set max_authorization_retries to -1.
     */
    esp_http_client_config_t config = {
        .url = "http://user:passwd@httpbin.org/basic-auth/user/passwd",
        .event_handler = _http_event_handler,
        .auth_type = HTTP_AUTH_TYPE_BASIC,
        .max_authorization_retries = -1,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Basic Auth Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void http_auth_basic_redirect(void)
{
    esp_http_client_config_t config = {
        .url = "http://user:passwd@httpbin.org/basic-auth/user/passwd",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Basic Auth redirect Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}
#endif

#if CONFIG_ESP_HTTP_CLIENT_ENABLE_DIGEST_AUTH
static void http_auth_digest(void)
{
    esp_http_client_config_t config = {
        .url = "http://user:passwd@httpbin.org/digest-auth/auth/user/passwd/MD5/never",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Digest Auth Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}
#endif

static void https_with_url(void)
{
    esp_http_client_config_t config = {
        .url = "https://www.howsmyssl.com",
        .event_handler = _http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void https_with_hostname_path(void)
{
    esp_http_client_config_t config = {
        .host = "www.howsmyssl.com",
        .path = "/",
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .event_handler = _http_event_handler,
        .cert_pem = howsmyssl_com_root_cert_pem_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void http_relative_redirect(void)
{
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/relative-redirect/3",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Relative path redirect Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void http_absolute_redirect(void)
{
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/absolute-redirect/3",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Absolute path redirect Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void http_redirect_to_https(void)
{
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/redirect-to?url=https%3A%2F%2Fwww.howsmyssl.com",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP redirect to HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void http_download_chunk(void)
{
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/stream-bytes/8912",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP chunk encoding Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void http_perform_as_stream_reader(void)
{
    char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
    if (buffer == NULL)
    {
        ESP_LOGE(TAG, "Cannot malloc http receive buffer");
        return;
    }
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/get",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;
    if ((err = esp_http_client_open(client, 0)) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        free(buffer);
        return;
    }
    int content_length = esp_http_client_fetch_headers(client);
    int total_read_len = 0, read_len;
    if (total_read_len < content_length && content_length <= MAX_HTTP_RECV_BUFFER)
    {
        read_len = esp_http_client_read(client, buffer, content_length);
        if (read_len <= 0)
        {
            ESP_LOGE(TAG, "Error read data");
        }
        buffer[read_len] = 0;
        ESP_LOGD(TAG, "read_len = %d", read_len);
    }
    ESP_LOGI(TAG, "HTTP Stream reader Status = %d, content_length = %d",
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);
}

static void https_async(void)
{
    esp_http_client_config_t config = {
        .url = "https://postman-echo.com/post",
        .event_handler = _http_event_handler,
        .cert_pem = postman_root_cert_pem_start,
        .is_async = true,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err;
    const char *post_data = "Using a Palantír requires a person with great strength of will and wisdom. The Palantíri were meant to "
                            "be used by the Dúnedain to communicate throughout the Realms in Exile. During the War of the Ring, "
                            "the Palantíri were used by many individuals. Sauron used the Ithil-stone to take advantage of the users "
                            "of the other two stones, the Orthanc-stone and Anor-stone, but was also susceptible to deception himself.";
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));
    while (1)
    {
        err = esp_http_client_perform(client);
        if (err != ESP_ERR_HTTP_EAGAIN)
        {
            break;
        }
    }
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

static void https_with_invalid_url(void)
{
    esp_http_client_config_t config = {
        .url = "https://not.existent.url",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}

/*
 *  http_native_request() demonstrates use of low level APIs to connect to a server,
 *  make a http request and read response. Event handler is not used in this case.
 *  Note: This approach should only be used in case use of low level APIs is required.
 *  The easiest way is to use esp_http_perform()
 */
static void http_native_request(void)
{
    char output_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0}; // Buffer to store response of http request
    int content_length = 0;
    esp_http_client_config_t config = {
        .url = "http://httpbin.org/get",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // GET Request
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    }
    else
    {
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0)
        {
            ESP_LOGE(TAG, "HTTP client fetch headers failed");
        }
        else
        {
            int data_read = esp_http_client_read_response(client, output_buffer, MAX_HTTP_OUTPUT_BUFFER);
            if (data_read >= 0)
            {
                ESP_LOGI(TAG, "HTTP GET Status = %d, content_length = %d",
                         esp_http_client_get_status_code(client),
                         esp_http_client_get_content_length(client));
                ESP_LOG_BUFFER_HEX(TAG, output_buffer, data_read);
            }
            else
            {
                ESP_LOGE(TAG, "Failed to read response");
            }
        }
    }
    esp_http_client_close(client);

    // POST Request
    const char *post_data = "{\"field1\":\"value1\"}";
    esp_http_client_set_url(client, "http://httpbin.org/post");
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    err = esp_http_client_open(client, strlen(post_data));
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
    }
    else
    {
        int wlen = esp_http_client_write(client, post_data, strlen(post_data));
        if (wlen < 0)
        {
            ESP_LOGE(TAG, "Write failed");
        }
        content_length = esp_http_client_fetch_headers(client);
        if (content_length < 0)
        {
            ESP_LOGE(TAG, "HTTP client fetch headers failed");
        }
        else
        {
            int data_read = esp_http_client_read_response(client, output_buffer, MAX_HTTP_OUTPUT_BUFFER);
            if (data_read >= 0)
            {
                ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %d",
                         esp_http_client_get_status_code(client),
                         esp_http_client_get_content_length(client));
                ESP_LOG_BUFFER_HEX(TAG, output_buffer, strlen(output_buffer));
            }
            else
            {
                ESP_LOGE(TAG, "Failed to read response");
            }
        }
    }
    esp_http_client_cleanup(client);
}

static void http_partial_download(void)
{
    esp_http_client_config_t config = {
        .url = "http://jigsaw.w3.org/HTTP/TE/foo.txt",
        .event_handler = _http_event_handler,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    // Download a file excluding first 10 bytes
    esp_http_client_set_header(client, "Range", "bytes=10-");
    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    // Download last 10 bytes of a file
    esp_http_client_set_header(client, "Range", "bytes=-10");
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    // Download 10 bytes from 11 to 20
    esp_http_client_set_header(client, "Range", "bytes=11-20");
    err = esp_http_client_perform(client);
    if (err == ESP_OK)
    {
        ESP_LOGI(TAG, "HTTP Status = %d, content_length = %d",
                 esp_http_client_get_status_code(client),
                 esp_http_client_get_content_length(client));
    }
    else
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(bsp_board_init());
    ESP_ERROR_CHECK(bsp_board_power_ctrl(POWER_MODULE_AUDIO, true));
    bsp_btn_init_default();
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");

    wifi_init_sta();

    bsp_codec_set_voice_volume(100);
   
    // 使用多线程
    xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);
    xTaskCreate(&btn_test_task, "btn_test_task", 8192, NULL, 5, NULL);
     
      
           
}
