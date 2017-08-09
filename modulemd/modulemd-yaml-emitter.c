/* modulemd-yaml-emitter.c
 *
 * Copyright (C) 2017 Stephen Gallagher
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE X CONSORTIUM BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <glib.h>
#include <glib/gstdio.h>
#include <yaml.h>
#include <errno.h>
#include "modulemd.h"
#include "modulemd-yaml.h"

#define YAML_EMITTER_EMIT_WITH_ERROR_RETURN(emitter, event, error, msg)       \
  do                                                                          \
    {                                                                         \
      if (!yaml_emitter_emit (emitter, event))                                \
        {                                                                     \
          g_debug ("Error: %s", msg);                                         \
          g_set_error_literal (                                               \
            error, MODULEMD_YAML_ERROR, MODULEMD_YAML_ERROR_EMIT, msg);       \
          goto error;                                                         \
        }                                                                     \
      g_debug ("Emitter event: %u", (event)->type);                           \
    }                                                                         \
  while (0)

#define MMD_YAML_EMITTER_ERROR_RETURN(error, msg)                             \
  do                                                                          \
    {                                                                         \
      g_message (msg);                                                        \
      g_set_error_literal (                                                   \
        error, MODULEMD_YAML_ERROR, MODULEMD_YAML_ERROR_EMIT, msg);           \
      goto error;                                                             \
    }                                                                         \
  while (0)

#define MMD_YAML_EMIT_SCALAR(event, scalar, style)                            \
  do                                                                          \
    {                                                                         \
      yaml_scalar_event_initialize (event,                                    \
                                    NULL,                                     \
                                    NULL,                                     \
                                    (yaml_char_t *)scalar,                    \
                                    (int)strlen (scalar),                     \
                                    1,                                        \
                                    1,                                        \
                                    style);                                   \
      YAML_EMITTER_EMIT_WITH_ERROR_RETURN (                                   \
        emitter, event, error, "Error writing scalar");                       \
      g_clear_pointer (&scalar, g_free);                                      \
    }                                                                         \
  while (0)

#define MMD_YAML_EMIT_STR_STR_DICT(event, name, value, style)                 \
  do                                                                          \
    {                                                                         \
      yaml_scalar_event_initialize (event,                                    \
                                    NULL,                                     \
                                    NULL,                                     \
                                    (yaml_char_t *)name,                      \
                                    (int)strlen (name),                       \
                                    1,                                        \
                                    1,                                        \
                                    YAML_PLAIN_SCALAR_STYLE);                 \
      YAML_EMITTER_EMIT_WITH_ERROR_RETURN (                                   \
        emitter, event, error, "Error writing name");                         \
      g_clear_pointer (&name, g_free);                                        \
                                                                              \
      yaml_scalar_event_initialize (event,                                    \
                                    NULL,                                     \
                                    NULL,                                     \
                                    (yaml_char_t *)value,                     \
                                    (int)strlen (value),                      \
                                    1,                                        \
                                    1,                                        \
                                    style);                                   \
      YAML_EMITTER_EMIT_WITH_ERROR_RETURN (                                   \
        emitter, event, error, "Error writing value");                        \
      g_clear_pointer (&value, g_free);                                       \
    }                                                                         \
  while (0)

static gboolean
_emit_modulemd_document (yaml_emitter_t *emitter,
                         ModulemdModule *module,
                         GError **error);
static gboolean
_emit_modulemd_root (yaml_emitter_t *emitter,
                     ModulemdModule *module,
                     GError **error);
static gboolean
_emit_modulemd_data (yaml_emitter_t *emitter,
                     ModulemdModule *module,
                     GError **error);
static gboolean
_emit_modulemd_licenses (yaml_emitter_t *emitter,
                         ModulemdModule *module,
                         GError **error);

static gboolean
_emit_modulemd_simpleset (yaml_emitter_t *emitter,
                          ModulemdSimpleSet *set,
                          GError **error);

gboolean
emit_yaml_file (ModulemdModule **modules, const gchar *path, GError **error)
{
  gboolean ret = FALSE;
  FILE *yaml_file = NULL;
  yaml_emitter_t emitter;
  yaml_event_t event;

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (modules, FALSE);

  g_debug ("TRACE: entering emit_yaml_file");

  yaml_emitter_initialize (&emitter);

  errno = 0;
  yaml_file = g_fopen (path, "wb");
  if (!yaml_file)
    {
      g_set_error (error,
                   MODULEMD_YAML_ERROR,
                   MODULEMD_YAML_ERROR_OPEN,
                   "Failed to open file: %s",
                   g_strerror (errno));
      goto error;
    }

  yaml_emitter_set_output_file (&emitter, yaml_file);

  yaml_stream_start_event_initialize (&event, YAML_UTF8_ENCODING);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    &emitter, &event, error, "Error starting stream");

  for (gsize i = 0; modules[i]; i++)
    {
      /* Write out the YAML */
      if (!_emit_modulemd_document (&emitter, modules[i], error))
        {
          MMD_YAML_EMITTER_ERROR_RETURN (error, "Could not emit YAML");
        }
    }

  yaml_stream_end_event_initialize (&event);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    &emitter, &event, error, "Error ending stream");

  ret = TRUE;

