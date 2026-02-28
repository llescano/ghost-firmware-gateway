/**
 * @file dns_server.c
 * @brief Servidor DNS para captive portal
 *
 * Redirige todas las peticiones DNS a la IP del SoftAP (192.168.4.1)
 * Esto permite que cualquier URL abra el portal de configuración.
 */

#include "dns_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include <string.h>

static const char *TAG = "dns_server";

// DNS header structure
typedef struct {
    uint16_t id;
    uint8_t flags;
    uint8_t rcode;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
} __attribute__((packed)) dns_header_t;

// DNS query structure
typedef struct {
    uint16_t type;
    uint16_t class;
} __attribute__((packed)) dns_query_t;

// DNS response structure
typedef struct {
    uint16_t ptr;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t len;
    uint32_t addr;
} __attribute__((packed)) dns_answer_t;

// DNS reply packet buffer
#define DNS_REPLY_LEN 256

static bool s_dns_running = false;

/**
 * @brief Decodifica nombre DNS (labels con prefijo de longitud)
 */
static int dns_name_decode(const uint8_t *packet, size_t len, size_t pos, char *out, size_t out_max)
{
    size_t out_pos = 0;
    bool first = true;

    while (pos < len) {
        uint8_t label_len = packet[pos];

        if (label_len == 0) {
            out[out_pos] = '\0';
            return pos + 1;
        }

        // Compressed pointer
        if ((label_len & 0xC0) == 0xC0) {
            if (pos + 1 >= len) return -1;
            uint16_t offset = ((label_len & 0x3F) << 8) | packet[pos + 1];
            return dns_name_decode(packet, len, offset, out, out_max);
        }

        pos++;
        if (pos + label_len > len) return -1;

        if (!first) {
            if (out_pos < out_max - 1) out[out_pos++] = '.';
        }
        first = false;

        for (int i = 0; i < label_len && out_pos < out_max - 1; i++) {
            out[out_pos++] = packet[pos++];
        }
    }

    out[out_pos] = '\0';
    return pos;
}

/**
 * @brief Construye respuesta DNS
 */
static int dns_reply_build(const uint8_t *req, size_t req_len, uint8_t *reply, size_t reply_max)
{
    if (req_len < sizeof(dns_header_t)) {
        return -1;
    }

    const dns_header_t *req_hdr = (const dns_header_t *)req;

    // Solo responder queries con una pregunta
    if (req_hdr->qdcount != 1 || req_hdr->ancount != 0 || req_hdr->nscount != 0) {
        return -1;
    }

    // Parsear query
    size_t pos = sizeof(dns_header_t);
    char name[128];
    pos = dns_name_decode(req, req_len, pos, name, sizeof(name));
    if (pos < 0 || pos + sizeof(dns_query_t) > req_len) {
        return -1;
    }

    const dns_query_t *query = (const dns_query_t *)(req + pos);

    // Solo responder a type A (IPv4) y class IN
    if (ntohs(query->type) != 1 || ntohs(query->class) != 1) {
        return -1;
    }

    // Construir respuesta
    dns_header_t *rep_hdr = (dns_header_t *)reply;
    memset(rep_hdr, 0, sizeof(*rep_hdr));
    rep_hdr->id = req_hdr->id;
    rep_hdr->flags = 0x80;  // Response
    rep_hdr->qdcount = req_hdr->qdcount;
    rep_hdr->ancount = htons(1);

    size_t rep_pos = sizeof(*rep_hdr);

    // Copiar question
    size_t qname_len = pos - sizeof(*req_hdr);
    if (rep_pos + qname_len + sizeof(*query) > reply_max) {
        return -1;
    }
    memcpy(reply + rep_pos, req + sizeof(*req_hdr), qname_len + sizeof(*query));
    rep_pos += qname_len + sizeof(*query);

    // Agregar answer - siempre devuelve 192.168.4.1
    dns_answer_t *answer = (dns_answer_t *)(reply + rep_pos);

    // Nombre comprimido (pointer al qname)
    answer->ptr = htons(0xC000 | sizeof(*rep_hdr));
    answer->type = htons(1);
    answer->class = htons(1);
    answer->ttl = htonl(300);  // 5 minutos
    answer->len = htons(4);
    answer->addr = htonl(0xC0A80401);  // 192.168.4.1

    rep_pos += sizeof(*answer);

    return rep_pos;
}

/**
 * @brief Handler UDP del servidor DNS
 */
static void dns_server_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, uint16_t port)
{
    if (p == NULL) return;

    uint8_t reply[DNS_REPLY_LEN];
    int reply_len = dns_reply_build(p->pbuf.payload, p->pbuf.len, reply, sizeof(reply));

    if (reply_len > 0) {
        struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, reply_len, PBUF_RAM);
        if (r) {
            memcpy(r->pbuf.payload, reply, reply_len);
            pbuf_sendto(pcb, r, addr, port);
            pbuf_free(r);

            // Log del nombre resuelto (primer request)
            static bool logged = false;
            if (!logged) {
                const dns_header_t *hdr = (const dns_header_t *)p->pbuf.payload;
                size_t pos = sizeof(*hdr);
                char name[128];
                dns_name_decode(p->pbuf.payload, p->pbuf.len, pos, name, sizeof(name));
                ESP_LOGI(TAG, "DNS query redirigido: %s → 192.168.4.1", name);
                logged = true;
            }
        }
    }

    pbuf_free(p);
}

// ============================================================================
// Funciones públicas
// ============================================================================

esp_err_t dns_server_start(void)
{
    if (s_dns_running) {
        return ESP_OK;
    }

    struct udp_pcb *pcb = udp_new();
    if (pcb == NULL) {
        ESP_LOGE(TAG, "Error creando PCB UDP");
        return ESP_ERR_NO_MEM;
    }

    err_t err = udp_bind(pcb, IP_ADDR_ANY, 53);
    if (err != ERR_OK) {
        ESP_LOGE(TAG, "Error haciendo bind en puerto 53");
        udp_remove(pcb);
        return ESP_FAIL;
    }

    udp_recv(pcb, dns_server_recv, NULL);

    s_dns_running = true;
    ESP_LOGI(TAG, "✅ DNS server iniciado (puerto 53)");

    return ESP_OK;
}

esp_err_t dns_server_stop(void)
{
    if (!s_dns_running) {
        return ESP_OK;
    }

    // UDP PCB se libera automáticamente al detener WiFi
    s_dns_running = false;
    ESP_LOGI(TAG, "DNS server detenido");

    return ESP_OK;
}
