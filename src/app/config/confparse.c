/* Copyright (c) 2001 Matej Pfajfar.
 * Copyright (c) 2001-2004, Roger Dingledine.
 * Copyright (c) 2004-2006, Roger Dingledine, Nick Mathewson.
 * Copyright (c) 2007-2019, The Tor Project, Inc. */
/* See LICENSE for licensing information */

/**
 * \file confparse.c
 *
 * \brief Back-end for parsing and generating key-value files, used to
 *   implement the torrc file format and the state file.
 *
 * This module is used by config.c to parse and encode torrc
 * configuration files, and by statefile.c to parse and encode the
 * $DATADIR/state file.
 *
 * To use this module, its callers provide an instance of
 * config_format_t to describe the mappings from a set of configuration
 * options to a number of fields in a C structure.  With this mapping,
 * the functions here can convert back and forth between the C structure
 * specified, and a linked list of key-value pairs.
 */

#define CONFPARSE_PRIVATE
#include "core/or/or.h"
#include "app/config/confparse.h"
#include "feature/nodelist/routerset.h"

#include "lib/container/bitarray.h"
#include "lib/encoding/confline.h"

static uint64_t config_parse_memunit(const char *s, int *ok);
static int config_parse_msec_interval(const char *s, int *ok);
static int config_parse_interval(const char *s, int *ok);
static void config_reset(const config_format_t *fmt, void *options,
                         const config_var_t *var, int use_defaults);

/** Allocate an empty configuration object of a given format type. */
void *
config_new(const config_format_t *fmt)
{
  void *opts = tor_malloc_zero(fmt->size);
  *(uint32_t*)STRUCT_VAR_P(opts, fmt->magic_offset) = fmt->magic;
  CONFIG_CHECK(fmt, opts);
  return opts;
}

/*
 * Functions to parse config options
 */

/** If <b>option</b> is an official abbreviation for a longer option,
 * return the longer option.  Otherwise return <b>option</b>.
 * If <b>command_line</b> is set, apply all abbreviations.  Otherwise, only
 * apply abbreviations that work for the config file and the command line.
 * If <b>warn_obsolete</b> is set, warn about deprecated names. */
const char *
config_expand_abbrev(const config_format_t *fmt, const char *option,
                     int command_line, int warn_obsolete)
{
  int i;
  if (! fmt->abbrevs)
    return option;
  for (i=0; fmt->abbrevs[i].abbreviated; ++i) {
    /* Abbreviations are case insensitive. */
    if (!strcasecmp(option,fmt->abbrevs[i].abbreviated) &&
        (command_line || !fmt->abbrevs[i].commandline_only)) {
      if (warn_obsolete && fmt->abbrevs[i].warn) {
        log_warn(LD_CONFIG,
                 "The configuration option '%s' is deprecated; "
                 "use '%s' instead.",
                 fmt->abbrevs[i].abbreviated,
                 fmt->abbrevs[i].full);
      }
      /* Keep going through the list in case we want to rewrite it more.
       * (We could imagine recursing here, but I don't want to get the
       * user into an infinite loop if we craft our list wrong.) */
      option = fmt->abbrevs[i].full;
    }
  }
  return option;
}

/** If <b>key</b> is a deprecated configuration option, return the message
 * explaining why it is deprecated (which may be an empty string). Return NULL
 * if it is not deprecated. The <b>key</b> field must be fully expanded. */
const char *
config_find_deprecation(const config_format_t *fmt, const char *key)
{
  if (BUG(fmt == NULL) || BUG(key == NULL))
    return NULL; // LCOV_EXCL_LINE
  if (fmt->deprecations == NULL)
    return NULL;

  const config_deprecation_t *d;
  for (d = fmt->deprecations; d->name; ++d) {
    if (!strcasecmp(d->name, key)) {
      return d->why_deprecated ? d->why_deprecated : "";
    }
  }
  return NULL;
}

/** As config_find_option, but return a non-const pointer. */
config_var_t *
config_find_option_mutable(config_format_t *fmt, const char *key)
{
  int i;
  size_t keylen = strlen(key);
  if (!keylen)
    return NULL; /* if they say "--" on the command line, it's not an option */
  /* First, check for an exact (case-insensitive) match */
  for (i=0; fmt->vars[i].name; ++i) {
    if (!strcasecmp(key, fmt->vars[i].name)) {
      return &fmt->vars[i];
    }
  }
  /* If none, check for an abbreviated match */
  for (i=0; fmt->vars[i].name; ++i) {
    if (!strncasecmp(key, fmt->vars[i].name, keylen)) {
      log_warn(LD_CONFIG, "The abbreviation '%s' is deprecated. "
               "Please use '%s' instead",
               key, fmt->vars[i].name);
      return &fmt->vars[i];
    }
  }
  /* Okay, unrecognized option */
  return NULL;
}

