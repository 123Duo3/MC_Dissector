//
// Created by Nickid2018 on 2023/7/13.
//

#include <stdlib.h>
#include "protocol_schema.h"
#include "../protocol_data.h"
#include "../protocol_je/je_dissect.h"
#include "../protocol_be/be_dissect.h"

struct _protocol_set {
    wmem_map_t *client_packet_map;
    wmem_map_t *server_packet_map;
    wmem_map_t *client_name_map;
    wmem_map_t *server_name_map;
};

struct _protocol_entry {
    guint id;
    gchar *name;
    protocol_field field;
};

struct _protocol_field {
    bool hf_resolved;
    gchar *name;
    int hf_index;
    wmem_map_t *additional_info;

    guint (*make_tree)(const guint8 *data, proto_tree *tree, tvbuff_t *tvb,
                       protocol_field field, guint offset, guint remaining, data_recorder recorder);
};

// ---------------------------------- Native Fields ----------------------------------
#define FIELD_MAKE_TREE(name) \
    guint make_tree_##name(const guint8 *data, proto_tree *tree, tvbuff_t *tvb, \
    protocol_field field, guint offset, guint remaining, data_recorder recorder)

FIELD_MAKE_TREE(var_int) {
    guint result;
    guint length = read_var_int(data + offset, remaining, &result);
    if (tree)
        proto_tree_add_uint(tree, field->hf_index, tvb, offset, length, record_uint(recorder, result));
    else
        record_uint(recorder, result);
    return length;
}

FIELD_MAKE_TREE(var_long) {
    guint64 result;
    guint length = read_var_long(data + offset, remaining, &result);
    if (tree)
        proto_tree_add_uint64(tree, field->hf_index, tvb, offset, length, record_uint64(recorder, result));
    else
        record_uint64(recorder, result);
    return length;
}

FIELD_MAKE_TREE(string) {
    guint8 *str;
    guint length = read_buffer(data + offset, &str);
    if (tree)
        proto_tree_add_string(tree, field->hf_index, tvb, offset, length, record(recorder, str));
    else
        record(recorder, str);
    return length;
}

FIELD_MAKE_TREE(var_buffer) {
    guint8 *str;
    guint length = read_buffer(data + offset, &str);
    if (tree)
        proto_tree_add_bytes(tree, field->hf_index, tvb, offset, length, str);
    return length;
}

#define SINGLE_LENGTH_FIELD_MAKE(name, len, func_add, func_parse, record) \
    FIELD_MAKE_TREE(name) {                                               \
        if (tree)                                                         \
            func_add(tree, field->hf_index, tvb, offset, len, record(recorder, func_parse(tvb, offset))); \
        else                                                              \
            record(recorder, func_parse(tvb, offset));                    \
        return len;                                                       \
    }

SINGLE_LENGTH_FIELD_MAKE(u8, 1, proto_tree_add_uint, tvb_get_guint8, record_uint)

SINGLE_LENGTH_FIELD_MAKE(u16, 2, proto_tree_add_uint, tvb_get_ntohs, record_uint)

SINGLE_LENGTH_FIELD_MAKE(u32, 4, proto_tree_add_uint, tvb_get_ntohl, record_uint)

SINGLE_LENGTH_FIELD_MAKE(u64, 8, proto_tree_add_uint64, tvb_get_ntoh64, record_uint64)

SINGLE_LENGTH_FIELD_MAKE(i8, 1, proto_tree_add_int, tvb_get_gint8, record_int)

SINGLE_LENGTH_FIELD_MAKE(i16, 2, proto_tree_add_int, tvb_get_ntohis, record_int)

SINGLE_LENGTH_FIELD_MAKE(i32, 4, proto_tree_add_int, tvb_get_ntohil, record_int)

SINGLE_LENGTH_FIELD_MAKE(i64, 8, proto_tree_add_int64, tvb_get_ntohi64, record_int64)

SINGLE_LENGTH_FIELD_MAKE(f32, 4, proto_tree_add_float, tvb_get_ntohieee_float, record_float)

SINGLE_LENGTH_FIELD_MAKE(f64, 8, proto_tree_add_double, tvb_get_ntohieee_double, record_double)

