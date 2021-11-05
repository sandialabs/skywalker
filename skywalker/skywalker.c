#include <skywalker/skywalker.h>

#include <khash.h>
#include <klist.h>
#include <kvec.h>

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

// Some basic data structures.

// A list of C strings.
#define free_string(x) free((char*)x->data)
KLIST_INIT(string_list, const char*, free_string)

// A hash table whose keys are C strings and whose values are also C strings.
KHASH_MAP_INIT_STR(string_map, const char*)

// A hash table whose keys are C strings and whose values are real numbers.
KHASH_MAP_INIT_STR(param_map, sw_real_t)

// A list of strings to be deallocated by skywalker.
static klist_t(string_list)* sw_strings_ = NULL;

// This function cleans up all maintained strings at the end of a process.
static void free_strings() {
  kl_destroy(string_list, sw_strings_);
}

// This function appends a string to the list of maintained strings, setting
// things up when it's called for the first time.
static void append_string(const char *s) {
  if (!sw_strings_) {
    sw_strings_ = kl_init(string_list);
    atexit(free_strings);
  }
  const char ** s_p = kl_pushp(string_list, sw_strings_);
  *s_p = s;
}

// Here we implement a portable version of the non-standard vasprintf
// function (see https://stackoverflow.com/questions/40159892/using-asprintf-on-windows).
static int vscprintf(const char *format, va_list ap) {
  va_list ap_copy;
  va_copy(ap_copy, ap);
  int retval = vsnprintf(NULL, 0, format, ap_copy);
  va_end(ap_copy);
  return retval;
}

static int vasprintf(char **strp, const char *format, va_list ap) {
  int len = vscprintf(format, ap);
  if (len == -1)
    return -1;
  char *str = (char*)malloc((size_t) len + 1);
  if (!str)
    return -1;
  int retval = vsnprintf(str, len + 1, format, ap);
  if (retval == -1) {
    free(str);
    return -1;
  }
  *strp = str;
  return retval;
}

// This function constructs a string in the manner of sprintf, but allocates
// and returns a string of sufficient size. Strings created this way are freed
// when the program exits,
static const char* new_string(const char *fmt, ...) {
  char *s;
  va_list ap;
  va_start(ap, fmt);
  vasprintf(&s, fmt, ap);
  va_end(ap);
  append_string(s);
  return s;
}

struct sw_settings_t {
  khash_t(string_map) *params;
};

// Creates a settings instance.
static sw_settings_t *sw_settings_new() {
  sw_settings_t *settings = malloc(sizeof(sw_settings_t));
  settings->params = kh_init(string_map);
  return settings;
}

// Destroys a settings instance, freeing all allocated resources.
static void sw_settings_free(sw_settings_t *settings) {
  kh_destroy(string_map, settings->params);
}

static sw_output_result_t sw_settings_set(sw_settings_t *settings,
                                          const char *name,
                                          const char *value) {
  sw_output_result_t result = {.error_code = SW_SUCCESS};
  int ret;
  const char* n = strdup(name);
  const char* v = strdup(value);
  khiter_t iter = kh_put(string_map, settings->params, n, &ret);
  kh_value(settings->params, iter) = v;
  append_string(n);
  append_string(v);
  return result;
}

sw_settings_result_t sw_settings_get(sw_settings_t *settings,
                                    const char* name) {
  khiter_t iter = kh_get(string_map, settings->params, name);
  sw_settings_result_t result = {.error_code = SW_SUCCESS};
  if (iter != kh_end(settings->params)) {
    result.value = kh_val(settings->params, iter);
  } else {
    result.error_code = SW_PARAM_NOT_FOUND;
    const char *s = new_string("The setting '%s' was not found.", name);
    result.error_message = s;
  }
  return result;
}

struct sw_input_t {
  khash_t(param_map) *params;
};

static sw_output_result_t sw_input_set(sw_input_t *input,
                                       const char *name,
                                       sw_real_t value) {
  sw_output_result_t result = {.error_code = SW_SUCCESS};
  int ret;
  const char* n = strdup(name);
  khiter_t iter = kh_put(param_map, input->params, n, &ret);
  kh_value(input->params, iter) = value;
  append_string(n);
  return result;
}

