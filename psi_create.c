/*

dvb2dvb - combine multiple SPTS to a MPTS

Copyright (C) 2014 Dave Chapman

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

*/

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "dvb2dvb.h"
#include "psi_create.h"
#include "ringbuffer.h"
#include "crc32.h"

static void put_u16be(uint8_t *buf, uint16_t x)
{
  buf[0] = (x & 0xff00) >> 8;
  buf[1] = x & 0xff;
}

static void put_u32be(uint8_t *buf, uint32_t x)
{
  buf[0] = (x & 0xff000000) >> 24;
  buf[1] = (x & 0xff0000) >> 16;
  buf[2] = (x & 0xff00) >> 8;
  buf[3] = (x & 0xff);
}

void recode_text(uint8_t* dst, uint8_t *src, int max_size, struct chunk_t* overflow)
{
  // TODO: Check existing content, and do not add codepage
  // info if it is all ASCII.  Also do not add if there is already
  // a code page indicator (first character <= 0x1f).
  //fprintf(stderr,"Recoding text - first char is 0x%02x\n",src[1]);
  int len = *src;
  if (len==0) {
    *dst = 0; // Nothing to copy.
  } else {
    int len = *src;
    uint8_t *p = dst+1;
    *p++ = 0x10;
    *p++ = 0x00;
    *p++ = 0x01;
    if (overflow && (overflow->len)) {
//      fprintf(stderr,"Copying overflow to start of output descriptor - len=%d\n",overflow->len);
      memcpy(p, &overflow->buf[0], overflow->len);
      p += overflow->len;
      overflow->len = 0;
    }
    if (overflow) {
      int space_left = max_size - (int)(p - dst - 1);
      //fprintf(stderr,"max_size=%d, space_left=%d\n",max_size,space_left);
      int to_copy = MIN(space_left, len);
      memcpy(p,src+1,to_copy);
      p += to_copy;
      if (to_copy < len) {
        //fprintf(stderr,"Overflow - saving %d bytes\n",len-to_copy);
        memcpy(&overflow->buf[0], src + to_copy + 1, len-to_copy);
        overflow->len = len-to_copy;
      }
    } else {
      memcpy(p,src+1,len);
      p += len;
    }
    *dst = p-dst-1;
  }
}

int copy_eit_descriptors(uint8_t *dst, uint8_t *src, int len, int onid)
{
  int i = 0;
  uint8_t *p = dst;
  struct chunk_t overflow;
  overflow.len = 0;

  while (i < len) {
    int descriptor_tag=src[i];
    int descriptor_length=src[i+1];

    int processed = 0;
    if (descriptor_tag == 0x4d) { // short_event_descriptor
      if ((onid == 0x0001)|| (onid==0x0085))  {  // Astra 19E providers seem to incorrectly use iso-8859-1 by default
        *p++ = 0x4d;
        uint8_t *p1 = p++; // Save position of descriptor length
        memcpy(p,src+i+2,3); // iso_639 language code
        p += 3;
        uint8_t *q = src+i+5;
        // event_name
        recode_text(p, q, 255, NULL);
        p += *p + 1; q += *q + 1;
        // text
        recode_text(p, q, 255, NULL);
        p += *p + 1;

        *p1 = p-p1-1;
        processed = 1;
      }
    } else if (descriptor_tag == 0x4e) { // extended_event_descriptor
      if ((onid == 0x0001)|| (onid==0x0085))  {  // Astra 19E providers seem to incorrectly use iso-8859-1 by default
        uint8_t *q = src + i;
        uint8_t *p1 = p + 1;  // Save position of descriptor length
        memcpy(p, q, 6);
        p += 6; q += 6;
        memcpy(p, q, *q + 1);  // items
        p += *p + 1; q += *q + 1;
        // text

        int max_size = 253 - (int)(p-p1-1) - 1;  // C+ uses a max descriptor length of 253 - why not 255?
        //fprintf(stderr,"Calling recode_text - descriptor_length=%d, *q=%d, p-p1=%d, max_size=%d                 \n",descriptor_length,*q,(int)(p-p1),max_size);
        recode_text(p, q, max_size, &overflow);
        //fprintf(stderr,"Recorded - overflow.len=%d\n",overflow.len);
        p += *p + 1;

	//fprintf(stderr,"Setting descriptor length = %d               \n",(int)(p-p1-1));   
        *p1 = p-p1-1;
        processed = 1;
      }
    }
    if (!processed) {
      memcpy(p, src+i, descriptor_length + 2);
      p += descriptor_length + 2;
    }
    i += descriptor_length + 2;
  }

  return p - dst;
}

