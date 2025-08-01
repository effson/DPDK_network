#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>

#include <stdio.h>
#include <arpa/inet.h>

#define NUM_MBUFS (4096-1)
#define BURST_SIZE	32

#define ENABLE_SEND		1
#define ENABLE_ARP		1

int gDpdkPortId = 0;

#if ENABLE_SEND
#define MAKE_IPV4_ADDR(a, b, c, d) (a + (b<<8) + (c<<16) + (d<<24))
static const uint32_t gLocalIp = MAKE_IPV4_ADDR(192, 168, 23, 173); // 本机 IP 地址

static uint32_t gSrcIp; //
static uint32_t gDstIp;
static uint8_t gSrcMac[RTE_ETHER_ADDR_LEN];
static uint8_t gDstMac[RTE_ETHER_ADDR_LEN];
static uint16_t gSrcPort;
static uint16_t gDstPort;
#endif

static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .mq_mode = RTE_ETH_MQ_RX_RSS,
        .mtu = RTE_ETHER_MTU
    }
};

static void ng_init_port(struct rte_mempool *mbuf_pool) {
    uint16_t nb_sys_ports = rte_eth_dev_count_avail();
    if (nb_sys_ports == 0) {
        rte_exit(EXIT_FAILURE, "No available Ethernet ports!\n");
    }

    /* 查询某个 DPDK 网卡端口（gDpdkPortId）的硬件能力和驱动信息，用于后续配置收发队列、判断支持哪些特性 */
    struct rte_eth_dev_info dev_info;
    if (rte_eth_dev_info_get(gDpdkPortId, &dev_info) != 0) {
        rte_exit(EXIT_FAILURE, "Failed to get device info for port %u\n", gDpdkPortId);
    }

    const int num_rx_queues = 1;
    const int num_tx_queues = 1;
    struct rte_eth_conf eth_conf = port_conf_default;
    rte_eth_dev_configure(gDpdkPortId, num_rx_queues, num_tx_queues, &eth_conf);

    /* 调用 DPDK 接口设置某个网卡端口的 接收队列（RX queue） */
    if (rte_eth_rx_queue_setup(gDpdkPortId, 0, 1024, rte_eth_dev_socket_id(gDpdkPortId),
                            NULL, mbuf_pool) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to setup RX queue for port %u\n", gDpdkPortId);
    }
    
#if ENABLE_SEND
    struct rte_eth_txconf tx_conf = dev_info.default_txconf;
    tx_conf.offloads = eth_conf.rxmode.offloads; // 设置发送队列的 offload 功能
    if (rte_eth_tx_queue_setup(gDpdkPortId, 0, 1024, 
        rte_eth_dev_socket_id(gDpdkPortId), &tx_conf) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to setup TX queue for port %u\n", gDpdkPortId);
    }
#endif

    if (rte_eth_dev_start(gDpdkPortId) < 0) {
        rte_exit(EXIT_FAILURE, "Failed to start port %u\n", gDpdkPortId);
    }
    rte_eth_promiscuous_enable(gDpdkPortId); // 设置网卡端口为混杂模式，接收所有数据包
}

