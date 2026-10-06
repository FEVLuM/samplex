/* 
 *  Copyright (c) 2025 Eclipse Foundation
 * 
 *  This program and the accompanying materials are made available 
 *  under the terms of the MIT license which is available at
 *  https://opensource.org/license/mit.
 * 
 *  SPDX-License-Identifier: MIT
 * 
 *  Contributors: 
 *     Frédéric Desbiens - Initial version.
 */
#include "cloud_config.h"
#include "board_init.h"
#include "mqtt_client.h"
#include "nanoprintf.h"
#include "nx_api.h"
#include "ssd1306.h"
#include "telemetry.h"
#include "wwd_networking.h"

#include <stdint.h>
#include <string.h>

// Helper function.
#define STRLEN(p) (sizeof(p) - 1)

/* Declare the MQTT thread stack space. */
static ULONG mqtt_client_stack[MQTT_CLIENT_STACK_SIZE / sizeof(ULONG)];

/* Declare buffers to hold message and topic. */
static UCHAR message_buffer[NXD_MQTT_MAX_MESSAGE_LENGTH];
static UCHAR topic_buffer[NXD_MQTT_MAX_TOPIC_NAME_LENGTH];


/* Declare the MQTT client control block. */
static NXD_MQTT_CLIENT mqtt_client;

typedef enum
{
    DISPLAY_WIFI_INITIALIZING,
    DISPLAY_WIFI_CONNECTING,
    DISPLAY_WIFI_CONNECTED,
    DISPLAY_WIFI_FAILED,
    DISPLAY_WIFI_SETUP_FAILED
} display_wifi_status_t;

typedef enum
{
    DISPLAY_MQTT_WAITING,
    DISPLAY_MQTT_CONNECTING,
    DISPLAY_MQTT_CONNECTED,
    DISPLAY_MQTT_DISCONNECTED,
    DISPLAY_MQTT_CONNECT_FAILED,
    DISPLAY_MQTT_SUBSCRIBE_FAILED,
    DISPLAY_MQTT_UNAVAILABLE
} display_mqtt_status_t;

static volatile display_wifi_status_t wifi_display_status = DISPLAY_WIFI_INITIALIZING;
static volatile display_mqtt_status_t mqtt_display_status = DISPLAY_MQTT_WAITING;
static volatile UINT button_a_pressed;
static volatile UINT button_b_pressed;
static volatile uint8_t rolling_counter;
static volatile float published_temperature_degC;

typedef enum
{
    TEMPERATURE_LIVE,
    TEMPERATURE_MAX,
    TEMPERATURE_FROZEN,
    TEMPERATURE_BOTH_BUTTONS
} temperature_mode_t;

static char *wifi_status_text(display_wifi_status_t status)
{
    switch (status)
    {
        case DISPLAY_WIFI_CONNECTING:
            return "WiFi: connecting";
        case DISPLAY_WIFI_CONNECTED:
            return "WiFi: connected";
        case DISPLAY_WIFI_FAILED:
            return "WiFi: init failed";
        case DISPLAY_WIFI_SETUP_FAILED:
            return "WiFi: setup failed";
        case DISPLAY_WIFI_INITIALIZING:
        default:
            return "WiFi: initializing";
    }
}

static char *mqtt_status_text(display_mqtt_status_t status)
{
    switch (status)
    {
        case DISPLAY_MQTT_CONNECTING:
            return "MQTT: connecting";
        case DISPLAY_MQTT_CONNECTED:
            return "MQTT: connected";
        case DISPLAY_MQTT_DISCONNECTED:
            return "MQTT: disconnected";
        case DISPLAY_MQTT_CONNECT_FAILED:
            return "MQTT: failed";
        case DISPLAY_MQTT_SUBSCRIBE_FAILED:
            return "MQTT: sub failed";
        case DISPLAY_MQTT_UNAVAILABLE:
            return "MQTT: unavailable";
        case DISPLAY_MQTT_WAITING:
        default:
            return "MQTT: waiting";
    }
}

