/*---------------------------------------------------------*\
| RGBController_ASRockArcB580GPU.h                          |
|                                                           |
|   RGBController for ASRock Arc B580 Steel Legend GPU      |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <cstdint>

#include "RGBController.h"
#include "ASRockArcB580GPUController.h"

class RGBController_ASRockArcB580GPU : public RGBController
{
public:
    explicit RGBController_ASRockArcB580GPU(ASRockArcB580GPUController* controller_ptr);
    ~RGBController_ASRockArcB580GPU() override;

    void SetupZones() override;
    void ResizeZone(int zone, int new_size) override;
    void DeviceUpdateLEDs() override;
    void UpdateZoneLEDs(int zone) override;
    void UpdateSingleLED(int led) override;
    void DeviceUpdateMode() override;

private:
    ASRockArcB580GPUController* controller;

    void AddHardwareMode(const char* name,
                         uint8_t value,
                         unsigned int flags,
                         unsigned int color_mode);
};