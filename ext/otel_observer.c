
#include "php.h"
#include "otel_observer.h"
#include "zend_observer.h"
#include "zend_execute.h"
#include "zend_extensions.h"
#include "zend_exceptions.h"
#include "zend_attributes.h"
#include "php_opentelemetry.h"

static int op_array_extension = -1;

static char *with_span_fqn = "OpenTelemetry\\Instrumentation\\WithSpan";
static char *span_attribute_fqn =
    "OpenTelemetry\\Instrumentation\\SpanAttribute";
// static char *with_span_fqn_lc = "opentelemetry\\instrumentation\\withspan";

static char *attribute_args_keys[] = {"name", "span_kind", "attributes"};

typedef struct otel_observer {
    zend_llist pre_hooks;
    zend_llist post_hooks;
} otel_observer;

typedef struct otel_exception_state {
    zend_object *exception;
    zend_object *prev_exception;
    const zend_op *opline_before_exception;
    bool has_opline;
    const zend_op *opline;
} otel_exception_state;

#define STACK_EXTENSION_LIMIT 16

typedef struct otel_arg_locator {
    zend_execute_data *execute_data;
    // Number of argument slots reserved in execute_data, any arguments beyond
    // this limit will be stored after auxiliary slots
    uint32_t reserved;
    // Number of arguments provided at the call site. May exceed the number of
    // arguments in the function definition
    uint32_t provided;
    // Number of slots between reserved and "extra" argument slots
    uint32_t auxiliary_slots;
    uint32_t extended_start;
    uint32_t extended_max;
    uint32_t extended_used;
    zval extended_slots[STACK_EXTENSION_LIMIT];
} otel_arg_locator;

static inline void
func_get_this_or_called_scope(zval *zv, zend_execute_data *execute_data) {
    if (execute_data->func->op_array.scope) {
        if (execute_data->func->op_array.fn_flags & ZEND_ACC_STATIC) {
            zend_class_entry *called_scope =
                zend_get_called_scope(execute_data);
            ZVAL_STR(zv, called_scope->name);
        } else {
            zend_object *this = zend_get_this_object(execute_data);
            ZVAL_OBJ_COPY(zv, this);
        }
    } else {
        ZVAL_NULL(zv);
    }
}

static inline void func_get_function_name(zval *zv, zend_execute_data *ex) {
    ZVAL_STR_COPY(zv, ex->func->op_array.function_name);
}

// get function args (invo zv) and any args with the
// SpanAttributes attribute applied (into zv_attrs)
static void func_get_args(zval *zv, zval *zv_attrs, zend_execute_data *ex,
                          bool check_for_attributes) {
    zval *p, *q;
    uint32_t i, first_extra_arg;
    uint32_t arg_count = ZEND_CALL_NUM_ARGS(ex);
    HashTable *ht; // attributes from SpanAttribute
    if (check_for_attributes) {
        ALLOC_HASHTABLE(ht);
        zend_hash_init(ht, 0, NULL, ZVAL_PTR_DTOR, 0);
    }

    // @see
    // https://github.com/php/php-src/blob/php-8.1.0/Zend/zend_builtin_functions.c#L235
    if (arg_count) {
        array_init_size(zv, arg_count);
        if (ex->func->type == ZEND_INTERNAL_FUNCTION) {
            first_extra_arg = arg_count;
        } else {
            first_extra_arg = ex->func->op_array.num_args;
        }
        zend_hash_real_init_packed(Z_ARRVAL_P(zv));
        ZEND_HASH_FILL_PACKED(Z_ARRVAL_P(zv)) {
            i = 0;
            p = ZEND_CALL_ARG(ex, 1);
            if (arg_count > first_extra_arg) {
                while (i < first_extra_arg) {
                    q = p;
                    if (EXPECTED(Z_TYPE_INFO_P(q) != IS_UNDEF)) {
                        ZVAL_DEREF(q);
                        if (Z_OPT_REFCOUNTED_P(q)) {
                            Z_ADDREF_P(q);
                        }
                        ZEND_HASH_FILL_SET(q);
                    } else {
                        ZEND_HASH_FILL_SET_NULL();
                    }
                    ZEND_HASH_FILL_NEXT();
                    p++;
                    i++;
                }
                p = ZEND_CALL_VAR_NUM(ex, ex->func->op_array.last_var +
                                              ex->func->op_array.T);
            }
            while (i < arg_count) {
                if (check_for_attributes && OTEL_G(attr_hooks_enabled) &&
                    ex->func->type != ZEND_INTERNAL_FUNCTION) {
                    zend_string *arg_name = ex->func->op_array.vars[i];
                    zend_attribute *attribute =
                        zend_get_parameter_attribute_str(
                            ex->func->common.attributes,
                            "opentelemetry\\instrumentation\\spanattribute",
                            sizeof("opentelemetry\\instrumentation\\spanattribu"
                                   "te") -
                                1,
                            i);
                    bool has_span_attribute = attribute != NULL;
                    if (has_span_attribute) {
                        if (attribute->argc) {
                            zval key = attribute->args[0].value;
                            zend_hash_add(ht, Z_STR(key), p);
                        } else {
                            zend_hash_add(ht, arg_name, p);
                        }
                    }
                }
                q = p;
                if (EXPECTED(Z_TYPE_INFO_P(q) != IS_UNDEF)) {
                    ZVAL_DEREF(q);
                    if (Z_OPT_REFCOUNTED_P(q)) {
                        Z_ADDREF_P(q);
                    }
                    ZEND_HASH_FILL_SET(q);
                } else {
                    ZEND_HASH_FILL_SET_NULL();
                }
                ZEND_HASH_FILL_NEXT();
                p++;
                i++;
            }
        }
        ZEND_HASH_FILL_END();
        Z_ARRVAL_P(zv)->nNumOfElements = arg_count;
    } else {
        ZVAL_EMPTY_ARRAY(zv);
    }
    if (zv_attrs) {
        ZVAL_ARR(zv_attrs, ht);
    }
}

