#include "modbus_tcp_task.h"

#include <string.h>
#include "energy_meter_task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#define TAG "mb_tcp"

/*
 * Address base. The decision prints its signal table with 3xxxx / 4xxxx entity
 * prefixes and numbers the first input register "1", i.e. the classic 1-based
 * data-model numbering, so the wire (PDU) address is the printed number minus
 * one. Some DSO gateways publish the raw PDU address instead, hence the knob.
 */
#define MB_TCP_ADDR(doc) ((uint16_t)((doc) - CONFIG_APP_MB_TCP_ADDR_BASE))

/* Input registers (FC04) - every value is a big-endian-word float32 over two
 * registers, which is what the uniform step of 2 in the table implies. */
#define IR_P_OUT MB_TCP_ADDR(1)   /* active power exported to the grid, kW */
#define IR_Q_OUT MB_TCP_ADDR(3)   /* reactive power exported to the grid, kVAr */
#define IR_UA    MB_TCP_ADDR(5)
#define IR_UB    MB_TCP_ADDR(7)
#define IR_UC    MB_TCP_ADDR(9)
#define IR_IA    MB_TCP_ADDR(11)
#define IR_IB    MB_TCP_ADDR(13)
#define IR_IC    MB_TCP_ADDR(15)
#define IR_FREQ  MB_TCP_ADDR(17)
/* Power factor sits at 1109, far above the rest of the table. That is what the
 * decision prints and what the DSO gateway will poll, so it is taken literally:
 * the input image is sized to reach it and the gap in between reads back as
 * zero, exactly as a sparse map on any other device would. */
#define IR_PF    MB_TCP_ADDR(1109)
#define MB_TCP_IR_COUNT (IR_PF + 2)

/* Coils (FC01/FC05) - control-enable flags. */
#define CO_P_ENABLE MB_TCP_ADDR(11)
#define CO_Q_ENABLE MB_TCP_ADDR(15)
#define MB_TCP_COIL_COUNT (CO_Q_ENABLE + 1)

/*
 * Holding registers (FC03/FC06) - power setpoints as a percentage.
 *
 * Stored and served as the raw 16-bit value the DSO wrote, with no scaling and
 * no clamping: the decision names Write Single Register (FC06) as the write
 * function but never states the percent scaling (whole percent or tenths), so
 * reinterpreting the value here could only lose information. Nothing in this
 * firmware acts on it yet.
 */
#define HR_P_SETPOINT MB_TCP_ADDR(13)
#define HR_Q_SETPOINT MB_TCP_ADDR(17)
#define MB_TCP_HR_COUNT (HR_Q_SETPOINT + 1)

/* Discrete inputs (FC02) - the function is part of the required set but the
 * decision defines no discrete signal, so the area reads back as reserved. */
#define MB_TCP_DI_COUNT 16

#define MB_TCP_MAX_CLIENTS 4
#define MB_TCP_FRAME_MAX 260      /* MBAP 7 + max PDU 253 */
#define MB_TCP_SELECT_MS 500
#define MB_TCP_IDLE_TOUT_MS 120000

#define MB_EX_ILLEGAL_FUNCTION 0x01
#define MB_EX_ILLEGAL_ADDRESS  0x02
#define MB_EX_ILLEGAL_VALUE    0x03

#define MB_TCP_NVS_NS "mbtcp"

static uint16_t s_input_regs[MB_TCP_IR_COUNT];
static uint16_t s_holding_regs[MB_TCP_HR_COUNT];
static uint8_t s_coils[MB_TCP_COIL_COUNT];
static uint8_t s_discrete[MB_TCP_DI_COUNT];

/* The server task owns every image above, so serving needs no lock. Only the
 * control snapshot escapes to other tasks. */
static portMUX_TYPE s_ctrl_mux = portMUX_INITIALIZER_UNLOCKED;
static modbus_tcp_control_t s_control;

