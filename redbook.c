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
#include <string.h>
#include <libretro.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation_cdrom.h>
#include "redbook.h"

#define ONE_FRAME_AUDIO_BYTES (75 * 2352) / 60

static unsigned *frame_buf = NULL;
static int frame_width = 0;
static int frame_height = 0;
static RFILE *file = NULL;

void redbook_init(int width, int height, uint32_t *buf)
{
   frame_width = width;
   frame_height = height;
   frame_buf = buf;
}

void redbook_free(void)
{
   if (file)
   {
      filestream_close(file);
      file = NULL;
   }
}

void redbook_run_frame(unsigned input_state)
{
   unsigned trigger_state = 0;
   static unsigned trigger_state_old = 0;
   char path[512] = {0};

   trigger_state = input_state & ~trigger_state_old;
   trigger_state_old = input_state;

   memset(frame_buf, 0xFFCCCCCC, frame_width * frame_height * sizeof(uint32_t));

   switch(trigger_state)
   {
      case (1 << RETRO_DEVICE_ID_JOYPAD_A):
      case (1 << RETRO_DEVICE_ID_JOYPAD_B):
      case (1 << RETRO_DEVICE_ID_JOYPAD_UP):
      case (1 << RETRO_DEVICE_ID_JOYPAD_DOWN):
      case (1 << RETRO_DEVICE_ID_JOYPAD_LEFT):
      case (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT):
      default:
         break;
   }

   if (!file)
   {
      const cdrom_toc_t *toc = retro_vfs_file_get_cdrom_toc();
      char first_audio_track = 1;
      int i;

      if (!toc)
         return;

      for (i = 0; i < toc->num_tracks; i++)
      {
         if (toc->track[i].audio)
         {
            first_audio_track = i + 1;
            break;
         }
      }

#ifdef _WIN32
      snprintf(path, sizeof(path), "cdrom://%c:/drive-track%02d.bin", toc->drive, first_audio_track);
#else
      snprintf(path, sizeof(path), "cdrom://drive%c-track%02d.bin", toc->drive, first_audio_track);
#endif

      file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, 0);
   }

   {
      char data[ONE_FRAME_AUDIO_BYTES] = {0};
      int i;

      filestream_read(file, data, ONE_FRAME_AUDIO_BYTES);

      for (i = 0; i < ONE_FRAME_AUDIO_BYTES / sizeof(int16_t); i += 2)
         audio_cb(((int16_t*)data)[i], ((int16_t*)data)[i + 1]);
   }
}
