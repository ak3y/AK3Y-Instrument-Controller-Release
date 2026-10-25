/******************************************************************************
 * File: config.h
 * Project: AK3Y Instrument Controller
 * Purpose: Hardware pin assignments
 * Description: Defines GPIO assignments for SPI, GPIB, microSD, OLED, and serial interfaces.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#pragma once

#include <Arduino.h>

namespace AK3Y
{
    namespace Config
    {
        namespace SPI
        {
            inline constexpr uint8_t SCK  = 18;
            inline constexpr uint8_t MISO = 19;
            inline constexpr uint8_t MOSI = 23;
        }

        namespace MCP23S17
        {
            inline constexpr uint8_t CS = 32;
        }

        namespace GPIB
        {
            inline constexpr uint8_t TE       = 25;
            inline constexpr uint8_t DC       = 26;
            inline constexpr uint8_t BufferOE = 33;
        }

        namespace SD
        {
            inline constexpr uint8_t CS = 27;
        }

        namespace OLED
        {
            inline constexpr uint8_t SDA = 21;
            inline constexpr uint8_t SCL = 22;
        }

        namespace RS232
        {
            inline constexpr uint8_t Port1TX = 17;
            inline constexpr uint8_t Port1RX = 34;

            inline constexpr uint8_t Port2TX = 16;
            inline constexpr uint8_t Port2RX = 35;
        }
    }
}