error:
  yaml_emitter_delete (&emitter);
  fclose (yaml_file);

  g_debug ("TRACE: exiting emit_yaml_file");
  return ret;
}

struct modulemd_yaml_string
{
  char *str;
  size_t len;
};

static int
_write_string (void *data, unsigned char *buffer, size_t size)
{
  struct modulemd_yaml_string *yaml_string =
    (struct modulemd_yaml_string *)data;

  yaml_string->str =
    g_realloc_n (yaml_string->str, yaml_string->len + size + 1, sizeof (char));

  memcpy (yaml_string->str + yaml_string->len, buffer, size);
  yaml_string->len += size;
  yaml_string->str[yaml_string->len] = '\0';

  return 1;
}

gboolean
emit_yaml_string (ModulemdModule **modules, gchar **_yaml, GError **error)
{
  gboolean ret = FALSE;
  yaml_emitter_t emitter;
  yaml_event_t event;
  struct modulemd_yaml_string *yaml_string =
    g_malloc0_n (1, sizeof (struct modulemd_yaml_string));

  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  g_return_val_if_fail (modules, FALSE);

  g_debug ("TRACE: entering emit_yaml_string");

  yaml_emitter_initialize (&emitter);

  yaml_emitter_set_output (&emitter, _write_string, (void *)yaml_string);

  yaml_stream_start_event_initialize (&event, YAML_UTF8_ENCODING);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    &emitter, &event, error, "Error starting stream");

  for (gsize i = 0; modules[i]; i++)
    {
      /* Write out the YAML */
      if (!_emit_modulemd_document (&emitter, modules[i], error))
        {
          MMD_YAML_ERROR_RETURN_RETHROW (error, "Could not emit YAML");
        }
    }

  yaml_stream_end_event_initialize (&event);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    &emitter, &event, error, "Error ending stream");

  *_yaml = yaml_string->str;
  ret = TRUE;

error:
  yaml_emitter_delete (&emitter);
  if (ret != TRUE)
    {
      g_clear_pointer (&yaml_string->str, g_free);
    }
  g_free (yaml_string);

  g_debug ("TRACE: exiting emit_yaml_string");
  return ret;
}

static gboolean
_emit_modulemd_document (yaml_emitter_t *emitter,
                         ModulemdModule *module,
                         GError **error)
{
  gboolean ret = FALSE;
  yaml_event_t event;

  g_debug ("TRACE: entering _emit_modulemd_document");
  yaml_document_start_event_initialize (&event, NULL, NULL, NULL, 0);

  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error starting document");

  if (!_emit_modulemd_root (emitter, module, error))
    {
      MMD_YAML_ERROR_RETURN_RETHROW (error, "Failed to process root");
    }

  yaml_document_end_event_initialize (&event, 0);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error ending document");

  ret = TRUE;
error:

  g_debug ("TRACE: exiting _emit_modulemd_document");
  return ret;
}

