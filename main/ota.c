/*
 * OTA example
 * (https://github.com/espressif/esp-idf/tree/master/examples/system/ota/simple_ota_example)
 * (https://github.com/espressif/esp-idf/tree/master/examples/system/ota/advanced_https_ota_example)
 *
 * This example code is in the Public Domain (or CC0 licensed, at your option.)
 *
 * Unless required by applicable law or agreed to in writing, this
 * software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * */

/*
 * Adapted by Jean-Pierre Pourrez  (https://github.com/bazooka07/Ka-Radio32/tree/extensions)
 * 2020-02-04
 * */

/*
 * For switching OTA partition from command line, use with x=0 or x=1 :
 * python $IDF_PATH/components/app_update/otatool.py switch_otadata --slot x
 * */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// #include "freertos/event_groups.h"

#include "esp_system.h"
// #include "esp_wifi.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"

// #include "nvs.h"
// #include "nvs_flash.h"
#include "string.h"
#include "app_main.h"
#include "webclient.h"
#include "websocket.h"

static const char *TAG = "OTA";
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

#define OTA_BUFFSIZE 2048

static bool taskState = false;
static char ota_write_data[OTA_BUFFSIZE + 1] = {"\0"};

/*
void *inmalloc(size_t size) {
	void* ret = malloc(size);
	ESP_LOGV(TAG,"server Malloc of %x : %u bytes - Heap size: %u", (int)ret, size, xPortGetFreeHeapSize());
	return ret;
}

void infree(void *p) {
	ESP_LOGV(TAG,"server free of %x - Heap size: %u", (int)p, xPortGetFreeHeapSize());
	if (p != NULL) { free(p); }
}
 * */

void wsUpgrade(const char* status, int count, int total) {
	char message[70] = {"\0"};
	sprintf(message, "{\"upgrade\":{\"status\":\"%s\",\"count\":%d,\"total\":%d}}", status, count, total);
	websocketbroadcast(message, strlen(message));
}

int compVersions(const char * version1, const char * version2) {
    unsigned major1 = 0, minor1 = 0, bugfix1 = 0;
    unsigned major2 = 0, minor2 = 0, bugfix2 = 0;
    sscanf(version1, "%u.%u.%u", &major1, &minor1, &bugfix1);
    sscanf(version2, "%u.%u.%u", &major2, &minor2, &bugfix2);
    if (major1 != major2) return major1 - major2;
    if (minor1 < minor2) return minor1 - minor2;
    return bugfix1 - bugfix2;
}

static void http_cleanup(esp_http_client_handle_t client) {
	esp_http_client_close(client);
	esp_http_client_cleanup(client);
}

static void __attribute__((noreturn)) task_fatal_error() {
	ESP_LOGE(TAG, "Existing task due to fatal error");
	(void) vTaskDelete(NULL);
	while(true) {
		;
	}
}

/*
static void infinite_loop(void) {
	ESP_LOGI(TAG, "When a new firmware is available on the server, press the reset button !");
	int i = 0;
	while(true) {
		ESP_LOGI(TAG, "Waiting for a new firmware .... %3d", ++i);
		vTaskDelay(600000 / portTICK_PERIOD_MS);
	}
}
 * */

