/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2016-2019 - Brad Parker
 *  Copyright (C) 2016-2019 - Andrés Suárez
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* TODO/FIXME - turn this into actual task */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <file/file_path.h>
#include <string/stdstring.h>

#ifdef _WIN32
#ifdef _XBOX
#include <xtl.h>
#define setmode _setmode
#define INVALID_FILE_ATTRIBUTES -1
#else
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif
#endif

#ifdef __WINRT__
#include <uwp/uwp_func.h>
#endif

#ifdef HAVE_CONFIG_H
#include "../config.h"
#endif

#include <boolean.h>

#include <encodings/crc32.h>
#include <compat/strl.h>
#include <compat/posix_string.h>
#include <file/file_path.h>
#include <file/archive_file.h>
#include <string/stdstring.h>

#include <retro_miscellaneous.h>
#include <streams/file_stream.h>
#include <retro_assert.h>

#include <lists/string_list.h>
#include <string/stdstring.h>

#ifdef HAVE_MENU
#include "../menu/menu_driver.h"
#endif

#include "../menu/menu_shader.h"

#ifdef HAVE_CHEEVOS
#include "../cheevos-new/cheevos.h"
#endif

#include "task_content.h"
#include "tasks_internal.h"

#include "../command.h"
#include "../core_info.h"
#include "../content.h"
#include "../configuration.h"
#include "../defaults.h"
#include "../frontend/frontend.h"
#include "../playlist.h"
#include "../paths.h"
#include "../retroarch.h"
#include "../verbosity.h"

#include "../msg_hash.h"
#include "../content.h"
#include "../dynamic.h"
#include "../retroarch.h"
#include "../file_path_special.h"
#include "../core.h"
#include "../paths.h"
#include "../verbosity.h"

#include "../discord/discord.h"

#include "task_patch.c"

extern bool discord_is_inited;

#define MAX_ARGS 32

typedef struct content_stream content_stream_t;
typedef struct content_information_ctx content_information_ctx_t;

struct content_stream
{
   uint32_t a;
   const uint8_t *b;
   size_t c;
   uint32_t crc;
};

struct content_information_ctx
{
   struct
   {
      struct retro_subsystem_info *data;
      unsigned size;
   } subsystem;

   char *name_ips;
   char *name_bps;
   char *name_ups;

   char *valid_extensions;
   char *directory_cache;
   char *directory_system;

   bool is_ips_pref;
   bool is_bps_pref;
   bool is_ups_pref;
   bool history_list_enable;
   bool block_extract;
   bool need_fullpath;
   bool set_supports_no_game_enable;
   bool patch_is_blocked;
   bool bios_is_missing;
   bool check_firmware_before_loading;

   struct string_list *temporary_content;
};

static struct string_list *temporary_content                  = NULL;
static bool _launched_from_cli                                = true;
static bool _content_is_inited                                = false;
static bool core_does_not_need_content                        = false;
static uint32_t content_rom_crc                               = 0;

static bool pending_subsystem_init                            = false;
static int  pending_subsystem_rom_num                         = 0;
static int  pending_subsystem_id                              = 0;
static unsigned  pending_subsystem_rom_id                     = 0;

static bool pending_content_rom_crc                           = false;
static char pending_content_rom_crc_path[PATH_MAX_LENGTH]     = {0};

static char pending_subsystem_ident[255];
#if 0
static char pending_subsystem_extensions[PATH_MAX_LENGTH];
#endif
static char *pending_subsystem_roms[RARCH_MAX_SUBSYSTEM_ROMS];

static int64_t content_file_read(const char *path, void **buf, int64_t *length)
{
#ifdef HAVE_COMPRESSION
   if (path_contains_compressed_file(path))
   {
      if (file_archive_compressed_read(path, buf, NULL, length))
         return 1;
   }
#endif
   return filestream_read_file(path, buf, length);
}

/**
 * content_load_init_wrap:
 * @args                 : Input arguments.
 * @argc                 : Count of arguments.
 * @argv                 : Arguments.
 *
 * Generates an @argc and @argv pair based on @args
 * of type rarch_main_wrap.
 **/
static void content_load_init_wrap(
      const struct rarch_main_wrap *args,
      int *argc, char **argv)
{
   int i;

   *argc = 0;
   argv[(*argc)++] = strdup("retroarch");

   if (args->content_path)
   {
      RARCH_LOG("Using content: %s.\n", args->content_path);
      argv[(*argc)++] = strdup(args->content_path);
   }
#ifdef HAVE_MENU
   else
   {
      RARCH_LOG("%s\n",
            msg_hash_to_str(MSG_NO_CONTENT_STARTING_DUMMY_CORE));
      argv[(*argc)++] = strdup("--menu");
   }
#endif

   if (args->sram_path)
   {
      argv[(*argc)++] = strdup("-s");
      argv[(*argc)++] = strdup(args->sram_path);
   }

   if (args->state_path)
   {
      argv[(*argc)++] = strdup("-S");
      argv[(*argc)++] = strdup(args->state_path);
   }

   if (args->config_path)
   {
      argv[(*argc)++] = strdup("-c");
      argv[(*argc)++] = strdup(args->config_path);
   }

#ifdef HAVE_DYNAMIC
   if (args->libretro_path)
   {
      argv[(*argc)++] = strdup("-L");
      argv[(*argc)++] = strdup(args->libretro_path);
   }
#endif

   if (args->verbose)
      argv[(*argc)++] = strdup("-v");

   for (i = 0; i < *argc; i++)
      RARCH_LOG("arg #%d: %s\n", i, argv[i]);
}

/**
 * content_load:
 *
 * Loads content file and starts up RetroArch.
 * If no content file can be loaded, will start up RetroArch
 * as-is.
 *
 * Returns: false (0) if retroarch_main_init failed,
 * otherwise true (1).
 **/
static bool content_load(content_ctx_info_t *info)
{
   unsigned i;
   int rarch_argc                    = 0;
   char *rarch_argv[MAX_ARGS]        = {NULL};
   char *argv_copy [MAX_ARGS]        = {NULL};
   char **rarch_argv_ptr             = (char**)info->argv;
   int *rarch_argc_ptr               = (int*)&info->argc;
   struct rarch_main_wrap *wrap_args = (struct rarch_main_wrap*)
      calloc(1, sizeof(*wrap_args));

   if (!wrap_args)
      return false;

   retro_assert(wrap_args);

   if (info->environ_get)
      info->environ_get(rarch_argc_ptr,
            rarch_argv_ptr, info->args, wrap_args);

   if (wrap_args->touched)
   {
      content_load_init_wrap(wrap_args, &rarch_argc, rarch_argv);
      memcpy(argv_copy, rarch_argv, sizeof(rarch_argv));
      rarch_argv_ptr = (char**)rarch_argv;
      rarch_argc_ptr = (int*)&rarch_argc;
   }

   rarch_ctl(RARCH_CTL_MAIN_DEINIT, NULL);

   wrap_args->argc = *rarch_argc_ptr;
   wrap_args->argv = rarch_argv_ptr;

   if (!retroarch_main_init(wrap_args->argc, wrap_args->argv))
   {
      for (i = 0; i < ARRAY_SIZE(argv_copy); i++)
         free(argv_copy[i]);
      free(wrap_args);
      return false;
   }

   if (pending_subsystem_init)
   {
      command_event(CMD_EVENT_CORE_INIT, NULL);
      content_clear_subsystem();
   }

   menu_shader_manager_init();

   command_event(CMD_EVENT_HISTORY_INIT, NULL);
   command_event(CMD_EVENT_RESUME, NULL);
   command_event(CMD_EVENT_VIDEO_SET_ASPECT_RATIO, NULL);

   dir_check_defaults();

   frontend_driver_process_args(rarch_argc_ptr, rarch_argv_ptr);
   frontend_driver_content_loaded();

   for (i = 0; i < ARRAY_SIZE(argv_copy); i++)
      free(argv_copy[i]);
   free(wrap_args);
   return true;
}

/**
 * load_content_into_memory:
 * @path         : buffer of the content file.
 * @buf          : size   of the content file.
 * @length       : size of the content file that has been read from.
 *
 * Read the content file. If read into memory, also performs soft patching
 * (see patch_content function) in case soft patching has not been
 * blocked by the enduser.
 *
 * Returns: true if successful, false on error.
 **/
