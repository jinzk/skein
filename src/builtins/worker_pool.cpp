#include "worker_pool.h"

#include "ast.h"
#include "builtins_io.h"
#include "fiber.h"
#include "json.h"
#include "platform.h"
#include "process_io.h"
#include "process_spawn.h"
#include "task.h"
#include "valuetype.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct worker_pool;
struct worker_job;

struct worker_slot {
    value_t *process = nullptr;
    process_io io;
    std::shared_ptr<worker_job> job;
    bool busy = false;
    /* Set while a fiber is blocked reading this slot's response. */
    bool completing = false;
    int failures = 0;
};

struct worker_job : std::enable_shared_from_this<worker_job> {
    std::weak_ptr<worker_pool> pool;
    long long id = 0;
    std::string request;
    int slot = -1;
    bool done = false;
    bool binary = false;
    value_t *result = nullptr;

    ~worker_job() {
        freeval(result);
        result = nullptr;
    }
};

void close_worker(worker_slot &slot, bool kill_if_running,
                  bool force = false);

struct worker_pool : std::enable_shared_from_this<worker_pool> {
    std::vector<worker_slot> workers;
    std::vector<std::shared_ptr<worker_job>> queue;
    std::string script;
    bool hybrid = false;
    long long next_id = 1;
    long long queue_limit = 1024;
    long long restart_limit = 3;
    long long ready_timeout_ms = 5000;
    long long restarts = 0;

    ~worker_pool() {
        for (worker_slot &worker : workers)
            close_worker(worker, true, true);
    }
};

value_t *worker_pool_get_field(void *data, const char *name);
value_t *worker_job_get_field(void *data, const char *name);

const native_type kWorkerPoolType = {
    "worker_pool",
    [](const void *data) {
        const worker_pool *pool = static_cast<const worker_pool *>(data);
        if (!pool)
            return std::string("<worker_pool>");
        return "<worker_pool: " + std::to_string(pool->workers.size()) +
               " workers>";
    },
    nullptr,
    worker_pool_get_field,
    nullptr,
    nullptr,
};

value_t *new_pool_value(std::shared_ptr<worker_pool> pool) {
    userdata_value_t *value = new userdata_value_t;
    value->type = T_userdata;
    value->meta = &kWorkerPoolType;
    value->data = std::move(pool);
    return (value_t *)value;
}

const native_type kWorkerJobType = {
    "worker_job",
    [](const void *data) {
        const worker_job *job = static_cast<const worker_job *>(data);
        if (!job)
            return std::string("<worker_job>");
        return "<worker_job: " + std::to_string(job->id) +
               (job->done ? " done>" : ">");
    },
    nullptr,
    worker_job_get_field,
    nullptr,
    nullptr,
};

value_t *new_job_value(std::shared_ptr<worker_job> job) {
    userdata_value_t *value = new userdata_value_t;
    value->type = T_userdata;
    value->meta = &kWorkerJobType;
    value->data = std::move(job);
    return (value_t *)value;
}

value_t *pool_builtin_call(const std::shared_ptr<worker_pool> &pool,
                           int functype,
                           const std::vector<value_t *> &args) {
    std::vector<value_t *> call_args;
    call_args.reserve(args.size() + 1);
    call_args.push_back(new_pool_value(pool));
    for (value_t *arg : args)
        call_args.push_back(dupval(arg));
    value_t *result = call_worker_builtin(functype, call_args);
    for (value_t *arg : call_args)
        freeval(arg);
    return result;
}

value_t *job_builtin_call(const std::shared_ptr<worker_job> &job,
                          int functype,
                          const std::vector<value_t *> &args) {
    std::vector<value_t *> call_args;
    call_args.reserve(args.size() + 1);
    call_args.push_back(new_job_value(job));
    for (value_t *arg : args)
        call_args.push_back(dupval(arg));
    value_t *result = call_worker_builtin(functype, call_args);
    for (value_t *arg : call_args)
        freeval(arg);
    return result;
}