static uint32_t func_get_arg_index_by_name(zend_execute_data *execute_data,
                                           zend_string *arg_name) {
    // @see
    // https://github.com/php/php-src/blob/php-8.1.0/Zend/zend_execute.c#L4515
    zend_function *fbc = execute_data->func;
    uint32_t num_args = fbc->common.num_args;
    if (EXPECTED(fbc->type == ZEND_USER_FUNCTION) ||
        EXPECTED(fbc->common.fn_flags & ZEND_ACC_USER_ARG_INFO)) {
        for (uint32_t i = 0; i < num_args; i++) {
            zend_arg_info *arg_info = &fbc->op_array.arg_info[i];
            if (zend_string_equals(arg_name, arg_info->name)) {
                return i;
            }
        }
    } else {
        for (uint32_t i = 0; i < num_args; i++) {
            zend_internal_arg_info *arg_info =
                &fbc->internal_function.arg_info[i];
            size_t len = strlen(arg_info->name);
            if (len == ZSTR_LEN(arg_name) &&
                !memcmp(arg_info->name, ZSTR_VAL(arg_name), len)) {
                return i;
            }
        }
    }

    return (uint32_t)-1;
}

static inline void func_get_retval(zval *zv, zval *retval) {
    if (UNEXPECTED(!retval || Z_ISUNDEF_P(retval))) {
        ZVAL_NULL(zv);
    } else {
        ZVAL_COPY(zv, retval);
    }
}

static inline void func_get_exception(zval *zv) {
    zend_object *exception = EG(exception);
    if (exception && zend_is_unwind_exit(exception)) {
        ZVAL_NULL(zv);
    } else if (UNEXPECTED(exception)) {
        ZVAL_OBJ_COPY(zv, exception);
    } else {
        ZVAL_NULL(zv);
    }
}

static inline void func_get_declaring_scope(zval *zv, zend_execute_data *ex) {
    if (ex->func->op_array.scope) {
        ZVAL_STR_COPY(zv, ex->func->op_array.scope->name);
    } else {
        ZVAL_NULL(zv);
    }
}

static inline void func_get_filename(zval *zv, zend_execute_data *ex) {
    if (ex->func->type != ZEND_INTERNAL_FUNCTION) {
        ZVAL_STR_COPY(zv, ex->func->op_array.filename);
    } else {
        ZVAL_NULL(zv);
    }
}

static inline void func_get_lineno(zval *zv, zend_execute_data *ex) {
    if (ex->func->type != ZEND_INTERNAL_FUNCTION) {
        ZVAL_LONG(zv, ex->func->op_array.line_start);
    } else {
        ZVAL_NULL(zv);
    }
}

static inline void func_get_attribute_args(zval *zv, zend_execute_data *ex) {
    zend_attribute *attr;
    attr = zend_get_attribute_str(
        ex->func->common.attributes, "opentelemetry\\instrumentation\\withspan",
        sizeof("opentelemetry\\instrumentation\\withspan") - 1);
    if (attr == NULL || attr->argc == 0) {
        ZVAL_EMPTY_ARRAY(zv);
        return;
    }

    HashTable *ht;
    ALLOC_HASHTABLE(ht);
    zend_hash_init(ht, attr->argc, NULL, ZVAL_PTR_DTOR, 0);
    zend_attribute_arg arg;
    zend_string *key;

    for (uint32_t i = 0; i < attr->argc; i++) {
        arg = attr->args[i];
        if (arg.name != NULL) {
            zend_hash_add(ht, arg.name, &arg.value);
        } else {
            key = zend_string_init(attribute_args_keys[i],
                                   strlen(attribute_args_keys[i]), 0);
            zend_hash_add(ht, key, &arg.value);
            zend_string_release(key); // todo move to end?
        }
    }

    ZVAL_ARR(zv, ht);
}

/**
 * Check if the object implements or extends the specified class
 */
bool is_object_compatible_with_type_hint(zval *object_zval,
                                         zend_class_entry *type_hint) {
    zend_class_entry *object_ce = Z_OBJCE_P(object_zval);
    return instanceof_function(object_ce, type_hint);
}