static bool load_content_into_memory(
      content_information_ctx_t *content_ctx,
      unsigned i, const char *path, void **buf,
      int64_t *length)
{
   uint8_t *ret_buf          = NULL;

   RARCH_LOG("%s: %s.\n",
         msg_hash_to_str(MSG_LOADING_CONTENT_FILE), path);

   if (!content_file_read(path, (void**) &ret_buf, length))
      return false;

   if (*length < 0)
      return false;

   if (i == 0)
   {
      enum rarch_content_type type = path_is_media_type(path);

      /* If we have a media type, ignore CRC32 calculation. */
      if (type == RARCH_CONTENT_NONE)
      {
         bool has_patch = false;

         /* First content file is significant, attempt to do patching,
          * CRC checking, etc. */

         /* Attempt to apply a patch. */
         if (!content_ctx->patch_is_blocked)
            has_patch = patch_content(
                  content_ctx->is_ips_pref,
                  content_ctx->is_bps_pref,
                  content_ctx->is_ups_pref,
                  content_ctx->name_ips,
                  content_ctx->name_bps,
                  content_ctx->name_ups,
                  (uint8_t**)&ret_buf,
                  (void*)length);

         if (has_patch)
         {
            content_rom_crc = encoding_crc32(0, ret_buf, (size_t)*length);
            RARCH_LOG("CRC32: 0x%x .\n", (unsigned)content_rom_crc);
         }
         else
         {
            pending_content_rom_crc      = true;
            strlcpy(pending_content_rom_crc_path,
                  path, sizeof(pending_content_rom_crc_path));
         }
      }
      else
         content_rom_crc = 0;
   }

   *buf = ret_buf;

   return true;
}

#ifdef HAVE_COMPRESSION
static bool load_content_from_compressed_archive(
      content_information_ctx_t *content_ctx,
      struct retro_game_info *info,
      unsigned i,
      struct string_list* additional_path_allocs,
      bool need_fullpath,
      const char *path,
      char **error_string)
{
   union string_list_elem_attr attributes;
   int64_t new_path_len              = 0;
   size_t new_basedir_size           = PATH_MAX_LENGTH * sizeof(char);
   size_t new_path_size              = PATH_MAX_LENGTH * sizeof(char);
   char *new_basedir                 = (char*)malloc(new_basedir_size);
   char *new_path                    = (char*)malloc(new_path_size);

   new_path[0]                       = '\0';
   new_basedir[0]                    = '\0';
   attributes.i                      = 0;

   RARCH_LOG("Compressed file in case of need_fullpath."
         " Now extracting to temporary directory.\n");

   if (!string_is_empty(content_ctx->directory_cache))
      strlcpy(new_basedir, content_ctx->directory_cache, new_basedir_size);

   if (!path_is_directory(new_basedir))
   {
      RARCH_WARN("Tried extracting to cache directory, but "
            "cache directory was not set or found. "
            "Setting cache directory to directory "
            "derived by basename...\n");
      fill_pathname_basedir(new_basedir, path, new_basedir_size);
   }

   new_path[0]    = '\0';
   new_basedir[0] = '\0';

   fill_pathname_join(new_path, new_basedir,
         path_basename(path), new_path_size);
   free(new_basedir);

   if (!file_archive_compressed_read(path,
         NULL, new_path, &new_path_len) || new_path_len < 0)
   {
      size_t path_size = 1024 * sizeof(char);
      char *str        = (char*)malloc(path_size);
      snprintf(str,
            path_size,
            "%s \"%s\".\n",
            msg_hash_to_str(MSG_COULD_NOT_READ_CONTENT_FILE),
            path);
      *error_string = strdup(str);
      free(str);
      free(new_path);
      return false;
   }

   string_list_append(additional_path_allocs, new_path, attributes);
   info[i].path =
      additional_path_allocs->elems[additional_path_allocs->size - 1].data;

   free(new_path);

   if (!string_list_append(content_ctx->temporary_content,
            new_path, attributes))
      return false;

   return true;
}

/* Try to extract all content we're going to load if appropriate. */

static bool content_file_init_extract(
      struct string_list *content,
      content_information_ctx_t *content_ctx,
      const struct retro_subsystem_info *special,
      char **error_string,
      union string_list_elem_attr *attr
      )
{
   unsigned i;
   char *new_path = NULL;

   for (i = 0; i < content->size; i++)
   {
      bool block_extract                 = content->elems[i].attr.i & 1;
      const char *path                   = content->elems[i].data;
      bool contains_compressed           = path_contains_compressed_file(path);

      /* Block extract check. */
      if (block_extract)
         continue;

      /* just use the first file in the archive */
      if (!contains_compressed && !path_is_compressed_file(path))
         continue;

      {
         size_t temp_content_size = PATH_MAX_LENGTH * sizeof(char);
         size_t new_path_size     = PATH_MAX_LENGTH * sizeof(char);
         char *temp_content       = (char*)malloc(temp_content_size);
         const char *valid_ext    = special ?
            special->roms[i].valid_extensions :
            content_ctx->valid_extensions;

         new_path        = (char*)malloc(new_path_size);

         temp_content[0] = new_path[0] = '\0';

         if (!string_is_empty(path))
            strlcpy(temp_content, path, temp_content_size);

         if (!valid_ext || !file_archive_extract_file(
                  temp_content,
                  temp_content_size,
                  valid_ext,
                  !string_is_empty(content_ctx->directory_cache) ?
                  content_ctx->directory_cache : NULL,
                  new_path,
                  new_path_size
                  ))
         {
            size_t path_size = 1024 * sizeof(char);
            char *str        = (char*)malloc(path_size);

            snprintf(str, path_size,
                  "%s: %s.\n",
                  msg_hash_to_str(
                     MSG_FAILED_TO_EXTRACT_CONTENT_FROM_COMPRESSED_FILE),
                  temp_content);
            *error_string = strdup(str);
            free(temp_content);
            free(str);
            free(new_path);
            return false;
         }

         string_list_set(content, i, new_path);

         free(temp_content);

         {
            bool append_success = string_list_append(content_ctx->temporary_content,
                     new_path, *attr);

            free(new_path);

            if (!append_success)
               return false;
         }
      }
   }

   return true;
}
#endif

/**
 * content_file_load:
 * @special          : subsystem of content to be loaded. Can be NULL.
 * content           :
 *
 * Load content file (for libretro core).
 *
 * Returns : true if successful, otherwise false.
 **/
