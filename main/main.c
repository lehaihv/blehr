/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "esp_log.h"
#include "esp_random.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOSConfig.h"
/* BLE */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "blehr_sens.h"

static const char *tag = "NimBLE_BLE_HeartRate";

static TimerHandle_t blehr_tx_timer;
static TimerHandle_t temp_tx_timer;
static TimerHandle_t hum_tx_timer;
static TimerHandle_t adv_restart_timer; // 🔑 NEW: Timer for delayed advertising restart

static bool notify_state;

static uint16_t conn_handle;

static const char *device_name = "blehr_sensor_1.0";

static int blehr_gap_event(struct ble_gap_event *event, void *arg);

static uint8_t blehr_addr_type;

/* Variable to simulate heart beats */
static uint8_t heartrate = 90;

/* Temperature simulation */
static int16_t current_temp = 2700; /* 27.00°C in 0.01°C units */
static bool temp_notify_state;

/* Humidity simulation */
static uint16_t current_hum = 7500; /* 75.00% in 0.01% units */
static bool hum_notify_state;

/**
 * Utility function to log an array of bytes.
 */
void
print_bytes(const uint8_t *bytes, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        MODLOG_DFLT(INFO, "%s0x%02x", i != 0 ? ":" : "", bytes[i]);
    }
}

void
print_addr(const void *addr)
{
    const uint8_t *u8p;

    u8p = addr;
    MODLOG_DFLT(INFO, "%02x:%02x:%02x:%02x:%02x:%02x",
                u8p[5], u8p[4], u8p[3], u8p[2], u8p[1], u8p[0]);
}

/*
 * Enables advertising with parameters:
 *     o General discoverable mode
 *     o Undirected connectable mode
 */
static void
blehr_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    int rc;

    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN |
                   BLE_HS_ADV_F_BREDR_UNSUP;

    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    fields.name = (uint8_t *)device_name;
    fields.name_len = strlen(device_name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error setting advertisement data; rc=%d\n", rc);
        return;
    }

    /* Begin advertising */
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(blehr_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, blehr_gap_event, NULL);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "error enabling advertisement; rc=%d\n", rc);
        return;
    }
}

// 🔑 NEW: Timer callback to safely restart advertising after disconnect
static void
blehr_restart_advertise(TimerHandle_t ev)
{
    if (ble_gap_adv_active()) {
        return; // Already advertising
    }
    MODLOG_DFLT(INFO, "Restarting advertising after disconnect delay\n");
    blehr_advertise();
}

static void
blehr_tx_hrate_stop(void)
{
    xTimerStop( blehr_tx_timer, 1000 / portTICK_PERIOD_MS );
}

/* Reset heart rate measurement */
static void
blehr_tx_hrate_reset(void)
{
    int rc;
    if (xTimerReset(blehr_tx_timer, 1000 / portTICK_PERIOD_MS ) == pdPASS) {
        rc = 0;
    } else {
        rc = 1;
    }
    assert(rc == 0);
}

/* This function simulates heart beat and notifies it to the client */
static void
blehr_tx_hrate(TimerHandle_t ev)
{
    static uint8_t hrm[2];
    int rc;
    struct os_mbuf *om;

    if (!notify_state) {
        blehr_tx_hrate_stop();
        heartrate = 90;
        return;
    }

    hrm[0] = 0x06; /* contact of a sensor */
    hrm[1] = heartrate; /* storing dummy data */

    /* Simulation of heart beats */
    heartrate++;
    if (heartrate == 160) {
        heartrate = 90;
    }

    om = ble_hs_mbuf_from_flat(hrm, sizeof(hrm));
    
    // 🔑 FIX: Added NULL check to prevent memory crash
    if (om == NULL) {
        MODLOG_DFLT(ERROR, "Failed to allocate mbuf for heart rate\n");
        return;
    }

    rc = ble_gatts_notify_custom(conn_handle, hrs_hrm_handle, om);

    // 🔑 FIX: Replaced assert(rc == 0) with error logging to prevent ESP32 reboots
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Failed to send heart rate notification; rc=%d\n", rc);
    }

    blehr_tx_hrate_reset();
}

