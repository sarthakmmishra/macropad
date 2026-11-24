#include <stddef.h>
#include <stdint.h>

#include <string.h>

#include "control.h"
#include "defs.h"
#include "display.h"
#include "time.h"
#include "usb.h"

typedef struct
{
    bool active;
    bool release;
    uint16_t current_step;
    uint16_t end_step;
    uint32_t resume_time_ms;
} sequence_runner_t;

#define MIN_SEQ_DELAY 20

static sequence_runner_t seq_runner = {0};

const mappings_t *mappings;
const macro_layer_t *current_layer;
static uint8_t current_layer_idx;

static const action_t *actions_pool;
static const action_t *sequence_pool;

static const uint8_t zero[PAYLOAD_SIZE] = {0};

static const action_t *get_actions_pool(void)
{
    return (action_t *)((const uint8_t *)mappings->layers + (sizeof(macro_layer_t) * mappings->total_layers));
}

static const action_t *get_sequence_pool(void)
{
    return &actions_pool[mappings->total_actions];
}

static const action_t *get_action(int idx)
{
    if (idx >= mappings->total_actions)
        return NULL;
    return &actions_pool[idx];
}

const macro_layer_t *get_layer(uint8_t layer)
{
    if (layer >= mappings->total_layers)
        return NULL;

    return &mappings->layers[layer];
}

uint8_t get_current_layer_idx(void)
{
    return current_layer_idx;
}

uint8_t get_num_layers(void)
{
    return mappings->total_layers;
}

const char *get_layer_name(uint8_t layer)
{
    if (layer >= mappings->total_layers)
        return NULL;

    return mappings->layers[layer].layer_name;
}

int control_init(void)
{
    mappings = (mappings_t *)MAPPINGS_ADDR;

    if (mappings == NULL)
        return -1;

    if (mappings->magic_number != MAGIC)
    {
        mappings = NULL;
        return -2;
    }

    if (mappings->total_layers < 1 || mappings->total_actions < 1)
    {
        mappings = NULL;
        return -3;
    }

    actions_pool = get_actions_pool();
    sequence_pool = get_sequence_pool();

    current_layer = &mappings->layers[0];
    current_layer_idx = 0;

    display_layer();

    return 0;
}

static void start_sequence(uint16_t offset, uint8_t length)
{
    if (!sequence_pool || length == 0 || seq_runner.active)
        return;

    seq_runner.active = true;
    seq_runner.release = false;
    seq_runner.current_step = offset;
    seq_runner.end_step = offset + length;
    seq_runner.resume_time_ms = get_millis();
}

void process_sequence_tick(void)
{
    if (!seq_runner.active || !sequence_pool)
        return;

    uint32_t now = get_millis();

    if (now < seq_runner.resume_time_ms)
        return;

    if (seq_runner.release)
    {
        usb_send_key(zero);
        usb_send_cntrl(zero);
        seq_runner.release = false;

        seq_runner.resume_time_ms = now + MIN_SEQ_DELAY;
        return;
    }

    if (seq_runner.current_step >= seq_runner.end_step)
    {
        usb_send_key(zero);
        usb_send_cntrl(zero);
        seq_runner.active = false;
        return;
    }

    const action_t *step = &sequence_pool[seq_runner.current_step++];

    switch (step->type)
    {
    case ACTION_TYPE_SINGLE_KEY: {
        usb_keyboard_report_t report;
        memset(&report, 0, sizeof(report));

        report.modifiers = step->key.modifiers;
        if (step->key.key_index < 104)
        {
            uint8_t byte_idx = step->key.key_index / 8;
            uint8_t bit_idx = step->key.key_index % 8;
            report.key_bitmap[byte_idx] |= (1 << bit_idx);
        }
        usb_send_key((const uint8_t *)&report);
        usb_send_cntrl(zero);
        seq_runner.resume_time_ms = now + MIN_SEQ_DELAY;
        seq_runner.release = true;
        return;
    }

    case ACTION_TYPE_CONSUMER_CNTRL: {
        usb_consumer_report_t report;
        report.consumer_code = step->consumer_code;
        usb_send_cntrl((const uint8_t *)&report);
        usb_send_key(zero);
        seq_runner.resume_time_ms = now + MIN_SEQ_DELAY;
        seq_runner.release = true;
        return;
    }

    case ACTION_TYPE_RELEASE: {
        usb_send_key(zero);
        usb_send_cntrl(zero);
        return;
    }

    case ACTION_TYPE_DELAY: {
        seq_runner.resume_time_ms = now + step->delay_ms;
        return;
    }

    default:
        return;
    }
}