/**
 * Check if pre/post function callback's signature is compatible with
 * the expected function signature.
 * This is a runtime check, since some parameters are only known at runtime.
 * Can be disabled via the opentelemetry.check_hook_functions ini value.
 */
static inline bool is_valid_signature(zend_fcall_info fci,
                                      zend_fcall_info_cache fcc) {
    if (OTEL_G(validate_hook_functions) == 0) {
        return 1;
    }
    zend_function *func = fcc.function_handler;
    zend_arg_info *arg_info;
    zend_type *arg_type;
    uint32_t type;
    uint32_t type_mask;

    for (uint32_t i = 0; i < func->common.num_args; i++) {
        // get type mask of callback argument
        arg_info = &func->common.arg_info[i];
        arg_type = &arg_info->type;
        type_mask = arg_type->type_mask;

        // get actual value + type
        zval param = fci.params[i];
        type = Z_TYPE(fci.params[i]);

        if (type_mask == IS_UNDEF) {
            // no type mask -> ok
        } else if (Z_TYPE(param) == IS_OBJECT) {
            // object special-case handling (check for interfaces, subclasses)
            zend_class_entry *ce = Z_OBJCE(param);
            if (!is_object_compatible_with_type_hint(&param, ce)) {
                return false;
            }
        } else if ((type_mask & (1 << type)) == 0) {
            // type is not compatible with mask
            return false;
        }
    }

    return true;
}

static void exception_isolation_start(otel_exception_state *save_state) {
    save_state->exception = EG(exception);
    save_state->prev_exception = EG(prev_exception);
    save_state->opline_before_exception = EG(opline_before_exception);

    EG(exception) = NULL;
    EG(prev_exception) = NULL;
    EG(opline_before_exception) = NULL;

    // If the hook handler throws an exception, the execute_data of the outer
    // frame may have its opline set to an exception handler too. This is done
    // before the chance to clear the exception, so opline has to be restored
    // to original value.
    zend_execute_data *execute_data = EG(current_execute_data);
    if (execute_data != NULL) {
        save_state->has_opline = true;
        save_state->opline = execute_data->opline;
    } else {
        save_state->has_opline = false;
        save_state->opline = NULL;
    }
}

static zend_object *exception_isolation_end(otel_exception_state *save_state) {
    zend_object *suppressed = EG(exception);
    // NULL this before call to zend_clear_exception, as it would try to jump
    // to exception handler then.
    EG(exception) = NULL;

    // this clears prev_exception if it was set for any reason
    zend_clear_exception();

    EG(exception) = save_state->exception;
    EG(prev_exception) = save_state->prev_exception;
    EG(opline_before_exception) = save_state->opline_before_exception;

    zend_execute_data *execute_data = EG(current_execute_data);
    if (execute_data != NULL && save_state->has_opline) {
        execute_data->opline = save_state->opline;
    }

    return suppressed;
}

static const char *zval_get_chars(zval *zv) {
    if (zv != NULL && Z_TYPE_P(zv) == IS_STRING) {
        return Z_STRVAL_P(zv);
    }
    return "null";
}

static void exception_isolation_handle_exception(zend_object *suppressed,
                                                 zval *class_name,
                                                 zval *function_name,
                                                 const char *type) {
    if (suppressed == NULL) {
        return;
    }

    zend_class_entry *exception_base = zend_get_exception_base(suppressed);
    zval return_value;
    zval *message =
        zend_read_property_ex(exception_base, suppressed,
                              ZSTR_KNOWN(ZEND_STR_MESSAGE), 1, &return_value);

    php_error_docref(NULL, E_CORE_WARNING,
                     "OpenTelemetry: %s threw exception,"
                     " class=%s function=%s message=%s",
                     type, zval_get_chars(class_name),
                     zval_get_chars(function_name), zval_get_chars(message));

    if (message != NULL) {
        ZVAL_DEREF(message);
    }

    OBJ_RELEASE(suppressed);
}

static void arg_locator_initialize(otel_arg_locator *arg_locator,
                                   zend_execute_data *execute_data) {
    arg_locator->execute_data = execute_data;

    if (execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
        // For internal functions, rather than having reserved number of slots
        // before auxiliary slots and extra ones after that, internal functions
        // have all (and only) arguments provided by call site before auxiliary
        // slots, and there is nothing after auxiliary slots.
        arg_locator->reserved = ZEND_CALL_NUM_ARGS(execute_data);
    } else {
        arg_locator->reserved = execute_data->func->op_array.last_var;
    }

    arg_locator->provided = ZEND_CALL_NUM_ARGS(execute_data);
    arg_locator->auxiliary_slots = execute_data->func->op_array.T;

    arg_locator->extended_used = 0;
    arg_locator->extended_start = arg_locator->provided > arg_locator->reserved
                                      ? arg_locator->provided
                                      : arg_locator->reserved;

    if (OTEL_G(allow_stack_extension)) {
        arg_locator->extended_max = STACK_EXTENSION_LIMIT;

        size_t slots_left_in_stack = EG(vm_stack_end) - EG(vm_stack_top);
        if (slots_left_in_stack < arg_locator->extended_max) {
            arg_locator->extended_max = slots_left_in_stack;
        }
    } else {
        arg_locator->extended_max = 0;
    }
}

