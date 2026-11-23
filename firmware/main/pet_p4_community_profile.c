#include "pet_p4_community.h"
#include "pet_p4_json_source.h"
#include "cJSON.h"
#include <string.h>

#define field cJSON_GetObjectItemCaseSensitive
static bool unique(const cJSON *root) {
  const cJSON *v;
  cJSON_ArrayForEach(v, root) {
    if (v->string && field(root, v->string) != v) return false;
    if (!unique(v)) return false;
  }
  return true;
}

cJSON *pet_community_json(const uint8_t *bytes, size_t n) {
  if (!bytes || !n || !pet_json_source_text_limit_bytes(bytes, n, 12)) return NULL;
  const char *end = NULL;
  cJSON *root = cJSON_ParseWithLengthOpts((const char *)bytes, n, &end, false);
  while (root && end < (const char *)bytes+n && strchr(" \n\t\r", *end)) ++end;
  if (end != (const char *)bytes+n || !unique(root)) {
    cJSON_Delete(root);
    return NULL;
  }
  return root;
}

static bool str(const cJSON *v, const char *s) {
  return cJSON_IsString(v) && !strcmp(v->valuestring, s);
}

static bool printable(const cJSON *v, size_t max) {
  if (!cJSON_IsString(v) || !v->valuestring[0] || strlen(v->valuestring) > max) return false;
  for (const unsigned char *p=(const unsigned char *)v->valuestring; *p; ++p)
    if (*p < 32 || *p == 127) return false;
  return true;
}

static bool integer(const cJSON *v, unsigned max) {
  return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= max &&
    v->valuedouble == (unsigned)v->valuedouble;
}

static bool version_ok(const char *v) {
  if (!v || strlen(v)>32) return false;
  for (unsigned i=0; i<3; ++i) {
    size_t n=strspn(v,"0123456789");
    if (!n || (n>1 && *v=='0')) return false;
    v+=n;
    if (i<2 && *v++!='.') return false;
  }
  return !*v;
}

bool pet_community_newer(const char *a, const char *b) {
  if (!version_ok(a) || !version_ok(b)) return false;
  for (int i=0; i<3; ++i) {
    size_t x=strspn(a,"0123456789"), y=strspn(b,"0123456789");
    if (x!=y) return x>y;
    int cmp=memcmp(a,b,x);
    if (cmp) return cmp>0;
    if (i<2) { a+=x+1; b+=y+1; }
  }
  return false;
}

static bool digest(const cJSON *v) {
  return cJSON_IsString(v) && strlen(v->valuestring)==64 &&
    strspn(v->valuestring,"0123456789abcdef")==64;
}
static bool row(const cJSON *v, pet_community_item_t *out) {
  const cJSON *id=field(v,"id"), *name=field(v,"name"), *ver=field(v,"version"),
    *hash=field(v,"sha256"), *bytes=field(v,"downloadBytes"), *total=field(v,"installBytes"),
    *count=field(v,"fileCount"), *privileges=field(v,"privileges");
  if (!cJSON_IsObject(v) || cJSON_GetArraySize(v)!=9 || !printable(id,43) ||
      id->valuestring[0]<'a' || id->valuestring[0]>'z' ||
      id->valuestring[strspn(id->valuestring,"abcdefghijklmnopqrstuvwxyz0123456789_-")] ||
      !printable(name,96) || !cJSON_IsString(ver) || !version_ok(ver->valuestring) ||
      !digest(hash) || !digest(field(v,"sourceSha256")) || !integer(count,2) || count->valueint!=2 ||
      !integer(total,6142) || total->valueint<2 || !integer(bytes,PET_COMMUNITY_PAYLOAD_MAX) ||
      (size_t)bytes->valueint!=total->valueint+PET_COMMUNITY_FRAME_OVERHEAD || !integer(privileges,1))
    return false;
  strcpy(out->id,id->valuestring); strcpy(out->name,name->valuestring);
  strcpy(out->version,ver->valuestring); strcpy(out->digest,hash->valuestring);
  out->bytes=bytes->valueint; out->privileges=privileges->valueint;
  return true;
}

bool pet_community_parse_catalog(const uint8_t *data,size_t size,const char *nonce,
    const char *after,pet_community_catalog_t *out) {
  if (!out || !nonce || !after || size>4096) return false;
  cJSON *root=pet_community_json(data,size);
  const cJSON *items=field(root,"items"), *next=field(root,"next"), *schema=field(root,"schema");
  bool ok=cJSON_IsObject(root) && cJSON_GetArraySize(root)==7 && integer(schema,1) && schema->valueint==1 &&
    str(field(root,"board"),"wifi6-p4") && str(field(root,"profile"),"p4-local-v1") &&
    str(field(root,"nonce"),nonce) && str(field(root,"after"),after) && cJSON_IsString(next) &&
    cJSON_IsArray(items) && cJSON_GetArraySize(items)<=PET_COMMUNITY_ROWS;
  out->count=0; out->next[0]=0;
  const char *previous=after;
  const cJSON *v;
  cJSON_ArrayForEach(v,items) {
    if (!ok) break;
    pet_community_item_t *item=&out->items[out->count++];
    ok=row(v,item) && strcmp(item->id,previous)>0;
    previous=item->id;
  }
  if (ok && next->valuestring[0]) {
    ok=out->count==PET_COMMUNITY_ROWS && !strcmp(next->valuestring,previous);
    if (ok) strcpy(out->next,previous);
  }
  if (!ok) out->count=0;
  cJSON_Delete(root);
  return ok;
}

