#ifndef __NG_ARP_H__
#define __NG_ARP_H__

#include <rte_ether.h>

#define ARP_ENTRY_STATUS_DYNAMIC	0
#define ARP_ENTRY_STATUS_STATIC		1

#define LL_ADD(item, list) do {		\
	item->prev = NULL;				\
	item->next = list;				\
	if (list != NULL) list->prev = item; \
	list = item;					\
} while(0)

#define LL_REMOVE(item, list) do {		\
	if (item->prev != NULL) item->prev->next = item->next;	\
	if (item->next != NULL) item->next->prev = item->prev;	\
	if (list == item) list = item->next;	\
	item->prev = item->next = NULL;			\
} while(0)

struct arp_entry {
	uint32_t ip;
	uint8_t hwaddr[RTE_ETHER_ADDR_LEN];
	uint8_t type;

	struct arp_entry *next;
	struct arp_entry *prev;
};

struct arp_table {
	struct arp_entry *entries;
	int count;
};

static struct  arp_table *arpt = NULL;
static struct  arp_table *arp_table_instance(void) {

	if (arpt == NULL) {

		arpt = rte_malloc("arp table", sizeof(struct  arp_table), 0);
		if (arpt == NULL) {
			rte_exit(EXIT_FAILURE, "rte_malloc arp table failed\n");
		}
		memset(arpt, 0, sizeof(struct  arp_table));
	}

	return arpt;
}

static uint8_t* ng_get_dst_macaddr(uint32_t dip) {

	struct arp_entry *iter;
	struct arp_table *table = arp_table_instance();

	for (iter = table->entries;iter != NULL;iter = iter->next) {
		if (dip == iter->ip) {
			return iter->hwaddr;
		}
	}

	return NULL;
}

#endif
