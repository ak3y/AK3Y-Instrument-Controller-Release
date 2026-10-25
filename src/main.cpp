/******************************************************************************
 * File: main.cpp
 * Project: AK3Y Instrument Controller
 * Purpose: Firmware entry point
 * Description: Creates the application and delegates Arduino setup and loop execution to it.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#include <Arduino.h>

#include "Application.h"

//
// Global application object.
//
// This is the only global object in the firmware.
//

AK3Y::Application app;

void setup()
{
    app.initialize();
}

void loop()
{
    app.update();
}