static zval *arg_locator_get_slot(otel_arg_locator *arg_locator, uint32_t index,
                                  const char **failure_reason) {

    if (index < arg_locator->reserved) {
        return ZEND_CALL_ARG(arg_locator->execute_data, index + 1);
    } else if (index < arg_locator->provided) {
        return ZEND_CALL_ARG(arg_locator->execute_data,
                             index + arg_locator->auxiliary_slots + 1);
    }

    uint32_t extended_index = index - arg_locator->extended_start;

    if (extended_index < arg_locator->extended_max) {
        uint32_t extended_index = index - arg_locator->extended_start;
        if (extended_index >= arg_locator->extended_used) {
            arg_locator->extended_used = extended_index + 1;
        }

        return &arg_locator->extended_slots[extended_index];
    }

    if (failure_reason != NULL) {
        // Having a hardcoded upper limit allows performing stack
        // extension as one step in the end, rather than moving slots around in
        // the stack each time a new argument is discovered
        if (extended_index >= STACK_EXTENSION_LIMIT) {
            *failure_reason = "exceeds built-in stack extension limit";
        } else if (!OTEL_G(allow_stack_extension)) {
            *failure_reason = "stack extension must be enabled with "
                              "opentelemetry.allow_stack_extension option";
        } else {
            *failure_reason = "not enough room left in stack page";
        }
    }

    return NULL;
}

static void arg_locator_store_extended(otel_arg_locator *arg_locator) {
    if (arg_locator->extended_used == 0) {
        return;
    }

    // This is safe because extended_max is adjusted to not exceed current stack
    // page end
    EG(vm_stack_top) += arg_locator->extended_used;

    if (arg_locator->execute_data->func->type == ZEND_INTERNAL_FUNCTION) {
        // For internal functions, the additional arguments need to go before
        // the auxiliary slots, therefore the auxiliary slots need to be moved
        // ahead
        zval *target =
            ZEND_CALL_ARG(arg_locator->execute_data, arg_locator->provided + 1);
        zval *aux_target = target + arg_locator->extended_used;

        memmove(aux_target, target,
                sizeof(*aux_target) * arg_locator->auxiliary_slots);
        memcpy(target, arg_locator->extended_slots,
               sizeof(*target) * arg_locator->extended_used);
    } else {
        // For PHP functions, the additional arguments go to the end of the
        // frame, so nothing else needs to be moved around
        zval *target = ZEND_CALL_ARG(arg_locator->execute_data,
                                     arg_locator->extended_start +
                                         arg_locator->auxiliary_slots + 1);
        memcpy(target, arg_locator->extended_slots,
               sizeof(*target) * arg_locator->extended_used);
    }
}

