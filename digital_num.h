
#ifndef __SEG_H
#define __SEG_H

#include <stdint.h>

void printNum(uint16_t num);
void Seg_Scan(void);
void Seg_Hardware_Init();
void Output_Segment_Data(uint8_t font_data);
extern volatile uint8_t num_brightness;

#endif