value_t *worker_pool_get_field(void *data, const char *name) {
    worker_pool *raw = static_cast<worker_pool *>(data);
    if (!raw || !name) {
        yyerror("worker_pool has no field");
        return nullptr;
    }
    auto pool = raw->shared_from_this();
    if (std::strcmp(name, "call") == 0) {
        return newnativeval([pool](const std::vector<value_t *> &args) {
            if (args.size() < 2 || args.size() > 3) {
                yyerror("worker_pool.call expects method, args[, timeout_ms]");
                return (value_t *)nullptr;
            }
            return pool_builtin_call(pool, B_worker_call, args);
        }, pool);
    }
    if (std::strcmp(name, "call_bytes") == 0) {
        return newnativeval([pool](const std::vector<value_t *> &args) {
            if (args.size() < 3 || args.size() > 4) {
                yyerror("worker_pool.call_bytes expects method, args, body[, timeout_ms]");
                return (value_t *)nullptr;
            }
            return pool_builtin_call(pool, B_worker_call_bytes, args);
        }, pool);
    }
    if (std::strcmp(name, "submit") == 0) {
        return newnativeval([pool](const std::vector<value_t *> &args) {
            if (args.size() != 2) {
                yyerror("worker_pool.submit expects method and args");
                return (value_t *)nullptr;
            }
            return pool_builtin_call(pool, B_worker_submit, args);
        }, pool);
    }
    if (std::strcmp(name, "submit_bytes") == 0) {
        return newnativeval([pool](const std::vector<value_t *> &args) {
            if (args.size() != 3) {
                yyerror("worker_pool.submit_bytes expects method, args and body");
                return (value_t *)nullptr;
            }
            return pool_builtin_call(pool, B_worker_submit_bytes, args);
        }, pool);
    }
    if (std::strcmp(name, "map") == 0) {
        return newnativeval([pool](const std::vector<value_t *> &args) {
            if (args.size() < 2 || args.size() > 3) {
                yyerror("worker_pool.map expects method, items[, timeout_ms]");
                return (value_t *)nullptr;
            }
            return pool_builtin_call(pool, B_worker_map, args);
        }, pool);
    }
    if (std::strcmp(name, "shutdown") == 0) {
        return newnativeval([pool](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("worker_pool.shutdown expects no arguments");
                return (value_t *)nullptr;
            }
            return pool_builtin_call(pool, B_worker_shutdown, args);
        }, pool);
    }
    if (std::strcmp(name, "stats") == 0) {
        return newnativeval([pool](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("worker_pool.stats expects no arguments");
                return (value_t *)nullptr;
            }
            return pool_builtin_call(pool, B_worker_stats, args);
        }, pool);
    }
    yyerror("worker_pool has no field %s", name);
    return nullptr;
}

value_t *worker_job_get_field(void *data, const char *name) {
    worker_job *raw = static_cast<worker_job *>(data);
    if (!raw || !name) {
        yyerror("worker_job has no field");
        return nullptr;
    }
    auto job = raw->shared_from_this();
    if (std::strcmp(name, "wait") == 0) {
        return newnativeval([job](const std::vector<value_t *> &args) {
            if (args.size() > 1) {
                yyerror("worker_job.wait expects at most one timeout");
                return (value_t *)nullptr;
            }
            return job_builtin_call(job, B_worker_wait, args);
        }, job);
    }
    if (std::strcmp(name, "id") == 0) {
        return newnativeval([job](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("worker_job.id expects no arguments");
                return (value_t *)nullptr;
            }
            return newintval(job->id);
        }, job);
    }
    if (std::strcmp(name, "done") == 0) {
        return newnativeval([job](const std::vector<value_t *> &args) {
            if (!args.empty()) {
                yyerror("worker_job.done expects no arguments");
                return (value_t *)nullptr;
            }
            return newboolval(job->done);
        }, job);
    }
    yyerror("worker_job has no field %s", name);
    return nullptr;
}

bool get_job(value_t *value, std::shared_ptr<worker_job> &job) {
    if (!value || value->type != T_userdata)
        return false;
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (userdata->meta != &kWorkerJobType)
        return false;
    job = std::static_pointer_cast<worker_job>(userdata->data);
    return true;
}

bool get_pool(value_t *value, std::shared_ptr<worker_pool> &pool) {
    if (!value || value->type != T_userdata)
        return false;
    userdata_value_t *userdata = (userdata_value_t *)value;
    if (userdata->meta != &kWorkerPoolType)
        return false;
    pool = std::static_pointer_cast<worker_pool>(userdata->data);
    return true;
}

value_t *dict_get(value_t *dict, const char *key) {
    if (!dict || dict->type != T_dict)
        return nullptr;
    dict_value_t *value = (dict_value_t *)dict;
    for (const auto &entry : value->payload->items) {
        if (entry.first->type == T_string &&
            std::strcmp(((string_value_t *)entry.first)->value, key) == 0)
            return entry.second;
    }
    return nullptr;
}

value_t *json_stringify(value_t *value) {
    return call_json_builtin(B_json_stringify, {value});
}

value_t *json_parse(value_t *value) {
    return call_json_builtin(B_json_parse, {value});
}

