/* ESP32 BiJin ToKei

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdio.h>
#include <string.h>
#include <sys/unistd.h>
#include <sys/stat.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include <time.h>
#include <errno.h>
#include <sys/fcntl.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "tftspi.h"
#include "tft.h"

#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include <sys/time.h>
#include <unistd.h>
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "apps/sntp/sntp.h"
#include "nvs_flash.h"

// ==========================================================
// Define which spi bus to use TFT_VSPI_HOST or TFT_HSPI_HOST
#define SPI_BUS TFT_VSPI_HOST
// ==========================================================

#define SPIFFS_BASE_PATH "/spiffs"
#define TIMEZONE 8 // hour offset
#define TIMEADJ 30 // seconds advanced real time for prefetch and load image

#define MARGIN_X 10
#define MARGIN_Y 10

#define NTP_WAIT 20 // seconds to wait NTP return
#define NTP_RETRY 3

// ==========================================================
// rotate cache files to avoid always write to same block
// assume each jpg not over 600k, 3M SPIFFS can fit 5 cache
#define CACHE_COUNT 5
// ==========================================================

static struct tm *tm_info; // time data
static char tmp_buf[64];	 // buffer for formating TFT display string
static time_t time_now;
static int last_show_minute = -1; // for checking if current minute picture already downloaded and shown
static int cache_i = 1;

//==================================================================================
static const char tag[] = "[BiJin ToKei]";

/* FreeRTOS event group to signal when we are connected & ready to make a request */
static EventGroupHandle_t wifi_event_group;

/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */
const int CONNECTED_BIT = 0x00000001;

// ==========================================================
// The web server that getting the time picture,
// this is one of the famous site, you may search
// more on the web.
#define WEB_SERVER "www.bijint.com"
#define WEB_PORT 80
static const char *REQUEST_FORMAT =
		// "GET http://" WEB_SERVER "/assets/toppict/jp/t1/%.2d%.2d.jpg HTTP/1.0\r\n"
		// "GET http://" WEB_SERVER "/assets/pict/jp/pc/%.2d%.2d.jpg HTTP/1.0\r\n"
		"GET http://" WEB_SERVER "/assets/pict/nagoya/pc/%.2d%.2d.jpg HTTP/1.0\r\n"
		"Host: " WEB_SERVER "\r\n"
		"User-Agent: esp-idf/1.0 esp32\r\n"
		"\r\n";
// ==========================================================

static void refresh_tm_info()
{
	time(&time_now);
	time_now += (TIMEZONE * 60 * 60) + TIMEADJ;
	tm_info = gmtime(&time_now);
}

//------------------------------------------------------------
static esp_err_t event_handler(void *ctx, system_event_t *event)
{
	switch (event->event_id)
	{
	case SYSTEM_EVENT_STA_START:
		esp_wifi_connect();
		break;
	case SYSTEM_EVENT_STA_GOT_IP:
		xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
		break;
	case SYSTEM_EVENT_STA_DISCONNECTED:
		/* This is a workaround as ESP32 WiFi libs don't currently
           auto-reassociate. */
		esp_wifi_connect();
		xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
		break;
	default:
		break;
	}
	return ESP_OK;
}

