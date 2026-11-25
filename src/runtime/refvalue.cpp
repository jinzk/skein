#include "refvalue.h"

#include "dict.h"
#include "structtype.h"
#include "symbol.h"

#include <algorithm>
#include <cstdio>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct ref_registry {
    uint64_t next_id = 1;
    std::vector<std::weak_ptr<ref_object>> objects;
    std::size_t allocations_since_collect = 0;
};

thread_local ref_registry tls_refs;

struct ref_handle {
    std::shared_ptr<ref_object> object;
};

struct weak_handle {
    uint64_t id = 0;
    uint64_t generation = 0;
    struct_def *def = nullptr;
    std::weak_ptr<ref_object> object;
};

value_t *ref_value_from_object(const std::shared_ptr<ref_object> &object) {
    if (!object)
        return nullptr;
    auto handle = std::make_shared<ref_handle>();
    handle->object = object;
    return newuserdataval(ref_native_type(), std::move(handle));
}

bool field_is_weak(struct_def *def, int index) {
    return def && index >= 0 && index < (int)def->fields.size() &&
           def->fields[(size_t)index].type.kind == TYPE_WEAK;
}

value_t *get_field_from_object(const std::shared_ptr<ref_object> &object,
                               const char *name) {
    if (!object || !object->def || !object->payload || !name) {
        yyerror("invalid ref value");
        return nullptr;
    }
    const char *field_name = name;
    int field = structtype_field_index(object->def, field_name);
    if (field >= 0 && (size_t)field < object->payload->fields.size()) {
        value_t *value = object->payload->fields[(size_t)field];
        if (!value)
            return newnull();
        if (field_is_weak(object->def, field) && is_ref_value(value))
            return new_weak_from_ref(value);
        return dupval(value);
    }
    std::shared_ptr<function_def> method =
        structtype_method(object->def, field_name);
    if (method) {
        value_t *receiver = ref_value_from_object(object);
        return newmethodval(receiver, method);
    }
    yyerror("field %s not found in %s", field_name,
            object->def->name.c_str());
    return nullptr;
}

bool set_field_on_object(const std::shared_ptr<ref_object> &object,
                         const char *name, value_t *value) {
    if (!object || !object->def || !object->payload || !name || !value) {
        yyerror("invalid ref value");
        freeval(value);
        return false;
    }
    int field = structtype_field_index(object->def, name);
    if (field < 0 || (size_t)field >= object->def->fields.size()) {
        yyerror("field %s not found in %s", name, object->def->name.c_str());
        freeval(value);
        return false;
    }
    const type_constraint &type = object->def->fields[(size_t)field].type;
    /* The caller owns `value`; store a private copy so the caller can free
     * its temporary after this callback returns. */
    value_t *stored = coerce_to_ref_type(&type, value);
    if (!stored)
        return false;
    if (!type_constraint_matches(&type, stored)) {
        yyerror("field %s of %s expects %s, got %s", name,
                object->def->name.c_str(), type_constraint_name(&type),
                typename_of(stored));
        freeval(stored);
        return false;
    }
    if ((size_t)field >= object->payload->fields.size()) {
        yyerror("invalid ref field assignment");
        freeval(stored);
        return false;
    }
    freeval(object->payload->fields[(size_t)field]);
    object->payload->fields[(size_t)field] = stored;
    return true;
}

std::string describe_ref(const void *data) {
    const ref_handle *handle = static_cast<const ref_handle *>(data);
    if (!handle || !handle->object || !handle->object->def)
        return "ref";
    return "ref " + handle->object->def->name;
}

std::string describe_weak(const void *data) {
    const weak_handle *handle = static_cast<const weak_handle *>(data);
    if (!handle || !handle->def)
        return "weak";
    std::string text = "weak " + handle->def->name;
    if (handle->object.expired() || !handle->object.lock())
        text += " (stale)";
    return text;
}

value_t *ref_get_field(void *data, const char *name) {
    ref_handle *handle = static_cast<ref_handle *>(data);
    return get_field_from_object(handle ? handle->object : nullptr, name);
}

bool ref_set_field(void *data, const char *name, value_t *value) {
    ref_handle *handle = static_cast<ref_handle *>(data);
    return set_field_on_object(handle ? handle->object : nullptr, name,
                               value);
}

