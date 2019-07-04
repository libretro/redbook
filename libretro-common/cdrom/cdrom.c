/* Copyright  (C) 2010-2019 The RetroArch team
*
* ---------------------------------------------------------------------------------------
* The following license statement only applies to this file (cdrom.c).
* ---------------------------------------------------------------------------------------
*
* Permission is hereby granted, free of charge,
* to any person obtaining a copy of this software and associated documentation files (the "Software"),
* to deal in the Software without restriction, including without limitation the rights to
* use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
* and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
* INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cdrom/cdrom.h>
#include <libretro.h>
#include <stdio.h>
#include <string.h>
#include <compat/strl.h>
#include <retro_math.h>
#include <streams/file_stream.h>
#include <retro_endianness.h>
#include <retro_miscellaneous.h>
#include <vfs/vfs_implementation.h>
#include <lists/string_list.h>
#include <lists/dir_list.h>
#include <string/stdstring.h>

#include <math.h>
#include <unistd.h>

#if defined(__linux__) && !defined(ANDROID)
#include <stropts.h>
#include <scsi/sg.h>
#endif

#if defined(_WIN32) && !defined(_XBOX)
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#endif

#define CDROM_CUE_TRACK_BYTES 107
#define CDROM_MAX_SENSE_BYTES 16
#define CDROM_MAX_RETRIES 10

typedef enum
{
   DIRECTION_NONE,
   DIRECTION_IN,
   DIRECTION_OUT
} CDROM_CMD_Direction;

void lba_to_msf(unsigned lba, unsigned char *min, unsigned char *sec, unsigned char *frame)
{
   if (!min || !sec || !frame)
      return;

   *frame = lba % 75;
   lba /= 75;
   *sec = lba % 60;
   lba /= 60;
   *min = lba;
}

unsigned msf_to_lba(unsigned char min, unsigned char sec, unsigned char frame)
{
   return (min * 60 + sec) * 75 + frame;
}

void increment_msf(unsigned char *min, unsigned char *sec, unsigned char *frame)
{
   if (!min || !sec || !frame)
      return;

   *min = (*frame == 74) ? (*sec < 59 ? *min : *min + 1) : *min;
   *sec = (*frame == 74) ? (*sec < 59 ? (*sec + 1) : 0) : *sec;
   *frame = (*frame < 74) ? (*frame + 1) : 0;
}

#if defined(_WIN32) && !defined(_XBOX)
static int cdrom_send_command_win32(HANDLE fh, CDROM_CMD_Direction dir, void *buf, size_t len, unsigned char *cmd, size_t cmd_len, unsigned char *sense, size_t sense_len)
{
   DWORD ioctl_bytes;
   BOOL ioctl_rv;
   struct sptd_with_sense
   {
     SCSI_PASS_THROUGH_DIRECT s;
     UCHAR sense[128];
   } sptd;

   memset(&sptd, 0, sizeof(sptd));

   sptd.s.Length = sizeof(sptd.s);
   sptd.s.CdbLength = cmd_len;

   switch (dir)
   {
      case DIRECTION_IN:
         sptd.s.DataIn = SCSI_IOCTL_DATA_IN;
         break;
      case DIRECTION_OUT:
         sptd.s.DataIn = SCSI_IOCTL_DATA_OUT;
         break;
      case DIRECTION_NONE:
      default:
         sptd.s.DataIn = SCSI_IOCTL_DATA_UNSPECIFIED;
         break;
   }

   sptd.s.TimeOutValue = 30;
   sptd.s.DataBuffer = buf;
   sptd.s.DataTransferLength = len;
   sptd.s.SenseInfoLength = sizeof(sptd.sense);
   sptd.s.SenseInfoOffset = offsetof(struct sptd_with_sense, sense);

   memcpy(sptd.s.Cdb, cmd, cmd_len);

   ioctl_rv = DeviceIoControl(fh, IOCTL_SCSI_PASS_THROUGH_DIRECT, &sptd,
      sizeof(sptd), &sptd, sizeof(sptd), &ioctl_bytes, NULL);

   if (!ioctl_rv || sptd.s.ScsiStatus != 0)
      return 1;

   return 0;
}
#endif

#if defined(__linux__) && !defined(ANDROID)
static int cdrom_send_command_linux(int fd, CDROM_CMD_Direction dir, void *buf, size_t len, unsigned char *cmd, size_t cmd_len, unsigned char *sense, size_t sense_len)
{
   sg_io_hdr_t sgio = {0};
   int rv;

   switch (dir)
   {
      case DIRECTION_IN:
         sgio.dxfer_direction = SG_DXFER_FROM_DEV;
         break;
      case DIRECTION_OUT:
         sgio.dxfer_direction = SG_DXFER_TO_DEV;
         break;
      case DIRECTION_NONE:
      default:
         sgio.dxfer_direction = SG_DXFER_NONE;
         break;
   }

   sgio.interface_id = 'S';
   sgio.cmd_len = cmd_len;
   sgio.cmdp = cmd;
   sgio.dxferp = buf;
   sgio.dxfer_len = len;
   sgio.sbp = sense;
   sgio.mx_sb_len = sense_len;
   sgio.timeout = 30000;

   rv = ioctl(fd, SG_IO, &sgio);

   if (rv == -1 || sgio.info & SG_INFO_CHECK)
      return 1;

   return 0;
}
#endif

static int cdrom_send_command(const libretro_vfs_implementation_file *stream, CDROM_CMD_Direction dir, void *buf, size_t len, unsigned char *cmd, size_t cmd_len, size_t skip)
{
   unsigned char *xfer_buf;
   unsigned char sense[CDROM_MAX_SENSE_BYTES] = {0};
   unsigned char retries_left = CDROM_MAX_RETRIES;
   int rv = 0;

   if (!cmd || cmd_len == 0)
      return 1;

   xfer_buf = (unsigned char*)calloc(1, len + skip);

   if (!xfer_buf)
      return 1;

#ifdef CDROM_DEBUG
   {
      unsigned i;

      printf("CDROM Send Command: ");

      for (i = 0; i < cmd_len / sizeof(*cmd); i++)
      {
         printf("%02X ", cmd[i]);
      }

      printf("\n");
      fflush(stdout);
   }
#endif

retry:
#if defined(__linux__) && !defined(ANDROID)
   if (!cdrom_send_command_linux(fileno(stream->fp), dir, xfer_buf, len + skip, cmd, cmd_len, sense, sizeof(sense)))
#else
#if defined(_WIN32) && !defined(_XBOX)
   if (!cdrom_send_command_win32(stream->fh, dir, xfer_buf, len + skip, cmd, cmd_len, sense, sizeof(sense)))
#endif
#endif
   {
      rv = 0;

      if (buf)
         memcpy(buf, xfer_buf + skip, len);
   }
   else
   {
      unsigned i;
      const char *sense_key_text = NULL;
      unsigned char key = sense[2] & 0xF;
      unsigned char asc = sense[12];
      unsigned char ascq = sense[13];

      (void)sense_key_text;
      (void)i;

      /* INQUIRY/TEST should never fail, don't retry */
      if (cmd[0] != 0x0 && cmd[0] != 0x12)
      {
         switch (key)
         {
            case 0:
            case 2:
            case 3:
            case 4:
            case 6:
               if (retries_left)
               {
#ifdef CDROM_DEBUG
                  printf("CDROM Read Retry...\n");
                  fflush(stdout);
#endif
                  retries_left--;
                  usleep(1000 * 1000);
                  goto retry;
               }
               else
               {
                  rv = 1;
#ifdef CDROM_DEBUG
                  printf("CDROM Read Retries failed, giving up.\n");
                  fflush(stdout);
#endif
               }

               break;
            default:
               break;
         }
      }