bool spawn_worker(worker_slot &slot, const std::string &script,
                  long long ready_timeout_ms, bool hybrid,
                  std::string &error) {
    const std::string executable = platform_executable_path();
    if (executable.empty()) {
        error = "cannot locate the skein executable";
        return false;
    }

    list_value_t *args = newlistval();
    args->payload->items.push_back(newstrval(script.c_str()));
    dict_value_t *options = newdictval();
    options->payload->items.push_back(std::make_pair(
        newstrval("args"), (value_t *)args));
    options->payload->items.push_back(std::make_pair(
        newstrval("stderr"), newstrval("inherit")));

    value_t *executable_value = newstrval(executable.c_str());
    value_t *process = call_process_spawn_builtin(
        B_process_spawn_command, {executable_value, (value_t *)options});
    freeval(executable_value);
    freeval((value_t *)options);
    if (!process) {
        error = "cannot spawn worker";
        return false;
    }
    slot.process = process;
    if (!slot.io.attach(process, error)) {
        close_worker(slot, true);
        return false;
    }
    /* Reserve the slot while the handshake runs so no fiber can dispatch. */
    slot.busy = true;
    slot.completing = true;

    value_t *ready_line =
        slot.io.read_line(ready_timeout_ms, error);
    if (!ready_line) {
        close_worker(slot, true);
        if (error.empty())
            error = "worker did not send a ready frame";
        return false;
    }
    value_t *ready = json_parse(ready_line);
    freeval(ready_line);
    value_t *ready_flag = dict_get(ready, "ready");
    value_t *version = dict_get(ready, "version");
    value_t *protocol = dict_get(ready, "protocol");
    const bool protocol_ok =
        hybrid ? (protocol && protocol->type == T_string &&
                  std::strcmp(((string_value_t *)protocol)->value,
                              "hybrid") == 0)
               : (!protocol ||
                  (protocol->type == T_string &&
                   std::strcmp(((string_value_t *)protocol)->value,
                               "jsonl") == 0));
    if (!ready || !ready_flag || ready_flag->type != T_bool ||
        !((bool_value_t *)ready_flag)->value || !version ||
        version->type != T_int ||
        ((int_value_t *)version)->value != 1 || !protocol_ok) {
        freeval(ready);
        close_worker(slot, true);
        error = "worker ready frame is invalid";
        return false;
    }
    freeval(ready);
    slot.busy = false;
    slot.completing = false;
    return true;
}

void close_worker(worker_slot &slot, bool kill_if_running, bool force) {
    slot.io.close_streams();
    if (slot.process) {
        if (!force) {
            value_t *timeout = newintval(1000);
            value_t *wait_result = call_process_spawn_builtin(
                B_process_wait, {slot.process, timeout});
            freeval(timeout);
            if (wait_result)
                freeval(wait_result);
        }
        if (kill_if_running) {
            std::vector<value_t *> kill_args{slot.process};
            if (force)
                kill_args.push_back(newintval(9));
            value_t *kill_result = call_process_spawn_builtin(
                B_process_kill, kill_args);
            if (kill_args.size() > 1)
                freeval(kill_args[1]);
            if (kill_result)
                freeval(kill_result);
        }
        value_t *close_result = call_process_spawn_builtin(
            B_process_close, {slot.process});
        if (close_result)
            freeval(close_result);
        freeval(slot.process);
        slot.process = nullptr;
    }
    slot.job.reset();
    slot.busy = false;
    slot.completing = false;
}

bool worker_usable(const worker_slot &slot) {
    return slot.process && slot.io.usable();
}

bool restart_worker(worker_pool *pool, worker_slot &slot,
                    std::string &error) {
    /* Keep the slot reserved for the whole restart, including the pumps that
       process wait/spawn can perform. */
    slot.busy = true;
    slot.completing = true;
    close_worker(slot, true);
    if (slot.failures >= pool->restart_limit) {
        slot.busy = false;
        slot.completing = false;
        error = "worker restart limit exceeded";
        return false;
    }
    ++slot.failures;
    slot.busy = true;
    slot.completing = true;
    if (!pool->script.empty() &&
        spawn_worker(slot, pool->script, pool->ready_timeout_ms,
                     pool->hybrid, error)) {
        ++pool->restarts;
        return true;
    }
    slot.busy = false;
    slot.completing = false;
    error = "cannot restart worker";
    return false;
}

bool make_request(long long id, const std::string &method, value_t *params,
                  const std::string *body, bool hybrid,
                  std::string &line) {
    if (body && !hybrid)
        return false;
    dict_value_t *request = newdictval();
    request->payload->items.push_back(std::make_pair(
        newstrval("id"), newintval(id)));
    request->payload->items.push_back(std::make_pair(
        newstrval("method"), newstrval(method.c_str())));
    request->payload->items.push_back(std::make_pair(
        newstrval("params"), dupval(params)));
    if (hybrid) {
        request->payload->items.push_back(std::make_pair(
            newstrval("body_len"),
            newintval(body ? (long long)body->size() : 0)));
    }
    value_t *json = json_stringify((value_t *)request);
    freeval((value_t *)request);
    if (!json)
        return false;
    line = ((string_value_t *)json)->value;
    line.push_back('\n');
    if (body)
        line.append(*body);
    freeval(json);
    return true;
}