int rewrite_eit(struct section_t* new_eit, struct section_t* eit, int old_service_id, int new_service_id, int onid, struct mux_t* mux)
{
  uint8_t *buf = &eit->buf[0];
  uint8_t *newbuf = &new_eit->buf[0];

  int service_id = (buf[3] << 8) | buf[4];
  if (service_id == old_service_id) {
    memcpy(newbuf, buf, 14);
    put_u16be(newbuf+3,  new_service_id);
    put_u16be(newbuf+8,  mux->tsid);
    put_u16be(newbuf+10, mux->onid);

    uint8_t *p = newbuf + 14;
    uint8_t *q = buf+14;
    //Assume 1 event for now.
    //   while (i < eit->length) {
      memcpy(p, q, 10);
      p += 10; q += 10;
      int descriptors_loop_length = ((q[0]&0x0f)<<8) | q[1];
      int new_descriptors_loop_length = copy_eit_descriptors(p+2, q+2, descriptors_loop_length, onid);
      int running_status = (q[0] & 0xe0) >> 5;
      int free_CA_mode = 0;
      put_u16be(p, (running_status << 13) | (free_CA_mode << 12) | new_descriptors_loop_length);
      p += new_descriptors_loop_length + 2;
    //    }

    new_eit->length = p - newbuf + 4;
    // Back-fill section_lenth
    put_u16be(newbuf+1, 0xf000 | (new_eit->length - 3));

    uint32_t crc = psi_crc32(newbuf, new_eit->length - 4, 0xffffffff);
    put_u32be(p, crc);

    return 0;
  } else {
    return -1;
  }
}

int copy_pmt_descriptors(uint8_t *dst, uint8_t *src, int len)
{
  int i = 0;
  uint8_t *p = dst;

  while (i < len) {
    int descriptor_tag=src[i];
    int descriptor_length=src[i+1];

    //fprintf(stderr,"descriptor_tag=0x%02x, len=%d\n",descriptor_tag,descriptor_length);
    if ((descriptor_tag != 0x09) &&  // ca descriptor
        (descriptor_tag != 0xc0)     // private descriptor used by C+ Spain
       ) {
      memcpy(p, src+i, descriptor_length + 2);
      p += descriptor_length + 2;
    }
    i += descriptor_length + 2;
  }

  return p - dst;
}

int copy_sdt_descriptors(uint8_t *dst, uint8_t *src, int len, int onid)
{
  int i = 0;
  uint8_t *p = dst;

  while (i < len) {
    int descriptor_tag=src[i];
    int descriptor_length=src[i+1];

    //fprintf(stderr,"descriptor_tag=0x%02x, len=%d\n",descriptor_tag,descriptor_length);
    // Only copy the service_descriptor
    if (descriptor_tag == 0x48) {
      if ((onid == 0x0001)|| (onid==0x0085))  {  // Astra 19E providers seem to incorrectly use iso-8859-1 by default
        // insert 0x10 0x00 0x01 before any text strings to indicate iso-8859-1
        *p++ = 0x48;
        uint8_t *p1 = p++; // Save position of descriptor length
        *p++ = src[i+2];  // stream_type
        uint8_t *q = src+i+3;
        // provider_name
        recode_text(p, q, 255, NULL);
        p += *p + 1; q += *q + 1;
        // service_name
        recode_text(p, q, 255, NULL);
        p += *p + 1;

        *p1 = p-p1-1;
      } else {
        memcpy(p, src+i, descriptor_length + 2);
        p += descriptor_length + 2;
      }
    }
    i += descriptor_length + 2;
  }

  return p - dst;
}