value_t *weak_get_field(void *data, const char *name) {
    weak_handle *handle = static_cast<weak_handle *>(data);
    if (!handle)
        return nullptr;
    std::shared_ptr<ref_object> object = handle->object.lock();
    if (!object || object->id != handle->id ||
        object->generation != handle->generation) {
        yyerror("weak reference is no longer valid");
        return nullptr;
    }
    return get_field_from_object(object, name);
}

bool weak_set_field(void *data, const char *name, value_t *value) {
    weak_handle *handle = static_cast<weak_handle *>(data);
    if (!handle) {
        freeval(value);
        return false;
    }
    std::shared_ptr<ref_object> object = handle->object.lock();
    if (!object || object->id != handle->id ||
        object->generation != handle->generation) {
        yyerror("weak reference is no longer valid");
        freeval(value);
        return false;
    }
    return set_field_on_object(object, name, value);
}

const native_type kRefType = {
    "ref",
    describe_ref,
    nullptr,
    ref_get_field,
    ref_set_field,
    nullptr,
};

const native_type kWeakType = {
    "weak",
    describe_weak,
    nullptr,
    weak_get_field,
    weak_set_field,
    nullptr,
};

struct snapshot_state {
    std::unordered_set<const ref_object *> seen;
};

value_t *snapshot_value_inner(value_t *value, snapshot_state &state);

value_t *snapshot_struct(struct_def *def,
                         const std::shared_ptr<struct_payload> &payload,
                         snapshot_state &state) {
    if (!def || !payload)
        return nullptr;
    auto copy = std::make_shared<struct_payload>();
    copy->fields.resize(payload->fields.size(), nullptr);
    for (std::size_t i = 0; i < payload->fields.size(); ++i) {
        copy->fields[i] = snapshot_value_inner(payload->fields[i], state);
        if (!copy->fields[i]) {
            for (value_t *field : copy->fields)
                freeval(field);
            return nullptr;
        }
    }
    return newstructval(def, std::move(copy));
}

value_t *snapshot_value_inner(value_t *value, snapshot_state &state) {
    if (!value)
        return nullptr;
    if (is_ref_value(value)) {
        ref_object *raw = ref_target(value);
        if (!raw || !raw->payload)
            return newnull();
        if (!state.seen.insert(raw).second) {
            yyerror("cannot snapshot a cyclic ref graph");
            return nullptr;
        }
        value_t *result = snapshot_struct(raw->def, raw->payload, state);
        state.seen.erase(raw);
        return result;
    }
    if (is_weak_value(value)) {
        value_t *strong = ref_upgrade(value);
        if (!strong)
            return nullptr;
        value_t *result = snapshot_value_inner(strong, state);
        freeval(strong);
        return result;
    }
    switch (value->type) {
        case T_struct: {
            struct struct_value_t *sv = (struct struct_value_t *)value;
            return snapshot_struct(sv->def, sv->payload, state);
        }
        case T_enum: {
            struct enum_value_t *ev = (struct enum_value_t *)value;
            if (!ev->def || !ev->payload) {
                yyerror("invalid enum value");
                return nullptr;
            }
            auto payload = std::make_shared<struct_payload>();
            payload->fields.resize(ev->payload->fields.size(), nullptr);
            for (std::size_t i = 0; i < ev->payload->fields.size(); ++i) {
                payload->fields[i] =
                    snapshot_value_inner(ev->payload->fields[i], state);
                if (!payload->fields[i]) {
                    for (value_t *field : payload->fields)
                        freeval(field);
                    return nullptr;
                }
            }
            return newenumval(ev->def, ev->variant, std::move(payload));
        }
        case T_list: {
            struct list_value_t *src = (struct list_value_t *)value;
            struct list_value_t *dst = newlistval();
            if (!src->payload)
                return (value_t *)dst;
            dst->payload->frozen = src->payload->frozen;
            for (value_t *item : src->payload->items) {
                value_t *copy = snapshot_value_inner(item, state);
                if (!copy) {
                    freeval((value_t *)dst);
                    return nullptr;
                }
                dst->payload->items.push_back(copy);
            }
            return (value_t *)dst;
        }
        case T_dict: {
            struct dict_value_t *src = (struct dict_value_t *)value;
            struct dict_value_t *dst = newdictval();
            dst->payload->is_namespace = src->payload->is_namespace;
            if (!src->payload)
                return (value_t *)dst;
            for (const auto &entry : src->payload->items) {
                value_t *key = snapshot_value_inner(entry.first, state);
                value_t *item = snapshot_value_inner(entry.second, state);
                if (!key || !item) {
                    freeval(key);
                    freeval(item);
                    freeval((value_t *)dst);
                    return nullptr;
                }
                dict_append_item(dst, key, item);
            }
            return (value_t *)dst;
        }
        case T_result: {
            struct result_value_t *rv = (struct result_value_t *)value;
            if (rv->ok) {
                value_t *inner = snapshot_value_inner(rv->value, state);
                if (!inner)
                    return nullptr;
                return newresultok(inner);
            }
            value_t *inner = snapshot_value_inner(rv->error, state);
            if (!inner)
                return nullptr;
            return newresulterr(inner);
        }
        default:
            return dupval(value);
    }
}

} // namespace

