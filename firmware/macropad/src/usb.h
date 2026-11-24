#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct __attribute__((packed)) {
	uint8_t modifiers;
	uint8_t key_bitmap[13];
} usb_keyboard_report_t;

typedef struct __attribute__((packed)) {
	uint8_t consumer_code;
} usb_consumer_report_t;

void usb_init(void);
void usb_run(void);
void usb_send_key(const uint8_t* payload);
void usb_send_cntrl(const uint8_t* payload);
