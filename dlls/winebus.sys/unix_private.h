/*
 * Copyright 2021 Rémi Bernon for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __WINEBUS_UNIX_PRIVATE_H
#define __WINEBUS_UNIX_PRIVATE_H

#include <stdarg.h>

#include <windef.h>
#include <winbase.h>
#include <winternl.h>

#include "unixlib.h"

#include "wine/list.h"

struct effect_periodic
{
    BYTE magnitude;
    BYTE offset;
    BYTE phase;
    UINT16 period;
};

struct effect_envelope
{
    BYTE attack_level;
    BYTE fade_level;
    UINT16 attack_time;
    UINT16 fade_time;
};

struct effect_condition
{
    BYTE center_point_offset;
    BYTE positive_coefficient;
    BYTE negative_coefficient;
    BYTE positive_saturation;
    BYTE negative_saturation;
    BYTE dead_band;
};

struct effect_constant_force
{
    UINT16 magnitude;
};

struct effect_ramp_force
{
    BYTE ramp_start;
    BYTE ramp_end;
};

struct effect_params
{
    USAGE effect_type;
    UINT16 duration;
    UINT16 trigger_repeat_interval;
    UINT16 sample_period;
    UINT16 start_delay;
    BYTE gain;
    BYTE trigger_button;
    BOOL axis_enabled[2];
    BOOL direction_enabled;
    BYTE direction[2];
    BYTE condition_count;
    /* only for periodic, constant or ramp forces */
    struct effect_envelope envelope;
    union
    {
        struct effect_periodic periodic;
        struct effect_condition condition[2];
        struct effect_constant_force constant_force;
        struct effect_ramp_force ramp_force;
    };
};

struct raw_device_vtbl
{
    void (*destroy)(struct unix_device *iface);
    NTSTATUS (*start)(struct unix_device *iface);
    void (*stop)(struct unix_device *iface);
    NTSTATUS (*get_report_descriptor)(struct unix_device *iface, BYTE *buffer, DWORD length, DWORD *out_length);
    void (*set_output_report)(struct unix_device *iface, HID_XFER_PACKET *packet, IO_STATUS_BLOCK *io);
    void (*get_feature_report)(struct unix_device *iface, HID_XFER_PACKET *packet, IO_STATUS_BLOCK *io);
    void (*set_feature_report)(struct unix_device *iface, HID_XFER_PACKET *packet, IO_STATUS_BLOCK *io);
};

struct hid_device_vtbl
{
    void (*destroy)(struct unix_device *iface);
    NTSTATUS (*start)(struct unix_device *iface);
    void (*stop)(struct unix_device *iface);
    NTSTATUS (*haptics_start)(struct unix_device *iface, DWORD duration_ms,
                              USHORT rumble_intensity, USHORT buzz_intensity);
    NTSTATUS (*physical_device_control)(struct unix_device *iface, USAGE control);
    NTSTATUS (*physical_device_set_gain)(struct unix_device *iface, BYTE value);
    NTSTATUS (*physical_effect_control)(struct unix_device *iface, BYTE index, USAGE control, BYTE iterations);
    NTSTATUS (*physical_effect_update)(struct unix_device *iface, BYTE index, struct effect_params *params);
};

struct hid_report_descriptor
{
    BYTE *data;
    SIZE_T size;
    SIZE_T max_size;
    BYTE next_report_id[3];
};

enum haptics_waveform_index
{
    HAPTICS_WAVEFORM_STOP_INDEX = 1,
    HAPTICS_WAVEFORM_NULL_INDEX = 2,
    HAPTICS_WAVEFORM_RUMBLE_INDEX = 3,
    HAPTICS_WAVEFORM_BUZZ_INDEX = 4,
    HAPTICS_WAVEFORM_LAST_INDEX = HAPTICS_WAVEFORM_BUZZ_INDEX,
};

struct hid_haptics_features
{
    WORD  waveform_list[HAPTICS_WAVEFORM_LAST_INDEX - HAPTICS_WAVEFORM_NULL_INDEX];
    WORD  duration_list[HAPTICS_WAVEFORM_LAST_INDEX - HAPTICS_WAVEFORM_NULL_INDEX];
    DWORD waveform_cutoff_time_ms;
};

struct hid_haptics_waveform
{
    WORD manual_trigger;
    WORD intensity;
};

struct hid_haptics
{
    struct hid_haptics_features features;
    struct hid_haptics_waveform waveforms[HAPTICS_WAVEFORM_LAST_INDEX + 1];
    BYTE features_report;
    BYTE waveform_report;
};

struct hid_physical
{
    USAGE effect_types[32];
    struct effect_params effect_params[256];

    BYTE device_control_report;
    BYTE device_gain_report;
    BYTE effect_control_report;
    BYTE effect_update_report;
    BYTE set_periodic_report;
    BYTE set_envelope_report;
    BYTE set_condition_report;
    BYTE set_constant_force_report;
    BYTE set_ramp_force_report;
};

