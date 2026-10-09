"""Interleave native DNS replies and a TCP poll through the real receive router."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_gui_hover import source_block

ROOT=Path(__file__).resolve().parents[1]


class DnsRoutingTests(unittest.TestCase):
    def test_interleaved_replies_validation_and_slot_reuse(self):
        cc=shutil.which('clang') or shutil.which('cc')
        if not cc:self.skipTest('host C compiler missing')
        source=(ROOT/'src/kernel/net/net.c').read_text(encoding='utf-8')
        start=source.index('struct dns_pending_query {')
        structure=source[start:source.index('\n};',start)+3]
        actual='\n'.join(source_block(source, marker) for marker in [
            'static struct dns_pending_query *dns_query_open(',
            'static int net_dns_dispatch_frame(const void *frame, size_t len) {',
            'static size_t dns_query_take(', 'static size_t dev_recv(void *buf, size_t max) {',
            'static int net_tcp_poll_once(void) {'])
        fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "net.h"
#define MAX_TASKS 4
static uint32_t net_dns_ip=0x0302000A;
uint32_t net_ip=0x0F02000A;
static uint16_t dns_next_id=0x1234,dns_next_port=49152;
static uint32_t irq_state;
static uint32_t irq_save(void){uint32_t old=irq_state;irq_state=1;return old;}
static void irq_restore(uint32_t flags){irq_state=flags;}
static uint16_t bswap16(uint16_t x){return (x<<8)|(x>>8);}
static uint8_t frames[8][1514];static size_t lengths[8];static int in,out,tcp_frames;
static size_t dev_recv_raw(void *buf,size_t max){if(out==in)return 0;assert(lengths[out]<=max);size_t n=lengths[out];memcpy(buf,frames[out++],n);return n;}
static int net_tcp_dispatch_frame(const void *f,size_t n){if(n>34&&((const uint8_t*)f)[23]==6){tcp_frames++;return 1;}return 0;}
'''+structure+r'''
static struct dns_pending_query dns_queries[MAX_TASKS];
'''+actual+r'''
static size_t packet(uint8_t *f,const struct dns_pending_query *slot,uint8_t marker){
 memset(f,0,1514);struct eth_frame *eth=(void*)f;eth->ethertype=bswap16(0x0800);
 struct ip_hdr *ip=(void*)eth->payload;ip->ver_ihl=0x45;ip->protocol=17;ip->src_ip=net_dns_ip;ip->dst_ip=net_ip;
 ip->total_len=bswap16(20+8+13);struct udp_hdr *udp=(void*)(ip+1);
 udp->src_port=bswap16(53);udp->dst_port=bswap16(slot->port);udp->length=bswap16(8+13);
 uint8_t *dns=(void*)(udp+1);dns[0]=slot->txid>>8;dns[1]=slot->txid;dns[2]=0x80;dns[12]=marker;
 return 14+20+8+13;
}
int main(void){
 struct dns_pending_query *a=dns_query_open(),*b=dns_query_open();assert(a&&b&&a->port!=b->port);
 /* A transport waiting for TCP drains both replies in reverse order. */
 lengths[in]=packet(frames[in],b,22);in++;lengths[in]=packet(frames[in],a,11);in++;
 lengths[in]=packet(frames[in],a,0);frames[in][23]=6;in++;
 uint8_t buffer[1514];assert(net_tcp_poll_once()==1);
 assert(dev_recv(buffer,sizeof(buffer))==0);assert(tcp_frames==1);
 assert(dns_query_take(a,buffer)==13&&buffer[12]==11);
 assert(dns_query_take(b,buffer)==13&&buffer[12]==22);
 assert(!dns_query_take(a,buffer));
 uint8_t f[1514];size_t n=packet(f,a,33);
 assert(net_dns_dispatch_frame(f,n));packet(f,a,44);assert(net_dns_dispatch_frame(f,n));
 assert(dns_query_take(a,buffer)==13&&buffer[12]==33); /* no overwrite */
 packet(f,a,55);f[43]^=1;assert(!net_dns_dispatch_frame(f,n));assert(!a->reply_length); /* wrong ID */
 packet(f,a,55);((struct ip_hdr*)(f+14))->src_ip^=1;assert(!net_dns_dispatch_frame(f,n));
 packet(f,a,55);((struct ip_hdr*)(f+14))->dst_ip^=1;assert(!net_dns_dispatch_frame(f,n));
 packet(f,a,55);((struct udp_hdr*)(f+34))->length=bswap16(8+11);assert(!net_dns_dispatch_frame(f,n));
 packet(f,a,55);assert(!net_dns_dispatch_frame(f,n-1));
 packet(f,a,55);((struct ip_hdr*)(f+14))->frag_off=bswap16(0x2000);assert(!net_dns_dispatch_frame(f,n));
 packet(f,a,55);((struct ip_hdr*)(f+14))->total_len=bswap16(20+8+12);assert(!net_dns_dispatch_frame(f,n));
 assert(dns_query_open()&&dns_query_open());assert(!dns_query_open());
 uint16_t old=a->txid;a->used=0;assert(dns_query_open()==a&&a->txid!=old&&!a->reply_length);
 assert(!irq_state);return 0;
}
'''
        with tempfile.TemporaryDirectory() as temp:
            c=Path(temp)/'routing.c';exe=Path(temp)/'routing.exe'
            c.write_text(fixture,encoding='utf-8')
            subprocess.run([cc,str(c),'-I'+str(ROOT/'src/kernel/net'),'-o',str(exe)],check=True,capture_output=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)


if __name__=='__main__':unittest.main()
