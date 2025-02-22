/*

  Copyright (C) 2017 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, version 3.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/

#ifndef _SOURCE_H
#define _SOURCE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <sndfile.h>
#include <string.h>
#include <sigutils/sigutils.h>
#include <SoapySDR/Device.h>
#include <SoapySDR/Formats.h>
#include <SoapySDR/Version.h>
#include <analyzer/serialize.h>
#include <util/util.h>
#include <object.h>

#define SUSCAN_SOURCE_DEFAULT_BUFSIZ 1024

#define SUSCAN_SOURCE_LOCAL_INTERFACE   "local"
#define SUSCAN_SOURCE_REMOTE_INTERFACE  "remote"

#define SUSCAN_SOURCE_DEFAULT_NAME      "Default source"
#define SUSCAN_SOURCE_DEFAULT_FREQ      433920000 /* 433 ISM */
#define SUSCAN_SOURCE_DEFAULT_SAMP_RATE 1000000
#define SUSCAN_SOURCE_DEFAULT_BANDWIDTH SUSCAN_SOURCE_DEFAULT_SAMP_RATE
#define SUSCAN_SOURCE_DEFAULT_READ_TIMEOUT 100000 /* 100 ms */
#define SUSCAN_SOURCE_ANTIALIAS_REL_SIZE    5
#define SUSCAN_SOURCE_DECIMATOR_BUFFER_SIZE 512

/************************** Source config API ********************************/
struct suscan_source_gain_desc {
  int epoch;
  char *name;
  SUFLOAT min;
  SUFLOAT max;
  SUFLOAT step;
  SUFLOAT def;
};

struct suscan_source_device_info {
  /* Borrowed list */
  PTR_LIST_CONST(struct suscan_source_gain_desc, gain_desc);
  PTR_LIST_CONST(char, antenna);
  const double *samp_rate_list;
  unsigned int samp_rate_count;
  SUFREQ freq_min;
  SUFREQ freq_max;
};

#define suscan_source_device_info_INITIALIZER   \
{                                               \
  NULL, /* gain_list */                         \
  0, /* gain_count */                           \
  NULL, /* antenna_list */                      \
  0, /* antenna_count */                        \
  NULL, /* samp_rate_list */                    \
  0, /* samp_rate_count */                      \
  0, /* freq_min */                             \
  0, /* freq_max */                             \
}

void suscan_source_device_info_finalize(struct suscan_source_device_info *info);

struct suscan_source_device {
  const char *interface;
  const char *driver;
  char *desc;
  SoapySDRKwargs *args;
  int index;
  SUBOOL available;
  int epoch;

  PTR_LIST(struct suscan_source_gain_desc, gain_desc);
  PTR_LIST(char, antenna);
  double *samp_rate_list;
  unsigned int samp_rate_count;

  SUFREQ freq_min;
  SUFREQ freq_max;
};

typedef struct suscan_source_device suscan_source_device_t;

SUINLINE const char *
suscan_source_device_get_param(
    const suscan_source_device_t *dev,
    const char *key)
{
  return SoapySDRKwargs_get(dev->args, key);
}

SUINLINE const char *
suscan_source_device_get_driver(const suscan_source_device_t *self)
{
  const char *driver;

  if ((driver = suscan_source_device_get_param(self, "driver")) == NULL)
    driver = self->driver;

  return driver;
}

SUINLINE SUBOOL
suscan_source_device_is_remote(const suscan_source_device_t *self)
{
  if (self->interface == NULL)
    return SU_FALSE;

  return strcmp(self->interface, SUSCAN_SOURCE_REMOTE_INTERFACE) == 0;
}

SUINLINE const char *
suscan_source_device_get_desc(const suscan_source_device_t *self)
{
  return self->desc;
}

SUINLINE int
suscan_source_device_get_index(const suscan_source_device_t *self)
{
  return self->index;
}

SUINLINE SUFREQ
suscan_source_device_get_min_freq(const suscan_source_device_t *self)
{
  return self->freq_min;
}

SUINLINE SUFREQ
suscan_source_device_get_max_freq(const suscan_source_device_t *self)
{
  return self->freq_max;
}

SUINLINE SUBOOL
suscan_source_device_is_available(const suscan_source_device_t *self)
{
  return self->available;
}

SUINLINE SUBOOL
suscan_source_device_is_populated(const suscan_source_device_t *self)
{
  /*
   * Remote devices are never populated
   */
  return !suscan_source_device_is_remote(self) && self->antenna_count != 0;
}

