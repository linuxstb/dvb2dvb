#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "dvb2dvb.h"
#include "json.h"

static int parse_mux_params(struct mux_t *mux, json_value *json)
{
  int i;
  char* s;

  if (json->type != json_object) {
    fprintf(stderr,"[JSON] Invalid mux - not an object\n");
    return -1;
  }

  for (i=0;i<(int)json->u.object.length;i++) {
    if (!strcmp(json->u.object.values[i].name,"frequency_khz"))
      mux->dvbmod_params.frequency_khz = json->u.object.values[i].value->u.integer;
    else if (!strcmp(json->u.object.values[i].name,"bandwidth_hz"))
      mux->dvbmod_params.bandwidth_hz = json->u.object.values[i].value->u.integer;
    else if (!strcmp(json->u.object.values[i].name,"transmission_mode")){
      s = json->u.object.values[i].value->u.string.ptr;
      if (!strcmp(s,"2K")) mux->dvbmod_params.transmission_mode = TRANSMISSION_MODE_2K;
      else if (!strcmp(s,"4K")) mux->dvbmod_params.transmission_mode = TRANSMISSION_MODE_4K;
      else if (!strcmp(s,"8K")) mux->dvbmod_params.transmission_mode = TRANSMISSION_MODE_8K;
      else {
        fprintf(stderr,"Unknown transmission_mode %s\n",s);
        return -1;
      }
    } else if (!strcmp(json->u.object.values[i].name,"constellation")) {
      s = json->u.object.values[i].value->u.string.ptr;
      if (!strcmp(s,"QPSK")) mux->dvbmod_params.constellation = QPSK;
      else if (!strcmp(s,"QAM_16")) mux->dvbmod_params.constellation = QAM_16;
      else if (!strcmp(s,"QAM_64")) mux->dvbmod_params.constellation = QAM_64;
      else {
        fprintf(stderr,"Unknown constellation %s\n",s);
        return -1;
      }
    } else if (!strcmp(json->u.object.values[i].name,"guard_interval")) {
      s = json->u.object.values[i].value->u.string.ptr;
      if (!strcmp(s,"1/4")) mux->dvbmod_params.guard_interval = GUARD_INTERVAL_1_4;
      else if (!strcmp(s,"1/8")) mux->dvbmod_params.guard_interval = GUARD_INTERVAL_1_8;
      else if (!strcmp(s,"1/16")) mux->dvbmod_params.guard_interval = GUARD_INTERVAL_1_16;
      else if (!strcmp(s,"1/32")) mux->dvbmod_params.guard_interval = GUARD_INTERVAL_1_32;
      else {
        fprintf(stderr,"Unknown guard_interval %s\n",s);
        return -1;
      }
    } else if (!strcmp(json->u.object.values[i].name,"code_rate_HP")) {
      s = json->u.object.values[i].value->u.string.ptr;
      if (!strcmp(s,"1/2")) mux->dvbmod_params.code_rate_HP = FEC_1_2;
      else if (!strcmp(s,"2/3")) mux->dvbmod_params.code_rate_HP = FEC_2_3;
      else if (!strcmp(s,"3/4")) mux->dvbmod_params.code_rate_HP = FEC_3_4;
      else if (!strcmp(s,"4/5")) mux->dvbmod_params.code_rate_HP = FEC_4_5;
      else if (!strcmp(s,"5/6")) mux->dvbmod_params.code_rate_HP = FEC_5_6;
      else if (!strcmp(s,"6/7")) mux->dvbmod_params.code_rate_HP = FEC_6_7;
      else if (!strcmp(s,"7/8")) mux->dvbmod_params.code_rate_HP = FEC_7_8;
      else if (!strcmp(s,"8/9")) mux->dvbmod_params.code_rate_HP = FEC_8_9;
      else {
        fprintf(stderr,"Unknown code_rate_HP %s\n",s);
        return -1;
      }
    } else if (!strcmp(json->u.object.values[i].name,"gain"))
      mux->gain = json->u.object.values[i].value->u.integer;
    else if (!strcmp(json->u.object.values[i].name,"tsid"))
      mux->tsid = json->u.object.values[i].value->u.integer;
    else if (!strcmp(json->u.object.values[i].name,"onid"))
      mux->onid = json->u.object.values[i].value->u.integer;
    else if (!strcmp(json->u.object.values[i].name,"nid"))
      mux->nid = json->u.object.values[i].value->u.integer;
  }

  return 0;
}