/* Create NIT

A UK Freeview NIT contains no network descriptors, and the following transport stream descriptors:

0x41 - service_list_descriptor (list of service ids and service types 
0x51 - terrstrial_delivery_descriptor
0x7f - extension_descriptor 
                  DVB-DescriptorTag: 127 (0x7f)  [= extension_descriptor]
                  descriptor_length: 5 (0x05)
                  descriptor_tag_extension: 9 (0x09)
                  selector_bytes:
                       0000:  47 42 52 f8                                        GBR.
0x5f - private_data_specifier_descriptor
                  DVB-DescriptorTag: 95 (0x5f)  [= private_data_specifier_descriptor]
                  descriptor_length: 4 (0x04)
                  PrivateDataSpecifier: 9018 (0x0000233a)  [= Independent Television Commission<A0>]
0x83 - logical_channel_numbers_descriptor
0x25 - metadata_pointer_descriptor
                  MPEG-DescriptorTag: 37 (0x25)  [= metadata_pointer_descriptor]
                  descriptor_length: 33 (0x21)
                       0000:  01 01 3f ff 1f 57 40 0d  0c 64 6d 6f 6c 2e 63 6f   ..?..W@..dmol.co
                       0010:  2e 75 6b 2f 31 0a 64 6d  6f 6c 2e 63 6f 2e 75 6b   .uk/1.dmol.co.uk
                       0020:  03   
*/
void create_nit(struct section_t* nitsec, struct mux_t* mux)
{
  struct service_t* services = mux->services;
  uint8_t *nit = &nitsec->buf[0];
  uint8_t *p;
  int i,j;

  int version_number = 1;
  int current_next_indicator = 1;

  memset(nit,0,sizeof(nitsec->buf));

  nit[0] = 0x40; // table_id
  // skip section_length - 2 bytes
  put_u16be(nit+3,mux->nid);
  nit[5] = 0xc0 | (version_number << 1) | current_next_indicator;
  nit[6] = 0x00;  // section_number
  nit[7] = 0x00;  // last_section_number
  put_u16be(nit+8,0xf000); // network_descriptors_length=0
  // nit+10,11 is transport_stream_loop_length
  i = 12;

  // Transport Stream loop - just one entry.
  put_u16be(nit+i,mux->tsid);
  put_u16be(nit+i+2,mux->onid);
  // nit+i+4,5 = transport_descriptors_length

  // Transport descriptors
  p = nit+i+6;

  // service_list_descriptor
  p[0] = 0x41;
  p[1] = mux->nservices * 3;
  for (j=0;j<mux->nservices;j++) {
    put_u16be(p+2+3*j, services[j].new_service_id);
    p[2+3*j+2] = services[j].service_type;
  }
  p += 2+mux->nservices*3;

  // terrestrial_delivery_descriptor
  int centre_frequency = 802000 * 100;
  int bandwidth = 0; // 8MHz
  int priority = 1;
  int Time_Slicing_indicator = 1;
  int MPE_FEC_indicator = 1;
  int reserved1 = 3;
  int constellation = 2; // QAM-64
  int hierarchy_information = 0; 
  int code_rate_HP_stream = 1; // 2/3
  int code_rate_LP_stream = 0;
  int guard_interval = 3; // 1/4
  int transmission_mode = 1; // 8k
  int other_frequency_flag = 1;

  p[0] = 0x5a;
  p[1] = 11;
  put_u32be(p+2,centre_frequency);
  p[6] = (bandwidth << 5) | (priority << 4) | (Time_Slicing_indicator << 3) | (MPE_FEC_indicator << 2) | reserved1;
  p[7] = (constellation << 6) | (hierarchy_information << 3) | code_rate_HP_stream;
  p[8] = (code_rate_LP_stream << 5) | (guard_interval << 3) | (transmission_mode << 1) | other_frequency_flag;
  p[9] = 0xff;
  p[10] = 0xff;
  p[11] = 0xff;
  p[12] = 0xff;
  p += 13;

  // private_data_specifier_description
  // I don't know the meaning of this, but it is sent on real Freeview muxes, and my
  // TV doesn't use the LCNs without it.
  p[0] = 0x5f;
  p[1] = 4;
  put_u32be(p+2,mux->onid);
  p += 6;

  // logical_channel_numbers descriptor
  p[0] = 0x83;
  p[1] = mux->nservices * 4;
  for (j=0;j<mux->nservices;j++) {
    put_u16be(p+2+4*j, services[j].new_service_id);
    put_u16be(p+2+4*j+2, 0xfc00 | services[j].lcn);
  }
  p += 2+mux->nservices*4;

  // Back-fill transport_stream_loop_length
  put_u16be(nit+10, 0xf000 | (p - (nit+i+6) - 4));

  // Back-fill transport_descriptors_length
  put_u16be(nit+i+4, 0xf000 | (p - (nit+i+6)));

  // Back-fill section_lenth
  int section_length = (p-nit) - 3 + 4;
  put_u16be(nit+1, 0xf000 | section_length);

  uint32_t crc = psi_crc32(nit, (p-nit), 0xffffffff);

  put_u32be(p, crc);

  nitsec->length = p - nit + 4;
}


