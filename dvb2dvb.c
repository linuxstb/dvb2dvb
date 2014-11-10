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
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <curl/curl.h>

#include "dvb2dvb.h"
#include "psi_read.h"
#include "psi_create.h"
#include "crc32.h"

#define TARGET_BITRATE 31668318

// SI table frequencies, in bits based on above bitrate
#define MS_TO_BITS(x)  ((int64_t)(((int64_t)TARGET_BITRATE * (int64_t)(x)) / 1000))

#define PAT_FREQ_IN_BITS  MS_TO_BITS(200)
#define PMT_FREQ_IN_BITS  MS_TO_BITS(200)
#define SDT_FREQ_IN_BITS  MS_TO_BITS(1000)
#define NIT_FREQ_IN_BITS  MS_TO_BITS(1000)

static uint8_t null_packet[188] = {
  0x47, 0x1f, 0xff, 0x10, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
  0xff, 0xff, 0xff, 0xff
};

void dump_service(struct service_t* services, int i)
{
  fprintf(stderr,"Service %d:\n",i);
  fprintf(stderr,"  URL: %s\n",services[i].url);
  fprintf(stderr,"  service_id: OLD=%d NEW=%d\n",services[i].service_id,services[i].new_service_id);
  fprintf(stderr,"  name: %s\n",services[i].name);
  fprintf(stderr,"  pmt_pid: OLD=%d NEW=%d\n",services[i].pmt_pid,services[i].new_pmt_pid);
  fprintf(stderr,"  pcr_pid: OLD=%d NEW=%d\n",services[i].pcr_pid,services[i].pid_map[services[i].pcr_pid]);
  fprintf(stderr,"  lcn: %d\n",services[i].lcn);
}

static size_t
curl_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  struct service_t* sv = userp;
  int count = size*nmemb;

  int n = rb_write(&sv->inbuf, contents, count);

  if (n < count) {
    fprintf(stderr,"\nERROR: Stream %d, Input buffer full, dropping %d bytes\n",sv->id,(count)-n);
  }

  /* Confirm there are bytes in the buffer */
  sv->status=1;

  //fprintf(stderr,"curl_callback, stream=%s, size=%d, written=%d\n",sv->name,(int)count,n);
  return count; /* Pretend we've consumed all */

}

static void *curl_thread(void* userp)
{
  struct service_t *sv = userp;
  CURL *curl;

  rb_init(&sv->inbuf);

  curl = curl_easy_init();
  curl_easy_setopt(curl, CURLOPT_URL, sv->url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_callback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)sv);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/1.0");
  curl_easy_perform(curl); /* ignores error */
  curl_easy_cleanup(curl);

  return NULL;
}

/* Read PAT/PMT/SDT from stream and stop at first packet with PCR */
int init_service(struct service_t* sv)
{
  uint8_t buf[188];
  int n;
  int pid;
  int i = 0;

  int error = pthread_create(&sv->curl_threadid,
                             NULL, /* default attributes please */
                             curl_thread,
                             (void *)sv);

  if (error)
    fprintf(stderr, "Couldn't run thread number %d, errno %d\n", i, error);
  else
    fprintf(stderr, "Thread %d, gets %s\n", i, sv->url);

  // First find the PAT, to identify the service_id and pmt_pid
  while(1) {
    n = rb_read(&sv->inbuf,buf,188);
    (void)n;
    i++;
    pid = (((buf[1] & 0x1f) << 8) | buf[2]);

    //fprintf(stderr,"Searching for PAT, pid=%d %02x %02x %02x %02x\n",pid,buf[0],buf[1],buf[2],buf[3]);
    if (pid==0) {
      process_pat(sv,buf);
      break;
    }
  }

  // Now process the other tables, in any order
  //  PMT: sv->pmt_pid
  //  SDT: 
  while((!sv->pmt.length) || (!sv->sdt.length)) {
    n = rb_read(&sv->inbuf,buf,188);
    i++;
    pid = (((buf[1] & 0x1f) << 8) | buf[2]);
    if (pid==sv->pmt_pid) {
      process_section(&sv->next_pmt,&sv->pmt,buf,0x02);
    } else if (pid==17) {
      process_section(&sv->next_sdt,&sv->sdt,buf,0x42);
      if (sv->sdt.length) {
        process_sdt(sv);
      }
    }
  }

  process_pmt(sv);

  //fprintf(stderr,"Read SDT (%d bytes) and PMT (%d bytes)\n",sv->sdt.length,sv->pmt.length);

  // Read until we find a packet with a PCR
  // TODO: Merge this into the loop above, so we are always using the latest PMT/PAT when we have found a PCR.

  //fprintf(stderr,"Searching for PCR...\n");

  // Create our new PMT section, removing unused streams and references to CA descriptors
  create_pmt(sv);

  return 0;  
}

