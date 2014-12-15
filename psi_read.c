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
#include <stdlib.h>
#include "dvb2dvb.h"
#include "psi_create.h"
#include "crc32.h"

static char* RST[] = {
  "Undefined",
  "Not running",
  "Starts in a few seconds",
  "Pausing",
  "Running",
  "Reserved",
  "Reserved",
  "Reserved"
};

static char pts_text[30];
char* pts2hmsu(uint64_t pts,char sep) {
  int h,m,s,u;

  pts/=90*300; // Convert to milliseconds
  h=(pts/(1000*60*60));
  m=(pts/(1000*60))-(h*60);
  s=(pts/1000)-(h*3600)-(m*60);
  u=pts-(h*1000*60*60)-(m*1000*60)-(s*1000);

  sprintf(pts_text,"%d:%02d:%02d%c%03d",h,m,s,sep,u);
  return(pts_text);
}

int process_pat(struct service_t* sv, uint8_t* buf)
{
  int i = 5;

  if (buf[4] != 0) {
    fprintf(stderr,"Unsupported PAT format - pointer_to_data != 0 (%d)\n",buf[4]);
    return -1;;
  }

  int table_id = buf[i++];
  int section_length = ((buf[i]&0x0f) << 8) | buf[i+1]; i += 2;
  if (section_length > 180) { 
    fprintf(stderr,"Multi-packet PAT not supported\n");
    return -1;
  }

  int transport_stream_id = (buf[i] << 8) | buf[i+1]; i += 2;
  int version_number = (buf[i]&0x3e) >> 1;
  int current_next_indicator = buf[i++] & 0x01;
  int section_number = buf[i++];
  int last_section_number = buf[i++];
  if (last_section_number != 0) {
    fprintf(stderr,"Multi-packet PAT not supported (last_section_number = %d)\n",last_section_number);
    return -1;
  }

  //fprintf(stderr,"table_id=%d\nsection_length=%d\ntransport_stream_id=0x%04x\nversion_number=%d\ncurrent_next_indicator=%d\nsection_number=%d\nlast_section_number=%d\n",table_id,section_length,transport_stream_id,version_number,current_next_indicator,section_number,last_section_number);
  int nprograms = (section_length - 9) / 4;
  if ((nprograms * 4 + 9) != section_length) {
    fprintf(stderr,"Unexpected section_length in PAT - %d is not equal to 9 + nprograms * 4\n",section_length);
    return -1;
  }

  int j;
  for (j = 0 ; j < nprograms; j++) {
    sv->service_id = (buf[i+j*4] << 8) | buf[i+j*4+1];
    sv->pmt_pid = ((buf[i+j*4+2]&0x1f) << 8) | buf[i+j*4+3];
    //printf("Program %d PMT PID %d\n",sv->service_id,sv->pmt_pid);
  }

  //fprintf(stderr,"Processed PAT\n");
  return 0;
}

int process_sdt(struct service_t* sv)
{
  uint8_t *buf = &sv->sdt.buf[0];
  int length = sv->sdt.length;
  int i = 3;

  sv->tsid = (buf[i] << 8) | buf[i+1]; i += 2;
  int version_number = (buf[i]&0x3e) >> 1;
  int current_next_indicator = buf[i++] & 0x01;
  int section_number = buf[i++];
  int last_section_number = buf[i++];
  sv->onid = (buf[i] << 8) | buf[i+1]; i += 2;
  i++; // Reserved

  fprintf(stderr,"Processing SDT section %d, last_section=%d- tsid=0x%04x, onid=0x%04x:\n",section_number,last_section_number,sv->tsid,sv->onid);
  while (i < length - 4) {
    int service_id = (buf[i] << 8) | buf[i+1]; i += 2;
    //fprintf(stderr,"Processing SDT: i=%d, service=%d\n",i,service_id);
    int EIT_schedule_flag = (buf[i] & 0x2) >> 1;
    int EIT_present_following_flag = buf[i] & 0x1;
    i++;  
    int running_status = (buf[i]&0xe0) >> 5;
    int free_CA_mode = (buf[i]&0x10) >> 4;
    int descriptors_loop_length=((buf[i]&0x0f)<<8) | buf[i+1]; i+= 2;

    int j = i;
    //fprintf(stderr,"SDT: service_id %d, running_status: %s, EIT_schedule_flag=%d,EIT_present_following_flag=%d,free_CA_mode=%d\n",service_id,RST[running_status],EIT_schedule_flag,EIT_present_following_flag,free_CA_mode);
    if (i + descriptors_loop_length > length) {
      fprintf(stderr,"ERROR in SDT: i=%d, j=%d, section_length=%d, descriptors_loop_length=%d\n",i,j,length,descriptors_loop_length);
      exit(1);
    }
    i+= descriptors_loop_length;

    //int s = services[service_id];
    while (j < i) {
      int descriptor_tag=buf[j++];
      int descriptor_length=buf[j++];
      int k = j;
      j += descriptor_length;

      if (descriptor_tag == 0x48) {
        int service_type=buf[k++];
        int provider_name_length=buf[k++];
        k += provider_name_length;
        int service_name_length = buf[k++]; 
        char tmp[128];
        memcpy(tmp,buf + k, service_name_length);
        tmp[service_name_length] = 0;
        if (service_id == sv->service_id) {
          sv->name = strdup(tmp);
          sv->service_type = service_type;
        }
        //fprintf(stderr,"Service ID %d, Service type %d, name=\"%s\", running_status=%s\n",service_id,service_type,tmp,RST[running_status]);
      }
    }
  }
  if (sv->name == NULL) {
    // Not found, keep searching for other sections.
    sv->sdt.length = 0;
  }
  return 0;
}

