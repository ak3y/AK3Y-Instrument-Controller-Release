/******************************************************************************
 * File: Configuration.h
 * Project: AK3Y Instrument Controller
 * Purpose: Central compile-time settings for the controller.
 * Details: Defines firmware and hardware identifiers, pin assignments, and compile-time constants. Contains no runtime configuration or mutable state.
 * Author: Bob Fontana, AK3Y
 *
 * Documentation only; firmware history is maintained in Git.
 ******************************************************************************/

#pragma once

#include <stdint.h>

namespace AK3Y
{

/******************************************************************************
 *
 *  Configuration
 *
 ******************************************************************************/

namespace Configuration
{

    /**************************************************************************
     *
     *  Firmware
     *
     **************************************************************************/

    namespace Firmware
    {
        inline constexpr char Version[] = "0.90(a)";

        inline constexpr char HardwareRevision[] = "Rev 1.0";
    }

    /**************************************************************************
     *
     *  Console
     *
     **************************************************************************/

    namespace Console
    {
        inline constexpr uint32_t BaudRate = 115200;
    }

    /**************************************************************************
     *
     *  Diagnostics
     *
     **************************************************************************/

    namespace Diagnostics
    {
        // Enable legacy engineering tests during startup only when needed.
        // Normal startup performs subsystem initialization and validation.
        inline constexpr bool RunStartupTests = false;
        inline constexpr uint32_t TimerDiagnosticDelayMs = 10;
    }

     /**************************************************************************
     *
     *  GPIB
     *
     **************************************************************************/

    namespace GPIB
    {
        inline constexpr uint8_t ControllerPrimaryAddress = 0;
    }

    /**************************************************************************
     *
     *  Processor Pin Assignments
     *
     **************************************************************************/

    namespace Pins
    {

        /**********************************************************************
         *
         *  SPI Device Chip Select Signals
         *
         **********************************************************************/

        namespace SPI
        {
            //
            // MCP23S17 GPIO Expander
            //

            inline constexpr uint8_t MCP23S17ChipSelect = 32;

            //
            // SD Card
            //

            inline constexpr uint8_t SDCardChipSelect = 27;
        }


        /**********************************************************************
         *
         *  I2C Interface Signals
         *
         **********************************************************************/
        namespace I2C
        {
            inline constexpr uint8_t SDA = 21;
            inline constexpr uint8_t SCL = 22;
        }

        /**********************************************************************
         *
         *  GPIB Interface Control Signals
         *
         *  These signals connect directly between the ESP32 and the
         *  GPIB interface hardware.  They do not pass through the
         *  MCP23S17 GPIO expander.
         *
         **********************************************************************/

        /**********************************************************************
         *
         *  RS-232 Interface Signals
         *
         **********************************************************************/

        namespace RS232
        {
            inline constexpr uint8_t Port1TX = 17;
            inline constexpr uint8_t Port1RX = 34;
            inline constexpr uint8_t Port2TX = 16;
            inline constexpr uint8_t Port2RX = 35;
        }

        namespace GPIB
        {
            //
            // SN75160 / SN75161 Transceiver Enable
            //

            inline constexpr uint8_t TE = 25;

            //
            // SN75161 Direction Control
            //

            inline constexpr uint8_t DC = 26;

            //
            // 74HCT245 Buffer Output Enable
            //
            // Active LOW
            //

            inline constexpr uint8_t BufferEnable = 33;

            //
            // EOI 74HCT245 Direction Control
            //

            inline constexpr uint8_t EOIDirection = 4;
        }

    }   // namespace Pins

}   // namespace Configuration

}   // namespace AK3Y