static void observer_begin(zend_execute_data *execute_data, zend_llist *hooks) {
    if (!zend_llist_count(hooks)) {
        return;
    }

    zval params[8];
    uint32_t param_count = 8;

    func_get_this_or_called_scope(&params[0], execute_data);
    func_get_args(&params[1], &params[7], execute_data, true);
    func_get_declaring_scope(&params[2], execute_data);
    func_get_function_name(&params[3], execute_data);
    func_get_filename(&params[4], execute_data);
    func_get_lineno(&params[5], execute_data);
    func_get_attribute_args(&params[6], execute_data);

    for (zend_llist_element *element = hooks->head; element;
         element = element->next) {
        zend_fcall_info fci = empty_fcall_info;
        zend_fcall_info_cache fcc = empty_fcall_info_cache;
        if (UNEXPECTED(zend_fcall_info_init((zval *)element->data, 0, &fci,
                                            &fcc, NULL, NULL) != SUCCESS)) {
            continue;
        }

        zval ret = {.u1.type_info = IS_UNDEF};
        fci.param_count = param_count;
        fci.params = params;
        fci.named_params = NULL;
        fci.retval = &ret;

        if (!is_valid_signature(fci, fcc)) {
            php_error_docref(NULL, E_CORE_WARNING,
                             "OpenTelemetry: pre hook invalid signature,"
                             " class=%s function=%s",
                             (Z_TYPE_P(&params[2]) == IS_NULL)
                                 ? "null"
                                 : Z_STRVAL_P(&params[2]),
                             Z_STRVAL_P(&params[3]));
            continue;
        }

        otel_exception_state save_state;
        exception_isolation_start(&save_state);

        if (zend_call_function(&fci, &fcc) == SUCCESS) {
            if (Z_TYPE(ret) == IS_ARRAY &&
                !zend_is_identical(&ret, &params[1])) {
                zend_ulong idx;
                zend_string *str_idx;
                zval *val;
                bool invalid_arg_warned = false;

                otel_arg_locator arg_locator;
                arg_locator_initialize(&arg_locator, execute_data);
                uint32_t args_initialized = arg_locator.provided;

                ZEND_HASH_FOREACH_KEY_VAL(Z_ARR(ret), idx, str_idx, val) {
                    const char *failure_reason = "";

                    if (str_idx != NULL) {
                        idx = func_get_arg_index_by_name(execute_data, str_idx);

                        if (idx == (uint32_t)-1) {
                            php_error_docref(
                                NULL, E_CORE_WARNING,
                                "OpenTelemetry: pre hook unknown "
                                "named arg %s, class=%s function=%s",
                                ZSTR_VAL(str_idx), zval_get_chars(&params[2]),
                                zval_get_chars(&params[3]));
                            continue;
                        }
                    }

                    zval *target = arg_locator_get_slot(&arg_locator, idx,
                                                        &failure_reason);

                    if (target == NULL) {
                        if (invalid_arg_warned) {
                            continue;
                        }

                        php_error_docref(NULL, E_CORE_WARNING,
                                         "OpenTelemetry: pre hook invalid "
                                         "argument index " ZEND_ULONG_FMT
                                         " - %s, class=%s function=%s",
                                         idx, failure_reason,
                                         zval_get_chars(&params[2]),
                                         zval_get_chars(&params[3]));
                        invalid_arg_warned = true;
                        continue;
                    }

                    if (idx >= args_initialized) {
                        // This slot was not initialized, need to initialize
                        // all slots between current and the last initialized
                        // one
                        for (uint32_t i = args_initialized; i < idx; i++) {
                            ZVAL_UNDEF(
                                arg_locator_get_slot(&arg_locator, i, NULL));
                            ZEND_ADD_CALL_FLAG(execute_data,
                                               ZEND_CALL_MAY_HAVE_UNDEF);
                        }

                        args_initialized = idx + 1;
                    } else {
                        // This slot was already initialized, need to
                        // decrement refcount before overwriting
                        zval_dtor(target);
                    }

                    if (idx >= arg_locator.reserved && Z_REFCOUNTED_P(val)) {
                        // If there are any "extra parameters" that are
                        // refcounted, then this flag must be set. While we
                        // cannot add any new extra parameter slots, this flag
                        // may not have been present because all the values
                        // were previously not refcounted
                        ZEND_ADD_CALL_FLAG(execute_data,
                                           ZEND_CALL_FREE_EXTRA_ARGS);
                    }

                    ZVAL_COPY(target, val);

                    if (idx < arg_locator.provided &&
                        Z_TYPE(params[1]) == IS_ARRAY) {
                        // This index is present in the array provided to begin
                        // hook, update it in that array as well
                        Z_TRY_ADDREF_P(val);
                        zend_hash_index_update(Z_ARR(params[1]), idx, val);
                    }
                }
                ZEND_HASH_FOREACH_END();

                arg_locator_store_extended(&arg_locator);

                // Update provided argument count if begin hook added arguments
                // that were not provided originally
                if (args_initialized > arg_locator.provided) {
                    ZEND_CALL_NUM_ARGS(execute_data) = args_initialized;
                }
            }
        }

        zend_object *suppressed = exception_isolation_end(&save_state);
        exception_isolation_handle_exception(suppressed, &params[2], &params[3],
                                             "pre hook");

        zval_dtor(&ret);
    }

    if (UNEXPECTED(ZEND_CALL_INFO(execute_data) & ZEND_CALL_MAY_HAVE_UNDEF)) {
        zend_object *exception = EG(exception);
        EG(exception) = (void *)(uintptr_t)-1;
        if (zend_handle_undef_args(execute_data) == FAILURE) {
            uint32_t arg_count = ZEND_CALL_NUM_ARGS(execute_data);
            for (uint32_t i = 0; i < arg_count; i++) {
                zval *arg = ZEND_CALL_VAR_NUM(execute_data, i);
                if (!Z_ISUNDEF_P(arg)) {
                    continue;
                }

                ZVAL_NULL(arg);
            }
        }
        EG(exception) = exception;
    }

    for (size_t i = 0; i < param_count; i++) {
        zval_dtor(&params[i]);
    }
}

