#ifndef PCODE_PROTOCOL_H
#define PCODE_PROTOCOL_H

#define PCODE_MAILBOX_OFFSET    0x138124
#define PCODE_DATA0_OFFSET      0x138128
#define PCODE_DATA1_OFFSET      0x13812C

#define PCODE_READY             (1u << 31)
#define PCODE_MB_COMMAND_MASK   0x000000FFu
#define PCODE_MB_PARAM1_MASK    0x0000FF00u
#define PCODE_MB_PARAM2_MASK    0x00FF0000u
#define PCODE_ERROR_MASK        0x000000FFu

#define PCODE_POWER_SETUP       0x7C
#define PCODE_THERMAL_INFO      0x25
#define PCODE_LATE_BINDING      0x5C
#define PCODE_FREQUENCY_CONFIG  0x6e
#define FAN_SPEED_CONTROL       0x7D
#define DGFX_PCODE_STATUS       0x7E

#define FSC_READ_NUM_FANS      0x4
#define FSC_PROBE_MIN          0x0
#define FSC_PROBE_MAX          0xF

#define GET_CAPABILITY_STATUS  0x0
#define GET_VERSION_LOW        0x1
#define GET_VERSION_HIGH       0x2

#define V1_FAN_SUPPORTED       (1u << 0)
#define VR_PARAMS_SUPPORTED    (1u << 3)
#define V1_FAN_PROVISIONED     (1u << 16)
#define VR_PARAMS_PROVISIONED  (1u << 19)

#define FAN_TABLE              1
#define VR_CONFIG              2

#define READ_THERMAL_LIMITS    0x0
#define READ_THERMAL_CONFIG    0x1
#define READ_THERMAL_DATA      0x2

#define POWER_SETUP_READ_I1    0x4
#define POWER_SETUP_WRITE_I1   0x5

#define DGFX_GET_INIT_STATUS   0x0
#define DGFX_INIT_STATUS_COMPLETE 0x1

#define PCODE_SUCCESS              0x0
#define PCODE_ILLEGAL_CMD          0x1
#define PCODE_TIMEOUT              0x2
#define PCODE_ILLEGAL_DATA         0x3
#define PCODE_ILLEGAL_SUBCOMMAND   0x4
#define PCODE_LOCKED               0x6
#define PCODE_GT_RATIO_OUT_OF_RANGE 0x10
#define PCODE_REJECTED             0x11

#define PCODE_MBOX(mbcmd, param1, param2) \
    (((uint32_t)(mbcmd) & 0xFF) | \
     (((uint32_t)(param1) & 0xFF) << 8) | \
     (((uint32_t)(param2) & 0xFF) << 16))

#define PCODE_TIMEOUT_US    1000

#define BMG_FAN_MAX         3

#endif
