#include <skywalker.h>

#include <khash.h>
#include <klist.h>
#include <kvec.h>
#include <yaml.h>

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef __cplusplus
extern "C" {
#endif

void sw_print_banner() {
  fprintf(stderr, "Skywalker v%d.%d.%d\n", SKYWALKER_MAJOR_VERSION,
          SKYWALKER_MINOR_VERSION, SKYWALKER_PATCH_VERSION);
}

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
static int sw_vscprintf(const char *format, va_list ap) {
  va_list ap_copy;
  va_copy(ap_copy, ap);
  int retval = vsnprintf(NULL, 0, format, ap_copy);
  va_end(ap_copy);
  return retval;
}

static int sw_vasprintf(char **strp, const char *format, va_list ap) {
  int len = sw_vscprintf(format, ap);
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
  sw_vasprintf(&s, fmt, ap);
  va_end(ap);
  append_string(s);
  return s;
}

// This function duplicates the given string and appends it to the list of
// strings to be freed when the program exits.
static const char* dup_string(const char *s) {
  size_t len = strlen(s);
  char *dup = malloc(sizeof(char)*(len+1));
  strcpy(dup, s);
  append_string(dup);
  return (const char*)dup;
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
  free(settings);
}

static sw_output_result_t sw_settings_set(sw_settings_t *settings,
                                          const char *name,
                                          const char *value) {
  sw_output_result_t result = {.error_code = SW_SUCCESS};
  const char* n = dup_string(name);
  const char* v = dup_string(value);
  int ret;
  khiter_t iter = kh_put(string_map, settings->params, n, &ret);
  assert(ret == 1);
  kh_value(settings->params, iter) = v;
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
  const char* n = dup_string(name);
  int ret;
  khiter_t iter = kh_put(param_map, input->params, n, &ret);
  assert(ret == 1);
  kh_value(input->params, iter) = value;
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
  const char* n = dup_string(name);
  int ret;
  khiter_t iter = kh_put(param_map, output->metrics, n, &ret);
  assert(ret == 1);
  kh_value(output->metrics, iter) = value;
  return result;
}

// ensemble type
struct sw_ensemble_t {
  size_t size, position;
  sw_input_t *inputs;
  sw_output_t *outputs;
  sw_settings_t *settings; // for freeing
};

//------------------------------------------------------------------------
//                              YAML parsing
//------------------------------------------------------------------------

// A YAML-specific string pool.
static klist_t(string_list)* yaml_strings_ = NULL;

// This function cleans up all maintained strings at the end of a process.
static void free_yaml_strings() {
  kl_destroy(string_list, yaml_strings_);
}

// This function creates a copy of a string encountered in the YAML parser,
// placing it in the YAML string pool.
static const char* dup_yaml_string(const char *s) {
  // Copy the string.
  size_t len = strlen(s);
  char *dup = malloc(sizeof(char)*(len+1));
  strcpy(dup, s);

  // Stick it in the YAML string pool.
  if (!yaml_strings_) {
    yaml_strings_ = kl_init(string_list);
  }
  const char ** s_p = kl_pushp(string_list, yaml_strings_);
  *s_p = dup;

  return (const char*)dup;
}

// A hash table whose keys are C strings and whose values are arrays of numbers.
typedef kvec_t(sw_real_t) real_vec_t;
KHASH_MAP_INIT_STR(yaml_param_map, real_vec_t)

// This type stores data parsed from YAML.
typedef struct yaml_data_t {
  sw_ens_type_t ensemble_type;
  sw_settings_t *settings;
  khash_t(yaml_param_map) *input;
  int error_code;
  const char *error_message;
} yaml_data_t;

// This type keeps track of the state of the YAML parser.
typedef struct parser_state_t {
  bool parsing_type;
  const char *settings_block;
  bool parsing_settings;
  const char *current_setting;
  bool parsing_input;
  bool parsing_input_sequence;
  const char *current_input;
} parser_state_t;

// Handles a YAML event, populating our data instance.
static void handle_yaml_event(yaml_event_t *event,
                              parser_state_t* state,
                              yaml_data_t *data)
{
  if (event->type == YAML_SCALAR_EVENT) {
    const char *value = (const char*)(event->data.scalar.value);
    // type block
    if (!state->parsing_type && (strcmp(value, "type") == 0)) {
      state->parsing_type = true;
    } else if (state->parsing_type) {
      if (strcmp(value, "lattice") == 0) {
        data->ensemble_type = SW_LATTICE;
      } else if (strcmp(value, "enumeration") == 0) {
        data->ensemble_type = SW_ENUMERATION;
      } else {
        data->error_code = SW_INVALID_ENSEMBLE_TYPE;
        data->error_message = new_string("Invalid ensemble type: %s", value);
      }
      state->parsing_type = false;

    // settings block
    } else if (!state->parsing_settings &&
               strcmp(value, state->settings_block) == 0) {
      assert(!state->current_setting);
      state->parsing_settings = true;
    } else if (state->parsing_settings) {
      if (!state->current_setting) {
        state->current_setting = dup_yaml_string(value);
      } else {
        sw_settings_set(data->settings, state->current_setting, value);
        state->current_setting = NULL;
      }

    // input block
    } else if (!state->parsing_input && strcmp(value, "input") == 0) {
      state->parsing_input = true;
    } else if (state->parsing_input) {
      if (!state->current_input) {
        state->current_input = dup_yaml_string(value);
      } else {
        // Try to interpret the value as a real number.
        char *endp;
        sw_real_t real_value = strtod(value, &endp);
        if (endp == value) { // invalid value!
          data->error_code = SW_INVALID_PARAM_VALUE;
          data->error_message = new_string(
            "Invalid input value for parameter %s: %s", state->current_input,
            value);
        } else {
          // Append the (valid) value to the list of inputs with this name.
          khiter_t iter = kh_get(yaml_param_map, data->input,
                                 state->current_input);
          if (iter == kh_end(data->input)) {
            int ret;
            iter = kh_put(yaml_param_map, data->input, state->current_input, &ret);
            assert(ret == 1);
            kv_init(kh_value(data->input, iter));
          }
          kv_push(sw_real_t, kh_value(data->input, iter), real_value);
        }
        if (!state->parsing_input_sequence) {
          state->current_input = NULL;
        }
      }
    }
  } else if (event->type == YAML_MAPPING_END_EVENT) {
    state->parsing_type = false;
    state->parsing_input = false;
    state->parsing_settings = false;
  } else if (event->type == YAML_SEQUENCE_START_EVENT) {
    if (state->parsing_input) state->parsing_input_sequence = true;
  } else if (event->type == YAML_SEQUENCE_END_EVENT) {
    state->parsing_input_sequence = false;
    if (state->current_input) {
      state->current_input = NULL;
    }
  }
}

// Postprocess the input parameters in the YAML data.
void postprocess_input_data(yaml_data_t *yaml_data) {
  // Expand any relevant 3-parameter lists.
  for (khiter_t iter = kh_begin(yaml_data->input);
      iter != kh_end(yaml_data->input); ++iter) {

    if (!kh_exist(yaml_data->input, iter)) continue;

    real_vec_t values = kh_value(yaml_data->input, iter);

    if (kv_size(values) == 3) {
      sw_real_t val0 = kv_A(values, 0),
                val1 = kv_A(values, 1),
                val2 = kv_A(values, 2);
      if ((val0 < val1) && (val2 < val1)) { // expand!
        real_vec_t expanded_values;
        kv_init(expanded_values);
        size_t size = (size_t)(ceil((val1 - val0) / val2) + 1);
        for (size_t i = 0; i < size; ++i) {
          kv_push(sw_real_t, expanded_values, val0 + i * val2);
        }
        kh_value(yaml_data->input, iter) = expanded_values;
        kv_destroy(values);
      }
    }
  }

  // Exponentiate any log10 values, renaming "log10(x)" to "x".
  khash_t(yaml_param_map) *renamed_input = kh_init(yaml_param_map);
  for (khiter_t iter = kh_begin(yaml_data->input);
      iter != kh_end(yaml_data->input); ++iter) {

    if (!kh_exist(yaml_data->input, iter)) continue;

    const char *param_name = kh_key(yaml_data->input, iter);
    size_t pname_len = strlen(param_name);
    real_vec_t values = kh_value(yaml_data->input, iter);

    char new_param_name[pname_len+1];
    if (strstr(param_name, "log10(") == param_name) {
      if (param_name[pname_len-1] != ')') { // Did we close our parens?
        yaml_data->error_code = SW_INVALID_PARAM_NAME;
        yaml_data->error_message = new_string("Unclosed parens in parameter %s.",
          param_name);
        kh_destroy(yaml_param_map, yaml_data->input);
        return;
      }
      if (!renamed_input)
        renamed_input = kh_init(yaml_param_map);

      memcpy(new_param_name, &param_name[6], pname_len-7);
      new_param_name[pname_len-7] = '\0';

      for (size_t i = 0; i < kv_size(values); ++i) {
        kv_A(values, i) = pow(10.0, kv_A(values, i));
      }
    } else {
      strcpy(new_param_name, param_name);
    }

    int ret;
    khiter_t r_iter = kh_put(yaml_param_map, renamed_input,
                             dup_yaml_string(new_param_name), &ret);
    assert(ret == 1);
    kh_value(renamed_input, r_iter) = values;
  }

  kh_destroy(yaml_param_map, yaml_data->input);
  yaml_data->input = renamed_input;
}

// Parses a YAML file, returning the results.
static yaml_data_t parse_yaml(FILE* file, const char* settings_block) {
  yaml_data_t data = {.error_code = 0};

  data.input = kh_init(yaml_param_map);
  data.settings = sw_settings_new();

  yaml_parser_t parser;
  yaml_parser_initialize(&parser);
  yaml_parser_set_input_file(&parser, file);

  parser_state_t state = {.settings_block = settings_block};
  yaml_event_type_t event_type;
  do {
    yaml_event_t event;

    // Parse the next YAML "event" and handle any errors encountered.
    if (!yaml_parser_parse(&parser, &event)) {
      kh_destroy(yaml_param_map, data.input);
      sw_settings_free(data.settings);
      data.error_code = SW_INVALID_YAML;
      data.error_message = dup_string(parser.problem);
      yaml_parser_delete(&parser);
      fclose(file);
      return data;
    }

    // Process the event, using it to populate our YAML data, and handle
    // any errors resulting from properly-formed YAML that doesn't conform
    // to Skywalker's spec.
    handle_yaml_event(&event, &state, &data);
    if (data.error_code != SW_SUCCESS) {
      kh_destroy(yaml_param_map, data.input);
      sw_settings_free(data.settings);
      yaml_parser_delete(&parser);
      fclose(file);
      return data;
    }
    event_type = event.type;
    yaml_event_delete(&event);
  } while (event_type != YAML_STREAM_END_EVENT);
  yaml_parser_delete(&parser);

  // Did we find a settings block?
  if (data.settings == NULL) {
    kh_destroy(yaml_param_map, data.input);
    data.error_code = SW_SETTINGS_NOT_FOUND;
    data.error_message = new_string("The settings block '%s' was not found.",
                                    settings_block);
  }

  // Postprocess the input parameters, expanding 3-element lists if needed, and
  // applying log10 operations.
  if (data.error_code == SW_SUCCESS)
    postprocess_input_data(&data);

  return data;
}

//------------------------------------------------------------------------
//                          Ensemble construction
//------------------------------------------------------------------------

static void assign_single_valued_params(yaml_data_t yaml_data,
                                        sw_input_t *input) {
  const char *name;
  real_vec_t values;
  kh_foreach(yaml_data.input, name, values,
    if (kv_size(values) == 1) {
      sw_input_set(input, name, kv_A(values, 0));
    }
  );
}

static void assign_1_lattice_param(yaml_data_t yaml_data, size_t l,
                                   sw_input_t *input) {
  const char *name;
  real_vec_t values;
  kh_foreach(yaml_data.input, name, values,
    if (kv_size(values) > 1) break;
  );
  sw_input_set(input, name, kv_A(values, l));
}

static void assign_2_lattice_params(yaml_data_t yaml_data, size_t l,
                                    sw_input_t *input) {
  const char *name1 = NULL, *name2 = NULL;
  real_vec_t values, values1, values2;
  for (khiter_t iter = kh_begin(yaml_data.input);
      iter != kh_end(yaml_data.input); ++iter) {

    if (!kh_exist(yaml_data.input, iter)) continue;

    values = kh_value(yaml_data.input, iter);
    if (kv_size(values) > 1) {
      if (name1 == NULL) {
        name1 = kh_key(yaml_data.input, iter);
        values1 = values;
      } else if (name2 == NULL) {
        name2 = kh_key(yaml_data.input, iter);
        values2 = values;
        break;
      }
    }
  }
  size_t n2 = kv_size(values2);
  size_t j1 = l / n2;
  size_t j2 = l - n2 * j1;
  sw_input_set(input, name1, kv_A(values1, j1));
  sw_input_set(input, name2, kv_A(values2, j2));
}

static void assign_3_lattice_params(yaml_data_t yaml_data, size_t l,
                                    sw_input_t *input) {
  const char *name1 = NULL, *name2 = NULL, *name3 = NULL;
  real_vec_t values, values1, values2, values3;
  for (khiter_t iter = kh_begin(yaml_data.input);
      iter != kh_end(yaml_data.input); ++iter) {

    if (!kh_exist(yaml_data.input, iter)) continue;

    values = kh_value(yaml_data.input, iter);
    if (kv_size(values) > 1) {
      if (name1 == NULL) {
        name1 = kh_key(yaml_data.input, iter);
        values1 = values;
      } else if (name2 == NULL) {
        name2 = kh_key(yaml_data.input, iter);
        values2 = values;
      } else if (name3 == NULL) {
        name3 = kh_key(yaml_data.input, iter);
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
  sw_input_set(input, name1, kv_A(values1, j1));
  sw_input_set(input, name2, kv_A(values2, j2));
  sw_input_set(input, name3, kv_A(values3, j3));
}

static void assign_4_lattice_params(yaml_data_t yaml_data, size_t l,
                                    sw_input_t *input) {
  const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL;
  real_vec_t values, values1, values2, values3, values4;
  for (khiter_t iter = kh_begin(yaml_data.input);
      iter != kh_end(yaml_data.input); ++iter) {

    if (!kh_exist(yaml_data.input, iter)) continue;

    values = kh_value(yaml_data.input, iter);
    if (kv_size(values) > 1) {
      if (name1 == NULL) {
        name1 = kh_key(yaml_data.input, iter);
        values1 = values;
      } else if (name2 == NULL) {
        name2 = kh_key(yaml_data.input, iter);
        values2 = values;
      } else if (name3 == NULL) {
        name3 = kh_key(yaml_data.input, iter);
        values3 = values;
      } else if (name4 == NULL) {
        name4 = kh_key(yaml_data.input, iter);
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
  sw_input_set(input, name1, kv_A(values1, j1));
  sw_input_set(input, name2, kv_A(values2, j2));
  sw_input_set(input, name3, kv_A(values3, j3));
  sw_input_set(input, name4, kv_A(values4, j4));
}

static void assign_5_lattice_params(yaml_data_t yaml_data, size_t l,
                                    sw_input_t *input) {
  const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL,
  *name5 = NULL;
  real_vec_t values, values1, values2, values3, values4, values5;
  for (khiter_t iter = kh_begin(yaml_data.input);
      iter != kh_end(yaml_data.input); ++iter) {

    if (!kh_exist(yaml_data.input, iter)) continue;

    values = kh_value(yaml_data.input, iter);
    if (kv_size(values) > 1) {
      if (name1 == NULL) {
        name1 = kh_key(yaml_data.input, iter);
        values1 = values;
      } else if (name2 == NULL) {
        name2 = kh_key(yaml_data.input, iter);
        values2 = values;
      } else if (name3 == NULL) {
        name3 = kh_key(yaml_data.input, iter);
        values3 = values;
      } else if (name4 == NULL) {
        name4 = kh_key(yaml_data.input, iter);
        values4 = values;
      } else if (name5 == NULL) {
        name5 = kh_key(yaml_data.input, iter);
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
  sw_input_set(input, name1, kv_A(values1, j1));
  sw_input_set(input, name2, kv_A(values2, j2));
  sw_input_set(input, name3, kv_A(values3, j3));
  sw_input_set(input, name4, kv_A(values4, j4));
  sw_input_set(input, name5, kv_A(values5, j5));
}

static void assign_6_lattice_params(yaml_data_t yaml_data, size_t l,
                                    sw_input_t *input) {
  const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL,
  *name5 = NULL, *name6 = NULL;
  real_vec_t values, values1, values2, values3, values4, values5,
                values6;
  for (khiter_t iter = kh_begin(yaml_data.input);
      iter != kh_end(yaml_data.input); ++iter) {

    if (!kh_exist(yaml_data.input, iter)) continue;

    values = kh_value(yaml_data.input, iter);
    if (kv_size(values) > 1) {
      if (name1 == NULL) {
        name1 = kh_key(yaml_data.input, iter);
        values1 = values;
      } else if (name2 == NULL) {
        name2 = kh_key(yaml_data.input, iter);
        values2 = values;
      } else if (name3 == NULL) {
        name3 = kh_key(yaml_data.input, iter);
        values3 = values;
      } else if (name4 == NULL) {
        name4 = kh_key(yaml_data.input, iter);
        values4 = values;
      } else if (name5 == NULL) {
        name5 = kh_key(yaml_data.input, iter);
        values5 = values;
      } else if (name6 == NULL) {
        name6 = kh_key(yaml_data.input, iter);
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
  sw_input_set(input, name1, kv_A(values1, j1));
  sw_input_set(input, name2, kv_A(values2, j2));
  sw_input_set(input, name3, kv_A(values3, j3));
  sw_input_set(input, name4, kv_A(values4, j4));
  sw_input_set(input, name5, kv_A(values5, j5));
  sw_input_set(input, name6, kv_A(values6, j6));
}

static void assign_7_lattice_params(yaml_data_t yaml_data, size_t l,
                                    sw_input_t *input) {
  const char *name1 = NULL, *name2 = NULL, *name3 = NULL, *name4 = NULL,
  *name5 = NULL, *name6 = NULL, *name7 = NULL;
  real_vec_t values, values1, values2, values3, values4, values5,
                values6, values7;
  for (khiter_t iter = kh_begin(yaml_data.input);
      iter != kh_end(yaml_data.input); ++iter) {

    if (!kh_exist(yaml_data.input, iter)) continue;

    values = kh_value(yaml_data.input, iter);
    if (kv_size(values) > 1) {
      if (name1 == NULL) {
        name1 = kh_key(yaml_data.input, iter);
        values1 = values;
      } else if (name2 == NULL) {
        name2 = kh_key(yaml_data.input, iter);
        values2 = values;
      } else if (name3 == NULL) {
        name3 = kh_key(yaml_data.input, iter);
        values3 = values;
      } else if (name4 == NULL) {
        name4 = kh_key(yaml_data.input, iter);
        values4 = values;
      } else if (name5 == NULL) {
        name5 = kh_key(yaml_data.input, iter);
        values5 = values;
      } else if (name6 == NULL) {
        name6 = kh_key(yaml_data.input, iter);
        values6 = values;
      } else if (name7 == NULL) {
        name7 = kh_key(yaml_data.input, iter);
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
  sw_input_set(input, name1, kv_A(values1, j1));
  sw_input_set(input, name2, kv_A(values2, j2));
  sw_input_set(input, name3, kv_A(values3, j3));
  sw_input_set(input, name4, kv_A(values4, j4));
  sw_input_set(input, name5, kv_A(values5, j5));
  sw_input_set(input, name6, kv_A(values6, j6));
  sw_input_set(input, name7, kv_A(values7, j7));
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
static sw_build_result_t build_lattice_ensemble(yaml_data_t yaml_data) {
  sw_build_result_t result = {.num_inputs = 1, .error_code = SW_SUCCESS};
  // Count up the number of inputs and parameters.
  size_t num_params = 0;
  {
    real_vec_t values;
    kh_foreach_value(yaml_data.input, values,
      result.num_inputs *= kv_size(values);
      if (kv_size(values) > 1) // exclude single-valued parameters
        num_params++;
    );
  }
  if (num_params == 0) {
    result.error_code = SW_EMPTY_ENSEMBLE;
    result.error_message = new_string("Ensemble has no members!");
    return result;
  } else if (num_params > 7) {
    result.error_code = SW_TOO_MANY_PARAMS;
    result.error_message =
      new_string("The given lattice ensemble has %d traversed parameters "
                 "(must be <= 7).", num_params);
    return result;
  }

  // Here's a dispatch mechanism that maps the number of parameters to a
  // a function that sifts them into an input.
  static void (*assign_lattice_params[])(yaml_data_t, size_t, sw_input_t*) = {
    NULL, assign_1_lattice_param, assign_2_lattice_params,
    assign_3_lattice_params, assign_4_lattice_params, assign_5_lattice_params,
    assign_6_lattice_params, assign_7_lattice_params
  };

  // Build a list of inputs corresponding to all the traversed parameters.
  result.inputs = malloc(sizeof(sw_input_t) * result.num_inputs);
  for (size_t l = 0; l < result.num_inputs; ++l) {
    result.inputs[l].params = kh_init(param_map);
    assign_single_valued_params(yaml_data, &result.inputs[l]);
    assign_lattice_params[num_params](yaml_data, l, &result.inputs[l]);
  }

  return result;
}

static sw_build_result_t build_enumeration_ensemble(yaml_data_t yaml_data) {
  sw_build_result_t result = {.error_code = SW_SUCCESS};
  size_t num_inputs = 0;
  const char *first_name;
  for (khiter_t iter = kh_begin(yaml_data.input);
       iter != kh_end(yaml_data.input); ++iter) {

    if (!kh_exist(yaml_data.input, iter)) continue;

    const char *name = kh_key(yaml_data.input, iter);
    real_vec_t values = kh_value(yaml_data.input, iter);

    if ((num_inputs == 0) || (num_inputs == 1)) {
      num_inputs = kv_size(values);
      first_name = name;
    } else if ((num_inputs != kv_size(values)) && (kv_size(values) > 1)) {
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
    real_vec_t values;
    result.inputs[l].params = kh_init(param_map);
    kh_foreach(yaml_data.input, name, values,
      if (kv_size(values) == 1)
        sw_input_set(&result.inputs[l], name, kv_A(values, 0));
      else
        sw_input_set(&result.inputs[l], name, kv_A(values, l));
    );
  }

  return result;
}

//------------------------------------------------------------------------
//                      Ensemble loading and writing
//------------------------------------------------------------------------

sw_ensemble_result_t sw_load_ensemble(const char* yaml_file,
                                      const char* settings_block) {
  sw_ensemble_result_t result = {.error_code = 0};

  // Validate inputs.
  if ((strcmp(settings_block, "type") == 0) ||
      (strcmp(settings_block, "input") == 0)) {
    result.error_code = SW_INVALID_SETTINGS_BLOCK;
    result.error_message = new_string("Invalid settings block name: '%s'"
                                      " (cannot be 'type' or 'input')",
                                      settings_block);
    return result;
  }

  FILE *file = fopen(yaml_file, "r");
  if (file == NULL) {
    result.error_code = SW_YAML_FILE_NOT_FOUND;
    result.error_message = new_string("The file '%s' could not be opened.",
                                      yaml_file);
    return result;
  }

  // Parse the YAML file, populating a data container.
  yaml_data_t data = parse_yaml(file, settings_block);
  fclose(file);

  if (data.error_code == SW_SUCCESS) {
    result.type = data.ensemble_type;
    sw_build_result_t build_result;
    if (data.ensemble_type == SW_LATTICE) {
      build_result = build_lattice_ensemble(data);
    } else {
      build_result = build_enumeration_ensemble(data);
    }
    if (build_result.error_code != SW_SUCCESS) {
      result.error_code = build_result.error_code;
      result.error_message = build_result.error_message;
    } else {
      sw_ensemble_t *ensemble = malloc(sizeof(sw_ensemble_t));
      ensemble->size = build_result.num_inputs;
      ensemble->position = 0;
      ensemble->inputs = build_result.inputs;
      ensemble->outputs = malloc(ensemble->size*sizeof(sw_output_t));
      for (size_t i = 0; i < ensemble->size; ++i) {
        ensemble->outputs[i].metrics = kh_init(param_map);
      }
      result.settings = data.settings;
      ensemble->settings = result.settings;
      result.ensemble = ensemble;
    }
  } else {
    result.error_code = data.error_code;
    result.error_message = data.error_message;
  }

  // Clean up YAML data.
  real_vec_t values;
  kh_foreach_value(data.input, values,
    kv_destroy(values);
  );
  kh_destroy(yaml_param_map, data.input);
  free_yaml_strings(); // delete YAML string pool

  return result;
}

size_t sw_ensemble_size(sw_ensemble_t* ensemble) {
  return ensemble->size;
}

bool sw_ensemble_next(sw_ensemble_t *ensemble,
                      sw_input_t **input,
                      sw_output_t **output) {
  if (ensemble->position >= (int)ensemble->size) {
    ensemble->position = 0; // reset for next traversal
    *input = NULL;
    *output = NULL;
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

      if (!kh_exist(params_0, iter)) continue;

      const char *name = kh_key(params_0, iter);
      fprintf(file, "input.%s = [", name);
      for (size_t i = 0; i < ensemble->size; ++i) {
        khash_t(param_map) *params_i = ensemble->inputs[i].params;
        khiter_t iter = kh_get(param_map, params_i, name);
        sw_real_t value = kh_val(params_i, iter);
        fprintf(file, "%g, ", value);
      }
      fprintf(file, "]\n");
    }
  }

  // Write output data.
  fprintf(file, "\n# Output data is stored here.\n");
  fprintf(file, "output = Object()\n");
  if (ensemble->size > 0) {
    khash_t(param_map) *params_0 = ensemble->outputs[0].metrics;
    for (khiter_t iter = kh_begin(params_0); iter != kh_end(params_0); ++iter) {

      if (!kh_exist(params_0, iter)) continue;

      const char *name = kh_key(params_0, iter);
      fprintf(file, "output.%s = [", name);
      for (size_t i = 0; i < ensemble->size; ++i) {
        khash_t(param_map) *params_i = ensemble->outputs[i].metrics;
        khiter_t iter = kh_get(param_map, params_i, name);
        sw_real_t value = kh_val(params_i, iter);
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
}

void sw_ensemble_free(sw_ensemble_t *ensemble) {
  if (ensemble->settings)
    sw_settings_free(ensemble->settings);
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

//----------------------------
// Skywalker Fortran bindings
//----------------------------

#ifdef SKYWALKER_F90

void sw_load_ensemble_f90(const char *yaml_file, const char *settings_block,
                          sw_settings_t **settings, sw_ensemble_t **ensemble,
                          int *type, int *error_code,
                          const char **error_message) {
  sw_ensemble_result_t result = sw_load_ensemble(yaml_file, settings_block);
  if (result.error_code == SW_SUCCESS) {
    *settings = result.settings;
    *ensemble = result.ensemble;
    *type = result.type;
  }
  *error_code = result.error_code;
  *error_message = result.error_message;
}

void sw_settings_get_f90(sw_settings_t *settings, const char *name,
                         const char **value, int *error_code,
                         const char **error_message) {
  sw_settings_result_t result = sw_settings_get(settings, name);
  if (result.error_code == SW_SUCCESS) {
    *value = result.value;
  }
  *error_code = result.error_code;
  *error_message = result.error_message;
}

void sw_input_get_f90(sw_input_t *input, const char *name, sw_real_t *value,
                      int *error_code, const char **error_message) {
  sw_input_result_t result = sw_input_get(input, name);
  if (result.error_code == SW_SUCCESS) {
    *value = result.value;
  }
  *error_code = result.error_code;
  *error_message = result.error_message;
}

// Returns a newly-allocated C string for the given Fortran string pointer with
// the given length. Strings of this sort are freed at program exit.
const char* sw_new_c_string(char* f_str_ptr, int f_str_len) {
  char* s = malloc(sizeof(char) * (f_str_len+1));
  memcpy(s, f_str_ptr, sizeof(char) * f_str_len);
  s[f_str_len] = '\0';
  append_string((const char*)s);
  return (const char*)s;
}

#endif // SKYWALKER_F90

#ifdef __cplusplus
} // extern "C"
#endif