static void observer_end(zend_execute_data *execute_data, zval *retval,
                         zend_llist *hooks) {
    if (!zend_llist_count(hooks)) {
        return;
    }

    zval params[8];
    uint32_t param_count = 8;

    func_get_this_or_called_scope(&params[0], execute_data);
    func_get_args(&params[1], NULL, execute_data, false);
    func_get_retval(&params[2], retval);
    func_get_exception(&params[3]);
    func_get_declaring_scope(&params[4], execute_data);
    func_get_function_name(&params[5], execute_data);
    func_get_filename(&params[6], execute_data);
    func_get_lineno(&params[7], execute_data);

    for (zend_llist_element *element = hooks->tail; element;
         element = element->prev) {
        zend_fcall_info fci = empty_fcall_info;
        zend_fcall_info_cache fcc = empty_fcall_info_cache;
        if (UNEXPECTED(zend_fcall_info_init((zval *)element->data, 0, &fci,
                                            &fcc, NULL, NULL) != SUCCESS)) {
            continue;
        }

        zval ret = {.u1.type_info = IS_UNDEF};
        fci.param_count = param_count;
        fci.params = params;
        fci.named_params = NULL;
        fci.retval = &ret;

        if (!is_valid_signature(fci, fcc)) {
            php_error_docref(NULL, E_CORE_WARNING,
                             "OpenTelemetry: post hook invalid signature, "
                             "class=%s function=%s",
                             (Z_TYPE_P(&params[4]) == IS_NULL)
                                 ? "null"
                                 : Z_STRVAL_P(&params[4]),
                             Z_STRVAL_P(&params[5]));
            continue;
        }

        otel_exception_state save_state;
        exception_isolation_start(&save_state);

        if (zend_call_function(&fci, &fcc) == SUCCESS) {
            /* TODO rather than ignoring return value if post callback doesn't
               have a return type-hint, could we check whether the types are
               compatible and allow if they are? */
            if (!Z_ISUNDEF(ret) &&
                (fcc.function_handler->op_array.fn_flags &
                 ZEND_ACC_HAS_RETURN_TYPE) &&
                !(ZEND_TYPE_PURE_MASK(
                      fcc.function_handler->common.arg_info[-1].type) &
                  MAY_BE_VOID)) {
                if (execute_data->return_value) {
                    zval_ptr_dtor(execute_data->return_value);
                    ZVAL_COPY(execute_data->return_value, &ret);
                    zval_ptr_dtor(&params[2]);
                    ZVAL_COPY_VALUE(&params[2], &ret);
                    ZVAL_UNDEF(&ret);
                }
            }
        }

        zend_object *suppressed = exception_isolation_end(&save_state);
        exception_isolation_handle_exception(suppressed, &params[4], &params[5],
                                             "post hook");

        zval_dtor(&ret);
    }

    for (size_t i = 0; i < param_count; i++) {
        zval_dtor(&params[i]);
    }
}

static void observer_begin_handler(zend_execute_data *execute_data) {
    otel_observer *observer = ZEND_OP_ARRAY_EXTENSION(
        &execute_data->func->op_array, op_array_extension);
    if (!observer || !zend_llist_count(&observer->pre_hooks)) {
        return;
    }

    observer_begin(execute_data, &observer->pre_hooks);
}

static void observer_end_handler(zend_execute_data *execute_data,
                                 zval *retval) {
    otel_observer *observer = ZEND_OP_ARRAY_EXTENSION(
        &execute_data->func->op_array, op_array_extension);
    if (!observer || !zend_llist_count(&observer->post_hooks)) {
        return;
    }

    observer_end(execute_data, retval, &observer->post_hooks);
}

static void free_observer(otel_observer *observer) {
    zend_llist_destroy(&observer->pre_hooks);
    zend_llist_destroy(&observer->post_hooks);
    efree(observer);
}

static void init_observer(otel_observer *observer) {
    zend_llist_init(&observer->pre_hooks, sizeof(zval),
                    (llist_dtor_func_t)zval_ptr_dtor, 0);
    zend_llist_init(&observer->post_hooks, sizeof(zval),
                    (llist_dtor_func_t)zval_ptr_dtor, 0);
}

static otel_observer *create_observer() {
    otel_observer *observer = emalloc(sizeof(otel_observer));
    init_observer(observer);
    return observer;
}

static void copy_observer(otel_observer *source, otel_observer *destination) {
    destination->pre_hooks = source->pre_hooks;
    destination->post_hooks = source->post_hooks;
}

static bool find_observers(HashTable *ht, zend_string *n, zend_llist *pre_hooks,
                           zend_llist *post_hooks) {
    otel_observer *observer = zend_hash_find_ptr_lc(ht, n);
    if (observer) {
        for (zend_llist_element *element = observer->pre_hooks.head; element;
             element = element->next) {
            zval_add_ref((zval *)&element->data);
            zend_llist_add_element(pre_hooks, &element->data);
        }
        for (zend_llist_element *element = observer->post_hooks.head; element;
             element = element->next) {
            zval_add_ref((zval *)&element->data);
            zend_llist_add_element(post_hooks, &element->data);
        }
        return true;
    }
    return false;
}

static void find_class_observers(HashTable *ht, HashTable *type_visited_lookup,
                                 zend_class_entry *ce, zend_llist *pre_hooks,
                                 zend_llist *post_hooks) {
    for (; ce; ce = ce->parent) {
        // Omit type if it was already visited
        if (zend_hash_exists(type_visited_lookup, ce->name)) {
            continue;
        }
        if (find_observers(ht, ce->name, pre_hooks, post_hooks)) {
            zend_hash_add_empty_element(type_visited_lookup, ce->name);
        }
        for (uint32_t i = 0; i < ce->num_interfaces; i++) {
            find_class_observers(ht, type_visited_lookup, ce->interfaces[i],
                                 pre_hooks, post_hooks);
        }
    }
}