static bool content_file_load(
      struct retro_game_info *info,
      const struct string_list *content,
      content_information_ctx_t *content_ctx,
      char **error_string,
      const struct retro_subsystem_info *special,
      struct string_list *additional_path_allocs
      )
{
   unsigned i;
   retro_ctx_load_content_info_t load_info;
   bool used_vfs_fallback_copy = false;
#ifdef __WINRT__
   rarch_system_info_t *system = runloop_get_system_info();
#endif

   for (i = 0; i < content->size; i++)
   {
      int         attr     = content->elems[i].attr.i;
      const char *path     = content->elems[i].data;
      bool need_fullpath   = attr & 2;
      bool require_content = attr & 4;
      bool path_empty      = string_is_empty(path);

      if (require_content && path_empty)
      {
         *error_string = strdup(msg_hash_to_str(MSG_ERROR_LIBRETRO_CORE_REQUIRES_CONTENT));
         return false;
      }

      info[i].path = NULL;

      if (!path_empty)
         info[i].path = path;

      if (!need_fullpath && !path_empty)
      {
         /* Load the content into memory. */

         int64_t len = 0;

         if (!load_content_into_memory(
                  content_ctx,
                  i, path, (void**)&info[i].data, &len))
         {
            size_t msg_size = 1024 * sizeof(char);
            char *msg       = (char*)malloc(msg_size);
            msg[0]          = '\0';

            snprintf(msg,
                  msg_size,
                  "%s \"%s\".\n",
                  msg_hash_to_str(MSG_COULD_NOT_READ_CONTENT_FILE),
                  path);
            *error_string = strdup(msg);
            free(msg);
            return false;
         }

         info[i].size = len;
      }
      else
      {
#ifdef HAVE_COMPRESSION
         if (     !content_ctx->block_extract
               && need_fullpath
               && path_contains_compressed_file(path)
               && !load_content_from_compressed_archive(
                  content_ctx,
                  &info[i], i,
                  additional_path_allocs, need_fullpath, path,
                  error_string))
            return false;
#endif

#ifdef __WINRT__
         /* TODO: When support for the 'actual' VFS is added, there will need to be some more logic here */
         if (!system->supports_vfs && !is_path_accessible_using_standard_io(path))
         {
            /* Fallback to a file copy into an accessible directory */
            char* buf;
            int64_t len;
            union string_list_elem_attr attributes;
            size_t new_basedir_size = PATH_MAX_LENGTH * sizeof(char);
            size_t new_path_size    = PATH_MAX_LENGTH * sizeof(char);
            char *new_basedir       = (char*)malloc(new_basedir_size);
            char *new_path          = (char*)malloc(new_path_size);

            new_path[0]             = '\0';
            new_basedir[0]          = '\0';
            attributes.i            = 0;

            RARCH_LOG("Core does not support VFS - copying to cache directory\n");

            if (!string_is_empty(content_ctx->directory_cache))
               strlcpy(new_basedir, content_ctx->directory_cache, new_basedir_size);
            if (string_is_empty(new_basedir) || !path_is_directory(new_basedir) || !is_path_accessible_using_standard_io(new_basedir))
            {
               RARCH_WARN("Tried copying to cache directory, but "
                     "cache directory was not set or found. "
                     "Setting cache directory to root of "
                     "writable app directory...\n");
               strlcpy(new_basedir, uwp_dir_data, new_basedir_size);
            }

            fill_pathname_join(new_path, new_basedir,
                  path_basename(path), new_path_size);
            free(new_basedir);

            /* TODO: This may fail on very large files...
             * but copying large files is not a good idea anyway */
            if (!filestream_read_file(path, &buf, &len))
            {
               size_t msg_size = 1024 * sizeof(char);
               char *msg       = (char*)malloc(msg_size);
               msg[0]          = '\0';

               snprintf(msg,
                     msg_size,
                     "%s \"%s\". (during copy read)\n",
                     msg_hash_to_str(MSG_COULD_NOT_READ_CONTENT_FILE),
                     path);
               *error_string = strdup(msg);
               free(msg);
               return false;
            }
            if (!filestream_write_file(new_path, buf, len))
            {
               size_t msg_size = 1024 * sizeof(char);
               char *msg       = (char*)malloc(msg_size);
               msg[0]          = '\0';

               free(buf);
               snprintf(msg,
                     msg_size,
                     "%s \"%s\". (during copy write)\n",
                     msg_hash_to_str(MSG_COULD_NOT_READ_CONTENT_FILE),
                     path);
               *error_string = strdup(msg);
               free(msg);
               return false;
            }
            free(buf);

            string_list_append(additional_path_allocs, new_path, attributes);
            info[i].path =
               additional_path_allocs->elems[additional_path_allocs->size - 1].data;

            string_list_append(content_ctx->temporary_content,
                  new_path, attributes);

            free(new_path);

            used_vfs_fallback_copy = true;
         }
#endif

         RARCH_LOG("%s\n", msg_hash_to_str(
                  MSG_CONTENT_LOADING_SKIPPED_IMPLEMENTATION_WILL_DO_IT));
         pending_content_rom_crc      = true;
         strlcpy(pending_content_rom_crc_path,
               path, sizeof(pending_content_rom_crc_path));

      }
   }

   load_info.content = content;
   load_info.special = special;
   load_info.info    = info;

   if (!core_load_game(&load_info))
   {
      /* This is probably going to fail on multifile ROMs etc.
       * so give a visible explanation of what is likely wrong */
      if (used_vfs_fallback_copy)
         *error_string = strdup(msg_hash_to_str(
                  MSG_ERROR_LIBRETRO_CORE_REQUIRES_VFS));
      else
         *error_string = strdup(msg_hash_to_str(
                  MSG_FAILED_TO_LOAD_CONTENT));

      return false;
   }

#ifdef HAVE_CHEEVOS
   if (!special)
   {
      const char *content_path     = content->elems[0].data;
      enum rarch_content_type type = path_is_media_type(content_path);

      rcheevos_set_cheats();

      if (type == RARCH_CONTENT_NONE && !string_is_empty(content_path))
         rcheevos_load(info);
   }
#endif

   return true;
}

static const struct
retro_subsystem_info *content_file_init_subsystem(
      const struct retro_subsystem_info *subsystem_data,
      size_t subsystem_current_count,
      char **error_string,
      bool *ret)
{
   size_t path_size                           = 1024 * sizeof(char);
   char *msg                                  = (char*)malloc(path_size);
   struct string_list *subsystem              = path_get_subsystem_list();
   const struct retro_subsystem_info *special = libretro_find_subsystem_info(
            subsystem_data, (unsigned)subsystem_current_count,
            path_get(RARCH_PATH_SUBSYSTEM));

   msg[0] = '\0';

   if (!special)
   {
      snprintf(msg, path_size,
            "Failed to find subsystem \"%s\" in libretro implementation.\n",
            path_get(RARCH_PATH_SUBSYSTEM));
      *error_string = strdup(msg);
      goto error;
   }

   if (special->num_roms && !subsystem)
   {
      strlcpy(msg,
            msg_hash_to_str(MSG_ERROR_LIBRETRO_CORE_REQUIRES_SPECIAL_CONTENT),
            path_size
            );
      *error_string = strdup(msg);
      goto error;
   }
   else if (special->num_roms && (special->num_roms != subsystem->size))
   {
      snprintf(msg,
            path_size,
            "Libretro core requires %u content files for "
            "subsystem \"%s\", but %u content files were provided.\n",
            special->num_roms, special->desc,
            (unsigned)subsystem->size);
      *error_string = strdup(msg);
      goto error;
   }
   else if (!special->num_roms && subsystem && subsystem->size)
   {
      snprintf(msg,
            path_size,
            "Libretro core takes no content for subsystem \"%s\", "
            "but %u content files were provided.\n",
            special->desc,
            (unsigned)subsystem->size);
      *error_string = strdup(msg);
      goto error;
   }

   *ret = true;
   free(msg);
   return special;

error:
   *ret = false;
   free(msg);
   return NULL;
}

static void content_file_init_set_attribs(
      struct string_list *content,
      const struct retro_subsystem_info *special,
      content_information_ctx_t *content_ctx,
      char **error_string,
      union string_list_elem_attr *attr)
{
   struct string_list *subsystem    = path_get_subsystem_list();

   attr->i                          = 0;

   if (!path_is_empty(RARCH_PATH_SUBSYSTEM) && special)
   {
      unsigned i;

      for (i = 0; i < subsystem->size; i++)
      {
         attr->i            = special->roms[i].block_extract;
         attr->i           |= special->roms[i].need_fullpath << 1;
         attr->i           |= special->roms[i].required      << 2;

         string_list_append(content, subsystem->elems[i].data, *attr);
      }
   }
   else
   {
      bool contentless           = false;
      bool is_inited             = false;
      bool content_path_is_empty = path_is_empty(RARCH_PATH_CONTENT);

      content_get_status(&contentless, &is_inited);

      attr->i               = content_ctx->block_extract;
      attr->i              |= content_ctx->need_fullpath << 1;
      attr->i              |= (!contentless)  << 2;

      if (content_path_is_empty
            && contentless
            && content_ctx->set_supports_no_game_enable)
         string_list_append(content, "", *attr);
      else if (!content_path_is_empty)
         string_list_append(content, path_get(RARCH_PATH_CONTENT), *attr);
   }
}

/**
 * content_init_file:
 *
 * Initializes and loads a content file for the currently
 * selected libretro core.
 *
 * Returns : true if successful, otherwise false.
 **/
static bool content_file_init(
      content_information_ctx_t *content_ctx,
      struct string_list *content,
      char **error_string)
{
   union string_list_elem_attr attr;
   struct retro_game_info               *info = NULL;
   bool subsystem_path_is_empty               = path_is_empty(RARCH_PATH_SUBSYSTEM);
   bool ret                                   = subsystem_path_is_empty;
   const struct retro_subsystem_info *special =
     subsystem_path_is_empty 
     ? NULL : content_file_init_subsystem(content_ctx->subsystem.data,
           content_ctx->subsystem.size, error_string, &ret);

   if (!ret)
      return false;

   content_file_init_set_attribs(content, special, content_ctx, error_string, &attr);
#ifdef HAVE_COMPRESSION
   content_file_init_extract(content, content_ctx, special, error_string, &attr);
#endif

   if (content->size > 0)
      info                   = (struct retro_game_info*)
         calloc(content->size, sizeof(*info));

   if (info)
   {
      unsigned i;
      struct string_list *additional_path_allocs = string_list_new();

      ret = content_file_load(info, content, content_ctx, error_string,
            special, additional_path_allocs);
      string_list_free(additional_path_allocs);

      for (i = 0; i < content->size; i++)
         free((void*)info[i].data);

      free(info);
   }
   else if (!special)
   {
      *error_string = strdup(msg_hash_to_str(MSG_ERROR_LIBRETRO_CORE_REQUIRES_CONTENT));
      ret = false;
   }

   return ret;
}