static int ng_encode_udp_pkt(uint8_t *msg, unsigned char *data, uint16_t total_len) { 
    // 1. ethhdr
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth->src_addr.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth->dst_addr.addr_bytes, gDstMac, RTE_ETHER_ADDR_LEN);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // 2. ip hdr
    struct rte_ipv4_hdr *ipv4 = (struct rte_ipv4_hdr *)(msg + sizeof(struct rte_ether_hdr));     
    /* 
     * 高 4 位表示IP 协议版本号: 版本号为 4，表示 IPv4 协议
     * 低 4 位表示IP 首部长度（IHL，单位为 4 字节）: 首部长度为 20 字节
     */
    ipv4->version_ihl = 0x45;
    ipv4->type_of_service = 0; // 服务类型
    ipv4->total_length = rte_cpu_to_be_16(total_len - sizeof(struct rte_ether_hdr));
    ipv4->packet_id = 0; // 分组标识符
    ipv4->fragment_offset = 0; // 分片偏移
    ipv4->time_to_live = 64; // 生存时间
    ipv4->next_proto_id = IPPROTO_UDP; // 协议类型为 UDP
    ipv4->hdr_checksum = 0; // 首部校验和，后续计算
    ipv4->src_addr = gSrcIp; // 源 IP 地址
    ipv4->dst_addr = gDstIp; // 目的 IP 地址
    ipv4->hdr_checksum = rte_ipv4_cksum(ipv4); // 计算 IPv4 首部校验和

    // 3. udp hdr
    struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(msg + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_ether_hdr));
    udp->src_port = gSrcPort; // 源端口
    udp->dst_port = gDstPort; // 目的端口
    uint16_t udp_length = total_len - sizeof(struct rte_ether_hdr) - sizeof(struct rte_ipv4_hdr);
    udp->dgram_len = rte_cpu_to_be_16(udp_length); // UDP

    rte_memcpy((uint8_t *)(udp + 1), data, udp_length - sizeof(struct rte_udp_hdr)); // 拷贝数据到 UDP 数据部分
    udp->dgram_cksum = 0; // UDP 校验和
    udp->dgram_cksum = rte_ipv4_udptcp_cksum(ipv4, udp); // 计算 UDP 校验和

    return 0;
}

static struct rte_mbuf *ng_send_udp(struct rte_mempool *mbuf_pool, uint8_t *data, uint16_t length) {
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (mbuf == NULL) {
        rte_exit(EXIT_FAILURE, "Failed to allocate mbuf!\n");
    }

    const unsigned total_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + length;
    mbuf->data_len = total_len;
    mbuf->pkt_len = total_len;
    mbuf->nb_segs = 1;
    mbuf->port = gDpdkPortId;

    uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_udp_pkt(pkt_data, data, total_len);
    
    return mbuf;
}

#if ENABLE_ARP
static int ng_encode_arp_pkt(uint8_t *msg, uint8_t *dst_mac, uint32_t sip, uint32_t dip) {
    // 1 ethhdr
    struct rte_ether_hdr *eth = (struct rte_ether_hdr *)msg;
    rte_memcpy(eth->src_addr.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(eth->dst_addr.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    // 2 arp hdr
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER); // 硬件地址格式为以太网
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4); // 协议类型为 IPv4
    arp->arp_hlen = RTE_ETHER_ADDR_LEN; // 硬件地址长度,硬件大小
    arp->arp_plen = sizeof(uint32_t); // 协议地址长度,协议大小
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY); // ARP 操作码为响应，即回复

    rte_memcpy(arp->arp_data.arp_sha.addr_bytes, gSrcMac, RTE_ETHER_ADDR_LEN);
    rte_memcpy(arp->arp_data.arp_tha.addr_bytes, dst_mac, RTE_ETHER_ADDR_LEN);
    arp->arp_data.arp_sip = sip;
    arp->arp_data.arp_tip = dip;

    return 0;
}

static  struct rte_mbuf *ng_send_arp(struct rte_mempool *mbuf_pool, uint8_t *dst_mac, uint32_t sip, uint32_t dip) {
    const unsigned total_length = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(mbuf_pool);
    if (!mbuf) {
	    rte_exit(EXIT_FAILURE, "rte_pktmbuf_alloc\n");
    }
    mbuf->data_len = total_length;
    mbuf->pkt_len = total_length;

    uint8_t *pkt_data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    ng_encode_arp_pkt(pkt_data, dst_mac, sip, dip);

    return mbuf;
}

#endif


