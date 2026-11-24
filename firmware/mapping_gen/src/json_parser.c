#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "defs.h"
#include "dictionary.h"

#define MAX_ACTION_POOL_SIZE 128
#define MAX_SEQUENCE_POOL_SIZE 128

typedef struct
{
	action_t actions[MAX_ACTION_POOL_SIZE];
	uint16_t count;
} action_builder_t;

typedef struct
{
	action_t steps[MAX_SEQUENCE_POOL_SIZE];
	uint16_t count;
} sequence_builder_t;

static uint16_t add_or_get_action(action_builder_t *pool, const action_t *act)
{
	for (uint16_t i = 0; i < pool->count; i++)
	{
		if (memcmp(&pool->actions[i], act, sizeof(action_t)) == 0)
		{
			return i;
		}
	}
	if (pool->count < MAX_ACTION_POOL_SIZE)
	{
		uint16_t idx = pool->count++;
		pool->actions[idx] = *act;
		return idx;
	}
	return 0;
}

static bool parse_single_action_string(const char *str, action_t *out_action)
{
	memset(out_action, 0, sizeof(action_t));

	if (token_starts_with_ci(str, "DELAY_"))
	{
		out_action->type = ACTION_TYPE_DELAY;
		out_action->delay_ms = (uint16_t)atoi(str + 6);
		return true;
	}

	uint16_t consumer_val = 0;
	if (lookup_consumer_value(str, &consumer_val))
	{
		out_action->type = ACTION_TYPE_CONSUMER_CNTRL;
		out_action->consumer_code = consumer_val;
		return true;
	}

	uint8_t usb_type = USB_TYPE_KEY;
	uint8_t raw_payload[PAYLOAD_SIZE] = {0};

	if (parse_shortcut(str, &usb_type, raw_payload))
	{
		out_action->type = ACTION_TYPE_SINGLE_KEY;
		out_action->key.modifiers = raw_payload[0];

		for (uint8_t byte_i = 1; byte_i < PAYLOAD_SIZE; byte_i++)
		{
			if (raw_payload[byte_i] != 0)
			{
				for (uint8_t bit_i = 0; bit_i < 8; bit_i++)
				{
					if (raw_payload[byte_i] & (1 << bit_i))
					{
						out_action->key.key_index = ((byte_i - 1) * 8) + bit_i;
						return true;
					}
				}
			}
		}
		return true;
	}

	return false;
}

static bool parse_json_action(cJSON *val_node, action_t *out_action, sequence_builder_t *seq_builder)
{
	memset(out_action, 0, sizeof(action_t));

	if (cJSON_IsArray(val_node))
	{
		uint16_t seq_start_offset = seq_builder->count;
		uint8_t seq_len = 0;

		cJSON *item = NULL;
		cJSON_ArrayForEach(item, val_node)
		{
			if (cJSON_IsString(item) && seq_builder->count < MAX_SEQUENCE_POOL_SIZE)
			{
				action_t step_action;
				if (parse_single_action_string(item->valuestring, &step_action))
				{
					seq_builder->steps[seq_builder->count++] = step_action;
					seq_len++;
				}
				else
				{
					return false;
				}
			}
		}

		if (seq_len > 0)
		{
			out_action->type = ACTION_TYPE_SEQUENCE;
			out_action->sequence.sequence_offset = seq_start_offset;
			out_action->sequence.length = seq_len;
			return true;
		}
		return false;
	}

	if (cJSON_IsString(val_node))
	{
		return parse_single_action_string(val_node->valuestring, out_action);
	}

	return false;
}

bool parse_json_value_node(cJSON *node, char *out_name, cJSON **out_val_node)
{
	if (cJSON_IsArray(node) && cJSON_GetArraySize(node) == 2)
	{
		cJSON *name_item = cJSON_GetArrayItem(node, 0);
		cJSON *val_item = cJSON_GetArrayItem(node, 1);
		if (cJSON_IsString(name_item))
		{
			strncpy(out_name, name_item->valuestring, MAX_BINDING_NAME);
			*out_val_node = val_item;
			return true;
		}
	}
	else
	{
		strncpy(out_name, "Binding", MAX_BINDING_NAME);
		*out_val_node = node;
		return true;
	}
	return false;
}

char *read_file_to_string(const char *filename)
{
	FILE *file = fopen(filename, "rb");
	if (!file)
		return NULL;

	fseek(file, 0, SEEK_END);
	long length = ftell(file);
	fseek(file, 0, SEEK_SET);

	char *buffer = (char *)malloc(length + 1);
	if (buffer)
	{
		fread(buffer, 1, length, file);
		buffer[length] = '\0';
	}
	fclose(file);
	return buffer;
}