static void menu_content_environment_get(int *argc, char *argv[],
      void *args, void *params_data)
{
   struct rarch_main_wrap *wrap_args = (struct rarch_main_wrap*)params_data;
   rarch_system_info_t *sys_info     = runloop_get_system_info();

   if (!wrap_args)
      return;

   wrap_args->no_content       = sys_info->load_no_content;

   if (!retroarch_override_setting_is_set(RARCH_OVERRIDE_SETTING_VERBOSITY, NULL))
      wrap_args->verbose       = verbosity_is_enabled();

   wrap_args->touched          = true;
   wrap_args->config_path      = NULL;
   wrap_args->sram_path        = NULL;
   wrap_args->state_path       = NULL;
   wrap_args->content_path     = NULL;

   if (!path_is_empty(RARCH_PATH_CONFIG))
      wrap_args->config_path   = path_get(RARCH_PATH_CONFIG);
   if (!dir_is_empty(RARCH_DIR_SAVEFILE))
      wrap_args->sram_path     = dir_get(RARCH_DIR_SAVEFILE);
   if (!dir_is_empty(RARCH_DIR_SAVESTATE))
      wrap_args->state_path    = dir_get(RARCH_DIR_SAVESTATE);
   if (!path_is_empty(RARCH_PATH_CONTENT))
      wrap_args->content_path  = path_get(RARCH_PATH_CONTENT);
   if (!retroarch_override_setting_is_set(RARCH_OVERRIDE_SETTING_LIBRETRO, NULL))
      wrap_args->libretro_path = string_is_empty(path_get(RARCH_PATH_CORE)) ? NULL :
         path_get(RARCH_PATH_CORE);

}

/**
 * task_load_content:
 *
 * Loads content into currently selected core.
 * Will also optionally push the content entry to the history playlist.
 *
 * Returns: true (1) if successful, otherwise false (0).
 **/
static bool task_load_content(content_ctx_info_t *content_info,
      content_information_ctx_t *content_ctx,
      bool launched_from_menu,
      bool launched_from_cli,
      char **error_string)
{
   bool contentless = false;
   bool is_inited   = false;

   if (!content_load(content_info))
      return false;

   content_get_status(&contentless, &is_inited);

   /* Push entry to top of history playlist */
   if (is_inited || contentless)
   {
      size_t tmp_size                = PATH_MAX_LENGTH * sizeof(char);
      char *tmp                      = (char*)malloc(tmp_size);
      const char *path_content       = path_get(RARCH_PATH_CONTENT);
      struct retro_system_info *info = runloop_get_libretro_system_info();

      tmp[0] = '\0';

      if (!string_is_empty(path_content))
         strlcpy(tmp, path_content, tmp_size);

      /* Path can be relative here.
       * Ensure we're pushing absolute path. */
      if (!launched_from_menu && !string_is_empty(tmp))
         path_resolve_realpath(tmp, tmp_size);

#ifdef HAVE_MENU
      /* Push quick menu onto menu stack */
      if (launched_from_cli)
         menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);
#endif

      if (info && !string_is_empty(tmp))
      {
         const char *core_path      = NULL;
         const char *core_name      = NULL;
         const char *label          = NULL;
         const char *crc32          = NULL;
         const char *db_name        = NULL;
         playlist_t *playlist_hist  = g_defaults.content_history;
         global_t *global           = global_get_ptr();

         switch (path_is_media_type(tmp))
         {
            case RARCH_CONTENT_MOVIE:
#ifdef HAVE_FFMPEG
               playlist_hist        = g_defaults.video_history;
               core_name            = "movieplayer";
               core_path            = "builtin";
#endif
               break;
            case RARCH_CONTENT_MUSIC:
               playlist_hist        = g_defaults.music_history;
               core_name            = "musicplayer";
               core_path            = "builtin";
               break;
            case RARCH_CONTENT_IMAGE:
#ifdef HAVE_IMAGEVIEWER
               playlist_hist        = g_defaults.image_history;
               core_name            = "imageviewer";
               core_path            = "builtin";
#endif
               break;
            default:
            {
#ifdef HAVE_MENU
               menu_handle_t *menu = NULL;
#endif
               core_info_t *core_info = NULL;

               /* Set core path */
               core_path            = path_get(RARCH_PATH_CORE);

               /* Set core display name
                * (As far as I can tell, core_info_get_current_core()
                * should always provide a valid pointer here...) */
               core_info_get_current_core(&core_info);

               if (core_info)
                  core_name         = core_info->display_name;

               if (string_is_empty(core_name))
                  core_name         = info->library_name;

#ifdef HAVE_MENU
               /* Set database name + checksum */
               if (menu_driver_ctl(RARCH_MENU_CTL_DRIVER_DATA_GET, &menu))
               {
                  playlist_t *playlist_curr = playlist_get_cached();

                  if (playlist_index_is_valid(playlist_curr, menu->rpl_entry_selection_ptr, tmp, core_path))
                  {
                     playlist_get_crc32(playlist_curr, menu->rpl_entry_selection_ptr, &crc32);
                     playlist_get_db_name(playlist_curr, menu->rpl_entry_selection_ptr, &db_name);
                  }
               }
#endif
               break;
            }
         }

         if (launched_from_cli)
         {
            settings_t *settings             = config_get_ptr();
            content_ctx->history_list_enable = settings->bools.history_list_enable;
         }

         if (global && !string_is_empty(global->name.label))
            label = global->name.label;

         if (
               content_ctx->history_list_enable
               && playlist_hist)
         {
            char subsystem_name[PATH_MAX_LENGTH];
            struct playlist_entry entry = {0};

            subsystem_name[0] = '\0';

            content_get_subsystem_friendly_name(path_get(RARCH_PATH_SUBSYSTEM), subsystem_name, sizeof(subsystem_name));

            /* the push function reads our entry as const, so these casts are safe */
            entry.path            = (char*)tmp;
            entry.label           = (char*)label;
            entry.core_path       = (char*)core_path;
            entry.core_name       = (char*)core_name;
            entry.crc32           = (char*)crc32;
            entry.db_name         = (char*)db_name;
            entry.subsystem_ident = (char*)path_get(RARCH_PATH_SUBSYSTEM);
            entry.subsystem_name  = (char*)subsystem_name;
            entry.subsystem_roms  = (struct string_list*)path_get_subsystem_list();

            command_playlist_push_write(
                  playlist_hist, &entry);
         }
      }

      free(tmp);
   }

   return true;
}

#ifdef HAVE_MENU
static bool command_event_cmd_exec(const char *data,
      content_information_ctx_t *content_ctx,
      bool launched_from_cli,
      char **error_string)
{
#if defined(HAVE_DYNAMIC)
   content_ctx_info_t content_info;

   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;
   content_info.environ_get = menu_content_environment_get;
#endif

   if (path_get(RARCH_PATH_CONTENT) != data)
   {
      path_clear(RARCH_PATH_CONTENT);
      if (!string_is_empty(data))
         path_set(RARCH_PATH_CONTENT, data);
   }

#if defined(HAVE_DYNAMIC)
   if (!task_load_content(&content_info, content_ctx,
            true, launched_from_cli, error_string))
      return false;
#else
   frontend_driver_set_fork(FRONTEND_FORK_CORE_WITH_ARGS);
#endif

   return true;
}
#endif