sw_input_result_t sw_input_get(sw_input_t *input,
                               const char *name) {
  khiter_t iter = kh_get(param_map, input->params, name);
  sw_input_result_t result = {.error_code = SW_SUCCESS};
  if (iter != kh_end(input->params)) {
    result.value = kh_val(input->params, iter);
  } else {
    result.error_code = SW_PARAM_NOT_FOUND;
    const char *s = new_string("The input parameter '%s' was not found.", name);
    result.error_message = s;
  }
  return result;
}

struct sw_output_t {
  khash_t(param_map) *metrics;
};

sw_output_result_t sw_output_set(sw_output_t *output,
                                 const char *name,
                                 sw_real_t value) {
  sw_output_result_t result = {.error_code = SW_SUCCESS};
  int ret;
  const char* n = strdup(name);
  khiter_t iter = kh_put(param_map, output->metrics, n, &ret);
  kh_value(output->metrics, iter) = value;
  append_string(n);
  return result;
}

// ensemble type
struct sw_ensemble_t {
  size_t size, position;
  sw_input_t *inputs;
  sw_output_t *outputs;
};

// Destroys an ensemble, freeing all allocated resources.
static void sw_ensemble_free(sw_ensemble_t *ensemble) {
  if (ensemble->inputs) {
    for (size_t i = 0; i < ensemble->size; ++i) {
      kh_destroy(param_map, ensemble->inputs[i].params);
      kh_destroy(param_map, ensemble->outputs[i].metrics);
    }
    free(ensemble->inputs);
    free(ensemble->outputs);
  }
  free(ensemble);
}

// A hash table whose keys are C strings and whose values are arrays of numbers.
typedef kvec_t(sw_real_t) sw_real_vec_t;
KHASH_MAP_INIT_STR(yaml_param_map, sw_real_vec_t)

// This type stores data parsed from YAML.
typedef struct sw_yaml_data_t {
  sw_ens_type_t ensemble_type;
  sw_settings_t *settings;
  khash_t(yaml_param_map) *params;
  int error_code;
  const char* error_message;
} sw_yaml_data_t;

// Parses a YAML file, returning the results.
static sw_yaml_data_t parse_yaml(const char* yaml_file,
                                 const char* settings_block) {
  sw_yaml_data_t data = {.error_code = 0};
  return data;
}

// This type contains results from building an ensemble.
typedef struct sw_build_result_t {
  const char *yaml_file;
  size_t num_inputs;
  sw_input_t *inputs;
  int error_code;
  const char *error_message;
} sw_build_result_t;

