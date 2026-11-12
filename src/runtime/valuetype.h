#ifndef _VALUETYPE_H_
#define _VALUETYPE_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

struct symlist;
struct scope_t;
struct symbol;
struct value_t;
struct type_constraint;
struct scope_context;

struct function_code : std::enable_shared_from_this<function_code> {
    struct symlist *sl;
    int nparams;
    int required_params;
    std::string name;
    bool is_method;
    bool variadic;
    bool is_generator;
    int def_line = 0;
    int def_column = 0;
    /* Package that defined this function, used for lazy compilation. */
    std::string package;
    /* Module container that defined this function; empty for ordinary
     * packages and the entry script. */
    std::string module;
    /* Source file that defined this function, used for import alias lookup. */
    std::string source;
    /* Optional static return type annotation; shared by function values. */
    std::shared_ptr<const struct type_constraint> ret_type;
    /* Captured binding names, resolved to symbol cells at closure creation. */
    std::vector<std::string> upvalues;
    /*
     * Opaque per-function execution cache owned by the VM bridge.  Kept as
     * void* so the runtime layer does not depend on interpreter/JIT types.
     */
    void *vm_cache = nullptr;
};

/*
 * Explicit upvalue storage.  While a frame is alive the cell is open and
 * points at the frame slot (or symbol cell); when the frame exits the cell is
 * closed and takes ownership of the value.  Closures only hold cells, never
 * raw frame/symbol pointers.
 */
struct upvalue_cell {
    struct value_t **slot = nullptr;
    struct value_t *value = nullptr;
    const struct type_constraint *type = nullptr;
    struct scope_context *owner = nullptr;
    std::string name;
    bool is_const = false;

    /*
     * Cells are chained per thread so the cycle collector can find cells
     * that are no longer reachable from any root.  A closed cell
     * (slot == nullptr) owns its value.
     */
    uint64_t gc_epoch = 0;
    struct upvalue_cell *gc_prev = nullptr;
    struct upvalue_cell *gc_next = nullptr;

    upvalue_cell();
    ~upvalue_cell();
};

/* Head of the current thread's upvalue-cell registry (GC use only). */
struct upvalue_cell *gc_cell_registry_head();

struct function_def {
    std::shared_ptr<function_code> code;
    std::shared_ptr<scope_t> closure;
    std::vector<std::shared_ptr<upvalue_cell>> upvalues;
};

enum val_type {
    T_int = 1,
    T_float,
    T_bool,
    T_string,
    T_char,
    T_byte,
    T_bytes,
    T_list,
    T_dict,
    T_func,
    T_builtin,
    T_userdata,
    T_result,
    T_error,
    T_struct,
    T_enum,
    T_lru,
    T_deque,
    T_heap,
    T_set,
    T_method,
    T_native,
    T_null,
    T_class,
    T_return,
    T_break,
    T_continue
};

struct value_t {
    enum val_type type;
};

struct control_value_t {
    enum val_type type;
    char *label;
};

struct int_value_t {
    enum val_type type;
    long long value;
    /* True for process-wide small-int singletons; freeval must not delete. */
    bool interned = false;
};

struct float_value_t {
    enum val_type type;
    double value;
};

struct bool_value_t {
    enum val_type type;
    bool value;
};

struct string_value_t {
    enum val_type type;
    /* `value` points at `storage->c_str()` when storage is set.  Borrowed
     * names (VM member lookups) leave storage null and point `value` at
     * caller-owned memory that outlives the temporary value. */
    char *value = nullptr;
    std::shared_ptr<const std::string> storage;
};

struct char_value_t {
    enum val_type type;
    unsigned int value; /* Unicode scalar value */
};

struct byte_value_t {
    enum val_type type;
    unsigned int value; /* 0..255 */
};

/* Reference-counted byte buffer with copy-on-write mutation.  Values that
 * share a buffer keep value semantics; `write()` detaches when the buffer is
 * aliased. */
struct bytes_buffer {
    std::shared_ptr<std::string> storage;

    bytes_buffer() = default;
    explicit bytes_buffer(std::string data);

    const std::string &read() const;
    std::string &write();
    std::size_t size() const;
    bool empty() const;
    const char *data() const;
    const char *c_str() const;
    const char &operator[](std::size_t index) const;
    operator const std::string &() const { return read(); }
};

struct bytes_value_t {
    enum val_type type;
    bytes_buffer value;
};

struct list_payload {
    std::vector<value_t *> items;
    ~list_payload();
};

