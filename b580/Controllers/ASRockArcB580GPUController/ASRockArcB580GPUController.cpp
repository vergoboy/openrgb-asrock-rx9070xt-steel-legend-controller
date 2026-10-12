/*---------------------------------------------------------*\
| ASRockArcB580GPUController.cpp                            |
|                                                           |
|   Driver for ASRock Arc B580 Steel Legend GPU RGB         |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "ASRockArcB580GPUController.h"

#include <utility>

ASRockArcB580GPUController::ASRockArcB580GPUController(i2c_smbus_interface* bus_ptr, std::string device_name)
    : bus(bus_ptr),
      name(std::move(device_name))
{
    channels.push_back({ "ARGB Header", 0x03 });
    channels.push_back({ "Top / Side",   0x06 });
    channels.push_back({ "Fan",          0x07 });
}

std::string ASRockArcB580GPUController::GetName() const
{
    return name;
}

std::string ASRockArcB580GPUController::GetLocation() const
{
    if(bus == nullptr)
    {
        return "I2C: unavailable";
    }

    return std::string("I2C bus ") + std::to_string(bus->bus_id) +
           " addr 0x36";
}

unsigned int ASRockArcB580GPUController::GetChannelCount() const
{
    return static_cast<unsigned int>(channels.size());
}

const ASRockArcB580GPUController::Channel& ASRockArcB580GPUController::GetChannel(unsigned int index) const
{
    return channels[index];
}

bool ASRockArcB580GPUController::WriteChannel(uint8_t channel,
                                              uint8_t mode,
                                              uint8_t red,
                                              uint8_t green,
                                              uint8_t blue,
                                              uint8_t speed,
                                              uint8_t brightness,
                                              uint8_t direction)
{
    if(bus == nullptr)
    {
        return false;
    }

    uint8_t packet[12] =
    {
        0x10, 0x00, channel,
        mode, red, green, blue,
        speed, brightness, direction,
        0x1A, 0x00
    };

    return bus->i2c_write_block(I2C_ADDRESS, sizeof(packet), packet) >= 0;
}