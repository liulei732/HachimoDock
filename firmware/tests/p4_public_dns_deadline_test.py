"""Run the ESP public DNS slot lifecycle against a controllable lwIP callback queue."""
from pathlib import Path
import subprocess

MAIN = Path(__file__).resolve().parents[1] / "main"


def test_late_dns_deadline_cancellation_cache_and_capacity(tmp_path):
    source = (MAIN / "pet_p4_live_tls.c").read_text()
    logic = source[source.index("enum { DNS_FREE"):source.index("esp_tls_t *pet_live_tls_connect_public_port")]
    fixture = tmp_path / "dns.c"
    fixture.write_text(r'''
#include <assert.h>
#include <arpa/inet.h>
#include <stdatomic.h>
#include "pet_p4_wifi_endpoint.h"
#define DNS_MAX_HOST_IP 2
#define LWIP_DNS_ADDRTYPE_IPV4 0
#define ERR_OK 0
#define ERR_INPROGRESS 1
#define IP_IS_V4(a) (true)
#define ip_2_ip4(a) (a)
#define ip4_addr_get_u32(a) (*(a))
#define pdMS_TO_TICKS(ms) (ms)
typedef uint32_t ip_addr_t;
typedef int err_t;
typedef bool (*pet_live_tls_current_fn)(void *);
static int64_t now,cancel_at=INT64_MAX;
static int mode;
static void (*queued_fn)(void *); static void *queued_arg;
static void (*reply)(const char *,const ip_addr_t *,void *);static void *reply_arg;
static int64_t esp_timer_get_time(void){return now*1000;}
static void vTaskDelay(unsigned delay){now+=delay;}
static bool current(void *unused){(void)unused;return now<cancel_at;}
static int tcpip_try_callback(void (*fn)(void *),void *arg){
  if(mode==3)return -1;
  if(mode==4){queued_arg=arg;queued_fn=fn;}else fn(arg);
  return 0;
}
static int dns_gethostbyname_addrtype(const char *host,ip_addr_t *addr,
 void (*found)(const char *,const ip_addr_t *,void *),void *arg,int type){
  assert(!strcmp(host,"feed.example.org") && type==0);
  addr[0]=inet_addr("10.0.0.5");addr[1]=mode==2?0:inet_addr("8.8.8.8");
  if(mode==1){reply=found;reply_arg=arg;return ERR_INPROGRESS;}
  return ERR_OK;
}
''' + logic + r'''
int main(void){
 const char *host="feed.example.org";char ip[INET_ADDRSTRLEN]={0};
 mode=1;assert(!public_dns_resolve(host,10000,current,NULL,ip)&&now>=2000&&now<2050);
 void *late=reply_arg;
 mode=0;assert(public_dns_resolve(host,now+5000,current,NULL,ip)&&!strcmp(ip,"8.8.8.8"));
 mode=4;cancel_at=now+10;
 assert(!public_dns_resolve(host,now+5000,current,NULL,ip)&&now<cancel_at+30);
 cancel_at=INT64_MAX;mode=0;assert(!public_dns_resolve(host,now+5000,current,NULL,ip));
 queued_fn(queued_arg); /* Already cancelled when the tcpip thread starts. */
 ip_addr_t stale[2]={inet_addr("1.1.1.1"),0};reply(host,stale,late);
 assert(public_dns_resolve(host,now+5000,current,NULL,ip)&&!strcmp(ip,"8.8.8.8"));
 mode=2;assert(!public_dns_resolve(host,now+5000,current,NULL,ip));
 mode=3;assert(!public_dns_resolve(host,now+5000,current,NULL,ip));
 mode=1;int64_t start=now;assert(!public_dns_resolve(host,start+40,current,NULL,ip)&&now<start+65);
 reply(host,NULL,reply_arg);
 for(unsigned i=0;i<2;i++)assert(atomic_load(&public_dns_slots[i].state)==DNS_FREE);
}
''')
    exe = tmp_path / "dns"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(MAIN), str(fixture), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=5)
