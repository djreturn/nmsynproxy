#define _GNU_SOURCE
#include <pthread.h>
#include "synproxy.h"
#include "iphdr.h"
#include "ipcksum.h"
#include "packet.h"
#include "hashseed.h"
#include "yyutils.h"

static inline uint64_t gettime64(void)
{
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec*1000UL*1000UL + tv.tv_usec;
}

struct uniform_userdata {
  struct worker_local *local;
  uint32_t table_start;
  uint32_t table_end;
};

static void uniform_fn(
  struct timer_link *timer, struct timer_linkheap *heap, void *userdata)
{
  struct uniform_userdata *ud = userdata;
  uint64_t time64 = gettime64();
  uint32_t bucket;
  for (bucket = ud->table_start; bucket < ud->table_end; bucket++)
  {
    struct hash_list_node *node;
    hash_table_lock_bucket(&ud->local->hash, bucket);
    HASH_TABLE_FOR_EACH_POSSIBLE(&ud->local->hash, node, bucket)
    {
      struct synproxy_hash_entry *entry;
      entry = CONTAINER_OF(node, struct synproxy_hash_entry, node);
      if (entry->timer.time64 < time64)
      {
        entry->timer.fn(timer, heap, entry->timer.userdata);
      }
    }
    hash_table_unlock_bucket(&ud->local->hash, bucket);
  }
  timer->time64 += (1000*1000);
  worker_local_wrlock(ud->local);
  timer_linkheap_add(heap, timer);
  worker_local_wrunlock(ud->local);
}

int threadcnt = 1;

#define POOL_SIZE 300
#define CACHE_SIZE 100
#define QUEUE_SIZE 512
#define BLOCK_SIZE 1800

static void periodic(uint64_t count, struct timeval *tv1ptr)
{
  struct timeval tv2;
  if ((count & (16*1024*1024-1)) == 0)
  {
    double diff;
    gettimeofday(&tv2, NULL);
    diff = tv2.tv_sec - tv1ptr->tv_sec + (tv2.tv_usec - tv1ptr->tv_usec)/1000.0/1000.0;
    printf("%g Mpps\n", 16*1024*1024/diff/1000.0/1000.0);
    *tv1ptr = tv2;
    //exit(0);
    pthread_exit(NULL);
  }
}

struct rx_args {
  struct queue *workerq;
  struct synproxy *synproxy;
  struct worker_local *local;
  int threadidx;
};

struct pktctx {
  char pkt[1514];
  char pktsmall[64];
};

int uniformcnt = 128;

struct uniform_userdata uniform_userdata[128] = {};
struct timer_link uniformtimer[128] = {};