const struct native_type *ref_native_type() { return &kRefType; }
const struct native_type *weak_native_type() { return &kWeakType; }

bool is_ref_value(const struct value_t *value) {
    if (!value || value->type != T_userdata)
        return false;
    const struct userdata_value_t *u = (const struct userdata_value_t *)value;
    return u->meta == &kRefType;
}

bool is_weak_value(const struct value_t *value) {
    if (!value || value->type != T_userdata)
        return false;
    const struct userdata_value_t *u = (const struct userdata_value_t *)value;
    return u->meta == &kWeakType;
}

bool is_ref_like_value(const struct value_t *value) {
    return is_ref_value(value) || is_weak_value(value);
}

const char *ref_transfer_guidance() {
    return "use std.ref.snapshot() to send a value copy, or send an "
           "std.arena id instead";
}

ref_object *ref_target(struct value_t *value) {
    if (!is_ref_value(value))
        return nullptr;
    struct userdata_value_t *u = (struct userdata_value_t *)value;
    ref_handle *handle = static_cast<ref_handle *>(u->data.get());
    return handle && handle->object && !handle->object->swept
               ? handle->object.get()
               : nullptr;
}

struct_def *ref_value_def(const struct value_t *value) {
    if (!value || value->type != T_userdata)
        return nullptr;
    const struct userdata_value_t *u = (const struct userdata_value_t *)value;
    if (u->meta == &kRefType) {
        const ref_handle *handle = static_cast<const ref_handle *>(u->data.get());
        return handle && handle->object ? handle->object->def : nullptr;
    }
    if (u->meta == &kWeakType) {
        const weak_handle *handle = static_cast<const weak_handle *>(u->data.get());
        return handle ? handle->def : nullptr;
    }
    return nullptr;
}

struct value_t *new_ref_value(struct_def *def,
                              const std::shared_ptr<struct_payload> &payload) {
    if (!def) {
        yyerror("invalid ref type");
        return nullptr;
    }
    auto object = std::make_shared<ref_object>();
    object->id = tls_refs.next_id++;
    object->generation = 1;
    object->def = def;
    object->payload = payload ? payload : std::make_shared<struct_payload>();
    if (object->payload->fields.size() < def->fields.size())
        object->payload->fields.resize(def->fields.size(), nullptr);
    for (std::size_t i = 0; i < object->payload->fields.size() &&
                           i < def->fields.size(); ++i) {
        if (def->fields[i].type.kind != TYPE_WEAK)
            continue;
        value_t *field = object->payload->fields[i];
        if (!is_ref_value(field))
            continue;
        value_t *weak = new_weak_from_ref(field);
        freeval(field);
        object->payload->fields[i] = weak;
    }
    tls_refs.objects.push_back(object);
    gc_note_ref_allocation();
    return ref_value_from_object(object);
}

struct value_t *new_weak_from_ref(struct value_t *value) {
    if (!value)
        return nullptr;
    if (is_weak_value(value))
        return dupval(value);
    if (!is_ref_value(value)) {
        yyerror("weak reference expects a ref value, got %s",
                typename_of(value));
        return nullptr;
    }
    struct userdata_value_t *u = (struct userdata_value_t *)value;
    ref_handle *handle = static_cast<ref_handle *>(u->data.get());
    if (!handle || !handle->object) {
        yyerror("invalid ref value");
        return nullptr;
    }
    auto weak = std::make_shared<weak_handle>();
    weak->id = handle->object->id;
    weak->generation = handle->object->generation;
    weak->def = handle->object->def;
    weak->object = handle->object;
    return newuserdataval(weak_native_type(), std::move(weak));
}