/* This is the authority boundary. The normal runtime parser subsequently
 * enforces its own device quotas, bindings, save semantics and geometry. */
static bool keys(const cJSON *obj, const char *allowed) {
  if (!cJSON_IsObject(obj)) return false;
  const cJSON *v;
  cJSON_ArrayForEach(v,obj) {
    const char *p=allowed;
    size_t n=strlen(v->string);
    while (*p) {
      size_t len=strcspn(p," ");
      if (n==len && !memcmp(p,v->string,n)) break;
      p+=len;
      if (*p) ++p;
    }
    if (!*p) return false;
  }
  return true;
}

static bool ident(const char *s) {
  return s && *s>='a' && *s<='z' && strlen(s)<=23 &&
    !s[strspn(s,"abcdefghijklmnopqrstuvwxyz0123456789_")];
}
static bool member(const cJSON *array,const char *s) {
  const cJSON *v;
  cJSON_ArrayForEach(v,array) if (str(v,s)) return true;
  return false;
}
static bool within(const cJSON *array,int min,int max) {
  int n=cJSON_GetArraySize(array);
  return cJSON_IsArray(array) && n>=min && n<=max;
}
static bool constant(const cJSON *v) {
  return cJSON_IsNumber(v) && v->valuedouble>=-1e9 && v->valuedouble<=1e9 && v->valuedouble==v->valueint;
}
static bool states_vars(const cJSON *states,const cJSON *initial,const cJSON *vars) {
  if (!within(states,1,6) || !cJSON_IsString(initial) || !member(states,initial->valuestring) ||
      !cJSON_IsObject(vars) || cJSON_GetArraySize(vars)>8) return false;
  const cJSON *v;
  cJSON_ArrayForEach(v,states) {
    if (!cJSON_IsString(v) || !ident(v->valuestring)) return false;
    for (const cJSON *p=states->child;p!=v;p=p->next) if (str(p,v->valuestring)) return false;
  }
  cJSON_ArrayForEach(v,vars)
    if (!ident(v->string) || !keys(v,"type init") || !str(field(v,"type"),"int") || !constant(field(v,"init"))) return false;
  return true;
}
static bool actions(const cJSON *buttons,const cJSON *rules,const cJSON *states,const cJSON *vars) {
  if (!within(buttons,0,2) || !within(rules,0,12)) return false;
  const cJSON *v;
  cJSON_ArrayForEach(v,buttons) {
    const cJSON *action=field(v,"action"), *control=field(v,"control");
    if (!keys(v,"action control event label") || !cJSON_IsString(action) || !ident(action->valuestring) ||
        strncmp(action->valuestring,"local_",6) || !printable(field(v,"label"),30) ||
        (!(str(control,"SW1") && str(field(v,"event"),"button.sw1.short_press")) &&
        !(str(control,"SW2") && str(field(v,"event"),"button.sw2.short_press")))) return false;
  }
  cJSON_ArrayForEach(v,rules) {
    const cJSON *from=field(v,"from"), *to=field(v,"to"), *on=field(v,"on");
    if (!keys(v,"from to on set inc") || !cJSON_IsString(on) || !cJSON_IsString(from) ||
        (!str(from,"*") && !member(states,from->valuestring)) ||
        (to && (!cJSON_IsString(to) || !member(states,to->valuestring)))) return false;
    bool bound=false;
    for (const cJSON *b=buttons->child;b;b=b->next) if (str(field(b,"action"),on->valuestring)) bound=true;
    if (!bound) return false;
    const char *const effects[]={"set","inc"};
    for (unsigned i=0;i<2;i++) {
      const cJSON *effect=field(v,effects[i]);
      if (effect && (!cJSON_IsObject(effect) || !effect->child)) return false;
      const cJSON *e;
      cJSON_ArrayForEach(e,effect) if (!field(vars,e->string) || !constant(e)) return false;
    }
  }
  return true;
}
static bool dashboard(const cJSON *d,const cJSON *vars,const cJSON *states) {
  if (!d || !d->child || !keys(d,"title headline footer metricValue visualStyle visualPalette visualLayout visualSprite")) return false;
  const cJSON *v;
  cJSON_ArrayForEach(v,d) {
    if (printable(v,96)) continue;
    const cJSON *ref=field(v,"var"), *map=field(v,"switch_state");
    if (!cJSON_IsObject(v) || cJSON_GetArraySize(v)!=1) return false;
    if (cJSON_IsString(ref) && field(vars,ref->valuestring)) continue;
    if (!cJSON_IsObject(map) || !map->child) return false;
    const cJSON *value;
    cJSON_ArrayForEach(value,map) if (!member(states,value->string) || !printable(value,96)) return false;
  }
  return true;
}
static bool surface_ok(const cJSON *s,const cJSON *d) {
  const cJSON *nodes=field(s,"elements");
  if (!keys(s,"engine graphics background elements") || !str(field(s,"engine"),"p4-app-surface-v1") ||
      !str(field(s,"graphics"),"p4-app-graphics-v1") || !integer(field(s,"background"),65535) || !within(nodes,1,8)) return false;
  const cJSON *n;
  cJSON_ArrayForEach(n,nodes) {
    const cJSON *type=field(n,"type"), *x=field(n,"x"), *y=field(n,"y"), *w=field(n,"w"), *h=field(n,"h");
    if (!integer(x,639) || !integer(y,479) || !integer(w,640-x->valueint) || !w->valueint ||
        !integer(h,480-y->valueint) || !h->valueint || !integer(field(n,"color"),65535)) return false;
    if (str(type,"rect") && keys(n,"type x y w h color")) continue;
    const cJSON *bind=field(n,"bind"), *label=field(n,"text");
    if (!str(type,"text") || !keys(n,"type x y w h color bind text") || !!bind==!!label ||
        (label && !printable(label,96)) || (bind && (!cJSON_IsString(bind) || !field(d,bind->valuestring)))) return false;
  }
  return true;
}