static gboolean
_emit_modulemd_root (yaml_emitter_t *emitter,
                     ModulemdModule *module,
                     GError **error)
{
  gboolean ret = FALSE;
  yaml_event_t event;
  gchar *name = NULL;
  gchar *value = NULL;

  g_debug ("TRACE: entering _emit_modulemd_root");

  yaml_mapping_start_event_initialize (
    &event, NULL, NULL, 1, YAML_BLOCK_MAPPING_STYLE);

  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error starting root mapping");


  /* document: modulemd */
  name = g_strdup ("document");
  value = g_strdup ("modulemd");
  MMD_YAML_EMIT_STR_STR_DICT (&event, name, value, YAML_PLAIN_SCALAR_STYLE);


  /* The modulemd version */
  name = g_strdup ("version");
  value = g_strdup_printf ("%llu", modulemd_module_get_mdversion (module));
  MMD_YAML_EMIT_STR_STR_DICT (&event, name, value, YAML_PLAIN_SCALAR_STYLE);


  /* The data */
  name = g_strdup ("data");

  yaml_scalar_event_initialize (&event,
                                NULL,
                                NULL,
                                (yaml_char_t *)name,
                                (int)strlen (name),
                                1,
                                1,
                                YAML_PLAIN_SCALAR_STYLE);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error writing data");
  g_clear_pointer (&name, g_free);

  if (!_emit_modulemd_data (emitter, module, error))
    {
      MMD_YAML_ERROR_RETURN_RETHROW (error, "Failed to emit data");
    }

  yaml_mapping_end_event_initialize (&event);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error ending root mapping");

  ret = TRUE;
error:
  g_free (name);
  g_free (value);

  g_debug ("TRACE: exiting _emit_modulemd_root");
  return ret;
}

static gboolean
_emit_modulemd_data (yaml_emitter_t *emitter,
                     ModulemdModule *module,
                     GError **error)
{
  gboolean ret = FALSE;
  yaml_event_t event;
  gchar *name = NULL;
  gchar *value = NULL;
  guint64 version = 0;

  g_debug ("TRACE: entering _emit_modulemd_data");

  yaml_mapping_start_event_initialize (
    &event, NULL, NULL, 1, YAML_BLOCK_MAPPING_STYLE);

  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error starting data mapping");


  /* Module name */
  value = g_strdup (modulemd_module_get_name (module));
  if (value)
    {
      name = g_strdup ("name");
      MMD_YAML_EMIT_STR_STR_DICT (
        &event, name, value, YAML_PLAIN_SCALAR_STYLE);
    }


  /* Module stream */
  value = g_strdup (modulemd_module_get_stream (module));
  if (value)
    {
      name = g_strdup ("stream");
      MMD_YAML_EMIT_STR_STR_DICT (
        &event, name, value, YAML_PLAIN_SCALAR_STYLE);
    }


  /* Module version */
  version = modulemd_module_get_version (module);
  if (version)
    {
      name = g_strdup ("version");
      value = g_strdup_printf ("%llu", version);
      MMD_YAML_EMIT_STR_STR_DICT (
        &event, name, value, YAML_PLAIN_SCALAR_STYLE);
    }


  /* Module summary */
  value = g_strdup (modulemd_module_get_summary (module));
  if (!value)
    {
      /* Summary is mandatory */
      MMD_YAML_EMITTER_ERROR_RETURN (error,
                                     "Missing required option data.summary");
    }
  name = g_strdup ("summary");
  MMD_YAML_EMIT_STR_STR_DICT (&event, name, value, YAML_PLAIN_SCALAR_STYLE);


  /* Module description */
  value = g_strdup (modulemd_module_get_description (module));
  if (!value)
    {
      /* Description is mandatory */
      MMD_YAML_EMITTER_ERROR_RETURN (
        error, "Missing required option data.description");
    }
  name = g_strdup ("description");
  MMD_YAML_EMIT_STR_STR_DICT (&event, name, value, YAML_FOLDED_SCALAR_STYLE);

  /* Module Licenses */
  if (!_emit_modulemd_licenses (emitter, module, error))
    {
      MMD_YAML_ERROR_RETURN_RETHROW (error, "Failed to emit data");
    }


  yaml_mapping_end_event_initialize (&event);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error ending data mapping");

  ret = TRUE;
error:
  g_free (name);
  g_free (value);

  g_debug ("TRACE: exiting _emit_modulemd_data");
  return ret;
}