void create_sdt(struct section_t* sdtsec, struct mux_t* mux)
{
  struct service_t *services = mux->services;
  uint8_t *sdt = &sdtsec->buf[0];
  int i,j,k;

  int version_number = 1;
  int current_next_indicator = 1;

  memset(sdt,0,sizeof(sdtsec->buf));

  sdt[0] = 0x42; // table_id
  // skip section_length - 2 bytes
  put_u16be(sdt+3,mux->tsid);
  sdt[5] = 0xc0 | (version_number << 1) | current_next_indicator;
  sdt[6] = 0x00;  // section_number
  sdt[7] = 0x00;  // last_section_number
  put_u16be(sdt+8,mux->onid);
  sdt[10] = 0xff; // reserved

  i = 11;
  for (k=0;k<mux->nservices;k++) {
    uint8_t *buf = &services[k].sdt.buf[0];
    j = 11;
    while (j < services[k].sdt.length - 4) {
      int service_id = (buf[j] << 8) | buf[j+1];
      //fprintf(stderr,"Processing SDT: i=%d, service=%d\n",i,service_id);
      int EIT_schedule_flag = 0;
      int EIT_present_following_flag = 1;
      int running_status = (buf[j+3]&0xe0) >> 5;
      int free_CA_mode = 0;
      int descriptors_loop_length=((buf[j+3]&0x0f)<<8) | buf[j+4];

      //fprintf(stderr,"create_sdt: service_id=%d, services[%d].service_id=%d\n",service_id,k,services[k].service_id);
      if (service_id == services[k].service_id) {
        put_u16be(sdt+i,services[k].new_service_id);
        sdt[i+2] = 0xfc | (EIT_schedule_flag << 1) | EIT_present_following_flag;

        int new_descriptors_loop_length = copy_sdt_descriptors(sdt+i+5,&services[k].sdt.buf[j+5], descriptors_loop_length, services[k].onid);

        put_u16be(sdt+i+3, (running_status << 13) | (free_CA_mode << 12) | new_descriptors_loop_length);
        i += 5 + new_descriptors_loop_length;
      }
      j += 5 + descriptors_loop_length;
    }
  }

  int section_length = i - 3 + 4;
  put_u16be(sdt+1, 0xe000 | section_length);

  uint32_t crc = psi_crc32(sdt, i, 0xffffffff);

  put_u32be(sdt+i, crc);

  sdtsec->length = i + 4;

  //  check_sdt(&sv->new_sdt);
}