typedef struct {
    int fd;
    size_t len;
    uint32_t last_rx_ms;
    uint8_t buf[MB_TCP_FRAME_MAX];
} mb_tcp_client_t;

static mb_tcp_client_t s_clients[MB_TCP_MAX_CLIENTS];

static uint32_t mb_tcp_now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

/* Big-endian word order (ABCD), matching store_float() in the RTU slave. */
static void store_float(uint16_t offset, float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    s_input_regs[offset] = (uint16_t)(bits >> 16);
    s_input_regs[offset + 1] = (uint16_t)bits;
}

static void mb_tcp_publish_control(void)
{
    modbus_tcp_control_t c = {
        .p_control_enabled = s_coils[CO_P_ENABLE] != 0,
        .q_control_enabled = s_coils[CO_Q_ENABLE] != 0,
        .p_setpoint_raw = s_holding_regs[HR_P_SETPOINT],
        .q_setpoint_raw = s_holding_regs[HR_Q_SETPOINT],
    };
    portENTER_CRITICAL(&s_ctrl_mux);
    s_control = c;
    portEXIT_CRITICAL(&s_ctrl_mux);
}

/* The spec requires the plant to keep running on the last setpoint when the
 * link drops, so the command has to outlive both the connection and a reboot. */
static void mb_tcp_control_load(void)
{
    nvs_handle_t h;
    if (nvs_open(MB_TCP_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        mb_tcp_publish_control();
        return;
    }
    uint16_t u16;
    uint8_t u8;
    if (nvs_get_u16(h, "p_sp", &u16) == ESP_OK) s_holding_regs[HR_P_SETPOINT] = u16;
    if (nvs_get_u16(h, "q_sp", &u16) == ESP_OK) s_holding_regs[HR_Q_SETPOINT] = u16;
    if (nvs_get_u8(h, "p_en", &u8) == ESP_OK) s_coils[CO_P_ENABLE] = u8 ? 1U : 0U;
    if (nvs_get_u8(h, "q_en", &u8) == ESP_OK) s_coils[CO_Q_ENABLE] = u8 ? 1U : 0U;
    nvs_close(h);
    mb_tcp_publish_control();
    ESP_LOGI(TAG, "control restored: P en=%u sp=%u, Q en=%u sp=%u",
             s_coils[CO_P_ENABLE], s_holding_regs[HR_P_SETPOINT],
             s_coils[CO_Q_ENABLE], s_holding_regs[HR_Q_SETPOINT]);
}

static void mb_tcp_control_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MB_TCP_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "control save failed to open NVS (%s)", esp_err_to_name(err));
        return;
    }
    nvs_set_u16(h, "p_sp", s_holding_regs[HR_P_SETPOINT]);
    nvs_set_u16(h, "q_sp", s_holding_regs[HR_Q_SETPOINT]);
    nvs_set_u8(h, "p_en", s_coils[CO_P_ENABLE]);
    nvs_set_u8(h, "q_en", s_coils[CO_Q_ENABLE]);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "control commit failed (%s)", esp_err_to_name(err));
    }
}

