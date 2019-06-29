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
#include <compat/strl.h>
#include <math.h>
#include "redbook.h"
#include "ugui_tools.h"

#define ONE_FRAME_AUDIO_BYTES (2352 * 75) / 60

static unsigned *frame_buf = NULL;
static int frame_width = 0;
static int frame_height = 0;
static RFILE *file = NULL;
static unsigned char audio_track = 1;

void redbook_init(int width, int height, uint32_t *buf)
{
   frame_width = width;
   frame_height = height;
   frame_buf = buf;

   gui_init(frame_width, frame_height, sizeof(unsigned));
   gui_set_window_title("Audio Player");
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
   const cdrom_toc_t *toc = retro_vfs_file_get_cdrom_toc();

   if (!toc)
      return;

   trigger_state = input_state & ~trigger_state_old;
   trigger_state_old = input_state;

   /*memset(frame_buf, 0xFFCCCCCC, frame_width * frame_height * sizeof(uint32_t));*/

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
      int i;

      for (i = 0; i < toc->num_tracks; i++)
      {
         if (toc->track[i].audio)
         {
            audio_track = i + 1;
            break;
         }
      }

#ifdef _WIN32
      snprintf(path, sizeof(path), "cdrom://%c:/drive-track%02d.bin", toc->drive, audio_track);
#else
      snprintf(path, sizeof(path), "cdrom://drive%c-track%02d.bin", toc->drive, audio_track);
#endif

      file = filestream_open(path, RETRO_VFS_FILE_ACCESS_READ, 0);
   }

   {
      int bytes = 2352 * round(ONE_FRAME_AUDIO_BYTES / 2352);

      char *data = calloc(1, bytes);

      if (data)
      {
         filestream_read(file, data, bytes);
         audio_batch_cb((const int16_t*)data, bytes / sizeof(unsigned));
         free(data);
      }
   }

   {
      char play_string[512] = {0};
      size_t pos = 0;
      char track_string[4] = {0};
      char audio_pos_string[10] = {0};
      char audio_total_string[10] = {0};
      int i;
      const libretro_vfs_implementation_file *stream = filestream_get_vfs_handle(file);
      const vfs_cdrom_t *cdrom = retro_vfs_file_get_cdrom_position(stream);
      unsigned char cur_track_min = 0;
      unsigned char cur_track_sec = 0;
      unsigned char cur_track_frame = 0;
      unsigned char total_track_min = 0;
      unsigned char total_track_sec = 0;
      unsigned char total_track_frame = 0;
      unsigned cur_lba = 0;

      if (cdrom && toc->num_tracks && cdrom->cur_track)
      {
         cur_lba = msf_to_lba(toc->track[cdrom->cur_track - 1].min + cdrom->cur_min, toc->track[cdrom->cur_track - 1].sec + cdrom->cur_sec, toc->track[cdrom->cur_track - 1].frame + cdrom->cur_frame);

         for (i = 0; i < toc->num_tracks; i++)
         {
            if (cur_lba >= toc->track[i].lba_start)
               audio_track = i + 1;
         }
      }
      else
         cur_lba = msf_to_lba(toc->track[0].min + cdrom->cur_min, toc->track[0].sec + cdrom->cur_sec, toc->track[0].frame + cdrom->cur_frame);

      lba_to_msf(cur_lba - toc->track[audio_track - 1].lba_start, &cur_track_min, &cur_track_sec, &cur_track_frame);
      lba_to_msf(toc->track[audio_track - 1].track_size, &total_track_min, &total_track_sec, &total_track_frame);

      snprintf(track_string, sizeof(track_string), "%02u", (unsigned)audio_track);
      snprintf(audio_pos_string, sizeof(audio_pos_string), "%02u:%02u", (unsigned)cur_track_min, (unsigned)cur_track_sec);
      snprintf(audio_total_string, sizeof(audio_total_string), "%02u:%02u", (unsigned)total_track_min, (unsigned)total_track_sec);

      pos = strlcpy(play_string, "Track ", sizeof(play_string));
      pos = strlcat(play_string, track_string, sizeof(play_string) - pos);
      pos = strlcat(play_string, " playing: ", sizeof(play_string) - pos);
      pos = strlcat(play_string, audio_pos_string, sizeof(play_string) - pos);
      pos = strlcat(play_string, " / ", sizeof(play_string) - pos);
      pos = strlcat(play_string, audio_total_string, sizeof(play_string) - pos);

      gui_set_message(play_string);
      gui_draw();
      video_cb(gui_get_framebuffer(), frame_width, frame_height, frame_width * sizeof(uint32_t));
   }
}
