#ifndef AUN_EMULATOR_H
#define AUN_EMULATOR_H

#include <stdint.h>

#define AUN_CMD_FIRST        30u
#define AUN_CMD_INIT         30u
#define AUN_CMD_STATUS       31u
#define AUN_CMD_TX           32u
#define AUN_CMD_TX_POLL      33u
#define AUN_CMD_RX_OPEN      34u
#define AUN_CMD_RX_POLL      35u
#define AUN_CMD_RX_CLOSE     36u
#define AUN_CMD_BCAST        37u
#define AUN_CMD_IMMEDIATE    38u
#define AUN_CMD_MAP_ADD      39u
#define AUN_CMD_TEST         40u
#define AUN_CMD_SET_MACHINE  41u
#define AUN_CMD_RX_DONE      42u
#define AUN_CMD_IMM_POLL     43u
#define AUN_CMD_IMM_REPLY    44u
#define AUN_CMD_LAST         44u

#include <stddef.h>
void aun_emulator_init(uint8_t instance, uint8_t address);
void aun_status_text(char *buf, size_t size);
void aun_emulator_command(uint32_t command_pointer, uint32_t addr);

#endif
