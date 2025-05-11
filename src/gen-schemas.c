/*
 * Copyright (C) 2025 Rafał Dzięgiel <rafostar.github@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <glib.h>
#include <glib/gprintf.h>
#include <gio/gio.h>
#include <clapper/clapper.h>

#define SCHEMA_BASE_ID "com.github.rafostar.Clapper.Enhancers"
#define SCHEMA_BASE_PATH "/com/github/rafostar/Clapper/Enhancers/"

#define INDENT1 "  "
#define INDENT2 INDENT1 INDENT1
#define INDENT3 INDENT2 INDENT1

#define SCHEMA_WRITE(fmt, ...) ({                                                                    \
    gchar _buffer[1024];                                                                             \
    gint _len = g_snprintf (_buffer, sizeof (_buffer), fmt "\n", ##__VA_ARGS__);                     \
    _len > 0 && *error == NULL && g_output_stream_write ((GOutputStream *) ostream, _buffer, _len, NULL, error) > 0; })

static const gchar *
_type_to_variant_format (GType type)
{
  switch (type) {
    case G_TYPE_BOOLEAN:
      return "b";
    case G_TYPE_INT:
      return "i";
    case G_TYPE_UINT:
      return "u";
    case G_TYPE_DOUBLE:
      return "d";
    case G_TYPE_STRING:
      return "s";
    default:
      return ""; // Error is set in switch creating "default"
  }
}

static gboolean
compile_schema (ClapperEnhancerProxy *proxy, GError **error)
{
  GFile *file;
  GFileOutputStream *ostream;
  GParamSpec **pspecs;
  guint i, n_pspecs;
  gchar name_str[64];
  gboolean has_props = FALSE;
  const gchar *module_name, *module_dir;

  if ((pspecs = clapper_enhancer_proxy_get_target_properties (proxy, &n_pspecs))) {
    for (i = 0; i < n_pspecs; ++i) {
      if ((has_props = (pspecs[i]->flags & CLAPPER_ENHANCER_PARAM_GLOBAL)))
        break;
    }
  }

  if (!has_props)
    return TRUE; // Nothing to do

  module_name = clapper_enhancer_proxy_get_module_name (proxy);
  module_dir = clapper_enhancer_proxy_get_module_dir (proxy);

  g_print ("Generating settings schema in %s\n", module_dir);

  g_snprintf (name_str, sizeof (name_str), "%s.gschema.xml", module_name);
  file = g_file_new_build_filename (module_dir, name_str, NULL);
  ostream = g_file_replace (file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, error);

  g_object_unref (file);

  if (!ostream)
    return FALSE;

  /* Check once if we can write */
  if (!SCHEMA_WRITE ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>")) {
    if (*error == NULL) {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
          "Could not write data into schema file");
    }
    g_object_unref (ostream);
    return FALSE;
  }

  SCHEMA_WRITE ("<schemalist>");

  /* Write enums and flags */
  for (i = 0; i < n_pspecs; ++i) {
    GParamSpec *pspec = pspecs[i];

    if (!(pspec->flags & CLAPPER_ENHANCER_PARAM_GLOBAL))
      continue;

    if (G_IS_PARAM_SPEC_ENUM (pspec)) {
      GEnumClass *enum_class = G_ENUM_CLASS (g_type_class_peek (pspec->value_type));
      guint j;

      SCHEMA_WRITE (INDENT1 "<enum id=\"" SCHEMA_BASE_ID ".%s.%s\">",
          module_name, g_type_name (pspec->value_type));

      for (j = 0; j < enum_class->n_values; ++j) {
        SCHEMA_WRITE (INDENT2 "<value value=\"%i\" nick=\"%s\"/>",
            enum_class->values[j].value, enum_class->values[j].value_nick);
      }

      SCHEMA_WRITE (INDENT1 "</enum>");
    } else if (G_IS_PARAM_SPEC_FLAGS (pspec)) {
      GFlagsClass *flags_class = G_FLAGS_CLASS (g_type_class_peek (pspec->value_type));
      guint j;

      SCHEMA_WRITE (INDENT1 "<flags id=\"" SCHEMA_BASE_ID ".%s.%s\">",
          module_name, g_type_name (pspec->value_type));

      for (j = 0; j < flags_class->n_values; ++j) {
        SCHEMA_WRITE (INDENT2 "<value value=\"%u\" nick=\"%s\"/>",
            flags_class->values[j].value, flags_class->values[j].value_nick);
      }

      SCHEMA_WRITE (INDENT1 "</flags>");
    }
  }

  SCHEMA_WRITE (INDENT1 "<schema id=\"" SCHEMA_BASE_ID ".%s\" path=\"" SCHEMA_BASE_PATH "%s/\">",
      module_name, module_name);

  /* Write schema keys */
  for (i = 0; i < n_pspecs; ++i) {
    GParamSpec *pspec = pspecs[i];

    if (!(pspec->flags & CLAPPER_ENHANCER_PARAM_GLOBAL))
      continue;

    if (G_IS_PARAM_SPEC_ENUM (pspec)) {
      GParamSpecEnum *p = (GParamSpecEnum *) pspec;
      GEnumClass *enum_class = G_ENUM_CLASS (g_type_class_peek (pspec->value_type));
      const gchar *def_nick = NULL;
      guint j;

      SCHEMA_WRITE (INDENT2 "<key name=\"%s\" enum=\"" SCHEMA_BASE_ID ".%s.%s\">",
          pspec->name, module_name, g_type_name (pspec->value_type));

      for (j = 0; j < enum_class->n_values; ++j) {
        if (enum_class->values[j].value == p->default_value) {
          def_nick = enum_class->values[j].value_nick;
          break;
        }
      }

      if (G_LIKELY (def_nick != NULL)) {
        SCHEMA_WRITE (INDENT3 "<default>\"%s\"</default>", def_nick);
      } else if (*error == NULL) {
        g_set_error (error, G_VARIANT_PARSE_ERROR, G_VARIANT_PARSE_ERROR_FAILED,
            "Invalid default value of enum: %s", g_type_name (pspec->value_type));
      }
    } else if (G_IS_PARAM_SPEC_FLAGS (pspec)) {
      GParamSpecFlags *p = (GParamSpecFlags *) pspec;
      GFlagsClass *flags_class = G_FLAGS_CLASS (g_type_class_peek (pspec->value_type));
      GString *def_string = g_string_new ("[");
      gchar *def_flags;
      guint j;

      SCHEMA_WRITE (INDENT2 "<key name=\"%s\" flags=\"" SCHEMA_BASE_ID ".%s.%s\">",
          pspec->name, module_name, g_type_name (pspec->value_type));

      for (j = 0; j < flags_class->n_values; ++j) {
        if (p->default_value & flags_class->values[j].value) {
          if (def_string->len > 1)
            g_string_append_c (def_string, ',');

          g_string_append_printf (def_string, "\"%s\"",
              flags_class->values[j].value_nick);
        }
      }

      g_string_append_c (def_string, ']');
      def_flags = g_string_free (def_string, FALSE);

      SCHEMA_WRITE (INDENT3 "<default>%s</default>", def_flags);
      g_free (def_flags);
    } else {
      SCHEMA_WRITE (INDENT2 "<key name=\"%s\" type=\"%s\">",
          pspec->name, _type_to_variant_format (pspec->value_type));

      switch (pspec->value_type) {
        case G_TYPE_BOOLEAN:{
          GParamSpecBoolean *p = (GParamSpecBoolean *) pspec;
          SCHEMA_WRITE (INDENT3 "<default>%s</default>", p->default_value ? "true" : "false");
          break;
        }
        case G_TYPE_INT:{
          GParamSpecInt *p = (GParamSpecInt *) pspec;
          SCHEMA_WRITE (INDENT3 "<default>%i</default>", p->default_value);
          break;
        }
        case G_TYPE_UINT:{
          GParamSpecUInt *p = (GParamSpecUInt *) pspec;
          SCHEMA_WRITE (INDENT3 "<default>%u</default>", p->default_value);
          break;
        }
        case G_TYPE_DOUBLE:{
          GParamSpecDouble *p = (GParamSpecDouble *) pspec;
          SCHEMA_WRITE (INDENT3 "<default>%lf</default>", p->default_value);
          break;
        }
        case G_TYPE_STRING:{
          GParamSpecString *p = (GParamSpecString *) pspec;
          SCHEMA_WRITE (INDENT3 "<default>\"%s\"</default>",
              (p->default_value != NULL) ? p->default_value : "");
          break;
        }
        default:
          if (*error == NULL) {
            g_set_error (error, G_VARIANT_PARSE_ERROR, G_VARIANT_PARSE_ERROR_FAILED,
                "Unsupported property type: %s", g_type_name (pspec->value_type));
          }
          break;
      }
    }

    if (*error != NULL)
      break;

    SCHEMA_WRITE (INDENT2 "</key>");
  }

  if (*error != NULL) {
    g_object_unref (ostream);
    return FALSE;
  }

  SCHEMA_WRITE (INDENT1 "</schema>");
  SCHEMA_WRITE ("</schemalist>");

  g_output_stream_close (G_OUTPUT_STREAM (ostream), NULL, error);
  g_object_unref (ostream);

  if (*error == NULL) {
    const gchar *argv[] = {
        "glib-compile-schemas",
        "--targetdir", module_dir,
        module_dir,
        NULL
    };
    g_spawn_sync (NULL, (gchar **) argv, NULL, G_SPAWN_SEARCH_PATH,
        NULL, NULL, NULL, NULL, NULL, error);
  }

  return (*error == NULL);
}