#ifdef CDROM_DEBUG
      printf("CHECK CONDITION\n");

      for (i = 0; i < CDROM_MAX_SENSE_BYTES; i++)
      {
         printf("%02X ", sense[i]);
      }

      printf("\n");

      if (sense[0] == 0x70)
         printf("CURRENT ERROR:\n");
      if (sense[0] == 0x71)
         printf("DEFERRED ERROR:\n");

      switch (key)
      {
         case 0:
            sense_key_text = "NO SENSE";
            break;
         case 1:
            sense_key_text = "RECOVERED ERROR";
            break;
         case 2:
            sense_key_text = "NOT READY";
            break;
         case 3:
            sense_key_text = "MEDIUM ERROR";
            break;
         case 4:
            sense_key_text = "HARDWARE ERROR";
            break;
         case 5:
            sense_key_text = "ILLEGAL REQUEST";
            break;
         case 6:
            sense_key_text = "UNIT ATTENTION";
            break;
         case 7:
            sense_key_text = "DATA PROTECT";
            break;
         case 8:
            sense_key_text = "BLANK CHECK";
            break;
         case 9:
            sense_key_text = "VENDOR SPECIFIC";
            break;
         case 10:
            sense_key_text = "COPY ABORTED";
            break;
         case 11:
            sense_key_text = "ABORTED COMMAND";
            break;
         case 13:
            sense_key_text = "VOLUME OVERFLOW";
            break;
         case 14:
            sense_key_text = "MISCOMPARE";
            break;
      }

      printf("Sense Key: %02X (%s)\n", key, sense_key_text);
      printf("ASC: %02X\n", asc);
      printf("ASCQ: %02X\n", ascq);

      switch (key)
      {
         case 2:
         {
            switch (asc)
            {
               case 4:
               {
                  switch (ascq)
                  {
                     case 1:
                        printf("Description: LOGICAL UNIT IS IN PROCESS OF BECOMING READY\n");
                        break;
                     default:
                        break;
                  }

                  break;
               }
               case 0x3a:
               {
                  switch (ascq)
                  {
                     case 0:
                        printf("Description: MEDIUM NOT PRESENT\n");
                        break;
                     case 3:
                        printf("Description: MEDIUM NOT PRESENT - LOADABLE\n");
                        break;
                     case 1:
                        printf("Description: MEDIUM NOT PRESENT - TRAY CLOSED\n");
                        break;
                     case 2:
                        printf("Description: MEDIUM NOT PRESENT - TRAY OPEN\n");
                        break;
                     default:
                        break;
                  }

                  break;
               }
               default:
                  break;
            }
         }
         case 6:
         {
            if (asc == 0x28 && ascq == 0)
               printf("Description: NOT READY TO READY CHANGE, MEDIUM MAY HAVE CHANGED\n");
            break;
         }
         default:
            break;
      }

      fflush(stdout);

      rv = 1;