/* This function simulates temperature and notifies it to the client */
static void
blehr_tx_temperature(TimerHandle_t ev)
{
    int rc;
    struct os_mbuf *om;

    if (!temp_notify_state) {
        return;
    }

    int16_t delta = (esp_random() % 21) - 10; 
    current_temp += delta;

    if (current_temp < 2500) current_temp = 2500;
    if (current_temp > 3000) current_temp = 3000;

    MODLOG_DFLT(INFO, "Temperature notification: %d.%02d C\n",
                current_temp / 100, abs(current_temp % 100));

    om = ble_hs_mbuf_from_flat(&current_temp, sizeof(current_temp));
    
    // 🔑 FIX: Added NULL check
    if (om == NULL) {
        MODLOG_DFLT(ERROR, "Failed to allocate mbuf for temperature\n");
        return;
    }

    rc = ble_gatts_notify_custom(conn_handle, temp_handle, om);

    // 🔑 FIX: Replaced assert(rc == 0) with error logging
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Failed to send temperature notification; rc=%d\n", rc);
    }
}

/* This function simulates humidity and notifies it to the client */
static void
blehr_tx_humidity(TimerHandle_t ev)
{
    int rc;
    struct os_mbuf *om;

    if (!hum_notify_state) {
        return;
    }

    int16_t delta = (esp_random() % 11) - 5; 
    current_hum += delta;

    if (current_hum < 7000) current_hum = 7000;
    if (current_hum > 8000) current_hum = 8000;

    MODLOG_DFLT(INFO, "Humidity notification: %d.%02d %%\n",
                current_hum / 100, current_hum % 100);

    om = ble_hs_mbuf_from_flat(&current_hum, sizeof(current_hum));
    
    // 🔑 FIX: Added NULL check
    if (om == NULL) {
        MODLOG_DFLT(ERROR, "Failed to allocate mbuf for humidity\n");
        return;
    }

    rc = ble_gatts_notify_custom(conn_handle, hum_handle, om);

    // 🔑 FIX: Replaced assert(rc == 0) with error logging
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Failed to send humidity notification; rc=%d\n", rc);
    }
}