static bool firmware_update_status(
      content_information_ctx_t *content_ctx)
{
   core_info_ctx_firmware_t firmware_info;
   bool set_missing_firmware  = false;
   core_info_t *core_info     = NULL;
   size_t s_size              = PATH_MAX_LENGTH * sizeof(char);
   char *s                    = NULL;
   
   core_info_get_current_core(&core_info);

   if (!core_info)
      return false;

   s                          = (char*)malloc(s_size);

   firmware_info.path         = core_info->path;

   if (!string_is_empty(content_ctx->directory_system))
      firmware_info.directory.system = content_ctx->directory_system;
   else
   {
      strlcpy(s, path_get(RARCH_PATH_CONTENT), s_size);
      path_basedir_wrapper(s);
      firmware_info.directory.system = s;
   }

   RARCH_LOG("Updating firmware status for: %s on %s\n",
         core_info->path,
         firmware_info.directory.system);

   rarch_ctl(RARCH_CTL_UNSET_MISSING_BIOS, NULL);

   core_info_list_update_missing_firmware(&firmware_info,
         &set_missing_firmware);

   free(s);

   if (set_missing_firmware)
      rarch_ctl(RARCH_CTL_SET_MISSING_BIOS, NULL);

   if(
         content_ctx->bios_is_missing &&
         content_ctx->check_firmware_before_loading)
   {
      runloop_msg_queue_push(
            msg_hash_to_str(MSG_FIRMWARE),
            100, 500, true, NULL,
            MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      RARCH_LOG("Load content blocked. Reason: %s\n",
            msg_hash_to_str(MSG_FIRMWARE));

      return true;
   }

   return false;
}

bool task_push_start_dummy_core(content_ctx_info_t *content_info)
{
   content_information_ctx_t content_ctx;
   bool ret                                   = true;
   char *error_string                         = NULL;
   global_t *global                           = global_get_ptr();
   settings_t *settings                       = config_get_ptr();
   rarch_system_info_t *sys_info              = runloop_get_system_info();

   if (!content_info)
      return false;

   content_ctx.check_firmware_before_loading  = settings->bools.check_firmware_before_loading;
   content_ctx.is_ips_pref                    = rarch_ctl(RARCH_CTL_IS_IPS_PREF, NULL);
   content_ctx.is_bps_pref                    = rarch_ctl(RARCH_CTL_IS_BPS_PREF, NULL);
   content_ctx.is_ups_pref                    = rarch_ctl(RARCH_CTL_IS_UPS_PREF, NULL);
   content_ctx.patch_is_blocked               = rarch_ctl(RARCH_CTL_IS_PATCH_BLOCKED, NULL);
   content_ctx.bios_is_missing                = rarch_ctl(RARCH_CTL_IS_MISSING_BIOS, NULL);
   content_ctx.directory_system               = NULL;
   content_ctx.directory_cache                = NULL;
   content_ctx.name_ips                       = NULL;
   content_ctx.name_bps                       = NULL;
   content_ctx.name_ups                       = NULL;
   content_ctx.valid_extensions               = NULL;
   content_ctx.block_extract                  = false;
   content_ctx.need_fullpath                  = false;
   content_ctx.set_supports_no_game_enable    = false;

   content_ctx.subsystem.data                 = NULL;
   content_ctx.subsystem.size                 = 0;

   content_ctx.history_list_enable            = settings->bools.history_list_enable;

   if (global)
   {
      if (!string_is_empty(global->name.ips))
         content_ctx.name_ips                 = strdup(global->name.ips);
      if (!string_is_empty(global->name.bps))
         content_ctx.name_bps                 = strdup(global->name.bps);
      if (!string_is_empty(global->name.ups))
         content_ctx.name_ups                 = strdup(global->name.ups);
   }

   if (!string_is_empty(settings->paths.directory_system))
      content_ctx.directory_system            = strdup(settings->paths.directory_system);

   if (!content_info->environ_get)
      content_info->environ_get = menu_content_environment_get;

   /* Clear content path */
   path_clear(RARCH_PATH_CONTENT);

   /* Preliminary stuff that has to be done before we
    * load the actual content. Can differ per mode. */
   sys_info->load_no_content = false;
   rarch_ctl(RARCH_CTL_STATE_FREE, NULL);
   rarch_ctl(RARCH_CTL_DATA_DEINIT, NULL);
   rarch_ctl(RARCH_CTL_TASK_INIT, NULL);

   /* Load content */
   if (!task_load_content(content_info, &content_ctx,
            false, false, &error_string))
   {
      if (error_string)
      {
         runloop_msg_queue_push(error_string, 2, 90, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         RARCH_ERR("%s\n", error_string);
         free(error_string);
      }

      ret =  false;
   }

   if (content_ctx.name_ips)
      free(content_ctx.name_ips);
   if (content_ctx.name_bps)
      free(content_ctx.name_bps);
   if (content_ctx.name_ups)
      free(content_ctx.name_ups);
   if (content_ctx.directory_system)
      free(content_ctx.directory_system);

   return ret;
}

#ifdef HAVE_MENU

bool task_push_load_content_from_playlist_from_menu(
      const char *core_path,
      const char *fullpath,
      const char *label,
      content_ctx_info_t *content_info,
      retro_task_callback_t cb,
      void *user_data)
{
   content_information_ctx_t content_ctx;

   bool ret                                   = true;
   char *error_string                         = NULL;
   global_t *global                           = global_get_ptr();
   settings_t *settings                       = config_get_ptr();
   rarch_system_info_t *sys_info              = runloop_get_system_info();

   content_ctx.check_firmware_before_loading  = settings->bools.check_firmware_before_loading;
   content_ctx.is_ips_pref                    = rarch_ctl(RARCH_CTL_IS_IPS_PREF, NULL);
   content_ctx.is_bps_pref                    = rarch_ctl(RARCH_CTL_IS_BPS_PREF, NULL);
   content_ctx.is_ups_pref                    = rarch_ctl(RARCH_CTL_IS_UPS_PREF, NULL);
   content_ctx.patch_is_blocked               = rarch_ctl(RARCH_CTL_IS_PATCH_BLOCKED, NULL);
   content_ctx.bios_is_missing                = rarch_ctl(RARCH_CTL_IS_MISSING_BIOS, NULL);
   content_ctx.directory_system               = NULL;
   content_ctx.directory_cache                = NULL;
   content_ctx.name_ips                       = NULL;
   content_ctx.name_bps                       = NULL;
   content_ctx.name_ups                       = NULL;
   content_ctx.valid_extensions               = NULL;
   content_ctx.block_extract                  = false;
   content_ctx.need_fullpath                  = false;
   content_ctx.set_supports_no_game_enable    = false;

   content_ctx.subsystem.data                 = NULL;
   content_ctx.subsystem.size                 = 0;

   content_ctx.history_list_enable            = settings->bools.history_list_enable;

   if (global)
   {
      if (!string_is_empty(global->name.ips))
         content_ctx.name_ips                 = strdup(global->name.ips);
      if (!string_is_empty(global->name.bps))
         content_ctx.name_bps                 = strdup(global->name.bps);
      if (!string_is_empty(global->name.ups))
         content_ctx.name_ups                 = strdup(global->name.ups);
      if (label)
         strlcpy(global->name.label, label, sizeof(global->name.label));
      else
         global->name.label[0] = '\0';
   }

   if (!string_is_empty(settings->paths.directory_system))
      content_ctx.directory_system            = strdup(settings->paths.directory_system);

   path_set(RARCH_PATH_CORE, core_path);

   /* Is content required by this core? */
   if (fullpath)
      sys_info->load_no_content = false;
   else
      sys_info->load_no_content = true;

   /* On targets that have no dynamic core loading support, we'd
    * execute the new core from this point. If this returns false,
    * we assume we can dynamically load the core. */
   if (!command_event_cmd_exec(fullpath, &content_ctx, CONTENT_MODE_LOAD_NONE, &error_string))
   {
      if (error_string)
      {
         runloop_msg_queue_push(error_string, 2, 90, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         RARCH_ERR("%s\n", error_string);
         free(error_string);
      }

      rarch_menu_running();

      ret = false;
      goto end;
   }

   /* Load core */
#ifdef HAVE_DYNAMIC
   command_event(CMD_EVENT_LOAD_CORE, NULL);
#else
   rarch_ctl(RARCH_CTL_SET_SHUTDOWN, NULL);
   rarch_menu_running_finished();
#endif

end:
   if (content_ctx.name_ips)
      free(content_ctx.name_ips);
   if (content_ctx.name_bps)
      free(content_ctx.name_bps);
   if (content_ctx.name_ups)
      free(content_ctx.name_ups);
   if (content_ctx.directory_system)
      free(content_ctx.directory_system);

   return ret;
}
#endif

bool task_push_start_current_core(content_ctx_info_t *content_info)
{
   content_information_ctx_t content_ctx;

   bool ret                                   = true;
   char *error_string                         = NULL;
   global_t *global                           = global_get_ptr();
   settings_t *settings                       = config_get_ptr();

   if (!content_info)
      return false;

   content_ctx.check_firmware_before_loading  = settings->bools.check_firmware_before_loading;
   content_ctx.is_ips_pref                    = rarch_ctl(RARCH_CTL_IS_IPS_PREF, NULL);
   content_ctx.is_bps_pref                    = rarch_ctl(RARCH_CTL_IS_BPS_PREF, NULL);
   content_ctx.is_ups_pref                    = rarch_ctl(RARCH_CTL_IS_UPS_PREF, NULL);
   content_ctx.patch_is_blocked               = rarch_ctl(RARCH_CTL_IS_PATCH_BLOCKED, NULL);
   content_ctx.bios_is_missing                = rarch_ctl(RARCH_CTL_IS_MISSING_BIOS, NULL);
   content_ctx.directory_system               = NULL;
   content_ctx.directory_cache                = NULL;
   content_ctx.name_ips                       = NULL;
   content_ctx.name_bps                       = NULL;
   content_ctx.name_ups                       = NULL;
   content_ctx.valid_extensions               = NULL;
   content_ctx.block_extract                  = false;
   content_ctx.need_fullpath                  = false;
   content_ctx.set_supports_no_game_enable    = false;

   content_ctx.subsystem.data                 = NULL;
   content_ctx.subsystem.size                 = 0;

   content_ctx.history_list_enable            = settings->bools.history_list_enable;

   if (global)
   {
      if (!string_is_empty(global->name.ips))
         content_ctx.name_ips                 = strdup(global->name.ips);
      if (!string_is_empty(global->name.bps))
         content_ctx.name_bps                 = strdup(global->name.bps);
      if (!string_is_empty(global->name.ups))
         content_ctx.name_ups                 = strdup(global->name.ups);
   }

   if (!string_is_empty(settings->paths.directory_system))
      content_ctx.directory_system            = strdup(settings->paths.directory_system);

   if (!content_info->environ_get)
      content_info->environ_get = menu_content_environment_get;

   /* Clear content path */
   path_clear(RARCH_PATH_CONTENT);

   /* Preliminary stuff that has to be done before we
    * load the actual content. Can differ per mode. */
   retroarch_set_current_core_type(CORE_TYPE_PLAIN, true);

   /* Load content */
   if (firmware_update_status(&content_ctx))
      goto end;

   if (!task_load_content(content_info, &content_ctx,
            true, false, &error_string))
   {
      if (error_string)
      {
         runloop_msg_queue_push(error_string, 2, 90, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         RARCH_ERR("%s\n", error_string);
         free(error_string);
      }

      rarch_menu_running();

      ret = false;
      goto end;
   }

#ifdef HAVE_MENU
   /* Push quick menu onto menu stack */
   menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);
#endif

end:
   if (content_ctx.name_ips)
      free(content_ctx.name_ips);
   if (content_ctx.name_bps)
      free(content_ctx.name_bps);
   if (content_ctx.name_ups)
      free(content_ctx.name_ups);
   if (content_ctx.directory_system)
      free(content_ctx.directory_system);

   return ret;
}

bool task_push_load_new_core(
      const char *core_path,
      const char *fullpath,
      content_ctx_info_t *content_info,
      enum rarch_core_type type,
      retro_task_callback_t cb,
      void *user_data)
{
   path_set(RARCH_PATH_CORE, core_path);

   /* Load core */
   command_event(CMD_EVENT_LOAD_CORE, NULL);

#ifndef HAVE_DYNAMIC
   /* Fork core? */
   if (!frontend_driver_set_fork(FRONTEND_FORK_CORE))
      return false;
#endif

   /* Preliminary stuff that has to be done before we
    * load the actual content. Can differ per mode. */
   retroarch_set_current_core_type(type, true);

   return true;
}

#ifdef HAVE_MENU
bool task_push_load_content_with_new_core_from_menu(
      const char *core_path,
      const char *fullpath,
      content_ctx_info_t *content_info,
      enum rarch_core_type type,
      retro_task_callback_t cb,
      void *user_data)
{
   content_information_ctx_t content_ctx;

   bool ret                                   = true;
   char *error_string                         = NULL;
   global_t *global                           = global_get_ptr();
   settings_t *settings                       = config_get_ptr();

   content_ctx.check_firmware_before_loading  = settings->bools.check_firmware_before_loading;
   content_ctx.is_ips_pref                    = rarch_ctl(RARCH_CTL_IS_IPS_PREF, NULL);
   content_ctx.is_bps_pref                    = rarch_ctl(RARCH_CTL_IS_BPS_PREF, NULL);
   content_ctx.is_ups_pref                    = rarch_ctl(RARCH_CTL_IS_UPS_PREF, NULL);
   content_ctx.patch_is_blocked               = rarch_ctl(RARCH_CTL_IS_PATCH_BLOCKED, NULL);
   content_ctx.bios_is_missing                = rarch_ctl(RARCH_CTL_IS_MISSING_BIOS, NULL);
   content_ctx.directory_system               = NULL;
   content_ctx.directory_cache                = NULL;
   content_ctx.name_ips                       = NULL;
   content_ctx.name_bps                       = NULL;
   content_ctx.name_ups                       = NULL;
   content_ctx.valid_extensions               = NULL;
   content_ctx.block_extract                  = false;
   content_ctx.need_fullpath                  = false;
   content_ctx.set_supports_no_game_enable    = false;

   content_ctx.subsystem.data                 = NULL;
   content_ctx.subsystem.size                 = 0;

   content_ctx.history_list_enable            = settings->bools.history_list_enable;

   if (global)
   {
      if (!string_is_empty(global->name.ips))
         content_ctx.name_ips                 = strdup(global->name.ips);
      if (!string_is_empty(global->name.bps))
         content_ctx.name_bps                 = strdup(global->name.bps);
      if (!string_is_empty(global->name.ups))
         content_ctx.name_ups                 = strdup(global->name.ups);

      global->name.label[0]                   = '\0';
   }

   if (!string_is_empty(settings->paths.directory_system))
      content_ctx.directory_system            = strdup(settings->paths.directory_system);

   path_set(RARCH_PATH_CONTENT, fullpath);
   path_set(RARCH_PATH_CORE, core_path);

#ifdef HAVE_DYNAMIC
   /* Load core */
   command_event(CMD_EVENT_LOAD_CORE, NULL);

   /* Load content */
   if (!content_info->environ_get)
      content_info->environ_get = menu_content_environment_get;

   if (firmware_update_status(&content_ctx))
      goto end;

   if (!task_load_content(content_info, &content_ctx,
            true, false, &error_string))
   {
      if (error_string)
      {
         runloop_msg_queue_push(error_string, 2, 90, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         RARCH_ERR("%s\n", error_string);
         free(error_string);
      }

      rarch_menu_running();

      ret = false;
      goto end;
   }

#else
   command_event_cmd_exec(path_get(RARCH_PATH_CONTENT), &content_ctx,
         false, &error_string);
   command_event(CMD_EVENT_QUIT, NULL);
#endif

   /* Push quick menu onto menu stack */
   if (type != CORE_TYPE_DUMMY)
      menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);

#ifdef HAVE_DYNAMIC
end:
#endif
   if (content_ctx.name_ips)
      free(content_ctx.name_ips);
   if (content_ctx.name_bps)
      free(content_ctx.name_bps);
   if (content_ctx.name_ups)
      free(content_ctx.name_ups);
   if (content_ctx.directory_system)
      free(content_ctx.directory_system);

   return ret;
}
#endif

static bool task_load_content_callback(content_ctx_info_t *content_info,
      bool loading_from_menu, bool loading_from_cli)
{
   content_information_ctx_t content_ctx;

   bool ret                                   = false;
   char *error_string                         = NULL;
   global_t *global                           = global_get_ptr();
   settings_t *settings                       = config_get_ptr();
   rarch_system_info_t *sys_info              = runloop_get_system_info();

   content_ctx.check_firmware_before_loading  = settings->bools.check_firmware_before_loading;
   content_ctx.is_ips_pref                    = rarch_ctl(RARCH_CTL_IS_IPS_PREF, NULL);
   content_ctx.is_bps_pref                    = rarch_ctl(RARCH_CTL_IS_BPS_PREF, NULL);
   content_ctx.is_ups_pref                    = rarch_ctl(RARCH_CTL_IS_UPS_PREF, NULL);
   content_ctx.patch_is_blocked               = rarch_ctl(RARCH_CTL_IS_PATCH_BLOCKED, NULL);
   content_ctx.bios_is_missing                = rarch_ctl(RARCH_CTL_IS_MISSING_BIOS, NULL);
   content_ctx.directory_system               = NULL;
   content_ctx.directory_cache                = NULL;
   content_ctx.name_ips                       = NULL;
   content_ctx.name_bps                       = NULL;
   content_ctx.name_ups                       = NULL;
   content_ctx.valid_extensions               = NULL;
   content_ctx.block_extract                  = false;
   content_ctx.need_fullpath                  = false;
   content_ctx.set_supports_no_game_enable    = false;

   content_ctx.subsystem.data                 = NULL;
   content_ctx.subsystem.size                 = 0;

   if (sys_info)
   {
      struct retro_system_info *system        = runloop_get_libretro_system_info();

      content_ctx.history_list_enable         = settings->bools.history_list_enable;
      content_ctx.set_supports_no_game_enable = settings->bools.set_supports_no_game_enable;

      if (!string_is_empty(settings->paths.directory_system))
         content_ctx.directory_system         = strdup(settings->paths.directory_system);
      if (!string_is_empty(settings->paths.directory_cache))
         content_ctx.directory_cache          = strdup(settings->paths.directory_cache);
      if (!string_is_empty(system->valid_extensions))
         content_ctx.valid_extensions         = strdup(system->valid_extensions);

      content_ctx.block_extract               = system->block_extract;
      content_ctx.need_fullpath               = system->need_fullpath;

      content_ctx.subsystem.data              = sys_info->subsystem.data;
      content_ctx.subsystem.size              = sys_info->subsystem.size;
   }

   content_ctx.history_list_enable            = settings->bools.history_list_enable;

   if (global)
   {
      if (!string_is_empty(global->name.ips))
         content_ctx.name_ips                 = strdup(global->name.ips);
      if (!string_is_empty(global->name.bps))
         content_ctx.name_bps                 = strdup(global->name.bps);
      if (!string_is_empty(global->name.ups))
         content_ctx.name_ups                 = strdup(global->name.ups);
   }

   if (!string_is_empty(settings->paths.directory_system))
      content_ctx.directory_system            = strdup(settings->paths.directory_system);

   if (!content_info->environ_get)
      content_info->environ_get = menu_content_environment_get;

   if (firmware_update_status(&content_ctx))
      goto end;

#ifdef HAVE_DISCORD
   if (discord_is_inited)
   {
      discord_userdata_t userdata;
      userdata.status = DISCORD_PRESENCE_NETPLAY_NETPLAY_STOPPED;
      command_event(CMD_EVENT_DISCORD_UPDATE, &userdata);
      userdata.status = DISCORD_PRESENCE_MENU;
      command_event(CMD_EVENT_DISCORD_UPDATE, &userdata);
   }
#endif

   ret = task_load_content(content_info, &content_ctx, true, loading_from_cli, &error_string);

end:
   if (content_ctx.name_ips)
      free(content_ctx.name_ips);
   if (content_ctx.name_bps)
      free(content_ctx.name_bps);
   if (content_ctx.name_ups)
      free(content_ctx.name_ups);
   if (content_ctx.directory_system)
      free(content_ctx.directory_system);
   if (content_ctx.directory_cache)
      free(content_ctx.directory_cache);
   if (content_ctx.valid_extensions)
      free(content_ctx.valid_extensions);

   if (!ret)
   {
      if (error_string)
      {
         runloop_msg_queue_push(error_string, 2, 90, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
         RARCH_ERR("%s\n", error_string);
         free(error_string);
      }

      return false;
   }

   return true;
}

bool task_push_load_content_with_new_core_from_companion_ui(
      const char *core_path,
      const char *fullpath,
      const char *label,
      content_ctx_info_t *content_info,
      retro_task_callback_t cb,
      void *user_data)
{
   global_t *global = global_get_ptr();

   path_set(RARCH_PATH_CONTENT, fullpath);
   path_set(RARCH_PATH_CORE, core_path);
#ifdef HAVE_DYNAMIC
   command_event(CMD_EVENT_LOAD_CORE, NULL);
#endif

   _launched_from_cli = false;

   if (global)
   {
      if (label)
         strlcpy(global->name.label, label, sizeof(global->name.label));
      else
         global->name.label[0] = '\0';
   }

   /* Load content */
   if (!task_load_content_callback(content_info, true, false))
      return false;

#ifdef HAVE_MENU
   /* Push quick menu onto menu stack */
   menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);
#endif

   return true;
}

bool task_push_load_content_from_cli(
      const char *core_path,
      const char *fullpath,
      content_ctx_info_t *content_info,
      enum rarch_core_type type,
      retro_task_callback_t cb,
      void *user_data)
{
   /* Load content */
   if (!task_load_content_callback(content_info, true, true))
      return false;

   return true;
}

bool task_push_start_builtin_core(
      content_ctx_info_t *content_info,
      enum rarch_core_type type,
      retro_task_callback_t cb,
      void *user_data)
{
   /* Clear content path */
   path_clear(RARCH_PATH_CONTENT);

   /* Preliminary stuff that has to be done before we
    * load the actual content. Can differ per mode. */
   retroarch_set_current_core_type(type, true);

   /* Load content */
   if (!task_load_content_callback(content_info, true, false))
   {
      rarch_menu_running();
      return false;
   }

   /* Push quick menu onto menu stack */
#ifdef HAVE_MENU
   menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);
#endif

   return true;
}

bool task_push_load_content_with_current_core_from_companion_ui(
      const char *fullpath,
      content_ctx_info_t *content_info,
      enum rarch_core_type type,
      retro_task_callback_t cb,
      void *user_data)
{
   path_set(RARCH_PATH_CONTENT, fullpath);

   /* Load content */
   if (!task_load_content_callback(content_info, true, false))
      return false;

   /* Push quick menu onto menu stack */
#ifdef HAVE_MENU
   if (type != CORE_TYPE_DUMMY)
      menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);
#endif

   return true;
}

bool task_push_load_content_with_core_from_menu(
      const char *fullpath,
      content_ctx_info_t *content_info,
      enum rarch_core_type type,
      retro_task_callback_t cb,
      void *user_data)
{
   path_set(RARCH_PATH_CONTENT, fullpath);

   /* Load content */
   if (!task_load_content_callback(content_info, true, false))
   {
      rarch_menu_running();
      return false;
   }

#ifdef HAVE_MENU
   /* Push quick menu onto menu stack */
   if (type != CORE_TYPE_DUMMY)
      menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);
#endif

   return true;
}