#endif
   }

   if (xfer_buf)
      free(xfer_buf);

   return rv;
}

static const char* get_profile(unsigned short profile)
{
   switch (profile)
   {
      case 2:
         return "Removable disk";
         break;
      case 8:
         return "CD-ROM";
         break;
      case 9:
         return "CD-R";
         break;
      case 0xA:
         return "CD-RW";
         break;
      case 0x10:
         return "DVD-ROM";
         break;
      case 0x11:
         return "DVD-R Sequential Recording";
         break;
      case 0x12:
         return "DVD-RAM";
         break;
      case 0x13:
         return "DVD-RW Restricted Overwrite";
         break;
      case 0x14:
         return "DVD-RW Sequential recording";
         break;
      case 0x15:
         return "DVD-R Dual Layer Sequential Recording";
         break;
      case 0x16:
         return "DVD-R Dual Layer Jump Recording";
         break;
      case 0x17:
         return "DVD-RW Dual Layer";
         break;
      case 0x1A:
         return "DVD+RW";
         break;
      case 0x1B:
         return "DVD+R";
         break;
      case 0x2A:
         return "DVD+RW Dual Layer";
         break;
      case 0x2B:
         return "DVD+R Dual Layer";
         break;
      case 0x40:
         return "BD-ROM";
         break;
      case 0x41:
         return "BD-R SRM";
         break;
      case 0x42:
         return "BD-R RRM";
         break;
      case 0x43:
         return "BD-RE";
         break;
      case 0x50:
         return "HD DVD-ROM";
         break;
      case 0x51:
         return "HD DVD-R";
         break;
      case 0x52:
         return "HD DVD-RAM";
         break;
      case 0x53:
         return "HD DVD-RW";
         break;
      case 0x58:
         return "HD DVD-R Dual Layer";
         break;
      case 0x5A:
         return "HD DVD-RW Dual Layer";
         break;
      default:
         break;
   }

   return "Unknown";
}

void cdrom_get_current_config_random_readable(const libretro_vfs_implementation_file *stream)
{
   unsigned char cdb[] = {0x46, 0x2, 0, 0x10, 0, 0, 0, 0, 0x14, 0};
   unsigned char buf[0x14] = {0};
   int rv = cdrom_send_command(stream, DIRECTION_IN, buf, sizeof(buf), cdb, sizeof(cdb), 0);
   int i;

   printf("get current config random readable status code %d\n", rv);

   if (rv)
      return;

   printf("Feature Header: ");

   for (i = 0; i < 8; i++)
   {
      printf("%02X ", buf[i]);
   }

   printf("\n");

   printf("Random Readable Feature Descriptor: ");

   for (i = 0; i < 12; i++)
   {
      printf("%02X ", buf[8 + i]);
   }

   printf("\n");

   printf("Supported commands: READ CAPACITY, READ (10)\n");
}

void cdrom_get_current_config_multiread(const libretro_vfs_implementation_file *stream)
{
   unsigned char cdb[] = {0x46, 0x2, 0, 0x1D, 0, 0, 0, 0, 0xC, 0};
   unsigned char buf[0xC] = {0};
   int rv = cdrom_send_command(stream, DIRECTION_IN, buf, sizeof(buf), cdb, sizeof(cdb), 0);
   int i;

   printf("get current config multi-read status code %d\n", rv);

   if (rv)
      return;

   printf("Feature Header: ");

   for (i = 0; i < 8; i++)
   {
      printf("%02X ", buf[i]);
   }

   printf("\n");

   printf("Multi-Read Feature Descriptor: ");

   for (i = 0; i < 4; i++)
   {
      printf("%02X ", buf[8 + i]);
   }

   printf("\n");

   printf("Supported commands: READ (10), READ CD, READ DISC INFORMATION, READ TRACK INFORMATION\n");
}

