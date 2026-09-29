#include "php_bridge.h"
#include <php.h>
#include <php_embed.h>
#include <zend_exceptions.h>
#include <zend_smart_str.h>
#include <zend_call_stack.h>
#include <ext/json/php_json.h>
#include <ext/pcre/php_pcre.h>
#include <locale.h>

static PhpHostCall host_call;
static void *host_context;
static int sapi_started, started, requested, callbacks_ready, broken, poisoned, stopping, depth;
static uint64_t instructions, next_callback;
static HashTable callbacks;
static char last_error[1024];
static char *saved_json;

static int host(const struct PhpMessage *message, struct PhpReply *reply)
{
    memset(reply, 0, sizeof(*reply));
    if (stopping && message->operation != PHP_HOST_LOG) {
        snprintf(reply->error, sizeof(reply->error), "PHP runtime is shutting down");
        return 0;
    }
    return host_call && host_call(host_context, message, reply);
}

static size_t output(const char *text, size_t length)
{
    struct PhpMessage m = {.operation = PHP_HOST_LOG, .text = text, .text_size = length};
    struct PhpReply r;
    host(&m, &r);
    return length;
}

static void log_message(const char *text, int type)
{
    (void)type;
    snprintf(last_error, sizeof(last_error), "%s", text);
    output(text, strlen(text));
}

static void no_variables(zval *variables) { (void)variables; }
static void no_flush(void *context) { (void)context; }
static int no_deactivate(void) { return SUCCESS; }

static int instruction(zend_execute_data *execute_data)
{
    if (++instructions > 10000000)
        zend_error(E_ERROR, "PHP instruction limit exceeded (10 million)");
    if (execute_data->opline->opcode == ZEND_INCLUDE_OR_EVAL)
        zend_error(E_ERROR, "include, require and eval are unavailable in the PHP proof of concept");
    return ZEND_USER_OPCODE_DISPATCH;
}

/* An explicit allowlist keeps new PHP builtins inaccessible by default. */
static const char *allowed_functions[] = {
    "abs", "acos", "acosh", "asin", "asinh", "atan", "atan2", "atanh", "base_convert",
    "bindec", "ceil", "cos", "cosh", "decbin", "dechex", "decoct", "deg2rad", "exp", "expm1",
    "fdiv", "floor", "fmod", "fpow", "hexdec", "hypot", "intdiv", "is_finite", "is_infinite",
    "is_nan", "log", "log10", "log1p", "max", "min", "octdec", "pi", "pow", "rad2deg",
    "round", "sin", "sinh", "sqrt", "tan", "tanh",
    "preg_match", "preg_match_all", "preg_replace", "preg_replace_callback",
    "preg_replace_callback_array", "preg_filter", "preg_split", "preg_quote", "preg_grep",
    "preg_last_error", "preg_last_error_msg",
    "strlen", "strcmp", "strncmp", "strcasecmp", "strncasecmp", "substr", "substr_count",
    "substr_replace", "strpos", "stripos", "strrpos", "strripos", "str_contains",
    "str_starts_with", "str_ends_with", "str_replace", "str_ireplace", "str_repeat", "str_pad",
    "strtolower", "strtoupper", "trim", "ltrim", "rtrim", "explode", "implode", "join",
    "chr", "ord", "bin2hex", "hex2bin", "pack", "unpack", "sprintf", "vsprintf",
    "count", "sizeof", "array_key_exists", "array_keys", "array_values", "array_merge",
    "array_fill", "array_slice", "array_splice", "array_push", "array_pop", "array_shift",
    "array_unshift", "array_reverse", "array_search", "in_array", "array_map", "array_filter",
    "array_reduce", "array_sum", "array_product", "array_unique", "array_is_list", "range",
    "sort", "rsort", "asort", "arsort", "ksort", "krsort", "usort", "uasort", "uksort",
    "json_encode", "json_decode", "json_validate", "json_last_error", "json_last_error_msg",
    "is_array", "is_bool", "is_float", "is_int", "is_integer", "is_null", "is_numeric",
    "is_object", "is_scalar", "is_string", "is_callable", "intval", "floatval", "boolval",
    "strval", "gettype", "get_debug_type", "get_class", "function_exists", "defined", "define",
    "constant", "call_user_func", "call_user_func_array", "func_get_args", "func_get_arg",
    "func_num_args", "card_log", "card_read", "card_write", "card_time_ns", "card_signal_read",
    "card_signal_drive", "card_after", "card_on_signal", "project_read", "project_write", NULL
};

