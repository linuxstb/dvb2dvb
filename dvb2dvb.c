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
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <curl/curl.h>

#include "dvb2dvb.h"
#include "psi_read.h"
#include "psi_create.h"
#include "crc32.h"
#include "parse_config.h"

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

void check_cc(char *msg, int service, uint8_t *my_cc, uint8_t *buf)
{
  if (buf[0] != 0x47) {
    fprintf(stderr,"%s: Service %d, NO SYNC BYTE: 0x%02x 0x%02x 0x%02x 0x%02x\n",msg,service,buf[0],buf[1],buf[2],buf[3]);
    return;
  }

  int pid = (((buf[1] & 0x1f) << 8) | buf[2]);
  int discontinuity_indicator=(buf[5]&0x80)>>7;
  int adaption_field_control=(buf[3]&0x30)>>4;
  if (my_cc[pid]==0xff) {
    my_cc[pid]=buf[3]&0x0f;
  } else {
    if ((adaption_field_control!=0) && (adaption_field_control!=2)) {
      my_cc[pid]++; my_cc[pid]%=16;
    }
  }

  if ((discontinuity_indicator==0) && (my_cc[pid]!=(buf[3]&0x0f))) {
    fprintf(stderr,"%s: Service %d, PID %d - packet incontinuity - expected %02x, found %02x\n",msg,service,pid,my_cc[pid],buf[3]&0x0f);
    my_cc[pid]=buf[3]&0x0f;
  }
}


static size_t
curl_callback(void *contents, size_t size, size_t nmemb, void *userp)
{
  struct service_t* sv = userp;
  int count = size*nmemb;

  // Check input stream for CC errors
  int needed = 188 - sv->curl_bytes;
  int bytes_left = count;
  //  fprintf(stderr,"Processing %d bytes, needed=%d\n",count,needed);
  uint8_t *p = contents;
  while (bytes_left >= needed) {
    memcpy(&sv->curl_buf[sv->curl_bytes],p,needed);
    bytes_left -= needed;
    sv->curl_bytes = 0;
    p += needed;
    //check_cc("curl",sv->id, &sv->curl_cc[0], &sv->curl_buf[0]);
    needed = 188;
  }
  if (bytes_left) {
    sv->curl_bytes = bytes_left;
    memcpy(&sv->curl_buf[0],p,sv->curl_bytes);
  }
  //  fprintf(stderr,"End of processing, %d bytes, byte_left=%d\n",count,bytes_left);

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

  // First find the PAT, to identify the service_id and pmt_pid
  while(1) {
    n = rb_read(&sv->inbuf,buf,188);
    check_cc("rb_read0",sv->id, &sv->my_cc[0], buf);
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
    check_cc("rb_read1",sv->id, &sv->my_cc[0], buf);
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

  if (sv->ait_pid) {
    create_ait(sv);
  }
  return 0;  
}

void read_to_next_pcr(struct service_t* sv)
{
  int found = 0;
  uint8_t* buf = (uint8_t*)(&sv->buf) + 188 * sv->packets_in_buf;

  while (!found) {
    int n = rb_read(&sv->inbuf,buf,188);
    check_cc("rb_read2",sv->id, &sv->my_cc[0], buf);
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

        if (sv->second_pcr < sv->first_pcr) {
          fprintf(stderr,"WARNING: PCR wraparound - first_pcr=%s",pts2hmsu(sv->first_pcr,'.'));
          fprintf(stderr,", second_pcr=%s",pts2hmsu(sv->second_pcr,'.'));
        }
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
  int n;
  int pid;
  uint8_t buf[188];

  while (1) {
    n = rb_read(&sv->inbuf,buf,188);
    check_cc("rb_read3",sv->id, &sv->my_cc[0], buf);
    (void)n;
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
        fprintf(stderr,"Service %d, pid=%d, start_pcr=%lld (%s)\n",sv->id,pid,sv->start_pcr,pts2hmsu(sv->start_pcr,'.'));
        memcpy(&sv->buf,buf,188);
        sv->packets_in_buf = 1;
        return;
      }
    }
  }
}

/* Function based on code in the tsrfsend.c application by Avalpa
   Digital Engineering srl */
