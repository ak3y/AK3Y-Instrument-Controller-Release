/******************************************************************************
 * File: Subsystem.h
 * Project: AK3Y Instrument Controller
 * Purpose: Subsystem interface
 * Description: Defines the common initialization contract for controller subsystems.
 * Note: This file header describes responsibilities; release history is in Git.
 ******************************************************************************/

#pragma once

namespace AK3Y
{

class Subsystem
{
public:

    virtual ~Subsystem() = default;

    virtual bool initialize() = 0;
};

}   // namespace AK3Y