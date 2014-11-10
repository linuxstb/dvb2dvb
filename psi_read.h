#ifndef _PSI_READ_H
#define _PSI_READ_H

#include <stdint.h>
#include "dvb2dvb.h"

int process_pat(struct service_t* sv, uint8_t* buf);
int process_sdt(struct service_t* sv);
int bcd2dec(unsigned char buf);
int process_pmt(struct service_t* sv);
void process_section(struct section_t* next, struct section_t* curr, uint8_t* buf, int table_id);

#endif
