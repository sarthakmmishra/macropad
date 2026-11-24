#pragma once

#include <stdint.h>

#define MAGIC 0x87AD91B2

#ifndef FLASH_BASE
#define FLASH_BASE 0x08000000
#endif
#define MAPPINGS_ADDR 0x08007800

#define MAX_LAYER_NAME 10
#define MAX_BINDING_NAME 10
#define NUM_KEYS 9
#define NUM_ENCODERS 2

#define PAYLOAD_SIZE 14

typedef enum: uint8_t {
	USB_TYPE_KEY, USB_TYPE_CNTRL
} usb_type_t;

typedef enum: uint8_t {
	ACTION_TYPE_NONE,
	ACTION_TYPE_SINGLE_KEY,
	ACTION_TYPE_CONSUMER_CNTRL,
	ACTION_TYPE_SEQUENCE,
	ACTION_TYPE_DELAY,
	ACTION_TYPE_RELEASE
} action_type_t;

typedef struct __attribute__((packed)) {
	action_type_t type;
	union {
		struct __attribute__((packed)) {
			uint8_t modifiers;
			uint8_t key_index;
		} key;

		uint8_t consumer_code;
		uint16_t delay_ms;

		struct __attribute__((packed)) {
			uint16_t sequence_offset;
			uint8_t length;
		} sequence;
	};
} action_t;

typedef struct __attribute__((packed)) {
	uint16_t press_idx;
	uint16_t ccw_idx;
	uint16_t cw_idx;
} encoder_action_t;

typedef struct __attribute__((packed)) {
	char layer_name[MAX_LAYER_NAME];
	char binding_names[NUM_KEYS + 2 * (NUM_ENCODERS - 1)][MAX_BINDING_NAME];
	uint16_t action_indices[NUM_KEYS];
	encoder_action_t enc_actions[NUM_ENCODERS - 1];
} macro_layer_t;

typedef struct __attribute__((packed)) {
	uint32_t magic_number;
	uint8_t total_layers;
	uint16_t total_actions;
	uint16_t seq_pool_count;
	uint8_t reserved[3];
	macro_layer_t layers[];
} mappings_t;