struct value_t *ref_upgrade(struct value_t *value) {
    if (!value)
        return nullptr;
    if (!is_weak_value(value))
        return dupval(value);
    weak_handle *handle =
        static_cast<weak_handle *>(((struct userdata_value_t *)value)->data.get());
    if (!handle)
        return newnull();
    std::shared_ptr<ref_object> object = handle->object.lock();
    if (!object || object->id != handle->id ||
        object->generation != handle->generation || object->swept)
        return newnull();
    return ref_value_from_object(object);
}

struct value_t *ref_snapshot(struct value_t *value) {
    snapshot_state state;
    return snapshot_value_inner(value, state);
}

bool ref_replace_target(struct value_t *ref, struct value_t *value) {
    ref_object *object = ref_target(ref);
    if (!object || !object->def) {
        yyerror("ref_replace_target expects a live ref value");
        return false;
    }
    value_t *owned = nullptr;
    if (is_ref_like_value(value)) {
        owned = ref_snapshot(value);
        if (!owned)
            return false;
        value = owned;
    }
    if (!value || value->type != T_struct) {
        yyerror("ref target replacement expects a struct value, got %s",
                value ? typename_of(value) : "null");
        freeval(owned);
        return false;
    }
    struct struct_value_t *source = (struct struct_value_t *)value;
    if (source->def != object->def) {
        yyerror("ref target replacement expects %s, got %s",
                object->def->name.c_str(),
                source->def ? source->def->name.c_str() : "struct");
        freeval(owned);
        return false;
    }
    auto payload = std::make_shared<struct_payload>();
    payload->fields.resize(object->def->fields.size(), nullptr);
    for (std::size_t i = 0; i < payload->fields.size(); ++i) {
        const type_constraint &type = object->def->fields[i].type;
        value_t *field = i < source->payload->fields.size()
                             ? source->payload->fields[i]
                             : nullptr;
        value_t *stored =
            field ? coerce_to_ref_type(&type, field) : newnull();
        if (!stored || !type_constraint_matches(&type, stored)) {
            yyerror("field %zu of %s expects %s", i,
                    object->def->name.c_str(), type_constraint_name(&type));
            freeval(stored);
            freeval(owned);
            return false;
        }
        payload->fields[i] = stored;
    }
    object->payload = std::move(payload);
    freeval(owned);
    return true;
}

struct value_t *coerce_to_ref_type(const struct type_constraint *type,
                                   struct value_t *value) {
    if (!type || !value)
        return NULL;
    if (type->kind == TYPE_WEAK && is_ref_value(value))
        return new_weak_from_ref(value);
    return dupval(value);
}

void gc_mark_ref_value(struct value_t *value, uint64_t epoch,
                       const std::function<void(struct value_t *)> &mark) {
    if (!is_ref_value(value))
        return;
    ref_object *object = ref_target(value);
    if (!object)
        return;
    object->gc_epoch = epoch;
    if (!object->payload)
        return;
    std::vector<value_t *> fields = object->payload->fields;
    for (value_t *field : fields)
        mark(field);
}

void gc_sweep_unmarked_refs(uint64_t epoch) {
    std::vector<value_t *> doomed;
    for (const auto &weak : tls_refs.objects) {
        std::shared_ptr<ref_object> object = weak.lock();
        if (!object || object->swept || object->gc_epoch == epoch)
            continue;
        object->swept = true;
        ++object->generation;
        if (!object->payload)
            continue;
        for (value_t *&field : object->payload->fields) {
            if (field)
                doomed.push_back(field);
            field = nullptr;
        }
    }
    for (value_t *value : doomed)
        freeval(value);
    tls_refs.objects.erase(
        std::remove_if(tls_refs.objects.begin(), tls_refs.objects.end(),
                       [](const std::weak_ptr<ref_object> &weak) {
                           return weak.expired();
                       }),
        tls_refs.objects.end());
}

void gc_note_ref_allocation() { ++tls_refs.allocations_since_collect; }

bool gc_ref_allocation_due(std::size_t threshold) {
    return tls_refs.allocations_since_collect >= threshold;
}

void gc_reset_ref_allocation_counter() {
    tls_refs.allocations_since_collect = 0;
}

void ref_shutdown() {
    for (const auto &weak : tls_refs.objects) {
        std::shared_ptr<ref_object> object = weak.lock();
        if (!object || object->swept)
            continue;
        object->swept = true;
        ++object->generation;
        if (!object->payload)
            continue;
        for (value_t *&field : object->payload->fields) {
            freeval(field);
            field = nullptr;
        }
    }
    tls_refs.objects.clear();
}