static void find_method_observers(HashTable *ht, zend_class_entry *ce,
                                  zend_string *fn, zend_llist *pre_hooks,
                                  zend_llist *post_hooks) {
    // Below hashtable stores information
    // whether type was already visited
    // This information is used to prevent
    // adding hooks more than once in the case
    // of extensive class hierarchy
    HashTable type_visited_lookup;
    zend_hash_init(&type_visited_lookup, 8, NULL, NULL, 0);
    HashTable *lookup = zend_hash_find_ptr_lc(ht, fn);
    if (lookup) {
        find_class_observers(lookup, &type_visited_lookup, ce, pre_hooks,
                             post_hooks);
    }
    zend_hash_destroy(&type_visited_lookup);
}

static zval create_attribute_observer_handler(char *fn) {
    zval callable;
    ZVAL_STRING(&callable, fn);

    return callable;
}

static otel_observer *resolve_observer(zend_execute_data *execute_data) {
    zend_function *fbc = execute_data->func;
    if (!fbc->common.function_name) {
        return NULL;
    }
    bool has_withspan_attribute = false;

    if (fbc->common.attributes != NULL) {
        zend_attribute *attr;
        attr = zend_get_attribute_str(
            fbc->common.attributes, "opentelemetry\\instrumentation\\withspan",
            sizeof("opentelemetry\\instrumentation\\withspan") - 1);
        has_withspan_attribute = (attr != NULL);
    }

    otel_observer observer_instance;
    init_observer(&observer_instance);

    if (fbc->op_array.scope) {
        find_method_observers(OTEL_G(observer_class_lookup),
                              fbc->op_array.scope, fbc->common.function_name,
                              &observer_instance.pre_hooks,
                              &observer_instance.post_hooks);
    } else {
        find_observers(OTEL_G(observer_function_lookup),
                       fbc->common.function_name, &observer_instance.pre_hooks,
                       &observer_instance.post_hooks);
    }

    if (!zend_llist_count(&observer_instance.pre_hooks) &&
        !zend_llist_count(&observer_instance.post_hooks)) {
        if (OTEL_G(attr_hooks_enabled) && has_withspan_attribute) {
            // there are no observers registered for this function/method, but
            // it has a WithSpan attribute. Add our generic pre/post handlers
            // as new observers.
            zval pre = create_attribute_observer_handler(
                OTEL_G(pre_handler_function_fqn));
            zval post = create_attribute_observer_handler(
                OTEL_G(post_handler_function_fqn));
            add_observer(fbc->op_array.scope ? fbc->op_array.scope->name : NULL,
                         fbc->common.function_name, &pre, &post);
            zval_ptr_dtor(&pre);
            zval_ptr_dtor(&post);
            // re-find to update pre/post hooks
            if (fbc->op_array.scope) {
                find_method_observers(
                    OTEL_G(observer_class_lookup), fbc->op_array.scope,
                    fbc->common.function_name, &observer_instance.pre_hooks,
                    &observer_instance.post_hooks);
            } else {
                find_observers(OTEL_G(observer_function_lookup),
                               fbc->common.function_name,
                               &observer_instance.pre_hooks,
                               &observer_instance.post_hooks);
            }

            if (!zend_llist_count(&observer_instance.pre_hooks) &&
                !zend_llist_count(&observer_instance.post_hooks)) {
                // failed to add hooks?
                return NULL;
            }
        } else {
            return NULL;
        }
    }
    otel_observer *observer = create_observer();
    copy_observer(&observer_instance, observer);
    zend_hash_next_index_insert_ptr(OTEL_G(observer_aggregates), observer);

    return observer;
}

static zend_observer_fcall_handlers
observer_fcall_init(zend_execute_data *execute_data) {
    // This means that either RINIT has not yet been called, or RSHUTDOWN has
    // already been called. The former can happen if another extension that is
    // loaded before this one invokes PHP functions in its RINIT. The latter
    // can happen if a header callback is set or when another extension invokes
    // PHP functions in their RSHUTDOWN.
    if (OTEL_G(observer_class_lookup) == NULL) {
        return (zend_observer_fcall_handlers){NULL, NULL};
    }

    if (op_array_extension == -1) {
        return (zend_observer_fcall_handlers){NULL, NULL};
    }

    otel_observer *observer = resolve_observer(execute_data);
    if (!observer) {
        return (zend_observer_fcall_handlers){NULL, NULL};
    }

    ZEND_OP_ARRAY_EXTENSION(&execute_data->func->op_array, op_array_extension) =
        observer;
    return (zend_observer_fcall_handlers){
        zend_llist_count(&observer->pre_hooks) ? observer_begin_handler : NULL,
        zend_llist_count(&observer->post_hooks) ? observer_end_handler : NULL,
    };
}

static void destroy_observer_lookup(zval *zv) { free_observer(Z_PTR_P(zv)); }

static void destroy_observer_class_lookup(zval *zv) {
    HashTable *table = Z_PTR_P(zv);
    zend_hash_destroy(table);
    FREE_HASHTABLE(table);
}