void display_thread_entry(ULONG parameter)
{
    char temperature_text[24];
    char counter_text[20];
    ULONG next_counter_tick = tx_time_get() + TX_TIMER_TICKS_PER_SECOND;
    ULONG poll_ticks = TX_TIMER_TICKS_PER_SECOND / 20u;
    temperature_mode_t temperature_mode = TEMPERATURE_LIVE;
    float frozen_temperature_degC = 0.0f;
    float last_displayed_temperature = -1000.0f;
    uint8_t last_displayed_counter = 0xFFu;
    display_wifi_status_t last_wifi_status = (display_wifi_status_t)-1;
    display_mqtt_status_t last_mqtt_status = (display_mqtt_status_t)-1;

    NX_PARAMETER_NOT_USED(parameter);
    if (poll_ticks == 0u)
    {
        poll_ticks = 1u;
    }

    while (1)
    {
        display_wifi_status_t wifi_status = wifi_display_status;
        display_mqtt_status_t mqtt_status = mqtt_display_status;
        UINT a_pressed = button_a_pressed;
        UINT b_pressed = button_b_pressed;
        UINT counter_should_pause = b_pressed && !a_pressed;
        float measured_temperature = get_current_temperature();
        float temperature;

        if (temperature_mode == TEMPERATURE_BOTH_BUTTONS)
        {
            if (!a_pressed && !b_pressed)
            {
                temperature_mode = TEMPERATURE_LIVE;
            }
        }
        else if (a_pressed && b_pressed)
        {
            frozen_temperature_degC = measured_temperature + 20.0f;
            temperature_mode = TEMPERATURE_BOTH_BUTTONS;
        }
        else if (b_pressed)
        {
            if (temperature_mode != TEMPERATURE_FROZEN)
            {
                frozen_temperature_degC = measured_temperature;
            }
            temperature_mode = TEMPERATURE_FROZEN;
        }
        else if (a_pressed)
        {
            temperature_mode = TEMPERATURE_MAX;
        }
        else
        {
            temperature_mode = TEMPERATURE_LIVE;
        }

        switch (temperature_mode)
        {
            case TEMPERATURE_MAX:
                temperature = TELEMETRY_TEMPERATURE_MAX_DEGC;
                break;
            case TEMPERATURE_FROZEN:
            case TEMPERATURE_BOTH_BUTTONS:
                temperature = frozen_temperature_degC;
                break;
            case TEMPERATURE_LIVE:
            default:
                temperature = measured_temperature;
                break;
        }
        published_temperature_degC = temperature;

        ULONG now = tx_time_get();
        if ((LONG)(now - next_counter_tick) >= 0)
        {
            if (!counter_should_pause)
            {
                rolling_counter = (uint8_t)(rolling_counter + 1u);
            }
            next_counter_tick += TX_TIMER_TICKS_PER_SECOND;
        }
        if (counter_should_pause)
        {
            next_counter_tick = now + TX_TIMER_TICKS_PER_SECOND;
        }

        if (temperature != last_displayed_temperature ||
            rolling_counter != last_displayed_counter ||
            wifi_status != last_wifi_status ||
            mqtt_status != last_mqtt_status)
        {
            npf_snprintf(temperature_text, sizeof(temperature_text), "Temp: %.1f C", (double)temperature);
            npf_snprintf(counter_text, sizeof(counter_text), "Count: %03u", (unsigned)rolling_counter);

            ssd1306_Fill(Black);
            ssd1306_SetCursor(14, 0);
            ssd1306_WriteString("FEVengers", Font_11x18, White);
            ssd1306_SetCursor(2, 20);
            ssd1306_WriteString(wifi_status_text(wifi_status), Font_7x10, White);
            ssd1306_SetCursor(2, 31);
            ssd1306_WriteString(mqtt_status_text(mqtt_status), Font_7x10, White);
            ssd1306_SetCursor(2, 42);
            ssd1306_WriteString(temperature_text, Font_7x10, White);
            ssd1306_SetCursor(2, 53);
            ssd1306_WriteString(counter_text, Font_7x10, White);
            ssd1306_UpdateScreen();

            last_displayed_temperature = temperature;
            last_displayed_counter = rolling_counter;
            last_wifi_status = wifi_status;
            last_mqtt_status = mqtt_status;
        }

        tx_thread_sleep(poll_ticks);
    }
}