/** If <b>key</b> is a configuration option, return the corresponding const
 * config_var_t.  Otherwise, if <b>key</b> is a non-standard abbreviation,
 * warn, and return the corresponding const config_var_t.  Otherwise return
 * NULL.
 */
const config_var_t *
config_find_option(const config_format_t *fmt, const char *key)
{
  return config_find_option_mutable((config_format_t*)fmt, key);
}

/** Return the number of option entries in <b>fmt</b>. */
static int
config_count_options(const config_format_t *fmt)
{
  int i;
  for (i=0; fmt->vars[i].name; ++i)
    ;
  return i;
}

/*
 * Functions to assign config options.
 */

/** <b>c</b>-\>key is known to be a real key. Update <b>options</b>
 * with <b>c</b>-\>value and return 0, or return -1 if bad value.
 *
 * Called from config_assign_line() and option_reset().
 */
static int
config_assign_value(const config_format_t *fmt, void *options,
                    config_line_t *c, char **msg)
{
  int i, ok;
  const config_var_t *var;
  void *lvalue;

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, c->key);
  tor_assert(var);

  lvalue = STRUCT_VAR_P(options, var->var_offset);

  switch (var->type) {

  case CONFIG_TYPE_INT:
  case CONFIG_TYPE_POSINT:
    i = (int)tor_parse_long(c->value, 10,
                            var->type==CONFIG_TYPE_INT ? INT_MIN : 0,
                            INT_MAX,
                            &ok, NULL);
    if (!ok) {
      tor_asprintf(msg,
          "Int keyword '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;

  case CONFIG_TYPE_UINT64: {
    uint64_t u64 = tor_parse_uint64(c->value, 10,
                                    0, UINT64_MAX, &ok, NULL);
    if (!ok) {
      tor_asprintf(msg,
          "uint64 keyword '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(uint64_t *)lvalue = u64;
    break;
  }

  case CONFIG_TYPE_CSV_INTERVAL: {
    /* We used to have entire smartlists here.  But now that all of our
     * download schedules use exponential backoff, only the first part
     * matters. */
    const char *comma = strchr(c->value, ',');
    const char *val = c->value;
    char *tmp = NULL;
    if (comma) {
      tmp = tor_strndup(c->value, comma - c->value);
      val = tmp;
    }

    i = config_parse_interval(val, &ok);
    if (!ok) {
      tor_asprintf(msg,
          "Interval '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      tor_free(tmp);
      return -1;
    }
    *(int *)lvalue = i;
    tor_free(tmp);
    break;
  }

  case CONFIG_TYPE_INTERVAL: {
    i = config_parse_interval(c->value, &ok);
    if (!ok) {
      tor_asprintf(msg,
          "Interval '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;
  }

  case CONFIG_TYPE_MSEC_INTERVAL: {
    i = config_parse_msec_interval(c->value, &ok);
    if (!ok) {
      tor_asprintf(msg,
          "Msec interval '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;
  }

  case CONFIG_TYPE_MEMUNIT: {
    uint64_t u64 = config_parse_memunit(c->value, &ok);
    if (!ok) {
      tor_asprintf(msg,
          "Value '%s %s' is malformed or out of bounds.",
          c->key, c->value);
      return -1;
    }
    *(uint64_t *)lvalue = u64;
    break;
  }

  case CONFIG_TYPE_BOOL:
    i = (int)tor_parse_long(c->value, 10, 0, 1, &ok, NULL);
    if (!ok) {
      tor_asprintf(msg,
          "Boolean '%s %s' expects 0 or 1.",
          c->key, c->value);
      return -1;
    }
    *(int *)lvalue = i;
    break;

  case CONFIG_TYPE_AUTOBOOL:
    if (!strcasecmp(c->value, "auto"))
      *(int *)lvalue = -1;
    else if (!strcmp(c->value, "0"))
      *(int *)lvalue = 0;
    else if (!strcmp(c->value, "1"))
      *(int *)lvalue = 1;
    else {
      tor_asprintf(msg, "Boolean '%s %s' expects 0, 1, or 'auto'.",
                   c->key, c->value);
      return -1;
    }
    break;

  case CONFIG_TYPE_STRING:
  case CONFIG_TYPE_FILENAME:
    tor_free(*(char **)lvalue);
    *(char **)lvalue = tor_strdup(c->value);
    break;

  case CONFIG_TYPE_DOUBLE:
    *(double *)lvalue = atof(c->value);
    break;

  case CONFIG_TYPE_ISOTIME:
    if (parse_iso_time(c->value, (time_t *)lvalue)) {
      tor_asprintf(msg,
          "Invalid time '%s' for keyword '%s'", c->value, c->key);
      return -1;
    }
    break;

  case CONFIG_TYPE_ROUTERSET:
    if (*(routerset_t**)lvalue) {
      routerset_free(*(routerset_t**)lvalue);
    }
    *(routerset_t**)lvalue = routerset_new();
    if (routerset_parse(*(routerset_t**)lvalue, c->value, c->key)<0) {
      tor_asprintf(msg, "Invalid exit list '%s' for option '%s'",
                   c->value, c->key);
      return -1;
    }
    break;

  case CONFIG_TYPE_CSV:
    if (*(smartlist_t**)lvalue) {
      SMARTLIST_FOREACH(*(smartlist_t**)lvalue, char *, cp, tor_free(cp));
      smartlist_clear(*(smartlist_t**)lvalue);
    } else {
      *(smartlist_t**)lvalue = smartlist_new();
    }

    smartlist_split_string(*(smartlist_t**)lvalue, c->value, ",",
                           SPLIT_SKIP_SPACE|SPLIT_IGNORE_BLANK, 0);
    break;

  case CONFIG_TYPE_LINELIST:
  case CONFIG_TYPE_LINELIST_S:
    {
      config_line_t *lastval = *(config_line_t**)lvalue;
      if (lastval && lastval->fragile) {
        if (c->command != CONFIG_LINE_APPEND) {
          config_free_lines(lastval);
          *(config_line_t**)lvalue = NULL;
        } else {
          lastval->fragile = 0;
        }
      }

      config_line_append((config_line_t**)lvalue, c->key, c->value);
    }
    break;
  case CONFIG_TYPE_OBSOLETE:
    log_warn(LD_CONFIG, "Skipping obsolete configuration option '%s'", c->key);
    break;
  case CONFIG_TYPE_LINELIST_V:
    tor_asprintf(msg,
        "You may not provide a value for virtual option '%s'", c->key);
    return -1;
    // LCOV_EXCL_START
  default:
    tor_assert_unreached();
    break;
    // LCOV_EXCL_STOP
  }
  return 0;
}

/** Mark every linelist in <b>options</b> "fragile", so that fresh assignments
 * to it will replace old ones. */
static void
config_mark_lists_fragile(const config_format_t *fmt, void *options)
{
  int i;
  tor_assert(fmt);
  tor_assert(options);

  for (i = 0; fmt->vars[i].name; ++i) {
    const config_var_t *var = &fmt->vars[i];
    config_line_t *list;
    if (var->type != CONFIG_TYPE_LINELIST &&
        var->type != CONFIG_TYPE_LINELIST_V)
      continue;

    list = *(config_line_t **)STRUCT_VAR_P(options, var->var_offset);
    if (list)
      list->fragile = 1;
  }
}

void
warn_deprecated_option(const char *what, const char *why)
{
  const char *space = (why && strlen(why)) ? " " : "";
  log_warn(LD_CONFIG, "The %s option is deprecated, and will most likely "
           "be removed in a future version of Tor.%s%s (If you think this is "
           "a mistake, please let us know!)",
           what, space, why);
}

/** If <b>c</b> is a syntactically valid configuration line, update
 * <b>options</b> with its value and return 0.  Otherwise return -1 for bad
 * key, -2 for bad value.
 *
 * If <b>clear_first</b> is set, clear the value first. Then if
 * <b>use_defaults</b> is set, set the value to the default.
 *
 * Called from config_assign().
 */
static int
config_assign_line(const config_format_t *fmt, void *options,
                   config_line_t *c, unsigned flags,
                   bitarray_t *options_seen, char **msg)
{
  const unsigned use_defaults = flags & CAL_USE_DEFAULTS;
  const unsigned clear_first = flags & CAL_CLEAR_FIRST;
  const unsigned warn_deprecations = flags & CAL_WARN_DEPRECATIONS;
  const config_var_t *var;

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, c->key);
  if (!var) {
    if (fmt->extra) {
      void *lvalue = STRUCT_VAR_P(options, fmt->extra->var_offset);
      log_info(LD_CONFIG,
               "Found unrecognized option '%s'; saving it.", c->key);
      config_line_append((config_line_t**)lvalue, c->key, c->value);
      return 0;
    } else {
      tor_asprintf(msg,
                "Unknown option '%s'.  Failing.", c->key);
      return -1;
    }
  }

  /* Put keyword into canonical case. */
  if (strcmp(var->name, c->key)) {
    tor_free(c->key);
    c->key = tor_strdup(var->name);
  }

  const char *deprecation_msg;
  if (warn_deprecations &&
      (deprecation_msg = config_find_deprecation(fmt, var->name))) {
    warn_deprecated_option(var->name, deprecation_msg);
  }

  if (!strlen(c->value)) {
    /* reset or clear it, then return */
    if (!clear_first) {
      if ((var->type == CONFIG_TYPE_LINELIST ||
           var->type == CONFIG_TYPE_LINELIST_S) &&
          c->command != CONFIG_LINE_CLEAR) {
        /* We got an empty linelist from the torrc or command line.
           As a special case, call this an error. Warn and ignore. */
        log_warn(LD_CONFIG,
                 "Linelist option '%s' has no value. Skipping.", c->key);
      } else { /* not already cleared */
        config_reset(fmt, options, var, use_defaults);
      }
    }
    return 0;
  } else if (c->command == CONFIG_LINE_CLEAR && !clear_first) {
    // XXXX This is unreachable, since a CLEAR line always has an
    // XXXX empty value.
    config_reset(fmt, options, var, use_defaults); // LCOV_EXCL_LINE
  }

  if (options_seen && (var->type != CONFIG_TYPE_LINELIST &&
                       var->type != CONFIG_TYPE_LINELIST_S)) {
    /* We're tracking which options we've seen, and this option is not
     * supposed to occur more than once. */
    int var_index = (int)(var - fmt->vars);
    if (bitarray_is_set(options_seen, var_index)) {
      log_warn(LD_CONFIG, "Option '%s' used more than once; all but the last "
               "value will be ignored.", var->name);
    }
    bitarray_set(options_seen, var_index);
  }

  if (config_assign_value(fmt, options, c, msg) < 0)
    return -2;
  return 0;
}

/** Restore the option named <b>key</b> in options to its default value.
 * Called from config_assign(). */
STATIC void
config_reset_line(const config_format_t *fmt, void *options,
                  const char *key, int use_defaults)
{
  const config_var_t *var;

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, key);
  if (!var)
    return; /* give error on next pass. */

  config_reset(fmt, options, var, use_defaults);
}

/** Return true iff value needs to be quoted and escaped to be used in
 * a configuration file. */
static int
config_value_needs_escape(const char *value)
{
  if (*value == '\"')
    return 1;
  while (*value) {
    switch (*value)
    {
    case '\r':
    case '\n':
    case '#':
      /* Note: quotes and backspaces need special handling when we are using
       * quotes, not otherwise, so they don't trigger escaping on their
       * own. */
      return 1;
    default:
      if (!TOR_ISPRINT(*value))
        return 1;
    }
    ++value;
  }
  return 0;
}

/** Return newly allocated line or lines corresponding to <b>key</b> in the
 * configuration <b>options</b>.  If <b>escape_val</b> is true and a
 * value needs to be quoted before it's put in a config file, quote and
 * escape that value. Return NULL if no such key exists. */
config_line_t *
config_get_assigned_option(const config_format_t *fmt, const void *options,
                           const char *key, int escape_val)
{
  const config_var_t *var;
  const void *value;
  config_line_t *result;
  tor_assert(options && key);

  CONFIG_CHECK(fmt, options);

  var = config_find_option(fmt, key);
  if (!var) {
    log_warn(LD_CONFIG, "Unknown option '%s'.  Failing.", key);
    return NULL;
  }
  value = STRUCT_VAR_P(options, var->var_offset);

  result = tor_malloc_zero(sizeof(config_line_t));
  result->key = tor_strdup(var->name);
  switch (var->type)
    {
    case CONFIG_TYPE_STRING:
    case CONFIG_TYPE_FILENAME:
      if (*(char**)value) {
        result->value = tor_strdup(*(char**)value);
      } else {
        tor_free(result->key);
        tor_free(result);
        return NULL;
      }
      break;
    case CONFIG_TYPE_ISOTIME:
      if (*(time_t*)value) {
        result->value = tor_malloc(ISO_TIME_LEN+1);
        format_iso_time(result->value, *(time_t*)value);
      } else {
        tor_free(result->key);
        tor_free(result);
      }
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_CSV_INTERVAL:
    case CONFIG_TYPE_INTERVAL:
    case CONFIG_TYPE_MSEC_INTERVAL:
    case CONFIG_TYPE_POSINT:
    case CONFIG_TYPE_INT:
      /* This means every or_options_t uint or bool element
       * needs to be an int. Not, say, a uint16_t or char. */
      tor_asprintf(&result->value, "%d", *(int*)value);
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_UINT64: /* Fall through */
    case CONFIG_TYPE_MEMUNIT:
      tor_asprintf(&result->value, "%"PRIu64,
                   (*(uint64_t*)value));
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_DOUBLE:
      tor_asprintf(&result->value, "%f", *(double*)value);
      escape_val = 0; /* Can't need escape. */
      break;

    case CONFIG_TYPE_AUTOBOOL:
      if (*(int*)value == -1) {
        result->value = tor_strdup("auto");
        escape_val = 0;
        break;
      }
      /* fall through */
    case CONFIG_TYPE_BOOL:
      result->value = tor_strdup(*(int*)value ? "1" : "0");
      escape_val = 0; /* Can't need escape. */
      break;
    case CONFIG_TYPE_ROUTERSET:
      result->value = routerset_to_string(*(routerset_t**)value);
      break;
    case CONFIG_TYPE_CSV:
      if (*(smartlist_t**)value)
        result->value =
          smartlist_join_strings(*(smartlist_t**)value, ",", 0, NULL);
      else
        result->value = tor_strdup("");
      break;
    case CONFIG_TYPE_OBSOLETE:
      log_fn(LOG_INFO, LD_CONFIG,
             "You asked me for the value of an obsolete config option '%s'.",
             key);
      tor_free(result->key);
      tor_free(result);
      return NULL;
    case CONFIG_TYPE_LINELIST_S:
      tor_free(result->key);
      tor_free(result);
      result = config_lines_dup_and_filter(*(const config_line_t **)value,
                                           key);
      break;
    case CONFIG_TYPE_LINELIST:
    case CONFIG_TYPE_LINELIST_V:
      tor_free(result->key);
      tor_free(result);
      result = config_lines_dup(*(const config_line_t**)value);
      break;
      // LCOV_EXCL_START
    default:
      tor_free(result->key);
      tor_free(result);
      log_warn(LD_BUG,"Unknown type %d for known key '%s'",
               var->type, key);
      return NULL;
      // LCOV_EXCL_STOP
    }

  if (escape_val) {
    config_line_t *line;
    for (line = result; line; line = line->next) {
      if (line->value && config_value_needs_escape(line->value)) {
        char *newval = esc_for_log(line->value);
        tor_free(line->value);
        line->value = newval;
      }
    }
  }

  return result;
}
/** Iterate through the linked list of requested options <b>list</b>.
 * For each item, convert as appropriate and assign to <b>options</b>.
 * If an item is unrecognized, set *msg and return -1 immediately,
 * else return 0 for success.
 *
 * If <b>clear_first</b>, interpret config options as replacing (not
 * extending) their previous values. If <b>clear_first</b> is set,
 * then <b>use_defaults</b> to decide if you set to defaults after
 * clearing, or make the value 0 or NULL.
 *
 * Here are the use cases:
 * 1. A non-empty AllowInvalid line in your torrc. Appends to current
 *    if linelist, replaces current if csv.
 * 2. An empty AllowInvalid line in your torrc. Should clear it.
 * 3. "RESETCONF AllowInvalid" sets it to default.
 * 4. "SETCONF AllowInvalid" makes it NULL.
 * 5. "SETCONF AllowInvalid=foo" clears it and sets it to "foo".
 *
 * Use_defaults   Clear_first
 *    0                0       "append"
 *    1                0       undefined, don't use
 *    0                1       "set to null first"
 *    1                1       "set to defaults first"
 * Return 0 on success, -1 on bad key, -2 on bad value.
 *
 * As an additional special case, if a LINELIST config option has
 * no value and clear_first is 0, then warn and ignore it.
 */

/*
There are three call cases for config_assign() currently.

Case one: Torrc entry
options_init_from_torrc() calls config_assign(0, 0)
  calls config_assign_line(0, 0).
    if value is empty, calls config_reset(0) and returns.
    calls config_assign_value(), appends.

Case two: setconf
options_trial_assign() calls config_assign(0, 1)
  calls config_reset_line(0)
    calls config_reset(0)
      calls option_clear().
  calls config_assign_line(0, 1).
    if value is empty, returns.
    calls config_assign_value(), appends.

Case three: resetconf
options_trial_assign() calls config_assign(1, 1)
  calls config_reset_line(1)
    calls config_reset(1)
      calls option_clear().
      calls config_assign_value(default)
  calls config_assign_line(1, 1).
    returns.
*/
int
config_assign(const config_format_t *fmt, void *options, config_line_t *list,
              unsigned config_assign_flags, char **msg)
{
  config_line_t *p;
  bitarray_t *options_seen;
  const int n_options = config_count_options(fmt);
  const unsigned clear_first = config_assign_flags & CAL_CLEAR_FIRST;
  const unsigned use_defaults = config_assign_flags & CAL_USE_DEFAULTS;

  CONFIG_CHECK(fmt, options);

  /* pass 1: normalize keys */
  for (p = list; p; p = p->next) {
    const char *full = config_expand_abbrev(fmt, p->key, 0, 1);
    if (strcmp(full,p->key)) {
      tor_free(p->key);
      p->key = tor_strdup(full);
    }
  }

  /* pass 2: if we're reading from a resetting source, clear all
   * mentioned config options, and maybe set to their defaults. */
  if (clear_first) {
    for (p = list; p; p = p->next)
      config_reset_line(fmt, options, p->key, use_defaults);
  }

  options_seen = bitarray_init_zero(n_options);
  /* pass 3: assign. */
  while (list) {
    int r;
    if ((r=config_assign_line(fmt, options, list, config_assign_flags,
                              options_seen, msg))) {
      bitarray_free(options_seen);
      return r;
    }
    list = list->next;
  }
  bitarray_free(options_seen);

  /** Now we're done assigning a group of options to the configuration.
   * Subsequent group assignments should _replace_ linelists, not extend
   * them. */
  config_mark_lists_fragile(fmt, options);

  return 0;
}

/** Reset config option <b>var</b> to 0, 0.0, NULL, or the equivalent.
 * Called from config_reset() and config_free(). */
static void
config_clear(const config_format_t *fmt, void *options,
             const config_var_t *var)
{
  void *lvalue = STRUCT_VAR_P(options, var->var_offset);
  (void)fmt; /* unused */
  switch (var->type) {
    case CONFIG_TYPE_STRING:
    case CONFIG_TYPE_FILENAME:
      tor_free(*(char**)lvalue);
      break;
    case CONFIG_TYPE_DOUBLE:
      *(double*)lvalue = 0.0;
      break;
    case CONFIG_TYPE_ISOTIME:
      *(time_t*)lvalue = 0;
      break;
    case CONFIG_TYPE_CSV_INTERVAL:
    case CONFIG_TYPE_INTERVAL:
    case CONFIG_TYPE_MSEC_INTERVAL:
    case CONFIG_TYPE_POSINT:
    case CONFIG_TYPE_INT:
    case CONFIG_TYPE_BOOL:
      *(int*)lvalue = 0;
      break;
    case CONFIG_TYPE_AUTOBOOL:
      *(int*)lvalue = -1;
      break;
    case CONFIG_TYPE_UINT64:
    case CONFIG_TYPE_MEMUNIT:
      *(uint64_t*)lvalue = 0;
      break;
    case CONFIG_TYPE_ROUTERSET:
      if (*(routerset_t**)lvalue) {
        routerset_free(*(routerset_t**)lvalue);
        *(routerset_t**)lvalue = NULL;
      }
      break;
    case CONFIG_TYPE_CSV:
      if (*(smartlist_t**)lvalue) {
        SMARTLIST_FOREACH(*(smartlist_t **)lvalue, char *, cp, tor_free(cp));
        smartlist_free(*(smartlist_t **)lvalue);
        *(smartlist_t **)lvalue = NULL;
      }
      break;
    case CONFIG_TYPE_LINELIST:
    case CONFIG_TYPE_LINELIST_S:
      config_free_lines(*(config_line_t **)lvalue);
      *(config_line_t **)lvalue = NULL;
      break;
    case CONFIG_TYPE_LINELIST_V:
      /* handled by linelist_s. */
      break;
    case CONFIG_TYPE_OBSOLETE:
      break;
  }
}

/** Clear the option indexed by <b>var</b> in <b>options</b>. Then if
 * <b>use_defaults</b>, set it to its default value.
 * Called by config_init() and option_reset_line() and option_assign_line(). */
static void
config_reset(const config_format_t *fmt, void *options,
             const config_var_t *var, int use_defaults)
{
  config_line_t *c;
  char *msg = NULL;
  CONFIG_CHECK(fmt, options);
  config_clear(fmt, options, var); /* clear it first */
  if (!use_defaults)
    return; /* all done */
  if (var->initvalue) {
    c = tor_malloc_zero(sizeof(config_line_t));
    c->key = tor_strdup(var->name);
    c->value = tor_strdup(var->initvalue);
    if (config_assign_value(fmt, options, c, &msg) < 0) {
      // LCOV_EXCL_START
      log_warn(LD_BUG, "Failed to assign default: %s", msg);
      tor_free(msg); /* if this happens it's a bug */
      // LCOV_EXCL_STOP
    }
    config_free_lines(c);
  }
}

/** Release storage held by <b>options</b>. */
void
config_free_(const config_format_t *fmt, void *options)
{
  int i;

  if (!options)
    return;

  tor_assert(fmt);

  for (i=0; fmt->vars[i].name; ++i)
    config_clear(fmt, options, &(fmt->vars[i]));
  if (fmt->extra) {
    config_line_t **linep = STRUCT_VAR_P(options, fmt->extra->var_offset);
    config_free_lines(*linep);
    *linep = NULL;
  }
  tor_free(options);
}

/** Return true iff the option <b>name</b> has the same value in <b>o1</b>
 * and <b>o2</b>.  Must not be called for LINELIST_S or OBSOLETE options.
 */
int
config_is_same(const config_format_t *fmt,
               const void *o1, const void *o2,
               const char *name)
{
  config_line_t *c1, *c2;
  int r = 1;
  CONFIG_CHECK(fmt, o1);
  CONFIG_CHECK(fmt, o2);

  c1 = config_get_assigned_option(fmt, o1, name, 0);
  c2 = config_get_assigned_option(fmt, o2, name, 0);
  r = config_lines_eq(c1, c2);
  config_free_lines(c1);
  config_free_lines(c2);
  return r;
}

/** Copy storage held by <b>old</b> into a new or_options_t and return it. */
void *
config_dup(const config_format_t *fmt, const void *old)
{
  void *newopts;
  int i;
  config_line_t *line;

  newopts = config_new(fmt);
  for (i=0; fmt->vars[i].name; ++i) {
    if (fmt->vars[i].type == CONFIG_TYPE_LINELIST_S)
      continue;
    if (fmt->vars[i].type == CONFIG_TYPE_OBSOLETE)
      continue;
    line = config_get_assigned_option(fmt, old, fmt->vars[i].name, 0);
    if (line) {
      char *msg = NULL;
      if (config_assign(fmt, newopts, line, 0, &msg) < 0) {
        // LCOV_EXCL_START
        log_err(LD_BUG, "config_get_assigned_option() generated "
                "something we couldn't config_assign(): %s", msg);
        tor_free(msg);
        tor_assert(0);
        // LCOV_EXCL_STOP
      }
    }
    config_free_lines(line);
  }
  return newopts;
}
/** Set all vars in the configuration object <b>options</b> to their default
 * values. */
void
config_init(const config_format_t *fmt, void *options)
{
  int i;
  const config_var_t *var;
  CONFIG_CHECK(fmt, options);

  for (i=0; fmt->vars[i].name; ++i) {
    var = &fmt->vars[i];
    if (!var->initvalue)
      continue; /* defaults to NULL or 0 */
    config_reset(fmt, options, var, 1);
  }
}

/** Allocate and return a new string holding the written-out values of the vars
 * in 'options'.  If 'minimal', do not write out any default-valued vars.
 * Else, if comment_defaults, write default values as comments.
 */
char *
config_dump(const config_format_t *fmt, const void *default_options,
            const void *options, int minimal,
            int comment_defaults)
{
  smartlist_t *elements;
  const void *defaults = default_options;
  void *defaults_tmp = NULL;
  config_line_t *line, *assigned;
  char *result;
  int i;
  char *msg = NULL;

  if (defaults == NULL) {
    defaults = defaults_tmp = config_new(fmt);
    config_init(fmt, defaults_tmp);
  }

  /* XXX use a 1 here so we don't add a new log line while dumping */
  if (default_options == NULL) {
    if (fmt->validate_fn(NULL, defaults_tmp, defaults_tmp, 1, &msg) < 0) {
      // LCOV_EXCL_START
      log_err(LD_BUG, "Failed to validate default config: %s", msg);
      tor_free(msg);
      tor_assert(0);
      // LCOV_EXCL_STOP
    }
  }

  elements = smartlist_new();
  for (i=0; fmt->vars[i].name; ++i) {
    int comment_option = 0;
    if (fmt->vars[i].type == CONFIG_TYPE_OBSOLETE ||
        fmt->vars[i].type == CONFIG_TYPE_LINELIST_S)
      continue;
    /* Don't save 'hidden' control variables. */
    if (!strcmpstart(fmt->vars[i].name, "__"))
      continue;
    if (minimal && config_is_same(fmt, options, defaults, fmt->vars[i].name))
      continue;
    else if (comment_defaults &&
             config_is_same(fmt, options, defaults, fmt->vars[i].name))
      comment_option = 1;

    line = assigned =
      config_get_assigned_option(fmt, options, fmt->vars[i].name, 1);

    for (; line; line = line->next) {
      if (!strcmpstart(line->key, "__")) {
        /* This check detects "hidden" variables inside LINELIST_V structures.
         */
        continue;
      }
      smartlist_add_asprintf(elements, "%s%s %s\n",
                   comment_option ? "# " : "",
                   line->key, line->value);
    }
    config_free_lines(assigned);
  }

  if (fmt->extra) {
    line = *(config_line_t**)STRUCT_VAR_P(options, fmt->extra->var_offset);
    for (; line; line = line->next) {
      smartlist_add_asprintf(elements, "%s %s\n", line->key, line->value);
    }
  }

  result = smartlist_join_strings(elements, "", 0, NULL);
  SMARTLIST_FOREACH(elements, char *, cp, tor_free(cp));
  smartlist_free(elements);
  if (defaults_tmp) {
    fmt->free_fn(defaults_tmp);
  }
  return result;
}

/** Mapping from a unit name to a multiplier for converting that unit into a
 * base unit.  Used by config_parse_unit. */
struct unit_table_t {
  const char *unit; /**< The name of the unit */
  uint64_t multiplier; /**< How many of the base unit appear in this unit */
};

/** Table to map the names of memory units to the number of bytes they
 * contain. */
static struct unit_table_t memory_units[] = {
  { "",          1 },
  { "b",         1<< 0 },
  { "byte",      1<< 0 },
  { "bytes",     1<< 0 },
  { "kb",        1<<10 },
  { "kbyte",     1<<10 },
  { "kbytes",    1<<10 },
  { "kilobyte",  1<<10 },
  { "kilobytes", 1<<10 },
  { "kilobits",  1<<7  },
  { "kilobit",   1<<7  },
  { "kbits",     1<<7  },
  { "kbit",      1<<7  },
  { "m",         1<<20 },
  { "mb",        1<<20 },
  { "mbyte",     1<<20 },
  { "mbytes",    1<<20 },
  { "megabyte",  1<<20 },
  { "megabytes", 1<<20 },
  { "megabits",  1<<17 },
  { "megabit",   1<<17 },
  { "mbits",     1<<17 },
  { "mbit",      1<<17 },
  { "gb",        1<<30 },
  { "gbyte",     1<<30 },
  { "gbytes",    1<<30 },
  { "gigabyte",  1<<30 },
  { "gigabytes", 1<<30 },
  { "gigabits",  1<<27 },
  { "gigabit",   1<<27 },
  { "gbits",     1<<27 },
  { "gbit",      1<<27 },
  { "tb",        UINT64_C(1)<<40 },
  { "tbyte",     UINT64_C(1)<<40 },
  { "tbytes",    UINT64_C(1)<<40 },
  { "terabyte",  UINT64_C(1)<<40 },
  { "terabytes", UINT64_C(1)<<40 },
  { "terabits",  UINT64_C(1)<<37 },
  { "terabit",   UINT64_C(1)<<37 },
  { "tbits",     UINT64_C(1)<<37 },
  { "tbit",      UINT64_C(1)<<37 },
  { NULL, 0 },
};

/** Table to map the names of time units to the number of seconds they
 * contain. */
static struct unit_table_t time_units[] = {
  { "",         1 },
  { "second",   1 },
  { "seconds",  1 },
  { "minute",   60 },
  { "minutes",  60 },
  { "hour",     60*60 },
  { "hours",    60*60 },
  { "day",      24*60*60 },
  { "days",     24*60*60 },
  { "week",     7*24*60*60 },
  { "weeks",    7*24*60*60 },
  { "month",    2629728, }, /* about 30.437 days */
  { "months",   2629728, },
  { NULL, 0 },
};

/** Table to map the names of time units to the number of milliseconds
 * they contain. */
static struct unit_table_t time_msec_units[] = {
  { "",         1 },
  { "msec",     1 },
  { "millisecond", 1 },
  { "milliseconds", 1 },
  { "second",   1000 },
  { "seconds",  1000 },
  { "minute",   60*1000 },
  { "minutes",  60*1000 },
  { "hour",     60*60*1000 },
  { "hours",    60*60*1000 },
  { "day",      24*60*60*1000 },
  { "days",     24*60*60*1000 },
  { "week",     7*24*60*60*1000 },
  { "weeks",    7*24*60*60*1000 },
  { NULL, 0 },
};

/** Parse a string <b>val</b> containing a number, zero or more
 * spaces, and an optional unit string.  If the unit appears in the
 * table <b>u</b>, then multiply the number by the unit multiplier.
 * On success, set *<b>ok</b> to 1 and return this product.
 * Otherwise, set *<b>ok</b> to 0.
 */
static uint64_t
config_parse_units(const char *val, struct unit_table_t *u, int *ok)
{
  uint64_t v = 0;
  double d = 0;
  int use_float = 0;
  char *cp;

  tor_assert(ok);

  v = tor_parse_uint64(val, 10, 0, UINT64_MAX, ok, &cp);
  if (!*ok || (cp && *cp == '.')) {
    d = tor_parse_double(val, 0, (double)UINT64_MAX, ok, &cp);
    if (!*ok)
      goto done;
    use_float = 1;
  }

  if (!cp) {
    *ok = 1;
    v = use_float ? ((uint64_t)d) :  v;
    goto done;
  }

  cp = (char*) eat_whitespace(cp);

  for ( ;u->unit;++u) {
    if (!strcasecmp(u->unit, cp)) {
      if (use_float)
        v = (uint64_t)(u->multiplier * d);
      else
        v *= u->multiplier;
      *ok = 1;
      goto done;
    }
  }
  log_warn(LD_CONFIG, "Unknown unit '%s'.", cp);
  *ok = 0;
 done:

  if (*ok)
    return v;
  else
    return 0;
}

/** Parse a string in the format "number unit", where unit is a unit of
 * information (byte, KB, M, etc).  On success, set *<b>ok</b> to true
 * and return the number of bytes specified.  Otherwise, set
 * *<b>ok</b> to false and return 0. */
static uint64_t
config_parse_memunit(const char *s, int *ok)
{
  uint64_t u = config_parse_units(s, memory_units, ok);
  return u;
}

/** Parse a string in the format "number unit", where unit is a unit of
 * time in milliseconds.  On success, set *<b>ok</b> to true and return
 * the number of milliseconds in the provided interval.  Otherwise, set
 * *<b>ok</b> to 0 and return -1. */
static int
config_parse_msec_interval(const char *s, int *ok)
{
  uint64_t r;
  r = config_parse_units(s, time_msec_units, ok);
  if (r > INT_MAX) {
    log_warn(LD_CONFIG, "Msec interval '%s' is too long", s);
    *ok = 0;
    return -1;
  }
  return (int)r;
}

/** Parse a string in the format "number unit", where unit is a unit of time.
 * On success, set *<b>ok</b> to true and return the number of seconds in
 * the provided interval.  Otherwise, set *<b>ok</b> to 0 and return -1.
 */
static int
config_parse_interval(const char *s, int *ok)
{
  uint64_t r;
  r = config_parse_units(s, time_units, ok);
  if (r > INT_MAX) {
    log_warn(LD_CONFIG, "Interval '%s' is too long", s);
    *ok = 0;
    return -1;
  }
  return (int)r;
}