SINGLE_LENGTH_FIELD_MAKE(boolean, 1, proto_tree_add_boolean, tvb_get_guint8, record_uint)

FIELD_MAKE_TREE(rest_buffer) {
    if (tree)
        proto_tree_add_bytes(tree, field->hf_index, tvb, offset, remaining,
                             record(recorder, tvb_memdup(wmem_packet_scope(), tvb, offset, remaining)));
    else
        record(recorder, tvb_memdup(wmem_packet_scope(), tvb, offset, remaining));
    return remaining;
}

FIELD_MAKE_TREE(uuid) {
    e_guid_t *uuid = wmem_new(wmem_packet_scope(), e_guid_t);
    tvb_get_guid(tvb, offset, uuid, 0);
    if (tree)
        proto_tree_add_guid(tree, field->hf_index, tvb, offset, 16, record(recorder, uuid));
    else
        record(recorder, uuid);
    return 16;
}

FIELD_MAKE_TREE(void) {
    return 0;
}

FIELD_MAKE_TREE(nbt) {
    guint length = count_nbt_length(data + offset);
    if (tree)
        proto_tree_add_bytes(tree, field->hf_index, tvb, offset, length,
                             tvb_memdup(wmem_packet_scope(), tvb, offset, length));
    return length;
}

FIELD_MAKE_TREE(optional_nbt) {
    guint8 present = tvb_get_guint8(tvb, offset);
    if (present != 0) {
        guint length = count_nbt_length(data + offset);
        if (tree)
            proto_tree_add_bytes(tree, field->hf_index, tvb, offset, length,
                                 tvb_memdup(wmem_packet_scope(), tvb, offset, length));
        return length;
    } else
        return 1;
}

guint make_tree_container(const guint8 *data, proto_tree *tree, tvbuff_t *tvb, protocol_field field, guint offset,
                          guint remaining, data_recorder recorder, bool is_je) {
    bool not_top = wmem_map_lookup(field->additional_info, GINT_TO_POINTER(-1)) == NULL;
    if (not_top)
        record_push(recorder);
    if (tree && not_top)
        tree = proto_tree_add_subtree(tree, tvb, offset, remaining,
                                      is_je ? ett_sub_je : ett_sub_be, NULL, field->name);
    guint length = GPOINTER_TO_UINT(wmem_map_lookup(field->additional_info, 0));
    guint total_length = 0;
    for (guint i = 1; i <= length; i++) {
        protocol_field sub_field = wmem_map_lookup(field->additional_info, GUINT_TO_POINTER(i));
        record_start(recorder, sub_field->name);
        guint sub_length = sub_field->make_tree(data, tree, tvb, sub_field, offset, remaining, recorder);
        offset += sub_length;
        total_length += sub_length;
        remaining -= sub_length;
    }
    if (not_top)
        record_pop(recorder);
    return total_length;
}

FIELD_MAKE_TREE(container_je) {
    return make_tree_container(data, tree, tvb, field, offset, remaining, recorder, true);
}

FIELD_MAKE_TREE(container_be) {
    return make_tree_container(data, tree, tvb, field, offset, remaining, recorder, false);
}

FIELD_MAKE_TREE(option) {
    bool is_present = tvb_get_guint8(tvb, offset) != 0;
    if (is_present) {
        protocol_field sub_field = wmem_map_lookup(field->additional_info, 0);
        if (field->hf_resolved && !sub_field->hf_resolved)
            sub_field->hf_index = field->hf_index;
        return sub_field->make_tree(data, tree, tvb, sub_field, offset + 1, remaining - 1, recorder) + 1;
    } else
        return 1;
}

FIELD_MAKE_TREE(buffer) {
    guint length = GPOINTER_TO_UINT(wmem_map_lookup(field->additional_info, 0));
    if (tree)
        proto_tree_add_bytes(tree, field->hf_index, tvb, offset + 1, length,
                             tvb_memdup(wmem_packet_scope(), tvb, offset, length));
    return length;
}

