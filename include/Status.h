/******************************************************************************
 * File: Status.h
 * Project: AK3Y Instrument Controller
 * Purpose: Shared status codes
 * Description: Defines common status outcomes used by controller subsystems.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#pragma once

namespace AK3Y
{
    enum class Status
    {
        Pass,
        Fail,
        Warning,
        Busy,
        Timeout,
        NotImplemented
    };
}