void button_a_callback(void)
{
    button_a_pressed = BUTTON_A_IS_PRESSED ? 1u : 0u;
}

void button_b_callback(void)
{
    button_b_pressed = BUTTON_B_IS_PRESSED ? 1u : 0u;
}

static float get_published_temperature(void)
{
    return published_temperature_degC;
}

static uint8_t get_rolling_counter(void)
{
    return rolling_counter;
}

/* Declare the disconnect notify function. */
static VOID client_disconnect_func(NXD_MQTT_CLIENT *client_ptr)
{
    NX_PARAMETER_NOT_USED(client_ptr);
    printf("client disconnected from broker.\r\n");
    mqtt_display_status = DISPLAY_MQTT_DISCONNECTED;
}

static void send_message(){
    UINT status;
    
    /* Publish a message with QoS Level 1. */
    char buffer[NXD_MQTT_MAX_MESSAGE_LENGTH] = {0};
    get_current_telemetry_string(buffer, sizeof(buffer), get_published_temperature(), get_rolling_counter());
    //printf("%s", buffer);

    status = nxd_mqtt_client_publish(&mqtt_client, MQTT_PUBLISH_TOPIC, STRLEN(MQTT_PUBLISH_TOPIC),
                                        (CHAR *)buffer, strlen(buffer), 0, QOS1, NX_WAIT_FOREVER);

    if (status != NXD_MQTT_SUCCESS){
        printf("Publish failed with code: %d\r\n", status);
    }
    else{
        printf("Published message.\r\n");
    }
}

static void receive_message(){
    UINT status;
    UINT topic_length, message_length;
    ULONG message_sent = 0;

    status = nxd_mqtt_client_message_get(&mqtt_client, topic_buffer, sizeof(topic_buffer), &topic_length,
                                        message_buffer, sizeof(message_buffer), &message_length);
    printf("Received message and status: %d \r\n", status);
    if (status == NXD_MQTT_SUCCESS){
        topic_buffer[topic_length] = 0;
        message_buffer[message_length] = 0;
        message_sent = message_buffer[0];
        status = tx_queue_send(&mqtt_queue, &message_sent, TX_WAIT_FOREVER);
        printf("Topic: %s, Message: %s\r\n", topic_buffer, message_buffer);
    }
}

static VOID client_notify_func(NXD_MQTT_CLIENT *client_ptr, UINT number_of_messages)
{
    NX_PARAMETER_NOT_USED(client_ptr);
    NX_PARAMETER_NOT_USED(number_of_messages);
    tx_event_flags_set(&mqtt_app_flag, MQTT_RECEIVE_EVENT, TX_OR);
    return;
}