void create_pmt(struct service_t* sv)
{
  uint8_t *pmt = &sv->new_pmt.buf[0];

  int version_number = 1;
  int current_next_indicator = 1;

  memset(pmt,0,sizeof(sv->new_pmt.buf));

  pmt[0] = 0x02; // table_id
  // skip section_length - 2 bytes
  put_u16be(pmt+3,sv->new_service_id);
  pmt[5] = 0xc0 | (version_number << 1) | current_next_indicator;
  pmt[6] = 0x00;  // section_number
  pmt[7] = 0x00;  // last_section_number
  put_u16be(pmt+8,0xe000 | sv->pid_map[sv->pcr_pid]);

  int program_info_length = ((sv->pmt.buf[10] & 0x0f) << 8) | sv->pmt.buf[11];
  put_u16be(pmt+10,0xf000 | program_info_length);  // program_info_length

  int i = 12;
  if (program_info_length) {
    memcpy(pmt + 12, &sv->pmt.buf[12], program_info_length);
    i += program_info_length;
  }
  int j = i;

  while ( j < sv->pmt.length - 4) {
    int stream_type = sv->pmt.buf[j];
    int pid = ((sv->pmt.buf[j+1]&0x1f) << 8) | sv->pmt.buf[j+2];
    int ES_info_length = ((sv->pmt.buf[j+3] & 0x0f) << 8) | sv->pmt.buf[j+4];

    if (sv->pid_map[pid]) {
      pmt[i] = stream_type;
      put_u16be(pmt+i+1, 0xe000 | sv->pid_map[pid]);

      // TODO: Only copy interesting descriptors
      int new_ES_info_length = copy_pmt_descriptors(pmt+i+5,&sv->pmt.buf[j+5], ES_info_length);
      put_u16be(pmt+i+3, 0xf000 | new_ES_info_length);

      i += 5 + new_ES_info_length;
    }
    j += 5 + ES_info_length;
  }

  /* hbbtv - insert reference to AIT stream */
  if (sv->ait_pid) {
    pmt[i++] = 0x05;
    put_u16be(pmt+i, 0xe000 | sv->ait_pid); i += 2;
    put_u16be(pmt+i, 0xf000 | 5); i += 2;
    pmt[i++] = 0x6f;  // application_signalling_descriptor
    pmt[i++] = 3; // length
    put_u16be(pmt+i, 0x8000 | sv->hbbtv.application_type); i += 2;
    pmt[i++] = sv->hbbtv.AIT_version_number;
  }

  int section_length = i - 3 + 4;
  put_u16be(pmt+1, 0xb000 | section_length);

  uint32_t crc = psi_crc32(pmt, i, 0xffffffff);

  put_u32be(pmt+i, crc);

  sv->new_pmt.length = i + 4;
}

void create_pat(struct section_t *patsec, struct mux_t* mux)
{
  struct service_t *services = mux->services;
  uint8_t *pat = &patsec->buf[0];
  int i,j;

  int section_length = 5 + (mux->nservices * 4) + 4;
  int version_number = 1;
  int current_next_indicator = 1;

  memset(pat,0,sizeof(struct section_t));

  pat[0]  = 0x00;  // table_id
  put_u16be(pat+1, 0x8000 | section_length);
  put_u16be(pat+3, mux->tsid);
  pat[5] = 0xc0 | (version_number << 1) | current_next_indicator;
  pat[6] = 0x00;  // section_number
  pat[7] = 0x00;  // last_section_number

  i = 8;
  for (j=0;j<mux->nservices;j++) {
    put_u16be(pat+i,services[j].new_service_id); i += 2;
    put_u16be(pat+i,0xe000 | services[j].new_pmt_pid); i += 2;
  }

  uint32_t crc = psi_crc32(pat, i, 0xffffffff);

  put_u32be(pat+i, crc);

  patsec->length = i + 4;
}