bool read_response(process_io &pipe, long long expected_id,
                   long long timeout_ms, bool binary, value_t *&result,
                   std::string &transport_error) {
    value_t *line = pipe.read_line(timeout_ms, transport_error);
    if (!line)
        return false;
    if (line->type == T_null) {
        freeval(line);
        transport_error = "worker closed its stdout";
        return false;
    }
    value_t *response = json_parse(line);
    freeval(line);
    if (!response) {
        transport_error = "invalid worker response";
        return false;
    }
    value_t *id = dict_get(response, "id");
    value_t *ok = dict_get(response, "ok");
    if (!id || id->type != T_int ||
        ((int_value_t *)id)->value != expected_id ||
        !ok || ok->type != T_bool) {
        freeval(response);
        transport_error = "invalid worker response";
        return false;
    }

    size_t body_size = 0;
    value_t *body_len = dict_get(response, "body_len");
    if (body_len) {
        if (body_len->type != T_int ||
            ((int_value_t *)body_len)->value < 0) {
            freeval(response);
            transport_error = "invalid worker body length";
            return false;
        }
        body_size = (size_t)((int_value_t *)body_len)->value;
    }
    if (body_size > 256u * 1024u * 1024u) {
        freeval(response);
        transport_error = "worker body is too large";
        return false;
    }
    std::string body;
    if (body_size > 0) {
        value_t *body_value =
            pipe.read_exact(body_size, timeout_ms, transport_error);
        if (!body_value) {
            freeval(response);
            return false;
        }
        body = ((bytes_value_t *)body_value)->value;
        freeval(body_value);
    }

    const bool ok_value = ((bool_value_t *)ok)->value;
    if (ok_value) {
        value_t *value = dict_get(response, "result");
        if (binary) {
            dict_value_t *combined = newdictval();
            combined->payload->items.push_back(std::make_pair(
                newstrval("value"), value ? dupval(value) : newnull()));
            combined->payload->items.push_back(std::make_pair(
                newstrval("body"), newbytesval(body)));
            result = newresultok((value_t *)combined);
        } else {
            if (!body.empty()) {
                freeval(response);
                transport_error = "unexpected worker body";
                return false;
            }
            result = newresultok(value ? dupval(value) : newnull());
        }
        freeval(response);
        return true;
    }
    if (!body.empty()) {
        freeval(response);
        transport_error = "unexpected worker error body";
        return false;
    }
    value_t *error = dict_get(response, "error");
    std::string kind = "worker";
    std::string message = "worker failed";
    if (error && error->type == T_dict) {
        value_t *kind_value = dict_get(error, "kind");
        value_t *message_value = dict_get(error, "message");
        if (kind_value && kind_value->type == T_string)
            kind = ((string_value_t *)kind_value)->value;
        if (message_value && message_value->type == T_string)
            message = ((string_value_t *)message_value)->value;
    }
    freeval(response);
    result = newresulterr_message(kind, message);
    return true;
}

void fail_job(const std::shared_ptr<worker_job> &job,
              const std::string &message) {
    if (!job || job->done)
        return;
    freeval(job->result);
    job->result = newresulterr_message("worker", message);
    job->done = true;
    job->slot = -1;
}

bool complete_slot(worker_pool *pool, size_t index, long long timeout_ms) {
    worker_slot &slot = pool->workers[index];
    std::shared_ptr<worker_job> job = slot.job;
    if (!job || slot.completing)
        return false;
    slot.completing = true;
    value_t *result = nullptr;
    std::string transport_error;
    const bool ok = read_response(slot.io, job->id,
                                  timeout_ms, job->binary, result,
                                  transport_error);
    slot.completing = false;
    slot.job.reset();
    job->slot = -1;
    if (!ok) {
        std::string restart_error;
        restart_worker(pool, slot, restart_error);
        fail_job(job, transport_error.empty() ? "worker read failed"
                                              : transport_error);
        return true;
    }
    slot.busy = false;
    if (result && result->type == T_result &&
        ((result_value_t *)result)->ok) {
        slot.failures = 0;
    }
    job->result = result;
    job->done = true;
    return true;
}

