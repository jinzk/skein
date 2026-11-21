/* Dynamic test module for the native module ABI. */
#include "skein_module.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {

const skein_api *g_api = nullptr;

skein_status twice_fn(skein_context *ctx, const skein_value *const *args,
                      size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "twice expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "twice expects an integer");
    *result = g_api->new_int(ctx, value * 2);
    return SKEIN_OK;
}

struct counter {
    int64_t value = 0;
};

void counter_destroy(void *data) { delete static_cast<counter *>(data); }

const skein_native_type counter_type = {
    "Counter", nullptr, counter_destroy, nullptr, nullptr, nullptr};

skein_status counter_new(skein_context *ctx, const skein_value *const *args,
                         size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "counter_new expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "counter_new expects an integer");
    counter *data = new counter();
    data->value = value;
    *result = g_api->new_userdata(ctx, &counter_type, data);
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status counter_value(skein_context *ctx, const skein_value *const *args,
                           size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "counter_value expects 1 argument");
    void *data = nullptr;
    if (!g_api->as_userdata(args[0], &counter_type, &data))
        return g_api->error(ctx, "type", "counter_value expects a Counter");
    *result = g_api->new_int(ctx, static_cast<counter *>(data)->value);
    return SKEIN_OK;
}

skein_status list_pair(skein_context *ctx, const skein_value *const *args,
                       size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "list_pair expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "list_pair expects an integer");
    skein_value *list = g_api->new_list(ctx);
    if (!list)
        return SKEIN_ERROR;
    if (!g_api->list_push(ctx, list, g_api->new_int(ctx, value)) ||
        !g_api->list_push(ctx, list, g_api->new_int(ctx, value * 2))) {
        g_api->free_value(ctx, list);
        return SKEIN_ERROR;
    }
    *result = list;
    return SKEIN_OK;
}