// Generates an array of inputs for a lattice ensemble.
static sw_build_result_t build_lattice_ensemble(sw_yaml_data_t yaml_data) {
  sw_build_result_t result = {.error_code = SW_SUCCESS};
  // Count up the number of inputs.
  size_t num_params = 0;
  {
    sw_real_vec_t values;
    kh_foreach_value(yaml_data.params, values,
      result.num_inputs *= kv_size(values);
      num_params++;
    );
  }
  if (num_params > 7) {
    result.error_code = SW_TOO_MANY_PARAMS;
    result.error_message =
      new_string("The lattice ensemble in %s has %d traversed parameters "
                 "(must be <= 7).");
    return result;
  }

  // Build a list of inputs corresponding to all the traversed parameters. This
  // involves some ugly index magic based on the number of parameters.
  #define set_param(index, name, value) { \
    int ret; \
    khiter_t iter = kh_put(param_map, result.inputs[index].params, name, &ret);\
    kh_value(result.inputs[index].params, iter) = value; \
  }
  result.inputs = malloc(sizeof(sw_input_t) * result.num_inputs);
  for (size_t l = 0; l < result.num_inputs; ++l) {
    result.inputs[l].params = kh_init(param_map);
    if (num_params == 1) {
      const char *name;
      sw_real_vec_t values;
      kh_foreach(yaml_data.params, name, values,
        if (kv_size(values) > 1) break;
      );
      sw_input_set(&result.inputs[l], name, kv_A(values, l));
    } else if (num_params == 2) {
      const char *name1 = NULL, *name2 = NULL;
      sw_real_vec_t values, values1, values2;
      for (khiter_t iter = kh_begin(yaml_data.params);
           iter != kh_end(yaml_data.params); ++iter) {
        values = kh_value(yaml_data.params, iter);
        if (kv_size(values) > 1) {
          if (name1 == NULL) {
            name1 = kh_key(yaml_data.params, iter);
            values1 = values;
          } else if (name2 == NULL) {
            name2 = kh_key(yaml_data.params, iter);
            values2 = values;
            break;
          }
        }
      }
      size_t n2 = kv_size(values2);
      size_t j1 = l / n2;
      size_t j2 = l - n2 * j1;
      sw_input_set(&result.inputs[l], name1, kv_A(values1, j1));
      sw_input_set(&result.inputs[l], name2, kv_A(values2, j2));
    } else if (num_params == 3) {
      const char *name1 = NULL, *name2 = NULL, *name3 = NULL;
      sw_real_vec_t values, values1, values2, values3;
      for (khiter_t iter = kh_begin(yaml_data.params);
           iter != kh_end(yaml_data.params); ++iter) {
        values = kh_value(yaml_data.params, iter);
        if (kv_size(values) > 1) {
          if (name1 == NULL) {
            name1 = kh_key(yaml_data.params, iter);
            values1 = values;
          } else if (name2 == NULL) {
            name2 = kh_key(yaml_data.params, iter);
            values2 = values;
          } else if (name3 == NULL) {
            name3 = kh_key(yaml_data.params, iter);
            values3 = values;
            break;
          }
        }
      }
      size_t n2 = kv_size(values2);
      size_t n3 = kv_size(values3);
      size_t j1 = l / (n2 * n3);
      size_t j2 = (l - n2 * n3 * j1) / n3;
      size_t j3 = l - n2 * n3 * j1 - n3 * j2;
      sw_input_set(&result.inputs[l], name1, kv_A(values1, j1));
      sw_input_set(&result.inputs[l], name2, kv_A(values2, j2));
      sw_input_set(&result.inputs[l], name3, kv_A(values3, j3));
    } else if (num_params == 4) {
      const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL;
      sw_real_vec_t values, values1, values2, values3, values4;
      for (khiter_t iter = kh_begin(yaml_data.params);
           iter != kh_end(yaml_data.params); ++iter) {
        values = kh_value(yaml_data.params, iter);
        if (kv_size(values) > 1) {
          if (name1 == NULL) {
            name1 = kh_key(yaml_data.params, iter);
            values1 = values;
          } else if (name2 == NULL) {
            name2 = kh_key(yaml_data.params, iter);
            values2 = values;
          } else if (name3 == NULL) {
            name3 = kh_key(yaml_data.params, iter);
            values3 = values;
          } else if (name4 == NULL) {
            name4 = kh_key(yaml_data.params, iter);
            values4 = values;
            break;
          }
        }
      }
      size_t n2 = kv_size(values2);
      size_t n3 = kv_size(values3);
      size_t n4 = kv_size(values4);
      size_t j1 = l / (n2 * n3 * n4);
      size_t j2 = (l - n2 * n3 * n4 * j1) / (n3 * n4);
      size_t j3 = (l - n2 * n3 * n4 * j1 - n3 * n4 * j2) / n4;
      size_t j4 = l - n2 * n3 * n4 * j1 - n3 * n4 * j2 - n4 * j3;
      sw_input_set(&result.inputs[l], name1, kv_A(values1, j1));
      sw_input_set(&result.inputs[l], name2, kv_A(values2, j2));
      sw_input_set(&result.inputs[l], name3, kv_A(values3, j3));
      sw_input_set(&result.inputs[l], name4, kv_A(values4, j4));
    } else if (num_params == 5) {
      const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL,
                 *name5 = NULL;
      sw_real_vec_t values, values1, values2, values3, values4, values5;
      for (khiter_t iter = kh_begin(yaml_data.params);
           iter != kh_end(yaml_data.params); ++iter) {
        values = kh_value(yaml_data.params, iter);
        if (kv_size(values) > 1) {
          if (name1 == NULL) {
            name1 = kh_key(yaml_data.params, iter);
            values1 = values;
          } else if (name2 == NULL) {
            name2 = kh_key(yaml_data.params, iter);
            values2 = values;
          } else if (name3 == NULL) {
            name3 = kh_key(yaml_data.params, iter);
            values3 = values;
          } else if (name4 == NULL) {
            name4 = kh_key(yaml_data.params, iter);
            values4 = values;
          } else if (name5 == NULL) {
            name5 = kh_key(yaml_data.params, iter);
            values5 = values;
            break;
          }
        }
      }
      size_t n2 = kv_size(values2);
      size_t n3 = kv_size(values3);
      size_t n4 = kv_size(values4);
      size_t n5 = kv_size(values5);
      size_t j1 = l / (n2 * n3 * n4 * n5);
      size_t j2 = (l - n2 * n3 * n4 * n5 * j1) / (n3 * n4 * n5);
      size_t j3 = (l - n2 * n3 * n4 * n5 * j1 - n3 * n4 * n5 * j2) / (n4 * n5);
      size_t j4 =
          (l - n2 * n3 * n4 * n5 * j1 - n3 * n4 * n5 * j2 - n4 * n5 * j3) / n5;
      size_t j5 = l - n2 * n3 * n4 * n5 * j1 - n3 * n4 * n5 * j2 -
                  n4 * n5 * j3 - n5 * j4;
      sw_input_set(&result.inputs[l], name1, kv_A(values1, j1));
      sw_input_set(&result.inputs[l], name2, kv_A(values2, j2));
      sw_input_set(&result.inputs[l], name3, kv_A(values3, j3));
      sw_input_set(&result.inputs[l], name4, kv_A(values4, j4));
      sw_input_set(&result.inputs[l], name5, kv_A(values5, j5));
    } else if (num_params == 6) {
      const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL,
                 *name5 = NULL, *name6 = NULL;
      sw_real_vec_t values, values1, values2, values3, values4, values5,
                    values6;
      for (khiter_t iter = kh_begin(yaml_data.params);
           iter != kh_end(yaml_data.params); ++iter) {
        values = kh_value(yaml_data.params, iter);
        if (kv_size(values) > 1) {
          if (name1 == NULL) {
            name1 = kh_key(yaml_data.params, iter);
            values1 = values;
          } else if (name2 == NULL) {
            name2 = kh_key(yaml_data.params, iter);
            values2 = values;
          } else if (name3 == NULL) {
            name3 = kh_key(yaml_data.params, iter);
            values3 = values;
          } else if (name4 == NULL) {
            name4 = kh_key(yaml_data.params, iter);
            values4 = values;
          } else if (name5 == NULL) {
            name5 = kh_key(yaml_data.params, iter);
            values5 = values;
          } else if (name6 == NULL) {
            name6 = kh_key(yaml_data.params, iter);
            values6 = values;
            break;
          }
        }
      }
      size_t n2 = kv_size(values2);
      size_t n3 = kv_size(values3);
      size_t n4 = kv_size(values4);
      size_t n5 = kv_size(values5);
      size_t n6 = kv_size(values6);
      size_t j1 = l / (n2 * n3 * n4 * n5 * n6);
      size_t j2 = (l - n2 * n3 * n4 * n5 * n6 * j1) / (n3 * n4 * n5 * n6);
      size_t j3 = (l - n2 * n3 * n4 * n5 * n6 * j1 - n3 * n4 * n5 * n6 * j2) /
                  (n4 * n5 * n6);
      size_t j4 = (l - n2 * n3 * n4 * n5 * n6 * j1 - n3 * n4 * n5 * n6 * j2 -
                   n4 * n5 * n6 * j3) /
                  (n5 * n6);
      size_t j5 = (l - n2 * n3 * n4 * n5 * n6 * j1 - n3 * n4 * n5 * n6 * j2 -
                   n4 * n5 * n6 * j3 - n5 * n6 * j4) /
                  n6;
      size_t j6 = l - n2 * n3 * n4 * n5 * n6 * j1 - n3 * n4 * n5 * n6 * j2 -
                  n4 * n5 * n6 * j3 - n5 * n6 * j4 - n6 * j5;
      sw_input_set(&result.inputs[l], name1, kv_A(values1, j1));
      sw_input_set(&result.inputs[l], name2, kv_A(values2, j2));
      sw_input_set(&result.inputs[l], name3, kv_A(values3, j3));
      sw_input_set(&result.inputs[l], name4, kv_A(values4, j4));
      sw_input_set(&result.inputs[l], name5, kv_A(values5, j5));
      sw_input_set(&result.inputs[l], name6, kv_A(values6, j6));
    } else {  // if (num_params == 7)
      const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL,
                 *name5 = NULL, *name6 = NULL, *name7 = NULL;
      sw_real_vec_t values, values1, values2, values3, values4, values5,
                    values6, values7;
      for (khiter_t iter = kh_begin(yaml_data.params);
           iter != kh_end(yaml_data.params); ++iter) {
        values = kh_value(yaml_data.params, iter);
        if (kv_size(values) > 1) {
          if (name1 == NULL) {
            name1 = kh_key(yaml_data.params, iter);
            values1 = values;
          } else if (name2 == NULL) {
            name2 = kh_key(yaml_data.params, iter);
            values2 = values;
          } else if (name3 == NULL) {
            name3 = kh_key(yaml_data.params, iter);
            values3 = values;
          } else if (name4 == NULL) {
            name4 = kh_key(yaml_data.params, iter);
            values4 = values;
          } else if (name5 == NULL) {
            name5 = kh_key(yaml_data.params, iter);
            values5 = values;
          } else if (name6 == NULL) {
            name6 = kh_key(yaml_data.params, iter);
            values6 = values;
          } else if (name7 == NULL) {
            name7 = kh_key(yaml_data.params, iter);
            values7 = values;
            break;
          }
        }
      }
      size_t n2 = kv_size(values2);
      size_t n3 = kv_size(values3);
      size_t n4 = kv_size(values4);
      size_t n5 = kv_size(values5);
      size_t n6 = kv_size(values6);
      size_t n7 = kv_size(values7);
      size_t j1 = l / (n2 * n3 * n4 * n5 * n6 * n7);
      size_t j2 =
          (l - n2 * n3 * n4 * n5 * n6 * n7 * j1) / (n3 * n4 * n5 * n6 * n7);
      size_t j3 =
          (l - n2 * n3 * n4 * n5 * n6 * n7 * j1 - n3 * n4 * n5 * n6 * n7 * j2) /
          (n4 * n5 * n6 * n7);
      size_t j4 = (l - n2 * n3 * n4 * n5 * n6 * n7 * j1 -
                   n3 * n4 * n5 * n6 * n7 * j2 - n4 * n5 * n6 * n7 * j3) /
                  (n5 * n6 * n7);
      size_t j5 =
          (l - n2 * n3 * n4 * n5 * n6 * n7 * j1 - n3 * n4 * n5 * n6 * n7 * j2 -
           n4 * n5 * n6 * n7 * j3 - n5 * n6 * n7 * j4) /
          (n6 * n7);
      size_t j6 =
          (l - n2 * n3 * n4 * n5 * n6 * n7 * j1 - n3 * n4 * n5 * n6 * n7 * j2 -
           n4 * n5 * n6 * n7 * j3 - n5 * n6 * n7 * j4 - n6 * n7 * j5) /
          n7;
      size_t j7 = l - n2 * n3 * n4 * n5 * n6 * n7 * j1 -
                  n3 * n4 * n5 * n6 * n7 * j2 - n4 * n5 * n6 * n7 * j3 -
                  n5 * n6 * n7 * j4 - n6 * n7 * j5 - n7 * j6;
      sw_input_set(&result.inputs[l], name1, kv_A(values1, j1));
      sw_input_set(&result.inputs[l], name2, kv_A(values2, j2));
      sw_input_set(&result.inputs[l], name3, kv_A(values3, j3));
      sw_input_set(&result.inputs[l], name4, kv_A(values4, j4));
      sw_input_set(&result.inputs[l], name5, kv_A(values5, j5));
      sw_input_set(&result.inputs[l], name6, kv_A(values6, j6));
      sw_input_set(&result.inputs[l], name7, kv_A(values7, j7));
    }
  }

  return result;
}