static int listed(const char *name, const char *const *list)
{
    for (; *list; ++list) if (strcmp(name, *list) == 0) return 1;
    return 0;
}

static void restrict_runtime(void)
{
    zend_string *name;
    smart_str disabled = {0};
    ZEND_HASH_FOREACH_STR_KEY(CG(function_table), name) {
        if (name && !listed(ZSTR_VAL(name), allowed_functions)) {
            smart_str_append(&disabled, name);
            smart_str_appendc(&disabled, ',');
        }
    } ZEND_HASH_FOREACH_END();
    smart_str_0(&disabled);
    /* Unlike disable_functions, direct deletion also removes PHP 8.4 exit/die. */
    if (disabled.s) {
        char *at = ZSTR_VAL(disabled.s), *end;
        while ((end = strchr(at, ','))) {
            zend_hash_str_del(CG(function_table), at, end - at);
            at = end + 1;
        }
    }
    smart_str_free(&disabled);
    zend_hash_rehash(CG(function_table));

    const char *safe_classes[] = {
        "stdclass", "closure", "generator", "exception", "errorexception", "error",
        "typeerror", "argumentcounterror", "valueerror", "arithmeticerror",
        "divisionbyzeroerror", "parseerror", "compileerror", "unhandledmatcherror",
        "jsonexception", "logicexception", "badfunctioncallexception", "badmethodcallexception",
        "domainexception", "invalidargumentexception", "lengthexception", "outofrangeexception",
        "runtimeexception", "outofboundsexception", "overflowexception", "rangeexception",
        "underflowexception", "unexpectedvalueexception", NULL
    };
    zend_class_entry *ce;
    ZEND_HASH_FOREACH_STR_KEY_PTR(CG(class_table), name, ce) {
        if (name && !(ce->ce_flags & ZEND_ACC_INTERFACE) &&
            !listed(ZSTR_VAL(name), safe_classes))
            zend_disable_class(ZSTR_VAL(name), ZSTR_LEN(name));
    } ZEND_HASH_FOREACH_END();
}

static int host_or_throw(const struct PhpMessage *m, struct PhpReply *r)
{
    if (host(m, r)) return 1;
    zend_throw_error(NULL, "%s", r->error[0] ? r->error : "Script host API failed");
    return 0;
}

PHP_FUNCTION(card_log)
{
    char *s; size_t n;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STRING(s, n) ZEND_PARSE_PARAMETERS_END();
    struct PhpMessage m = {.operation=PHP_HOST_LOG, .text=s, .text_size=n};
    struct PhpReply r;
    host_or_throw(&m, &r);
}

PHP_FUNCTION(card_read)
{
    char *s; size_t n; zend_long address;
    ZEND_PARSE_PARAMETERS_START(2, 2) Z_PARAM_STRING(s,n) Z_PARAM_LONG(address) ZEND_PARSE_PARAMETERS_END();
    struct PhpMessage m = {.operation=PHP_HOST_READ, .text=s, .text_size=n, .a=address};
    struct PhpReply r;
    if (host_or_throw(&m,&r)) RETURN_LONG(r.value);
}

PHP_FUNCTION(card_write)
{
    char *s; size_t n; zend_long address, value;
    ZEND_PARSE_PARAMETERS_START(3, 3) Z_PARAM_STRING(s,n) Z_PARAM_LONG(address) Z_PARAM_LONG(value) ZEND_PARSE_PARAMETERS_END();
    struct PhpMessage m = {.operation=PHP_HOST_WRITE, .text=s, .text_size=n, .a=address, .b=value};
    struct PhpReply r;
    if (host_or_throw(&m,&r)) RETURN_TRUE;
}

PHP_FUNCTION(card_time_ns)
{
    ZEND_PARSE_PARAMETERS_NONE();
    struct PhpMessage m = {.operation=PHP_HOST_TIME};
    struct PhpReply r;
    if (host_or_throw(&m,&r)) RETURN_LONG(r.value);
}