struct dict_payload {
    std::vector<std::pair<value_t *, value_t *>> items;
    /* True for std/package namespaces, which keep dot access for exports. */
    bool is_namespace = false;
    /* Lazily built index for large dictionaries.  Indices refer to `items`;
     * any size change invalidates it. */
    mutable std::unordered_map<std::uint64_t, std::vector<std::size_t>> index;
    mutable bool index_valid = false;
    mutable std::size_t index_size = 0;
    ~dict_payload();
};

struct list_value_t {
    enum val_type type;
    std::shared_ptr<list_payload> payload;
};

struct lru_payload {
    /* Recency order: index 0 is the least recently used entry, the last
     * element is the most recently used one. */
    std::vector<std::pair<value_t *, value_t *>> items;
    std::size_t capacity = 0;
    ~lru_payload();
};

struct lru_value_t {
    enum val_type type;
    std::shared_ptr<lru_payload> payload;
};

/*
 * Persistent deque: two singly-linked stacks sharing their tails with older
 * versions, so push/pop are O(1) amortized while the value keeps value
 * semantics.  `front` is in queue order (head is the front element); `back`
 * is stored reversed (head is the back element).
 */
struct deque_node {
    value_t *value = nullptr;
    std::shared_ptr<deque_node> next;
    ~deque_node();
};

struct deque_payload {
    std::shared_ptr<deque_node> front;
    std::shared_ptr<deque_node> front_tail;
    std::shared_ptr<deque_node> back;
    std::shared_ptr<deque_node> back_tail;
    std::size_t front_size = 0;
    std::size_t back_size = 0;
    ~deque_payload();
};

struct deque_value_t {
    enum val_type type;
    std::shared_ptr<deque_payload> payload;
};

/*
 * Persistent leftist heap: immutable nodes shared between versions, so push
 * and pop create O(log n) new nodes while the value keeps value semantics.
 */
struct heap_node {
    value_t *value = nullptr;
    std::shared_ptr<heap_node> left;
    std::shared_ptr<heap_node> right;
    int rank = 1;
    ~heap_node();
};

struct heap_payload {
    std::shared_ptr<heap_node> root;
    value_t *less = nullptr;
    std::size_t size = 0;
    ~heap_payload();
};

struct heap_value_t {
    enum val_type type;
    std::shared_ptr<heap_payload> payload;
};

/*
 * Persistent set: a randomized treap ordered by key hash (with a bucket per
 * hash for collisions) plus a persistent insertion log, so add/remove are
 * O(log n) and the value keeps value semantics.
 */
struct set_bucket {
    std::vector<value_t *> keys;
    ~set_bucket();
};

struct set_node {
    std::uint64_t hash = 0;
    std::uint64_t priority = 0;
    std::shared_ptr<set_bucket> bucket;
    std::shared_ptr<set_node> left;
    std::shared_ptr<set_node> right;
};

struct set_payload {
    std::shared_ptr<set_node> root;
    std::shared_ptr<deque_payload> order;
    std::size_t size = 0;
    ~set_payload();
};

struct set_value_t {
    enum val_type type;
    std::shared_ptr<set_payload> payload;
};

struct dict_value_t {
    enum val_type type;
    std::shared_ptr<dict_payload> payload;
};

struct func_value_t {
    enum val_type type;
    std::shared_ptr<function_def> def;
    /* Free-list link used only while the object is pooled. */
    struct func_value_t *next = nullptr;
};

struct builtin_value_t {
    enum val_type type;
    int functype;
};

/* Native callable value: static bindings and manifest-bound FFI functions.
 * `children` keeps script values reachable for the cycle collector. */
struct native_function_value {
    enum val_type type;
    std::function<struct value_t *(
        const std::vector<struct value_t *> &args)>
        call;
    std::shared_ptr<void> owner;
    std::vector<struct value_t *> children;

    ~native_function_value();
};

struct stream_ops {
    bool (*read)(void *data, std::size_t max_bytes, long long timeout_ms,
                 std::string &out, bool &eof, std::string &error);
    bool (*write)(void *data, const std::string &bytes, long long timeout_ms,
                  std::size_t &written, std::string &error);
    bool (*flush)(void *data, std::string &error);
    bool (*close)(void *data, bool &was_open, std::string &error);
    bool (*eof)(void *data, bool &eof, std::string &error);
    bool (*seekable)(void *data);
    bool (*seek)(void *data, long long offset, int whence,
                 long long &position, std::string &error);
    bool (*tell)(void *data, long long &position, std::string &error);
    struct value_t *(*read_async)(void *data,
                                  const std::shared_ptr<void> &owner,
                                  std::size_t max_bytes, long long timeout_ms,
                                  std::string &error);
    struct value_t *(*write_async)(void *data,
                                   const std::shared_ptr<void> &owner,
                                   const std::string &bytes,
                                   long long timeout_ms,
                                   std::string &error);
};