SUBOOL suscan_source_device_walk(
    SUBOOL (*function) (
        const suscan_source_device_t *dev,
        unsigned int index,
        void *privdata),
    void *privdata);

const suscan_source_device_t *suscan_source_device_get_by_index(
    unsigned int index);

const suscan_source_device_t *suscan_source_get_null_device(void);

/* Internal */
struct suscan_source_gain_desc *suscan_source_device_assert_gain_unsafe(
    suscan_source_device_t *dev,
    const char *name,
    SUFLOAT min,
    SUFLOAT max,
    unsigned int step);

/* Internal */
SUBOOL suscan_source_device_preinit(void);

/* Internal */
SUBOOL suscan_source_register_null_device(void);

/* Internal */
const suscan_source_device_t *suscan_source_device_find_first_sdr(void);

/* Internal */
const struct suscan_source_gain_desc *suscan_source_device_lookup_gain_desc(
    const suscan_source_device_t *self,
    const char *name);

/* Internal */
const struct suscan_source_gain_desc *suscan_source_gain_desc_new_hidden(
    const char *name,
    SUFLOAT value);

/* Internal */
suscan_source_device_t *suscan_source_device_assert(
    const char *interface,
    const SoapySDRKwargs *args);

/* Internal */
SUBOOL suscan_source_device_populate_info(suscan_source_device_t *self);

/* Internal */
suscan_source_device_t *suscan_source_device_new(
    const char *interface,
    const SoapySDRKwargs *args);

/* Internal */
suscan_source_device_t *suscan_source_device_dup(
    const suscan_source_device_t *self);

/* Internal */
void suscan_source_device_destroy(suscan_source_device_t *dev);

unsigned int suscan_source_device_get_count(void);

SUBOOL suscan_source_device_get_info(
    const suscan_source_device_t *self,
    unsigned int channel,
    struct suscan_source_device_info *info);

enum suscan_source_type {
  SUSCAN_SOURCE_TYPE_FILE,
  SUSCAN_SOURCE_TYPE_SDR
};

enum suscan_source_format {
  SUSCAN_SOURCE_FORMAT_AUTO,
  SUSCAN_SOURCE_FORMAT_RAW_FLOAT32,
  SUSCAN_SOURCE_FORMAT_WAV,
  SUSCAN_SOURCE_FORMAT_RAW_UNSIGNED8
};

struct suscan_source_gain_value {
  const struct suscan_source_gain_desc *desc;
  SUFLOAT val;
};

SUSCAN_SERIALIZABLE(suscan_source_config) {
  enum suscan_source_type type;
  enum suscan_source_format format;
  char *label; /* Label for this configuration */

  /* Common for all source types */
  SUFREQ  freq;
  SUFREQ  lnb_freq;
  SUFLOAT bandwidth;
  SUBOOL  iq_balance;
  SUBOOL  dc_remove;
  SUFLOAT ppm;
  unsigned int samp_rate;
  unsigned int average;

  /* For file sources */
  char *path;
  SUBOOL loop;

  /* For SDR sources */
  const suscan_source_device_t *device; /* Borrowed, optional */
  const char *interface;
  SoapySDRKwargs *soapy_args;
  char *antenna;
  unsigned int channel;
  PTR_LIST(struct suscan_source_gain_value, gain);
  PTR_LIST(struct suscan_source_gain_value, hidden_gain);
};

typedef struct suscan_source_config suscan_source_config_t;

SUBOOL suscan_source_config_deserialize_ex(
    struct suscan_source_config *self,
    grow_buf_t *buffer,
    const char *force_host);

SUBOOL suscan_source_config_walk(
    SUBOOL (*function) (suscan_source_config_t *cfg, void *privdata),
    void *privdata);

/* Serialization methods */
suscan_object_t *suscan_source_config_to_object(
    const suscan_source_config_t *source);

suscan_source_config_t *suscan_source_config_from_object(
    const suscan_object_t *object);

const char *suscan_source_config_get_label(const suscan_source_config_t *source);
SUBOOL suscan_source_config_set_label(
    suscan_source_config_t *config,
    const char *label);

enum suscan_source_type suscan_source_config_get_type(
    const suscan_source_config_t *config);

enum suscan_source_format suscan_source_config_get_format(
    const suscan_source_config_t *config);

void suscan_source_config_set_type_format(
    suscan_source_config_t *config,
    enum suscan_source_type type,
    enum suscan_source_format format);