static void mb_tcp_refresh_inputs(void)
{
    atm90e32as_measurements_t m;
    if (energy_meter_get_latest(&m) != ESP_OK) {
        return;
    }
    /* Firmware sign convention is import-positive, the spec asks for the power
     * exported to the grid, so the sign flips. Active export is one-directional
     * by definition here and floors at zero while the site is consuming;
     * reactive export stays signed because leading and lagging are both real. */
    float p_out_kw = -m.total_active_power / 1000.0f;
    if (!(p_out_kw > 0.0f)) {
        p_out_kw = 0.0f;
    }
    store_float(IR_P_OUT, p_out_kw);
    store_float(IR_Q_OUT, -m.total_reactive_power / 1000.0f);
    store_float(IR_UA, m.voltage[0]);
    store_float(IR_UB, m.voltage[1]);
    store_float(IR_UC, m.voltage[2]);
    store_float(IR_IA, m.current[0]);
    store_float(IR_IB, m.current[1]);
    store_float(IR_IC, m.current[2]);
    store_float(IR_FREQ, m.frequency);
    store_float(IR_PF, m.total_power_factor);
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static int mb_tcp_read_bits(const uint8_t *image, uint16_t count, const uint8_t *req,
                            uint8_t *rsp, uint8_t *exception)
{
    uint16_t start = rd16(req + 1);
    uint16_t qty = rd16(req + 3);
    if (qty < 1U || qty > 2000U) {
        *exception = MB_EX_ILLEGAL_VALUE;
        return -1;
    }
    if ((uint32_t)start + qty > count) {
        *exception = MB_EX_ILLEGAL_ADDRESS;
        return -1;
    }
    uint8_t nbytes = (uint8_t)((qty + 7U) / 8U);
    rsp[0] = req[0];
    rsp[1] = nbytes;
    memset(rsp + 2, 0, nbytes);
    for (uint16_t i = 0; i < qty; i++) {
        if (image[start + i]) {
            rsp[2 + i / 8U] |= (uint8_t)(1U << (i % 8U));
        }
    }
    return 2 + nbytes;
}

static int mb_tcp_read_regs(const uint16_t *image, uint16_t count, const uint8_t *req,
                            uint8_t *rsp, uint8_t *exception)
{
    uint16_t start = rd16(req + 1);
    uint16_t qty = rd16(req + 3);
    if (qty < 1U || qty > 125U) {
        *exception = MB_EX_ILLEGAL_VALUE;
        return -1;
    }
    if ((uint32_t)start + qty > count) {
        *exception = MB_EX_ILLEGAL_ADDRESS;
        return -1;
    }
    rsp[0] = req[0];
    rsp[1] = (uint8_t)(qty * 2U);
    for (uint16_t i = 0; i < qty; i++) {
        wr16(rsp + 2 + i * 2, image[start + i]);
    }
    return 2 + qty * 2;
}

/* Returns the response PDU length, or -1 with *exception set. */
static int mb_tcp_handle_pdu(const uint8_t *req, size_t req_len, uint8_t *rsp,
                             uint8_t *exception)
{
    if (req_len < 1) {
        *exception = MB_EX_ILLEGAL_FUNCTION;
        return -1;
    }
    switch (req[0]) {
    case 0x01:  /* Read Coils */
    case 0x02:  /* Read Discrete Inputs */
    case 0x03:  /* Read Holding Registers */
    case 0x04:  /* Read Input Registers */
    case 0x05:  /* Write Single Coil */
    case 0x06:  /* Write Single Register */
        if (req_len < 5) {
            *exception = MB_EX_ILLEGAL_VALUE;
            return -1;
        }
        break;
    default:
        *exception = MB_EX_ILLEGAL_FUNCTION;
        return -1;
    }

    switch (req[0]) {
    case 0x01:
        return mb_tcp_read_bits(s_coils, MB_TCP_COIL_COUNT, req, rsp, exception);
    case 0x02:
        return mb_tcp_read_bits(s_discrete, MB_TCP_DI_COUNT, req, rsp, exception);
    case 0x03:
        return mb_tcp_read_regs(s_holding_regs, MB_TCP_HR_COUNT, req, rsp, exception);
    case 0x04:
        return mb_tcp_read_regs(s_input_regs, MB_TCP_IR_COUNT, req, rsp, exception);
    case 0x05: {
        uint16_t addr = rd16(req + 1);
        uint16_t value = rd16(req + 3);
        if (value != 0x0000U && value != 0xFF00U) {
            *exception = MB_EX_ILLEGAL_VALUE;
            return -1;
        }
        /* Only the two control-enable coils are writable; the rest of the area
         * exists so a full-range FC01 scan does not fault. */
        if (addr != CO_P_ENABLE && addr != CO_Q_ENABLE) {
            *exception = MB_EX_ILLEGAL_ADDRESS;
            return -1;
        }
        uint8_t next = value ? 1U : 0U;
        if (s_coils[addr] != next) {
            s_coils[addr] = next;
            mb_tcp_publish_control();
            mb_tcp_control_save();
            ESP_LOGI(TAG, "%s control %s by DSO",
                     addr == CO_P_ENABLE ? "P" : "Q", next ? "enabled" : "disabled");
        }
        memcpy(rsp, req, 5);
        return 5;
    }
    case 0x06: {
        uint16_t addr = rd16(req + 1);
        uint16_t value = rd16(req + 3);
        if (addr != HR_P_SETPOINT && addr != HR_Q_SETPOINT) {
            *exception = MB_EX_ILLEGAL_ADDRESS;
            return -1;
        }
        if (s_holding_regs[addr] != value) {
            s_holding_regs[addr] = value;
            mb_tcp_publish_control();
            mb_tcp_control_save();
            ESP_LOGI(TAG, "%s setpoint = %u (raw) from DSO",
                     addr == HR_P_SETPOINT ? "P" : "Q", value);
        }
        memcpy(rsp, req, 5);
        return 5;
    }
    default:
        *exception = MB_EX_ILLEGAL_FUNCTION;
        return -1;
    }
}

static void mb_tcp_client_close(mb_tcp_client_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
    }
    c->len = 0;
}