//-------------------------------
static void initialise_wifi(void)
{
	tcpip_adapter_init();
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi_config = {
			.sta = {
					.ssid = CONFIG_WIFI_SSID,
					.password = CONFIG_WIFI_PASSWORD,
			},
	};

	ESP_LOGI(tag, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
	sprintf(tmp_buf, "Setting WiFi configuration SSID %s...", wifi_config.sta.ssid);
	_fg = TFT_YELLOW;
	TFT_print(tmp_buf, MARGIN_X, LASTY + TFT_getfontheight() + 2);

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
}

static void http_get_task()
{
	const struct addrinfo hints = {
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res;
	struct in_addr *addr;
	int s, r;
	char recv_buf[512]; // assume response header must smaller than buffer size

	while (1) // loop forever
	{
		refresh_tm_info();
		int curr_hour = tm_info->tm_hour;
		int curr_minute = tm_info->tm_min;

		// check valid time and minute changed
		if ((curr_hour >= 0) && (curr_hour < 24) && (curr_minute >= 0) && (curr_minute < 60) && curr_minute != last_show_minute)
		{
			char req_buf[128];

			int str_len = sprintf(req_buf, REQUEST_FORMAT, curr_hour, curr_minute);

			ESP_LOGI(tag, "%s", req_buf);

			/* Wait for the callback to set the CONNECTED_BIT in the
           event group.
        */
			xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT,
													false, true, portMAX_DELAY);
			ESP_LOGI(tag, "Connected to AP");

			int err = getaddrinfo(WEB_SERVER, "80", &hints, &res);

			if (err != 0 || res == NULL)
			{
				ESP_LOGE(tag, "DNS lookup failed err=%d res=%p", err, res);
				ESP_LOGE(tag, "Restart and try again");
				esp_restart();
			}

			/* Code to print the resolved IP.
           Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
			addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
			ESP_LOGI(tag, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

			s = socket(res->ai_family, res->ai_socktype, 0);
			if (s < 0)
			{
				ESP_LOGE(tag, "... Failed to allocate socket.");
				ESP_LOGE(tag, "Restart and try again");
				esp_restart();
			}
			ESP_LOGI(tag, "... allocated socket");

			if (connect(s, res->ai_addr, res->ai_addrlen) != 0)
			{
				ESP_LOGE(tag, "... socket connect failed errno=%d", errno);
				ESP_LOGE(tag, "Restart and try again");
				esp_restart();
			}

			ESP_LOGI(tag, "... connected");
			freeaddrinfo(res);

			if (write(s, req_buf, str_len) < 0)
			{
				ESP_LOGE(tag, "... socket send failed");
				ESP_LOGE(tag, "Restart and try again");
				esp_restart();
			}
			ESP_LOGI(tag, "... socket send success");

			struct timeval receiving_timeout;
			receiving_timeout.tv_sec = 5;
			receiving_timeout.tv_usec = 0;
			if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
										 sizeof(receiving_timeout)) < 0)
			{
				ESP_LOGE(tag, "... failed to set socket receiving timeout");
				ESP_LOGE(tag, "Restart and try again");
				esp_restart();
			}
			ESP_LOGI(tag, "... set socket receiving timeout success");

			char filename_buf[20];
			sprintf(filename_buf, SPIFFS_BASE_PATH "/cache%d.jpg", cache_i++);
			ESP_LOGI(tag, "... cache file: %s", filename_buf);
			if (cache_i > CACHE_COUNT)
			{
				cache_i = 1;
			}

			FILE *f = fopen(filename_buf, "w");
			if (f == NULL)
			{
				ESP_LOGE(tag, "Failed to open file for writing");
				return;
			}

			/* Read HTTP response */
			r = read(s, recv_buf, sizeof(recv_buf));
			int offset = -1;
			for (int i = 0; i < r; i++)
			{
				putchar(recv_buf[i]); // output header for debug only
				// search response header end, double carriage return
				if ((recv_buf[i] == '\r') && (recv_buf[i + 1] == '\n') && (recv_buf[i + 2] == '\r') && (recv_buf[i + 3] == '\n'))
				{
					offset = i + 4;
				}
			}
			int file_size = 0;
			// if found some response content at first buffer, start write from offset, otherwise start write from second buffer
			if ((offset > 0) && (offset < r))
			{
				ESP_LOGI(tag, "Found response content offset: %d", offset);
				// write response content (jpg file)
				file_size += fwrite(recv_buf + offset, 1, r - offset, f);
			}

			do
			{
				r = read(s, recv_buf, sizeof(recv_buf));
				file_size += fwrite(recv_buf, 1, r, f);
			} while (r > 0);
			fclose(f);
			ESP_LOGI(tag, "File written: %d", file_size);
			close(s);
			ESP_LOGI(tag, "freemem=%d", esp_get_free_heap_size()); // show free heap for debug only
			vTaskDelay(4000 / portTICK_PERIOD_MS); // wait spiffs cache write finish

			// clear screen and show current, in case cannot display the jpg
			TFT_fillRect(0, 0, _width, _height, TFT_BLACK);
			sprintf(tmp_buf, "%d-%.2d-%.2d %.2d:%.2d:%.2d", (tm_info->tm_year + 1900), (tm_info->tm_mon + 1), tm_info->tm_mday, tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
			TFT_print(tmp_buf, MARGIN_X, CENTER);

			// display image in 1/2 size
			TFT_jpg_image(CENTER, CENTER, 1, filename_buf, NULL, 0);

			last_show_minute = curr_minute;
		}

		vTaskDelay(1000 / portTICK_PERIOD_MS);
	}
}

//--------------------------
static int obtain_time(void)
{
	int res = 1;
	int retry = 0;
	initialise_wifi();
	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);

	_fg = TFT_GREEN;
	TFT_print("Getting time over NTP", MARGIN_X, LASTY + TFT_getfontheight() + 2);

	ESP_LOGI(tag, "Initializing SNTP");
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, "pool.ntp.org");
	sntp_init();

	// wait for time to be set
	int wait = 0;

	refresh_tm_info();

	while (tm_info->tm_year < (2016 - 1900) && retry < NTP_RETRY)
	{
		if (++wait >= NTP_WAIT)
		{
			retry++;
			wait = 0;
			sntp_init();
			sprintf(tmp_buf, "Retry %0d/%d", retry, NTP_RETRY);
			_fg = TFT_RED;
			TFT_print(tmp_buf, MARGIN_X, LASTY + TFT_getfontheight() + 2);
		}
		vTaskDelay(1000 / portTICK_RATE_MS);
		refresh_tm_info();
	}
	if (tm_info->tm_year < (2016 - 1900))
	{
		ESP_LOGI(tag, "System time NOT set.");
		res = 0;
	}
	else
	{
		ESP_LOGI(tag, "System time is set.");
	}

	return res;
}