static int calc_channel_capacity(struct dvb_modulator_parameters *params)
{
  uint64_t channel_capacity;
  int temp;

  switch (params->constellation) {
    case QPSK:
      temp = 0;
      break;
    case QAM_16:
      temp = 1;
      break;
    case QAM_64:
      temp = 2;
      break;
    default:
      fprintf(stderr,"Invalid constellation, aborting\n");
      exit(1);
  }
  channel_capacity = params->bandwidth_hz * 1000;
  channel_capacity = channel_capacity * ( temp * 2 + 2);

  switch (params->guard_interval) {
    case GUARD_INTERVAL_1_32:
      channel_capacity = channel_capacity*32/33;
      break;
    case GUARD_INTERVAL_1_16:
      channel_capacity = channel_capacity*16/17;
      break;
    case GUARD_INTERVAL_1_8:
      channel_capacity = channel_capacity*8/9;
      break;
    case GUARD_INTERVAL_1_4:
      channel_capacity = channel_capacity*4/5;
      break;
    default:
      fprintf(stderr,"Invalid guard interval, aborting\n");
      exit(1);
  }
  switch (params->code_rate_HP) {
    case FEC_1_2:
      channel_capacity = channel_capacity*1/2;
      break;
    case FEC_2_3:
      channel_capacity = channel_capacity*2/3;
      break;
    case FEC_3_4:
      channel_capacity = channel_capacity*3/4;
      break;
    case FEC_5_6:
      channel_capacity = channel_capacity*5/6;
      break;
    case FEC_7_8:
      channel_capacity = channel_capacity*7/8;
      break;
    default:
      fprintf(stderr,"Invalid coderate, aborting\n");
      exit(1);
  }

  return channel_capacity/544*423;
}

static void *output_thread(void* userp)
{
  struct mux_t *m = userp;
  int mod_fd;
  int result;
  int channel_capacity;
  int gain;

  /* Open Device */
  if ((mod_fd = open(m->device, O_RDWR)) < 0) {
    fprintf(stderr,"Failed to open device.\n");
    return;
  }

  m->dvbmod_params.cell_id = 0;
  result = ioctl(mod_fd, DVBMOD_SET_PARAMETERS, &m->dvbmod_params);

  struct dvb_modulator_gain_range gain_range;
  gain_range.frequency_khz = m->dvbmod_params.frequency_khz;
  result = ioctl(mod_fd, DVBMOD_GET_RF_GAIN_RANGE, &gain_range);
  fprintf(stderr,"Gain range: %d to %d\n",gain_range.min_gain,gain_range.max_gain);

  result = ioctl(mod_fd, DVBMOD_SET_RF_GAIN, &m->gain);
  fprintf(stderr,"Gain set to %d\n",m->gain);

  /* Wait for 4MB in the ringbuffer */
  while (rb_get_bytes_used(&m->outbuf) < 4*1024*1024) {
    usleep(50000);
  }

  /* The main transfer loop */
  unsigned char buf[188*200];
  int n;
  unsigned long long bytes_sent = 0;
  while(1) {
    n = rb_read(&m->outbuf,buf,sizeof(buf));
    if (n == 0) { break; }

    int to_write = n;
    int bytes_done = 0;
    while (bytes_done < to_write) {
      n = write(mod_fd,buf+bytes_done,to_write-bytes_done);
      if (n == 0) {
        /* This shouldn't happen */
        fprintf(stderr,"Zero write\n");
        usleep(500);
      } else if (n <= 0) {
	fprintf(stderr,"Write error %d: ",n);
        perror("Write error: ");
      } else {
        //if (n < sizeof(buf)) { fprintf(stderr,"Short write - %d bytes\n",n); }
        //fprintf(stderr,"Wrote %d\n",n);
        bytes_sent += n;
        bytes_done += n;
        //fprintf(stderr,"Bytes sent: %llu\r",bytes_sent);
      }
    }
  }  

  close(mod_fd);
  return;
}

static int ms_to_bits(int channel_capacity, int ms)
{
  return ((int64_t)(((int64_t)channel_capacity * (int64_t)(ms)) / 1000));
}

