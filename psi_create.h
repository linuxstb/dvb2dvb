#ifndef _PSI_CREATE_H
#define _PSI_CREATE_H

#include <stdint.h>
#include "dvb2dvb.h"

struct chunk_t {
  int len;
  char buf[128];
};

void recode_text(uint8_t* dst, uint8_t *src, int max_size, struct chunk_t* overflow);
int copy_eit_descriptors(uint8_t *dst, uint8_t *src, int len, int onid);
int rewrite_eit(struct section_t* new_eit, struct section_t* eit, int old_service_id, int new_service_id, int onid);
int copy_pmt_descriptors(uint8_t *dst, uint8_t *src, int len);
int copy_sdt_descriptors(uint8_t *dst, uint8_t *src, int len, int onid);
void create_nit(struct section_t* nitsec, struct service_t* services, int nservices);
void create_sdt(struct section_t* sdtsec, struct service_t* services, int nservices);
void create_pmt(struct service_t* sv);
void create_pat(struct section_t *patsec, struct service_t *services, int nservices);
int copy_section(uint8_t* tsbuf, struct section_t* section, int pid);
int write_section(int fd, struct section_t* section, int pid);

#endif