void try_dispatch(worker_pool *pool) {
    while (!pool->queue.empty()) {
        size_t index = pool->workers.size();
        for (size_t i = 0; i < pool->workers.size(); ++i) {
            if (!pool->workers[i].busy) {
                index = i;
                break;
            }
        }
        if (index == pool->workers.size())
            return;
        worker_slot &slot = pool->workers[index];
        if (!worker_usable(slot)) {
            std::string error;
            if (!restart_worker(pool, slot, error)) {
                std::shared_ptr<worker_job> job = pool->queue.front();
                pool->queue.erase(pool->queue.begin());
                fail_job(job, error);
                continue;
            }
        }
        std::shared_ptr<worker_job> job = pool->queue.front();
        pool->queue.erase(pool->queue.begin());
        job->slot = (int)index;
        slot.job = job;
        slot.busy = true;
        /* Block other fibers from reading this slot until the request is out. */
        slot.completing = true;
        if (!slot.io.write(job->request) ||
            !slot.io.flush()) {
            slot.completing = false;
            slot.job.reset();
            slot.busy = false;
            job->slot = -1;
            std::string error;
            restart_worker(pool, slot, error);
            fail_job(job, "cannot send request");
            continue;
        }
        slot.completing = false;
    }
}

long long remaining_timeout(
    const std::chrono::steady_clock::time_point &start,
    long long timeout_ms) {
    if (timeout_ms <= 0)
        return 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    return timeout_ms - elapsed;
}

/* Let other fibers run while another fiber completes this worker slot.
   Use a real fiber suspension: polling task waits do not yield the native
   stack to the caller and can starve queued jobs with many concurrent
   requests. */
void worker_yield() {
    value_t *task_value = call_task_builtin(B_task_yield, {});
    if (!task_value)
        return;
    task_ptr task;
    if (get_task_value(task_value, task))
        fiber_await_task(task);
    freeval(task_value);
}

}  // namespace

void worker_pool_force_shutdown(struct value_t *value) {
    std::shared_ptr<worker_pool> pool;
    if (!get_pool(value, pool))
        return;
    for (const auto &job : pool->queue)
        fail_job(job, "worker pool shut down");
    pool->queue.clear();
    for (worker_slot &worker : pool->workers) {
        if (worker.job) {
            fail_job(worker.job, "worker pool shut down");
            worker.job.reset();
        }
        worker.busy = false;
        worker.completing = false;
        close_worker(worker, true, true);
    }
}

