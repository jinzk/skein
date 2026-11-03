#include "worker_pool.h"

#include "ast.h"
#include "io.h"
#include "json.h"
#include "platform.h"
#include "process_spawn.h"
#include "valuetype.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

struct worker_slot {
    value_t *process = nullptr;
    value_t *stdin_stream = nullptr;
    value_t *stdout_stream = nullptr;
    bool busy = false;
};

struct worker_pool {
    std::vector<worker_slot> workers;
    long long next_id = 1;

    ~worker_pool() {
        for (worker_slot &worker : workers) {
            if (worker.process) {
                freeval(worker.process);
                worker.process = nullptr;
            }
        }
    }
};

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
};

value_t *new_pool_value(std::shared_ptr<worker_pool> pool) {
    userdata_value_t *value = new userdata_value_t;
    value->type = T_userdata;
    value->meta = &kWorkerPoolType;
    value->data = std::move(pool);
    return (value_t *)value;
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

value_t *call_io(int functype, std::vector<value_t *> args) {
    return call_io_builtin(functype, args);
}

bool write_bytes(value_t *stream, const std::string &data) {
    value_t *bytes = newbytesval(data);
    value_t *result = call_io(
        B_io_write_all, {stream, bytes});
    freeval(bytes);
    if (!result)
        return false;
    freeval(result);
    return true;
}

bool flush_stream(value_t *stream) {
    value_t *result = call_io(B_io_flush, {stream});
    if (!result)
        return false;
    freeval(result);
    return true;
}

value_t *read_line(value_t *stream) {
    return call_io(B_io_read_line, {stream});
}

value_t *json_stringify(value_t *value) {
    return call_json_builtin(B_json_stringify, {value});
}

value_t *json_parse(value_t *value) {
    return call_json_builtin(B_json_parse, {value});
}

bool spawn_worker(worker_slot &slot, const std::string &script,
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
        B_process_spawn, {executable_value, (value_t *)options});
    freeval(executable_value);
    freeval((value_t *)options);
    if (!process) {
        error = "cannot spawn worker";
        return false;
    }

    value_t *stdin_stream = dict_get(process, "stdin");
    value_t *stdout_stream = dict_get(process, "stdout");
    if (!stdin_stream || !stdout_stream) {
        freeval(process);
        error = "worker process has no pipes";
        return false;
    }
    slot.process = process;
    slot.stdin_stream = stdin_stream;
    slot.stdout_stream = stdout_stream;
    return true;
}

void close_worker(worker_slot &slot, bool kill_if_running) {
    if (slot.stdin_stream) {
        value_t *result = call_io(B_io_close, {slot.stdin_stream});
        if (result)
            freeval(result);
    }
    if (slot.process) {
        value_t *timeout = newintval(1000);
        value_t *wait_result = call_process_spawn_builtin(
            B_process_wait, {slot.process, timeout});
        freeval(timeout);
        if (wait_result)
            freeval(wait_result);
        if (kill_if_running) {
            value_t *kill_result = call_process_spawn_builtin(
                B_process_kill, {slot.process});
            if (kill_result)
                freeval(kill_result);
        }
        value_t *close_result = call_process_spawn_builtin(
            B_process_close, {slot.process});
        if (close_result)
            freeval(close_result);
        freeval(slot.process);
    }
    slot = worker_slot{};
}

bool make_request(long long id, const std::string &method, value_t *params,
                  std::string &line) {
    dict_value_t *request = newdictval();
    request->payload->items.push_back(std::make_pair(
        newstrval("id"), newintval(id)));
    request->payload->items.push_back(std::make_pair(
        newstrval("method"), newstrval(method.c_str())));
    request->payload->items.push_back(std::make_pair(
        newstrval("params"), dupval(params)));
    value_t *json = json_stringify((value_t *)request);
    freeval((value_t *)request);
    if (!json)
        return false;
    line = ((string_value_t *)json)->value;
    line.push_back('\n');
    freeval(json);
    return true;
}

bool read_response(value_t *stdout_stream, long long expected_id,
                   value_t *&result) {
    value_t *line = read_line(stdout_stream);
    if (!line)
        return false;
    if (line->type == T_null) {
        freeval(line);
        result = newresulterr_message("worker", "worker closed its stdout");
        return true;
    }
    value_t *response = json_parse(line);
    freeval(line);
    if (!response) {
        result = newresulterr_message("worker", "invalid worker response");
        return true;
    }
    value_t *id = dict_get(response, "id");
    value_t *ok = dict_get(response, "ok");
    if (!id || id->type != T_int ||
        ((int_value_t *)id)->value != expected_id ||
        !ok || ok->type != T_bool) {
        freeval(response);
        result = newresulterr_message("worker", "invalid worker response");
        return true;
    }
    if (((bool_value_t *)ok)->value) {
        value_t *value = dict_get(response, "result");
        result = newresultok(value ? dupval(value) : newnull());
        freeval(response);
        return true;
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

}  // namespace

value_t *call_worker_builtin(
    int functype, const std::vector<value_t *> &args) {
    if (functype == B_worker_pool) {
        if (args.size() != 2 || args[0]->type != T_int ||
            ((int_value_t *)args[0])->value <= 0 ||
            args[1]->type != T_string) {
            yyerror("std.worker.pool expects a positive size and script");
            return NULL;
        }
        const long long size = ((int_value_t *)args[0])->value;
        std::shared_ptr<worker_pool> pool = std::make_shared<worker_pool>();
        pool->workers.resize((size_t)size);
        for (long long i = 0; i < size; ++i) {
            std::string error;
            if (!spawn_worker(pool->workers[(size_t)i],
                              ((string_value_t *)args[1])->value, error)) {
                for (worker_slot &worker : pool->workers) {
                    if (worker.process)
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
        for (worker_slot &worker : pool->workers)
            close_worker(worker, true);
        return newnull();
    }

    if (functype != B_worker_call && functype != B_worker_map) {
        yyerror("unknown worker function");
        return NULL;
    }

    const bool is_map = functype == B_worker_map;
    if (args.size() != 3 || args[1]->type != T_string ||
        args[2]->type != (is_map ? T_list : T_list)) {
        yyerror("%s expects pool, method and list",
                is_map ? "std.worker.map" : "std.worker.call");
        return NULL;
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
            if (!worker.busy) {
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
        if (!make_request(id, method, args[2], line) ||
            !write_bytes(slot->stdin_stream, line) ||
            !flush_stream(slot->stdin_stream)) {
            yyerror("std.worker.call: cannot send request");
            return NULL;
        }
        slot->busy = true;
        value_t *result = nullptr;
        const bool ok = read_response(slot->stdout_stream, id, result);
        slot->busy = false;
        if (!ok) {
            yyerror("std.worker.call: cannot read response");
            return NULL;
        }
        return result;
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
            const long long id = pool->next_id++;
            std::string line;
            if (!make_request(id, method, items->payload->items[start + i],
                              line) ||
                !write_bytes(worker.stdin_stream, line) ||
                !flush_stream(worker.stdin_stream)) {
                freeval((value_t *)results);
                yyerror("std.worker.map: cannot send request");
                return NULL;
            }
            worker.busy = true;
            ids.push_back(id);
        }
        for (size_t i = 0; i < count; ++i) {
            worker_slot &worker = pool->workers[i];
            value_t *result = nullptr;
            if (!read_response(worker.stdout_stream, ids[i], result)) {
                worker.busy = false;
                freeval((value_t *)results);
                yyerror("std.worker.map: cannot read response");
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