int bcd2dec(unsigned char buf)
{
  return (((buf&0xf0) >> 4) * 10) + (buf & 0x0f);
}

int process_pmt(struct service_t* sv)
{
  uint8_t *buf = &sv->pmt.buf[0];
  int length = sv->pmt.length;
  int i = 3;

  int program_id = (buf[i] << 8) | buf[i+1]; i += 2;
  int version_number = (buf[i]&0x3e) >> 1;
  int current_next_indicator = buf[i] & 0x01; i++;
  int section_number = buf[i++];
  int last_section_number = buf[i++];
  sv->pcr_pid = ((buf[i]&0x1f)<<8) | buf[i+1]; i+=2;
  int program_info_length = ((buf[i]&0x0f)<<8) | buf[i+1]; i+=2;
  //fprintf(stderr,"PMT program_info_length=%d\n",program_info_length);
  i += program_info_length;

  //fprintf(stderr,"PMT: program_id=%d, version=%d, current_next_indicator=%d, section_number=%d, last_section_number=%d\n",program_id,version_number,current_next_indicator,section_number,last_section_number);
  //fprintf(stderr,"pcr_pid=%d\n",sv->pcr_pid);

  int new_pid = sv->new_pmt_pid+1;
  while ( i < length - 4) {
    int stream_type = buf[i++];
    int pid = ((buf[i]&0x1f) << 8) | buf[i+1]; i += 2;
    int ES_info_length = ((buf[i] & 0x0f) << 8) | buf[i+1]; i += 2;
    i += ES_info_length;

    /* The following list is taken from tvheadend.  These are the only stream types
       tvheadend includes in its output for a service */
    switch (stream_type) {
      case 0x01: // SCT_MPEG2VIDEO;
      case 0x02: // SCT_MPEG2VIDEO;
      case 0x80: // 0x80 is DigiCipher II (North American cable) encrypted MPEG-2
      case 0x03: // SCT_MPEG2AUDIO;
      case 0x04: // SCT_MPEG2AUDIO;
      case 0x06: // SCT_AC3 or SCT_TELETEXT
      case 0x81: // SCT_AC3 or SCT_TELETEXT
      case 0x0f: // SCT_MP4A;
      case 0x11: // SCT_AAC;
      case 0x1b: // SCT_H264;
      case 0x24: // SCT_HEVC;
        sv->pid_map[pid] = new_pid++;
        break;
      default:
        break;
    }

    //fprintf(stderr,"[INFO] PID %d - stream_type 0x%02x mapped to PID %d\n",pid,stream_type,sv->pid_map[pid]);
  }

  // If the PCR PID hasn't already been mapped, then map it to the next new PID
  if (!sv->pid_map[sv->pcr_pid]) {
    sv->pid_map[sv->pcr_pid] = new_pid++;
  }

  if (sv->hbbtv.url) {
    sv->ait_pid = new_pid;
  }

  return 0;
}

void process_section(struct section_t* next, struct section_t* curr, uint8_t* buf, int table_id)
{
  int i,n;

  int payload_unit_start_indicator = (buf[1]&0x40)>>6;

  if (next->length == 0) { // No packets read
    if (payload_unit_start_indicator) {
      i = 5 + buf[4];
      if (buf[i]==table_id) {
        next->length = 3 + (((buf[i+1]&0x0f) << 8) | buf[i+2]);
        int to_copy = MIN(188-i,next->length);
        memcpy(&next->buf[0],buf+i,to_copy);
        next->bytes_read = to_copy;
      } else {
        //fprintf(stderr,"Skipping table_id 0x%02x\n",buf[i]);
        return;
      }
    } else {
      return;
    }
  } else { 
    i = 4;
    if (payload_unit_start_indicator) {
      fprintf(stderr,"PUSI set within section - not handled!\n");
      i++;
    }
    int to_copy = MIN(188-i,next->length-next->bytes_read);
    memcpy(&next->buf[0]+next->bytes_read,buf+i,to_copy);
    next->bytes_read += to_copy;
  }

  if (next->bytes_read == next->length) {
    /* We have a complete section */
    if (psi_crc32(&next->buf[0],next->length,0xffffffff)==0) {
      memcpy(curr, next, sizeof(struct section_t));
    } else {
      fprintf(stderr,"CRC error, skipping section - table_id=0x%02x, length=%d\n",next->buf[0],next->length);
    }

    memset(next, 0, sizeof(struct section_t));
  }

  return;
}