PHP_FUNCTION(card_signal_read)
{
    char *s; size_t n;
    ZEND_PARSE_PARAMETERS_START(1, 1) Z_PARAM_STRING(s,n) ZEND_PARSE_PARAMETERS_END();
    struct PhpMessage m = {.operation=PHP_HOST_SIGNAL_READ, .text=s, .text_size=n};
    struct PhpReply r;
    if (host_or_throw(&m,&r)) RETURN_LONG(r.value);
}

PHP_FUNCTION(card_signal_drive)
{
    char *s; size_t n; zend_long mv, strength;
    ZEND_PARSE_PARAMETERS_START(3, 3) Z_PARAM_STRING(s,n) Z_PARAM_LONG(mv) Z_PARAM_LONG(strength) ZEND_PARSE_PARAMETERS_END();
    struct PhpMessage m = {.operation=PHP_HOST_SIGNAL_DRIVE, .text=s, .text_size=n, .a=mv, .b=strength};
    struct PhpReply r;
    if (host_or_throw(&m,&r)) RETURN_TRUE;
}

static void register_callback(INTERNAL_FUNCTION_PARAMETERS, int signal)
{
    if (stopping) {
        zend_throw_error(NULL, "PHP runtime is shutting down");
        return;
    }
    zval *callback, copy;
    char *s = NULL; size_t n = 0; zend_long delay = 0;
    if (signal) {
        ZEND_PARSE_PARAMETERS_START(2,2) Z_PARAM_STRING(s,n) Z_PARAM_ZVAL(callback) ZEND_PARSE_PARAMETERS_END();
    } else {
        ZEND_PARSE_PARAMETERS_START(2,2) Z_PARAM_LONG(delay) Z_PARAM_ZVAL(callback) ZEND_PARSE_PARAMETERS_END();
    }
    if (!zend_is_callable(callback, 0, NULL)) {
        zend_throw_error(NULL, "Expected a callable");
        return;
    }
    zend_ulong id = ++next_callback;
    ZVAL_COPY(&copy, callback);
    zend_hash_index_add(&callbacks, id, &copy);
    struct PhpMessage m = {.operation=signal ? PHP_HOST_ON_SIGNAL : PHP_HOST_AFTER,
        .text=s, .text_size=n, .a=delay, .b=(int64_t)id};
    struct PhpReply r;
    if (!host_or_throw(&m,&r)) {
        zend_hash_index_del(&callbacks, id);
        return;
    }
    RETURN_LONG(id);
}
PHP_FUNCTION(card_after) { register_callback(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0); }
PHP_FUNCTION(card_on_signal) { register_callback(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1); }

PHP_FUNCTION(project_read)
{
    char *s; size_t n;
    ZEND_PARSE_PARAMETERS_START(1,1) Z_PARAM_STRING(s,n) ZEND_PARSE_PARAMETERS_END();
    struct PhpMessage m = {.operation=PHP_HOST_PROJECT_READ, .text=s, .text_size=n};
    struct PhpReply r;
    if (host_or_throw(&m,&r)) RETURN_STRINGL(r.data ? r.data : "", r.size);
}
PHP_FUNCTION(project_write)
{
    char *s, *data; size_t n, size;
    ZEND_PARSE_PARAMETERS_START(2,2) Z_PARAM_STRING(s,n) Z_PARAM_STRING(data,size) ZEND_PARSE_PARAMETERS_END();
    struct PhpMessage m = {.operation=PHP_HOST_PROJECT_WRITE, .text=s, .text_size=n, .data=data, .data_size=size};
    struct PhpReply r;
    if (host_or_throw(&m,&r)) RETURN_TRUE;
}

