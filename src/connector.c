/*
 *   connector.c
 *   Copyright (C) 2019 David García Goñi <dagargo@gmail.com>
 *
 *   This file is part of Elektroid.
 *
 *   Elektroid is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Elektroid is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Elektroid. If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <math.h>
#include <endian.h>
#include <poll.h>
#include <zlib.h>
#include <libgen.h>
#include <json-glib/json-glib.h>
#include "connector.h"
#include "utils.h"
#include "sample.h"
#include "package.h"

#define KB 1024
#define BUFF_SIZE (4 * KB)
#define RING_BUFF_SIZE (256 * KB)
#define DATA_TRANSF_BLOCK_BYTES 0x2000
#define OS_TRANSF_BLOCK_BYTES 0x800
#define POLL_TIMEOUT 20
#define MAX_ZIP_SIZE (128 * 1024 * 1024)

#define AFMK1_ID 0x04
#define AKEYS_ID 0x06
#define ARMK1_ID 0x08
#define AHMK1_ID 0x0a
#define DTAKT_ID 0x0c
#define AFMK2_ID 0x0e
#define ARMK2_ID 0x10
#define DTONE_ID 0x14
#define AHMK2_ID 0x16
#define DKEYS_ID 0x1c
#define MOD_S_ID 0x19
#define MOD_C_ID 0x1b

#define FS_DATA_PRJ_PREFIX "/projects"
#define FS_DATA_SND_PREFIX "/soundbanks"
#define FS_SAMPLES_START_POS 5
#define FS_DATA_START_POS 18
#define FS_SAMPLES_SIZE_POS_W 21
#define FS_SAMPLES_LAST_FRAME_POS_W 33
#define FS_SAMPLES_PAD_RES 22
#define ELEKTRON_SAMPLE_INFO_PAD_I32_LEN 10

struct elektron_sample_info
{
  guint32 type;
  guint32 sample_len_bytes;
  guint32 sample_rate;
  guint32 loop_start;
  guint32 loop_end;
  guint32 loop_type;
  guint32 padding[ELEKTRON_SAMPLE_INFO_PAD_I32_LEN];
};

typedef GByteArray *(*connector_msg_id_func) (guint);

typedef GByteArray *(*connector_msg_id_len_func) (guint, guint);

typedef GByteArray *(*connector_msg_path_func) (const gchar *);

typedef GByteArray *(*connector_msg_path_len_func) (const gchar *, guint);

typedef GByteArray *(*connector_msg_read_blk_func) (guint, guint, guint);

typedef GByteArray *(*connector_msg_write_blk_func) (guint, GByteArray *,
						     guint *, guint, void *);

typedef void (*connector_copy_array) (GByteArray *, GByteArray *);

typedef gint (*connector_path_func) (struct connector *, const gchar *);

typedef gint (*connector_src_dst_func) (struct connector *, const gchar *,
					const gchar *);

static gint connector_delete_samples_dir (struct connector *, const gchar *);

static gint connector_read_samples_dir (struct item_iterator *, const gchar *,
					void *);

static gint connector_create_samples_dir (const gchar *, void *);

static gint connector_delete_samples_item (const gchar *, void *);

static gint connector_move_samples_item (const gchar *, const gchar *,
					 void *);

static gint connector_download_sample (const gchar *, GByteArray *,
				       struct job_control *, void *);

static gint connector_upload_sample (const gchar *, GByteArray *,
				     struct job_control *, void *);

static gint connector_delete_raw_dir (struct connector *, const gchar *);

static gint connector_read_raw_dir (struct item_iterator *, const gchar *,
				    void *);

static gint connector_create_raw_dir (const gchar *, void *);

static gint connector_delete_raw_item (const gchar *, void *);

static gint connector_move_raw_item (const gchar *, const gchar *, void *);

static gint connector_download_raw (const gchar *, GByteArray *,
				    struct job_control *, void *);

static gint connector_upload_raw (const gchar *, GByteArray *,
				  struct job_control *, void *);

static gint connector_read_data_dir_any (struct item_iterator *,
					 const gchar *, void *);

static gint connector_read_data_dir_prj (struct item_iterator *,
					 const gchar *, void *);

static gint connector_read_data_dir_snd (struct item_iterator *,
					 const gchar *, void *);

static gint connector_move_data_item_any (const gchar *, const gchar *,
					  void *);
static gint connector_move_data_item_prj (const gchar *, const gchar *,
					  void *);
static gint connector_move_data_item_snd (const gchar *, const gchar *,
					  void *);

static gint connector_copy_data_item_any (const gchar *, const gchar *,
					  void *);

static gint connector_copy_data_item_prj (const gchar *, const gchar *,
					  void *);

static gint connector_copy_data_item_snd (const gchar *, const gchar *,
					  void *);

static gint connector_clear_data_item_any (const gchar *, void *);

static gint connector_clear_data_item_prj (const gchar *, void *);

static gint connector_clear_data_item_snd (const gchar *, void *);

static gint connector_swap_data_item_any (const gchar *, const gchar *,
					  void *);

static gint connector_swap_data_item_prj (const gchar *, const gchar *,
					  void *);

static gint connector_swap_data_item_snd (const gchar *, const gchar *,
					  void *);

static gint connector_download_data_any (const gchar *, GByteArray *,
					 struct job_control *, void *);

static gint connector_download_data_snd_pkg (const gchar *, GByteArray *,
					     struct job_control *, void *);

static gint connector_download_data_prj_pkg (const gchar *, GByteArray *,
					     struct job_control *, void *);

static gint connector_upload_data_any (const gchar *, GByteArray *,
				       struct job_control *, void *);

static gint connector_upload_data_prj_pkg (const gchar *, GByteArray *,
					   struct job_control *, void *);

static gint connector_upload_data_snd_pkg (const gchar *, GByteArray *,
					   struct job_control *, void *);

static gint connector_copy_iterator (struct item_iterator *,
				     struct item_iterator *);

static const guint8 MSG_HEADER[] = { 0xf0, 0, 0x20, 0x3c, 0x10, 0 };

static const guint8 PING_REQUEST[] = { 0x1 };
static const guint8 SOFTWARE_VERSION_REQUEST[] = { 0x2 };
static const guint8 DEVICEUID_REQUEST[] = { 0x3 };
static const guint8 STORAGEINFO_REQUEST[] = { 0x5 };

static const guint8 FS_SAMPLE_READ_DIR_REQUEST[] = { 0x10 };
static const guint8 FS_SAMPLE_CREATE_DIR_REQUEST[] = { 0x11 };
static const guint8 FS_SAMPLE_DELETE_DIR_REQUEST[] = { 0x12 };
static const guint8 FS_SAMPLE_DELETE_FILE_REQUEST[] = { 0x20 };
static const guint8 FS_SAMPLE_RENAME_FILE_REQUEST[] = { 0x21 };
static const guint8 FS_SAMPLE_GET_FILE_INFO_FROM_HASH_AND_SIZE_REQUEST[] =
  { 0x23, 0, 0, 0, 0, 0, 0, 0, 0 };
static const guint8 FS_SAMPLE_OPEN_FILE_READER_REQUEST[] = { 0x30 };
static const guint8 FS_SAMPLE_CLOSE_FILE_READER_REQUEST[] = { 0x31 };
static const guint8 FS_SAMPLE_READ_FILE_REQUEST[] =
  { 0x32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const guint8 FS_SAMPLE_OPEN_FILE_WRITER_REQUEST[] =
  { 0x40, 0, 0, 0, 0 };
static const guint8 FS_SAMPLE_CLOSE_FILE_WRITER_REQUEST[] =
  { 0x41, 0, 0, 0, 0, 0, 0, 0, 0 };
static const guint8 FS_SAMPLE_WRITE_FILE_REQUEST[] =
  { 0x42, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static const guint8 FS_RAW_READ_DIR_REQUEST[] = { 0x14 };
static const guint8 FS_RAW_CREATE_DIR_REQUEST[] = { 0x15 };
static const guint8 FS_RAW_DELETE_DIR_REQUEST[] = { 0x16 };
static const guint8 FS_RAW_DELETE_FILE_REQUEST[] = { 0x24 };
static const guint8 FS_RAW_RENAME_FILE_REQUEST[] = { 0x25 };
static const guint8 FS_RAW_OPEN_FILE_READER_REQUEST[] = { 0x33 };
static const guint8 FS_RAW_CLOSE_FILE_READER_REQUEST[] = { 0x34 };
static const guint8 FS_RAW_READ_FILE_REQUEST[] =
  { 0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static const guint8 FS_RAW_OPEN_FILE_WRITER_REQUEST[] = { 0x43, 0, 0, 0, 0 };
static const guint8 FS_RAW_CLOSE_FILE_WRITER_REQUEST[] =
  { 0x44, 0, 0, 0, 0, 0, 0, 0, 0 };
static const guint8 FS_RAW_WRITE_FILE_REQUEST[] =
  { 0x45, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static const guint8 DATA_LIST_REQUEST[] = { 0x53 };
static const guint8 DATA_READ_OPEN_REQUEST[] = { 0x54 };
static const guint8 DATA_READ_PARTIAL_REQUEST[] = { 0x55 };
static const guint8 DATA_READ_CLOSE_REQUEST[] = { 0x56 };
static const guint8 DATA_WRITE_OPEN_REQUEST[] = { 0x57 };
static const guint8 DATA_WRITE_PARTIAL_REQUEST[] = { 0x58 };
static const guint8 DATA_WRITE_CLOSE_REQUEST[] = { 0x59 };
static const guint8 DATA_MOVE_REQUEST[] = { 0x5a };
static const guint8 DATA_COPY_REQUEST[] = { 0x5b };
static const guint8 DATA_CLEAR_REQUEST[] = { 0x5c };
static const guint8 DATA_SWAP_REQUEST[] = { 0x5d };
static const guint8 OS_UPGRADE_START_REQUEST[] =
  { 0x50, 0, 0, 0, 0, 's', 'y', 's', 'e', 'x', '\0', 1 };
static const guint8 OS_UPGRADE_WRITE_RESPONSE[] =
  { 0x51, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

static const gchar *FS_TYPE_NAMES[] = { "+Drive", "RAM" };

static const struct connector_device_desc ANALOG_FOUR_DESC = {
  .name = "Analog Four",
  .alias = "af",
  .id = AFMK1_ID,
  .fss = FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = 0
};

static const struct connector_device_desc ANALOG_KEYS_DESC = {
  .name = "Analog Keys",
  .alias = "ak",
  .id = AKEYS_ID,
  .fss = FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = 0
};

static const struct connector_device_desc ANALOG_RYTM_DESC = {
  .name = "Analog Rytm",
  .alias = "ar",
  .id = ARMK1_ID,
  .fss = FS_SAMPLES | FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = STORAGE_PLUS_DRIVE | STORAGE_RAM
};

static const struct connector_device_desc ANALOG_HEAT_DESC = {
  .name = "Analog Heat",
  .alias = "ah",
  .id = AHMK1_ID,
  .fss = FS_DATA_ALL | FS_DATA_SND,
  .storages = 0
};

static const struct connector_device_desc DIGITAKT_DESC = {
  .name = "Digitakt",
  .alias = "dt",
  .id = DTAKT_ID,
  .fss = FS_SAMPLES | FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = STORAGE_PLUS_DRIVE | STORAGE_RAM
};

static const struct connector_device_desc ANALOG_FOUR_MKII_DESC = {
  .name = "Analog Four MKII",
  .alias = "af",
  .id = AFMK2_ID,
  .fss = FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = 0
};

static const struct connector_device_desc ANALOG_RYTM_MKII_DESC = {
  .name = "Analog Rytm MKII",
  .alias = "ar",
  .id = ARMK2_ID,
  .fss = FS_SAMPLES | FS_DATA_ALL,
  .storages = STORAGE_PLUS_DRIVE | STORAGE_RAM
};

static const struct connector_device_desc DIGITONE_DESC = {
  .name = "Digitone",
  .alias = "dn",
  .id = DTONE_ID,
  .fss = FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = 0
};

static const struct connector_device_desc ANALOG_HEAT_MKII_DESC = {
  .name = "Analog Heat MKII",
  .alias = "ah",
  .id = AHMK2_ID,
  .fss = FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = 0
};

static const struct connector_device_desc DIGITONE_KEYS_DESC = {
  .name = "Digitone Keys",
  .alias = "dn",
  .id = DKEYS_ID,
  .fss = FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = 0
};

static const struct connector_device_desc MODEL_SAMPLES_DESC = {
  .name = "Model:Samples",
  .alias = "ms",
  .id = MOD_S_ID,
  .fss = FS_SAMPLES | FS_DATA_ALL | FS_DATA_PRJ | FS_DATA_SND,
  .storages = STORAGE_PLUS_DRIVE | STORAGE_RAM
};

static const struct connector_device_desc MODEL_CYCLES_DESC = {
  .name = "Model:Cycles",
  .alias = "mc",
  .id = MOD_C_ID,
  .fss = FS_RAW_ALL | FS_RAW_PRESETS | FS_DATA_ALL | FS_DATA_PRJ,
  .storages = STORAGE_PLUS_DRIVE
};

static const struct connector_device_desc *CONNECTOR_DEVICE_DESCS[] = {
  &ANALOG_FOUR_DESC, &ANALOG_KEYS_DESC, &ANALOG_RYTM_DESC, &ANALOG_HEAT_DESC,
  &DIGITAKT_DESC, &ANALOG_FOUR_MKII_DESC, &ANALOG_RYTM_MKII_DESC,
  &DIGITONE_DESC, &ANALOG_HEAT_MKII_DESC, &DIGITONE_KEYS_DESC,
  &MODEL_SAMPLES_DESC, &MODEL_CYCLES_DESC
};

static const struct fs_operations FS_SAMPLES_OPERATIONS = {
  .fs = FS_SAMPLES,
  .readdir = connector_read_samples_dir,
  .mkdir = connector_create_samples_dir,
  .delete = connector_delete_samples_item,
  .rename = connector_move_samples_item,
  .move = connector_move_samples_item,
  .copy = NULL,
  .clear = NULL,
  .swap = NULL,
  .download = connector_download_sample,
  .upload = connector_upload_sample,
  .getid = get_item_name,
  .load = sample_load,
  .save = sample_save,
  .extension = "wav"
};

static const struct fs_operations FS_RAW_ANY_OPERATIONS = {
  .fs = FS_RAW_ALL,
  .readdir = connector_read_raw_dir,
  .mkdir = connector_create_raw_dir,
  .delete = connector_delete_raw_item,
  .rename = connector_move_raw_item,
  .move = connector_move_raw_item,
  .copy = NULL,
  .clear = NULL,
  .swap = NULL,
  .download = connector_download_raw,
  .upload = connector_upload_raw,
  .getid = get_item_name,
  .load = load_file,
  .save = save_file,
  .extension = "raw"
};

static const struct fs_operations FS_RAW_PRESETS_OPERATIONS = {
  .fs = FS_RAW_PRESETS,
  .readdir = connector_read_raw_dir,
  .mkdir = connector_create_raw_dir,
  .delete = connector_delete_raw_item,
  .rename = connector_move_raw_item,
  .move = connector_move_raw_item,
  .copy = NULL,
  .clear = NULL,
  .swap = NULL,
  .download = connector_download_raw,
  .upload = connector_upload_raw,
  .getid = get_item_name,
  .load = load_file,
  .save = save_file,
  .extension = "snd"
};

static const struct fs_operations FS_DATA_ANY_OPERATIONS = {
  .fs = FS_DATA_ALL,
  .readdir = connector_read_data_dir_any,
  .mkdir = NULL,
  .delete = connector_clear_data_item_any,
  .rename = NULL,
  .move = connector_move_data_item_any,
  .copy = connector_copy_data_item_any,
  .clear = connector_clear_data_item_any,
  .swap = connector_swap_data_item_any,
  .download = connector_download_data_any,
  .upload = connector_upload_data_any,
  .getid = get_item_index,
  .load = load_file,
  .save = save_file,
  .extension = "data"
};

static const struct fs_operations FS_DATA_PRJ_OPERATIONS = {
  .fs = FS_DATA_PRJ,
  .readdir = connector_read_data_dir_prj,
  .mkdir = NULL,
  .delete = connector_clear_data_item_prj,
  .rename = NULL,
  .move = connector_move_data_item_prj,
  .copy = connector_copy_data_item_prj,
  .clear = connector_clear_data_item_prj,
  .swap = connector_swap_data_item_prj,
  .download = connector_download_data_prj_pkg,
  .upload = connector_upload_data_prj_pkg,
  .getid = get_item_index,
  .load = load_file,
  .save = save_file,
  .extension = "prj"
};

static const struct fs_operations FS_DATA_SND_OPERATIONS = {
  .fs = FS_DATA_SND,
  .readdir = connector_read_data_dir_snd,
  .mkdir = NULL,
  .delete = connector_clear_data_item_snd,
  .rename = NULL,
  .move = connector_move_data_item_snd,
  .copy = connector_copy_data_item_snd,
  .clear = connector_clear_data_item_snd,
  .swap = connector_swap_data_item_snd,
  .download = connector_download_data_snd_pkg,
  .upload = connector_upload_data_snd_pkg,
  .getid = get_item_index,
  .load = load_file,
  .save = save_file,
  .extension = "snd"
};

static const struct fs_operations *FS_OPERATIONS[] = {
  &FS_SAMPLES_OPERATIONS, &FS_RAW_ANY_OPERATIONS, &FS_RAW_PRESETS_OPERATIONS,
  &FS_DATA_ANY_OPERATIONS, &FS_DATA_PRJ_OPERATIONS, &FS_DATA_SND_OPERATIONS
};

static const int FS_OPERATIONS_N =
  sizeof (FS_OPERATIONS) / sizeof (struct fs_operations *);

static enum item_type connector_get_path_type (struct connector *,
					       const gchar *,
					       fs_init_iter_func);

static void
connector_free_iterator_data (void *iter_data)
{
  struct connector_iterator_data *data = iter_data;

  free_msg (data->msg);
  g_free (data);
}

const struct fs_operations *
connector_get_fs_operations (enum connector_fs fs)
{
  const struct fs_operations *fs_operations = NULL;
  for (int i = 0; i < FS_OPERATIONS_N; i++)
    {
      if (FS_OPERATIONS[i]->fs == fs)
	{
	  fs_operations = FS_OPERATIONS[i];
	  break;
	}
    }
  return fs_operations;
}

static inline gchar *
connector_get_utf8 (const gchar * s)
{
  return g_convert (s, -1, "UTF8", "CP1252", NULL, NULL, NULL);
}

static inline gchar *
connector_get_cp1252 (const gchar * s)
{
  return g_convert (s, -1, "CP1252", "UTF8", NULL, NULL, NULL);
}

static inline guint8
connector_get_msg_status (const GByteArray * msg)
{
  return msg->data[5];
}

static inline gchar *
connector_get_msg_string (const GByteArray * msg)
{
  return (gchar *) & msg->data[6];
}

static guint
connector_next_smplrw_entry (struct item_iterator *iter)
{
  guint32 *data32;
  gchar *name_cp1252;
  struct connector_iterator_data *data = iter->data;

  if (iter->item.name != NULL)
    {
      g_free (iter->item.name);
    }

  if (data->pos == data->msg->len)
    {
      iter->item.name = NULL;
      return -ENOENT;
    }
  else
    {
      data32 = (guint32 *) & data->msg->data[data->pos];
      data->hash = be32toh (*data32);
      data->pos += sizeof (guint32);

      data32 = (guint32 *) & data->msg->data[data->pos];
      iter->item.size = be32toh (*data32);
      data->pos += sizeof (guint32);

      data->pos++;		//write_protected

      iter->item.type = data->msg->data[data->pos];
      data->pos++;

      name_cp1252 = (gchar *) & data->msg->data[data->pos];
      iter->item.name = connector_get_utf8 (name_cp1252);
      if (data->fs == FS_RAW_ALL && iter->item.type == ELEKTROID_FILE)
	{
	  //This eliminates the extension ".mc-snd" that the device provides.
	  iter->item.name[strlen (iter->item.name) - 7] = 0;
	}
      data->pos += strlen (name_cp1252) + 1;

      iter->item.index = -1;

      return 0;
    }
}

static gint
connector_init_iterator (struct item_iterator *iter, GByteArray * msg,
			 iterator_next next, enum connector_fs fs)
{
  struct connector_iterator_data *data =
    malloc (sizeof (struct connector_iterator_data));

  data->msg = msg;
  data->pos = fs == FS_DATA_ALL ? FS_DATA_START_POS : FS_SAMPLES_START_POS;
  data->fs = fs;

  iter->data = data;
  iter->next = next;
  iter->free = connector_free_iterator_data;
  iter->copy = connector_copy_iterator;
  iter->item.name = NULL;

  return 0;
}

static gint
connector_copy_iterator (struct item_iterator *dst, struct item_iterator *src)
{
  struct connector_iterator_data *data = src->data;
  GByteArray *array = g_byte_array_sized_new (data->msg->len);
  g_byte_array_append (array, data->msg->data, data->msg->len);
  return connector_init_iterator (dst, array, src->next, data->fs);
}

static GByteArray *
connector_decode_payload (const GByteArray * src)
{
  GByteArray *dst;
  int i, j, k, dst_len;
  unsigned int shift;

  dst_len = src->len - ceill (src->len / 8.0);
  dst = g_byte_array_new ();
  g_byte_array_set_size (dst, dst_len);

  for (i = 0, j = 0; i < src->len; i += 8, j += 7)
    {
      shift = 0x40;
      for (k = 0; k < 7 && i + k + 1 < src->len; k++)
	{
	  dst->data[j + k] =
	    src->data[i + k + 1] | (src->data[i] & shift ? 0x80 : 0);
	  shift = shift >> 1;
	}
    }

  return dst;
}

static GByteArray *
connector_encode_payload (const GByteArray * src)
{
  GByteArray *dst;
  int i, j, k, dst_len;
  unsigned int accum;

  dst_len = src->len + ceill (src->len / 7.0);
  dst = g_byte_array_new ();
  g_byte_array_set_size (dst, dst_len);

  for (i = 0, j = 0; j < src->len; i += 8, j += 7)
    {
      accum = 0;
      for (k = 0; k < 7; k++)
	{
	  accum = accum << 1;
	  if (j + k < src->len)
	    {
	      if (src->data[j + k] & 0x80)
		{
		  accum |= 1;
		}
	      dst->data[i + k + 1] = src->data[j + k] & 0x7f;
	    }
	}
      dst->data[i] = accum;
    }

  return dst;
}

static GByteArray *
connector_msg_to_raw (const GByteArray * msg)
{
  GByteArray *encoded;
  GByteArray *sysex = g_byte_array_new ();

  g_byte_array_append (sysex, MSG_HEADER, sizeof (MSG_HEADER));
  encoded = connector_encode_payload (msg);
  g_byte_array_append (sysex, encoded->data, encoded->len);
  free_msg (encoded);
  g_byte_array_append (sysex, (guint8 *) "\xf7", 1);

  return sysex;
}

static gint
connector_get_smplrw_info_from_msg (GByteArray * info_msg, guint32 * id,
				    guint * size)
{
  if (connector_get_msg_status (info_msg))
    {
      if (id)
	{
	  *id = be32toh (*((guint32 *) & info_msg->data[6]));
	}
      if (size)
	{
	  *size = be32toh (*((guint32 *) & info_msg->data[10]));
	}
    }
  else
    {
      if (id)
	{
	  return -EIO;
	}
    }

  return 0;
}

static GByteArray *
connector_new_msg (const guint8 * data, guint len)
{
  GByteArray *msg = g_byte_array_new ();

  g_byte_array_append (msg, (guchar *) "\0\0\0\0", 4);
  g_byte_array_append (msg, data, len);

  return msg;
}

static GByteArray *
connector_new_msg_uint8 (const guint8 * data, guint len, guint8 type)
{
  GByteArray *msg = connector_new_msg (data, len);

  g_byte_array_append (msg, &type, 1);

  return msg;
}

static GByteArray *
connector_new_msg_path (const guint8 * data, guint len, const gchar * path)
{
  GByteArray *msg;
  gchar *path_cp1252 = connector_get_cp1252 (path);

  if (!path_cp1252)
    {
      return NULL;
    }

  msg = connector_new_msg (data, len);
  g_byte_array_append (msg, (guchar *) path_cp1252, strlen (path_cp1252) + 1);
  g_free (path_cp1252);

  return msg;
}

static GByteArray *
connector_new_msg_close_common_read (const guint8 * data, guint len, guint id)
{
  guint32 aux32;
  GByteArray *msg = connector_new_msg (data, len);

  aux32 = htobe32 (id);
  g_byte_array_append (msg, (guchar *) & aux32, sizeof (guint32));
  return msg;
}

static GByteArray *
connector_new_msg_close_sample_read (guint id)
{
  return
    connector_new_msg_close_common_read (FS_SAMPLE_CLOSE_FILE_READER_REQUEST,
					 sizeof
					 (FS_SAMPLE_CLOSE_FILE_READER_REQUEST),
					 id);
}

static GByteArray *
connector_new_msg_close_raw_read (guint id)
{
  return
    connector_new_msg_close_common_read (FS_RAW_CLOSE_FILE_READER_REQUEST,
					 sizeof
					 (FS_RAW_CLOSE_FILE_READER_REQUEST),
					 id);
}

static GByteArray *
connector_new_msg_open_common_write (const guint8 * data, guint len,
				     const gchar * path, guint bytes)
{
  guint32 aux32;
  GByteArray *msg = connector_new_msg_path (data, len, path);

  aux32 = htobe32 (bytes);
  memcpy (&msg->data[5], &aux32, sizeof (guint32));

  return msg;
}

static GByteArray *
connector_new_msg_open_sample_write (const gchar * path, guint bytes)
{
  return
    connector_new_msg_open_common_write (FS_SAMPLE_OPEN_FILE_WRITER_REQUEST,
					 sizeof
					 (FS_SAMPLE_OPEN_FILE_WRITER_REQUEST),
					 path,
					 bytes +
					 sizeof (struct
						 elektron_sample_info));
}

static GByteArray *
connector_new_msg_open_raw_write (const gchar * path, guint bytes)
{
  return connector_new_msg_open_common_write (FS_RAW_OPEN_FILE_WRITER_REQUEST,
					      sizeof
					      (FS_RAW_OPEN_FILE_WRITER_REQUEST),
					      path, bytes);
}


static GByteArray *
connector_new_msg_list (const gchar * path, int32_t start_index,
			int32_t end_index, gboolean all)
{
  guint32 aux32;
  guint8 aux8;
  GByteArray *msg = connector_new_msg_path (DATA_LIST_REQUEST,
					    sizeof (DATA_LIST_REQUEST),
					    path);

  aux32 = htobe32 (start_index);
  g_byte_array_append (msg, (guchar *) & aux32, sizeof (guint32));
  aux32 = htobe32 (end_index);
  g_byte_array_append (msg, (guchar *) & aux32, sizeof (guint32));
  aux8 = all;
  g_byte_array_append (msg, (guchar *) & aux8, sizeof (guint8));

  return msg;
}

static GByteArray *
connector_new_msg_write_sample_blk (guint id, GByteArray * sample,
				    guint * total, guint seq, void *data)
{
  guint32 aux32;
  guint16 aux16, *aux16p;
  int i, consumed, bytes_blk;
  struct sample_loop_data *sample_loop_data = data;
  struct elektron_sample_info elektron_sample_info;
  GByteArray *msg = connector_new_msg (FS_SAMPLE_WRITE_FILE_REQUEST,
				       sizeof (FS_SAMPLE_WRITE_FILE_REQUEST));


  aux32 = htobe32 (id);
  memcpy (&msg->data[5], &aux32, sizeof (guint32));
  aux32 = htobe32 (DATA_TRANSF_BLOCK_BYTES * seq);
  memcpy (&msg->data[13], &aux32, sizeof (guint32));

  bytes_blk = DATA_TRANSF_BLOCK_BYTES;
  consumed = 0;

  if (seq == 0)
    {
      elektron_sample_info.type = 0;
      elektron_sample_info.sample_len_bytes = htobe32 (sample->len);
      elektron_sample_info.sample_rate = htobe32 (ELEKTRON_SAMPLE_RATE);
      elektron_sample_info.loop_start = htobe32 (sample_loop_data->start);
      elektron_sample_info.loop_end = htobe32 (sample_loop_data->end);
      elektron_sample_info.loop_type = htobe32 (ELEKTRON_LOOP_TYPE);
      memset (&elektron_sample_info.padding, 0,
	      sizeof (guint32) * ELEKTRON_SAMPLE_INFO_PAD_I32_LEN);

      g_byte_array_append (msg, (guchar *) & elektron_sample_info,
			   sizeof (struct elektron_sample_info));

      consumed = sizeof (struct elektron_sample_info);
      bytes_blk -= consumed;
    }

  i = 0;
  aux16p = (guint16 *) & sample->data[*total];
  while (i < bytes_blk && *total < sample->len)
    {
      aux16 = htobe16 (*aux16p);
      g_byte_array_append (msg, (guint8 *) & aux16, sizeof (guint16));
      aux16p++;
      (*total) += sizeof (guint16);
      consumed += sizeof (guint16);
      i += sizeof (guint16);
    }

  aux32 = htobe32 (consumed);
  memcpy (&msg->data[9], &aux32, sizeof (guint32));

  return msg;
}

static GByteArray *
connector_new_msg_write_raw_blk (guint id, GByteArray * raw, guint * total,
				 guint seq, void *data)
{
  gint len;
  guint32 aux32;
  GByteArray *msg = connector_new_msg (FS_RAW_WRITE_FILE_REQUEST,
				       sizeof (FS_RAW_WRITE_FILE_REQUEST));

  aux32 = htobe32 (id);
  memcpy (&msg->data[5], &aux32, sizeof (guint32));
  aux32 = htobe32 (DATA_TRANSF_BLOCK_BYTES * seq);
  memcpy (&msg->data[13], &aux32, sizeof (guint32));

  len = raw->len - *total;
  len = len > DATA_TRANSF_BLOCK_BYTES ? DATA_TRANSF_BLOCK_BYTES : len;
  g_byte_array_append (msg, &raw->data[*total], len);
  (*total) += len;

  aux32 = htobe32 (len);
  memcpy (&msg->data[9], &aux32, sizeof (guint32));

  return msg;
}

static GByteArray *
connector_new_msg_close_common_write (const guint8 * data, guint len,
				      guint id, guint bytes)
{
  guint32 aux32;
  GByteArray *msg = connector_new_msg (data, len);

  aux32 = htobe32 (id);
  memcpy (&msg->data[5], &aux32, sizeof (guint32));
  aux32 = htobe32 (bytes);
  memcpy (&msg->data[9], &aux32, sizeof (guint32));

  return msg;
}

static GByteArray *
connector_new_msg_close_sample_write (guint id, guint bytes)
{
  return
    connector_new_msg_close_common_write (FS_SAMPLE_CLOSE_FILE_WRITER_REQUEST,
					  sizeof
					  (FS_SAMPLE_CLOSE_FILE_WRITER_REQUEST),
					  id,
					  bytes +
					  sizeof (struct
						  elektron_sample_info));
}

static GByteArray *
connector_new_msg_close_raw_write (guint id, guint bytes)
{
  return
    connector_new_msg_close_common_write (FS_RAW_CLOSE_FILE_WRITER_REQUEST,
					  sizeof
					  (FS_RAW_CLOSE_FILE_WRITER_REQUEST),
					  id, bytes);
}

static GByteArray *
connector_new_msg_read_common_blk (const guint8 * data, guint len, guint id,
				   guint start, guint size)
{
  guint32 aux;
  GByteArray *msg = connector_new_msg (data, len);

  aux = htobe32 (id);
  memcpy (&msg->data[5], &aux, sizeof (guint32));
  aux = htobe32 (size);
  memcpy (&msg->data[9], &aux, sizeof (guint32));
  aux = htobe32 (start);
  memcpy (&msg->data[13], &aux, sizeof (guint32));

  return msg;
}

static GByteArray *
connector_new_msg_read_sample_blk (guint id, guint start, guint size)
{
  return connector_new_msg_read_common_blk (FS_SAMPLE_READ_FILE_REQUEST,
					    sizeof
					    (FS_SAMPLE_READ_FILE_REQUEST), id,
					    start, size);
}

static GByteArray *
connector_new_msg_read_raw_blk (guint id, guint start, guint size)
{
  return connector_new_msg_read_common_blk (FS_RAW_READ_FILE_REQUEST,
					    sizeof (FS_RAW_READ_FILE_REQUEST),
					    id, start, size);
}

static GByteArray *
connector_raw_to_msg (GByteArray * sysex)
{
  GByteArray *msg;
  GByteArray *payload;
  gint len = sysex->len - sizeof (MSG_HEADER) - 1;

  if (len > 0)
    {
      payload = g_byte_array_new ();
      g_byte_array_append (payload, &sysex->data[sizeof (MSG_HEADER)], len);
      msg = connector_decode_payload (payload);
      free_msg (payload);
    }
  else
    {
      msg = NULL;
    }

  return msg;
}

static ssize_t
connector_tx_raw (struct connector *connector, const guint8 * data, guint len)
{
  ssize_t tx_len;

  if (!connector->outputp)
    {
      error_print ("Output port is NULL\n");
      return -ENOTCONN;
    }

  snd_rawmidi_read (connector->inputp, NULL, 0);	// trigger reading

  tx_len = snd_rawmidi_write (connector->outputp, data, len);
  if (tx_len < 0)
    {
      error_print ("Error while writing to device: %s\n",
		   snd_strerror (tx_len));
      connector_destroy (connector);
    }
  return tx_len;
}

gint
connector_tx_sysex (struct connector *connector,
		    struct connector_sysex_transfer *transfer)
{
  ssize_t tx_len;
  guint total;
  guint len;
  guchar *b;
  gint res = 0;

  transfer->status = SENDING;

  b = transfer->raw->data;
  total = 0;
  while (total < transfer->raw->len && transfer->active)
    {
      len = transfer->raw->len - total;
      if (len > BUFF_SIZE)
	{
	  len = BUFF_SIZE;
	}

      tx_len = connector_tx_raw (connector, b, len);
      if (tx_len < 0)
	{
	  res = tx_len;
	  break;
	}
      b += len;
      total += len;
    }

  transfer->active = FALSE;
  transfer->status = FINISHED;
  return res;
}

static gint
connector_tx (struct connector *connector, const GByteArray * msg)
{
  gint res;
  guint16 aux;
  gchar *text;
  struct connector_sysex_transfer transfer;

  aux = htobe16 (connector->seq);
  memcpy (msg->data, &aux, sizeof (guint16));
  if (connector->seq == USHRT_MAX)
    {
      connector->seq = 0;
    }
  else
    {
      connector->seq++;
    }

  transfer.active = TRUE;
  transfer.raw = connector_msg_to_raw (msg);

  res = connector_tx_sysex (connector, &transfer);
  if (!res)
    {
      if (debug_level > 1)
	{
	  text = debug_get_hex_msg (transfer.raw);
	  debug_print (2, "Raw message sent (%d): %s\n", transfer.raw->len,
		       text);
	  free (text);
	}

      text = debug_get_hex_msg (msg);
      debug_print (1, "Message sent (%d): %s\n", msg->len, text);
      free (text);
    }

  free_msg (transfer.raw);
  return res;
}

void
connector_rx_drain (struct connector *connector)
{
  debug_print (2, "Draining buffer...\n");
  connector->rx_len = 0;
  snd_rawmidi_drain (connector->inputp);
}

static gboolean
connector_is_rt_msg (guint8 * data, guint len)
{
  guint i;
  guint8 *b;

  for (i = 0, b = data; i < len; i++, b++)
    {
      if (*b < 0xf8)		//Not System Real-Time Messages
	{
	  return FALSE;
	}
    }

  return TRUE;
}

static ssize_t
connector_rx_raw (struct connector *connector, guint8 * data, guint len,
		  struct connector_sysex_transfer *transfer)
{
  ssize_t rx_len;
  guint total_time;
  unsigned short revents;
  gint err;
  gchar *text;

  if (!connector->inputp)
    {
      error_print ("Input port is NULL\n");
      return -ENOTCONN;
    }

  total_time = 0;

  while (1)
    {
      err = poll (connector->pfds, connector->npfds, POLL_TIMEOUT);

      if (!transfer->active)
	{
	  return -ENODATA;
	}

      if (err == 0)
	{
	  total_time += POLL_TIMEOUT;
	  if (((transfer->batch && transfer->status == RECEIVING)
	       || !transfer->batch) && transfer->timeout > -1
	      && total_time >= transfer->timeout)
	    {
	      debug_print (1, "Timeout!\n");
	      return -ENODATA;
	    }
	  continue;
	}

      if (err < 0)
	{
	  error_print ("Error while polling. %s.\n", g_strerror (errno));
	  connector_destroy (connector);
	  return err;
	}

      if ((err =
	   snd_rawmidi_poll_descriptors_revents (connector->inputp,
						 connector->pfds,
						 connector->npfds,
						 &revents)) < 0)
	{
	  error_print ("Error while getting poll events. %s.\n",
		       snd_strerror (err));
	  connector_destroy (connector);
	  return err;
	}

      if (revents & (POLLERR | POLLHUP))
	{
	  return -ENODATA;
	}

      if (!(revents & POLLIN))
	{
	  continue;
	}

      rx_len = snd_rawmidi_read (connector->inputp, data, len);

      if (rx_len == -EAGAIN || rx_len == 0)
	{
	  continue;
	}

      if (rx_len > 0)
	{
	  if (connector_is_rt_msg (data, rx_len))
	    {
	      continue;
	    }
	  break;
	}

      if (rx_len < 0)
	{
	  error_print ("Error while reading from device: %s\n",
		       snd_strerror (rx_len));
	  connector_destroy (connector);
	  break;
	}

    }

  if (debug_level > 1)
    {
      text = debug_get_hex_data (3, data, rx_len);
      debug_print (2, "Buffer content (%zu): %s\n", rx_len, text);
      free (text);
    }

  return rx_len;
}

gint
connector_rx_sysex (struct connector *connector,
		    struct connector_sysex_transfer *transfer)
{
  gint i;
  guint8 *b;
  gint res = 0;

  transfer->status = WAITING;
  transfer->raw = g_byte_array_new ();

  i = 0;
  if (connector->rx_len < 0)
    {
      connector->rx_len = 0;
    }
  b = connector->buffer;

  while (1)
    {
      if (i == connector->rx_len)
	{
	  connector->rx_len =
	    connector_rx_raw (connector, connector->buffer, BUFF_SIZE,
			      transfer);

	  if (connector->rx_len == -ENODATA)
	    {
	      res = -ENODATA;
	      goto error;
	    }

	  if (connector->rx_len < 0)
	    {
	      res = -EIO;
	      goto error;
	    }

	  b = connector->buffer;
	  i = 0;
	}

      while (i < connector->rx_len && *b != 0xf0)
	{
	  b++;
	  i++;
	}

      if (i < connector->rx_len)
	{
	  break;
	}
    }

  g_byte_array_append (transfer->raw, b, 1);
  b++;
  i++;
  transfer->status = RECEIVING;

  while (1)
    {
      if (i == connector->rx_len)
	{
	  connector->rx_len =
	    connector_rx_raw (connector, connector->buffer, BUFF_SIZE,
			      transfer);

	  if (connector->rx_len == -ENODATA && transfer->batch)
	    {
	      break;
	    }

	  if (connector->rx_len < 0)
	    {
	      res = -EIO;
	      goto error;
	    }

	  b = connector->buffer;
	  i = 0;
	}

      while (i < connector->rx_len && (*b != 0xf7 || transfer->batch))
	{
	  if (!connector_is_rt_msg (b, 1))
	    {
	      g_byte_array_append (transfer->raw, b, 1);
	    }
	  b++;
	  i++;
	}

      if (i < connector->rx_len)
	{
	  g_byte_array_append (transfer->raw, b, 1);
	  connector->rx_len = connector->rx_len - i - 1;
	  if (connector->rx_len > 0)
	    {
	      memmove (connector->buffer, &connector->buffer[i + 1],
		       connector->rx_len);
	    }
	  break;
	}

    }

  goto end;

error:
  free_msg (transfer->raw);
  transfer->raw = NULL;
end:
  transfer->active = FALSE;
  transfer->status = FINISHED;
  return res;
}

static GByteArray *
connector_rx (struct connector *connector)
{
  gchar *text;
  GByteArray *msg;
  struct connector_sysex_transfer transfer;

  transfer.active = TRUE;
  transfer.timeout = SYSEX_TIMEOUT;
  transfer.batch = FALSE;

  if (connector_rx_sysex (connector, &transfer))
    {
      return NULL;
    }
  while (transfer.raw->len < 12
	 || (transfer.raw->len >= 12
	     && (transfer.raw->data[0] != MSG_HEADER[0]
		 || transfer.raw->data[1] != MSG_HEADER[1]
		 || transfer.raw->data[2] != MSG_HEADER[2]
		 || transfer.raw->data[3] != MSG_HEADER[3]
		 || transfer.raw->data[4] != MSG_HEADER[4]
		 || transfer.raw->data[5] != MSG_HEADER[5])))
    {
      if (debug_level > 1)
	{
	  text = debug_get_hex_msg (transfer.raw);
	  debug_print (2, "Message skipped (%d): %s\n", transfer.raw->len,
		       text);
	  free (text);
	}
      free_msg (transfer.raw);

      transfer.active = TRUE;
      if (connector_rx_sysex (connector, &transfer))
	{
	  return NULL;
	}
    }

  if (debug_level > 1)
    {
      text = debug_get_hex_msg (transfer.raw);
      debug_print (2, "Raw message received (%d): %s\n", transfer.raw->len,
		   text);
      free (text);
    }

  msg = connector_raw_to_msg (transfer.raw);
  if (msg)
    {
      text = debug_get_hex_msg (msg);
      debug_print (1, "Message received (%d): %s\n", msg->len, text);
      free (text);
    }

  free_msg (transfer.raw);
  return msg;
}

static GByteArray *
connector_tx_and_rx (struct connector *connector, GByteArray * tx_msg)
{
  ssize_t len;
  GByteArray *rx_msg;

  g_mutex_lock (&connector->mutex);

  connector_rx_drain (connector);

  len = connector_tx (connector, tx_msg);
  if (len < 0)
    {
      rx_msg = NULL;
      goto cleanup;
    }

  rx_msg = connector_rx (connector);

cleanup:
  free_msg (tx_msg);
  g_mutex_unlock (&connector->mutex);
  return rx_msg;
}

static gint
connector_read_common_dir (struct item_iterator *iter, const gchar * dir,
			   void *data, const guint8 msg[], int size,
			   fs_init_iter_func init_iter, enum connector_fs fs)
{
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  struct connector *connector = data;

  tx_msg = connector_new_msg_path (msg, size, dir);
  if (!tx_msg)
    {
      return -EINVAL;
    }

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }

  if (rx_msg->len == 5
      && connector_get_path_type (connector, dir, init_iter) != ELEKTROID_DIR)
    {
      free_msg (rx_msg);
      return -ENOTDIR;
    }

  return connector_init_iterator (iter, rx_msg, connector_next_smplrw_entry,
				  fs);
}

static gint
connector_read_samples_dir (struct item_iterator *iter, const gchar * dir,
			    void *data)
{
  return connector_read_common_dir (iter, dir, data,
				    FS_SAMPLE_READ_DIR_REQUEST,
				    sizeof (FS_SAMPLE_READ_DIR_REQUEST),
				    connector_read_samples_dir, FS_SAMPLES);
}

static gint
connector_read_raw_dir (struct item_iterator *iter, const gchar * dir,
			void *data)
{
  return connector_read_common_dir (iter, dir, data, FS_RAW_READ_DIR_REQUEST,
				    sizeof (FS_RAW_READ_DIR_REQUEST),
				    connector_read_raw_dir, FS_RAW_ALL);
}

static enum item_type
connector_get_path_type (struct connector *connector, const gchar * path,
			 fs_init_iter_func init_iter)
{
  gchar *name_copy;
  gchar *parent_copy;
  gchar *name;
  gchar *parent;
  enum item_type res;
  struct item_iterator iter;

  if (strcmp (path, "/") == 0)
    {
      return ELEKTROID_DIR;
    }

  name_copy = strdup (path);
  parent_copy = strdup (path);
  name = basename (name_copy);
  parent = dirname (parent_copy);
  res = ELEKTROID_NONE;
  if (!init_iter (&iter, parent, connector))
    {
      while (!next_item_iterator (&iter))
	{
	  if (strcmp (name, iter.item.name) == 0)
	    {
	      res = iter.item.type;
	      break;
	    }
	}
      free_item_iterator (&iter);
    }

  g_free (name_copy);
  g_free (parent_copy);
  return res;
}

static gint
connector_src_dst_common (struct connector *connector,
			  const gchar * src, const gchar * dst,
			  const guint8 * data, guint len)
{
  gint res;
  GByteArray *rx_msg;
  GByteArray *tx_msg = connector_new_msg (data, len);

  gchar *dst_cp1252 = connector_get_cp1252 (dst);
  if (!dst_cp1252)
    {
      return -EINVAL;
    }

  gchar *src_cp1252 = connector_get_cp1252 (src);
  if (!src_cp1252)
    {
      g_free (dst_cp1252);
      return -EINVAL;
    }

  g_byte_array_append (tx_msg, (guchar *) src_cp1252,
		       strlen (src_cp1252) + 1);
  g_byte_array_append (tx_msg, (guchar *) dst_cp1252,
		       strlen (dst_cp1252) + 1);

  g_free (src_cp1252);
  g_free (dst_cp1252);

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }
  //Response: x, x, x, x, 0xa1, [0 (error), 1 (success)]...
  if (connector_get_msg_status (rx_msg))
    {
      res = 0;
    }
  else
    {
      res = -EPERM;
      error_print ("%s (%s)\n", snd_strerror (res),
		   connector_get_msg_string (rx_msg));
    }
  free_msg (rx_msg);

  return res;
}

static gint
connector_rename_sample_file (struct connector *connector, const gchar * src,
			      const gchar * dst)
{
  return connector_src_dst_common (connector, src, dst,
				   FS_SAMPLE_RENAME_FILE_REQUEST,
				   sizeof (FS_SAMPLE_RENAME_FILE_REQUEST));
}

static gint
connector_rename_raw_file (struct connector *connector, const gchar * src,
			   const gchar * dst)
{
  return connector_src_dst_common (connector, src, dst,
				   FS_RAW_RENAME_FILE_REQUEST,
				   sizeof (FS_RAW_RENAME_FILE_REQUEST));
}

static gint
connector_move_common_item (const gchar * src, const gchar * dst, void *data,
			    fs_init_iter_func init_iter,
			    connector_src_dst_func mv, fs_path_func mkdir,
			    connector_path_func rmdir)
{
  enum item_type type;
  gint res;
  gchar *src_plus;
  gchar *dst_plus;
  struct item_iterator iter;
  struct connector *connector = data;

  //Renaming is not implemented for directories so we need to implement it.

  debug_print (1, "Renaming remotely from %s to %s...\n", src, dst);

  type = connector_get_path_type (connector, src, init_iter);
  if (type == ELEKTROID_FILE)
    {
      return mv (connector, src, dst);
    }
  else if (type == ELEKTROID_DIR)
    {
      res = mkdir (dst, connector);
      if (res)
	{
	  return res;
	}
      if (!init_iter (&iter, src, connector))
	{
	  while (!next_item_iterator (&iter) && !res)
	    {
	      src_plus = chain_path (src, iter.item.name);
	      dst_plus = chain_path (dst, iter.item.name);
	      res =
		connector_move_common_item (src_plus, dst_plus, connector,
					    init_iter, mv, mkdir, rmdir);
	      free (src_plus);
	      free (dst_plus);
	    }
	  free_item_iterator (&iter);
	}
      if (!res)
	{
	  res = rmdir (connector, src);
	}
      return res;
    }
  else
    {
      return -EBADF;
    }
}

static gint
connector_path_common (struct connector *connector, const gchar * path,
		       const guint8 * template, gint size)
{
  gint res;
  GByteArray *rx_msg;
  GByteArray *tx_msg;

  tx_msg = connector_new_msg_path (template, size, path);
  if (!tx_msg)
    {
      return -EINVAL;
    }

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }
  //Response: x, x, x, x, 0xX0, [0 (error), 1 (success)]...
  if (connector_get_msg_status (rx_msg))
    {
      res = 0;
    }
  else
    {
      res = -EPERM;
      error_print ("%s (%s)\n", snd_strerror (res),
		   connector_get_msg_string (rx_msg));
    }
  free_msg (rx_msg);

  return res;
}

static gint
connector_delete_sample (struct connector *connector, const gchar * path)
{
  return connector_path_common (connector, path,
				FS_SAMPLE_DELETE_FILE_REQUEST,
				sizeof (FS_SAMPLE_DELETE_FILE_REQUEST));
}

static gint
connector_delete_samples_dir (struct connector *connector, const gchar * path)
{
  return connector_path_common (connector, path, FS_SAMPLE_DELETE_DIR_REQUEST,
				sizeof (FS_SAMPLE_DELETE_DIR_REQUEST));
}

//This adds back the extension ".mc-snd" that the device provides.
static gchar *
connector_add_ext_to_mc_snd (const gchar * path)
{
  gchar *path_with_ext = malloc (PATH_MAX);
  snprintf (path_with_ext, PATH_MAX, "%s%s", path, ".mc-snd");
  return path_with_ext;
}

static gint
connector_delete_raw (struct connector *connector, const gchar * path)
{
  gint ret;
  gchar *path_with_ext = connector_add_ext_to_mc_snd (path);
  ret = connector_path_common (connector, path_with_ext,
			       FS_RAW_DELETE_FILE_REQUEST,
			       sizeof (FS_RAW_DELETE_FILE_REQUEST));
  g_free (path_with_ext);
  return ret;
}

static gint
connector_delete_raw_dir (struct connector *connector, const gchar * path)
{
  return connector_path_common (connector, path, FS_RAW_DELETE_DIR_REQUEST,
				sizeof (FS_RAW_DELETE_DIR_REQUEST));
}

static gint
connector_move_samples_item (const gchar * src, const gchar * dst, void *data)
{
  return connector_move_common_item (src, dst, data,
				     connector_read_samples_dir,
				     connector_rename_sample_file,
				     connector_create_samples_dir,
				     connector_delete_samples_dir);
}

static gint
connector_move_raw_item (const gchar * src, const gchar * dst, void *data)
{
  gint ret;
  gchar *src_with_ext = connector_add_ext_to_mc_snd (src);
  ret = connector_move_common_item (src_with_ext, dst, data,
				    connector_read_raw_dir,
				    connector_rename_raw_file,
				    connector_create_raw_dir,
				    connector_delete_raw_dir);
  g_free (src_with_ext);
  return ret;
}

static gint
connector_delete_common_item (const gchar * path, void *data,
			      fs_init_iter_func init_iter,
			      connector_path_func rmdir,
			      connector_path_func rm)
{
  gchar *new_path;
  struct item_iterator iter;
  struct connector *connector = data;
  gint res;

  if (connector_get_path_type (connector, path, init_iter) == ELEKTROID_DIR)
    {
      debug_print (1, "Deleting %s samples dir...\n", path);

      if (init_iter (&iter, path, connector))
	{
	  error_print ("Error while opening samples dir %s dir\n", path);
	}
      else
	{
	  res = 0;
	  while (!res && !next_item_iterator (&iter))
	    {
	      new_path = chain_path (path, iter.item.name);
	      res = res
		|| connector_delete_common_item (new_path, connector,
						 init_iter, rmdir, rm);
	      free (new_path);
	    }
	  free_item_iterator (&iter);
	}
      return res || rmdir (connector, path);
    }
  else
    {
      return rm (connector, path);
    }
}

static gint
connector_delete_samples_item (const gchar * path, void *data)
{
  return connector_delete_common_item (path, data,
				       connector_read_samples_dir,
				       connector_delete_samples_dir,
				       connector_delete_sample);
}

static gint
connector_delete_raw_item (const gchar * path, void *data)
{
  return connector_delete_common_item (path, data,
				       connector_read_raw_dir,
				       connector_delete_raw_dir,
				       connector_delete_raw);
}

static gint
connector_upload_smplrw (const gchar * path, GByteArray * input,
			 struct job_control *control, void *data,
			 connector_msg_path_len_func new_msg_open_write,
			 connector_msg_write_blk_func new_msg_write_blk,
			 connector_msg_id_len_func new_msg_close_write)
{
  struct connector *connector = data;
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  guint transferred;
  guint32 id;
  int i;
  gboolean active;
  gint res = 0;

  //If the file already exists the device makes no difference between creating a new file and creating an already existent file.
  //Also, the new file would be discarded if an upload is not completed.

  tx_msg = new_msg_open_write (path, input->len);
  if (!tx_msg)
    {
      return -EINVAL;
    }

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }

  //Response: x, x, x, x, 0xc0, [0 (error), 1 (success)], id, frames
  res = connector_get_smplrw_info_from_msg (rx_msg, &id, NULL);
  if (res)
    {
      error_print ("%s (%s)\n", snd_strerror (res),
		   connector_get_msg_string (rx_msg));
      free_msg (rx_msg);
      return res;
    }
  free_msg (rx_msg);

  transferred = 0;
  i = 0;
  if (control)
    {
      g_mutex_lock (&control->mutex);
      active = control->active;
      g_mutex_unlock (&control->mutex);
    }
  else
    {
      active = TRUE;
    }

  while (transferred < input->len && active)
    {
      tx_msg = new_msg_write_blk (id, input, &transferred, i, control->data);
      rx_msg = connector_tx_and_rx (connector, tx_msg);
      if (!rx_msg)
	{
	  return -EIO;
	}
      //Response: x, x, x, x, 0xc2, [0 (error), 1 (success)]...
      if (!connector_get_msg_status (rx_msg))
	{
	  error_print ("Unexpected status\n");
	}
      free_msg (rx_msg);
      i++;

      if (control)
	{
	  set_job_control_progress (control,
				    transferred / (double) input->len);
	  g_mutex_lock (&control->mutex);
	  active = control->active;
	  g_mutex_unlock (&control->mutex);
	}

      usleep (REST_TIME);
    }

  debug_print (2, "%d bytes sent\n", transferred);

  if (active)
    {
      tx_msg = new_msg_close_write (id, transferred);
      rx_msg = connector_tx_and_rx (connector, tx_msg);
      if (!rx_msg)
	{
	  return -EIO;
	}
      //Response: x, x, x, x, 0xc1, [0 (error), 1 (success)]...
      if (!connector_get_msg_status (rx_msg))
	{
	  error_print ("Unexpected status\n");
	}
      free_msg (rx_msg);
    }

  return res;
}

static gint
connector_upload_sample_part (const gchar * path, GByteArray * sample,
			      struct job_control *control, void *data)
{
  return connector_upload_smplrw (path, sample, control, data,
				  connector_new_msg_open_sample_write,
				  connector_new_msg_write_sample_blk,
				  connector_new_msg_close_sample_write);
}

static gint
connector_upload_sample (const gchar * path, GByteArray * output,
			 struct job_control *control, void *data)
{
  control->parts = 1;
  control->part = 0;
  return connector_upload_sample_part (path, output, control, data);
}

static gint
connector_upload_raw (const gchar * path, GByteArray * sample,
		      struct job_control *control, void *data)
{
  return connector_upload_smplrw (path, sample, control, data,
				  connector_new_msg_open_raw_write,
				  connector_new_msg_write_raw_blk,
				  connector_new_msg_close_raw_write);
}

static GByteArray *
connector_new_msg_open_sample_read (const gchar * path)
{
  return connector_new_msg_path (FS_SAMPLE_OPEN_FILE_READER_REQUEST,
				 sizeof
				 (FS_SAMPLE_OPEN_FILE_READER_REQUEST), path);
}

static GByteArray *
connector_new_msg_open_raw_read (const gchar * path)
{
  return connector_new_msg_path (FS_RAW_OPEN_FILE_READER_REQUEST,
				 sizeof
				 (FS_RAW_OPEN_FILE_READER_REQUEST), path);
}

static void
connector_copy_sample_data (GByteArray * input, GByteArray * output)
{
  gint i;
  gint16 v;
  gint16 *frame = (gint16 *) input->data;

  for (i = 0; i < input->len; i += sizeof (gint16))
    {
      v = be16toh (*frame);
      g_byte_array_append (output, (guint8 *) & v, sizeof (gint16));
      frame++;
    }
}

static void
connector_copy_raw_data (GByteArray * input, GByteArray * output)
{
  g_byte_array_append (output, input->data, input->len);
}

static gint
connector_download_smplrw (const gchar * path, GByteArray * output,
			   struct job_control *control, void *data,
			   connector_msg_path_func new_msg_open_read,
			   guint read_offset,
			   connector_msg_read_blk_func new_msg_read_blk,
			   connector_msg_id_func new_msg_close_read,
			   connector_copy_array copy_array)
{
  struct connector *connector = data;
  struct sample_loop_data *sample_loop_data;
  struct elektron_sample_info *elektron_sample_info;
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  GByteArray *array;
  guint32 id;
  guint frames;
  guint next_block_start;
  guint req_size;
  guint offset;
  gboolean active;
  gint res;

  tx_msg = new_msg_open_read (path);
  if (!tx_msg)
    {
      return -EINVAL;
    }

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }
  res = connector_get_smplrw_info_from_msg (rx_msg, &id, &frames);
  if (res)
    {
      error_print ("%s (%s)\n", snd_strerror (res),
		   connector_get_msg_string (rx_msg));
      free_msg (rx_msg);
      return res;
    }
  free_msg (rx_msg);

  debug_print (2, "%d frames to download\n", frames);

  if (control)
    {
      g_mutex_lock (&control->mutex);
      active = control->active;
      g_mutex_unlock (&control->mutex);
    }
  else
    {
      active = TRUE;
    }

  array = g_byte_array_new ();
  res = 0;
  next_block_start = 0;
  offset = read_offset;
  control->data = NULL;
  while (next_block_start < frames && active)
    {
      req_size =
	frames - next_block_start >
	DATA_TRANSF_BLOCK_BYTES ? DATA_TRANSF_BLOCK_BYTES : frames -
	next_block_start;
      tx_msg = new_msg_read_blk (id, next_block_start, req_size);
      rx_msg = connector_tx_and_rx (connector, tx_msg);
      if (!rx_msg)
	{
	  res = -EIO;
	  goto cleanup;
	}
      g_byte_array_append (array, &rx_msg->data[FS_SAMPLES_PAD_RES + offset],
			   req_size - offset);

      next_block_start += req_size;
      //Only in the first iteration. It has no effect for the raw filesystem (M:C) as offset is 0.
      if (offset)
	{
	  offset = 0;
	  elektron_sample_info =
	    (struct elektron_sample_info *) &rx_msg->data[FS_SAMPLES_PAD_RES];
	  sample_loop_data = malloc (sizeof (struct elektron_sample_info));
	  sample_loop_data->start =
	    be32toh (elektron_sample_info->loop_start);
	  sample_loop_data->end = be32toh (elektron_sample_info->loop_end);
	  control->data = sample_loop_data;
	  debug_print (2, "Loop start at %d, loop end at %d\n",
		       sample_loop_data->start, sample_loop_data->end);
	}

      free_msg (rx_msg);

      if (control)
	{
	  set_job_control_progress (control,
				    next_block_start / (double) frames);
	  g_mutex_lock (&control->mutex);
	  active = control->active;
	  g_mutex_unlock (&control->mutex);
	}

      usleep (REST_TIME);
    }

  debug_print (2, "%d bytes received\n", next_block_start);

  if (active)
    {
      copy_array (array, output);
    }
  else
    {
      res = -1;
    }

  tx_msg = new_msg_close_read (id);
  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      res = -EIO;
      goto cleanup;
    }
  //Response: x, x, x, x, 0xb1, 00 00 00 0a 00 01 65 de (sample id and received bytes)
  free_msg (rx_msg);

cleanup:
  free_msg (array);
  if (res)
    {
      g_free (control->data);
    }
  return res;
}

static gint
connector_download_sample_part (const gchar * path, GByteArray * output,
				struct job_control *control, void *data)
{
  return connector_download_smplrw (path, output, control, data,
				    connector_new_msg_open_sample_read,
				    sizeof
				    (struct elektron_sample_info),
				    connector_new_msg_read_sample_blk,
				    connector_new_msg_close_sample_read,
				    connector_copy_sample_data);
}

static gint
connector_download_sample (const gchar * path, GByteArray * output,
			   struct job_control *control, void *data)
{
  control->parts = 1;
  control->part = 0;
  return connector_download_sample_part (path, output, control, data);
}

static gint
connector_download_raw (const gchar * path, GByteArray * output,
			struct job_control *control, void *data)
{
  gint ret;
  gchar *path_with_ext = connector_add_ext_to_mc_snd (path);
  ret = connector_download_smplrw (path_with_ext, output, control, data,
				   connector_new_msg_open_raw_read,
				   0,
				   connector_new_msg_read_raw_blk,
				   connector_new_msg_close_raw_read,
				   connector_copy_raw_data);
  g_free (path_with_ext);
  return ret;
}

static gint
connector_create_samples_dir (const gchar * path, void *data)
{
  struct connector *connector = data;
  return connector_path_common (connector, path, FS_SAMPLE_CREATE_DIR_REQUEST,
				sizeof (FS_SAMPLE_CREATE_DIR_REQUEST));
}

static gint
connector_create_raw_dir (const gchar * path, void *data)
{
  struct connector *connector = data;
  return connector_path_common (connector, path, FS_RAW_CREATE_DIR_REQUEST,
				sizeof (FS_RAW_CREATE_DIR_REQUEST));
}

static GByteArray *
connector_new_msg_upgrade_os_start (guint size)
{
  GByteArray *msg = connector_new_msg (OS_UPGRADE_START_REQUEST,
				       sizeof (OS_UPGRADE_START_REQUEST));

  memcpy (&msg->data[5], &size, sizeof (guint32));

  return msg;
}

static GByteArray *
connector_new_msg_upgrade_os_write (GByteArray * os_data, gint * offset)
{
  GByteArray *msg = connector_new_msg (OS_UPGRADE_WRITE_RESPONSE,
				       sizeof (OS_UPGRADE_WRITE_RESPONSE));
  guint len;
  guint32 crc;
  guint32 aux32;

  if (*offset + OS_TRANSF_BLOCK_BYTES < os_data->len)
    {
      len = OS_TRANSF_BLOCK_BYTES;
    }
  else
    {
      len = os_data->len - *offset;
    }

  crc = crc32 (0xffffffff, &os_data->data[*offset], len);

  debug_print (2, "CRC: %0x\n", crc);

  aux32 = htobe32 (crc);
  memcpy (&msg->data[5], &aux32, sizeof (guint32));
  aux32 = htobe32 (len);
  memcpy (&msg->data[9], &aux32, sizeof (guint32));
  aux32 = htobe32 (*offset);
  memcpy (&msg->data[13], &aux32, sizeof (guint32));

  g_byte_array_append (msg, &os_data->data[*offset], len);

  *offset = *offset + len;

  return msg;
}

gint
connector_upgrade_os (struct connector *connector,
		      struct connector_sysex_transfer *transfer)
{
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  gint8 op;
  gint offset;
  gint res = 0;

  transfer->status = SENDING;

  tx_msg = connector_new_msg_upgrade_os_start (transfer->raw->len);
  rx_msg = connector_tx_and_rx (connector, tx_msg);

  if (!rx_msg)
    {
      res = -EIO;
      goto end;
    }
  //Response: x, x, x, x, 0xd1, [0 (ok), 1 (error)]...
  op = connector_get_msg_status (rx_msg);
  if (op)
    {
      res = -EIO;
      error_print ("%s (%s)\n", snd_strerror (res),
		   connector_get_msg_string (rx_msg));
      free_msg (rx_msg);
      goto end;
    }

  free_msg (rx_msg);

  offset = 0;
  while (offset < transfer->raw->len && transfer->active)
    {
      tx_msg = connector_new_msg_upgrade_os_write (transfer->raw, &offset);
      rx_msg = connector_tx_and_rx (connector, tx_msg);

      if (!rx_msg)
	{
	  res = -EIO;
	  break;
	}
      //Response: x, x, x, x, 0xd1, int32, [0..3]...
      op = rx_msg->data[9];
      if (op == 1)
	{
	  break;
	}
      else if (op > 1)
	{
	  res = -EIO;
	  error_print ("%s (%s)\n", snd_strerror (res),
		       connector_get_msg_string (rx_msg));
	  free_msg (rx_msg);
	  break;
	}

      free_msg (rx_msg);

      usleep (REST_TIME);
    }

end:
  transfer->active = FALSE;
  transfer->status = FINISHED;
  return res;
}

void
connector_destroy (struct connector *connector)
{
  int err;

  debug_print (1, "Destroying connector...\n");

  if (connector->inputp)
    {
      err = snd_rawmidi_close (connector->inputp);
      if (err)
	{
	  error_print ("Error while closing MIDI port: %s\n",
		       snd_strerror (err));
	}
      connector->inputp = NULL;
    }

  if (connector->outputp)
    {
      err = snd_rawmidi_close (connector->outputp);
      if (err)
	{
	  error_print ("Error while closing MIDI port: %s\n",
		       snd_strerror (err));
	}
      connector->outputp = NULL;
    }

  if (connector->device_name)
    {
      free (connector->device_name);
      free (connector->fw_version);
      free (connector->alias);
      connector->device_name = NULL;
    }

  if (connector->buffer)
    {
      free (connector->buffer);
      connector->buffer = NULL;
    }

  if (connector->pfds)
    {
      free (connector->pfds);
      connector->pfds = NULL;
    }
}

gint
connector_get_storage_stats (struct connector *connector,
			     enum connector_storage type,
			     struct connector_storage_stats *statfs)
{
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  gint8 op;
  guint64 *data;
  int index;
  gint res = 0;

  tx_msg = connector_new_msg_uint8 (STORAGEINFO_REQUEST,
				    sizeof (STORAGEINFO_REQUEST), type);
  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }

  op = connector_get_msg_status (rx_msg);
  if (!op)
    {
      error_print ("%s (%s)\n", snd_strerror (-EIO),
		   connector_get_msg_string (rx_msg));
      free_msg (rx_msg);
      return -EIO;
    }

  index = 0;
  for (int i = 0, storage = STORAGE_PLUS_DRIVE; storage <= STORAGE_RAM;
       i++, storage <<= 1)
    {
      if (storage == type)
	{
	  index = i;
	}
    }

  statfs->name = FS_TYPE_NAMES[index];
  data = (guint64 *) & rx_msg->data[6];
  statfs->bfree = be64toh (*data);
  data = (guint64 *) & rx_msg->data[14];
  statfs->bsize = be64toh (*data);

  free_msg (rx_msg);

  return res;
}

gdouble
connector_get_storage_stats_percent (struct connector_storage_stats *statfs)
{
  return (statfs->bsize - statfs->bfree) * 100.0 / statfs->bsize;
}

static const struct connector_device_desc *
connector_get_device_desc (guint8 id)
{
  guint total =
    sizeof (CONNECTOR_DEVICE_DESCS) / sizeof (struct connector_device_desc *);
  guint i;

  for (i = 0; i < total; i++)
    {
      if (id == CONNECTOR_DEVICE_DESCS[i]->id)
	{
	  return CONNECTOR_DEVICE_DESCS[i];
	}
    }
  return NULL;
}

gint
connector_init (struct connector *connector, gint card)
{
  int err;
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  snd_rawmidi_params_t *params;
  gchar name[32];
  sprintf (name, "hw:%d", card);
  connector->inputp = NULL;
  connector->outputp = NULL;
  connector->device_name = NULL;
  connector->buffer = NULL;
  connector->rx_len = 0;
  connector->pfds = NULL;
  if (card < 0)
    {
      debug_print (1, "Invalid card\n");
      err = -EINVAL;
      goto cleanup;
    }

  debug_print (1, "Initializing connector to '%s'...\n", name);
  if ((err =
       snd_rawmidi_open (&connector->inputp, &connector->outputp,
			 name, SND_RAWMIDI_NONBLOCK | SND_RAWMIDI_SYNC)) < 0)
    {
      error_print ("Error while opening MIDI port: %s\n", g_strerror (err));
      goto cleanup;
    }

  debug_print (1, "Setting blocking mode...\n");
  if ((err = snd_rawmidi_nonblock (connector->outputp, 0)) < 0)
    {
      error_print ("Error while setting blocking mode\n");
      goto cleanup;
    }
  if ((err = snd_rawmidi_nonblock (connector->inputp, 1)) < 0)
    {
      error_print ("Error while setting blocking mode\n");
      goto cleanup;
    }

  debug_print (1, "Stopping device...\n");
  if (snd_rawmidi_write (connector->outputp, "\xfc", 1) < 0)
    {
      error_print ("Error while stopping device\n");
    }

  connector->seq = 0;
  connector->device_name = malloc (LABEL_MAX);
  if (!connector->device_name)
    {
      goto cleanup;
    }

  connector->buffer = malloc (sizeof (guint8) * BUFF_SIZE);
  if (!connector->buffer)
    {
      goto cleanup;
    }

  connector->npfds = snd_rawmidi_poll_descriptors_count (connector->inputp);
  connector->pfds = malloc (connector->npfds * sizeof (struct pollfd));
  if (!connector->buffer)
    {
      goto cleanup;
    }
  snd_rawmidi_poll_descriptors (connector->inputp, connector->pfds,
				connector->npfds);
  err = snd_rawmidi_params_malloc (&params);
  if (err)
    {
      goto cleanup;
    }

  err = snd_rawmidi_params_current (connector->inputp, params);
  if (err)
    {
      goto cleanup_params;
    }

  err =
    snd_rawmidi_params_set_buffer_size (connector->inputp, params,
					RING_BUFF_SIZE);
  if (err)
    {
      goto cleanup_params;
    }

  err = snd_rawmidi_params (connector->inputp, params);
  if (err)
    {
      goto cleanup_params;
    }

  tx_msg = connector_new_msg (PING_REQUEST, sizeof (PING_REQUEST));
  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      err = -EIO;
      goto cleanup_params;
    }
  connector->alias = strdup ((gchar *) & rx_msg->data[7 + rx_msg->data[6]]);
  connector->device_desc = connector_get_device_desc (rx_msg->data[5]);
  free_msg (rx_msg);
  if (!connector->device_desc)
    {
      err = -ENODEV;
      goto cleanup_params;
    }

  tx_msg =
    connector_new_msg (SOFTWARE_VERSION_REQUEST,
		       sizeof (SOFTWARE_VERSION_REQUEST));
  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      err = -EIO;
      goto cleanup_params;
    }
  connector->fw_version = strdup ((gchar *) & rx_msg->data[10]);
  free_msg (rx_msg);

  if (debug_level > 1)
    {
      tx_msg =
	connector_new_msg (DEVICEUID_REQUEST, sizeof (DEVICEUID_REQUEST));
      rx_msg = connector_tx_and_rx (connector, tx_msg);
      if (rx_msg)
	{
	  debug_print (1, "UID: %x\n", *((guint32 *) & rx_msg->data[5]));
	  free_msg (rx_msg);
	}
    }

  snprintf (connector->device_name, LABEL_MAX, "%s %s (%s)",
	    connector->device_desc->name,
	    connector->fw_version, connector->alias);
  debug_print (1, "Connected to %s\n", connector->device_name);
  err = 0;

cleanup_params:
  snd_rawmidi_params_free (params);
cleanup:
  if (err)
    {
      connector_destroy (connector);
    }
  return err;
}

gboolean
connector_check (struct connector *connector)
{
  return (connector->inputp && connector->outputp);
}

static struct connector_system_device *
connector_get_system_device (snd_ctl_t * ctl, int card, int device)
{
  snd_rawmidi_info_t *info;
  const gchar *name;
  const gchar *sub_name;
  int subs, subs_in, subs_out;
  int sub;
  int err;
  struct connector_system_device *connector_system_device;

  snd_rawmidi_info_alloca (&info);
  snd_rawmidi_info_set_device (info, device);
  snd_rawmidi_info_set_stream (info, SND_RAWMIDI_STREAM_INPUT);
  err = snd_ctl_rawmidi_info (ctl, info);
  if (err >= 0)
    {
      subs_in = snd_rawmidi_info_get_subdevices_count (info);
    }
  else
    {
      subs_in = 0;
    }

  snd_rawmidi_info_set_stream (info, SND_RAWMIDI_STREAM_OUTPUT);
  err = snd_ctl_rawmidi_info (ctl, info);
  if (err >= 0)
    {
      subs_out = snd_rawmidi_info_get_subdevices_count (info);
    }
  else
    {
      subs_out = 0;
    }

  subs = subs_in > subs_out ? subs_in : subs_out;
  if (!subs)
    {
      return NULL;
    }

  if (subs_in <= 0 || subs_out <= 0)
    {
      return NULL;
    }

  sub = 0;
  snd_rawmidi_info_set_stream (info, sub < subs_in ?
			       SND_RAWMIDI_STREAM_INPUT :
			       SND_RAWMIDI_STREAM_OUTPUT);
  snd_rawmidi_info_set_subdevice (info, sub);
  err = snd_ctl_rawmidi_info (ctl, info);
  if (err < 0)
    {
      error_print ("Cannot get rawmidi information %d:%d:%d: %s\n",
		   card, device, sub, snd_strerror (err));
      return NULL;
    }

  name = snd_rawmidi_info_get_name (info);
  sub_name = snd_rawmidi_info_get_subdevice_name (info);
  if (strncmp (sub_name, "Elektron", 8) == 0)
    {
      debug_print (1, "Adding hw:%d (%s) %s...\n", card, name, sub_name);
      connector_system_device =
	malloc (sizeof (struct connector_system_device));
      connector_system_device->card = card;
      connector_system_device->name = strdup (sub_name);
      return connector_system_device;
    }
  else
    {
      return NULL;
    }
}

static void
connector_fill_card_elektron_devices (gint card, GArray * devices)
{
  snd_ctl_t *ctl;
  gchar name[32];
  gint device;
  gint err;
  struct connector_system_device *connector_system_device;

  sprintf (name, "hw:%d", card);
  if ((err = snd_ctl_open (&ctl, name, 0)) < 0)
    {
      error_print ("Cannot open control for card %d: %s\n",
		   card, snd_strerror (err));
      return;
    }
  device = -1;
  while (!(err = snd_ctl_rawmidi_next_device (ctl, &device)) && device >= 0)
    {
      connector_system_device =
	connector_get_system_device (ctl, card, device);
      if (connector_system_device)
	{
	  g_array_append_vals (devices, connector_system_device, 1);
	}
    }
  if (err < 0)
    {
      error_print ("Cannot determine device number: %s\n",
		   snd_strerror (err));
    }
  snd_ctl_close (ctl);
}

GArray *
connector_get_system_devices ()
{
  gint card, err;
  GArray *devices =
    g_array_new (FALSE, FALSE, sizeof (struct connector_system_device));

  card = -1;
  while (!(err = snd_card_next (&card)) && card >= 0)
    {
      connector_fill_card_elektron_devices (card, devices);
    }
  if (err < 0)
    {
      error_print ("Cannot determine card number: %s\n", snd_strerror (err));
    }

  return devices;
}

static guint
connector_next_data_entry (struct item_iterator *iter)
{
  gchar *name_cp1252;
  guint32 *data32;
  guint16 *data16;
  guint8 type;
  guint8 has_children;
  struct connector_iterator_data *data = iter->data;

  if (iter->item.name != NULL)
    {
      g_free (iter->item.name);
    }

  if (data->pos == data->msg->len)
    {
      iter->item.name = NULL;
      return -ENOENT;
    }

  name_cp1252 = (gchar *) & data->msg->data[data->pos];
  iter->item.name = connector_get_utf8 (name_cp1252);
  data->pos += strlen (name_cp1252) + 1;
  has_children = data->msg->data[data->pos];
  data->pos++;
  type = data->msg->data[data->pos];
  data->pos++;

  switch (type)
    {
    case 1:
      iter->item.type = ELEKTROID_DIR;
      data->pos += sizeof (guint32);	// child entries
      iter->item.size = 0;
      iter->item.index = -1;
      data->operations = 0;
      data->has_valid_data = 0;
      data->has_metadata = 0;
      break;
    case 2:
      iter->item.type = has_children ? ELEKTROID_DIR : ELEKTROID_FILE;

      data32 = (guint32 *) & data->msg->data[data->pos];
      iter->item.index = be32toh (*data32);	//index
      data->pos += sizeof (gint32);

      data32 = (guint32 *) & data->msg->data[data->pos];
      iter->item.size = be32toh (*data32);
      data->pos += sizeof (guint32);

      data16 = (guint16 *) & data->msg->data[data->pos];
      data->operations = be16toh (*data16);
      data->pos += sizeof (guint16);

      data->has_valid_data = data->msg->data[data->pos];
      data->pos++;

      data->has_metadata = data->msg->data[data->pos];
      data->pos++;

      break;
    default:
      error_print ("Unrecognized data entry: %d\n", iter->item.type);
      break;
    }

  return 0;
}

static gchar *
connector_add_prefix_to_path (const gchar * dir, const gchar * prefix)
{
  gchar *full = malloc (PATH_MAX);

  if (prefix)
    {
      snprintf (full, PATH_MAX, "%s%s", prefix, dir);
    }
  else
    {
      strcpy (full, dir);
    }

  return full;
}

static gint
connector_read_data_dir_prefix (struct item_iterator *iter, const gchar * dir,
				void *data, const char *prefix)
{
  int res;
  GByteArray *tx_msg;
  GByteArray *rx_msg;
  struct connector *connector = data;
  gchar *dir_w_prefix = connector_add_prefix_to_path (dir, prefix);

  tx_msg = connector_new_msg_list (dir_w_prefix, 0, 0, 1);
  g_free (dir_w_prefix);
  if (!tx_msg)
    {
      return -EINVAL;
    }

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }

  res = connector_get_msg_status (rx_msg);
  if (!res)
    {
      free_msg (rx_msg);
      return -ENOTDIR;
    }

  return connector_init_iterator (iter, rx_msg, connector_next_data_entry,
				  FS_DATA_ALL);
}

static gint
connector_read_data_dir_any (struct item_iterator *iter, const gchar * dir,
			     void *data)
{
  return connector_read_data_dir_prefix (iter, dir, data, NULL);
}

static gint
connector_read_data_dir_prj (struct item_iterator *iter, const gchar * dir,
			     void *data)
{
  return connector_read_data_dir_prefix (iter, dir, data, FS_DATA_PRJ_PREFIX);
}

static gint
connector_read_data_dir_snd (struct item_iterator *iter, const gchar * dir,
			     void *data)
{
  return connector_read_data_dir_prefix (iter, dir, data, FS_DATA_SND_PREFIX);
}

static gint
connector_dst_src_data_prefix_common (const gchar * src, const gchar * dst,
				      void *data, const char *prefix,
				      const guint8 * op_data, guint len)
{
  gint res;
  struct connector *connector = data;
  char *src_w_prefix = connector_add_prefix_to_path (src, prefix);
  char *dst_w_prefix = connector_add_prefix_to_path (dst, prefix);

  res = connector_src_dst_common (connector, src_w_prefix, dst_w_prefix,
				  op_data, len);
  g_free (src_w_prefix);
  g_free (dst_w_prefix);

  return res;
}

static gint
connector_move_data_item_prefix (const gchar * src, const gchar * dst,
				 void *data, const char *prefix)
{
  return connector_dst_src_data_prefix_common (src, dst, data, prefix,
					       DATA_MOVE_REQUEST,
					       sizeof (DATA_MOVE_REQUEST));
}

static gint
connector_move_data_item_any (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_move_data_item_prefix (src, dst, data, NULL);
}

static gint
connector_move_data_item_prj (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_move_data_item_prefix (src, dst, data, FS_DATA_PRJ_PREFIX);
}

static gint
connector_move_data_item_snd (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_move_data_item_prefix (src, dst, data, FS_DATA_SND_PREFIX);
}

static gint
connector_copy_data_item_prefix (const gchar * src, const gchar * dst,
				 void *data, const gchar * prefix)
{
  return connector_dst_src_data_prefix_common (src, dst, data, prefix,
					       DATA_COPY_REQUEST,
					       sizeof (DATA_COPY_REQUEST));
}

static gint
connector_copy_data_item_any (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_copy_data_item_prefix (src, dst, data, NULL);
}

static gint
connector_copy_data_item_prj (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_copy_data_item_prefix (src, dst, data, FS_DATA_PRJ_PREFIX);
}

static gint
connector_copy_data_item_snd (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_copy_data_item_prefix (src, dst, data, FS_DATA_SND_PREFIX);
}

static gint
connector_path_data_prefix_common (const gchar * path,
				   void *data, const char *prefix,
				   const guint8 * op_data, guint len)
{
  gint res;
  struct connector *connector = data;
  char *path_w_prefix = connector_add_prefix_to_path (path, prefix);

  res = connector_path_common (connector, path_w_prefix, op_data, len);
  g_free (path_w_prefix);

  return res;
}

static gint
connector_clear_data_item_prefix (const gchar * path, void *data,
				  const gchar * prefix)
{
  return connector_path_data_prefix_common (path, data, prefix,
					    DATA_CLEAR_REQUEST,
					    sizeof (DATA_CLEAR_REQUEST));
}

static gint
connector_clear_data_item_any (const gchar * path, void *data)
{
  return connector_clear_data_item_prefix (path, data, NULL);
}

static gint
connector_clear_data_item_prj (const gchar * path, void *data)
{
  return connector_clear_data_item_prefix (path, data, FS_DATA_PRJ_PREFIX);
}

static gint
connector_clear_data_item_snd (const gchar * path, void *data)
{
  return connector_clear_data_item_prefix (path, data, FS_DATA_SND_PREFIX);
}

static gint
connector_swap_data_item_prefix (const gchar * src, const gchar * dst,
				 void *data, const gchar * prefix)
{
  return connector_dst_src_data_prefix_common (src, dst, data, prefix,
					       DATA_SWAP_REQUEST,
					       sizeof (DATA_SWAP_REQUEST));
}

static gint
connector_swap_data_item_any (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_swap_data_item_prefix (src, dst, data, NULL);
}

static gint
connector_swap_data_item_prj (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_swap_data_item_prefix (src, dst, data, FS_DATA_PRJ_PREFIX);
}

static gint
connector_swap_data_item_snd (const gchar * src, const gchar * dst,
			      void *data)
{
  return connector_swap_data_item_prefix (src, dst, data, FS_DATA_SND_PREFIX);
}

static gint
connector_open_datum (struct connector *connector, const gchar * path,
		      guint32 * jid, gint mode, guint32 size)
{
  guint32 *data32;
  guint32 sizebe;
  guint32 chunk_size;
  guint8 compression;
  GByteArray *rx_msg;
  GByteArray *tx_msg;
  const guint8 *data;
  guint len;
  gchar *path_cp1252;
  gint res = 0;

  if (mode == O_RDONLY)
    {
      data = DATA_READ_OPEN_REQUEST;
      len = sizeof (DATA_READ_OPEN_REQUEST);
    }
  else if (mode == O_WRONLY)
    {
      data = DATA_WRITE_OPEN_REQUEST;
      len = sizeof (DATA_WRITE_OPEN_REQUEST);
    }
  else
    {
      return -EINVAL;
    }

  tx_msg = connector_new_msg (data, len);
  if (!tx_msg)
    {
      return -ENOMEM;
    }

  path_cp1252 = connector_get_cp1252 (path);

  if (mode == O_RDONLY)
    {
      g_byte_array_append (tx_msg, (guint8 *) path_cp1252,
			   strlen (path_cp1252) + 1);
      chunk_size = htobe32 (DATA_TRANSF_BLOCK_BYTES);
      g_byte_array_append (tx_msg, (guint8 *) & chunk_size, sizeof (guint32));
      compression = 1;
      g_byte_array_append (tx_msg, &compression, sizeof (guint8));
    }

  if (mode == O_WRONLY)
    {
      sizebe = htobe32 (size);
      g_byte_array_append (tx_msg, (guint8 *) & sizebe, sizeof (guint32));
      g_byte_array_append (tx_msg, (guint8 *) path_cp1252,
			   strlen (path_cp1252) + 1);
    }

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      res = -EIO;
      goto cleanup;
    }

  if (!connector_get_msg_status (rx_msg))
    {
      res = -EPERM;
      error_print ("%s (%s)\n", snd_strerror (res),
		   connector_get_msg_string (rx_msg));
      free_msg (rx_msg);
      goto cleanup;
    }

  data32 = (guint32 *) & rx_msg->data[6];
  *jid = be32toh (*data32);

  if (mode == O_RDONLY)
    {
      data32 = (guint32 *) & rx_msg->data[10];
      chunk_size = be32toh (*data32);

      compression = rx_msg->data[14];

      debug_print (1,
		   "Open datum info: job id: %d; chunk size: %d; compression: %d\n",
		   *jid, chunk_size, compression);
    }

  if (mode == O_WRONLY)
    {
      debug_print (1, "Open datum info: job id: %d\n", *jid);
    }

  free_msg (rx_msg);

cleanup:
  g_free (path_cp1252);
  return res;
}

static gint
connector_close_datum (struct connector *connector,
		       guint32 jid, gint mode, guint32 wsize)
{
  guint32 jidbe;
  guint32 wsizebe;
  guint32 r_jid;
  guint32 asize;
  guint32 *data32;
  GByteArray *rx_msg;
  GByteArray *tx_msg;
  const guint8 *data;
  guint len;

  if (mode == O_RDONLY)
    {
      data = DATA_READ_CLOSE_REQUEST;
      len = sizeof (DATA_READ_CLOSE_REQUEST);
    }
  else if (mode == O_WRONLY)
    {
      data = DATA_WRITE_CLOSE_REQUEST;
      len = sizeof (DATA_WRITE_CLOSE_REQUEST);
    }
  else
    {
      return -EINVAL;
    }

  tx_msg = connector_new_msg (data, len);
  if (!tx_msg)
    {
      return -ENOMEM;
    }

  jidbe = htobe32 (jid);
  g_byte_array_append (tx_msg, (guchar *) & jidbe, sizeof (guint32));

  if (mode == O_WRONLY)
    {
      wsizebe = htobe32 (wsize);
      g_byte_array_append (tx_msg, (guchar *) & wsizebe, sizeof (guint32));
    }

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return -EIO;
    }

  if (!connector_get_msg_status (rx_msg))
    {
      error_print ("%s (%s)\n", snd_strerror (-EPERM),
		   connector_get_msg_string (rx_msg));
      free_msg (rx_msg);
      return -EPERM;
    }

  data32 = (guint32 *) & rx_msg->data[6];
  r_jid = be32toh (*data32);

  data32 = (guint32 *) & rx_msg->data[10];
  asize = be32toh (*data32);

  debug_print (1, "Close datum info: job id: %d; size: %d\n", r_jid, asize);

  free_msg (rx_msg);

  if (mode == O_WRONLY && asize != wsize)
    {
      error_print
	("Actual download bytes (%d) differs from expected ones (%d)\n",
	 asize, wsize);
      return -EINVAL;
    }

  return 0;
}

static gint
connector_download_data_prefix (const gchar * path, GByteArray * output,
				struct job_control *control, void *data,
				const gchar * prefix)
{
  gint res;
  guint32 seq;
  guint32 seqbe;
  guint32 jid;
  guint32 r_jid;
  guint32 r_seq;
  guint32 status;
  guint8 last;
  guint32 hash;
  guint32 *data32;
  guint32 jidbe;
  guint32 data_size;
  gboolean active;
  GByteArray *rx_msg;
  GByteArray *tx_msg;
  gchar *path_w_prefix = connector_add_prefix_to_path (path, prefix);
  struct connector *connector = data;

  res = connector_open_datum (connector, path_w_prefix, &jid, O_RDONLY, 0);
  g_free (path_w_prefix);
  if (res)
    {
      return -EIO;
    }

  usleep (REST_TIME);

  jidbe = htobe32 (jid);

  res = 0;
  seq = 0;
  last = 0;
  control->data = NULL;
  if (control)
    {
      g_mutex_lock (&control->mutex);
      active = control->active;
      g_mutex_unlock (&control->mutex);
    }
  else
    {
      active = TRUE;
    }
  while (!last && active)
    {
      tx_msg =
	connector_new_msg (DATA_READ_PARTIAL_REQUEST,
			   sizeof (DATA_READ_PARTIAL_REQUEST));
      g_byte_array_append (tx_msg, (guint8 *) & jidbe, sizeof (guint32));
      seqbe = htobe32 (seq);
      g_byte_array_append (tx_msg, (guint8 *) & seqbe, sizeof (guint32));
      rx_msg = connector_tx_and_rx (connector, tx_msg);
      if (!rx_msg)
	{
	  res = -EIO;
	  break;
	}

      if (!connector_get_msg_status (rx_msg))
	{
	  res = -EPERM;
	  error_print ("%s (%s)\n", snd_strerror (res),
		       connector_get_msg_string (rx_msg));
	  free_msg (rx_msg);
	  break;
	}

      data32 = (guint32 *) & rx_msg->data[6];
      r_jid = be32toh (*data32);

      data32 = (guint32 *) & rx_msg->data[10];
      r_seq = be32toh (*data32);

      data32 = (guint32 *) & rx_msg->data[14];
      status = be32toh (*data32);

      last = rx_msg->data[18];

      data32 = (guint32 *) & rx_msg->data[19];
      hash = be32toh (*data32);

      data32 = (guint32 *) & rx_msg->data[23];
      data_size = be32toh (*data32);

      if (data_size)
	{
	  debug_print (1,
		       "Read datum info: job id: %d; last: %d; seq: %d; status: %d; hash: 0x%08x\n",
		       r_jid, last, r_seq, status, hash);

	  g_byte_array_append (output, (guint8 *) & rx_msg->data[27],
			       data_size);
	}
      else
	{
	  // Sometimes, the first message returns 0 data size and the rest of the parameters are not initialized.
	  debug_print (1,
		       "Read datum info: job id: %d; last: %d, hash: 0x%08x\n",
		       r_jid, last, hash);
	  status = 0;
	}

      free_msg (rx_msg);
      seq++;

      if (control)
	{
	  set_job_control_progress (control, status / 1000.0);
	  g_mutex_lock (&control->mutex);
	  active = control->active;
	  g_mutex_unlock (&control->mutex);
	}

      usleep (REST_TIME);
    }

  return connector_close_datum (connector, jid, O_RDONLY, 0);
}

static gint
connector_download_data_any (const gchar * path, GByteArray * output,
			     struct job_control *control, void *data)
{
  control->parts = 1;
  control->part = 0;
  return connector_download_data_prefix (path, output, control, data, NULL);
}

static gint
connector_download_data_prj (const gchar * path, GByteArray * output,
			     struct job_control *control, void *data)
{
  return connector_download_data_prefix (path, output, control, data,
					 FS_DATA_PRJ_PREFIX);
}

static gint
connector_download_data_snd (const gchar * path, GByteArray * output,
			     struct job_control *control, void *data)
{
  return connector_download_data_prefix (path, output, control, data,
					 FS_DATA_SND_PREFIX);
}

gchar *
connector_get_sample_path_from_hash_size (struct connector *connector,
					  guint32 hash, guint32 size)
{
  guint32 aux32;
  gchar *path;
  GByteArray *rx_msg, *tx_msg =
    connector_new_msg (FS_SAMPLE_GET_FILE_INFO_FROM_HASH_AND_SIZE_REQUEST,
		       sizeof
		       (FS_SAMPLE_GET_FILE_INFO_FROM_HASH_AND_SIZE_REQUEST));

  aux32 = htobe32 (hash);
  memcpy (&tx_msg->data[5], &aux32, sizeof (guint32));
  aux32 = htobe32 (size);
  memcpy (&tx_msg->data[9], &aux32, sizeof (guint32));

  rx_msg = connector_tx_and_rx (connector, tx_msg);
  if (!rx_msg)
    {
      return NULL;
    }

  if (connector_get_msg_status (rx_msg))
    {
      path = strdup ((gchar *) & rx_msg->data[14]);
    }
  else
    {
      path = NULL;
    }
  g_byte_array_free (rx_msg, TRUE);
  return path;
}

static gint
connector_download_data_pkg (const gchar * path, GByteArray * output,
			     struct job_control *control, void *data,
			     enum package_type type,
			     const struct fs_operations *ops,
			     fs_remote_file_op download)
{
  gint ret;
  gchar *pkg_name;
  struct package pkg;
  struct connector *connector = data;

  pkg_name = connector_get_download_name (connector, NULL, ops, path);
  if (!pkg_name)
    {
      return -1;
    }

  if (package_begin
      (&pkg, pkg_name, connector->fw_version, connector->device_desc, type))
    {
      g_free (pkg_name);
      return -1;
    }

  ret =
    package_receive_pkg_resources (&pkg, path, control, connector, download,
				   connector_download_sample_part);
  ret = ret || package_end (&pkg, output);

  package_destroy (&pkg);
  return ret;
}

static gint
connector_download_data_snd_pkg (const gchar * path, GByteArray * output,
				 struct job_control *control, void *data)
{
  return connector_download_data_pkg (path, output, control, data,
				      PKG_FILE_TYPE_SOUND,
				      &FS_DATA_SND_OPERATIONS,
				      connector_download_data_snd);
}

static gint
connector_download_data_prj_pkg (const gchar * path, GByteArray * output,
				 struct job_control *control, void *data)
{
  return connector_download_data_pkg (path, output, control, data,
				      PKG_FILE_TYPE_PROJECT,
				      &FS_DATA_PRJ_OPERATIONS,
				      connector_download_data_prj);
}

gchar *
connector_get_upload_path (struct connector *connector,
			   struct item_iterator *remote_iter,
			   const struct fs_operations *ops,
			   const gchar * dst_dir, const gchar * src_path,
			   gint32 * next_index)
{
  gchar *path, *indexs, *namec, *name, *aux;
  struct item_iterator iter;
  gboolean empty;

  if (ops->fs == FS_SAMPLES || ops->fs == FS_RAW_ALL
      || ops->fs == FS_RAW_PRESETS)
    {
      namec = strdup (src_path);
      name = basename (namec);
      remove_ext (name);
      aux = chain_path (dst_dir, name);
      g_free (namec);

      if (ops->fs == FS_RAW_ALL || ops->fs == FS_RAW_PRESETS)
	{
	  path = connector_add_ext_to_mc_snd (aux);
	  g_free (aux);
	}
      else
	{
	  path = aux;
	}
      return path;
    }

  if (remote_iter)
    {
      if (copy_item_iterator (&iter, remote_iter))
	{
	  return strdup (dst_dir);
	}
    }
  else
    {
      if (ops->readdir (&iter, dst_dir, connector))
	{
	  return strdup (dst_dir);
	}
    }

  empty = TRUE;
  while (!next_item_iterator (&iter))
    {
      empty = FALSE;
      if (iter.item.index > *next_index)
	{
	  break;
	}
      (*next_index)++;
    }

  free_item_iterator (&iter);

  indexs = malloc (PATH_MAX);
  snprintf (indexs, PATH_MAX, "%d", *next_index);
  if (empty)
    {
      (*next_index)++;
    }
  path = chain_path (dst_dir, indexs);
  g_free (indexs);

  return path;
}

gchar *
connector_get_download_name (struct connector *connector,
			     struct item_iterator *remote_iter,
			     const struct fs_operations *ops,
			     const gchar * src_path)
{
  gint32 id;
  struct item_iterator iter;
  const gchar *src_dir;
  gchar *namec, *name, *src_dirc;
  gint ret;

  namec = strdup (src_path);
  name = basename (namec);

  if (ops->fs == FS_SAMPLES || ops->fs == FS_RAW_ALL
      || ops->fs == FS_RAW_PRESETS)
    {
      name = strdup (name);
      goto end;
    }

  if (remote_iter)
    {
      if (copy_item_iterator (&iter, remote_iter))
	{
	  name = NULL;
	  goto cleanup;
	}
    }
  else
    {
      src_dirc = strdup (src_path);
      src_dir = dirname (src_dirc);
      ret = ops->readdir (&iter, src_dir, connector);
      g_free (src_dirc);
      if (ret)
	{
	  name = NULL;
	  goto cleanup;
	}
    }

  id = atoi (name);
  name = NULL;

  while (!next_item_iterator (&iter))
    {
      if (iter.item.index == id)
	{
	  name = get_item_name (&iter.item);
	  break;
	}
    }

  free_item_iterator (&iter);
cleanup:
  g_free (namec);
end:
  return name;
}

gchar *
connector_get_download_path (struct connector *connector,
			     struct item_iterator *remote_iter,
			     const struct fs_operations *ops,
			     const gchar * dst_dir, const gchar * src_path)
{
  gchar *path, *src_pathc, *name, *dl_ext, *filename;
  const gchar *src_fpath, *md_ext, *ext = get_ext (src_path);

  src_pathc = strdup (src_path);
  if (ext && strcmp (ext, "metadata") == 0 && ops->fs != FS_SAMPLES)
    {
      src_fpath = dirname (src_pathc);
      md_ext = ".metadata";
    }
  else
    {
      src_fpath = src_pathc;
      md_ext = "";
    }

  name = connector_get_download_name (connector, remote_iter, ops, src_fpath);
  if (name)
    {
      dl_ext = connector_get_full_ext (connector->device_desc, ops);
      filename = malloc (PATH_MAX);
      snprintf (filename, PATH_MAX, "%s.%s%s", name, dl_ext, md_ext);
      path = chain_path (dst_dir, filename);
      g_free (name);
      g_free (dl_ext);
      g_free (filename);
    }
  else
    {
      path = NULL;
    }

  g_free (src_pathc);
  return path;
}

static gint
connector_upload_data_prefix (const gchar * path, GByteArray * array,
			      struct job_control *control, void *data,
			      const gchar * prefix)
{
  gint res;
  guint32 seq;
  guint32 jid;
  guint32 crc;
  guint32 len;
  guint32 r_jid;
  guint32 r_seq;
  guint32 offset;
  guint32 *data32;
  guint32 jidbe;
  guint32 aux32;
  gboolean active;
  guint32 total;
  GByteArray *rx_msg;
  GByteArray *tx_msg;
  gchar *path_w_prefix = connector_add_prefix_to_path (path, prefix);
  struct connector *connector = data;

  res =
    connector_open_datum (connector, path_w_prefix, &jid, O_WRONLY,
			  array->len);
  g_free (path_w_prefix);
  if (res)
    {
      goto end;
    }

  usleep (REST_TIME);

  jidbe = htobe32 (jid);

  seq = 0;
  offset = 0;
  control->data = NULL;
  if (control)
    {
      g_mutex_lock (&control->mutex);
      active = control->active;
      g_mutex_unlock (&control->mutex);
    }
  else
    {
      active = TRUE;
    }

  while (offset < array->len && active)
    {
      tx_msg =
	connector_new_msg (DATA_WRITE_PARTIAL_REQUEST,
			   sizeof (DATA_WRITE_PARTIAL_REQUEST));
      g_byte_array_append (tx_msg, (guint8 *) & jidbe, sizeof (guint32));
      aux32 = htobe32 (seq);
      g_byte_array_append (tx_msg, (guint8 *) & aux32, sizeof (guint32));

      if (offset + DATA_TRANSF_BLOCK_BYTES < array->len)
	{
	  len = DATA_TRANSF_BLOCK_BYTES;
	}
      else
	{
	  len = array->len - offset;
	}

      crc = crc32 (0xffffffff, &array->data[offset], len);
      aux32 = htobe32 (crc);
      g_byte_array_append (tx_msg, (guint8 *) & aux32, sizeof (guint32));

      aux32 = htobe32 (len);
      g_byte_array_append (tx_msg, (guint8 *) & aux32, sizeof (guint32));

      g_byte_array_append (tx_msg, &array->data[offset], len);

      rx_msg = connector_tx_and_rx (connector, tx_msg);
      if (!rx_msg)
	{
	  res = -EIO;
	  goto end;
	}

      usleep (REST_TIME);

      if (!connector_get_msg_status (rx_msg))
	{
	  res = -EPERM;
	  error_print ("%s (%s)\n", snd_strerror (res),
		       connector_get_msg_string (rx_msg));
	  free_msg (rx_msg);
	  break;
	}

      data32 = (guint32 *) & rx_msg->data[6];
      r_jid = be32toh (*data32);

      data32 = (guint32 *) & rx_msg->data[10];
      r_seq = be32toh (*data32);

      data32 = (guint32 *) & rx_msg->data[14];
      total = be32toh (*data32);

      free_msg (rx_msg);

      debug_print (1,
		   "Write datum info: job id: %d; seq: %d; total: %d\n",
		   r_jid, r_seq, total);

      seq++;
      offset += len;

      if (total != offset)
	{
	  error_print
	    ("Actual upload bytes (%d) differs from expected ones (%d)\n",
	     total, offset);
	}

      if (control)
	{
	  set_job_control_progress (control, offset / (gdouble) array->len);
	  g_mutex_lock (&control->mutex);
	  active = control->active;
	  g_mutex_unlock (&control->mutex);
	}
    }

  debug_print (2, "%d bytes sent\n", offset);

  res = connector_close_datum (connector, jid, O_WRONLY, array->len);

end:
  return res;
}

static gint
connector_upload_data_any (const gchar * path, GByteArray * array,
			   struct job_control *control, void *data)
{
  control->parts = 1;
  control->part = 0;
  return connector_upload_data_prefix (path, array, control, data, NULL);
}

static gint
connector_upload_data_prj (const gchar * path, GByteArray * array,
			   struct job_control *control, void *data)
{
  return connector_upload_data_prefix (path, array, control, data,
				       FS_DATA_PRJ_PREFIX);
}

static gint
connector_upload_data_snd (const gchar * path, GByteArray * array,
			   struct job_control *control, void *data)
{
  return connector_upload_data_prefix (path, array, control, data,
				       FS_DATA_SND_PREFIX);
}

static gint
connector_upload_data_pkg (const gchar * path, GByteArray * input,
			   struct job_control *control, void *data,
			   guint8 type, const struct fs_operations *ops,
			   fs_remote_file_op upload)
{
  gint ret;
  struct package pkg;
  struct connector *connector = data;

  ret = package_open (&pkg, input, connector->device_desc);
  if (!ret)
    {
      ret = package_send_pkg_resources (&pkg, path, control, connector,
					upload, connector_upload_sample_part);
      package_close (&pkg);
    }
  return ret;
}

static gint
connector_upload_data_snd_pkg (const gchar * path, GByteArray * input,
			       struct job_control *control, void *data)
{
  return connector_upload_data_pkg (path, input, control, data,
				    PKG_FILE_TYPE_SOUND,
				    &FS_DATA_SND_OPERATIONS,
				    connector_upload_data_snd);
}

static gint
connector_upload_data_prj_pkg (const gchar * path, GByteArray * input,
			       struct job_control *control, void *data)
{
  return connector_upload_data_pkg (path, input, control, data,
				    PKG_FILE_TYPE_PROJECT,
				    &FS_DATA_PRJ_OPERATIONS,
				    connector_upload_data_prj);
}

gchar *
connector_get_full_ext (const struct connector_device_desc *desc,
			const struct fs_operations *ops)
{
  gchar *ext = malloc (LABEL_MAX);
  if (ops->fs == FS_SAMPLES)
    {
      snprintf (ext, LABEL_MAX, "%s", ops->extension);
    }
  else
    {
      snprintf (ext, LABEL_MAX, "%s%s", desc->alias, ops->extension);
    }
  return ext;
}