void create_ait(struct service_t* sv)
{
  uint8_t *ait = &sv->ait.buf[0];
  int i,j,k;
  int current_next_indicator = 1;

  memset(ait,0,sizeof(sv->ait.buf));

  ait[0] = 0x74; // table_id
  // skip section_length - 2 bytes
  put_u16be(ait+3,sv->hbbtv.application_type);
  ait[5] = 0xc0 | (sv->hbbtv.AIT_version_number << 1) | current_next_indicator;
  ait[6] = 0x00;  // section_number
  ait[7] = 0x00;  // last_section_number
  
  // Common descriptors (empty)
  ait[8] = 0xf0;
  ait[9] = 0;

  // Application loop
  i = 12;
  j = i;

  put_u32be(ait+i, 0x13); i += 4;  // organisation_id
  put_u16be(ait+i, 0x0001) ; i += 2;  // application_id = 1 (unsigned application)
  ait[i++] = 0x01;  // application_control_code = autostart application
  i += 2; // descriptors loop length
  k = i;
  // application descriptors

  // 1) transport protocol descriptor
  int url_len = strlen(sv->hbbtv.url);
  ait[i++] = 2; // transport_protocol_descriptor
  ait[i++] = url_len + 5; // descriptor_length
  put_u16be(ait+i, 0x0003); i += 2; // protocol_id = Transport via HTTP over the interaction channel 
  ait[i++] = 0; // tranport_protocol_label
  ait[i++] = url_len;
  memcpy(ait+i, sv->hbbtv.url, url_len); i += url_len;
  ait[i++] = 0;

  // 2) application_descriptor  
  ait[i++] = 0; // application_descriptor
  ait[i++] = 9;
  ait[i++] = 5; // application_profiles_length
  put_u16be(ait+i, 0x0001); i += 2;
  ait[i++] = 0x00;
  ait[i++] = 0x05;
  ait[i++] = 0x00;
  ait[i++] = (0 << 7) | (0x3 << 5) | 0x1f;
  ait[i++] = 2; // application_priority
  ait[i++] = 0; // transport_protocol_label

  // 3) application_name_descriptor
  char* application_name = "hbbtv application";
  int name_len = strlen(application_name);
  ait[i++] = 0x01; // application_name_descriptor
  ait[i++] = name_len + 4;
  memcpy(ait+i,"eng",3); i += 3;
  ait[i++] = name_len;
  memcpy(ait+i,application_name,name_len); i += name_len;

  // 4) simple_application_location_descriptor
  int path_len = strlen(sv->hbbtv.initial_path);
  ait[i++] = 0x15; // simple_application_location_descriptor
  ait[i++] = path_len;
  memcpy(ait+i,sv->hbbtv.initial_path,path_len); i += path_len;
  
  put_u16be(ait+k-2,0xf000 | (i-k));  // descriptors loop length
  put_u16be(ait+j-2,0xf000 | (i-j));  // length of application 

  int section_length = i - 3 + 4;
  put_u16be(ait+1, 0xf000 | section_length);

  uint32_t crc = psi_crc32(ait, i, 0xffffffff);

  put_u32be(ait+i, crc);

  sv->ait.length = i + 4;
}

int copy_section(uint8_t* tsbuf, struct section_t* section, int pid)
{
  int i;
  uint8_t *buf = &section->buf[0];
  int n = section->length;
  int bytes_written = 0;
  int num_packets = 0;

  while (n > 0) {
    tsbuf[0] = 0x47;
    if (bytes_written == 0) {
      put_u16be(tsbuf+1,0x4000 | pid);
      tsbuf[4] = 0;
      i = 5;
    } else {
      put_u16be(tsbuf+1,pid);
      i = 4;
    }
    tsbuf[3] = 0x10 | section->cc;
    section->cc = (section->cc + 1) % 16;

    int to_write = MIN(188-i,n);
    memcpy(tsbuf+i, buf+bytes_written, to_write);
    if (i+to_write < 188) {
      memset(tsbuf+i+to_write,0xff,188-(i+to_write));
    }

    tsbuf += 188;
    bytes_written += to_write;
    n -= to_write;
    num_packets++;
  }
  return num_packets;
}

int write_section(struct ringbuffer_t* rb, struct section_t* section, int pid)
{
  int i;
  uint8_t *buf = &section->buf[0];
  uint8_t tsbuf[188];
  int n = section->length;
  int bytes_written = 0;
  int num_packets = 0;

  tsbuf[0] = 0x47;

  while (n > 0) {
    if (bytes_written == 0) {
      put_u16be(tsbuf+1,0x4000 | pid);
      tsbuf[4] = 0;
      i = 5;
    } else {
      put_u16be(tsbuf+1,pid);
      i = 4;
    }
    tsbuf[3] = 0x10 | section->cc;
    section->cc = (section->cc + 1) % 16;

    int to_write = MIN(188-i,n);
    memcpy(tsbuf+i, buf+bytes_written, to_write);
    if (i+to_write < 188) {
      memset(tsbuf+i+to_write,0xff,188-(i+to_write));
    }

    rb_write(rb, tsbuf, 188);
    bytes_written += to_write;
    n -= to_write;
    num_packets++;
  }
  return num_packets;
}

