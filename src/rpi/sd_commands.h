/* sd_commands.h - the SD command and response encodings shared by the SD
   card driver (rpi/sdcard.c) and the WiFi SDIO host (wifi/sdio_host.c):
   the host controller's CMDTM field layout, the response-type codes, the
   command/transfer interrupt flags and error masks, and the R1 card-status
   bits.  One copy, so the two drivers cannot drift apart on a load-bearing
   mask. */
#ifndef SD_COMMANDS_H
#define SD_COMMANDS_H

#define SD_CMD_INDEX(a)    ((a) << 24)
#define SD_CMD_TYPE_NORMAL 0x0
#define SD_CMD_TYPE_SUSPEND   (1 << 22)
#define SD_CMD_TYPE_RESUME (2 << 22)
#define SD_CMD_TYPE_ABORT  (3 << 22)
#define SD_CMD_TYPE_MASK    (3 << 22)
#define SD_CMD_ISDATA      (1 << 21)
#define SD_CMD_IXCHK_EN    (1 << 20)
#define SD_CMD_CRCCHK_EN   (1 << 19)
#define SD_CMD_RSPNS_TYPE_NONE   0        /* For no response */
#define SD_CMD_RSPNS_TYPE_136 (1 << 16)   /* For response R2 (with CRC), R3,4 (no CRC) */
#define SD_CMD_RSPNS_TYPE_48  (2 << 16)   /* For responses R1, R5, R6, R7 (with CRC) */
#define SD_CMD_RSPNS_TYPE_48B (3 << 16)   /* For responses R1b, R5b (with CRC) */
#define SD_CMD_RSPNS_TYPE_MASK  (3 << 16)
#define SD_CMD_MULTI_BLOCK (1 << 5)
#define SD_CMD_DAT_DIR_HC  0
#define SD_CMD_DAT_DIR_CH  (1 << 4)
#define SD_CMD_AUTO_CMD_EN_NONE  0
#define SD_CMD_AUTO_CMD_EN_CMD12 (1 << 2)
#define SD_CMD_AUTO_CMD_EN_CMD23 (2 << 2)
#define SD_CMD_BLKCNT_EN      (1 << 1)
#define SD_CMD_DMA          1

#define SD_ERR_CMD_TIMEOUT 0
#define SD_ERR_CMD_CRC     1
#define SD_ERR_CMD_END_BIT 2
#define SD_ERR_CMD_INDEX   3
#define SD_ERR_DATA_TIMEOUT   4
#define SD_ERR_DATA_CRC    5
#define SD_ERR_DATA_END_BIT   6
#define SD_ERR_CURRENT_LIMIT  7
#define SD_ERR_AUTO_CMD12  8
#define SD_ERR_ADMA     9
#define SD_ERR_TUNING      10
#define SD_ERR_RSVD     11

#define SD_ERR_MASK_CMD_TIMEOUT     (1 << (16 + SD_ERR_CMD_TIMEOUT))
#define SD_ERR_MASK_CMD_CRC         (1 << (16 + SD_ERR_CMD_CRC))
#define SD_ERR_MASK_CMD_END_BIT     (1 << (16 + SD_ERR_CMD_END_BIT))
#define SD_ERR_MASK_CMD_INDEX       (1 << (16 + SD_ERR_CMD_INDEX))

#define SD_ERR_MASK_DATA_TIMEOUT    (1 << (16 + SD_ERR_DATA_TIMEOUT))
#define SD_ERR_MASK_DATA_CRC        (1 << (16 + SD_ERR_DATA_CRC))
#define SD_ERR_MASK_DATA_END_BIT    (1 << (16 + SD_ERR_DATA_END_BIT))
#define SD_ERR_MASK_CURRENT_LIMIT   (1 << (16 + SD_ERR_CURRENT_LIMIT))
#define SD_ERR_MASK_AUTO_CMD12      (1 << (16 + SD_ERR_AUTO_CMD12))
#define SD_ERR_MASK_ADMA            (1 << (16 + SD_ERR_ADMA))

/* R1 card-status bits that mean THIS command was refused, or that its data
   is not trustworthy.  The controller's own flags only report whether the
   command completed on the wire - a card can answer cleanly and still be
   telling us it did not take the data.
   COM_CRC_ERROR (23) and ILLEGAL_COMMAND (22) are deliberately NOT here:
   they latch from a PREVIOUS command and are cleared by being read, and the
   CMD8 / inquiry-ACMD41 probing during init sets them legitimately on some
   cards, so testing them would fail perfectly good transfers.
   CARD_IS_LOCKED (25), READY_FOR_DATA and APP_CMD are status, not errors. */
#define SD_R1_OUT_OF_RANGE          (1u << 31)
#define SD_R1_ADDRESS_ERROR         (1u << 30)
#define SD_R1_BLOCK_LEN_ERROR       (1u << 29)
#define SD_R1_WP_VIOLATION          (1u << 26)
#define SD_R1_CARD_ECC_FAILED       (1u << 21)
#define SD_R1_CC_ERROR              (1u << 20)
#define SD_R1_ERROR                 (1u << 19)
#define SD_R1_CSD_OVERWRITE         (1u << 16)
#define SD_R1_DATA_ERROR_MASK       (SD_R1_OUT_OF_RANGE | SD_R1_ADDRESS_ERROR | \
                                     SD_R1_BLOCK_LEN_ERROR | SD_R1_WP_VIOLATION | \
                                     SD_R1_CARD_ECC_FAILED | SD_R1_CC_ERROR | \
                                     SD_R1_ERROR | SD_R1_CSD_OVERWRITE)
#define SD_ERR_MASK_TUNING          (1 << (16 + SD_ERR_TUNING))

#define SD_COMMAND_COMPLETE     1
#define SD_TRANSFER_COMPLETE    (1U << 1)
#define SD_BLOCK_GAP_EVENT      (1U << 2)
#define SD_DMA_INTERRUPT        (1U << 3)
#define SD_BUFFER_WRITE_READY   (1U << 4)
#define SD_BUFFER_READ_READY    (1U << 5)
#define SD_CARD_INSERTION       (1U << 6)
#define SD_CARD_REMOVAL         (1U << 7)
#define SD_CARD_INTERRUPT       (1U << 8)

#define SD_RESP_NONE        SD_CMD_RSPNS_TYPE_NONE
#define SD_RESP_R1          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R1b         (SD_CMD_RSPNS_TYPE_48B | SD_CMD_CRCCHK_EN)
#define SD_RESP_R2          (SD_CMD_RSPNS_TYPE_136 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R3          SD_CMD_RSPNS_TYPE_48
#define SD_RESP_R4          SD_CMD_RSPNS_TYPE_136
#define SD_RESP_R5          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R5b         (SD_CMD_RSPNS_TYPE_48B | SD_CMD_CRCCHK_EN)
#define SD_RESP_R6          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)
#define SD_RESP_R7          (SD_CMD_RSPNS_TYPE_48 | SD_CMD_CRCCHK_EN)

#define SD_DATA_READ        (SD_CMD_ISDATA | SD_CMD_DAT_DIR_CH)
#define SD_DATA_WRITE       (SD_CMD_ISDATA | SD_CMD_DAT_DIR_HC)


/* CONTROL1 software resets */
#define SD_RESET_ALL            (1 << 24)
#define SD_RESET_CMD            (1u << 25)
#define SD_RESET_DAT            (1u << 26)

#endif /* SD_COMMANDS_H */