//=============
void app_main()
{
	// ========  PREPARE DISPLAY INITIALIZATION  =========

	// === SET GLOBAL VARIABLES ==========================

	// ===================================================
	// ==== Set display type                         =====
	tft_disp_type = DEFAULT_DISP_TYPE;
	//tft_disp_type = DISP_TYPE_ILI9341;
	//tft_disp_type = DISP_TYPE_ILI9488;
	//tft_disp_type = DISP_TYPE_ST7735B;
	// ===================================================

	// ===================================================
	// === Set display resolution if NOT using default ===
	// === DEFAULT_TFT_DISPLAY_WIDTH &                 ===
	// === DEFAULT_TFT_DISPLAY_HEIGHT                  ===
	_width = DEFAULT_TFT_DISPLAY_WIDTH;		// smaller dimension
	_height = DEFAULT_TFT_DISPLAY_HEIGHT; // larger dimension
	//_width = 128;  // smaller dimension
	//_height = 160; // larger dimension
	// ===================================================

	// ===================================================
	// ==== Set maximum spi clock for display read    ====
	//      operations, function 'find_rd_speed()'    ====
	//      can be used after display initialization  ====
	max_rdclock = 8000000;
	// ===================================================

	// ====================================================================
	// === Pins MUST be initialized before SPI interface initialization ===
	// ====================================================================
	TFT_PinsInit();

	// ====  CONFIGURE SPI DEVICES(s)  ====================================================================================

	spi_lobo_device_handle_t spi;

	spi_lobo_bus_config_t buscfg = {
			.miso_io_num = PIN_NUM_MISO, // set SPI MISO pin
			.mosi_io_num = PIN_NUM_MOSI, // set SPI MOSI pin
			.sclk_io_num = PIN_NUM_CLK,	// set SPI CLK pin
			.quadwp_io_num = -1,
			.quadhd_io_num = -1,
			.max_transfer_sz = 6 * 1024,
	};
	spi_lobo_device_interface_config_t devcfg = {
			.clock_speed_hz = 8000000,				 // Initial clock out at 8 MHz
			.mode = 0,												 // SPI mode 0
			.spics_io_num = PIN_NUM_CS,				 // set SPI CS pin
			.flags = LB_SPI_DEVICE_HALFDUPLEX, // ALWAYS SET  to HALF DUPLEX MODE!! for display spi
	};

	// ====================================================================================================================

	vTaskDelay(500 / portTICK_RATE_MS);
	printf("\r\n==============================\r\n");
	printf("Pins used: miso=%d, mosi=%d, sck=%d, cs=%d\r\n", PIN_NUM_MISO, PIN_NUM_MOSI, PIN_NUM_CLK, PIN_NUM_CS);
	printf("==============================\r\n\r\n");

	// ==================================================================
	// ==== Initialize the SPI bus and attach the LCD to the SPI bus ====

	ESP_ERROR_CHECK(spi_lobo_bus_add_device(SPI_BUS, &buscfg, &devcfg, &spi));
	printf("SPI: display device added to spi bus (%d)\r\n", SPI_BUS);
	disp_spi = spi;

	// ==== Test select/deselect ====
	ESP_ERROR_CHECK(spi_lobo_device_select(spi, 1));
	ESP_ERROR_CHECK(spi_lobo_device_deselect(spi));

	printf("SPI: attached display device, speed=%u\r\n", spi_lobo_get_speed(spi));
	printf("SPI: bus uses native pins: %s\r\n", spi_lobo_uses_native_pins(spi) ? "true" : "false");

	// ================================
	// ==== Initialize the Display ====

	printf("SPI: display init...\r\n");
	TFT_display_init();
	printf("OK\r\n");

	// ---- Detect maximum read speed ----
	max_rdclock = find_rd_speed();
	printf("SPI: Max rd speed = %u\r\n", max_rdclock);

	// ==== Set SPI clock used for display operations ====
	spi_lobo_set_speed(spi, DEFAULT_SPI_CLOCK);
	printf("SPI: Changed speed to %u\r\n", spi_lobo_get_speed(spi));

	printf("\r\n---------------------\r\n");
	printf("Graphics demo started\r\n");
	printf("---------------------\r\n");

	font_rotate = 0;
	text_wrap = 0;
	font_transparent = 0;
	font_forceFixed = 0;
	image_debug = 0;

	TFT_setGammaCurve(DEFAULT_GAMMA_CURVE);
	TFT_setRotation(LANDSCAPE);
	TFT_setFont(DEFAULT_FONT, NULL);

	ESP_ERROR_CHECK(nvs_flash_init());

	_fg = TFT_ORANGE;
	TFT_print("BiJin ToKei", MARGIN_X, MARGIN_Y);
	if (obtain_time())
	{
		_fg = TFT_CYAN;
		refresh_tm_info();
		sprintf(tmp_buf, "System time is set: %d-%.2d-%.2d %.2d:%.2d:%.2d", (tm_info->tm_year + 1900), (tm_info->tm_mon + 1), tm_info->tm_mday, tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);
		TFT_print(tmp_buf, MARGIN_X, LASTY + TFT_getfontheight() + 2);
	}
	else
	{
		_fg = TFT_RED;
		TFT_print("ERROR.", MARGIN_X, LASTY + TFT_getfontheight() + 2);
		ESP_LOGE(tag, "Restart and try again");
		esp_restart();
	}
	vTaskDelay(2000 / portTICK_RATE_MS);

	_fg = TFT_BLUE;
	TFT_print("Initializing SPIFFS", MARGIN_X, LASTY + TFT_getfontheight() + 2);
	// ==== Initialize the file system ====
	printf("\r\n\n");

	esp_vfs_spiffs_conf_t conf = {
			.base_path = SPIFFS_BASE_PATH,
			.partition_label = NULL,
			.max_files = 5,
			.format_if_mount_failed = true};

	// Use settings defined above to initialize and mount SPIFFS filesystem.
	// Note: esp_vfs_spiffs_register is an all-in-one convenience function.
	ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

	vTaskDelay(2000 / portTICK_RATE_MS);

	_fg = TFT_MAGENTA;
	TFT_print("Start HTTP GET TASK", MARGIN_X, LASTY + TFT_getfontheight() + 2);

	xTaskCreate(&http_get_task, "http_get_task", 4096, NULL, 5, NULL);
}