static void release_action(action_type_t action_type)
{
    switch (action_type)
    {
    case ACTION_TYPE_CONSUMER_CNTRL:
        usb_send_cntrl(zero);
        break;
    case ACTION_TYPE_SINGLE_KEY:
        usb_send_key(zero);
        break;
    default:
        break;
    }
}

static void dispatch_action(const action_t *action)
{
    if (!action)
        return;

    usb_keyboard_report_t key_report;
    usb_consumer_report_t cntrl_report;

    switch (action->type)
    {
    case ACTION_TYPE_SINGLE_KEY:
        memset(&key_report, 0, sizeof(key_report));

        key_report.modifiers = action->key.modifiers;
        if (action->key.key_index < 104)
        {
            uint8_t byte_idx = action->key.key_index / 8;
            uint8_t bit_idx = action->key.key_index % 8;
            key_report.key_bitmap[byte_idx] |= (1 << bit_idx);
        }
        usb_send_key((const uint8_t *)&key_report);
        break;
    case ACTION_TYPE_CONSUMER_CNTRL:
        memset(&cntrl_report, 0, sizeof(cntrl_report));
        cntrl_report.consumer_code = action->consumer_code;
        usb_send_cntrl((const uint8_t *)&cntrl_report);
        break;
    case ACTION_TYPE_SEQUENCE:
        start_sequence(action->sequence.sequence_offset, action->sequence.length);
        break;
    default:
        break;
    }
}

void control_handle_key_event(uint8_t key, bool pressed)
{
    if (key >= NUM_KEYS)
        return;

    const action_t *action = get_action(current_layer->action_indices[key]);

    if (pressed)
        dispatch_action(action);
    else
        release_action(action->type);
}

void control_handle_enc_press(uint8_t enc, bool pressed)
{
    if (enc >= NUM_ENCODERS)
        return;

    if (enc == 0 && pressed)
    {
        display_cycle_mode();
        display_layer();
        return;
    }

    const encoder_action_t *enc_action = &current_layer->enc_actions[enc - 1];
    const action_t *action = get_action(enc_action->press_idx);

    if (pressed)
        dispatch_action(action);
    else
        release_action(action->type);
}

static action_type_t prev_enc_action[NUM_ENCODERS - 1] = {ACTION_TYPE_NONE};

void control_handle_enc_rotate(uint8_t enc, int8_t dir)
{
    if (enc >= NUM_ENCODERS)
        return;

    if (enc == 0)
    {
        if (dir == -1)
        {
            if (current_layer_idx > 0)
                current_layer_idx--;
        }
        else if (dir == 1)
        {
            if (current_layer_idx < (mappings->total_layers - 1))
                current_layer_idx++;
        }

        current_layer = &mappings->layers[current_layer_idx];
        display_layer();
        return;
    }

    const encoder_action_t *enc_action = &current_layer->enc_actions[enc - 1];
    uint16_t target_idx = 0;

    target_idx = (dir == 1) ? enc_action->cw_idx : enc_action->ccw_idx;

    const action_t *action = get_action(target_idx);

    if (dir == 0)
    {
        release_action(prev_enc_action[enc - 1]);
        prev_enc_action[enc - 1] = ACTION_TYPE_NONE;
    }
    else
    {
        dispatch_action(action);
        prev_enc_action[enc - 1] = action->type;
    }
}
