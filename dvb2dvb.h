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

#ifndef _DVB2DVB_H
#define _DVB2DVB_H

#include <stdint.h>
#include <pthread.h>
#include "ringbuffer.h"
#include "dvbmod.h"

#ifndef MAX
#define MAX(X,Y) ((X) > (Y) ? (X) : (Y))
#endif
#ifndef MIN
#define MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#endif

// Buffer size for input packets - must be large enough to hold all packets between two PCR timestamps
// DVB-T max is about 31Mbits/s, pcr max interval is 40ms, so buffer needs to be at least 1240000 bits (about 824 TS packets)
// So we make the buffer hold 2048 TS packets (about 371KB)
#define INPUT_BUFFER_SIZE_IN_PACKETS 2048

struct section_t
{
  int length;
  int bytes_read;
  uint8_t buf[4096];
  int cc;
};

struct hbbtv_t
{
  char* url;
  char* initial_path;
  int application_type;
  int AIT_version_number;
};

struct service_t
{
  int id;
  int status; /* 0 = not started, 1 = streaming */
  char *url;
  char* name;

  int service_id;
  int service_type;
  int onid;
  int tsid;
  int new_service_id;
  int lcn;
  int pmt_pid;
  int pcr_pid;
  int new_pmt_pid;           /* First PID used (for PMT) in output stream */
  int ait_pid;
  uint16_t pid_map[8192];
  int64_t start_pcr;
  int64_t first_pcr;
  int64_t second_pcr;
  int packets_in_buf;
  int packets_written;
  uint8_t my_cc[8192];
  uint8_t buf[INPUT_BUFFER_SIZE_IN_PACKETS*188];
  int64_t bitpos[INPUT_BUFFER_SIZE_IN_PACKETS];

  uint8_t curl_cc[8192];
  uint8_t curl_buf[188];
  int curl_bytes;

  struct section_t pmt;
  struct section_t sdt;
  struct section_t eit;
  struct section_t next_pmt;
  struct section_t next_sdt;
  struct section_t next_eit;

  struct hbbtv_t hbbtv;
  struct section_t ait;

  struct section_t new_pmt;

  /* Curl-related fields */
  pthread_t curl_threadid;

  struct ringbuffer_t inbuf;  /* Input ringbuffer for curl requests */
};

struct mux_t
{
  char* device;
  struct dvb_modulator_parameters dvbmod_params;
  int gain;
  int tsid;
  int onid;
  int nid;

  int channel_capacity;
  int pat_freq_in_bits;
  int pmt_freq_in_bits;
  int sdt_freq_in_bits;
  int nit_freq_in_bits;
  int ait_freq_in_bits;

  struct section_t pat;
  struct section_t sdt;
  struct section_t nit;

  int nservices;
  struct service_t* services;  

  pthread_t threadid;  /* Mux processing thread id */
  pthread_t output_threadid;  /* Output thread id */

  struct ringbuffer_t outbuf;  /* Output ringbuffer to write to modulator */
};


#endif