int main(int argc, char *argv[]) {
    if (rte_eal_init(argc, argv) < 0) {
	    rte_exit(EXIT_FAILURE, "Error with EAL init!\n");
    }

    /* DPDK 程序中 创建用于接收/发送数据包的 mbuf 内存池 */
    struct rte_mempool *mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL",
                        NUM_MBUFS, 0, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL) {
        rte_exit(EXIT_FAILURE, "Error creating mbuf pool!\n");
    }

    /* 初始化 DPDK 网卡端口 */
    ng_init_port(mbuf_pool);

    rte_eth_macaddr_get(gDpdkPortId, (struct rte_ether_addr *)gSrcMac);

    while (1) {
        struct rte_mbuf *rx_pkts[BURST_SIZE];
        unsigned nb_rx = rte_eth_rx_burst(gDpdkPortId, 0, rx_pkts, BURST_SIZE);
        if (nb_rx > BURST_SIZE) {
            rte_exit(EXIT_FAILURE, "Received too many packets: %u\n", nb_rx);
        }

        unsigned i = 0;
        for (i = 0; i < nb_rx; i++) {
            // 提取数据包首地址并强制转换成以太网头结构体指针
            struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(rx_pkts[i], struct rte_ether_hdr*);

#if ENABLE_ARP
            if (eth_hdr->ether_type == rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP)) {
                // 处理 ARP 数据包
                struct rte_arp_hdr *arp_hdr = rte_pktmbuf_mtod_offset(rx_pkts[i], struct rte_arp_hdr *,
                    sizeof(struct rte_ether_hdr));

                struct in_addr addr;
                addr.s_addr = arp_hdr->arp_data.arp_tip;
                printf("src: %s ---> arp", inet_ntoa(addr));

                addr.s_addr = gLocalIp;
                printf(" local: %s \n", inet_ntoa(addr));
                if (arp_hdr->arp_data.arp_tip == gLocalIp) {
                    // 如果 ARP 请求的目标 IP 地址是本机 IP，则发送 ARP 响应
                    struct rte_mbuf *arp_buf = ng_send_arp(mbuf_pool, arp_hdr->arp_data.arp_sha.addr_bytes, 
                            arp_hdr->arp_data.arp_tip, arp_hdr->arp_data.arp_sip);
                    rte_eth_tx_burst(gDpdkPortId, 0, &arp_buf, 1);
                    rte_pktmbuf_free(arp_buf);
                    rte_pktmbuf_free(rx_pkts[i]);
                } 
                continue;
            }

#endif
            /* 
             *以太网协议类型 RTE_ETHER_TYPE_IPV4 从本机字节序（CPU 本地序）转换为大端字节序
             *只处理 IPv4 数据包
             */
            if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
                continue;
            }

            struct rte_ipv4_hdr *ipv4_hdr = rte_pktmbuf_mtod_offset(rx_pkts[i], struct rte_ipv4_hdr *,
				sizeof(struct rte_ether_hdr));// 提取 IPv4 报文头部地址，跳过以太网头部
            if (ipv4_hdr->next_proto_id == IPPROTO_UDP) {
                uint8_t ip_hdr_len = (ipv4_hdr->version_ihl & 0x0F) * 4;
                struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)((char *)ipv4_hdr + ip_hdr_len);

#if ENABLE_SEND
                rte_memcpy(gDstMac, eth_hdr->src_addr.addr_bytes, RTE_ETHER_ADDR_LEN);

                rte_memcpy(&gSrcIp, &ipv4_hdr->dst_addr, sizeof(uint32_t));
                rte_memcpy(&gDstIp, &ipv4_hdr->src_addr, sizeof(uint32_t));

                rte_memcpy(&gSrcPort, &udp_hdr->dst_port, sizeof(uint16_t));
                rte_memcpy(&gDstPort, &udp_hdr->src_port, sizeof(uint16_t));
#endif

                uint16_t length = ntohs(udp_hdr->dgram_len);
                *(((char*)udp_hdr) + length) = '\0';

                struct in_addr addr;
                addr.s_addr = ipv4_hdr->src_addr;
                printf("src: %s:%d, ", inet_ntoa(addr), rte_be_to_cpu_16(udp_hdr->src_port));

                addr.s_addr = ipv4_hdr->dst_addr;
                printf("dst: %s:%d, length: %d-->%s\n", inet_ntoa(addr), rte_be_to_cpu_16(udp_hdr->dst_port), length,
                       (char *)udp_hdr + sizeof(struct rte_udp_hdr));
#if ENABLE_SEND
                struct rte_mbuf *txbuf = ng_send_udp(mbuf_pool, (uint8_t *)(udp_hdr + 1), length);
                rte_eth_tx_burst(gDpdkPortId, 0, &txbuf, 1);
                rte_pktmbuf_free(txbuf);
#endif
                rte_pktmbuf_free(rx_pkts[i]);
            }
        }
    }

    return 0;
}