void cdrom_get_current_config_cdread(const libretro_vfs_implementation_file *stream)
{
   unsigned char cdb[] = {0x46, 0x2, 0, 0x1E, 0, 0, 0, 0, 0x10, 0};
   unsigned char buf[0x10] = {0};
   int rv = cdrom_send_command(stream, DIRECTION_IN, buf, sizeof(buf), cdb, sizeof(cdb), 0);
   int i;

   printf("get current config cd read status code %d\n", rv);

   if (rv)
      return;

   printf("Feature Header: ");

   for (i = 0; i < 8; i++)
   {
      printf("%02X ", buf[i]);
   }

   printf("\n");

   printf("CD Read Feature Descriptor: ");

   for (i = 0; i < 8; i++)
   {
      printf("%02X ", buf[8 + i]);
   }

   if (buf[8 + 2] & 1)
      printf("(current)\n");

   printf("Supported commands: READ CD, READ CD MSF, READ TOC/PMA/ATIP\n");
}

void cdrom_get_current_config_profiles(const libretro_vfs_implementation_file *stream)
{
   unsigned char cdb[] = {0x46, 0x2, 0, 0x0, 0, 0, 0, 0xFF, 0xFA, 0};
   unsigned char buf[0xFFFA] = {0};
   int rv = cdrom_send_command(stream, DIRECTION_IN, buf, sizeof(buf), cdb, sizeof(cdb), 0);
   int i;

   printf("get current config profiles status code %d\n", rv);

   if (rv)
      return;

   printf("Feature Header: ");

   for (i = 0; i < 8; i++)
   {
      printf("%02X ", buf[i]);
   }

   printf("\n");

   printf("Profile List Descriptor: ");

   for (i = 0; i < 4; i++)
   {
      printf("%02X ", buf[8 + i]);
   }

   printf("\n");

   printf("Number of profiles: %u\n", buf[8 + 3] / 4);

   for (i = 0; i < buf[8 + 3] / 4; i++)
   {
      unsigned short profile = (buf[8 + (4 * (i + 1))] << 8) | buf[8 + (4 * (i + 1)) + 1];
      profile = swap_if_big32(profile);

      printf("Profile Number: %04X (%s) ", profile, get_profile(profile));

      if (buf[8 + (4 * (i + 1)) + 2] & 1)
         printf("(current)\n");
      else
         printf("\n");
   }
}

void cdrom_get_current_config_core(const libretro_vfs_implementation_file *stream)
{
   unsigned char cdb[] = {0x46, 0x2, 0, 0x1, 0, 0, 0, 0, 0x14, 0};
   unsigned char buf[20] = {0};
   unsigned intf_std = 0;
   int rv = cdrom_send_command(stream, DIRECTION_IN, buf, sizeof(buf), cdb, sizeof(cdb), 0);
   int i;
   const char *intf_std_name = "Unknown";

   printf("get current config core status code %d\n", rv);

   if (rv)
      return;

   printf("Feature Header: ");

   for (i = 0; i < 8; i++)
   {
      printf("%02X ", buf[i]);
   }

   printf("\n");

   if (buf[6] == 0 && buf[7] == 8)
      printf("Current Profile: CD-ROM\n");
   else
      printf("Current Profile: %02X%02X\n", buf[6], buf[7]);

   printf("Core Feature Descriptor: ");

   for (i = 0; i < 12; i++)
   {
      printf("%02X ", buf[8 + i]);
   }

   printf("\n");

   intf_std = buf[8 + 4] << 24 | buf[8 + 5] << 16 | buf[8 + 6] << 8 | buf[8 + 7];

   intf_std = swap_if_big32(intf_std);

   switch (intf_std)
   {
      case 0:
         intf_std_name = "Unspecified";
         break;
      case 1:
         intf_std_name = "SCSI Family";
         break;
      case 2:
         intf_std_name = "ATAPI";
         break;
      case 7:
         intf_std_name = "Serial ATAPI";
         break;
      case 8:
         intf_std_name = "USB";
         break;
      default:
         break;
   }

   printf("Physical Interface Standard: %u (%s)\n", intf_std, intf_std_name);
}

