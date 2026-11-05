UINT8 MultiPCMRead();
void MultiPCMWrite(INT32 offset, UINT8 data);
void MultiPCMSetBank(UINT32 leftoffs, UINT32 rightoffs);
void MultiPCMSetVolume(double vol);
void MultiPCMInit(INT32 clock, UINT8 *SndROM, INT32 bAdd);
void MultiPCMExit();
void MultiPCMReset();
void MultiPCMSetMonoMode(INT32 monomode);
void MultiPCMUpdate(INT16 *buffer, INT32 samples);
void MultiPCMScan(INT32 nAction, INT32 *pnMin);

// FBNeo PS5 2.3: two chips (the calls above are chip 0's)
void MultiPCMInitChip(INT32 nChip, INT32 clock, UINT8 *SndROM, INT32 bAdd);
void MultiPCMWriteChip(INT32 nChip, INT32 offset, UINT8 data);
void MultiPCMSetBankChip(INT32 nChip, UINT32 leftoffs, UINT32 rightoffs);
void MultiPCMSetVolumeChip(INT32 nChip, double vol);
void MultiPCMResetChip(INT32 nChip);
void MultiPCMUpdateChip(INT32 nChip, INT16 *buffer, INT32 samples);
void MultiPCMScanChip(INT32 nChip, INT32 nAction, INT32 *pnMin);
// 1: as MAME's current multipcm in its key on (it starts the sample again) and its channel order (Sega Model 2)
void MultiPCMSetMameModeChip(INT32 nChip, INT32 on);
// 2.3 (Model 2): the chip's sound made up to pos / frame_len of this frame (the caller's clock: its CPU's cycles), before
// a write (MAME's stream update); MultiPCMUpdateChip at the frame's end makes the rest
void MultiPCMSyncChip(INT32 nChip, INT64 pos, INT64 frame_len);
