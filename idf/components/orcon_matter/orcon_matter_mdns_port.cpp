// UDP 5353 is shared by two mDNS responders: Orcon's (IDF mdns, orcon15rf.local, net.cpp MDNS.begin) and CHIP's
// minimal mDNS (Matter discovery). lwIP binds a second PCB to a port only when BOTH carry SOF_REUSEADDR. CHIP sets it
// (UDPEndPointImplLwIP GetPCB), IDF's mdns does not (mdns_networking_lwip.c pcb_init). Without it CHIP's DNS-SD never
// listens (minimal_mdns ServerBase::Listen stops at the failed bind), so kServerReady never comes: Matter fails with
// "server-timeout" and a trial boot is not confirmed. That rolled the first Matter build back on a board (25-11).
// Every bind of 5353 gets the flag, in either order and again when IDF's mdns rebinds after a WiFi outage. With
// CONFIG_LWIP_SO_REUSE_RXTOALL both PCBs receive every multicast mDNS packet; each answers only for its own records.
#include <lwip/ip.h>
#include <lwip/udp.h>
#include <sdkconfig.h>

#if CONFIG_ORCON_MATTER_SHARE_MDNS_PORT
extern "C" err_t __real_udp_bind(struct udp_pcb* pcb, const ip_addr_t* ipaddr, u16_t port);
extern "C" err_t __wrap_udp_bind(struct udp_pcb* pcb, const ip_addr_t* ipaddr, u16_t port) {
    if (pcb && port == 5353) ip_set_option(pcb, SOF_REUSEADDR);   // lwIP's own thread, as every udp_bind caller
    return __real_udp_bind(pcb, ipaddr, port);
}
#endif