int parse_json_layout(const char *filename, mappings_t **out_map, size_t *out_bin_size)
{
	char *json_data = read_file_to_string(filename);
	if (!json_data)
	{
		fprintf(stderr, "Error reading file: %s\n", filename);
		return -1;
	}

	cJSON *root = cJSON_Parse(json_data);
	free(json_data);

	if (!root || !cJSON_IsArray(root))
	{
		fprintf(stderr, "JSON Parse Error: Root element must be an array of layers.\n");
		cJSON_Delete(root);
		return -1;
	}

	int num_layers = cJSON_GetArraySize(root);
	if (num_layers == 0)
	{
		fprintf(stderr, "Error: No layers defined in JSON.\n");
		cJSON_Delete(root);
		return -1;
	}

	action_builder_t act_builder = {0};
	sequence_builder_t seq_builder = {0};

	macro_layer_t *temp_layers = (macro_layer_t *)calloc(num_layers, sizeof(macro_layer_t));
	if (!temp_layers)
	{
		cJSON_Delete(root);
		return -1;
	}

	for (int i = 0; i < num_layers; i++)
	{
		cJSON *layer_json = cJSON_GetArrayItem(root, i);
		macro_layer_t *active_layer = &temp_layers[i];

		cJSON *name_node = cJSON_GetObjectItemCaseSensitive(layer_json, "name");
		if (cJSON_IsString(name_node))
		{
			strncpy(active_layer->layer_name, name_node->valuestring, MAX_LAYER_NAME);
		}

		cJSON *keys_obj = cJSON_GetObjectItemCaseSensitive(layer_json, "keys");
		if (cJSON_IsObject(keys_obj))
		{
			for (int k = 0; k < NUM_KEYS; k++)
			{
				char key_prop_name[16];
				snprintf(key_prop_name, sizeof(key_prop_name), "key_%d", k);

				cJSON *key_node = cJSON_GetObjectItemCaseSensitive(keys_obj, key_prop_name);
				if (key_node)
				{
					char disp_name[32] = {0};
					cJSON *val_node = NULL;

					if (parse_json_value_node(key_node, disp_name, &val_node))
					{
						strncpy(active_layer->binding_names[k], disp_name, MAX_BINDING_NAME);

						action_t parsed_action;
						if (parse_json_action(val_node, &parsed_action, &seq_builder))
						{
							active_layer->action_indices[k] = add_or_get_action(&act_builder, &parsed_action);
						}
						else
						{
							goto parse_fail;
						}
					}
				}
			}
		}

		cJSON *enc_obj = cJSON_GetObjectItemCaseSensitive(layer_json, "encoder");
		if (cJSON_IsObject(enc_obj))
		{
			cJSON *ccw = cJSON_GetObjectItemCaseSensitive(enc_obj, "ccw");
			cJSON *cw = cJSON_GetObjectItemCaseSensitive(enc_obj, "cw");
			cJSON *sw = cJSON_GetObjectItemCaseSensitive(enc_obj, "sw");

			char disp_name[32];
			cJSON *val_node = NULL;
			action_t enc_action;

			if (ccw && parse_json_value_node(ccw, disp_name, &val_node))
			{
				strncpy(active_layer->binding_names[9], disp_name, MAX_BINDING_NAME);
				if (parse_json_action(val_node, &enc_action, &seq_builder))
				{
					active_layer->enc_actions->ccw_idx = add_or_get_action(&act_builder, &enc_action);
				}
				else
				{
					goto parse_fail;
				}
			}

			if (cw && parse_json_value_node(cw, disp_name, &val_node))
			{
				strncpy(active_layer->binding_names[10], disp_name, MAX_BINDING_NAME);
				if (parse_json_action(val_node, &enc_action, &seq_builder))
				{
					active_layer->enc_actions->cw_idx = add_or_get_action(&act_builder, &enc_action);
				}
				else
				{
					goto parse_fail;
				}
			}

			if (sw && parse_json_value_node(sw, disp_name, &val_node))
			{
				if (parse_json_action(val_node, &enc_action, &seq_builder))
				{
					active_layer->enc_actions->press_idx = add_or_get_action(&act_builder, &enc_action);
				}
				else
				{
					goto parse_fail;
				}
			}
		}
	}

	cJSON_Delete(root);

	size_t layers_size = sizeof(macro_layer_t) * num_layers;
	size_t actions_size = sizeof(action_t) * act_builder.count;
	size_t seq_size = sizeof(action_t) * seq_builder.count;

	*out_bin_size = sizeof(mappings_t) + layers_size + actions_size + seq_size;
	mappings_t *map = (mappings_t *)calloc(1, *out_bin_size);
	if (!map)
	{
		free(temp_layers);
		return -1;
	}

	map->magic_number = MAGIC;
	map->total_layers = num_layers;
	map->total_actions = act_builder.count;
	map->seq_pool_count = seq_builder.count;

	uint8_t *buffer_ptr = (uint8_t *)map + sizeof(mappings_t);

	memcpy(buffer_ptr, temp_layers, layers_size);
	buffer_ptr += layers_size;

	memcpy(buffer_ptr, act_builder.actions, actions_size);
	buffer_ptr += actions_size;

	memcpy(buffer_ptr, seq_builder.steps, seq_size);

	free(temp_layers);
	*out_map = map;
	return 0;

parse_fail:
	cJSON_Delete(root);
	free(temp_layers);
	return 1;
}
