#pragma once
#include "global.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

void imuSensorInit();
void imuSensorProcess();