int cdrom_read_subq(libretro_vfs_implementation_file *stream, unsigned char *buf, size_t len)
{
   /* MMC Command: READ TOC/PMA/ATIP */
   unsigned char cdb[] = {0x43, 0x2, 0x2, 0, 0, 0, 0x1, 0x9, 0x30, 0};
#ifdef CDROM_DEBUG
   unsigned short data_len = 0;
   unsigned char first_session = 0;
   unsigned char last_session = 0;
   int i;
#endif
   int rv;

   if (!buf)
      return 1;

   rv = cdrom_send_command(stream, DIRECTION_IN, buf, len, cdb, sizeof(cdb), 0);

   if (rv)
     return 1;

#ifdef CDROM_DEBUG
   data_len = buf[0] << 8 | buf[1];
   first_session = buf[2];
   last_session = buf[3];

   printf("Data Length: %d\n", data_len);
   printf("First Session: %d\n", first_session);
   printf("Last Session: %d\n", last_session);

   for (i = 0; i < (data_len - 2) / 11; i++)
   {
      unsigned char session_num = buf[4 + (i * 11) + 0];
      unsigned char adr = (buf[4 + (i * 11) + 1] >> 4) & 0xF;
      /*unsigned char control = buf[4 + (i * 11) + 1] & 0xF;*/
      unsigned char tno = buf[4 + (i * 11) + 2];
      unsigned char point = buf[4 + (i * 11) + 3];
      unsigned char pmin = buf[4 + (i * 11) + 8];
      unsigned char psec = buf[4 + (i * 11) + 9];
      unsigned char pframe = buf[4 + (i * 11) + 10];

      /*printf("i %d control %d adr %d tno %d point %d: ", i, control, adr, tno, point);*/
      /* why is control always 0? */

      if (/*(control == 4 || control == 6) && */adr == 1 && tno == 0 && point >= 1 && point <= 99)
      {
         printf("- Session#: %d TNO %d POINT %d ", session_num, tno, point);
         printf("Track start time: (MSF %02u:%02u:%02u) ", (unsigned)pmin, (unsigned)psec, (unsigned)pframe);
      }
      else if (/*(control == 4 || control == 6) && */adr == 1 && tno == 0 && point == 0xA0)
      {
         printf("- Session#: %d TNO %d POINT %d ", session_num, tno, point);
         printf("First Track Number: %d ", pmin);
         printf("Disc Type: %d ", psec);
      }
      else if (/*(control == 4 || control == 6) && */adr == 1 && tno == 0 && point == 0xA1)
      {
         printf("- Session#: %d TNO %d POINT %d ", session_num, tno, point);
         printf("Last Track Number: %d ", pmin);
      }
      else if (/*(control == 4 || control == 6) && */adr == 1 && tno == 0 && point == 0xA2)
      {
         printf("- Session#: %d TNO %d POINT %d ", session_num, tno, point);
         printf("Lead-out runtime: (MSF %02u:%02u:%02u) ", (unsigned)pmin, (unsigned)psec, (unsigned)pframe);
      }

      printf("\n");
   }

   fflush(stdout);
#endif
   return 0;
}

static int cdrom_read_track_info(libretro_vfs_implementation_file *stream, unsigned char track, cdrom_toc_t *toc)
{
   /* MMC Command: READ TRACK INFORMATION */
   unsigned char cdb[] = {0x52, 0x1, 0, 0, 0, track, 0, 0x1, 0x80, 0};
   unsigned char buf[384] = {0};
   unsigned lba = 0;
   unsigned track_size = 0;
   int rv = cdrom_send_command(stream, DIRECTION_IN, buf, sizeof(buf), cdb, sizeof(cdb), 0);

   if (rv)
     return 1;

   memcpy(&lba, buf + 8, 4);
   memcpy(&track_size, buf + 24, 4);

   lba = swap_if_little32(lba);
   track_size = swap_if_little32(track_size);

   /* lba_start may be earlier than the MSF start times seen in read_subq */
   toc->track[track - 1].lba_start = lba;
   toc->track[track - 1].track_size = track_size;

#ifdef CDROM_DEBUG
   printf("Track %d Info: ", track);
   printf("Copy: %d ", (buf[5] & 0x10) > 0);
   printf("Data Mode: %d ", buf[6] & 0xF);
   printf("LBA Start: %d ", lba);
   printf("Track Size: %d\n", track_size);
   fflush(stdout);
#endif

   return 0;
}

int cdrom_set_read_speed(libretro_vfs_implementation_file *stream, unsigned speed)
{
   unsigned new_speed = swap_if_big32(speed);
   /* MMC Command: SET CD SPEED */
   unsigned char cmd[] = {0xBB, 0, (new_speed >> 24) & 0xFF, (new_speed >> 16) & 0xFF, (new_speed >> 8) & 0xFF, new_speed & 0xFF, 0, 0, 0, 0, 0, 0 };

   return cdrom_send_command(stream, DIRECTION_NONE, NULL, 0, cmd, sizeof(cmd), 0);
}