struct hid_device_state
{
    ULONG bit_size;
    USHORT abs_axis_start;
    USHORT abs_axis_count;
    USHORT rel_axis_start;
    USHORT rel_axis_count;
    USHORT hatswitch_start;
    USHORT hatswitch_count;
    USHORT button_start;
    USHORT button_count;
    USHORT report_len;
    BYTE *report_buf;
    BYTE *last_report_buf;
    BOOL dropped;
    BYTE id;
};

struct unix_device
{
    const struct raw_device_vtbl *vtbl;
    struct list entry;
    LONG ref;

    const struct hid_device_vtbl *hid_vtbl;
    struct hid_report_descriptor hid_report_descriptor;
    struct hid_device_state hid_device_state;
    struct hid_haptics hid_haptics;
    struct hid_physical hid_physical;
};

extern void *raw_device_create(const struct raw_device_vtbl *vtbl, SIZE_T size) DECLSPEC_HIDDEN;
extern void *hid_device_create(const struct hid_device_vtbl *vtbl, SIZE_T size) DECLSPEC_HIDDEN;

extern NTSTATUS sdl_bus_init(void *) DECLSPEC_HIDDEN;
extern NTSTATUS sdl_bus_wait(void *) DECLSPEC_HIDDEN;
extern NTSTATUS sdl_bus_stop(void *) DECLSPEC_HIDDEN;

extern NTSTATUS udev_bus_init(void *) DECLSPEC_HIDDEN;
extern NTSTATUS udev_bus_wait(void *) DECLSPEC_HIDDEN;
extern NTSTATUS udev_bus_stop(void *) DECLSPEC_HIDDEN;

extern NTSTATUS iohid_bus_init(void *) DECLSPEC_HIDDEN;
extern NTSTATUS iohid_bus_wait(void *) DECLSPEC_HIDDEN;
extern NTSTATUS iohid_bus_stop(void *) DECLSPEC_HIDDEN;

extern void bus_event_cleanup(struct bus_event *event) DECLSPEC_HIDDEN;
extern void bus_event_queue_destroy(struct list *queue) DECLSPEC_HIDDEN;
extern BOOL bus_event_queue_device_removed(struct list *queue, struct unix_device *device) DECLSPEC_HIDDEN;
extern BOOL bus_event_queue_device_created(struct list *queue, struct unix_device *device, struct device_desc *desc) DECLSPEC_HIDDEN;
extern BOOL bus_event_queue_input_report(struct list *queue, struct unix_device *device,
                                         BYTE *report, USHORT length) DECLSPEC_HIDDEN;
extern BOOL bus_event_queue_pop(struct list *queue, struct bus_event *event) DECLSPEC_HIDDEN;

extern BOOL hid_device_begin_report_descriptor(struct unix_device *iface, USAGE usage_page, USAGE usage) DECLSPEC_HIDDEN;
extern BOOL hid_device_end_report_descriptor(struct unix_device *iface) DECLSPEC_HIDDEN;

extern BOOL hid_device_begin_input_report(struct unix_device *iface) DECLSPEC_HIDDEN;
extern BOOL hid_device_end_input_report(struct unix_device *iface) DECLSPEC_HIDDEN;
extern BOOL hid_device_add_buttons(struct unix_device *iface, USAGE usage_page,
                                   USAGE usage_min, USAGE usage_max) DECLSPEC_HIDDEN;
extern BOOL hid_device_add_hatswitch(struct unix_device *iface, INT count) DECLSPEC_HIDDEN;
extern BOOL hid_device_add_axes(struct unix_device *iface, BYTE count, USAGE usage_page,
                                const USAGE *usages, BOOL rel, LONG min, LONG max) DECLSPEC_HIDDEN;

extern BOOL hid_device_add_haptics(struct unix_device *iface) DECLSPEC_HIDDEN;
extern BOOL hid_device_add_physical(struct unix_device *iface, USAGE *usages, USHORT count) DECLSPEC_HIDDEN;

extern BOOL hid_device_set_abs_axis(struct unix_device *iface, ULONG index, LONG value) DECLSPEC_HIDDEN;
extern BOOL hid_device_set_rel_axis(struct unix_device *iface, ULONG index, LONG value) DECLSPEC_HIDDEN;
extern BOOL hid_device_set_button(struct unix_device *iface, ULONG index, BOOL is_set) DECLSPEC_HIDDEN;
extern BOOL hid_device_set_hatswitch_x(struct unix_device *iface, ULONG index, LONG new_x) DECLSPEC_HIDDEN;
extern BOOL hid_device_set_hatswitch_y(struct unix_device *iface, ULONG index, LONG new_y) DECLSPEC_HIDDEN;

extern BOOL hid_device_sync_report(struct unix_device *iface) DECLSPEC_HIDDEN;
extern void hid_device_drop_report(struct unix_device *iface) DECLSPEC_HIDDEN;

BOOL is_xbox_gamepad(WORD vid, WORD pid) DECLSPEC_HIDDEN;
BOOL is_dualshock4_gamepad(WORD vid, WORD pid) DECLSPEC_HIDDEN;

#endif /* __WINEBUS_UNIX_PRIVATE_H */