skein_status dict_value(skein_context *ctx, const skein_value *const *args,
                        size_t argc, skein_value **result) {
    if (argc != 1)
        return g_api->error(ctx, "type", "dict_value expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "dict_value expects an integer");
    skein_value *dict = g_api->new_dict(ctx);
    if (!dict)
        return SKEIN_ERROR;
    if (!g_api->dict_set(ctx, dict, "value", g_api->new_int(ctx, value))) {
        g_api->free_value(ctx, dict);
        return SKEIN_ERROR;
    }
    *result = dict;
    return SKEIN_OK;
}

bool has_string_len() {
    return g_api &&
           g_api->size >=
               offsetof(skein_api, new_string_len) +
                   sizeof(g_api->new_string_len);
}

skein_status string_len_fn(skein_context *ctx, const skein_value *const *args,
                           size_t argc, skein_value **result) {
    (void)args;
    (void)argc;
    if (!has_string_len())
        return g_api->error(ctx, "type", "new_string_len is unavailable");
    /* Buffer without a trailing NUL: new_string() could not safely read it. */
    const char data[] = {'h', 'e', 'l', 'l', 'o'};
    *result = g_api->new_string_len(ctx, data, sizeof(data));
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

struct async_job {
    int64_t input = 0;
    int64_t output = 0;
    std::atomic<skein_value *> task{nullptr};
    skein_context *ctx = nullptr;
    bool cancellable = false;
};

bool has_async_api() {
    return g_api &&
           g_api->size >=
               offsetof(skein_api, task_cancelled) +
                   sizeof(g_api->task_cancelled);
}

void async_work(void *data) {
    async_job *job = static_cast<async_job *>(data);
    if (job->cancellable) {
        skein_value *task = job->task.load();
        while (!task) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            task = job->task.load();
        }
        while (!g_api->task_cancelled(job->ctx, task))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    job->output = job->input * 2;
}

skein_value *async_finish(skein_context *ctx, void *data) {
    async_job *job = static_cast<async_job *>(data);
    return g_api->new_int(ctx, job->output);
}

void async_destroy(void *data) {
    async_job *job = static_cast<async_job *>(data);
    skein_value *task = job->task.load();
    if (task)
        g_api->free_value(job->ctx, task);
    delete job;
}

skein_status twice_async_fn(skein_context *ctx,
                            const skein_value *const *args, size_t argc,
                            skein_value **result) {
    if (!has_async_api())
        return g_api->error(ctx, "type", "native async API is unavailable");
    if (argc != 1)
        return g_api->error(ctx, "type", "twice_async expects 1 argument");
    int64_t value = 0;
    if (!g_api->as_int(args[0], &value))
        return g_api->error(ctx, "type", "twice_async expects an integer");
    async_job *job = new async_job();
    job->input = value;
    job->ctx = ctx;
    *result = g_api->call_task(ctx, async_work, async_finish, async_destroy,
                               job);
    if (!*result) {
        delete job;
        return SKEIN_ERROR;
    }
    job->task = g_api->dup_value(ctx, *result);
    return SKEIN_OK;
}

skein_status cancellable_fn(skein_context *ctx,
                            const skein_value *const *args, size_t argc,
                            skein_value **result) {
    (void)args;
    (void)argc;
    if (!has_async_api())
        return g_api->error(ctx, "type", "native async API is unavailable");
    async_job *job = new async_job();
    job->cancellable = true;
    job->ctx = ctx;
    *result = g_api->call_task(ctx, async_work, async_finish, async_destroy,
                               job);
    if (!*result) {
        delete job;
        return SKEIN_ERROR;
    }
    job->task = g_api->dup_value(ctx, *result);
    return SKEIN_OK;
}

bool has_post_api() {
    return g_api &&
           g_api->size >=
               offsetof(skein_api, cancel_post_callback) +
                   sizeof(g_api->cancel_post_callback);
}

struct post_data {
    skein_context *ctx = nullptr;
    skein_value *callback = nullptr;
    skein_value *arg = nullptr;
    std::atomic<skein_post_id> id{0};
};

void post_work(void *data) {
    post_data *job = static_cast<post_data *>(data);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const skein_value *args[] = {job->arg};
    job->id.store(g_api->post_callback(job->ctx, job->callback, args, 1));
}

skein_value *post_finish(skein_context *ctx, void *data) {
    post_data *job = static_cast<post_data *>(data);
    return g_api->new_int(ctx, static_cast<int64_t>(job->id.load()));
}

void post_destroy(void *data) {
    post_data *job = static_cast<post_data *>(data);
    if (job->callback)
        g_api->free_value(job->ctx, job->callback);
    if (job->arg)
        g_api->free_value(job->ctx, job->arg);
    delete job;
}

skein_status post_events_fn(skein_context *ctx,
                            const skein_value *const *args, size_t argc,
                            skein_value **result) {
    if (!has_post_api())
        return g_api->error(ctx, "type", "native post API is unavailable");
    if (argc != 2)
        return g_api->error(ctx, "type", "post_events expects 2 arguments");
    post_data *job = new post_data();
    job->ctx = ctx;
    job->callback = g_api->dup_value(ctx, args[0]);
    job->arg = g_api->dup_value(ctx, args[1]);
    *result = g_api->call_task(ctx, post_work, post_finish, post_destroy, job);
    if (!*result) {
        post_destroy(job);
        return SKEIN_ERROR;
    }
    return SKEIN_OK;
}

skein_status post_now_fn(skein_context *ctx, const skein_value *const *args,
                         size_t argc, skein_value **result) {
    if (!has_post_api())
        return g_api->error(ctx, "type", "native post API is unavailable");
    if (argc != 2)
        return g_api->error(ctx, "type", "post_now expects 2 arguments");
    skein_post_id id = g_api->post_callback(ctx, args[0], &args[1], 1);
    *result = g_api->new_int(ctx, static_cast<int64_t>(id));
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

skein_status cancel_post_fn(skein_context *ctx,
                            const skein_value *const *args, size_t argc,
                            skein_value **result) {
    if (!has_post_api())
        return g_api->error(ctx, "type", "native post API is unavailable");
    int64_t id = 0;
    if (argc != 1 || !g_api->as_int(args[0], &id))
        return g_api->error(ctx, "type", "cancel_post expects an id");
    *result = g_api->new_bool(ctx,
                              g_api->cancel_post_callback(
                                  ctx, static_cast<skein_post_id>(id)));
    return *result ? SKEIN_OK : SKEIN_ERROR;
}

const skein_function functions[] = {
    {"twice", twice_fn},
    {"counter_new", counter_new},
    {"counter_value", counter_value},
    {"list_pair", list_pair},
    {"dict_value", dict_value},
    {"string_len", string_len_fn},
    {"twice_async", twice_async_fn},
    {"cancellable", cancellable_fn},
    {"post_events", post_events_fn},
    {"post_now", post_now_fn},
    {"cancel_post", cancel_post_fn},
};

const skein_native_type *const types[] = {&counter_type};

skein_status module_init(skein_context *ctx, const skein_api *api) {
    (void)ctx;
    g_api = api;
    return SKEIN_OK;
}

void module_fini(skein_context *ctx) {
    (void)ctx;
    const char *path = std::getenv("SKEIN_TEST_FINI_FILE");
    if (!path || !*path)
        return;
    FILE *file = std::fopen(path, "w");
    if (!file)
        return;
    std::fputs("fini", file);
    std::fclose(file);
}

const skein_module module = {
    SKEIN_MODULE_ABI_VERSION,
    "test.dynamic",
    module_init,
    module_fini,
    functions,
    11,
    types,
    1,
    nullptr,
    0,
};

} // namespace

SKEIN_MODULE_EXPORT const skein_module *skein_module_descriptor(void) {
    return &module;
}