FIELD_MAKE_TREE(mapper) {
    protocol_field sub_field = wmem_map_lookup(field->additional_info, GINT_TO_POINTER(-1));
    if (field->hf_resolved && !sub_field->hf_resolved)
        sub_field->hf_index = field->hf_index;
    gchar *recording = record_get_recording(recorder);
    guint length = sub_field->make_tree(data, NULL, tvb, sub_field, offset, remaining, recorder);
    guint map = GPOINTER_TO_UINT(record_query(recorder, 1, recording));
    gchar *map_name = wmem_map_lookup(field->additional_info, GUINT_TO_POINTER(map));
    record_start(recorder, recording);
    record(recorder, map_name);
    if (tree)
        proto_tree_add_string(tree, field->hf_index, tvb, offset, length, map_name);
    return length;
}

FIELD_MAKE_TREE(array) {
    protocol_field sub_field = wmem_map_lookup(field->additional_info, GINT_TO_POINTER(1));
    if (field->hf_resolved && !sub_field->hf_resolved)
        sub_field->hf_index = field->hf_index;
    char *len_data = wmem_map_lookup(field->additional_info, 0);
    guint len = 0;
    guint data_count = 0;
    if (len_data == NULL)
        len = read_var_int(data + offset, remaining, &data_count);
    else
        data_count = GPOINTER_TO_UINT(record_query(recorder, 1, len_data));
    offset += len;
    remaining -= len;
    gchar *recording = record_get_recording(recorder);
    char *name_raw = sub_field->name;
    for (int i = 0; i < data_count; i++) {
        record_start(recorder, g_strconcat(recording, "[", g_strdup_printf("%d", i), "]", NULL));
        sub_field->name = g_strdup_printf("%s[%d]", field->name, i);
        guint sub_length = sub_field->make_tree(data, tree, tvb, sub_field, offset, remaining, recorder);
        offset += sub_length;
        len += sub_length;
        remaining -= sub_length;
    }
    sub_field->name = name_raw;
    return len;
}

guint make_tree_bitfield(const guint8 *data, proto_tree *tree, tvbuff_t *tvb, protocol_field field, guint offset,
                         guint remaining, data_recorder recorder, bool is_je) {
    int size = GPOINTER_TO_INT(wmem_map_lookup(field->additional_info, GINT_TO_POINTER(-1)));
    int *const *bitfields = wmem_map_lookup(field->additional_info, GINT_TO_POINTER(-2));
    if (tree)
        proto_tree_add_bitmask(tree, tvb, offset, field->hf_index,
                               is_je ? ett_sub_je : ett_sub_be, bitfields, ENC_BIG_ENDIAN);
    record_push(recorder);
    int offset_bit = 0;
    for (int i = 0; i < size; i++) {
        int len = GPOINTER_TO_INT(wmem_map_lookup(field->additional_info, GINT_TO_POINTER(i * 3)));
        bool signed_ = GPOINTER_TO_INT(wmem_map_lookup(field->additional_info, GINT_TO_POINTER(i * 3 + 1)));
        char *name = wmem_map_lookup(field->additional_info, GINT_TO_POINTER(i * 3 + 2));
        record_start(recorder, name);
        if (len <= 32) {
            guint read = tvb_get_bits(tvb, offset * 8 + offset_bit, len, ENC_BIG_ENDIAN);
            if (signed_)
                record_int(recorder, *(gint32 *) &read);
            else
                record_uint(recorder, read);
        } else {
            guint64 read = tvb_get_bits64(tvb, offset * 8 + offset_bit, len, ENC_BIG_ENDIAN);
            if (signed_)
                record_int64(recorder, *(gint64 *) &read);
            else
                record_uint64(recorder, read);
        }
        offset_bit += len;
    }
    record_pop(recorder);
    return (offset_bit + 7) / 8;
}

FIELD_MAKE_TREE(bitfield_je) {
    return make_tree_bitfield(data, tree, tvb, field, offset, remaining, recorder, true);
}

FIELD_MAKE_TREE(bitfield_be) {
    return make_tree_bitfield(data, tree, tvb, field, offset, remaining, recorder, false);
}

// ------------------------------- End of Native Fields --------------------------------

wmem_map_t *native_make_tree_map = NULL;
wmem_map_t *native_unknown_fallback_map = NULL;