bool task_push_load_subsystem_with_core_from_menu(
      const char *fullpath,
      content_ctx_info_t *content_info,
      enum rarch_core_type type,
      retro_task_callback_t cb,
      void *user_data)
{
   pending_subsystem_init = true;

   /* Load content */
   if (!task_load_content_callback(content_info, true, false))
   {
      rarch_menu_running();
      return false;
   }

#ifdef HAVE_MENU
   /* Push quick menu onto menu stack */
   if (type != CORE_TYPE_DUMMY)
      menu_driver_ctl(RARCH_MENU_CTL_SET_PENDING_QUICK_MENU, NULL);
#endif

   return true;
}

void content_get_status(
      bool *contentless,
      bool *is_inited)
{
   *contentless = core_does_not_need_content;
   *is_inited   = _content_is_inited;
}

/* Clears the pending subsystem rom buffer*/
void content_clear_subsystem(void)
{
   unsigned i;

   pending_subsystem_rom_id = 0;
   pending_subsystem_init   = false;

   for (i = 0; i < RARCH_MAX_SUBSYSTEM_ROMS; i++)
   {
      if (pending_subsystem_roms[i])
      {
         free(pending_subsystem_roms[i]);
         pending_subsystem_roms[i] = NULL;
      }
   }
}

/* Checks if launched from the commandline */
bool content_launched_from_cli(void)
{
   return _launched_from_cli;
}

