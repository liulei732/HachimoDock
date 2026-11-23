#ifndef PET_COMPONENT_NETWORK_DEVICE_H
#define PET_COMPONENT_NETWORK_DEVICE_H
#include "pet_component_network.h"
#if defined(PET_P4_COMPONENT_NETWORK) && PET_P4_COMPONENT_NETWORK &&                               \
    defined(PET_P4_SYSTEM_WIFI) && PET_P4_SYSTEM_WIFI
#define PET_COMPONENT_NETWORK_ENABLED 1
bool pet_net_device_storage_ready(void);
bool pet_net_device_activate(const char *, const cJSON *, pet_net_lease_t *);
bool pet_net_device_forget(const char *);
int pet_net_device_provision(const char *, const char *, const char *);
int pet_net_device_provision_target(const char *, const char *, const char *, const char *);
cJSON *pet_net_device_status(void);
#else
#define PET_COMPONENT_NETWORK_ENABLED 0
static inline bool pet_net_device_storage_ready(void) { return false; }
static inline int pet_net_device_provision(const char *o, const char *k, const char *v) {
  (void)o;
  (void)k;
  (void)v;
  return PET_NET_UNAVAILABLE;
}
static inline int pet_net_device_provision_target(const char *o,const char *k,const char *v,const char *target) {
  (void)target; return pet_net_device_provision(o,k,v);
}
static inline cJSON *pet_net_device_status(void) { return NULL; }
static inline bool pet_net_device_activate(const char *id, const cJSON *policy,
                                           pet_net_lease_t *lease) {
  (void)id;
  (void)policy;
  if (lease)
    *lease = (pet_net_lease_t){0};
  return false;
}
static inline bool pet_net_device_forget(const char *id) {
  (void)id;
  return true;
}
#endif
#endif