static void *rx_func(void *userdata)
{
  struct rx_args *args = userdata;
  int threadidx = args->threadidx;
  void *ether;
  struct pktctx ctx[90] = {};
  char cli_mac[6] = {0x02,0,0,0,0,0x04};
  char lan_mac[6] = {0x02,0,0,0,0,0x01};
  void *ip;
  void *tcp;
  struct ll_alloc_st st;
  //struct queue_cache cache;
  struct port outport;
  struct asdiscardfunc_userdata ud;
  struct timeval tv1;
  uint64_t count = 0;
  int i;
  int cnt = sizeof(ctx)/sizeof(*ctx);

  gettimeofday(&tv1, NULL);

  ud.loc = &loc;
  outport.portfunc = asdiscardfunc;
  outport.userdata = &ud;

  //if (queue_cache_init(&cache, args->workerq, CACHE_SIZE) != 0)
  //{
  //  abort();
  //}
  if (ll_alloc_st_init(&st, POOL_SIZE, BLOCK_SIZE) != 0)
  {
    abort();
  }

  if (threadidx == 0)
  {
    uint64_t time64 = gettime64();
    for (i = 0; i < uniformcnt; i++)
    {
      uniform_userdata[i].local = args->local;
      uniform_userdata[i].table_start =
        args->local->hash.bucketcnt*i/uniformcnt;
      uniform_userdata[i].table_end =
        args->local->hash.bucketcnt*(i+1)/uniformcnt;
      uniformtimer[i].userdata = &uniform_userdata[i];
      uniformtimer[i].fn = uniform_fn;
      uniformtimer[i].time64 = time64 + (i+1)*1000*1000/uniformcnt;
      timer_linkheap_add(&args->local->timers, &uniformtimer[i]);
    }
  }
  
  for (i = 0; i < cnt; i++)
  {
    ether = ctx[i].pkt;
    memcpy(ether_dst(ether), lan_mac, 6);
    memcpy(ether_src(ether), cli_mac, 6);
    ether_set_type(ether, ETHER_TYPE_IP);
    ip = ether_payload(ether);
    ip_set_version(ip, 4);
    ip_set_hdr_len(ip, 20);
    ip_set_total_len(ip, sizeof(ctx[i].pkt) - 14);
    ip_set_dont_frag(ip, 1);
    ip_set_id(ip, 123);
    ip_set_ttl(ip, 64);
    ip_set_proto(ip, 6);
    ip_set_src(ip, (10<<24)|(2*(i+cnt*threadidx)+2));
    ip_set_dst(ip, (11<<24)|(2*(i+cnt*threadidx)+1));
    ip_set_hdr_cksum_calc(ip, 20);
    tcp = ip_payload(ip);
    tcp_set_src_port(tcp, 12345);
    tcp_set_dst_port(tcp, 54321);
    tcp_set_ack_on(tcp);
    tcp_set_cksum_calc(ip, 20, tcp, sizeof(ctx[i].pkt) - 14 - 20);
  
    ether = ctx[i].pktsmall;
    memcpy(ether_dst(ether), lan_mac, 6);
    memcpy(ether_src(ether), cli_mac, 6);
    ether_set_type(ether, ETHER_TYPE_IP);
    ip = ether_payload(ether);
    ip_set_version(ip, 4);
    ip_set_hdr_len(ip, 20);
    ip_set_total_len(ip, sizeof(ctx[i].pktsmall) - 14);
    ip_set_dont_frag(ip, 1);
    ip_set_id(ip, 123);
    ip_set_ttl(ip, 64);
    ip_set_proto(ip, 6);
    ip_set_src(ip, (10<<24)|(2*(i+cnt*threadidx)+2));
    ip_set_dst(ip, (11<<24)|(2*(i+cnt*threadidx)+1));
    ip_set_hdr_cksum_calc(ip, 20);
    tcp = ip_payload(ip);
    tcp_set_src_port(tcp, 12345);
    tcp_set_dst_port(tcp, 54321);
    tcp_set_ack_on(tcp);
    tcp_set_cksum_calc(ip, 20, tcp, sizeof(ctx[i].pktsmall) - 14 - 20);
  }

  for (;;)
  {
    uint64_t expiry;
    uint64_t time64;
    int try;
    worker_local_rdlock(args->local);
    expiry = timer_linkheap_next_expiry_time(&args->local->timers);
    worker_local_rdunlock(args->local);
    (void)expiry;
    time64 = gettime64();
    worker_local_rdlock(args->local);
    try = (timer_linkheap_next_expiry_time(&args->local->timers) < time64);
    worker_local_rdunlock(args->local);
    if (try)
    {
      worker_local_wrlock(args->local);
      while (timer_linkheap_next_expiry_time(&args->local->timers) < time64)
      {
        struct timer_link *timer = timer_linkheap_next_expiry_timer(&args->local->timers);
        timer_linkheap_remove(&args->local->timers, timer);
        worker_local_wrunlock(args->local);
        timer->fn(timer, &args->local->timers, timer->userdata);
        worker_local_wrlock(args->local);
      }
      worker_local_wrunlock(args->local);
    }
    for (i = 0; i < cnt; i++)
    {
      struct packet *pktstruct;
      time64 = gettime64();
  
      pktstruct = ll_alloc_st(&st, packet_size(sizeof(ctx[i].pkt)));
      pktstruct->direction = PACKET_DIRECTION_UPLINK;
      pktstruct->sz = sizeof(ctx[i].pkt);
      memcpy(packet_data(pktstruct), ctx[i].pkt, sizeof(ctx[i].pkt));
      if (uplink(args->synproxy, args->local, pktstruct, &outport, time64, &st))
      {
        ll_free_st(&st, pktstruct);
      }
      count++;
      periodic(count, &tv1);
  
  
      pktstruct = ll_alloc_st(&st, packet_size(sizeof(ctx[i].pkt)));
      pktstruct->direction = PACKET_DIRECTION_UPLINK;
      pktstruct->sz = sizeof(ctx[i].pkt);
      memcpy(packet_data(pktstruct), ctx[i].pkt, sizeof(ctx[i].pkt));
      if (uplink(args->synproxy, args->local, pktstruct, &outport, time64, &st))
      {
        ll_free_st(&st, pktstruct);
      }
      count++;
      periodic(count, &tv1);
  
      pktstruct = ll_alloc_st(&st, packet_size(sizeof(ctx[i].pktsmall)));
      pktstruct->direction = PACKET_DIRECTION_UPLINK;
      pktstruct->sz = sizeof(ctx[i].pktsmall);
      memcpy(packet_data(pktstruct), ctx[i].pktsmall, sizeof(ctx[i].pktsmall));
      if (uplink(args->synproxy, args->local, pktstruct, &outport, time64, &st))
      {
        ll_free_st(&st, pktstruct);
      }
      count++;
      periodic(count, &tv1);
    }
  }
}


int main(int argc, char **argv)
{
  pthread_t rx[64];
  struct rx_args rx_args[64];
  struct synproxy synproxy;
  //struct queue workerq;
  //struct queue txq;
  struct worker_local local;
  cpu_set_t cpuset;
  struct conf conf = CONF_INITIALIZER;
  int i,j;

  confyydirparse(argv[0], "conf.txt", &conf, 0);
  synproxy_init(&synproxy, &conf);

  hash_seed_init();
  setlinebuf(stdout);

  //if (queue_init(&workerq, QUEUE_SIZE) != 0)
  //{
  //  abort();
  //}
  //if (queue_init(&txq, QUEUE_SIZE) != 0)
  //{
  //  abort();
  //}

  //worker_local_init(&local, &synproxy, 0, 0);
  worker_local_init(&local, &synproxy, 0, 1);
  //synproxy_hash_put_connected(
  //  &local, (10<<24)|2, 12345, (11<<24)|1, 54321, gettime64());
  for (j = 0; j < 90*threadcnt; j++)
  {
    synproxy_hash_put_connected(
      &local, (10<<24)|(2*j+2), 12345, (11<<24)|(2*j+1), 54321,
      gettime64());
  }

  for (i = 0; i < threadcnt; i++)
  {
    //rx_args[i].workerq = &workerq;
    rx_args[i].synproxy = &synproxy;
    rx_args[i].local = &local;
    rx_args[i].threadidx = i;

    pthread_create(&rx[i], NULL, rx_func, &rx_args[i]);
    CPU_ZERO(&cpuset);
    CPU_SET(i, &cpuset);
    pthread_setaffinity_np(rx[i], sizeof(cpuset), &cpuset);
  }
  for (i = 0; i < threadcnt; i++)
  {
    pthread_join(rx[i], NULL);
  }

  synproxy_free(&synproxy);

  return 0;
}