static gboolean
_emit_modulemd_licenses (yaml_emitter_t *emitter,
                         ModulemdModule *module,
                         GError **error)
{
  gboolean ret = FALSE;
  yaml_event_t event;
  gchar *name = NULL;
  ModulemdSimpleSet *set = NULL;
  guint64 version = 0;

  g_debug ("TRACE: entering _emit_modulemd_licenses");

  name = g_strdup ("license");
  MMD_YAML_EMIT_SCALAR (&event, name, YAML_PLAIN_SCALAR_STYLE);

  yaml_mapping_start_event_initialize (
    &event, NULL, NULL, 1, YAML_BLOCK_MAPPING_STYLE);

  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error starting license mapping");


  /* Module Licenses */
  set = modulemd_module_get_module_licenses (module);
  if (!set)
    {
      /* Module license is mandatory */
      MMD_YAML_EMITTER_ERROR_RETURN (
        error, "Missing required option data.license.module");
    }
  name = g_strdup ("module");
  MMD_YAML_EMIT_SCALAR (&event, name, YAML_PLAIN_SCALAR_STYLE);

  if (!_emit_modulemd_simpleset (emitter, set, error))
    {
      MMD_YAML_ERROR_RETURN_RETHROW (error, "Error writing module licenses");
    }
  g_clear_pointer (&set, g_object_unref);


  /* Content licenses */
  set = modulemd_module_get_content_licenses (module);
  if (set)
    {
      /* Content licenses are optional */
      name = g_strdup ("content");
      MMD_YAML_EMIT_SCALAR (&event, name, YAML_PLAIN_SCALAR_STYLE);

      if (!_emit_modulemd_simpleset (emitter, set, error))
        {
          MMD_YAML_ERROR_RETURN_RETHROW (error,
                                         "Error writing module licenses");
        }
      g_clear_pointer (&set, g_object_unref);
    }


  yaml_mapping_end_event_initialize (&event);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error ending license mapping");

  ret = TRUE;
error:
  g_free (name);
  if (set)
    {
      g_object_unref (set);
    }

  g_debug ("TRACE: exiting _emit_modulemd_licenses");
  return ret;
}

static gboolean
_emit_modulemd_simpleset (yaml_emitter_t *emitter,
                          ModulemdSimpleSet *set,
                          GError **error)
{
  gboolean ret = FALSE;
  gsize i;
  yaml_event_t event;
  gchar **strv = modulemd_simpleset_get_as_strv (set);

  g_debug ("TRACE: entering _emit_modulemd_simpleset");

  yaml_sequence_start_event_initialize (
    &event, NULL, NULL, 1, YAML_BLOCK_SEQUENCE_STYLE);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error starting simpleset sequence");

  for (i = 0; strv[i]; i++)
    {
      MMD_YAML_EMIT_SCALAR (&event, strv[i], YAML_PLAIN_SCALAR_STYLE);
    }

  yaml_sequence_end_event_initialize (&event);
  YAML_EMITTER_EMIT_WITH_ERROR_RETURN (
    emitter, &event, error, "Error ending simpleset sequence");

  ret = TRUE;
error:
  for (gsize i = 0; strv[i]; i++)
    {
      g_free (strv[i]);
    }
  g_free (strv);

  g_debug ("TRACE: exiting _emit_modulemd_simpleset");
  return ret;
}