struct native_type {
    const char *name;
    /* Return a human-readable representation of one instance. */
    std::string (*describe)(const void *data);
    const struct stream_ops *stream = nullptr;
    /* Optional named field access for dot syntax (`p.x`, `p.x = v`).
     * Implementations report their own errors. */
    struct value_t *(*get_field)(void *data, const char *name) = nullptr;
    bool (*set_field)(void *data, const char *name,
                      struct value_t *value) = nullptr;
    /* Visit script values owned by the userdata payload so the cycle
     * collector can see them (e.g. a callback holding a script function). */
    void (*for_each_child)(
        const void *data,
        const std::function<void(const struct value_t *)> &fn) = nullptr;
};

struct userdata_value_t {
    enum val_type type;
    const struct native_type *meta;
    std::shared_ptr<void> data;
};

struct error_value_t {
    enum val_type type;
    std::string kind;
    std::string message;
};

struct result_value_t {
    enum val_type type;
    bool ok;
    struct value_t *value;
    struct value_t *error;
};

struct value_t *newreturn();
struct value_t *newbreak(const char *label = NULL);
struct value_t *newcontinue(const char *label = NULL);
struct value_t *newintval(long long i);
struct value_t *newfloatval(double f);
struct value_t *newboolval(bool b);
struct value_t *newstrval(const char *s);
struct value_t *newcharval(unsigned int codepoint);
struct value_t *newbyteval(unsigned int value);
struct value_t *newbytesval(std::string data);
struct value_t *newbuiltinval(int functype);
struct value_t *newnativeval(
    std::function<struct value_t *(
        const std::vector<struct value_t *> &args)> call,
    std::shared_ptr<void> owner = nullptr);
struct value_t *newerrorval(const std::string &kind,
                            const std::string &message);
struct value_t *newresultok(struct value_t *value);
struct value_t *newresulterr(struct value_t *error);
struct value_t *newresulterr_message(const std::string &kind,
                                     const std::string &message);
/* Convert any value into an error value, taking ownership of `value`. */
struct value_t *normalize_error(struct value_t *value,
                                const char *default_kind);
const char *error_kind_of(const struct value_t *value);
std::string error_message_of(const struct value_t *value);
struct value_t *newuserdataval(const struct native_type *meta,
                               std::shared_ptr<void> data);
struct value_t *newnull();
struct value_t *newfuncval(
    std::shared_ptr<function_code> code,
    std::shared_ptr<struct scope_t> closure,
    std::vector<std::shared_ptr<struct upvalue_cell>> upvalues = {});
void free_function_code(struct function_code *code);
struct value_t *dupval(struct value_t *v);

/* Release thread-local value free lists before a worker runtime exits. */
void value_runtime_thread_shutdown();
struct list_value_t *newlistval();
struct dict_value_t *newdictval();
struct lru_value_t *newlruval(std::size_t capacity);
struct deque_value_t *newdequeval();
/* Takes ownership of `less`. */
struct heap_value_t *newheapval(struct value_t *less);
struct set_value_t *newsetval();

/* Borrowed pointers in queue order; no dupval. */
void deque_items_in_order(const struct deque_payload *payload,
                          std::vector<value_t *> &out);
/* Persistent push; dupvals `value` and returns a new payload. */
std::shared_ptr<deque_payload> deque_push_back_payload(
    const std::shared_ptr<deque_payload> &source, struct value_t *value);
/* Borrowed pointers in level order; no dupval. */
void heap_items_level_order(const struct heap_payload *payload,
                            std::vector<value_t *> &out);
/* Set membership and persistent updates. */
bool set_contains(const struct set_payload *payload, const struct value_t *key);
std::shared_ptr<set_node> set_insert(const std::shared_ptr<set_node> &root,
                                     const struct value_t *key, bool &added);
std::shared_ptr<set_node> set_erase(const std::shared_ptr<set_node> &root,
                                    const struct value_t *key, bool &removed);
/* Borrowed keys in insertion order (latest occurrence wins); no dupval. */
void set_values_in_order(const struct set_payload *payload,
                         std::vector<value_t *> &out);

/* Free a value and everything it owns. NULL is safe. */
void freeval(struct value_t *v);

/* Print a value in a human-readable form (no trailing newline). */
void printvalue(struct value_t *v);

/* Convert a value to its human-readable string form. */
std::string value_to_string(const struct value_t *v);

const char *typename_of(const struct value_t *v);

#endif
