#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_mac.h"
#include "esp_log.h"

#define LINK_UART UART_NUM_0
#define PACKET_MAX 1029
#define KIND_HCI 1
#define KIND_START 0x10
#define KIND_STOP 0x11
#define KIND_READY 0x12
#define KIND_CLOSED 0x13
#define KIND_PING 0x14
#define KIND_FATAL 0x7f

typedef struct {
    uint16_t length;
    uint8_t kind;
    uint8_t data[PACKET_MAX];
} frame_t;
static QueueHandle_t output_queue, uart_events;
static uint32_t first_error;
static bool controller_enabled, controller_initialized;
static uint32_t callbacks_active;
static TickType_t last_host_frame;

static void fail(uint32_t code)
{
    uint32_t expected = 0;
    __atomic_compare_exchange_n(&first_error, &expected, code, false,
                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
static uint32_t error(void)
{
    return __atomic_load_n(&first_error, __ATOMIC_SEQ_CST);
}
static bool emit(uint8_t kind, const uint8_t *data, size_t size)
{
    if (size > PACKET_MAX) { fail(1); return false; }
    frame_t frame = { .kind = kind, .length = size };
    if (size) memcpy(frame.data, data, size);
    if (xQueueSend(output_queue, &frame, 0) != pdTRUE) {
        fail(2); return false;
    }
    return true;
}
static bool write_frame(const frame_t *frame)
{
    uint8_t header[] = { 0xa5, frame->kind, frame->length & 255,
                         frame->length >> 8 };
    if (uart_write_bytes(LINK_UART, header, sizeof(header)) != sizeof(header)) return false;
    if (frame->length && uart_write_bytes(LINK_UART, frame->data, frame->length) != frame->length) return false;
    return uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(2000)) == ESP_OK;
}
static void output_task(void *unused)
{
    frame_t frame;
    for (;;) {
        if (xQueueReceive(output_queue, &frame, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (!write_frame(&frame)) fail(3);
        }
        uint32_t code = error();
        if (code) {
            frame.kind = KIND_FATAL; frame.length = 4;
            for (int i = 0; i < 4; i++) frame.data[i] = code >> (i * 8);
            write_frame(&frame);
            vTaskDelete(NULL);
        }
    }
}
static void uart_event_task(void *unused)
{
    uart_event_t event;
    for (;;) {
        if (xQueueReceive(uart_events, &event, portMAX_DELAY) != pdTRUE) continue;
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR ||
            event.type == UART_BREAK) fail(4 + event.type);
    }
}
static void controller_can_receive(void) { }
static int controller_packet(uint8_t *data, uint16_t size)
{
    __atomic_add_fetch(&callbacks_active, 1, __ATOMIC_SEQ_CST);
    if (!error()) emit(KIND_HCI, data, size);
    __atomic_sub_fetch(&callbacks_active, 1, __ATOMIC_SEQ_CST);
    return 0;
}
static const esp_vhci_host_callback_t callbacks = {
    .notify_host_send_available = controller_can_receive,
    .notify_host_recv = controller_packet,
};
static bool read_exact(uint8_t *data, size_t length)
{
    size_t offset = 0;
    while (offset < length && !error()) {
        if (controller_enabled && xTaskGetTickCount() - last_host_frame > pdMS_TO_TICKS(10000)) {
            fail(40); return false;
        }
        int count = uart_read_bytes(LINK_UART, data + offset, length - offset,
                                    pdMS_TO_TICKS(50));
        if (count < 0) { fail(20); return false; }
        offset += count;
    }
    return offset == length && !error();
}
static bool read_frame(frame_t *frame)
{
    uint8_t header[4];
    if (!read_exact(header, sizeof(header))) return false;
    frame->kind = header[1];
    frame->length = header[2] | ((uint16_t)header[3] << 8);
    if (header[0] != 0xa5 || frame->length > PACKET_MAX) { fail(21); return false; }
    return read_exact(frame->data, frame->length);
}
static bool hci_valid(const frame_t *frame)
{
    const uint8_t *p = frame->data;
    if (frame->length >= 4 && p[0] == 1) return frame->length == 4 + p[3];
    if (frame->length >= 5 && p[0] == 2)
        return frame->length == 5 + p[3] + ((uint16_t)p[4] << 8);
    return false;
}
static void controller_stop(void)
{
    if (controller_enabled) {
        if (esp_bt_controller_disable() != ESP_OK) { fail(30); return; }
        controller_enabled = false;
    }
    if (controller_initialized) {
        if (esp_bt_controller_deinit() != ESP_OK) { fail(41); return; }
        controller_initialized = false;
        if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE ||
            __atomic_load_n(&callbacks_active, __ATOMIC_SEQ_CST) != 0) fail(42);
    }
}
void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    output_queue = xQueueCreate(16, sizeof(frame_t));
    if (!output_queue) return;
    uart_config_t config = {
        .baud_rate = 115200, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_param_config(LINK_UART, &config) != ESP_OK ||
        uart_set_pin(LINK_UART, 1, 3, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(LINK_UART, 8192, 4096, 32, &uart_events, 0) != ESP_OK) return;
    if (xTaskCreate(output_task, "hci_serial_tx", 4096, NULL, 8, NULL) != pdPASS ||
        xTaskCreate(uart_event_task, "hci_uart_events", 2048, NULL, 9, NULL) != pdPASS) {
        fail(31); return;
    }
    const char banner[] = "AZKI_VHCI_SERIAL_V1 READY\n";
    uart_write_bytes(LINK_UART, banner, sizeof(banner) - 1);
    uart_wait_tx_done(LINK_UART, pdMS_TO_TICKS(1000));
    frame_t input;
    if (!read_frame(&input)) goto stopped;
    if (input.kind != KIND_START || input.length != 0) { fail(32); goto stopped; }
    esp_bt_controller_config_t bt = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt) != ESP_OK) { fail(34); goto stopped; }
    controller_initialized = true;
    if (esp_bt_controller_enable(ESP_BT_MODE_BTDM) != ESP_OK) { fail(35); goto stopped; }
    controller_enabled = true;
    last_host_frame = xTaskGetTickCount();
    if (esp_vhci_host_register_callback(&callbacks) != ESP_OK) { fail(36); goto stopped; }
    uint8_t address[6];
    if (esp_read_mac(address, ESP_MAC_BT) != ESP_OK) { fail(37); goto stopped; }
    if (!emit(KIND_READY, address, sizeof(address))) goto stopped;
    while (!error() && read_frame(&input)) {
        last_host_frame = xTaskGetTickCount();
        if (input.kind == KIND_PING && input.length == 0) continue;
        if (input.kind == KIND_STOP && input.length == 0) {
            controller_stop();
            if (!error()) emit(KIND_CLOSED, NULL, 0);
            goto halted;
        }
        if (input.kind != KIND_HCI || !hci_valid(&input)) { fail(38); break; }
        TickType_t start = xTaskGetTickCount();
        while (!error() && !esp_vhci_host_check_send_available()) {
            if (xTaskGetTickCount() - start > pdMS_TO_TICKS(2000)) { fail(39); break; }
            vTaskDelay(1);
        }
        if (!error()) esp_vhci_host_send_packet(input.data, input.length);
    }
stopped:
    controller_stop();
halted:
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
