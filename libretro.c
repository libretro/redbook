/*
Copyright 2019 Brad Parker

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

#include <retro_miscellaneous.h>
#include <streams/file_stream.h>

#include <libretro.h>
#include "redbook.h"

#define VIDEO_WIDTH 320
#define VIDEO_HEIGHT 240
#define VIDEO_PIXELS VIDEO_WIDTH * VIDEO_HEIGHT

#define DESC_NUM_PORTS(desc) ((desc)->port_max - (desc)->port_min + 1)
#define DESC_NUM_INDICES(desc) ((desc)->index_max - (desc)->index_min + 1)
#define DESC_NUM_IDS(desc) ((desc)->id_max - (desc)->id_min + 1)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define DESC_OFFSET(desc, port, index, id) ( \
   port * ((desc)->index_max - (desc)->index_min + 1) * ((desc)->id_max - (desc)->id_min + 1) + \
   index * ((desc)->id_max - (desc)->id_min + 1) + \
   id \
)

static uint32_t *frame_buf;
static struct retro_log_callback logging;
static retro_log_printf_t log_cb;
/*static bool use_audio_cb;*/
static float last_aspect;
static float last_sample_rate;
static retro_environment_t environ_cb;
static char retro_base_directory[4096];
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
/*static char retro_game_path[4096];
static struct retro_rumble_interface rumble;
static unsigned phase;
static int mouse_rel_x;
static int mouse_rel_y;*/
static char *cue_sheet = NULL;

struct descriptor
{
   int device;
   int port_min;
   int port_max;
   int index_min;
   int index_max;
   int id_min;
   int id_max;
   uint16_t *value;
};

struct remote_joypad_message
{
   int port;
   int device;
   int index;
   int id;
   uint16_t state;
};

static struct descriptor joypad =
{
   .device = RETRO_DEVICE_JOYPAD,
   .port_min = 0,
   .port_max = 0,
   .index_min = 0,
   .index_max = 0,
   .id_min = RETRO_DEVICE_ID_JOYPAD_B,
   .id_max = RETRO_DEVICE_ID_JOYPAD_R3,
   .value = NULL
};

static struct descriptor analog =
{
   .device = RETRO_DEVICE_ANALOG,
   .port_min = 0,
   .port_max = 0,
   .index_min = RETRO_DEVICE_INDEX_ANALOG_LEFT,
   .index_max = RETRO_DEVICE_INDEX_ANALOG_RIGHT,
   .id_min = RETRO_DEVICE_ID_ANALOG_X,
   .id_max = RETRO_DEVICE_ID_ANALOG_Y,
   .value = NULL
};

static struct descriptor *descriptors[] =
{
   &joypad,
   &analog
};

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   va_list va;
   (void)level;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void retro_init(void)
{
   const char *dir = NULL;
   int size = 0;
   struct descriptor *desc = NULL;
   int i;

   frame_buf = (uint32_t*)calloc(1, VIDEO_PIXELS * sizeof(uint32_t));

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir && *dir)
   {
      snprintf(retro_base_directory, sizeof(retro_base_directory), "%s", dir);
   }

   /* Allocate descriptor values */
   for (i = 0; i < ARRAY_SIZE(descriptors); i++)
   {
      desc = descriptors[i];
      size = DESC_NUM_PORTS(desc) * DESC_NUM_INDICES(desc) * DESC_NUM_IDS(desc);
      descriptors[i]->value = (uint16_t*)calloc(size, sizeof(uint16_t));
   }

   redbook_init(VIDEO_WIDTH, VIDEO_HEIGHT, frame_buf);
}

