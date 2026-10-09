/* Host stub of the TinyUSB MSC host API that usb_storage.c uses; the fake
   drives behind it are in test_usb_storage.c. */
#ifndef STUB_TUSB_H
#define STUB_TUSB_H

#include <stdbool.h>
#include <stdint.h>

enum { MSC_CSW_STATUS_PASSED = 0, MSC_CSW_STATUS_FAILED = 1 };

typedef struct {
   uint32_t signature, tag, data_residue;
   uint8_t  status;
} msc_csw_t;

typedef struct {
   uint8_t lun;
   uint32_t total_bytes;
} msc_cbw_t;

typedef struct {
   const msc_cbw_t *cbw;
   const msc_csw_t *csw;
   void *scsi_data;
   uintptr_t user_arg;
} tuh_msc_complete_data_t;

typedef bool (*tuh_msc_complete_cb_t)(uint8_t dev_addr, tuh_msc_complete_data_t const *cb_data);

bool     tuh_msc_mounted(uint8_t dev_addr);
uint32_t tuh_msc_get_block_count(uint8_t dev_addr, uint8_t lun);
uint32_t tuh_msc_get_block_size(uint8_t dev_addr, uint8_t lun);
bool     tuh_msc_read10(uint8_t dev_addr, uint8_t lun, void *buffer, uint32_t lba,
                        uint16_t block_count, tuh_msc_complete_cb_t complete_cb, uintptr_t arg);
bool     tuh_vid_pid_get(uint8_t dev_addr, uint16_t *vid, uint16_t *pid);
void     tuh_int_handler(uint8_t rhport, bool in_isr);
void     tuh_task(void);

void tuh_msc_mount_cb(uint8_t dev_addr);
void tuh_msc_umount_cb(uint8_t dev_addr);

#endif