void otaTask(void *pvParams) {
#ifdef CONFIG_OTA_FIRMWARE_ACCOUNT
	ESP_LOGI(TAG, "Starting upgrading");

	/*
	char* ota_write_data = malloc(OTA_BUFFSIZE + 1);
	ESP_LOGV(TAG,"server Malloc of %x : %u bytes - Heap size: %u", (int)ota_write_data, (OTA_BUFFSIZE + 1), xPortGetFreeHeapSize());
	if(ota_write_data == NULL) {
		ESP_LOGE(TAG, "server failed ! Heap size: %d", xPortGetFreeHeapSize());
		vTaskDelay(50 / portTICK_PERIOD_MS);
		taskState = false;
		(void)vTaskDelete( NULL );
		return;
	}
	vTaskDelay(50 / portTICK_PERIOD_MS);
	* */

	char upgrade_url[128];
	sprintf(
		upgrade_url,
		"https://raw.githubusercontent.com/%s/%s/%s/build/%s.bin",
		CONFIG_OTA_FIRMWARE_ACCOUNT,
		CONFIG_OTA_FIRMWARE_REPOSITORY,
		CONFIG_OTA_FIRMWARE_BRANCH,
		(char *) pvParams // filename without extension
		);

	const esp_partition_t *configured = esp_ota_get_boot_partition();
	const esp_partition_t * running = esp_ota_get_running_partition();

	if(configured != running) {
		ESP_LOGW(TAG,
			"Configured OTA boot partition at offset 0x%08x, but running from offset 0x%08x",
			configured->address,
			running->address
		);
	}

	ESP_LOGI(TAG,
		"Running partition type %d subtype %d (offset 0x%08x)",
		running->type,
		running->subtype,
		running->address
	);

	// xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
	ESP_LOGI(TAG, "Connecting to server : %s", upgrade_url);

	esp_http_client_config_t config = {
		.url = upgrade_url,
		.cert_pem = (char *)server_cert_pem_start
	};
	esp_http_client_handle_t client = esp_http_client_init(&config);
	if(client == NULL) {
		ESP_LOGE(TAG, "Failed to initialise HTTP connection");
		task_fatal_error();
	}

	esp_err_t err = esp_http_client_open(client, 0);
	if(err != ESP_OK) {
		ESP_LOGE(TAG,
			"Failed to open HTTP connection  : %s",
			esp_err_to_name(err)
		);
		esp_http_client_cleanup(client);
		task_fatal_error();
	}

	esp_http_client_fetch_headers(client);
	esp_ota_handle_t update_handle = 0;
	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
	ESP_LOGI(TAG,
		"Writing to partition subtype %d at offset 0x%08x",
		update_partition->subtype,
		update_partition->address
	);

	assert(update_partition != NULL);

	TickType_t delay_loading = 30 / portTICK_PERIOD_MS;
	uint min_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
	esp_app_desc_t new_app_info;
	int binary_file_length = 0;
	bool image_header_was_checked = false;
	while(true) {
		int data_read = esp_http_client_read(client, ota_write_data, OTA_BUFFSIZE);
		if(data_read < 0) {
			ESP_LOGE(TAG, "Error SSL data read error");
			break;
		} else if(data_read > 0) {
			if(! image_header_was_checked) {
				if(data_read < min_size) {
					ESP_LOGE(TAG, "Received package of %u doesn't fit the minimal length (%u)", data_read, min_size);
					break;
				} else {
					memcpy(&new_app_info, &ota_write_data[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)], sizeof(esp_app_desc_t));
					ESP_LOGI(TAG, "New firmware version : %s", new_app_info.version);

					esp_app_desc_t app_info;
					if(esp_ota_get_partition_description(running, &app_info) == ESP_OK) {
						ESP_LOGI(TAG, "Running firmware version : %s", app_info.version);
						if(compVersions(new_app_info.version, app_info.version) <= 0) {
							ESP_LOGW(TAG,
								"Current version (%s) is equal or more recent than the version online (%s)",
								app_info.version,
								new_app_info.version
							);
							wsUpgrade("updated", 0, 0);
							break;
						} else { // New firmware available
							const esp_partition_t *last_invalid_app = esp_ota_get_last_invalid_partition();
							if(
								last_invalid_app != NULL &&
								esp_ota_get_partition_description(last_invalid_app, &app_info) == ESP_OK
							) {
								ESP_LOGI(TAG, "Last invalid firmware version : %s", app_info.version);
								if(compVersions(new_app_info.version, app_info.version) == 0) {
									ESP_LOGW(TAG, "New version (%s) is the same as invalid version", new_app_info.version);
									wsUpgrade("Invalid", 0, binary_file_length);
									break;
								}
							}

							err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
							if(err != ESP_OK) {
								ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
								wsUpgrade("failed", binary_file_length, binary_file_length);
								break;
							}
							ESP_LOGI(TAG, "esp_ota_begin succeeded");
							wsUpgrade("starting", 0, 0);
							clientDisconnect("OTA");
							image_header_was_checked = true;
						}
					} else {
						ESP_LOGE(TAG, "Unable to get the current version");
						break;
					}
				}
			}

			err = esp_ota_write(update_handle, (const void *) ota_write_data, data_read);
			if(err != ESP_OK) {
				ESP_LOGW(TAG, "Unable to write more than %u bytes in the partition", binary_file_length);
				break;
			}
			binary_file_length += data_read;
			// ESP_LOGD(TAG, "Written image length : %d", binary_file_length);
			ESP_LOGW(TAG, "Written image length : %d", binary_file_length);
			wsUpgrade("downloading", binary_file_length, binary_file_length);
			vTaskDelay(delay_loading);
		} else { // data_read == 0
			ESP_LOGI(TAG, "Connection closed.");
			ESP_LOGI(TAG, "Total written binary data length : %d", binary_file_length);

			if(esp_ota_end(update_handle) != ESP_OK) {
				ESP_LOGE(TAG, "esp_ota_end failed !");
			} else {
				err = esp_ota_set_boot_partition(update_partition);
				if(err != ESP_OK) {
					ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
				} else {
				    ESP_LOGI(TAG, "Prepare to restart system!");
					vTaskDelay(delay_loading);
				    esp_restart();
				}
			}

			break;
		}
	}

	http_cleanup(client);
	// ESP_LOGV(TAG,"server free of %x - Heap size: %u", (int)ota_write_data, xPortGetFreeHeapSize());
	// free(ota_write_data);
	vTaskDelay(50 / portTICK_PERIOD_MS);
	ESP_LOGV(TAG, "Done");
	wsUpgrade("Done", 0, 0);
	taskState = false;
#else
	ESP_LOGE(TAG, "CONFIG_OTA_FIRMWARE_ACCOUNT not set. Check config !!");
#endif
	wsUpgrade("Bye-bye", 0, 0);
	ESP_LOGV(TAG, "Bye-Bye");
	taskState = false;
	vTaskDelay(100 / portTICK_PERIOD_MS);
	(void)vTaskDelete( NULL );
}

void update_firmware(char* fname) {
	if (!taskState) {
		taskState = true;
		xTaskHandle pxCreatedTask;
		xTaskCreate(otaTask, "otaTask", 8192, fname, PRIO_OTA, &pxCreatedTask);
		ESP_LOGI(TAG, "otaTask: %x",(unsigned int)pxCreatedTask);
	} else {
		ESP_LOGI(TAG, "otaTask: already running. Ignore");
		wsUpgrade("Running" , 0, 100);
	}
}