#define ADD_NATIVE(json_name, make_name, unknown_flag) \
    wmem_map_insert(native_make_tree_map, #json_name, make_tree_##make_name); \
    wmem_map_insert(native_unknown_fallback_map, #json_name, #unknown_flag);

void init_schema_data() {
    native_make_tree_map = wmem_map_new(wmem_epan_scope(), g_str_hash, g_str_equal);
    native_unknown_fallback_map = wmem_map_new(wmem_epan_scope(), g_str_hash, g_str_equal);

    ADD_NATIVE(varint, var_int, uint)
    ADD_NATIVE(optvarint, var_int, uint)
    ADD_NATIVE(varlong, var_long, uint64)
    ADD_NATIVE(string, string, string)
    ADD_NATIVE(u8, u8, uint)
    ADD_NATIVE(u16, u16, uint)
    ADD_NATIVE(u32, u32, uint)
    ADD_NATIVE(u64, u64, uint64)
    ADD_NATIVE(i8, i8, int)
    ADD_NATIVE(i16, i16, int)
    ADD_NATIVE(i32, i32, int)
    ADD_NATIVE(i64, i64, int64)
    ADD_NATIVE(bool, boolean, boolean)
    ADD_NATIVE(f32, f32, float)
    ADD_NATIVE(f64, f64, double)
    ADD_NATIVE(UUID, uuid, uuid)
    ADD_NATIVE(restBuffer, rest_buffer, bytes)
    ADD_NATIVE(void, void, uint)
    ADD_NATIVE(nbt, nbt, bytes)
    ADD_NATIVE(optionalNbt, optional_nbt, bytes)
}

protocol_field parse_protocol(cJSON *data, cJSON *types, bool is_je, bool on_top) {
    if (cJSON_IsString(data)) {
        char *type = data->valuestring;
        void *make_tree_func = wmem_map_lookup(native_make_tree_map, type);
        if (make_tree_func != NULL) {
            protocol_field field = wmem_new(wmem_file_scope(), protocol_field_t);
            char *unknown_fallback = wmem_map_lookup(native_unknown_fallback_map, type);
            field->hf_index = GPOINTER_TO_INT(wmem_map_lookup(
                    is_je ? unknown_hf_map_je : unknown_hf_map_be, unknown_fallback));
            field->hf_resolved = false;
            field->name = NULL;
            field->additional_info = NULL;
            field->make_tree = make_tree_func;
            return field;
        }
        return parse_protocol(cJSON_GetObjectItem(types, data->valuestring), types, is_je, false);
    }
    if (cJSON_GetArraySize(data) != 2)
        return NULL;
    char *type = cJSON_GetArrayItem(data, 0)->valuestring;

    protocol_field field = wmem_new(wmem_file_scope(), protocol_field_t);
    field->additional_info = wmem_map_new(wmem_file_scope(), g_direct_hash, g_direct_equal);
    field->make_tree = NULL;
    field->name = NULL;
    field->hf_index = -1;
    field->hf_resolved = false;

    wmem_map_t *search_hf_map = is_je ? name_hf_map_je : name_hf_map_be;
    cJSON *fields = cJSON_GetArrayItem(data, 1);
    if (strcmp(type, "container") == 0) { // container
        field->make_tree = is_je ? make_tree_container_je : make_tree_container_be;
        int size = cJSON_GetArraySize(fields);
        wmem_map_insert(field->additional_info, 0, GINT_TO_POINTER(size));
        for (int i = 0; i < size; i++) {
            cJSON *field_data = cJSON_GetArrayItem(fields, i);
            cJSON *type_data = cJSON_GetObjectItem(field_data, "type");
            protocol_field sub_field = parse_protocol(type_data, types, is_je, false);
            if (sub_field == NULL)
                return NULL;
            if (cJSON_HasObjectItem(field_data, "name")) {
                sub_field->name = strdup(cJSON_GetObjectItem(field_data, "name")->valuestring);
                int hf_index = GPOINTER_TO_INT(wmem_map_lookup(search_hf_map, sub_field->name));
                if (hf_index != 0) {
                    sub_field->hf_index = hf_index;
                    field->hf_resolved = true;
                }
            } else
                sub_field->name = strdup("Anon Field");
            wmem_map_insert(field->additional_info, GINT_TO_POINTER(i + 1), sub_field);
        }
        if (on_top)
            wmem_map_insert(field->additional_info, GINT_TO_POINTER(-1), GINT_TO_POINTER(1));
        return field;
    } else if (strcmp(type, "option") == 0) { // option
        field->make_tree = make_tree_option;
        protocol_field sub_field = parse_protocol(fields, types, is_je, false);
        if (sub_field == NULL)
            return NULL;
        wmem_map_insert(field->additional_info, 0, sub_field);
        return field;
    } else if (strcmp(type, "buffer") == 0) { // buffer
        field->hf_index = GPOINTER_TO_INT(wmem_map_lookup(search_hf_map, "bytes"));
        if (cJSON_HasObjectItem(fields, "count")) {
            field->make_tree = make_tree_buffer;
            cJSON *count = cJSON_GetObjectItem(fields, "count");
            wmem_map_insert(field->additional_info, 0, GINT_TO_POINTER(count->valueint));
        } else
            field->make_tree = make_tree_var_buffer;
        return field;
    } else if (strcmp(type, "mapper") == 0) { // mapper
        cJSON *type_data = cJSON_GetObjectItem(fields, "type");
        protocol_field sub_field = parse_protocol(type_data, types, is_je, false);
        if (sub_field == NULL)
            return NULL;
        field->make_tree = make_tree_mapper;
        wmem_map_insert(field->additional_info, GINT_TO_POINTER(-1), sub_field);
        cJSON *mappings = cJSON_GetObjectItem(fields, "mappings");
        cJSON *now = mappings->child;
        while (now != NULL) {
            char *key = now->string;
            int key_int = strtol(key, NULL, 10);
            char *value = now->valuestring;
            wmem_map_insert(sub_field->additional_info, GINT_TO_POINTER(key_int), strdup(value));
            now = now->next;
        }
        return field;
    } else if (strcmp(type, "array") == 0) { // array
        cJSON *count_type = cJSON_GetObjectItem(fields, "countType");
        char *count_type_str = count_type->valuestring;
        if (strcmp(count_type_str, "varint") != 0)
            wmem_map_insert(field->additional_info, 0, strdup(count_type_str));
        cJSON *type_data = cJSON_GetObjectItem(fields, "type");
        protocol_field sub_field = parse_protocol(type_data, types, is_je, false);
        if (sub_field == NULL)
            return NULL;
        field->make_tree = make_tree_array;
        wmem_map_insert(field->additional_info, GINT_TO_POINTER(1), sub_field);
        return field;
    } else if (strcmp(type, "bitfield") == 0) {
        int size = cJSON_GetArraySize(fields);
        wmem_map_insert(field->additional_info, GINT_TO_POINTER(-1), GINT_TO_POINTER(size));
        char *bitmask_name = "";
        for (int i = 0; i < size; i++) {
            cJSON *field_data = cJSON_GetArrayItem(fields, i);
            bool signed_ = cJSON_GetObjectItem(field_data, "signed")->valueint;
            int bits = cJSON_GetObjectItem(field_data, "size")->valueint;
            char *name = cJSON_GetObjectItem(field_data, "name")->valuestring;
            bitmask_name = g_strdup_printf("%s[%d]%s", bitmask_name, bits, name);
            wmem_map_insert(field->additional_info, GINT_TO_POINTER(i * 3), GINT_TO_POINTER(bits));
            wmem_map_insert(field->additional_info, GINT_TO_POINTER(i * 3 + 1), GINT_TO_POINTER(signed_));
            wmem_map_insert(field->additional_info, GINT_TO_POINTER(i * 3 + 2), strdup(name));
        }
        int **hf_data = wmem_map_lookup(is_je ? bitmask_hf_map_je : bitmask_hf_map_be, bitmask_name);
        if (hf_data == NULL)
            return NULL;
        wmem_map_insert(field->additional_info, GINT_TO_POINTER(-2), hf_data);
        field->make_tree = is_je ? make_tree_bitfield_je : make_tree_bitfield_be;
        int hf_index = GPOINTER_TO_INT(wmem_map_lookup(search_hf_map, bitmask_name));
        if (hf_index == 0)
            return NULL;
        field->hf_index = hf_index;
        field->hf_resolved = true;
        return field;
    }
    // entityMetadataLoop/topBitSetTerminatedArray/switch

    return NULL;
}

void make_simple_protocol(cJSON *data, cJSON *types, wmem_map_t *packet_map, wmem_map_t *name_map, bool is_je) {
    cJSON *packets = cJSON_GetObjectItem(data, "packet");
    // Path: [1].[0].type.[1].mappings
    cJSON *c1 = cJSON_GetArrayItem(packets, 1);
    cJSON *c2 = cJSON_GetArrayItem(c1, 0);
    cJSON *c3 = cJSON_GetObjectItem(c2, "type");
    cJSON *c4 = cJSON_GetArrayItem(c3, 1);
    cJSON *mappings = cJSON_GetObjectItem(c4, "mappings");
    cJSON *now = mappings->child;
    while (now != NULL) {
        char *packet_id_str = now->string;
        gchar *packet_name = strdup(now->valuestring);
        char *ptr;
        guint packet_id = (guint) strtol(packet_id_str + 2, &ptr, 16);
        wmem_map_insert(name_map, packet_name, GUINT_TO_POINTER(packet_id + 1));

        protocol_entry entry = wmem_new(wmem_file_scope(), protocol_entry_t);
        entry->id = packet_id;
        entry->name = packet_name;
        wmem_map_insert(packet_map, GUINT_TO_POINTER(packet_id), entry);

        gchar *packet_definition = g_strconcat("packet_", packet_name, NULL);
        entry->field = parse_protocol(cJSON_GetObjectItem(data, packet_definition), types, is_je, true);
        g_free(packet_definition);

        now = now->next;
    }
}

protocol_set create_protocol_set(cJSON *types, cJSON *data, bool is_je) {
    protocol_set set = wmem_new(wmem_file_scope(), protocol_set_t);
    set->client_packet_map = wmem_map_new(wmem_file_scope(), g_direct_hash, g_direct_equal);
    set->server_packet_map = wmem_map_new(wmem_file_scope(), g_direct_hash, g_direct_equal);
    set->client_name_map = wmem_map_new(wmem_file_scope(), g_str_hash, g_str_equal);
    set->server_name_map = wmem_map_new(wmem_file_scope(), g_str_hash, g_str_equal);

    cJSON *to_client = cJSON_GetObjectItem(cJSON_GetObjectItem(data, "toClient"), "types");
    cJSON *to_server = cJSON_GetObjectItem(cJSON_GetObjectItem(data, "toServer"), "types");
    make_simple_protocol(to_client, types, set->client_packet_map, set->client_name_map, is_je);
    make_simple_protocol(to_server, types, set->server_packet_map, set->server_name_map, is_je);

    return set;
}

gchar *get_packet_name(protocol_entry entry) {
    return entry == NULL ? "Unknown" : entry->name;
}

gint get_packet_id_by_entry(protocol_entry entry) {
    return entry == NULL ? -1 : (int) entry->id;
}

gint get_packet_id(protocol_set set, gchar *name, bool is_client) {
    wmem_map_t *name_map = is_client ? set->client_packet_map : set->server_name_map;
    return GPOINTER_TO_INT(wmem_map_lookup(name_map, name)) - 1;
}

protocol_entry get_protocol_entry(protocol_set set, guint packet_id, bool is_client) {
    wmem_map_t *packet_map = is_client ? set->client_packet_map : set->server_packet_map;
    return wmem_map_lookup(packet_map, GUINT_TO_POINTER(packet_id));
}

bool make_tree(protocol_entry entry, proto_tree *tree, tvbuff_t *tvb, const guint8 *data, guint remaining) {
    if (entry->field != NULL) {
        data_recorder recorder = create_data_recorder();
        gint len = entry->field->make_tree(data, tree, tvb, entry->field, 1, remaining - 1, recorder);
        destroy_data_recorder(recorder);
        if (len != remaining - 1)
            proto_tree_add_string_format_value(tree, hf_invalid_data_je, tvb, 1, remaining - 1,
                                               "length mismatch", "Packet length mismatch, expected %d, got %d", len,
                                               remaining - 1);
        return true;
    }
    return false;
}