int cdrom_write_cue(libretro_vfs_implementation_file *stream, char **out_buf, size_t *out_len, char cdrom_drive, unsigned char *num_tracks, cdrom_toc_t *toc)
{
   unsigned char buf[2352] = {0};
   unsigned short data_len = 0;
   size_t len = 0;
   size_t pos = 0;
   int rv = 0;
   int i;

   if (!out_buf || !out_len || !num_tracks || !toc)
   {
#ifdef CDROM_DEBUG
      printf("Invalid buffer/length pointer for CDROM cue sheet\n");
      fflush(stdout);
#endif
      return 1;
   }

   cdrom_set_read_speed(stream, 0xFFFFFFFF);

   rv = cdrom_read_subq(stream, buf, sizeof(buf));

   if (rv)
      return rv;

   data_len = buf[0] << 8 | buf[1];

   for (i = 0; i < (data_len - 2) / 11; i++)
   {
      unsigned char adr = (buf[4 + (i * 11) + 1] >> 4) & 0xF;
      unsigned char tno = buf[4 + (i * 11) + 2];
      unsigned char point = buf[4 + (i * 11) + 3];
      unsigned char pmin = buf[4 + (i * 11) + 8];

      if (/*(control == 4 || control == 6) && */adr == 1 && tno == 0 && point == 0xA1)
      {
         *num_tracks = pmin;
#ifdef CDROM_DEBUG
         printf("Number of CDROM tracks: %d\n", *num_tracks);
         fflush(stdout);
#endif
         break;
      }
   }

   if (!*num_tracks || *num_tracks > 99)
   {
#ifdef CDROM_DEBUG
      printf("Invalid number of CDROM tracks: %d\n", *num_tracks);
      fflush(stdout);
#endif
      return 1;
   }

   len = CDROM_CUE_TRACK_BYTES * (*num_tracks);
   toc->num_tracks = *num_tracks;
   *out_buf = (char*)calloc(1, len);
   *out_len = len;

   for (i = 0; i < (data_len - 2) / 11; i++)
   {
      /*unsigned char session_num = buf[4 + (i * 11) + 0];*/
      unsigned char adr = (buf[4 + (i * 11) + 1] >> 4) & 0xF;
      unsigned char control = buf[4 + (i * 11) + 1] & 0xF;
      unsigned char tno = buf[4 + (i * 11) + 2];
      unsigned char point = buf[4 + (i * 11) + 3];
      unsigned char pmin = buf[4 + (i * 11) + 8];
      unsigned char psec = buf[4 + (i * 11) + 9];
      unsigned char pframe = buf[4 + (i * 11) + 10];
      unsigned lba = msf_to_lba(pmin, psec, pframe);

      /*printf("i %d control %d adr %d tno %d point %d: ", i, control, adr, tno, point);*/
      /* why is control always 0? */

      if (/*(control == 4 || control == 6) && */adr == 1 && tno == 0 && point >= 1 && point <= 99)
      {
         unsigned char mode = 1;
         bool audio = false;
         const char *track_type = "MODE1/2352";

         mode = adr;
         audio = (!(control & 0x4) && !(control & 0x5));

#ifdef CDROM_DEBUG
         printf("Track %02d CONTROL %01X ADR %01X MODE %d AUDIO? %d\n", point, control, adr, mode, audio);
         fflush(stdout);
#endif

         toc->track[point - 1].track_num = point;
         toc->track[point - 1].min = pmin;
         toc->track[point - 1].sec = psec;
         toc->track[point - 1].frame = pframe;
         toc->track[point - 1].lba = lba;
         toc->track[point - 1].mode = mode;
         toc->track[point - 1].audio = audio;

         if (audio)
            track_type = "AUDIO";
         else if (mode == 1)
            track_type = "MODE1/2352";
         else if (mode == 2)
            track_type = "MODE2/2352";

         cdrom_read_track_info(stream, point, toc);

#if defined(_WIN32) && !defined(_XBOX)
         pos += snprintf(*out_buf + pos, len - pos, "FILE \"cdrom://%c://drive-track%02d.bin\" BINARY\n", cdrom_drive, point);
#else
         pos += snprintf(*out_buf + pos, len - pos, "FILE \"cdrom://drive%c-track%02d.bin\" BINARY\n", cdrom_drive, point);
#endif
         pos += snprintf(*out_buf + pos, len - pos, "  TRACK %02d %s\n", point, track_type);

         {
            unsigned pregap_lba_len = toc->track[point - 1].lba - toc->track[point - 1].lba_start;

            if (toc->track[point - 1].audio && pregap_lba_len > 0)
            {
               unsigned char min = 0;
               unsigned char sec = 0;
               unsigned char frame = 0;

               lba_to_msf(pregap_lba_len, &min, &sec, &frame);

               pos += snprintf(*out_buf + pos, len - pos, "    INDEX 00 00:00:00\n");
               pos += snprintf(*out_buf + pos, len - pos, "    INDEX 01 %02u:%02u:%02u\n", (unsigned)min, (unsigned)sec, (unsigned)frame);
            }
            else
               pos += snprintf(*out_buf + pos, len - pos, "    INDEX 01 00:00:00\n");
         }
      }
   }

   return 0;
}

