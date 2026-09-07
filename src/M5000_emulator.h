void M5000_emulator_init(uint8_t instance, uint8_t address);
uint8_t M5000_emulator_read_instance(void);

/* True while a stopped recording's WAV is still being written to the card
   (the flush runs one slice per poll pass for seconds) and host_path names
   that file, or a directory holding it.  beeb_path_busy() asks, so a
   WebDAV/MTP DELETE, MOVE or PUT cannot free the cluster chain the open
   FIL is extending (FF_FS_LOCK=0 gives no protection of its own). */
bool M5000_recording_path_busy(const char *host_path);