static sw_build_result_t build_enumeration_ensemble(sw_yaml_data_t yaml_data) {
  sw_build_result_t result = {.error_code = SW_SUCCESS};
  size_t num_inputs = 0;
  const char *first_name;
  for (khiter_t iter = kh_begin(yaml_data.params);
       iter != kh_end(yaml_data.params); ++iter) {
    const char *name = kh_key(yaml_data.params, iter);
    sw_real_vec_t values = kh_value(yaml_data.params, iter);

    if (num_inputs == 0) {
      num_inputs = kv_size(values);
      first_name = name;
    } else if (num_inputs != kv_size(values)) {
      result.error_code = SW_INVALID_ENUMERATION;
      result.error_message = new_string(
        "Invalid enumeration: Parameter %s has a different number of values (%ld)"
        " than %s (%ld)", name, kv_size(values), first_name, num_inputs);
    }
  }

  if (num_inputs == 0) {
    result.error_code = SW_EMPTY_ENSEMBLE;
    result.error_message = new_string("Ensemble has no members!");
  }

  // Trudge through all the ensemble parameters as defined.
  result.num_inputs = num_inputs;
  result.inputs = malloc(sizeof(sw_input_t) * result.num_inputs);
  for (size_t l = 0; l < num_inputs; ++l) {
    const char *name;
    sw_real_vec_t values;
    kh_foreach(yaml_data.params, name, values,
      sw_input_set(&result.inputs[l], name, kv_A(values, l));
    );
  }

  return result;
}

