#include "net.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "stats.h"

static const char *TAG = "net";

static esp_eth_handle_t s_eth;
static esp_netif_t *s_netif;
static volatile bool s_has_ip;

bool net_has_ip(void) { return s_has_ip; }

static void on_eth(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t speed = ETH_SPEED_10M;
        eth_duplex_t duplex = ETH_DUPLEX_HALF;
        esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
        stats_lock();
        g_stats.eth_link = true;
        g_stats.eth_speed = speed == ETH_SPEED_100M ? 100 : 10;
        g_stats.eth_full_duplex = duplex == ETH_DUPLEX_FULL;
        stats_unlock();
        ESP_LOGI(TAG, "Link up, %d Mbps %s duplex", speed == ETH_SPEED_100M ? 100 : 10,
                 duplex == ETH_DUPLEX_FULL ? "full" : "half");
        stats_event(EV_INFO, "Ethernet link up (%d Mbps %s duplex)", speed == ETH_SPEED_100M ? 100 : 10,
                    duplex == ETH_DUPLEX_FULL ? "full" : "half");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_has_ip = false;
        stats_lock();
        g_stats.eth_link = false;
        g_stats.eth_has_ip = false;
        stats_unlock();
        ESP_LOGW(TAG, "Link down");
        stats_event(EV_ERROR, "Ethernet link down");
    }
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)base;
    (void)id;
    const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
    s_has_ip = true;
    stats_lock();
    g_stats.eth_has_ip = true;
    g_stats.ip = e->ip_info.ip.addr;
    g_stats.mask = e->ip_info.netmask.addr;
    g_stats.gw = e->ip_info.gw.addr;
    stats_unlock();
    ESP_LOGI(TAG, "IP " IPSTR " mask " IPSTR " gw " IPSTR, IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.netmask),
             IP2STR(&e->ip_info.gw));
    stats_event(EV_INFO, "Got IP " IPSTR, IP2STR(&e->ip_info.ip));
}

void net_start(const bridge_settings_t *cfg) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t ncfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&ncfg);
    esp_netif_set_hostname(s_netif, cfg->hostname);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    // The P4 defaults are this board's wiring: MDC 31, MDIO 52, RMII clock in on GPIO50,
    // TX_EN 49, TXD 34/35, CRS_DV 28, RXD 29/30.
    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = 51;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_config, &s_eth));
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)));

    uint8_t mac_addr[6];
    esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac_addr);
    stats_lock();
    memcpy(g_stats.mac, mac_addr, 6);
    stats_unlock();

    if (cfg->use_static) {
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_ip_info_t ip = {.ip.addr = cfg->ip, .netmask.addr = cfg->mask, .gw.addr = cfg->gw};
        ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip));
        if (cfg->dns) {
            esp_netif_dns_info_t dns = {0};
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = cfg->dns;
            esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns);
        }
        ESP_LOGI(TAG, "Static IP " IPSTR, IP2STR(&ip.ip));
    } else {
        ESP_LOGI(TAG, "DHCP");
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_ip, NULL));
#if CONFIG_BRIDGE_TEST_BREAK == 2
    ESP_LOGE(TAG, "TEST BUILD: no network on purpose (Ethernet not started)");
#else
    ESP_ERROR_CHECK(esp_eth_start(s_eth));
#endif

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(cfg->hostname);
        mdns_instance_name_set("Zigbee Ethernet Bridge");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        // Discovery for Zigbee Studio and other tools: API on port 80, Zigbee serial on tcp_port.
        char zport[8];
        snprintf(zport, sizeof(zport), "%u", cfg->tcp_port);
        mdns_txt_item_t txt[] = {
            {"api", "zbbridge/1"},
            {"zigbee_port", zport},
            {"version", esp_app_get_description()->version},
        };
        mdns_service_add("Zigbee Ethernet Bridge", "_zbbridge", "_tcp", 80, txt, 3);
    }

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    esp_netif_sntp_init(&sntp);
}