SUFREQ suscan_source_config_get_freq(const suscan_source_config_t *config);
void suscan_source_config_set_freq(
    suscan_source_config_t *config,
    SUFREQ freq);

SUFREQ suscan_source_config_get_lnb_freq(const suscan_source_config_t *config);
void suscan_source_config_set_lnb_freq(
    suscan_source_config_t *config,
    SUFREQ lnb_freq);

SUFLOAT suscan_source_config_get_bandwidth(
    const suscan_source_config_t *config);
void suscan_source_config_set_bandwidth(
    suscan_source_config_t *config,
    SUFLOAT bandwidth);

SUBOOL suscan_source_config_get_iq_balance(
    const suscan_source_config_t *config);
void suscan_source_config_set_iq_balance(
    suscan_source_config_t *config,
    SUBOOL iq_balance);

SUBOOL suscan_source_config_get_dc_remove(const suscan_source_config_t *config);
void suscan_source_config_set_dc_remove(
    suscan_source_config_t *config,
    SUBOOL dc_remove);

SUBOOL suscan_source_config_get_loop(const suscan_source_config_t *config);
void suscan_source_config_set_loop(suscan_source_config_t *config, SUBOOL loop);

const char *suscan_source_config_get_path(const suscan_source_config_t *config);
SUBOOL suscan_source_config_set_path(
    suscan_source_config_t *config,
    const char *path);

const char *suscan_source_config_get_antenna(
    const suscan_source_config_t *config);
SUBOOL suscan_source_config_set_antenna(
    suscan_source_config_t *config,
    const char *antenna);

unsigned int suscan_source_config_get_samp_rate(
    const suscan_source_config_t *config);
void suscan_source_config_set_samp_rate(
    suscan_source_config_t *config,
    unsigned int samp_rate);

unsigned int suscan_source_config_get_average(
    const suscan_source_config_t *config);
SUBOOL suscan_source_config_set_average(
    suscan_source_config_t *config,
    unsigned int average);

unsigned int suscan_source_config_get_channel(
    const suscan_source_config_t *config);

const char *suscan_source_config_get_interface(
    const suscan_source_config_t *self);

void suscan_source_config_set_channel(
    suscan_source_config_t *config,
    unsigned int channel);

struct suscan_source_gain_value *suscan_source_config_lookup_gain(
    const suscan_source_config_t *config,
    const char *name);

SUBOOL suscan_source_config_walk_gains(
    const suscan_source_config_t *config,
    SUBOOL (*gain_cb) (void *privdata, const char *name, SUFLOAT value),
    void *privdata);

SUBOOL suscan_source_config_walk_gains_ex(
    const suscan_source_config_t *config,
    SUBOOL (*gain_cb) (void *privdata, struct suscan_source_gain_value *),
    void *privdata);

struct suscan_source_gain_value *suscan_source_config_assert_gain(
    suscan_source_config_t *config,
    const char *name,
    SUFLOAT value);

SUFLOAT suscan_source_config_get_gain(
    const suscan_source_config_t *config,
    const char *name);

SUBOOL suscan_source_config_set_gain(
    suscan_source_config_t *config,
    const char *name,
    SUFLOAT value);

SUFLOAT suscan_source_config_get_ppm(const suscan_source_config_t *config);

void suscan_source_config_set_ppm(
    suscan_source_config_t *config,
    SUFLOAT ppm);

SUBOOL suscan_source_config_set_device(
    suscan_source_config_t *config,
    const suscan_source_device_t *self);

SUBOOL suscan_source_config_set_interface(
    suscan_source_config_t *self,
    const char *interface);

SUINLINE const suscan_source_device_t *
suscan_source_config_get_device(const suscan_source_config_t *config)
{
  return config->device;
}

char *suscan_source_config_get_sdr_args(const suscan_source_config_t *config);

SUBOOL suscan_source_config_set_sdr_args(
    const suscan_source_config_t *config,
    const char *args);

suscan_source_config_t *suscan_source_config_new_default(void);
suscan_source_config_t *suscan_source_config_new(
    enum suscan_source_type type,
    enum suscan_source_format format);

suscan_source_config_t *suscan_source_config_clone(
    const suscan_source_config_t *config);

void suscan_source_config_swap(
    suscan_source_config_t *config1,
    suscan_source_config_t *config2);

suscan_source_config_t *suscan_source_config_lookup(const char *label);

SUBOOL suscan_source_config_unregister(suscan_source_config_t *config);