/* Consume every complete MBAP frame sitting in the client buffer. Returns false
 * when the stream is unusable and the socket has to go. */
static bool mb_tcp_drain_client(mb_tcp_client_t *c)
{
    while (c->len >= 6) {
        uint16_t proto = rd16(c->buf + 2);
        uint16_t length = rd16(c->buf + 4);
        if (proto != 0U || length < 2U || length > (MB_TCP_FRAME_MAX - 6)) {
            ESP_LOGW(TAG, "malformed MBAP (proto=%u len=%u); dropping client", proto, length);
            return false;
        }
        size_t frame = (size_t)6 + length;
        if (c->len < frame) {
            break;  /* wait for the rest */
        }
        uint8_t uid = c->buf[6];
        const uint8_t *pdu = c->buf + 7;
        size_t pdu_len = length - 1U;

        /* Unit ID: the gateway normally addresses the configured ID, but 0 and
         * 0xFF are the conventional "the TCP endpoint itself" values. */
        if (uid == CONFIG_APP_MB_TCP_UNIT_ID || uid == 0x00U || uid == 0xFFU) {
            uint8_t rsp[MB_TCP_FRAME_MAX];
            uint8_t exception = 0;
            int rsp_len = mb_tcp_handle_pdu(pdu, pdu_len, rsp + 7, &exception);
            if (rsp_len < 0) {
                rsp[7] = (uint8_t)(pdu[0] | 0x80U);
                rsp[8] = exception;
                rsp_len = 2;
            }
            memcpy(rsp, c->buf, 4);                 /* transaction + protocol id */
            wr16(rsp + 4, (uint16_t)(rsp_len + 1));
            rsp[6] = uid;
            int total = 7 + rsp_len;
            if (send(c->fd, rsp, total, 0) != total) {
                ESP_LOGW(TAG, "send failed (errno %d); dropping client", errno);
                return false;
            }
        }
        memmove(c->buf, c->buf + frame, c->len - frame);
        c->len -= frame;
    }
    return true;
}

static int mb_tcp_listen(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        ESP_LOGE(TAG, "socket() failed (errno %d)", errno);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_APP_MB_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(:%d) failed (errno %d)", CONFIG_APP_MB_TCP_PORT, errno);
        close(fd);
        return -1;
    }
    if (listen(fd, MB_TCP_MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "listen() failed (errno %d)", errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "listening on :%d (unit id 0x%02X, address base %d)",
             CONFIG_APP_MB_TCP_PORT, CONFIG_APP_MB_TCP_UNIT_ID,
             CONFIG_APP_MB_TCP_ADDR_BASE);
    return fd;
}