/* needs 32 bytes for full vendor, product and version */
int cdrom_get_inquiry(const libretro_vfs_implementation_file *stream, char *model, int len, bool *is_cdrom)
{
   /* MMC Command: INQUIRY */
   unsigned char cdb[] = {0x12, 0, 0, 0, 0xff, 0};
   unsigned char buf[256] = {0};
   int rv = cdrom_send_command(stream, DIRECTION_IN, buf, sizeof(buf), cdb, sizeof(cdb), 0);
   bool cdrom = false;

   if (rv)
      return 1;

   if (model && len >= 32)
   {
      memset(model, 0, len);

      /* vendor */
      memcpy(model, buf + 8, 8);

      model[8] = ' ';

      /* product */
      memcpy(model + 9, buf + 16, 16);

      model[25] = ' ';

      /* version */
      memcpy(model + 26, buf + 32, 4);
   }

   cdrom = (buf[0] == 5);

   if (is_cdrom && cdrom)
      *is_cdrom = true;

#ifdef CDROM_DEBUG
   printf("Device Model: %s (is CD-ROM? %s)\n", model, (cdrom ? "yes" : "no"));
#endif
   return 0;
}

int cdrom_read(libretro_vfs_implementation_file *stream, unsigned char min, unsigned char sec, unsigned char frame, void *s, size_t len, size_t skip)
{
   /* MMC Command: READ CD MSF */
   unsigned char cdb[] = {0xB9, 0, 0, min, sec, frame, 0, 0, 0, 0xF8, 0, 0};
   int rv;

   if (len + skip <= 2352)
   {
      unsigned char next_min = (frame == 74) ? (sec < 59 ? min : min + 1) : min;
      unsigned char next_sec = (frame == 74) ? (sec < 59 ? (sec + 1) : 0) : sec;
      unsigned char next_frame = (frame < 74) ? (frame + 1) : 0;

      cdb[6] = next_min;
      cdb[7] = next_sec;
      cdb[8] = next_frame;

#ifdef CDROM_DEBUG
      printf("single-frame read: from %d %d %d to %d %d %d skip %" PRId64 "\n", cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], skip);
      fflush(stdout);
#endif
   }
   else
   {
      unsigned frames = msf_to_lba(min, sec, frame) + ceil((len + skip) / 2352.0);

      lba_to_msf(frames, &cdb[6], &cdb[7], &cdb[8]);

#ifdef CDROM_DEBUG
      printf("multi-frame read: from %d %d %d to %d %d %d skip %" PRId64 "\n", cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], skip);
      fflush(stdout);
#endif
   }

   rv = cdrom_send_command(stream, DIRECTION_IN, s, len, cdb, sizeof(cdb), skip);

#ifdef CDROM_DEBUG
   printf("read status code %d\n", rv);
   fflush(stdout);
#endif

   if (rv)
      return 1;

   return 0;
}

int cdrom_stop(const libretro_vfs_implementation_file *stream)
{
   /* MMC Command: START STOP UNIT */
   unsigned char cdb[] = {0x1B, 0, 0, 0, 0x0, 0};
   int rv = cdrom_send_command(stream, DIRECTION_NONE, NULL, 0, cdb, sizeof(cdb), 0);

#ifdef CDROM_DEBUG
   printf("stop status code %d\n", rv);
   fflush(stdout);
#endif

   if (rv)
      return 1;

   return 0;
}

int cdrom_unlock(const libretro_vfs_implementation_file *stream)
{
   /* MMC Command: PREVENT ALLOW MEDIUM REMOVAL */
   unsigned char cdb[] = {0x1E, 0, 0, 0, 0x2, 0};
   int rv = cdrom_send_command(stream, DIRECTION_NONE, NULL, 0, cdb, sizeof(cdb), 0);

#ifdef CDROM_DEBUG
   printf("persistent prevent clear status code %d\n", rv);
   fflush(stdout);
#endif

   if (rv)
      return 1;

   cdb[4] = 0x0;

   rv = cdrom_send_command(stream, DIRECTION_NONE, NULL, 0, cdb, sizeof(cdb), 0);

#ifdef CDROM_DEBUG
   printf("prevent clear status code %d\n", rv);
   fflush(stdout);
#endif

   if (rv)
      return 1;

   return 0;
}