void read_to_next_pcr(struct service_t* sv)
{
  int found = 0;
  uint8_t* buf = (uint8_t*)(&sv->buf) + 188 * sv->packets_in_buf;

  while (!found) {
    int n = rb_read(&sv->inbuf,buf,188);
    (void)n;
    int pid = (((buf[1] & 0x1f) << 8) | buf[2]);
    if (pid==sv->pcr_pid) {
      if (((buf[3] & 0x20) == 0x20) && (buf[4] > 5) && (buf[5] & 0x10)) {
        sv->first_pcr = sv->second_pcr;
        sv->second_pcr  = (uint64_t)buf[6] << 25;
        sv->second_pcr |= (uint64_t)buf[7] << 17;
        sv->second_pcr |= (uint64_t)buf[8] << 9;
        sv->second_pcr |= (uint64_t)buf[9] << 1;
        sv->second_pcr |= ((uint64_t)buf[10] >> 7) & 0x01;
        sv->second_pcr *= 300;
        sv->second_pcr += ((buf[10] & 0x01) << 8) | buf[11];
        found = 1;
      }
    }

    if (pid==0x12) {
      process_section(&sv->next_eit,&sv->eit,buf,0x4e);  // EITpf, actual TS
      if (sv->eit.length) {
        struct section_t new_eit;
        if (rewrite_eit(&new_eit, &sv->eit, sv->service_id, sv->new_service_id, sv->onid) == 0) {  // This is for this service
          int npackets = copy_section(buf, &new_eit, 0x12);
          sv->packets_in_buf += npackets;
          buf += npackets * 188;
        }
        sv->eit.length = 0; // Clear section, we are done with it.
      }
    }

    if (sv->pid_map[pid]) {
      // Change PID
      buf[1] = (buf[1] & ~0x1f) | ((sv->pid_map[pid] & 0x1f00) >> 8);
      buf[2] = sv->pid_map[pid] & 0x00ff;

      sv->packets_in_buf++;
      buf += 188;
    }
  }
}

void sync_to_pcr(struct service_t* sv)
{
  int i,n;
  int pid;
  uint8_t buf[188];

  while (1) {
    n = rb_read(&sv->inbuf,buf,188);
    (void)n;
    i++;
    pid = (((buf[1] & 0x1f) << 8) | buf[2]);
    if (pid==sv->pmt_pid) {
      process_section(&sv->next_pmt,&sv->pmt,buf,0x02);
    } else if (pid==17) {
      process_section(&sv->next_sdt,&sv->sdt,buf,0x42);
    } else if (pid==sv->pcr_pid) {
      // e.g. 4709 0320 b7 10 ff5b d09c 00ab
      if (((buf[3] & 0x20) == 0x20) && (buf[4] > 5) && (buf[5] & 0x10)) {
        sv->start_pcr  = (uint64_t)buf[6] << 25;
        sv->start_pcr |= (uint64_t)buf[7] << 17;
        sv->start_pcr |= (uint64_t)buf[8] << 9;
        sv->start_pcr |= (uint64_t)buf[9] << 1;
        sv->start_pcr |= ((uint64_t)buf[10] >> 7) & 0x01;
        sv->start_pcr *= 300;
        sv->start_pcr += ((buf[10] & 0x01) << 8) | buf[11];
        sv->second_pcr = sv->start_pcr;
        //fprintf(stderr,"pid=%d, pcr=%lld (%s), packets=%d\n",pid,sv->start_pcr,pts2hmsu(sv->start_pcr,'.'),i);
        memcpy(&sv->buf,buf,188);
        sv->packets_in_buf = 1;
        return;
      }
    }
  }
}