static void mb_tcp_accept(int listen_fd)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        return;
    }
    for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0) {
            s_clients[i].fd = fd;
            s_clients[i].len = 0;
            s_clients[i].last_rx_ms = mb_tcp_now_ms();
            ESP_LOGI(TAG, "client %d connected from %s", i, inet_ntoa(peer.sin_addr));
            return;
        }
    }
    ESP_LOGW(TAG, "connection table full; rejecting %s", inet_ntoa(peer.sin_addr));
    close(fd);
}

static void modbus_tcp_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
        s_clients[i].fd = -1;
    }
    mb_tcp_control_load();

    int listen_fd = -1;
    while (true) {
        if (listen_fd < 0) {
            listen_fd = mb_tcp_listen();
            if (listen_fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(5000));
                continue;
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;
        for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
            if (s_clients[i].fd >= 0) {
                FD_SET(s_clients[i].fd, &rfds);
                if (s_clients[i].fd > maxfd) {
                    maxfd = s_clients[i].fd;
                }
            }
        }
        struct timeval tv = {
            .tv_sec = MB_TCP_SELECT_MS / 1000,
            .tv_usec = (MB_TCP_SELECT_MS % 1000) * 1000,
        };
        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);

        /* Refresh on every pass, not only on traffic, so a poll always reads an
         * image at most one select period old. */
        mb_tcp_refresh_inputs();

        if (ready < 0) {
            ESP_LOGE(TAG, "select() failed (errno %d); restarting listener", errno);
            for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
                mb_tcp_client_close(&s_clients[i]);
            }
            close(listen_fd);
            listen_fd = -1;
            continue;
        }

        uint32_t now = mb_tcp_now_ms();
        for (int i = 0; i < MB_TCP_MAX_CLIENTS; i++) {
            mb_tcp_client_t *c = &s_clients[i];
            if (c->fd < 0) {
                continue;
            }
            if (FD_ISSET(c->fd, &rfds)) {
                int n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - c->len, 0);
                if (n <= 0) {
                    ESP_LOGI(TAG, "client %d disconnected", i);
                    mb_tcp_client_close(c);
                    continue;
                }
                c->len += (size_t)n;
                c->last_rx_ms = now;
                if (!mb_tcp_drain_client(c)) {
                    mb_tcp_client_close(c);
                    continue;
                }
                if (c->len >= sizeof(c->buf)) {
                    /* A full buffer with no complete frame in it means the peer
                     * is not speaking Modbus TCP. */
                    ESP_LOGW(TAG, "client %d buffer stalled; dropping", i);
                    mb_tcp_client_close(c);
                }
            } else if ((now - c->last_rx_ms) > MB_TCP_IDLE_TOUT_MS) {
                ESP_LOGI(TAG, "client %d idle timeout", i);
                mb_tcp_client_close(c);
            }
        }

        if (ready > 0 && FD_ISSET(listen_fd, &rfds)) {
            mb_tcp_accept(listen_fd);
        }
    }
}

esp_err_t modbus_tcp_task_start(void)
{
    static bool started;
    if (started) {
        return ESP_OK;
    }
    if (xTaskCreate(modbus_tcp_task, "modbus_tcp", CONFIG_APP_MB_TCP_TASK_STACK_SIZE,
                    NULL, CONFIG_APP_MB_TCP_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create Modbus TCP task");
        return ESP_ERR_NO_MEM;
    }
    started = true;
    return ESP_OK;
}

esp_err_t modbus_tcp_get_control(modbus_tcp_control_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_ctrl_mux);
    *out = s_control;
    portEXIT_CRITICAL(&s_ctrl_mux);
    return ESP_OK;
}