ZEND_BEGIN_ARG_INFO_EX(log_args, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, message, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(read_args, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, space_name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, address, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(write_args, 0, 0, 3)
    ZEND_ARG_TYPE_INFO(0, space_name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, address, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, value, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(no_args, 0, 0, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(signal_read_args, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(signal_drive_args, 0, 0, 3)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, millivolts, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, strength, IS_LONG, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(timer_args, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, delay_ns, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(signal_callback_args, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, name, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, callback, IS_CALLABLE, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(project_read_args, 0, 0, 1)
    ZEND_ARG_TYPE_INFO(0, relative_path, IS_STRING, 0)
ZEND_END_ARG_INFO()
ZEND_BEGIN_ARG_INFO_EX(project_write_args, 0, 0, 2)
    ZEND_ARG_TYPE_INFO(0, relative_path, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, bytes, IS_STRING, 0)
ZEND_END_ARG_INFO()
static const zend_function_entry host_functions[] = {
    PHP_FE(card_log, log_args) PHP_FE(card_read, read_args) PHP_FE(card_write, write_args)
    PHP_FE(card_time_ns, no_args) PHP_FE(card_signal_read, signal_read_args)
    PHP_FE(card_signal_drive, signal_drive_args) PHP_FE(card_after, timer_args)
    PHP_FE(card_on_signal, signal_callback_args) PHP_FE(project_read, project_read_args)
    PHP_FE(project_write, project_write_args) PHP_FE_END
};

static int start_engine(void)
{
    php_embed_module.ub_write = output;
    php_embed_module.log_message = log_message;
    php_embed_module.register_server_variables = no_variables;
    php_embed_module.flush = no_flush;
    php_embed_module.deactivate = no_deactivate;
    php_embed_module.php_ini_ignore = 1;
    php_embed_module.php_ini_ignore_cwd = 1;
    sapi_startup(&php_embed_module);
    sapi_started = 1;
    php_embed_module.ini_entries =
        "html_errors=0\n display_errors=0\n log_errors=1\n memory_limit=64M\n"
        "max_execution_time=0\n max_input_time=-1\n variables_order=\n"
        "register_argc_argv=0\n expose_php=0\n enable_dl=0\n allow_url_fopen=0\n"
        "allow_url_include=0\n zend.max_allowed_stack_size=1048576\n"
        "pcre.jit=0\n pcre.backtrack_limit=100000\n pcre.recursion_limit=1000\n";
    php_embed_module.additional_functions = host_functions;
    if (php_module_startup(&php_embed_module, NULL) != SUCCESS) return 0;
    started = 1;
    restrict_runtime();
    for (int op=0; op<=ZEND_VM_LAST_OPCODE; ++op)
        zend_set_user_opcode_handler((uint8_t)op, instruction);
    return 1;
}

static int start_request(void)
{
    if (requested || broken) return 0;
    if (!started) {
        /* A partially initialized engine must never return to the pool. */
        broken = 1;
        if (!start_engine()) return 0;
        broken = 0;
    }
    stopping = poisoned = 0;
    next_callback = 0;
    broken = 1;
#ifdef ZEND_CHECK_STACK_LIMIT
    /* An idle engine may be leased by a different host engine thread. */
    zend_call_stack_init();
#endif
    SG(options) |= SAPI_OPTION_NO_CHDIR;
    if (php_request_startup() != SUCCESS) return 0;
    requested = 1;
    pcre2_set_heap_limit(php_pcre_mctx(), 1024); /* KiB, in addition to work/depth limits */
    SG(headers_sent) = 1;
    SG(request_info).no_headers = 1;
    zend_hash_init(&callbacks, 8, NULL, ZVAL_PTR_DTOR, 0);
    callbacks_ready = 1;
    zval initial;
    object_init(&initial);
    zend_hash_str_update(&EG(symbol_table), "state", 5, &initial);
    broken = 0;
    return 1;
}

static int stop_request(void)
{
    stopping = 1;
    instructions = 0;
    /* User destructors may throw or exhaust their opcode budget. Always reach
       request shutdown, even if destroying retained callbacks bails out. */
    if (callbacks_ready) {
        callbacks_ready = 0;
        zend_first_try {
            zend_hash_destroy(&callbacks);
        } zend_end_try();
    }
    if (requested) {
        volatile int cleaned = 0;
        zend_first_try {
            php_request_shutdown(NULL);
            cleaned = 1;
        } zend_end_try();
        requested = 0;
        if (!cleaned) broken = 1;
    }
    free(saved_json);
    saved_json = NULL;
    host_call = NULL;
    host_context = NULL;
    poisoned = depth = 0;
    last_error[0] = 0;
    return !broken;
}

static int valid_state(zval *value, unsigned level)
{
    if (level > 64) return 0;
    ZVAL_DEREF(value);
    switch (Z_TYPE_P(value)) {
    case IS_NULL: case IS_TRUE: case IS_FALSE: case IS_LONG: case IS_STRING: return 1;
    case IS_DOUBLE: return zend_finite(Z_DVAL_P(value));
    case IS_ARRAY: case IS_OBJECT: {
        if (Z_TYPE_P(value) == IS_OBJECT && Z_OBJCE_P(value) != zend_standard_class_def) return 0;
        HashTable *table = Z_TYPE_P(value) == IS_ARRAY ? Z_ARRVAL_P(value) : Z_OBJPROP_P(value);
        /* JSON has one object type: use stdClass for maps and PHP lists for
           arrays so decoding cannot silently change an associative array into
           an object (or an empty object into a list). */
        if (Z_TYPE_P(value) == IS_ARRAY && !zend_array_is_list(table)) return 0;
        zval *child;
        ZEND_HASH_FOREACH_VAL(table, child) {
            if (!valid_state(child, level+1)) return 0;
        } ZEND_HASH_FOREACH_END();
        return 1;
    }
    default: return 0;
    }
}

static int run(const struct PhpMessage *m, struct PhpReply *r)
{
    if (m->operation == PHP_LOAD) {
        if (!start_request()) return 0;
        zend_string *source = zend_string_init(m->data, m->data_size, 0);
        zend_op_array *code = zend_compile_string(source, m->text, ZEND_COMPILE_POSITION_AT_SHEBANG);
        zend_string_release(source);
        if (!code) return 0;
        zval result;
        ZVAL_UNDEF(&result);
        zend_execute(code, &result);
        zval_ptr_dtor(&result);
        destroy_op_array(code);
        efree(code);
        return !EG(exception);
    }
    if (m->operation == PHP_RELEASE) {
        zend_hash_index_del(&callbacks, (zend_ulong)m->a);
        return 1;
    }
    if (m->operation == PHP_SAVE) {
        zval *value = zend_hash_str_find(&EG(symbol_table), "state", 5);
        if (value) ZVAL_DEREF(value);
        if (!value || !valid_state(value, 0) ||
            Z_TYPE_P(value) != IS_OBJECT) {
            snprintf(r->error, sizeof(r->error), "$state must be a stdClass containing JSON data (stdClass maps and list arrays)");
            return 0;
        }
        smart_str json = {0};
        if (php_json_encode_ex(&json, value, PHP_JSON_PRESERVE_ZERO_FRACTION, 65) != SUCCESS) {
            smart_str_free(&json);
            snprintf(r->error, sizeof(r->error), "$state cannot be encoded as JSON");
            return 0;
        }
        smart_str_0(&json);
        if (!json.s || ZSTR_LEN(json.s) > 1048576 || ZSTR_VAL(json.s)[0] != '{') {
            smart_str_free(&json);
            snprintf(r->error, sizeof(r->error), "$state must be a JSON object of at most 1 MiB");
            return 0;
        }
        free(saved_json);
        saved_json = malloc(ZSTR_LEN(json.s) + 1);
        if (!saved_json) { smart_str_free(&json); return 0; }
        r->size = ZSTR_LEN(json.s);
        memcpy(saved_json, ZSTR_VAL(json.s), r->size+1);
        r->data = saved_json;
        smart_str_free(&json);
        return 1;
    }
    if (m->operation == PHP_RESTORE) {
        if (m->data_size > 1048576) {
            snprintf(r->error, sizeof(r->error), "PHP state exceeds 1 MiB");
            return 0;
        }
        zval value;
        ZVAL_UNDEF(&value);
        /* Zend's JSON scanner expects the trailing NUL of a PHP string. Host
           snapshot payloads are length-delimited and need not have one. */
        zend_string *json = zend_string_init(m->data, m->data_size, 0);
        int decoded = php_json_decode_ex(&value, ZSTR_VAL(json), ZSTR_LEN(json), 0, 66);
        zend_string_release(json);
        if (decoded != SUCCESS ||
            Z_TYPE(value) != IS_OBJECT || !valid_state(&value, 0)) {
            snprintf(r->error, sizeof(r->error), "Invalid PHP state JSON");
            zval_ptr_dtor(&value);
            return 0;
        }
        zval *slot = zend_hash_str_find(&EG(symbol_table), "state", 5);
        if (slot) {
            ZVAL_DEREF(slot);
            zval_ptr_dtor(slot);
            ZVAL_COPY_VALUE(slot, &value);
        } else zend_hash_str_update(&EG(symbol_table), "state", 5, &value);
        return 1;
    }

    zval callable, result, args[2];
    unsigned count = 1;
    ZVAL_UNDEF(&result);
    if (m->operation == PHP_TIMER || m->operation == PHP_SIGNAL) {
        zval *stored = zend_hash_index_find(&callbacks, (zend_ulong)m->a);
        if (!stored) return 1;
        ZVAL_COPY(&callable, stored);
        if (m->operation == PHP_SIGNAL) {
            ZVAL_STRINGL(&args[0], m->text, m->text_size);
            ZVAL_LONG(&args[1], m->b);
            count = 2;
        } else ZVAL_LONG(&args[0], m->b);
    } else {
        const char *name = m->operation == PHP_RESET ? "on_reset" :
            m->operation == PHP_READ ? "on_read" : "on_write";
        if (!zend_hash_str_exists(EG(function_table), name, strlen(name))) return 1;
        ZVAL_STRING(&callable, name);
        if (m->operation == PHP_RESET) ZVAL_BOOL(&args[0], m->a);
        else ZVAL_LONG(&args[0], m->a);
        if (m->operation == PHP_WRITE) { ZVAL_LONG(&args[1], m->b); count = 2; }
    }
    int ok = call_user_function(EG(function_table), NULL, &callable, &result, count, args) == SUCCESS && !EG(exception);
    if (ok && m->operation == PHP_READ) {
        ok = Z_TYPE(result) == IS_LONG && Z_LVAL(result) >= 0 && Z_LVAL(result) <= 255;
        if (ok) r->value = Z_LVAL(result);
        else snprintf(r->error, sizeof(r->error), "on_read must return an integer byte");
    }
    zval_ptr_dtor(&result);
    zval_ptr_dtor(&callable);
    for (unsigned i=0; i<count; ++i) zval_ptr_dtor(&args[i]);
    return ok;
}

static int script_php_run(const struct PhpMessage *m, struct PhpReply *r, PhpHostCall callback, void *context)
{
    /* Threads created by the host's libc do not initialize this namespace's
       libc ctype TLS. The public locale API initializes it on every entry,
       including when a leased request moves to the simulation thread. */
    uselocale(LC_GLOBAL_LOCALE);
#ifdef ZEND_CHECK_STACK_LIMIT
    if (started && !depth) zend_call_stack_init();
#endif
    memset(r, 0, sizeof(*r));
    host_call = callback;
    host_context = context;
    if (m->operation == PHP_STOP) return stop_request();
    if (m->operation == PHP_SHUTDOWN) {
        stop_request();
        zend_first_try {
            if (started) php_module_shutdown();
        } zend_end_try();
        if (sapi_started) sapi_shutdown();
        started = sapi_started = 0;
        return 1;
    }
    if (broken || (m->operation != PHP_LOAD && !requested)) {
        snprintf(r->error, sizeof(r->error), "PHP engine is unavailable or has no active request");
        return 0;
    }
    if (poisoned) {
        snprintf(r->error, sizeof(r->error), "PHP runtime stopped after a fatal error; reset or reload the script");
        return 0;
    }
    if (!depth) { instructions = 0; last_error[0] = 0; }
    if (++depth > 64) {
        --depth;
        snprintf(r->error, sizeof(r->error), "PHP callback nesting exceeds 64 levels");
        return 0;
    }
    volatile int ok = 0;
    zend_try {
        ok = run(m, r);
        if (EG(exception)) {
            zval temp;
            zval *message = zend_read_property(EG(exception)->ce, EG(exception), "message", 7, 1, &temp);
            snprintf(r->error, sizeof(r->error), "%s", message && Z_TYPE_P(message) == IS_STRING
                ? Z_STRVAL_P(message) : "PHP exception");
            zend_clear_exception();
            ok = 0;
        }
    } zend_catch {
        poisoned = 1;
        ok = 0;
    } zend_end_try();
    --depth;
    if (!ok && !r->error[0]) snprintf(r->error, sizeof(r->error), "%s",
        last_error[0] ? last_error : "PHP execution failed");
    return ok;
}

__attribute__((visibility("default")))
const struct PhpBridgeApi *script_php_get_api(void)
{
    static const struct PhpBridgeApi api = {
        SCRIPT_PHP_BRIDGE_ABI, sizeof(struct PhpBridgeApi), script_php_run
    };
    return &api;
}