sw_ensemble_result_t sw_load_ensemble(const char* yaml_file,
                                      const char* settings_block) {
  sw_yaml_data_t data = parse_yaml(yaml_file, settings_block);
  sw_ensemble_result_t result = {.error_code = data.error_code,
                                 .error_message = data.error_message};
  if (data.error_code == SW_SUCCESS) {
    result.settings = data.settings;
    result.type = data.ensemble_type;
    sw_build_result_t build_result;
    if (data.ensemble_type == SW_LATTICE) {
      build_result = build_lattice_ensemble(data);
    } else {
      build_result = build_enumeration_ensemble(data);
    }
    sw_ensemble_t *ensemble = malloc(sizeof(sw_ensemble_t));
    ensemble->size = build_result.num_inputs;
    ensemble->position = 0;
    ensemble->inputs = build_result.inputs;
    ensemble->outputs = malloc(ensemble->size*sizeof(sw_output_t));
    for (size_t i = 0; i < ensemble->size; ++i) {
      ensemble->outputs[i].metrics = kh_init(param_map);
    }
  } else {
    result.error_code = data.error_code;
    result.error_message = data.error_message;
    sw_settings_free(data.settings);
  }

  // Clean up YAML data.
  sw_real_vec_t values;
  kh_foreach_value(data.params, values,
    kv_destroy(values);
  );
  kh_destroy(yaml_param_map, data.params);

  return result;
}