int main(int argc, char* argv[])
{
  struct service_t *services;
  int nservices;
  int i,j;
  int service_id = 60000; // TODO: Make configurable
  int lcn = 91;          // TODO: Make configurable
  struct section_t pat;
  struct section_t sdt;
  struct section_t nit;

  if (argc < 2) {
    fprintf(stderr,"Usage: dvb2dvb file1.ts file2.ts ...\n");
    return 1;
  }

  /* Must initialize libcurl before any threads are started */
  curl_global_init(CURL_GLOBAL_ALL);

  nservices = argc-1;
  services = calloc(nservices,sizeof(struct service_t));
  for (i=0;i<nservices;i++) {
    services[i].id = i;
    services[i].url = strdup(argv[i+1]);
    services[i].new_pmt_pid = (i+1)*100;
    services[i].new_service_id = service_id++;
    services[i].lcn = lcn++;

    int res = init_service(&services[i]);
    if (res < 0) {
      fprintf(stderr,"Error opening service %d (%s), aborting\n",i,services[i].url);
      return 1;
    }

    dump_service(services,i);
  }

  int active_services = 0;
  while (active_services < nservices) {
    for (i=0;i<nservices;i++) {
      if (services[i].status) { active_services++; }
    }
    fprintf(stderr,"Waiting for services - %d started\r",active_services);
  }

  for (i=0;i<nservices;i++) {
    int to_skip = (rb_get_bytes_used(&services[i].inbuf)/188) * 188;
    rb_skip(&services[i].inbuf, to_skip);
    fprintf(stderr,"Skipped %d bytes from service %d\n",to_skip,i);
  }

  for (i=0;i<nservices;i++) {
    sync_to_pcr(&services[i]);
  }

  //fprintf(stderr,"Creating PAT - nservices=%d\n",nservices);

  create_pat(&pat, services, nservices);
  create_sdt(&sdt, services, nservices);
  create_nit(&nit, services, nservices);

  int64_t output_bitpos = 0;
  int64_t next_pat_bitpos = 0;
  int64_t next_pmt_bitpos = 0;
  int64_t next_sdt_bitpos = 0;
  int64_t next_nit_bitpos = 0;

  // The main output loop.  We output one TS packet (either real or padding) in each iteration.
  int x = 1;
  int64_t padding_bits = 0;
  int eit_cc = 0;
  while (1) {
    // Ensure we have enough data for every service.
    for (i=0;i<nservices;i++) {
      if (services[i].packets_in_buf-services[i].packets_written == 1) {  // Will contain a PCR.
        struct service_t* sv = &services[i];

        // Move last packet to start of buffer
        sv->first_pcr = sv->second_pcr;
        memcpy(&sv->buf, (uint8_t*)(&sv->buf) + (188 * (sv->packets_in_buf-1)), 188);
        sv->packets_written = 0;
        sv->packets_in_buf = 1;

        read_to_next_pcr(sv);

        int64_t pcr_diff = sv->second_pcr - sv->first_pcr;
        int npackets = sv->packets_in_buf;
        //double packet_duration = pcr_diff / (double)npackets;
        //fprintf(stderr,"Stream %d: pcr_diff = %lld, npackets=%lld, packet duration=%.8g ticks\n",i,pcr_diff,npackets,packet_duration);

        // Now calculate the output position for each packet, in terms of total bits written so far.
        for (j=0;j<sv->packets_in_buf;j++) {
          int64_t  packet_pcr = sv->first_pcr + ((j * pcr_diff)/(npackets-1)) - sv->start_pcr;
          sv->bitpos[j] = (packet_pcr * TARGET_BITRATE) / 27000000;
          //fprintf(stderr, "Stream %d, packet %d, packet_pcr = %lld, bitpos %lld\n",i,j,packet_pcr,sv->bitpos[j]);
        }
      }
    }

    // Find the service with the most urgent packet (i.e. earliest bitpos)
    struct service_t* sv = &services[0];
    for (i=1;i<nservices;i++) {
      if (services[i].bitpos[services[i].packets_written] < sv->bitpos[sv->packets_written]) {
        sv = &services[i];
      }
    }

    //fprintf(stderr,"output_bitpos=%d, sv->bitpos[sv->packets_written]=%d\n",output_bitpos,sv->bitpos[sv->packets_written]);

#if 0
    fprintf(stderr,"output_bitpos  next_pat   next_pmt   next_sdt   next_nit");
    for (i=0;i<nservices;i++) { fprintf(stderr,"  service_%d",i); }
    fprintf(stderr,"\n");
    fprintf(stderr,"%lld %lld %lld %lld %lld",output_bitpos,next_pat_bitpos,next_pmt_bitpos,next_sdt_bitpos,next_sdt_bitpos);
    for (i=0;i<nservices;i++) { fprintf(stderr," %lld",services[i].bitpos[services[i].packets_written]); }
    fprintf(stderr,"\n");
//    return 0;
#endif

    /* Now check for PSI packets */
    int next_psi = 0;
    int64_t next_bitpos = sv->bitpos[sv->packets_written];
    if (next_pat_bitpos <= next_bitpos) { next_psi = 1; next_bitpos = next_pat_bitpos; }
    if (next_pmt_bitpos <= next_bitpos) { next_psi = 2; next_bitpos = next_pmt_bitpos; }
    if (next_sdt_bitpos <= next_bitpos) { next_psi = 3; next_bitpos = next_sdt_bitpos; }
    if (next_nit_bitpos <= next_bitpos) { next_psi = 4; next_bitpos = next_nit_bitpos; }

    /* Output NULL packets until we reach next_bitpos */
    while (next_bitpos > output_bitpos) {
      //fprintf(stderr,"next_bitpos=%lld, output_bitpos=%lld            \n",next_bitpos,output_bitpos);
      write(1, &null_packet, 188);
      padding_bits += 188*8;
      output_bitpos += 188*8;
    }

    /* Now output whichever packet is next */
    int n;
    uint8_t* buf;
    int pid;
    switch (next_psi) {
      case 0:
        buf = &sv->buf[188*sv->packets_written];
        pid = (((buf[1] & 0x1f) << 8) | buf[2]);
        if (pid==0x12) { // EIT - fix CC
          buf[3] = 0x10 | eit_cc;
          eit_cc = (eit_cc + 1) % 16;
        }
        write(1, &sv->buf[188*sv->packets_written], 188);
        n = 1;
        sv->packets_written++;
        break;

      case 1: // PAT
        n = write_section(1, &pat, 0);
        next_pat_bitpos += PAT_FREQ_IN_BITS;
        break;

      case 2: // PMT
        n = 0;
        for (i=0;i<nservices;i++) {
          n += write_section(1,&services[i].new_pmt, services[i].new_pmt_pid);
        }
        next_pmt_bitpos += PMT_FREQ_IN_BITS;
        break;

      case 3: // SDT
        n = write_section(1, &sdt, 0x11);
        next_sdt_bitpos += SDT_FREQ_IN_BITS;
        break;

      case 4: // NIT
        n = write_section(1, &nit, 0x10);
        next_nit_bitpos += NIT_FREQ_IN_BITS;
        break;
    }
    output_bitpos += n * 188*8;

    if (x==1000) {
      x = 0;
      for (i=0;i<nservices;i++) {
        fprintf(stderr,"%10d  ",rb_get_bytes_used(&services[i].inbuf));
      }
      fprintf(stderr,"Average capacity used: %.3g%%\r",100.0*(double)(output_bitpos-padding_bits)/(double)output_bitpos);
    }
    x++;
  }

  return 0;
}