int cdrom_open_tray(const libretro_vfs_implementation_file *stream)
{
   /* MMC Command: START STOP UNIT */
   unsigned char cdb[] = {0x1B, 0, 0, 0, 0x2, 0};
   int rv;

   cdrom_unlock(stream);
   cdrom_stop(stream);

   rv = cdrom_send_command(stream, DIRECTION_NONE, NULL, 0, cdb, sizeof(cdb), 0);

#ifdef CDROM_DEBUG
   printf("open tray status code %d\n", rv);
   fflush(stdout);
#endif

   if (rv)
      return 1;

   return 0;
}

int cdrom_close_tray(const libretro_vfs_implementation_file *stream)
{
   /* MMC Command: START STOP UNIT */
   unsigned char cdb[] = {0x1B, 0, 0, 0, 0x3, 0};
   int rv = cdrom_send_command(stream, DIRECTION_NONE, NULL, 0, cdb, sizeof(cdb), 0);

#ifdef CDROM_DEBUG
   printf("close tray status code %d\n", rv);
   fflush(stdout);
#endif

   if (rv)
      return 1;

   return 0;
}

struct string_list* cdrom_get_available_drives(void)
{
   struct string_list *list = string_list_new();
#if defined(__linux__) && !defined(ANDROID)
   struct string_list *dir_list = dir_list_new("/dev", NULL, false, false, false, false);
   int drive_index = 0;
   int i;

   if (!dir_list)
      return list;

   for (i = 0; i < dir_list->size; i++)
   {
      if (strstr(dir_list->elems[i].data, "/dev/sg"))
      {
         char drive_model[32] = {0};
         char drive_string[64] = {0};
         union string_list_elem_attr attr = {0};
         int dev_index = 0;
         RFILE *file = filestream_open(dir_list->elems[i].data, RETRO_VFS_FILE_ACCESS_READ, 0);
         const libretro_vfs_implementation_file *stream;
         bool is_cdrom = false;

         if (!file)
            continue;

         stream = filestream_get_vfs_handle(file);
         cdrom_get_inquiry(stream, drive_model, sizeof(drive_model), &is_cdrom);
         filestream_close(file);

         if (!is_cdrom)
            continue;

         sscanf(dir_list->elems[i].data + strlen("/dev/sg"), "%d", &dev_index);

         attr.i = dev_index;

         snprintf(drive_string, sizeof(drive_string), "Drive %d: ", drive_index + 1);

         if (!string_is_empty(drive_model))
            strlcat(drive_string, drive_model, sizeof(drive_string));
         else
            strlcat(drive_string, "Unknown Drive", sizeof(drive_string));

         string_list_append(list, drive_string, attr);
         drive_index++;
      }
   }

   string_list_free(dir_list);
#endif
#if defined(_WIN32) && !defined(_XBOX)
   DWORD drive_mask = GetLogicalDrives();
   int i;
   int drive_index = 0;

   for (i = 0; i < sizeof(DWORD) * 8; i++)
   {
      char path[] = {"a:\\"};
      char cdrom_path[] = {"cdrom://a:/drive-track01.bin"};

      path[0] += i;
      cdrom_path[8] += i;

      /* this drive letter doesn't exist */
      if (!(drive_mask & (1 << i)))
         continue;

      if (GetDriveType(path) != DRIVE_CDROM)
         continue;
      else
      {
         char drive_model[32] = {0};
         char drive_string[64] = {0};
         union string_list_elem_attr attr = {0};
         RFILE *file = filestream_open(cdrom_path, RETRO_VFS_FILE_ACCESS_READ, 0);
         const libretro_vfs_implementation_file *stream;
         bool is_cdrom = false;

         if (!file)
            continue;

         stream = filestream_get_vfs_handle(file);
         cdrom_get_inquiry(stream, drive_model, sizeof(drive_model), &is_cdrom);
         filestream_close(file);

         if (!is_cdrom)
            continue;

         attr.i = path[0];

         snprintf(drive_string, sizeof(drive_string), "Drive %d: ", drive_index + 1);

         if (!string_is_empty(drive_model))
            strlcat(drive_string, drive_model, sizeof(drive_string));
         else
            strlcat(drive_string, "Unknown Drive", sizeof(drive_string));

         string_list_append(list, drive_string, attr);
         drive_index++;
      }
   }
#endif
   return list;
}

bool cdrom_is_media_inserted(const libretro_vfs_implementation_file *stream)
{
   /* MMC Command: TEST UNIT READY */
   unsigned char cdb[] = {0x00, 0, 0, 0, 0, 0};
   int rv = cdrom_send_command(stream, DIRECTION_NONE, NULL, 0, cdb, sizeof(cdb), 0);

#ifdef CDROM_DEBUG
   printf("media inserted status code %d\n", rv);
   fflush(stdout);
#endif

   /* Will also return false if the drive is simply not ready yet (tray open, disc spinning back up after tray closed etc).
    * Command will not block or wait for media to become ready. */
   if (rv)
      return false;

   return true;
}

