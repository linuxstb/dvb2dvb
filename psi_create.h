#ifndef _PSI_CREATE_H
#define _PSI_CREATE_H

#include <stdint.h>
#include "dvb2dvb.h"
#include "ringbuffer.h"

struct chunk_t {
  int len;
  char buf[128];
};

void recode_text(uint8_t* dst, uint8_t *src, int max_size, struct chunk_t* overflow);
int copy_eit_descriptors(uint8_t *dst, uint8_t *src, int len, int onid);
int rewrite_eit(struct section_t* new_eit, struct section_t* eit, int old_service_id, int new_service_id, int onid, struct mux_t* mux);
int copy_pmt_descriptors(uint8_t *dst, uint8_t *src, int len);
int copy_sdt_descriptors(uint8_t *dst, uint8_t *src, int len, int onid);
void create_nit(struct section_t* nitsec, struct mux_t* mux);
void create_sdt(struct section_t* sdtsec, struct mux_t* mux);
void create_pmt(struct service_t* sv);
void create_ait(struct service_t* sv);
void create_pat(struct section_t *patsec, struct mux_t* mux);
int copy_section(uint8_t* tsbuf, struct section_t* section, int pid);
int write_section(struct ringbuffer_t* rb, struct section_t* section, int pid);

#endif