gint
main (gint argc, gchar **argv)
{
  ClapperEnhancerProxyList *list;
  GString *path_string;
  GError *error = NULL;
  const gchar *destdir;
  gchar *enhancers_path;
  gchar **names;
  guint i, n_proxies;

  if (argc != 3) {
    g_printerr ("Usage: %s <SRC_DIR> <name1,name2,...>\n", argv[0]);
    return 1;
  }

#if !CLAPPER_WITH_ENHANCERS_LOADER
  g_printerr ("Cannot generate schemas when Clapper is compiled"
      " without \"enhancers-loader\"\n");
  return 1;
#endif

  destdir = g_getenv ("DESTDIR");
  path_string = g_string_new (NULL);

  names = g_strsplit (argv[2], ",", 0);
  for (i = 0; i < g_strv_length (names); ++i) {
    gchar *dir_path;

    if (i > 0)
      g_string_append_c (path_string, ':');

    if (destdir && *destdir != '\0')
      dir_path = g_build_filename (destdir, argv[1], names[i], NULL);
    else
      dir_path = g_build_filename (argv[1], names[i], NULL);

    g_string_append (path_string, dir_path);
    g_free (dir_path);
  }
  g_strfreev (names);

  enhancers_path = g_string_free (path_string, FALSE);

  g_setenv ("GST_REGISTRY_DISABLE", "yes", TRUE);
  g_setenv ("PYTHONDONTWRITEBYTECODE", "1", TRUE);
  g_setenv ("CLAPPER_DISABLE_CACHE", "1", TRUE);
  g_setenv ("CLAPPER_ENHANCERS_PATH", enhancers_path, TRUE);
  g_setenv ("CLAPPER_ENHANCERS_EXTRA_PATH", "", TRUE);

  g_free (enhancers_path);

  clapper_init (NULL, NULL);

  list = clapper_get_global_enhancer_proxies ();
  n_proxies = clapper_enhancer_proxy_list_get_n_proxies (list);

  if (n_proxies == 0) {
    g_printerr ("No enhancers found in source directory!\n");
    return 1;
  }

  for (i = 0; i < n_proxies; ++i) {
    ClapperEnhancerProxy *proxy = clapper_enhancer_proxy_list_peek_proxy (list, i);

    if (!compile_schema (proxy, &error)) {
      g_printerr ("Could not compile schema, reason: %s\n",
          (error && error->message) ? error->message : "Unknown");
      return 1;
    }
  }

  return 0;
}
