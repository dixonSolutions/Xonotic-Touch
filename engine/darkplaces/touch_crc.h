#ifndef TOUCH_CRC_H
#define TOUCH_CRC_H

// Godnotic: com_crc16.c CRC_Block eight bytes at a time (touch_crc.c).
// True with *crc set when it computed the CRC; false: use the byte walk.
qbool TouchCRC_Block(const unsigned char *data, size_t size, unsigned short *crc);
void TouchCRC_Init(void);

#endif