static int
blehr_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        MODLOG_DFLT(INFO, "connection %s; status=%d\n",
                    event->connect.status == 0 ? "established" : "failed",
                    event->connect.status);

        // 🔑 FIX: Only set conn_handle if connection was successful
        if (event->connect.status == 0) {
            conn_handle = event->connect.conn_handle;
        } else {
            blehr_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        MODLOG_DFLT(INFO, "disconnect; reason=%d\n", event->disconnect.reason);
        
        blehr_tx_hrate_stop();
        xTimerStop(temp_tx_timer, 1000 / portTICK_PERIOD_MS);
        xTimerStop(hum_tx_timer, 1000 / portTICK_PERIOD_MS);

        notify_state = false;
        temp_notify_state = false;
        hum_notify_state = false;
        conn_handle = 0;

        // 🔑 CRITICAL FIX: Use a timer to delay advertising restart by 500ms.
        // This prevents the rc=3 (EAGAIN) error caused by calling blehr_advertise() 
        // while NimBLE is still cleaning up the previous connection.
        xTimerReset(adv_restart_timer, pdMS_TO_TICKS(500));
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        MODLOG_DFLT(INFO, "adv complete\n");
        blehr_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        MODLOG_DFLT(INFO, "subscribe event; cur_notify=%d value handle; "
                    "val_handle=%d\n",
                    event->subscribe.cur_notify, hrs_hrm_handle);
        if (event->subscribe.attr_handle == hrs_hrm_handle) {
            notify_state = event->subscribe.cur_notify;
            blehr_tx_hrate_reset();
        } else if (event->subscribe.attr_handle == temp_handle) {
            temp_notify_state = event->subscribe.cur_notify;
            MODLOG_DFLT(INFO, "Temperature notify %s\n",
                        temp_notify_state ? "enabled" : "disabled");
            if (temp_notify_state) {
                xTimerReset(temp_tx_timer, 1000 / portTICK_PERIOD_MS);
            } else {
                xTimerStop(temp_tx_timer, 1000 / portTICK_PERIOD_MS);
            }
        } else if (event->subscribe.attr_handle == hum_handle) {
            hum_notify_state = event->subscribe.cur_notify;
            MODLOG_DFLT(INFO, "Humidity notify %s\n",
                        hum_notify_state ? "enabled" : "disabled");
            if (hum_notify_state) {
                xTimerReset(hum_tx_timer, 1000 / portTICK_PERIOD_MS);
            } else {
                xTimerStop(hum_tx_timer, 1000 / portTICK_PERIOD_MS);
            }
        } else if (event->subscribe.attr_handle != hrs_hrm_handle) {
            notify_state = event->subscribe.cur_notify;
            blehr_tx_hrate_stop();
        }
        ESP_LOGI("BLE_GAP_SUBSCRIBE_EVENT", "conn_handle from subscribe=%d", conn_handle);
        break;

    case BLE_GAP_EVENT_MTU:
        MODLOG_DFLT(INFO, "mtu update event; conn_handle=%d mtu=%d\n",
                    event->mtu.conn_handle,
                    event->mtu.value);
        break;
    }

    return 0;
}

static void
blehr_on_sync(void)
{
    int rc;

    rc = ble_hs_id_infer_auto(0, &blehr_addr_type);
    assert(rc == 0);

    uint8_t addr_val[6] = {0};
    rc = ble_hs_id_copy_addr(blehr_addr_type, addr_val, NULL);

    MODLOG_DFLT(INFO, "Device Address: ");
    print_addr(addr_val);
    MODLOG_DFLT(INFO, "\n");

    /* Begin advertising */
    blehr_advertise();
}

static void
blehr_on_reset(int reason)
{
    MODLOG_DFLT(ERROR, "Resetting state; reason=%d\n", reason);
}

void blehr_host_task(void *param)
{
    ESP_LOGI(tag, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void app_main(void)
{
    int rc;

    /* Configure GPIO38 for RGB LED - set to low to ensure LED is off */
    gpio_reset_pin(GPIO_NUM_38);
    gpio_set_direction(GPIO_NUM_38, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_38, 1);

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        MODLOG_DFLT(ERROR, "Failed to init nimble %d \n", ret);
        return;
    }

    ble_hs_cfg.sync_cb = blehr_on_sync;
    ble_hs_cfg.reset_cb = blehr_on_reset;

    blehr_tx_timer = xTimerCreate("blehr_tx_timer", pdMS_TO_TICKS(150), pdTRUE, (void *)0, blehr_tx_hrate);
    temp_tx_timer = xTimerCreate("temp_tx_timer", pdMS_TO_TICKS(100), pdTRUE, (void *)0, blehr_tx_temperature);
    hum_tx_timer = xTimerCreate("hum_tx_timer", pdMS_TO_TICKS(120), pdTRUE, (void *)0, blehr_tx_humidity);
    
    // 🔑 NEW: Create the delayed advertising restart timer (single shot, 500ms)
    adv_restart_timer = xTimerCreate("adv_restart_timer", pdMS_TO_TICKS(500), pdFALSE, (void *)0, blehr_restart_advertise);

#if MYNEWT_VAL(BLE_GATTS)
    rc = gatt_svr_init();
    assert(rc == 0);

    rc = ble_svc_gap_device_name_set(device_name);
    assert(rc == 0);
#endif

    nimble_port_freertos_init(blehr_host_task);
}