/* The main thread for each mux */
static void *mux_thread(void* userp)
{
  struct mux_t *m = userp;
  int i,j;

  /* Calculate target bitrate */
  m->channel_capacity = calc_channel_capacity(&m->dvbmod_params);
  fprintf(stderr,"Channel capacity = %dbps\n",m->channel_capacity);

  // SI table frequencies, in bits based on above bitrate
  m->pat_freq_in_bits = ms_to_bits(m->channel_capacity,200);
  m->pmt_freq_in_bits = ms_to_bits(m->channel_capacity,200);
  m->sdt_freq_in_bits = ms_to_bits(m->channel_capacity,1000);
  m->nit_freq_in_bits = ms_to_bits(m->channel_capacity,1000);
  m->ait_freq_in_bits = ms_to_bits(m->channel_capacity,500);

  /* Initialise output ringbuffer */
  rb_init(&m->outbuf);

  /* Start output thread */
  fprintf(stderr,"Creating output thread\n");
  int error = pthread_create(&m->output_threadid,
                             NULL, /* default attributes please */
                             output_thread,
                             (void *)m);
  if (error) {
    fprintf(stderr, "Couldn't create output thread - errno %d\n", error);
    return;
  }

  for (i=0;i<m->nservices;i++) {
    m->services[i].id = i;
    m->services[i].new_pmt_pid = (i+1)*100;
    for (j=0;j<8192;j++) { m->services[i].my_cc[j] = 0xff; }
    for (j=0;j<8192;j++) { m->services[i].curl_cc[j] = 0xff; }

    fprintf(stderr,"Creating thread %d\n",i);
    int error = pthread_create(&m->services[i].curl_threadid,
                               NULL, /* default attributes please */
                               curl_thread,
                               (void *)&m->services[i]);

    if (error)
      fprintf(stderr, "Couldn't run thread number %d, errno %d\n", i, error);
    else
      fprintf(stderr, "Thread %d, gets %s\n", i, m->services[i].url);
  }

  for (i=0;i<m->nservices;i++) {
    int res = init_service(&m->services[i]);
    if (res < 0) {
      fprintf(stderr,"Error opening service %d (%s), aborting\n",i,m->services[i].url);
      return;
    }

    dump_service(m->services,i);
  }

  int active_services = 0;
  while (active_services < m->nservices) {
    for (i=0;i<m->nservices;i++) {
      if (m->services[i].status) { active_services++; }
    }
    fprintf(stderr,"Waiting for services - %d started\r",active_services);
  }

  for (i=0;i<m->nservices;i++) {
    int to_skip = (rb_get_bytes_used(&m->services[i].inbuf)/188) * 188;
    rb_skip(&m->services[i].inbuf, to_skip);
    fprintf(stderr,"Skipped %d bytes from service %d\n",to_skip,i);
    // Reset CC counters
    for (j=0;j<8192;j++) { m->services[i].my_cc[j] = 0xff; }
  }

  for (i=0;i<m->nservices;i++) {
    sync_to_pcr(&m->services[i]);
  }

  //fprintf(stderr,"Creating PAT - nservices=%d\n",nservices);

  create_pat(&m->pat, m->services, m->nservices);
  create_sdt(&m->sdt, m->services, m->nservices);
  create_nit(&m->nit, m->services, m->nservices);

  int64_t output_bitpos = 0;
  int64_t next_pat_bitpos = 0;
  int64_t next_pmt_bitpos = 0;
  int64_t next_sdt_bitpos = 0;
  int64_t next_nit_bitpos = 0;
  int64_t next_ait_bitpos = 0;

  // The main output loop.  We output one TS packet (either real or padding) in each iteration.
  int x = 1;
  int64_t padding_bits = 0;
  int eit_cc = 0;
  while (1) {
    // Ensure we have enough data for every service.
    for (i=0;i<m->nservices;i++) {
      if (m->services[i].packets_in_buf-m->services[i].packets_written == 1) {  // Will contain a PCR.
        struct service_t* sv = &m->services[i];

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
          sv->bitpos[j] = (packet_pcr * m->channel_capacity) / 27000000;
          //fprintf(stderr, "Stream %d, packet %d, packet_pcr = %lld, bitpos %lld\n",i,j,packet_pcr,sv->bitpos[j]);
        }
      }
    }

    // Find the service with the most urgent packet (i.e. earliest bitpos)
    struct service_t* sv = &m->services[0];
    for (i=1;i<m->nservices;i++) {
      if (m->services[i].bitpos[m->services[i].packets_written] < sv->bitpos[sv->packets_written]) {
        sv = &m->services[i];
      }
    }

    //fprintf(stderr,"output_bitpos=%d, sv->bitpos[sv->packets_written]=%d\n",output_bitpos,sv->bitpos[sv->packets_written]);

#if 0
    fprintf(stderr,"output_bitpos  next_pat   next_pmt   next_sdt   next_nit");
    for (i=0;i<m->nservices;i++) { fprintf(stderr,"  service_%d",i); }
    fprintf(stderr,"\n");
    fprintf(stderr,"%lld %lld %lld %lld %lld",output_bitpos,next_pat_bitpos,next_pmt_bitpos,next_sdt_bitpos,next_sdt_bitpos);
    for (i=0;i<m->nservices;i++) { fprintf(stderr," %lld",m->services[i].bitpos[m->services[i].packets_written]); }
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
    if ((m->services[0].ait_pid) && (next_ait_bitpos <= next_bitpos)) { next_psi = 5; next_bitpos = next_ait_bitpos; } 

    /* Output NULL packets until we reach next_bitpos */
    while (next_bitpos > output_bitpos) {
      //fprintf(stderr,"next_bitpos=%lld, output_bitpos=%lld            \n",next_bitpos,output_bitpos);
      rb_write(&m->outbuf, null_packet, 188);
      padding_bits += 188*8;
      output_bitpos += 188*8;
    }

    /* Now output whichever packet is next */
    int n,res;
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
        res = rb_write(&m->outbuf, &sv->buf[188*sv->packets_written], 188);
        if (res != 188) { fprintf(stderr,"Write error - res=%d\n",res); }
        n = 1;
        sv->packets_written++;
        break;

      case 1: // PAT
        n = write_section(&m->outbuf, &m->pat, 0);
        next_pat_bitpos += m->pat_freq_in_bits;
        break;

      case 2: // PMT
        n = 0;
        for (i=0;i<m->nservices;i++) {
          n += write_section(&m->outbuf,&m->services[i].new_pmt, m->services[i].new_pmt_pid);
        }
        next_pmt_bitpos += m->pmt_freq_in_bits;
        break;

      case 3: // SDT
        n = write_section(&m->outbuf, &m->sdt, 0x11);
        next_sdt_bitpos += m->sdt_freq_in_bits;
        break;

      case 4: // NIT
        n = write_section(&m->outbuf, &m->nit, 0x10);
        next_nit_bitpos += m->nit_freq_in_bits;
        break;

      case 5: // AIT
        n = write_section(&m->outbuf, &m->services[0].ait, m->services[0].ait_pid);
        next_ait_bitpos += m->ait_freq_in_bits;
        break;
    }
    output_bitpos += n * 188*8;

    if (x==1000) {
      x = 0;
      for (i=0;i<m->nservices;i++) {
        fprintf(stderr,"%10d  ",rb_get_bytes_used(&m->services[i].inbuf));
      }
      fprintf(stderr,"Average capacity used: %.3g%%  Outbuf = %10d               \r",100.0*(double)(output_bitpos-padding_bits)/(double)output_bitpos,rb_get_bytes_used(&m->outbuf));
    }
    x++;
  }
}

int main(int argc, char* argv[])
{
  int nmuxes;
  struct mux_t *muxes;

  if (argc != 2) {
    fprintf(stderr,"Usage: dvb2dvb config.json\n");
    return 1;
  }

  nmuxes = parse_config(argv[1],&muxes);

  if (nmuxes < 0) {
    fprintf(stderr,"[JSON] Error reading config file\n");
    return 1;
  }

  if (nmuxes != 1) {
    fprintf(stderr,"[JSON] Error - only 1 mux supported for now\n");
    return 1;
  }

  /* Must initialize libcurl before any threads are started */
  curl_global_init(CURL_GLOBAL_ALL);

  /* TODO: Do this for each mux */

  fprintf(stderr,"Creating mux processing thread 0\n");
  int error = pthread_create(&muxes[0].threadid,
                             NULL, /* default attributes please */
                             mux_thread,
                             (void *)muxes);

  fprintf(stderr,"Created mux thread - error=%d\n",error);
  fprintf(stderr,"Waiting for mux thread to terminate...\n");
  
  pthread_join(muxes[0].threadid, NULL);

  fprintf(stderr,"Mux thread terminated.\n");
  return 0;
}