//  nmuxes = parse_config(argv[1],&muxes);
int parse_config(char* filename, struct mux_t **muxes_res)
{
  int i,j;
  struct mux_t mux_defaults;
  struct mux_t *mux;
  int nmuxes;
  int service_id = 60000; // TODO: Make configurable
  int lcn = 91;          // TODO: Make configurable

  char jsonbuf[65536];  // TODO: Allocate dynamically
  int fd = open(filename,O_RDONLY);
  if (fd < 0) {
    fprintf(stderr,"Cannot read configuration file\n");
    return -2;
  }
  int n = read(fd,jsonbuf,sizeof(jsonbuf));
  close(fd);

  json_value *json = json_parse(jsonbuf,n);

  if (json==NULL) {
    fprintf(stderr,"Error parsing JSON\n");
    return -3;
  }

  if (json->type != json_object) {
    fprintf(stderr,"[JSON] Error - expecting top-level object\n");
    return -4;
  }

  json_value *common = NULL;
  json_value *muxes = NULL;
  for (i=0;i<(int)json->u.object.length;i++) {
    if (!strcmp(json->u.object.values[i].name,"common"))
      common = json->u.object.values[i].value;
    else if (!strcmp(json->u.object.values[i].name,"muxes"))
      muxes = json->u.object.values[i].value;
  }
  if (!muxes) {
    fprintf(stderr,"[JSON]: ERROR - No muxes defined in config file\n");
    return -5;
  }

  if (muxes->type != json_array) {
    fprintf(stderr,"[JSON] ERROR: muxes must be an array\n");
    return -6;
  }

  nmuxes = muxes->u.array.length;

  if (nmuxes == 0) {
    fprintf(stderr,"ERROR: No muxes defined\n");
    return -7;
  }

  *muxes_res = calloc(nmuxes, sizeof(struct mux_t));
  mux = *muxes_res;

  /* Process the common parameters */
  memset(&mux_defaults, 0, sizeof(mux_defaults));
  if (parse_mux_params(&mux_defaults, common) < 0) {
    fprintf(stderr,"Error parsing common section\n");
    return -8;
  }

  for (j = 0; j < nmuxes; j++) {
    /* Set mux to default values */
    *mux = mux_defaults;

    json_value* m = muxes->u.array.values[j];
    if (parse_mux_params(mux, m) < 0) {
      fprintf(stderr,"Error parsing muxes\n");
      return -9;
    }

    json_value *services = NULL;
    for (i=0;i<(int)m->u.object.length;i++) {
      if (!strcmp(m->u.object.values[i].name,"services"))
        services = m->u.object.values[i].value;
    }

    if (services == NULL) {
      fprintf(stderr,"[JSON] Error - no services in mux\n");
      return -10;
    }

    mux->nservices = services->u.array.length;
    fprintf(stderr,"Mux %d, found %d services\n",j,mux->nservices);

    mux->services = calloc(mux->nservices,sizeof(struct service_t));
    for (i=0;i<mux->nservices;i++) {
      json_value *s = services->u.array.values[i];
      if (s->type != json_object) {
        fprintf(stderr,"ERROR: Service %d is not a JSON object\n",i);
        return -11;
      }
      int j;
      for (j=0;j<(int)s->u.object.length;j++) {
        if ((!strcmp(s->u.object.values[j].name,"url")) && (s->u.object.values[j].value->type == json_string))
          mux->services[i].url = strdup(s->u.object.values[j].value->u.string.ptr);
        else if ((!strcmp(s->u.object.values[j].name,"service_id")) && (s->u.object.values[j].value->type == json_integer))
          mux->services[i].new_service_id = s->u.object.values[j].value->u.integer;
        else if ((!strcmp(s->u.object.values[j].name,"lcn")) && (s->u.object.values[j].value->type == json_integer))
          mux->services[i].lcn = s->u.object.values[j].value->u.integer;
      }
      
      /* Add hbbtv to first service */
      if (i < 0) {
        mux->services[i].hbbtv.application_type = 0x0010;
        mux->services[i].hbbtv.AIT_version_number = 1;
        mux->services[i].hbbtv.url = "http://www.avalpa.com/assets/freesoft/HbbTv/";
        mux->services[i].hbbtv.initial_path = "index.php";
      }
    }

    mux++;
  }

  return nmuxes;
}