static bool save_ok(const cJSON *p,const cJSON *vars,const cJSON *states) {
  const cJSON *version=field(p,"version"), *saved=field(p,"vars"), *map=field(p,"states"), *v;
  if (!keys(p,"version vars states") || !integer(version,65535) || !version->valueint ||
      !within(saved,1,8) || !cJSON_IsObject(map) || cJSON_GetArraySize(map)!=cJSON_GetArraySize(states)) return false;
  cJSON_ArrayForEach(v,saved) {
    if (!cJSON_IsString(v) || !field(vars,v->valuestring)) return false;
    for (const cJSON *prev=saved->child;prev!=v;prev=prev->next) if (str(prev,v->valuestring)) return false;
  }
  cJSON_ArrayForEach(v,map) if (!member(states,v->string) || !cJSON_IsString(v) || !member(states,v->valuestring)) return false;
  return true;
}

static bool profile(cJSON *widget,cJSON *buttons,unsigned privileges) {
  const cJSON *vars=field(widget,"vars"), *states=field(widget,"states"), *d=field(widget,"dashboard");
  const cJSON *s=field(widget,"surface"), *p=field(widget,"persist"), *schema=field(widget,"schema_version");
  return keys(widget,"schema_version engine states initial_state vars transitions tick dashboard surface persist") &&
    integer(schema,1) && schema->valueint==1 && str(field(widget,"engine"),"p4-bounded-runtime-v4") &&
    within(field(widget,"tick"),0,0) && states_vars(states,field(widget,"initial_state"),vars) &&
    actions(buttons,field(widget,"transitions"),states,vars) && dashboard(d,vars,states) &&
    (!s || surface_ok(s,d)) && privileges==(unsigned)!!p && (!p || (s && save_ok(p,vars,states)));
}

static bool file(const uint8_t **cursor,size_t *remaining,const char *path,size_t max,
    const uint8_t **out,size_t *size) {
  size_t prefix=strlen(path)+1;
  if (*remaining<prefix+4 || memcmp(*cursor,path,prefix)) return false;
  const uint8_t *p=*cursor+prefix;
  uint32_t n=p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
  if (!n || n>max || n>*remaining-prefix-4) return false;
  *out=p+4; *size=n; *cursor=*out+n; *remaining-=prefix+4+n;
  return true;
}

bool pet_community_parse_payload(const pet_community_item_t *item,const uint8_t *data,
    size_t size,pet_community_files_t *files) {
  if (!item || !files || !data || size!=item->bytes || size>PET_COMMUNITY_PAYLOAD_MAX ||
      !file(&data,&size,"buttons.json",PET_COMMUNITY_BUTTONS_MAX,&files->buttons,&files->buttons_len) ||
      !file(&data,&size,"runtime/widget.json",PET_COMMUNITY_WIDGET_MAX,&files->widget,&files->widget_len) || size) return false;
  cJSON *widget=pet_community_json(files->widget,files->widget_len);
  cJSON *buttons=pet_community_json(files->buttons,files->buttons_len);
  bool ok=profile(widget,buttons,item->privileges);
  if (ok && item->privileges) {
    char *contract=cJSON_PrintUnformatted(field(widget,"persist"));
    ok=contract && pet_community_hash(contract,strlen(contract),files->save_contract);
    cJSON_free(contract);
  } else strcpy(files->save_contract,"-");
  cJSON_Delete(widget); cJSON_Delete(buttons);
  return ok;
}
