#ifndef ECONET_EMULATOR_H
#define ECONET_EMULATOR_H

#include <stdint.h>

#define ECO_CMD_FIRST        30u
#define ECO_CMD_INIT         30u
#define ECO_CMD_STATUS       31u
#define ECO_CMD_TX           32u
#define ECO_CMD_TX_POLL      33u
#define ECO_CMD_RX_OPEN      34u
#define ECO_CMD_RX_POLL      35u
#define ECO_CMD_RX_CLOSE     36u
#define ECO_CMD_BCAST        37u
#define ECO_CMD_IMMEDIATE    38u
#define ECO_CMD_MAP_ADD      39u
#define ECO_CMD_TEST         40u
#define ECO_CMD_SET_MACHINE  41u
#define ECO_CMD_RX_DONE      42u
#define ECO_CMD_IMM_POLL     43u
#define ECO_CMD_IMM_REPLY    44u
#define ECO_CMD_LAST         44u

#include <stddef.h>
void econet_emulator_init(void);
void econet_status_text(char *buf, size_t size);
void econet_emulator_command(uint32_t command_pointer, uint32_t addr);

#endif