value_t *call_worker_builtin(
    int functype, const std::vector<value_t *> &args) {
    if (functype == B_worker_pool) {
        if ((args.size() != 2 && args.size() != 3) ||
            args[0]->type != T_int ||
            ((int_value_t *)args[0])->value <= 0 ||
            args[1]->type != T_string) {
            yyerror("std.worker.pool expects size, script and optional "
                    "options");
            return NULL;
        }
        const long long size = ((int_value_t *)args[0])->value;
        std::shared_ptr<worker_pool> pool = std::make_shared<worker_pool>();
        pool->script = ((string_value_t *)args[1])->value;
        if (args.size() == 3) {
            if (args[2]->type != T_dict) {
                yyerror("std.worker.pool options must be a dict");
                return NULL;
            }
            dict_value_t *options = (dict_value_t *)args[2];
            for (const auto &entry : options->payload->items) {
                if (entry.first->type != T_string)
                    continue;
                const std::string key =
                    ((string_value_t *)entry.first)->value;
                if (key == "protocol") {
                    if (entry.second->type != T_string) {
                        yyerror("std.worker.pool protocol must be a string");
                        return NULL;
                    }
                    const std::string protocol =
                        ((string_value_t *)entry.second)->value;
                    if (protocol == "hybrid")
                        pool->hybrid = true;
                    else if (protocol != "jsonl") {
                        yyerror("std.worker.pool: unknown protocol %s",
                                protocol.c_str());
                        return NULL;
                    }
                    continue;
                }
                if (entry.second->type != T_int ||
                    ((int_value_t *)entry.second)->value < 0) {
                    yyerror("std.worker.pool option %s must be a "
                            "non-negative integer", key.c_str());
                    return NULL;
                }
                const long long value =
                    ((int_value_t *)entry.second)->value;
                if (key == "queue_limit")
                    pool->queue_limit = value;
                else if (key == "restart_limit")
                    pool->restart_limit = value;
                else if (key == "ready_timeout_ms")
                    pool->ready_timeout_ms = value;
                else {
                    yyerror("std.worker.pool: unknown option %s",
                            key.c_str());
                    return NULL;
                }
            }
        }
        pool->workers.resize((size_t)size);
        for (long long i = 0; i < size; ++i) {
            std::string error;
            if (!spawn_worker(pool->workers[(size_t)i],
                              ((string_value_t *)args[1])->value,
                              pool->ready_timeout_ms, pool->hybrid,
                              error)) {
                for (worker_slot &worker : pool->workers) {
                    if (worker.process && worker.io.usable())
                        close_worker(worker, true);
                }
                yyerror("std.worker.pool: %s", error.c_str());
                return NULL;
            }
        }
        return new_pool_value(std::move(pool));
    }

    if (functype == B_worker_shutdown) {
        if (args.size() != 1) {
            yyerror("std.worker.shutdown expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
        std::shared_ptr<worker_pool> pool;
        if (!get_pool(args[0], pool)) {
            yyerror("std.worker.shutdown expects a worker pool");
            return NULL;
        }
        for (const auto &job : pool->queue)
            fail_job(job, "worker pool shut down");
        pool->queue.clear();
        for (worker_slot &worker : pool->workers) {
            if (worker.job) {
                fail_job(worker.job, "worker pool shut down");
                worker.job.reset();
            }
            close_worker(worker, true);
        }
        return newnull();
    }

    if (functype == B_worker_stats) {
        if (args.size() != 1) {
            yyerror("std.worker.stats expects 1 argument, got %d",
                    (int)args.size());
            return NULL;
        }
        std::shared_ptr<worker_pool> pool;
        if (!get_pool(args[0], pool)) {
            yyerror("std.worker.stats expects a worker pool");
            return NULL;
        }
        long long busy = 0;
        long long available = 0;
        for (const worker_slot &worker : pool->workers) {
            if (worker.busy)
                ++busy;
            if (worker_usable(worker))
                ++available;
        }
        dict_value_t *result = newdictval();
        result->payload->items.push_back(std::make_pair(
            newstrval("workers"), newintval((long long)pool->workers.size())));
        result->payload->items.push_back(std::make_pair(
            newstrval("busy"), newintval(busy)));
        result->payload->items.push_back(std::make_pair(
            newstrval("available"), newintval(available)));
        result->payload->items.push_back(std::make_pair(
            newstrval("queued"), newintval((long long)pool->queue.size())));
        result->payload->items.push_back(std::make_pair(
            newstrval("restarts"), newintval(pool->restarts)));
        return (value_t *)result;
    }

    if (functype == B_worker_submit) {
        if (args.size() != 3 || args[1]->type != T_string ||
            args[2]->type != T_list) {
            yyerror("std.worker.submit expects pool, method and list");
            return NULL;
        }
        std::shared_ptr<worker_pool> pool;
        if (!get_pool(args[0], pool)) {
            yyerror("std.worker.submit expects a worker pool");
            return NULL;
        }
        std::shared_ptr<worker_job> job = std::make_shared<worker_job>();
        job->pool = pool;
        job->id = pool->next_id++;
        if (!make_request(job->id, ((string_value_t *)args[1])->value,
                          args[2], nullptr, pool->hybrid, job->request)) {
            yyerror("std.worker.submit: cannot encode request");
            return NULL;
        }
        if ((long long)pool->queue.size() >= pool->queue_limit) {
            yyerror("std.worker.submit: worker queue is full");
            return NULL;
        }
        pool->queue.push_back(job);
        try_dispatch(pool.get());
        return new_job_value(std::move(job));
    }

    if (functype == B_worker_submit_bytes) {
        if (args.size() != 4 || args[1]->type != T_string ||
            args[2]->type != T_list || args[3]->type != T_bytes) {
            yyerror("std.worker.submit_bytes expects pool, method, list "
                    "and bytes body");
            return NULL;
        }
        std::shared_ptr<worker_pool> pool;
        if (!get_pool(args[0], pool)) {
            yyerror("std.worker.submit_bytes expects a worker pool");
            return NULL;
        }
        if (!pool->hybrid) {
            yyerror("std.worker.submit_bytes requires a hybrid worker pool");
            return NULL;
        }
        std::shared_ptr<worker_job> job = std::make_shared<worker_job>();
        job->pool = pool;
        job->id = pool->next_id++;
        job->binary = true;
        const std::string &body = ((bytes_value_t *)args[3])->value;
        if (!make_request(job->id, ((string_value_t *)args[1])->value,
                          args[2], &body, true, job->request)) {
            yyerror("std.worker.submit_bytes: cannot encode request");
            return NULL;
        }
        if ((long long)pool->queue.size() >= pool->queue_limit) {
            yyerror("std.worker.submit_bytes: worker queue is full");
            return NULL;
        }
        pool->queue.push_back(job);
        try_dispatch(pool.get());
        return new_job_value(std::move(job));
    }

    if (functype == B_worker_call_bytes) {
        if ((args.size() != 4 && args.size() != 5) ||
            args[1]->type != T_string || args[2]->type != T_list ||
            args[3]->type != T_bytes) {
            yyerror("std.worker.call_bytes expects pool, method, list, "
                    "bytes body and optional timeout");
            return NULL;
        }
        std::shared_ptr<worker_pool> pool;
        if (!get_pool(args[0], pool)) {
            yyerror("std.worker.call_bytes expects a worker pool");
            return NULL;
        }
        if (!pool->hybrid) {
            yyerror("std.worker.call_bytes requires a hybrid worker pool");
            return NULL;
        }
        long long timeout_ms = 0;
        if (args.size() == 5) {
            if (args[4]->type != T_int ||
                ((int_value_t *)args[4])->value < 0) {
                yyerror("std.worker.call_bytes timeout must be a "
                        "non-negative integer");
                return NULL;
            }
            timeout_ms = ((int_value_t *)args[4])->value;
        }
        std::shared_ptr<worker_job> job = std::make_shared<worker_job>();
        job->pool = pool;
        job->id = pool->next_id++;
        job->binary = true;
        const std::string &body = ((bytes_value_t *)args[3])->value;
        if (!make_request(job->id, ((string_value_t *)args[1])->value,
                          args[2], &body, true, job->request)) {
            yyerror("std.worker.call_bytes: cannot encode request");
            return NULL;
        }
        if ((long long)pool->queue.size() >= pool->queue_limit) {
            yyerror("std.worker.call_bytes: worker queue is full");
            return NULL;
        }
        pool->queue.push_back(job);
        try_dispatch(pool.get());

        value_t *job_value = new_job_value(job);
        std::vector<value_t *> wait_args{job_value};
        value_t *timeout_value = nullptr;
        if (args.size() == 5) {
            timeout_value = newintval(timeout_ms);
            wait_args.push_back(timeout_value);
        }
        value_t *result = call_worker_builtin(B_worker_wait, wait_args);
        freeval(job_value);
        if (timeout_value)
            freeval(timeout_value);
        return result;
    }

    if (functype == B_worker_wait) {
        if (args.empty() || args.size() > 2) {
            yyerror("std.worker.wait expects 1 or 2 arguments, got %d",
                    (int)args.size());
            return NULL;
        }
        std::shared_ptr<worker_job> job;
        if (!get_job(args[0], job)) {
            yyerror("std.worker.wait expects a worker job");
            return NULL;
        }
        std::shared_ptr<worker_pool> pool = job->pool.lock();
        if (!pool) {
            yyerror("std.worker.wait: worker pool is gone");
            return NULL;
        }
        long long timeout_ms = 0;
        if (args.size() == 2) {
            if (args[1]->type != T_int ||
                ((int_value_t *)args[1])->value < 0) {
                yyerror("std.worker.wait timeout must be a non-negative "
                        "integer");
                return NULL;
            }
            timeout_ms = ((int_value_t *)args[1])->value;
        }
        const auto start = std::chrono::steady_clock::now();
        for (;;) {
            if (job->done)
                return job->result ? dupval(job->result) : newnull();
            try_dispatch(pool.get());
            if (job->done)
                return job->result ? dupval(job->result) : newnull();
            if (job->slot >= 0) {
                if (complete_slot(pool.get(), (size_t)job->slot,
                                  remaining_timeout(start, timeout_ms)))
                    continue;
                if (timeout_ms > 0 &&
                    remaining_timeout(start, timeout_ms) <= 0) {
                    fail_job(job, "worker wait timed out");
                    continue;
                }
                worker_yield();
                continue;
            }
            bool queued = false;
            for (const auto &entry : pool->queue) {
                if (entry == job) {
                    queued = true;
                    break;
                }
            }
            if (!queued) {
                fail_job(job, "worker job was lost");
                continue;
            }
            bool found_busy = false;
            bool completed = false;
            for (size_t i = 0; i < pool->workers.size(); ++i) {
                if (pool->workers[i].busy) {
                    completed = complete_slot(
                        pool.get(), i,
                        remaining_timeout(start, timeout_ms));
                    found_busy = true;
                    break;
                }
            }
            if (found_busy) {
                if (completed)
                    continue;
                if (timeout_ms > 0 &&
                    remaining_timeout(start, timeout_ms) <= 0) {
                    for (auto it = pool->queue.begin();
                         it != pool->queue.end(); ++it) {
                        if (*it == job) {
                            pool->queue.erase(it);
                            break;
                        }
                    }
                    fail_job(job, "worker wait timed out");
                    continue;
                }
                worker_yield();
                continue;
            }
            if (timeout_ms > 0 &&
                remaining_timeout(start, timeout_ms) <= 0) {
                for (auto it = pool->queue.begin(); it != pool->queue.end();
                     ++it) {
                    if (*it == job) {
                        pool->queue.erase(it);
                        break;
                    }
                }
                fail_job(job, "worker wait timed out");
                continue;
            }
            fail_job(job, "no usable worker");
        }
    }

    if (functype != B_worker_call && functype != B_worker_map) {
        yyerror("unknown worker function");
        return NULL;
    }

    const bool is_map = functype == B_worker_map;
    if ((args.size() != 3 && args.size() != 4) ||
        args[1]->type != T_string || args[2]->type != T_list) {
        yyerror("%s expects pool, method, list and optional timeout",
                is_map ? "std.worker.map" : "std.worker.call");
        return NULL;
    }
    long long timeout_ms = 0;
    if (args.size() == 4) {
        if (args[3]->type != T_int ||
            ((int_value_t *)args[3])->value < 0) {
            yyerror("%s timeout must be a non-negative integer",
                    is_map ? "std.worker.map" : "std.worker.call");
            return NULL;
        }
        timeout_ms = ((int_value_t *)args[3])->value;
    }
    std::shared_ptr<worker_pool> pool;
    if (!get_pool(args[0], pool)) {
        yyerror("std.worker expects a worker pool");
        return NULL;
    }
    const std::string method = ((string_value_t *)args[1])->value;

    if (!is_map) {
        worker_slot *slot = nullptr;
        for (worker_slot &worker : pool->workers) {
            if (!worker.busy && !worker.completing &&
                worker_usable(worker)) {
                slot = &worker;
                break;
            }
        }
        if (!slot) {
            yyerror("std.worker.call: all workers are busy");
            return NULL;
        }
        const long long id = pool->next_id++;
        std::string line;
        slot->busy = true;
        /* Reserve the slot before writing in case the write yields fibers. */
        slot->completing = true;
        if (!make_request(id, method, args[2], nullptr, pool->hybrid, line) ||
            !slot->io.write(line) ||
            !slot->io.flush()) {
            slot->completing = false;
            slot->busy = false;
            yyerror("std.worker.call: cannot send request");
            return NULL;
        }
        value_t *result = nullptr;
        std::string transport_error;
        const bool ok = read_response(slot->io, id,
                                      timeout_ms, false, result,
                                      transport_error);
        slot->completing = false;
        slot->busy = false;
        if (!ok) {
            std::string restart_error;
            restart_worker(pool.get(), *slot, restart_error);
            return newresulterr_message(
                "worker", transport_error.empty() ? "worker read failed"
                                                  : transport_error);
        }
        return result;
    }

    if (!pool->queue.empty()) {
        yyerror("std.worker.map: pool has pending submitted jobs");
        return NULL;
    }
    for (const worker_slot &worker : pool->workers) {
        if (worker.busy) {
            yyerror("std.worker.map: pool has busy workers");
            return NULL;
        }
    }
    struct list_value_t *items = (list_value_t *)args[2];
    struct list_value_t *results = newlistval();
    const size_t batch = pool->workers.size();
    for (size_t start = 0; start < items->payload->items.size();
         start += batch) {
        const size_t count =
            std::min(batch, items->payload->items.size() - start);
        std::vector<long long> ids;
        ids.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            worker_slot &worker = pool->workers[i];
            if (!worker_usable(worker)) {
                std::string restart_error;
                if (!restart_worker(pool.get(), worker, restart_error)) {
                    freeval((value_t *)results);
                    yyerror("std.worker.map: no usable worker");
                    return NULL;
                }
            }
            const long long id = pool->next_id++;
            std::string line;
            worker.busy = true;
            worker.completing = true;
            if (!make_request(id, method, items->payload->items[start + i],
                              nullptr, pool->hybrid, line) ||
                !worker.io.write(line) ||
                !worker.io.flush()) {
                worker.completing = false;
                worker.busy = false;
                freeval((value_t *)results);
                yyerror("std.worker.map: cannot send request");
                return NULL;
            }
            ids.push_back(id);
        }
        for (size_t i = 0; i < count; ++i) {
            worker_slot &worker = pool->workers[i];
            value_t *result = nullptr;
            std::string transport_error;
            const bool ok =
                read_response(worker.io, ids[i],
                              timeout_ms, false, result, transport_error);
            worker.completing = false;
            if (!ok) {
                worker.busy = false;
                std::string restart_error;
                restart_worker(pool.get(), worker, restart_error);
                freeval((value_t *)results);
                yyerror("std.worker.map: %s",
                        transport_error.empty() ? "cannot read response"
                                                : transport_error.c_str());
                return NULL;
            }
            worker.busy = false;
            if (result->type != T_result ||
                !((result_value_t *)result)->ok) {
                freeval((value_t *)results);
                if (result->type == T_result &&
                    ((result_value_t *)result)->error) {
                    value_t *error = dupval(((result_value_t *)result)->error);
                    freeval(result);
                    if (error->type == T_error) {
                        yyerror("std.worker.map: %s",
                                ((error_value_t *)error)->message.c_str());
                    } else {
                        yyerror("std.worker.map: worker failed");
                    }
                    freeval(error);
                } else {
                    freeval(result);
                    yyerror("std.worker.map: worker failed");
                }
                return NULL;
            }
            results->payload->items.push_back(
                dupval(((result_value_t *)result)->value));
            freeval(result);
        }
    }
    return (value_t *)results;
}