static void add_function_observer(HashTable *ht, zend_string *fn,
                                  zval *pre_hook, zval *post_hook) {
    zend_string *lc = zend_string_tolower(fn);
    otel_observer *observer = zend_hash_find_ptr(ht, lc);
    if (!observer) {
        observer = create_observer();
        zend_hash_update_ptr(ht, lc, observer);
    }
    zend_string_release(lc);

    if (pre_hook) {
        zval_add_ref(pre_hook);
        zend_llist_add_element(&observer->pre_hooks, pre_hook);
    }
    if (post_hook) {
        zval_add_ref(post_hook);
        zend_llist_add_element(&observer->post_hooks, post_hook);
    }
}

static void add_method_observer(HashTable *ht, zend_string *cn, zend_string *fn,
                                zval *pre_hook, zval *post_hook) {
    zend_string *lc = zend_string_tolower(fn);
    HashTable *function_table = zend_hash_find_ptr(ht, lc);
    if (!function_table) {
        ALLOC_HASHTABLE(function_table);
        zend_hash_init(function_table, 8, NULL, destroy_observer_lookup, 0);
        zend_hash_update_ptr(ht, lc, function_table);
    }
    zend_string_release(lc);

    add_function_observer(function_table, cn, pre_hook, post_hook);
}

bool add_observer(zend_string *cn, zend_string *fn, zval *pre_hook,
                  zval *post_hook) {
    if (op_array_extension == -1) {
        return false;
    }

    if (cn) {
        add_method_observer(OTEL_G(observer_class_lookup), cn, fn, pre_hook,
                            post_hook);
    } else {
        add_function_observer(OTEL_G(observer_function_lookup), fn, pre_hook,
                              post_hook);
    }

    return true;
}

void observer_globals_init(void) {
    if (!OTEL_G(observer_class_lookup)) {
        ALLOC_HASHTABLE(OTEL_G(observer_class_lookup));
        zend_hash_init(OTEL_G(observer_class_lookup), 8, NULL,
                       destroy_observer_class_lookup, 0);
    }
    if (!OTEL_G(observer_function_lookup)) {
        ALLOC_HASHTABLE(OTEL_G(observer_function_lookup));
        zend_hash_init(OTEL_G(observer_function_lookup), 8, NULL,
                       destroy_observer_lookup, 0);
    }
    if (!OTEL_G(observer_aggregates)) {
        ALLOC_HASHTABLE(OTEL_G(observer_aggregates));
        zend_hash_init(OTEL_G(observer_aggregates), 8, NULL,
                       destroy_observer_lookup, 0);
    }
}

void observer_globals_cleanup(void) {
    if (OTEL_G(observer_class_lookup)) {
        zend_hash_destroy(OTEL_G(observer_class_lookup));
        FREE_HASHTABLE(OTEL_G(observer_class_lookup));
        OTEL_G(observer_class_lookup) = NULL;
    }
    if (OTEL_G(observer_function_lookup)) {
        zend_hash_destroy(OTEL_G(observer_function_lookup));
        FREE_HASHTABLE(OTEL_G(observer_function_lookup));
        OTEL_G(observer_function_lookup) = NULL;
    }
    if (OTEL_G(observer_aggregates)) {
        zend_hash_destroy(OTEL_G(observer_aggregates));
        FREE_HASHTABLE(OTEL_G(observer_aggregates));
        OTEL_G(observer_aggregates) = NULL;
    }
}

void opentelemetry_observer_init(INIT_FUNC_ARGS) {
    if (type != MODULE_TEMPORARY) {
        zend_observer_fcall_register(observer_fcall_init);
        op_array_extension =
            zend_get_op_array_extension_handle("opentelemetry");
    }
}

void attribute_init(char *fqn, uint32_t flags) {
    zend_function_entry attr_methods[] = {PHP_FE_END};
    zend_class_entry ce, *class_entry;

    INIT_CLASS_ENTRY(ce, fqn, attr_methods);
    class_entry = zend_register_internal_class_ex(&ce, NULL);
    class_entry->ce_flags |= flags;

    zend_string *attribute_name =
        zend_string_init_interned("Attribute", sizeof("Attribute") - 1, 1);
    zend_attribute *attr =
        zend_add_class_attribute(class_entry, attribute_name, 1);
    zend_string_release(attribute_name);
    zval attr_val;
    ZVAL_LONG(&attr_val, ZEND_ATTRIBUTE_TARGET_ALL);
    ZVAL_COPY_VALUE(&attr->args[0].value, &attr_val);

    zend_internal_attribute_register(class_entry, ZEND_ATTRIBUTE_TARGET_ALL);
}

void opentelemetry_attributes_init() {
    attribute_init(with_span_fqn, ZEND_ACC_FINAL |
                                      ZEND_ATTRIBUTE_TARGET_METHOD |
                                      ZEND_ATTRIBUTE_TARGET_FUNCTION);
    attribute_init(span_attribute_fqn,
                   ZEND_ACC_FINAL | ZEND_ATTRIBUTE_TARGET_PARAMETER);
}