/* Get the current subsystem */
int content_get_subsystem(void)
{
   return pending_subsystem_id;
}

/* Set the current subsystem*/
void content_set_subsystem(unsigned idx)
{
   rarch_system_info_t                  *system = runloop_get_system_info();
   const struct retro_subsystem_info *subsystem;

   /* Core fully loaded, use the subsystem data */
   if (system->subsystem.data)
      subsystem = system->subsystem.data + idx;
   /* Core not loaded completely, use the data we peeked on load core */
   else
      subsystem = subsystem_data + idx;

   pending_subsystem_id = idx;

   if (subsystem && subsystem_current_count > 0)
   {
      strlcpy(pending_subsystem_ident,
         subsystem->ident, sizeof(pending_subsystem_ident));

      pending_subsystem_rom_num                    = subsystem->num_roms;
   }

   RARCH_LOG("[subsystem] settings current subsytem to: %d(%s) roms: %d\n",
      pending_subsystem_id, pending_subsystem_ident, pending_subsystem_rom_num);
}

/* Sets the subsystem by name */
bool content_set_subsystem_by_name(const char* subsystem_name)
{
   rarch_system_info_t                  *system = runloop_get_system_info();
   const struct retro_subsystem_info *subsystem;
   unsigned i = 0;

   /* Core fully loaded, use the subsystem data */
   if (system->subsystem.data)
      subsystem = system->subsystem.data;
   /* Core not loaded completely, use the data we peeked on load core */
   else
      subsystem = subsystem_data;

   for (i = 0; i < subsystem_current_count; i++, subsystem++)
   {
      if (string_is_equal(subsystem_name, subsystem->ident))
      {
         content_set_subsystem(i);
         return true;
      }
   }

   return false;
}