bool sw_ensemble_next(sw_ensemble_t *ensemble,
                      sw_input_t **input,
                      sw_output_t **output) {
  if (ensemble->position >= (int)ensemble->size) {
    return false;
  }

  *input = &ensemble->inputs[ensemble->position];
  *output = &ensemble->outputs[ensemble->position];
  ++ensemble->position;
  return true;
}

void sw_ensemble_write(sw_ensemble_t *ensemble, const char *module_filename) {
  FILE* file = fopen(module_filename, "w");
  fprintf(file, "# This file was automatically generated by skywalker.\n\n");
  fprintf(file, "from math import nan as nan\n\n");
  fprintf(
      file,
      "# Object is just a dynamic container that stores input/output data.\n");
  fprintf(file, "class Object(object):\n");
  fprintf(file, "    pass\n\n");

  // Write input data.
  fprintf(file, "# Input is stored here.\n");
  fprintf(file, "input = Object()\n");
  if (ensemble->size > 0) {
    khash_t(param_map) *params_0 = ensemble->inputs[0].params;
    for (khiter_t iter = kh_begin(params_0); iter != kh_end(params_0); ++iter) {
      const char *name = kh_key(params_0, iter);
      fprintf(file, "input.%s = [", name);
      for (size_t i = 0; i < ensemble->size; ++i) {
        khash_t(param_map) *params_i = ensemble->inputs[i].params;
        sw_real_t value = kh_get(param_map, params_i, name);
        fprintf(file, "%g, ", value);
      }
    }
  }
  fprintf(file, "]\n");

  // Write output data.
  fprintf(file, "\n# Output data is stored here.\n");
  fprintf(file, "output = Object()\n");
  if (ensemble->size > 0) {
    khash_t(param_map) *params_0 = ensemble->outputs[0].metrics;
    for (khiter_t iter = kh_begin(params_0); iter != kh_end(params_0); ++iter) {
      const char *name = kh_key(params_0, iter);
      fprintf(file, "output.%s = [", name);
      for (size_t i = 0; i < ensemble->size; ++i) {
        khash_t(param_map) *params_i = ensemble->outputs[i].metrics;
        sw_real_t value = kh_get(param_map, params_i, name);
        if (isnan(value)) {
          fprintf(file, "nan, ");
        } else {
          fprintf(file, "%g, ", value);
        }
      }
      fprintf(file, "]\n");
    }
  }

  fclose(file);

  // Destroy the ensemble.
  sw_ensemble_free(ensemble);
}

//----------------------------
// Skywalker Fortran bindings
//----------------------------

#ifdef SKYWALKER_F90
#endif

#ifdef __cplusplus
} // extern "C"
#endif
