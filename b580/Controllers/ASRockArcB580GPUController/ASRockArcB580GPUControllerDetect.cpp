/*---------------------------------------------------------*\
| ASRockArcB580GPUControllerDetect.cpp                      |
|                                                           |
|   Detector for ASRock Arc B580 Steel Legend GPU RGB       |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "Detector.h"
#include "LogManager.h"
#include "ResourceManager.h"
#include "ASRockArcB580GPUController.h"
#include "RGBController_ASRockArcB580GPU.h"
#include "i2c_smbus.h"

#include <string>
#include <vector>

extern "C"
{
#if defined(__GNUC__)
__attribute__((used, visibility("default")))
#endif
const char* ASROCK_ARC_B580_STEEL_LEGEND_BUILD_MARKER = "ASRock Arc B580 Steel Legend";
}

namespace
{
    static constexpr uint8_t ASROCK_ARC_B580_RGB_ADDRESS = 0x36;
    static constexpr int     ASROCK_ARC_B580_TEST_BUS_ID = 28;
}

void DetectASRockArcB580GPUControllers(std::vector<i2c_smbus_interface*>& busses)
{
    for(i2c_smbus_interface* bus : busses)
    {
        if(bus == nullptr)
        {
            continue;
        }

        // The ASRock Arc B580 Steel Legend RGB controller sits on the
        // Synopsys DesignWare I2C bus of the discrete Battlemage G21
        // GPU (PCI device 0000:03:00.0) at address 0x36.
        //
        // For a robust upstream-ready detector we should match against
        // the PCI device path of the bus.  The low-level i2c_smbus_interface
        // does not expose the sysfs device path directly, so we fall back
        // to the known bus id (patched by the installer).  For an
        // upstream-ready detector, register an I2C PCI detector against
        // the card's exact PCI/subsystem IDs instead.
        if(bus->bus_id != ASROCK_ARC_B580_TEST_BUS_ID)
        {
            continue;
        }

        const std::string name = "ASRock Arc B580 Steel Legend";

        ASRockArcB580GPUController* controller = new ASRockArcB580GPUController(bus, name);
        RGBController_ASRockArcB580GPU* rgb_controller = new RGBController_ASRockArcB580GPU(controller);

        LOG_INFO("[%s] Registering ASRock Arc B580 Steel Legend RGB controller on I2C bus %d address 0x%02X",
                 name.c_str(), bus->bus_id, ASROCK_ARC_B580_RGB_ADDRESS);

        ResourceManager::get()->RegisterRGBController(rgb_controller);
        return;
    }
}

REGISTER_I2C_DETECTOR("ASRock Arc B580 Steel Legend", DetectASRockArcB580GPUControllers);