void suscan_source_config_destroy(suscan_source_config_t *);

/****************************** Source API ***********************************/
struct suscan_source {
  suscan_source_config_t *config; /* Source may alter configuration! */
  SUBOOL capturing;
  SUBOOL soft_dc_correction;
  SUBOOL soft_iq_balance;
  SUSDIFF (*read) (
        struct suscan_source *source,
        SUCOMPLEX *buffer,
        SUSCOUNT max);

  /* File sources are accessed through a soundfile handle */
  SNDFILE *sf;
  SF_INFO sf_info;
  SUBOOL iq_file;

  /* SDR sources are accessed through SoapySDR */
  SoapySDRDevice *sdr;
  SoapySDRStream *rx_stream;
  size_t chan_array[1];
  SUFLOAT samp_rate; /* Actual sample rate */
  size_t mtu;

  /* To prevent source from looping forever */
  SUBOOL force_eos;

  /* Downsampling members */
  SUFLOAT *antialias_alloc;
  const SUFLOAT *antialias;
  SUCOMPLEX accums[SUSCAN_SOURCE_ANTIALIAS_REL_SIZE];
  SUCOMPLEX *decim_buf;
  int ptrs[SUSCAN_SOURCE_ANTIALIAS_REL_SIZE];
  int decim;
  int decim_length;
};

typedef struct suscan_source suscan_source_t;

SUBOOL suscan_source_stop_capture(suscan_source_t *source);
SUBOOL suscan_source_start_capture(suscan_source_t *source);

SUBOOL suscan_source_set_agc(suscan_source_t *source, SUBOOL set);
SUBOOL suscan_source_set_dc_remove(suscan_source_t *source, SUBOOL remove);
SUBOOL suscan_source_set_freq(suscan_source_t *source, SUFREQ freq);
SUBOOL suscan_source_set_ppm(suscan_source_t *source, SUFLOAT ppm);
SUBOOL suscan_source_set_lnb_freq(suscan_source_t *source, SUFREQ freq);
SUBOOL suscan_source_set_freq2(suscan_source_t *source, SUFREQ freq, SUFREQ lnb);

SUBOOL suscan_source_set_gain(
    suscan_source_t *source,
    const char *name,
    SUFLOAT gain);

SUBOOL suscan_source_set_bandwidth(suscan_source_t *source, SUFLOAT bw);
SUBOOL suscan_source_set_antenna(suscan_source_t *source, const char *name);

SUFREQ suscan_source_get_freq(const suscan_source_t *source);

suscan_source_t *suscan_source_new(suscan_source_config_t *config);

SUSDIFF suscan_source_read(
    suscan_source_t *source,
    SUCOMPLEX *buffer,
    SUSCOUNT max);

SUINLINE enum suscan_source_type
suscan_source_get_type(const suscan_source_t *src)
{
  return src->config->type;
}

SUINLINE SUFLOAT
suscan_source_get_samp_rate(const suscan_source_t *src)
{
  if (src->capturing)
    return src->samp_rate / src->decim;
  else
    return src->config->samp_rate;
}

SUINLINE void
suscan_source_force_eos(suscan_source_t *src)
{
  src->force_eos = SU_TRUE;
}

SUINLINE const suscan_source_config_t *
suscan_source_get_config(const suscan_source_t *src)
{
  return src->config;
}

SUINLINE const char *
suscan_source_config_get_param(
    const suscan_source_config_t *self,
    const char *key)
{
  return SoapySDRKwargs_get(self->soapy_args, key);
}

SUINLINE SUBOOL
suscan_source_config_set_param(
    const suscan_source_config_t *self,
    const char *key,
    const char *value)
{
  /* DANGER */
  SoapySDRKwargs_set(self->soapy_args, key, value);
  /* DANGER */

  return SU_TRUE;
}

SUINLINE SUBOOL
suscan_source_config_is_remote(const suscan_source_config_t *self)
{
  if (self->interface == NULL)
    return SU_FALSE;

  return strcmp(self->interface, SUSCAN_SOURCE_REMOTE_INTERFACE) == 0;
}

SUINLINE SUBOOL
suscan_source_is_capturing(const suscan_source_t *src)
{
  return src->capturing;
}

void suscan_source_destroy(suscan_source_t *config);

SUBOOL suscan_source_config_register(suscan_source_config_t *config);
SUBOOL suscan_source_detect_devices(void);

/* Internal */
SUBOOL suscan_init_sources(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* _SOURCE_H */
