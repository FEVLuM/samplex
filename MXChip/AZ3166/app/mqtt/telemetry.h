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
#include "sensor.h"
#include "tx_api.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifndef _TELEMETRY_H
#define _TELEMETRY_H

// Log telemetry if needed
//#define LOG_TELEMETRY

// Sensor data
typedef struct{
    float pressure_hPa;
    float temperature_degC;
    float humidity_perc;
    float acceleration_mg[3];
    float magnetic_mG[3];
} sensor_data;

#define TELEMETRY_TEMPERATURE_MAX_DEGC 85.0f

void telemetry_thread_entry(ULONG parameter);
void get_current_telemetry_string(char* output, size_t output_size, float temperature_degC, uint8_t rolling_counter);
float get_current_temperature(void);

#endif // _TELEMETRY_H