static ULONG error_count;
static void mqtt_thread_work(NX_IP *ip_ptr, NX_PACKET_POOL *pool_ptr){
    UINT status;
    NXD_ADDRESS server_ip;
    ULONG events;

    printf("Creating MQTT client\r\n");
    /* Create MQTT client instance. */
    status = nxd_mqtt_client_create(&mqtt_client, MQTT_CLIENT_NAME, MQTT_CLIENT_NAME, STRLEN(MQTT_CLIENT_NAME),
                                    ip_ptr, pool_ptr, (VOID *)mqtt_client_stack, sizeof(mqtt_client_stack),
                                    MQTT_THREAD_PRIORTY, NX_NULL, 0);

    if (status){
        printf("Error in creating MQTT client: 0x%02x\n", status);
        error_count++;
    }

    printf(" MQTT client created\r\n");

    /* Register the disconnect notification function. */
    nxd_mqtt_client_disconnect_notify_set(&mqtt_client, client_disconnect_func);

    server_ip.nxd_ip_version = 4;
    server_ip.nxd_ip_address.v4 = MQTT_LOCAL_BROKER_IP;

    /* Start the connection to the server. */

    status = nxd_mqtt_client_connect(&mqtt_client, &server_ip, NXD_MQTT_PORT,
                                     MQTT_KEEP_ALIVE_TIMER, 0, NX_WAIT_FOREVER);
    if (status != NXD_MQTT_SUCCESS){
                printf("MQTT connect failed with code: %d\r\n", status);
        mqtt_display_status = DISPLAY_MQTT_CONNECT_FAILED;
    }
    else{
        printf("MQTT Client connected.\r\n");
        mqtt_display_status = DISPLAY_MQTT_CONNECTED;
    }

    /* Subscribe to the topic with QoS level 0. */
    status = nxd_mqtt_client_subscribe(&mqtt_client, MQTT_SUBSCRIBE_TOPIC, STRLEN(MQTT_SUBSCRIBE_TOPIC), QOS0);
    if (status != NXD_MQTT_SUCCESS){
                printf("MQTT subscribe failed with code: %d\r\n", status);
        mqtt_display_status = DISPLAY_MQTT_SUBSCRIBE_FAILED;
    }
    else{
        printf("Subscribed to topic %s.\r\n", MQTT_SUBSCRIBE_TOPIC);
    }

    /* Set the receive notify function. */
    status = nxd_mqtt_client_receive_notify_set(&mqtt_client, client_notify_func);
    if (status != NXD_MQTT_SUCCESS){
                printf("MQTT receive notify setup failed with code: %d\r\n", status);
    }
    else{
        printf("MQTT Receive notify function set.\r\n");
    }

    /* Now wait for the broker to publish the message. */
    printf("Waiting for messages\r\n");

    while (1){
        tx_event_flags_get(&mqtt_app_flag, MQTT_ALL_EVENTS, TX_OR_CLEAR, &events, TX_WAIT_FOREVER);
        if (events & MQTT_RECEIVE_EVENT){
            receive_message();

        }
        else if (events & MQTT_MESSAGE_READY){
            send_message();
        }
    }

    /* Cleanup. Release resources. */
    nxd_mqtt_client_unsubscribe(&mqtt_client, MQTT_SUBSCRIBE_TOPIC, STRLEN(MQTT_SUBSCRIBE_TOPIC));
    nxd_mqtt_client_disconnect(&mqtt_client);
    nxd_mqtt_client_delete(&mqtt_client);

    return;
}

void mqtt_thread_entry(ULONG parameter){

    UINT status;

    printf("Starting Eclipse ThreadX MQTT thread\r\n\r\n");
    RGB_LED_SET_R(2047);
    RGB_LED_SET_G(700);
    RGB_LED_SET_B(0);
    wifi_display_status = DISPLAY_WIFI_INITIALIZING;
    mqtt_display_status = DISPLAY_MQTT_WAITING;

    // Initialize the network
    if ((status = wwd_network_init(WIFI_SSID, WIFI_PASSWORD, WIFI_MODE))){
        printf("ERROR: Failed to initialize the network (0x%08x)\r\n", status);
        wifi_display_status = DISPLAY_WIFI_FAILED;
        mqtt_display_status = DISPLAY_MQTT_UNAVAILABLE;
        return;
    }

    wifi_display_status = DISPLAY_WIFI_CONNECTING;
    if ((status = wwd_network_connect())){
        printf("ERROR: Failed to connect to the network (0x%08x)\r\n", status);
        wifi_display_status = DISPLAY_WIFI_SETUP_FAILED;
        mqtt_display_status = DISPLAY_MQTT_UNAVAILABLE;
        return;
    }

    wifi_display_status = DISPLAY_WIFI_CONNECTED;
    mqtt_display_status = DISPLAY_MQTT_CONNECTING;
    mqtt_thread_work(&nx_ip, nx_pool);
}