void content_get_subsystem_friendly_name(const char* subsystem_name, char* subsystem_friendly_name, size_t len)
{
   rarch_system_info_t                  *system = runloop_get_system_info();
   const struct retro_subsystem_info *subsystem;
   unsigned i = 0;

   /* Core fully loaded, use the subsystem data */
   if (system->subsystem.data)
      subsystem = system->subsystem.data;
   /* Core not loaded completely, use the data we peeked on load core */
   else
      subsystem = subsystem_data;

   for (i = 0; i < subsystem_current_count; i++, subsystem++)
   {
      if (string_is_equal(subsystem_name, subsystem->ident))
      {
         strlcpy(subsystem_friendly_name, subsystem->desc, len);
         break;
      }
   }

   return;
}

/* Add a rom to the subsystem rom buffer */
void content_add_subsystem(const char* path)
{
   size_t pending_size                              = PATH_MAX_LENGTH * sizeof(char);
   pending_subsystem_roms[pending_subsystem_rom_id] = (char*)malloc(pending_size);

   strlcpy(pending_subsystem_roms[pending_subsystem_rom_id], path, pending_size);
   RARCH_LOG("[subsystem] subsystem id: %d subsystem ident: %s rom id: %d, rom path: %s\n",
      pending_subsystem_id, pending_subsystem_ident, pending_subsystem_rom_id,
      pending_subsystem_roms[pending_subsystem_rom_id]);
   pending_subsystem_rom_id++;
}

/* Get the current subsystem rom id */
unsigned content_get_subsystem_rom_id(void)
{
   return pending_subsystem_rom_id;
}

void content_set_does_not_need_content(void)
{
   core_does_not_need_content = true;
}

void content_unset_does_not_need_content(void)
{
   core_does_not_need_content = false;
}

uint32_t content_get_crc(void)
{
   if (pending_content_rom_crc)
   {
      pending_content_rom_crc      = false;
      content_rom_crc              = file_crc32(0,
            (const char*)pending_content_rom_crc_path);
      RARCH_LOG("CRC32: 0x%x .\n", (unsigned)content_rom_crc);
   }
   return content_rom_crc;
}

char* content_get_subsystem_rom(unsigned index)
{
   return pending_subsystem_roms[index];
}

bool content_is_inited(void)
{
   return _content_is_inited;
}

void content_deinit(void)
{
   unsigned i;

   if (temporary_content)
   {
      for (i = 0; i < temporary_content->size; i++)
      {
         const char *path = temporary_content->elems[i].data;

         RARCH_LOG("%s: %s.\n",
               msg_hash_to_str(MSG_REMOVING_TEMPORARY_CONTENT_FILE), path);
         if (filestream_delete(path) != 0)
            RARCH_ERR("%s: %s.\n",
                  msg_hash_to_str(MSG_FAILED_TO_REMOVE_TEMPORARY_FILE),
                  path);
      }
      string_list_free(temporary_content);
   }

   temporary_content          = NULL;
   content_rom_crc            = 0;
   _content_is_inited         = false;
   core_does_not_need_content = false;
   pending_content_rom_crc    = false;
}

/* Set environment variables before a subsystem load */
void content_set_subsystem_info(void)
{
   if (!pending_subsystem_init)
      return;

   path_set(RARCH_PATH_SUBSYSTEM, pending_subsystem_ident);
   path_set_special(pending_subsystem_roms, pending_subsystem_rom_num);
}

/* Initializes and loads a content file for the currently
 * selected libretro core. */
bool content_init(void)
{
   content_information_ctx_t content_ctx;

   bool ret                                   = true;
   char *error_string                         = NULL;
   struct string_list *content                = NULL;
   global_t *global                           = global_get_ptr();
   settings_t *settings                       = config_get_ptr();
   rarch_system_info_t *sys_info              = runloop_get_system_info();

   temporary_content                          = string_list_new();

   content_ctx.check_firmware_before_loading  = settings->bools.check_firmware_before_loading;
   content_ctx.patch_is_blocked               = rarch_ctl(RARCH_CTL_IS_PATCH_BLOCKED, NULL);
   content_ctx.is_ips_pref                    = rarch_ctl(RARCH_CTL_IS_IPS_PREF, NULL);
   content_ctx.is_bps_pref                    = rarch_ctl(RARCH_CTL_IS_BPS_PREF, NULL);
   content_ctx.is_ups_pref                    = rarch_ctl(RARCH_CTL_IS_UPS_PREF, NULL);
   content_ctx.temporary_content              = temporary_content;
   content_ctx.history_list_enable            = false;
   content_ctx.directory_system               = NULL;
   content_ctx.directory_cache                = NULL;
   content_ctx.name_ips                       = NULL;
   content_ctx.name_bps                       = NULL;
   content_ctx.name_ups                       = NULL;
   content_ctx.valid_extensions               = NULL;
   content_ctx.block_extract                  = false;
   content_ctx.need_fullpath                  = false;
   content_ctx.set_supports_no_game_enable    = false;

   content_ctx.subsystem.data                 = NULL;
   content_ctx.subsystem.size                 = 0;

   if (global)
   {
      if (!string_is_empty(global->name.ips))
         content_ctx.name_ips                 = strdup(global->name.ips);
      if (!string_is_empty(global->name.bps))
         content_ctx.name_bps                 = strdup(global->name.bps);
      if (!string_is_empty(global->name.ups))
         content_ctx.name_ups                 = strdup(global->name.ups);
   }

   if (sys_info)
   {
      struct retro_system_info *system        = runloop_get_libretro_system_info();

      content_ctx.history_list_enable         = settings->bools.history_list_enable;
      content_ctx.set_supports_no_game_enable = settings->bools.set_supports_no_game_enable;

      if (!string_is_empty(settings->paths.directory_system))
         content_ctx.directory_system         = strdup(settings->paths.directory_system);
      if (!string_is_empty(settings->paths.directory_cache))
         content_ctx.directory_cache          = strdup(settings->paths.directory_cache);
      if (!string_is_empty(system->valid_extensions))
         content_ctx.valid_extensions         = strdup(system->valid_extensions);

      content_ctx.block_extract               = system->block_extract;
      content_ctx.need_fullpath               = system->need_fullpath;

      content_ctx.subsystem.data              = sys_info->subsystem.data;
      content_ctx.subsystem.size              = sys_info->subsystem.size;
   }

   _content_is_inited = true;
   content            = string_list_new();

   if (     !temporary_content
         || !content_file_init(&content_ctx, content, &error_string))
   {
      content_deinit();

      ret                = false;
   }

   string_list_free(content);

   if (content_ctx.name_ips)
      free(content_ctx.name_ips);
   if (content_ctx.name_bps)
      free(content_ctx.name_bps);
   if (content_ctx.name_ups)
      free(content_ctx.name_ups);
   if (content_ctx.directory_system)
      free(content_ctx.directory_system);
   if (content_ctx.directory_cache)
      free(content_ctx.directory_cache);
   if (content_ctx.valid_extensions)
      free(content_ctx.valid_extensions);

   if (error_string)
   {
      if (ret)
      {
         RARCH_LOG("%s\n", error_string);
      }
      else
      {
         RARCH_ERR("%s\n", error_string);
      }
      runloop_msg_queue_push(error_string, 2, ret ? 1 : 180, true, NULL, MESSAGE_QUEUE_ICON_DEFAULT, MESSAGE_QUEUE_CATEGORY_INFO);
      free(error_string);
   }

   return ret;
}