void retro_deinit(void)
{
   int i;

   redbook_free();
   free(frame_buf);
   frame_buf = NULL;

   /* Free descriptor values */
   for (i = 0; i < ARRAY_SIZE(descriptors); i++)
   {
      free(descriptors[i]->value);
      descriptors[i]->value = NULL;
   }
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   log_cb(RETRO_LOG_INFO, "Plugging device %u into port %u.\n", device, port);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Redbook Audio Player";
   info->library_version  = "1.0";
   info->need_fullpath    = true;
   info->valid_extensions = "cue|bin";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   float aspect                = (float)VIDEO_WIDTH / (float)VIDEO_HEIGHT;
   float sampling_rate         = 44100.0f;

   info->timing.fps            = 60.0f;
   info->timing.sample_rate    = sampling_rate;

   info->geometry.base_width   = VIDEO_WIDTH;
   info->geometry.base_height  = VIDEO_HEIGHT;
   info->geometry.max_width    = VIDEO_WIDTH;
   info->geometry.max_height   = VIDEO_HEIGHT;
   info->geometry.aspect_ratio = aspect;

   last_aspect                 = aspect;
   last_sample_rate            = sampling_rate;
}

void retro_set_environment(retro_environment_t cb)
{
   bool no_content = false;

   static const struct retro_controller_description controllers[] =
   {
      { "Controller", RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 0) },
   };

   static const struct retro_controller_info ports[] =
   {
      { controllers, 1 },
      { NULL, 0 },
   };

   environ_cb = cb;

   if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &logging))
      log_cb = logging.log;
   else
      log_cb = fallback_log;

   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &no_content);
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
}

static void update_input(void)
{
   struct descriptor *desc = NULL;
   uint16_t state;
   uint16_t old;
   int offset;
   int port;
   int index;
   int id;
   int i;

   /* Poll input */
   input_poll_cb();

   /* Parse descriptors */
   for (i = 0; i < ARRAY_SIZE(descriptors); i++)
   {
      /* Get current descriptor */
      desc = descriptors[i];

      if (!desc || !desc->value)
         continue;

      /* Go through range of ports/indices/IDs */
      for (port = desc->port_min; port <= desc->port_max; port++)
      {
         for (index = desc->index_min; index <= desc->index_max; index++)
         {
            for (id = desc->id_min; id <= desc->id_max; id++)
            {
               /* Compute offset into array */
               offset = DESC_OFFSET(desc, port, index, id);

               /* Get old state */
               old = desc->value[offset];

               /* Get new state */
               state = input_state_cb(
                     port,
                     desc->device,
                     index,
                     id);

               /* Continue if state is unchanged */
               if (state == old)
                  continue;

               /* Update state */
               desc->value[offset] = state;
            }
         }
      }
   }
}


static void check_variables(void)
{

}

/*static void audio_callback(void)
{

}

static void audio_set_state(bool enable)
{
   (void)enable;
}*/

void retro_run(void)
{
   bool updated = false;
   unsigned input_state = 0;
   int offset = 0;
   int i;

   update_input();

   /* Combine RetroPad input states into one value */
   for (i = joypad.id_min; i <= joypad.id_max; i++)
   {
      offset = DESC_OFFSET(&joypad, 0, 0, i);

      if (joypad.value && joypad.value[offset])
         input_state |= 1 << i;
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      check_variables();

   redbook_run_frame(input_state);
}

bool retro_load_game(const struct retro_game_info *info)
{
   int64_t len = 0;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   /*struct retro_audio_callback audio_cb = { audio_callback, audio_set_state };*/
   struct retro_input_descriptor desc[] =
   {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_INFO, "XRGB8888 is not supported.\n");
      return false;
   }

   /*snprintf(retro_game_path, sizeof(retro_game_path), "%s", info->path);
   use_audio_cb = environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &audio_cb);*/

   check_variables();

   (void)info;

   if (!filestream_read_file(info->path, (void**)&cue_sheet, &len))
   {
      printf("Error reading from path: %s\n", info->path);
      return false;
   }

   return true;
}

void retro_unload_game(void)
{

}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   return false;
}

size_t retro_serialize_size(void)
{
   return false;
}

bool retro_serialize(void *data_, size_t size)
{
   return false;
}

bool retro_unserialize(const void *data_, size_t size)
{
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{

}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

